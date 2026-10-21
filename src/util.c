#include "app.h"

#include <bcrypt.h>
#include <stdarg.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

bool app_log_open(App *app, const wchar_t *path)
{
    if (!path || !path[0]) {
        return false;
    }
    /* Plain byte stream: the ccs= encoding layer does not support setvbuf and
     * a crashed buffered stream would lose every diagnostic line. */
    FILE *file = NULL;
    if (_wfopen_s(&file, path, L"a") != 0 || !file) {
        return false;
    }
    app->log = file;
    return true;
}

void app_log_close(App *app)
{
    if (!app->log) {
        return;
    }
    EnterCriticalSection(&app->log_lock);
    fclose(app->log);
    app->log = NULL;
    LeaveCriticalSection(&app->log_lock);
}

void app_log(App *app, const char *format, ...)
{
    if (!app || !app->log) {
        return;
    }
    EnterCriticalSection(&app->log_lock);
    if (app->log) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        fprintf(app->log, "%02u:%02u:%02u.%03u ",
                now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        va_list args;
        va_start(args, format);
        vfprintf(app->log, format, args);
        va_end(args);
        fputc('\n', app->log);
        fflush(app->log);
    }
    LeaveCriticalSection(&app->log_lock);
}

/* ------------------------------------------------------------------ */
/* Text conversion                                                    */
/* ------------------------------------------------------------------ */

void utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    if (cap == 0) {
        return;
    }
    dst[0] = 0;
    if (!src) {
        return;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap) <= 0) {
        dst[0] = 0;
    }
}

void wide_to_utf8(const wchar_t *src, char *dst, size_t cap)
{
    if (cap == 0) {
        return;
    }
    dst[0] = 0;
    if (!src) {
        return;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL, NULL) <= 0) {
        dst[0] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Pairing helpers                                                    */
/* ------------------------------------------------------------------ */

void make_token(char out[TOKEN_HEX_CHARS + 1])
{
    unsigned char bytes[16];
    if (BCryptGenRandom(NULL, bytes, (ULONG)sizeof(bytes),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        uint64_t seed = (uint64_t)counter.QuadPart ^ ((uint64_t)GetTickCount64() << 32) ^
                        (uint64_t)GetCurrentProcessId();
        for (int i = 0; i < 16; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            bytes[i] = (unsigned char)(seed >> 56);
        }
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
    out[TOKEN_HEX_CHARS] = 0;
}

void pairing_url(App *app, char *out, size_t cap)
{
    out[0] = 0;
    EnterCriticalSection(&app->lock);
    _snprintf_s(out, cap, _TRUNCATE, "http://%s:%d/c/%s",
                app->pairing.address, app->pairing.port, app->pairing.token);
    LeaveCriticalSection(&app->lock);
}

/* ------------------------------------------------------------------ */
/* Minimal JSON probing (messages are tiny and machine generated)      */
/* ------------------------------------------------------------------ */

const char *json_value(const char *json, const char *key)
{
    if (!json || !key) {
        return NULL;
    }
    char pattern[64];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\"", key);
    size_t pattern_len = strlen(pattern);
    const char *cursor = json;
    while ((cursor = strstr(cursor, pattern)) != NULL) {
        const char *value = cursor + pattern_len;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
            value++;
        }
        if (*value == ':') {
            value++;
            while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
                value++;
            }
            return value;
        }
        cursor += pattern_len;
    }
    return NULL;
}

bool json_string(const char *json, const char *key, char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    out[0] = 0;
    const char *value = json_value(json, key);
    if (!value || *value != '"') {
        return false;
    }
    value++;
    size_t used = 0;
    while (*value && *value != '"' && used + 1 < cap) {
        if (*value == '\\' && value[1]) {
            value++;
            switch (*value) {
            case 'n': out[used++] = '\n'; break;
            case 'r': out[used++] = '\r'; break;
            case 't': out[used++] = '\t'; break;
            default: out[used++] = *value; break;
            }
            value++;
            continue;
        }
        out[used++] = *value++;
    }
    out[used] = 0;
    return used > 0;
}

bool json_int(const char *json, const char *key, int *out)
{
    const char *value = json_value(json, key);
    if (!value) {
        return false;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }
    if (parsed < -2147483647L - 1 || parsed > 2147483647L) {
        return false;
    }
    *out = (int)parsed;
    return true;
}

bool json_bool(const char *json, const char *key, bool *out)
{
    const char *value = json_value(json, key);
    if (!value) {
        return false;
    }
    if (strncmp(value, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Clocks                                                             */
/* ------------------------------------------------------------------ */

static LONGLONG performance_frequency(void)
{
    static volatile LONG initialized = 0;
    static LONGLONG frequency = 1;
    if (InterlockedCompareExchange(&initialized, 1, 0) == 0) {
        LARGE_INTEGER value;
        if (QueryPerformanceFrequency(&value) && value.QuadPart > 0) {
            frequency = value.QuadPart;
        }
    }
    while (initialized != 1) {
        Sleep(0);
    }
    return frequency;
}

uint64_t now_ns(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    LONGLONG frequency = performance_frequency();
    uint64_t seconds = (uint64_t)(counter.QuadPart / frequency);
    uint64_t remainder = (uint64_t)(counter.QuadPart % frequency);
    return seconds * 1000000000ULL + (remainder * 1000000000ULL) / (uint64_t)frequency;
}

uint64_t now_ms(void)
{
    return (uint64_t)GetTickCount64();
}
