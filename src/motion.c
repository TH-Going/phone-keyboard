/*
 * Mouse motion smoothing.
 *
 * The phone batches pointer deltas every 8 ms, which would otherwise be
 * injected as coarse jumps. Each incoming delta becomes a segment that is
 * drained over a 16 ms window in 4 ms steps, so the PC cursor moves in small
 * regular increments. Every injection happens under the plan lock, which keeps
 * movement ordered with clicks, wheel and key events.
 */
#include "app.h"

#define MOTION_STEP_NS 4000000ULL
#define MOTION_WINDOW_NS 16000000ULL
#define MOTION_MAX_SEGMENTS 64

typedef struct {
    int dx;
    int dy;
    int emitted_x;
    int emitted_y;
    uint64_t start_ns;
} Segment;

struct Motion {
    CRITICAL_SECTION lock;
    Segment segments[MOTION_MAX_SEGMENTS];
    int count;
    long long ready_x;
    long long ready_y;
    bool stopped;
    bool dry_run;
    HANDLE wake;
    HANDLE timer;
    HANDLE worker;
};

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef TIMER_ALL_ACCESS
#define TIMER_ALL_ACCESS 0x001F0003
#endif

static int scale_component(int value, uint64_t elapsed_ns)
{
    const long long total = (long long)MOTION_WINDOW_NS;
    long long numerator = (long long)value * (long long)elapsed_ns;
    long long sign = (numerator > 0) - (numerator < 0);
    return (int)((numerator + sign * (total / 2)) / total);
}

static bool motion_pending(const Motion *motion)
{
    return motion->count > 0 || motion->ready_x != 0 || motion->ready_y != 0;
}

static void motion_clear(Motion *motion)
{
    motion->count = 0;
    motion->ready_x = 0;
    motion->ready_y = 0;
}

static void motion_take_ready(Motion *motion, int *dx, int *dy)
{
    long long take_x = motion->ready_x;
    long long take_y = motion->ready_y;
    if (take_x > 2147483647LL) {
        take_x = 2147483647LL;
    } else if (take_x < -2147483647LL - 1) {
        take_x = -2147483647LL - 1;
    }
    if (take_y > 2147483647LL) {
        take_y = 2147483647LL;
    } else if (take_y < -2147483647LL - 1) {
        take_y = -2147483647LL - 1;
    }
    motion->ready_x -= take_x;
    motion->ready_y -= take_y;
    *dx = (int)take_x;
    *dy = (int)take_y;
}

static bool motion_emit(Motion *motion, int dx, int dy)
{
    if (motion->dry_run) {
        return true;
    }
    return input_send_relative_move(dx, dy);
}

static void motion_sample(Motion *motion, uint64_t now, int *dx, int *dy)
{
    int kept = 0;
    for (int i = 0; i < motion->count; i++) {
        Segment *segment = &motion->segments[i];
        uint64_t elapsed = now - segment->start_ns;
        if (elapsed > MOTION_WINDOW_NS) {
            elapsed = MOTION_WINDOW_NS;
        }
        int next_x = scale_component(segment->dx, elapsed);
        int next_y = scale_component(segment->dy, elapsed);
        motion->ready_x += (long long)(next_x - segment->emitted_x);
        motion->ready_y += (long long)(next_y - segment->emitted_y);
        segment->emitted_x = next_x;
        segment->emitted_y = next_y;
        if (segment->emitted_x != segment->dx || segment->emitted_y != segment->dy) {
            motion->segments[kept++] = *segment;
        }
    }
    motion->count = kept;
    motion_take_ready(motion, dx, dy);
}

static void motion_finish(Motion *motion)
{
    for (int i = 0; i < motion->count; i++) {
        motion->ready_x += (long long)(motion->segments[i].dx - motion->segments[i].emitted_x);
        motion->ready_y += (long long)(motion->segments[i].dy - motion->segments[i].emitted_y);
    }
    motion->count = 0;
}

static uint64_t motion_delay_ns(const Motion *motion, uint64_t now)
{
    if (motion->count == 0) {
        return MOTION_STEP_NS;
    }
    uint64_t deadline = motion->segments[0].start_ns + MOTION_WINDOW_NS;
    uint64_t delay = deadline > now ? deadline - now : 0;
    if (delay > MOTION_STEP_NS) {
        delay = MOTION_STEP_NS;
    }
    return delay;
}

static void motion_wait_ns(Motion *motion, uint64_t delay_ns)
{
    if (delay_ns == 0) {
        return;
    }
    if (motion->timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((delay_ns + 99) / 100);
        if (SetWaitableTimer(motion->timer, &due, 0, NULL, NULL, FALSE)) {
            /* A finite wait bounds shutdown if the timer never fires. */
            if (WaitForSingleObject(motion->timer, 20) == WAIT_OBJECT_0) {
                return;
            }
        }
    }
    Sleep((DWORD)((delay_ns + 999999) / 1000000));
}

