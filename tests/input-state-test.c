/* Exercise the real input protocol without ever calling Windows SendInput. */
#include "../src/app.h"

static int failures;
static int injection_failures;
static bool flush_ok = true;
static char events[256];
static size_t event_count;
static int move_x, move_y, wheel_x, wheel_y;

static void record(char event)
{
    if (event_count + 1 < sizeof(events)) {
        events[event_count++] = event;
        events[event_count] = 0;
    }
}

static UINT WINAPI test_send_input(UINT count, LPINPUT inputs, int size)
{
    (void)size;
    for (UINT i = 0; i < count; i++) {
        if (inputs[i].type == INPUT_MOUSE) {
            if (inputs[i].mi.dwFlags & MOUSEEVENTF_HWHEEL) wheel_x += (LONG)inputs[i].mi.mouseData;
            if (inputs[i].mi.dwFlags & MOUSEEVENTF_WHEEL) wheel_y += (LONG)inputs[i].mi.mouseData;
        }
        record(inputs[i].type == INPUT_MOUSE
            ? ((inputs[i].mi.dwFlags & MOUSEEVENTF_LEFTUP) ? 'U' : 'D') : 'K');
    }
    if (injection_failures > 0) {
        injection_failures--;
        return 0;
    }
    return count;
}

#define SendInput test_send_input
#include "../src/input.c"

struct Motion { int unused; };
static Motion test_motion;

Motion *motion_create(bool dry_run) { (void)dry_run; return &test_motion; }
void motion_destroy(Motion *motion) { (void)motion; }
bool motion_push(Motion *motion, int dx, int dy)
{
    (void)motion;
    move_x += dx;
    move_y += dy;
    record('M');
    return true;
}
bool motion_flush(Motion *motion) { (void)motion; record('F'); return flush_ok; }
void motion_cancel(Motion *motion) { (void)motion; record('C'); }

static void check(bool condition, const char *message)
{
    printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) failures++;
}

static void clear_events(void)
{
    event_count = 0;
    events[0] = 0;
    move_x = move_y = wheel_x = wheel_y = 0;
}

static void apply_axes(InputSession *session, const char *tag, int dx, int dy)
{
    char json[96];
    _snprintf_s(json, sizeof(json), _TRUNCATE, "{\"t\":\"%s\",\"dx\":%d,\"dy\":%d}", tag, dx, dy);
    if (!input_apply_json(session, json)) failures++;
}

