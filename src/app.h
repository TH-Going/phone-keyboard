/*
 * Phone Keyboard (pure C / Win32 port)
 *
 * Shared types and module interfaces. The PC side has no web view at all:
 * the pairing QR code is painted with GDI in a compact main window.
 * Closing the window leaves the server running in the notification area.
 */
#ifndef APP_H
#define APP_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#define _WIN32_WINNT 0x0A00

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "iphlpapi.lib")

#define APP_TITLE L"手键"

#define TRAY_CLASS L"PhoneKeyboardTray"
#define CARD_CLASS L"PhoneKeyboardCard"

#define TRAY_ICON_ID 1

#define WM_APP_TRAY (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)
#define WM_APP_REFRESH (WM_APP + 3)
#define WM_APP_SHOW (WM_APP + 4)
#define WM_APP_QUIT (WM_APP + 5)

#define TIMER_STATUS 1
#define TIMER_QUIT 2

#define TOKEN_HEX_CHARS 32
#define PAIRING_URL_MAX 192
#define MAX_HELD_KEYS 32

#define MENU_ID_REFRESH 102
#define MENU_ID_COPY 103
#define MENU_ID_QUIT 104
#define MENU_ID_SHOW 105

#define SPEED_MIN_PERCENT 50
#define SPEED_MAX_PERCENT 300
#define SPEED_STEP_PERCENT 10

typedef struct App App;
typedef struct Motion Motion;

typedef struct {
    char address[64];
    int port;
    char token[TOKEN_HEX_CHARS + 1];
} Pairing;

/* One keyboard key that is logically held down. */
typedef struct {
    char code[24];
    unsigned short vk;
    bool extended;
} HeldKey;

typedef struct {
    HeldKey keys[MAX_HELD_KEYS];
    int key_count;
    unsigned int buttons; /* bit0 left, bit1 right, bit2 middle */
    bool dry_run;
    Motion *motion;
    int mouse_speed_percent;
    int scroll_speed_percent;
    int move_remainder_x, move_remainder_y;
    int wheel_remainder_x, wheel_remainder_y;
} InputSession;

struct App {
    HINSTANCE instance;
    HICON icon;
    HWND tray;
    HWND card;

    /* options (mostly for automated tests and previews) */
    bool no_tray;
    bool dry_run;
    bool allow_multi;
    bool should_quit;
    ULONGLONG quit_at;       /* millisecond clock deadline, 0 = none */
    DWORD preview_ms;        /* legacy option: show the main window at startup */
    int forced_port;         /* 0 = pick an ephemeral port */
    char forced_token[TOKEN_HEX_CHARS + 1];
    wchar_t render_card_path[MAX_PATH];
    wchar_t qr_dump_path[MAX_PATH];
    wchar_t state_file_path[MAX_PATH];
    wchar_t log_path[MAX_PATH];

    /* Speed relative to the phone's baseline; read by connection threads. */
    volatile LONG mouse_speed_percent;
    volatile LONG scroll_speed_percent;

    /* pairing + server */
    CRITICAL_SECTION lock;
    Pairing pairing;
    SOCKET listener;
    SOCKET active;
    uint64_t active_id;
    volatile LONG server_stop;
    volatile LONG connected;

    /* notification area */
    bool tray_added;
    bool tray_version4;
    bool icon_rect_ok;
    RECT icon_rect;

    /* main window */
    bool card_visible;
    bool card_settings;

    /* logging */
    FILE *log;
    CRITICAL_SECTION log_lock;

};

/* util.c */
void app_log(App *app, const char *format, ...);
bool app_log_open(App *app, const wchar_t *path);
void app_log_close(App *app);
void utf8_to_wide(const char *src, wchar_t *dst, size_t cap);
void wide_to_utf8(const wchar_t *src, char *dst, size_t cap);
void make_token(char out[TOKEN_HEX_CHARS + 1]);
void pairing_url(App *app, char *out, size_t cap);
const char *json_value(const char *json, const char *key);
bool json_string(const char *json, const char *key, char *out, size_t cap);
bool json_int(const char *json, const char *key, int *out);
bool json_bool(const char *json, const char *key, bool *out);
uint64_t now_ns(void);
uint64_t now_ms(void);

/* net.c */
bool net_local_ipv4(char *out, size_t cap);

/* sha1.c */
void sha1_digest(const unsigned char *data, size_t len, unsigned char out[20]);
void base64_encode(const unsigned char *data, size_t len, char *out, size_t cap);

/* input.c */
void input_session_init(InputSession *session, bool dry_run);
void input_session_set_speed(InputSession *session, int mouse_percent, int scroll_percent);
void input_session_dispose(InputSession *session);
bool input_apply_json(InputSession *session, const char *json);
void input_release_all(InputSession *session);
bool input_send_relative_move(int dx, int dy);

/* motion.c */
Motion *motion_create(bool dry_run);
void motion_destroy(Motion *motion);
bool motion_push(Motion *motion, int dx, int dy);
bool motion_flush(Motion *motion);
void motion_cancel(Motion *motion);

/* server.c */
bool server_start(App *app);
void server_stop(App *app);
void server_rotate_token(App *app);
bool server_connected(App *app);

/* card.c */
bool card_create(App *app);
void card_destroy(App *app);
void card_show(App *app);
void card_hide(App *app);
void card_refresh(App *app);
bool card_render_to_bmp(App *app, const wchar_t *path);
bool card_dump_qr(App *app, const wchar_t *path);

/* main.c helpers used by other modules */
void tray_refresh_rect(App *app);
bool tray_icon_rect(App *app, RECT *out);
void app_request_quit(App *app);
void app_write_state(App *app);
void app_set_speed(App *app, bool scroll, int percent, bool persist);

#endif /* APP_H */