static DWORD WINAPI motion_thread(LPVOID parameter)
{
    Motion *motion = (Motion *)parameter;

    for (;;) {
        EnterCriticalSection(&motion->lock);
        while (!motion->stopped && !motion_pending(motion)) {
            LeaveCriticalSection(&motion->lock);
            WaitForSingleObject(motion->wake, INFINITE);
            EnterCriticalSection(&motion->lock);
        }
        if (motion->stopped) {
            LeaveCriticalSection(&motion->lock);
            break;
        }
        uint64_t delay = motion_delay_ns(motion, now_ns());
        LeaveCriticalSection(&motion->lock);

        motion_wait_ns(motion, delay);

        EnterCriticalSection(&motion->lock);
        if (motion->stopped) {
            LeaveCriticalSection(&motion->lock);
            break;
        }
        int dx = 0;
        int dy = 0;
        motion_sample(motion, now_ns(), &dx, &dy);
        if ((dx != 0 || dy != 0) && !motion_emit(motion, dx, dy)) {
            /* Never replay movement that Windows refused to inject. */
            motion_clear(motion);
        }
        LeaveCriticalSection(&motion->lock);
    }
    return 0;
}

Motion *motion_create(bool dry_run)
{
    Motion *motion = (Motion *)calloc(1, sizeof(Motion));
    if (!motion) {
        return NULL;
    }
    InitializeCriticalSection(&motion->lock);
    motion->dry_run = dry_run;
    motion->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    motion->timer = CreateWaitableTimerExW(NULL, NULL,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    if (!motion->wake) {
        DeleteCriticalSection(&motion->lock);
        free(motion);
        return NULL;
    }
    motion->worker = CreateThread(NULL, 0, motion_thread, motion, 0, NULL);
    if (!motion->worker) {
        CloseHandle(motion->wake);
        if (motion->timer) {
            CloseHandle(motion->timer);
        }
        DeleteCriticalSection(&motion->lock);
        free(motion);
        return NULL;
    }
    return motion;
}

void motion_destroy(Motion *motion)
{
    if (!motion) {
        return;
    }
    EnterCriticalSection(&motion->lock);
    motion->stopped = true;
    motion_clear(motion);
    SetEvent(motion->wake);
    LeaveCriticalSection(&motion->lock);

    if (motion->worker) {
        WaitForSingleObject(motion->worker, 2000);
        CloseHandle(motion->worker);
    }
    if (motion->timer) {
        CloseHandle(motion->timer);
    }
    if (motion->wake) {
        CloseHandle(motion->wake);
    }
    DeleteCriticalSection(&motion->lock);
    free(motion);
}

bool motion_push(Motion *motion, int dx, int dy)
{
    if (!motion) {
        return false;
    }
    if (dx == 0 && dy == 0) {
        return true;
    }
    EnterCriticalSection(&motion->lock);
    if (motion->stopped) {
        LeaveCriticalSection(&motion->lock);
        return false;
    }
    if (motion->count == MOTION_MAX_SEGMENTS) {
        /* Bound storage during a burst without dropping distance. */
        Segment *oldest = &motion->segments[0];
        motion->ready_x += (long long)(oldest->dx - oldest->emitted_x);
        motion->ready_y += (long long)(oldest->dy - oldest->emitted_y);
        memmove(&motion->segments[0], &motion->segments[1],
                sizeof(Segment) * (size_t)(MOTION_MAX_SEGMENTS - 1));
        motion->count--;
    }
    Segment *segment = &motion->segments[motion->count++];
    segment->dx = dx;
    segment->dy = dy;
    segment->emitted_x = 0;
    segment->emitted_y = 0;
    segment->start_ns = now_ns();
    SetEvent(motion->wake);
    LeaveCriticalSection(&motion->lock);
    return true;
}

bool motion_flush(Motion *motion)
{
    if (!motion) {
        return false;
    }
    EnterCriticalSection(&motion->lock);
    if (motion->stopped) {
        LeaveCriticalSection(&motion->lock);
        return false;
    }
    for (;;) {
        motion_finish(motion);
        int dx = 0;
        int dy = 0;
        motion_take_ready(motion, &dx, &dy);
        if (dx == 0 && dy == 0) {
            break;
        }
        if (!motion_emit(motion, dx, dy)) {
            motion_clear(motion);
            LeaveCriticalSection(&motion->lock);
            return false;
        }
        if (!motion_pending(motion)) {
            break;
        }
    }
    LeaveCriticalSection(&motion->lock);
    return true;
}

void motion_cancel(Motion *motion)
{
    if (!motion) {
        return;
    }
    EnterCriticalSection(&motion->lock);
    motion_clear(motion);
    LeaveCriticalSection(&motion->lock);
}