int main(void)
{
    const char *down = "{\"t\":\"button\",\"button\":\"left\",\"down\":true}";
    const char *up = "{\"t\":\"button\",\"button\":\"left\",\"down\":false}";
    InputSession session;
    input_session_init(&session, false);

    check(input_apply_json(&session, down) && session.buttons == 1, "left down is tracked after successful injection");
    clear_events();
    check(input_apply_json(&session, down) && strcmp(events, "F") == 0, "duplicate down does not inject again");
    clear_events();
    input_apply_json(&session, "{\"t\":\"move\",\"dx\":12,\"dy\":-5}");
    check(input_apply_json(&session, up) && session.buttons == 0 && strcmp(events, "MFU") == 0,
          "pending move is flushed before left up");
    clear_events();
    check(input_apply_json(&session, up) && strcmp(events, "F") == 0, "duplicate up does not inject again");

    injection_failures = 1;
    check(!input_apply_json(&session, down) && session.buttons == 0, "failed down is not recorded as held");
    input_apply_json(&session, down);
    injection_failures = 1;
    check(!input_apply_json(&session, up) && session.buttons == 1, "failed up keeps its release record");
    clear_events();
    input_release_all(&session);
    check(session.buttons == 0 && strcmp(events, "CU") == 0, "release retries a failed up");

    input_apply_json(&session, down);
    flush_ok = false;
    clear_events();
    check(!input_apply_json(&session, up) && session.buttons == 0 && strcmp(events, "FCU") == 0,
          "failed motion flush still cancels movement and releases left button");
    flush_ok = true;

    input_apply_json(&session, down);
    injection_failures = 1;
    input_release_all(&session);
    check(session.buttons == 1, "failed cleanup preserves the button for another attempt");
    input_release_all(&session);
    check(session.buttons == 0, "a later cleanup releases the retained button");

    input_apply_json(&session, down);
    injection_failures = 1;
    check(!input_apply_json(&session, "{\"t\":\"release\"}") && session.buttons == 1,
          "failed release reports failure so the server can retry cleanup");
    input_release_all(&session);
    check(session.buttons == 0, "server error cleanup can retry a failed release frame");

    input_apply_json(&session, "{\"t\":\"key\",\"action\":\"down\",\"code\":\"ShiftLeft\"}");
    input_apply_json(&session, down);
    input_apply_json(&session, "{\"t\":\"release\"}");
    check(session.buttons == 0 && session.key_count == 0, "release clears keyboard and mouse together");

    input_apply_json(&session, down);
    input_session_dispose(&session);
    check(session.buttons == 0 && session.motion == NULL, "session disposal releases a held drag");

    input_session_init(&session, true);
    clear_events();
    input_apply_json(&session, down);
    input_apply_json(&session, up);
    check(session.buttons == 0 && strcmp(events, "FF") == 0, "dry-run tracks state without input injection");
    input_session_dispose(&session);

    input_session_init(&session, false);
    clear_events();
    apply_axes(&session, "move", 12, -5);
    apply_axes(&session, "wheel", 120, -240);
    check(move_x == 12 && move_y == -5 && wheel_x == 120 && wheel_y == -240,
          "default speed preserves existing movement and wheel input");

    input_session_set_speed(&session, 150, 50);
    clear_events();
    apply_axes(&session, "move", 20, -10);
    apply_axes(&session, "wheel", 120, -240);
    check(move_x == 30 && move_y == -15 && wheel_x == 60 && wheel_y == -120,
          "mouse and scroll multipliers are independent on both axes");

    input_session_set_speed(&session, 125, 75);
    clear_events();
    for (int i = 0; i < 4; i++) {
        input_session_set_speed(&session, 125, 75);
        apply_axes(&session, "move", 1, -1);
        apply_axes(&session, "wheel", -1, 1);
    }
    check(move_x == 5 && move_y == -5 && wheel_x == -3 && wheel_y == 3,
          "small positive and negative deltas retain fractions across frames");

    input_session_set_speed(&session, 110, 110);
    clear_events();
    for (int i = 0; i < 10; i++) {
        apply_axes(&session, "move", 1, -1);
        apply_axes(&session, "wheel", -1, 1);
    }
    check(move_x == 11 && move_y == -11 && wheel_x == -11 && wheel_y == 11,
          "a 0.1 multiplier step retains positive and negative fractions");

    input_session_set_speed(&session, 50, 75);
    clear_events();
    apply_axes(&session, "move", 1, -1);
    apply_axes(&session, "move", -1, 1);
    check(move_x == 0 && move_y == 0, "opposite subpixel moves cancel without drift");

    apply_axes(&session, "move", 1, -1);
    input_session_set_speed(&session, 150, 75);
    clear_events();
    apply_axes(&session, "move", 1, -1);
    check(move_x == 1 && move_y == -1, "changing sensitivity applies immediately and drops its old fraction");
    input_session_set_speed(&session, 150, 200);
    clear_events();
    apply_axes(&session, "move", 1, -1);
    apply_axes(&session, "wheel", 120, -120);
    check(move_x == 2 && move_y == -2 && wheel_x == 240 && wheel_y == -240,
          "changing scroll speed preserves the independent mouse remainder");

    input_session_set_speed(&session, 50, 50);
    apply_axes(&session, "move", 1, -1);
    apply_axes(&session, "wheel", 1, -1);
    input_release_all(&session);
    clear_events();
    apply_axes(&session, "move", 1, -1);
    apply_axes(&session, "wheel", 1, -1);
    check(move_x == 0 && move_y == 0 && wheel_x == 0 && wheel_y == 0,
          "release clears all fractional input from the previous gesture");

    input_session_set_speed(&session, 200, 200);
    input_apply_json(&session, down);
    clear_events();
    apply_axes(&session, "move", 12, -5);
    input_apply_json(&session, up);
    check(move_x == 24 && move_y == -10 && strcmp(events, "MFU") == 0 && session.buttons == 0,
          "configured speed also scales drags and flushes before mouse release");

    input_session_set_speed(&session, 300, 300);
    clear_events();
    apply_axes(&session, "move", 2147483647, -2147483647);
    apply_axes(&session, "wheel", -2147483647, 2147483647);
    check(move_x == 600 && move_y == -600 && wheel_x == -4800 && wheel_y == 4800,
          "maximum speed retains input limits without integer overflow");

    input_session_set_speed(&session, 0, 301);
    clear_events();
    apply_axes(&session, "move", 12, -5);
    apply_axes(&session, "wheel", 120, -240);
    check(move_x == 12 && move_y == -5 && wheel_x == 120 && wheel_y == -240,
          "invalid speed values fall back to the original speed");
    input_session_dispose(&session);

    printf("%s\n", failures ? "input state checks failed" : "all input state checks passed");
    return failures ? 1 : 0;
}
