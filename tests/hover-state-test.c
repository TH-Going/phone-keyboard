/* Main-window and tray regression tests. Native controls receive messages,
 * but the top-level window stays hidden and settings use a registry double.
 * The historical filename is kept for existing test commands. */
#include "../src/app.h"

static uint64_t test_now;
static POINT test_cursor;
static RECT test_icon;
static bool test_rect_ok;
static bool test_cursor_ok;
static DWORD test_saved_speed[2];
static LSTATUS test_read_result[2] = {ERROR_FILE_NOT_FOUND, ERROR_FILE_NOT_FOUND};
static LSTATUS test_create_result, test_write_result;
static int test_warnings;
static int test_menu_command;
static int test_write_count;

static int setting_index(const wchar_t *name)
{
    if (wcscmp(name, L"MouseSpeedPercent") == 0) return 0;
    if (wcscmp(name, L"ScrollSpeedPercent") == 0) return 1;
    return -1;
}

static LSTATUS WINAPI test_reg_get_value(HKEY key, LPCWSTR path, LPCWSTR name, DWORD flags,
                                        LPDWORD type, PVOID data, LPDWORD bytes)
{
    (void)type;
    int index = setting_index(name);
    if (key != HKEY_CURRENT_USER || wcscmp(path, L"Software\\PhoneKeyboardC") != 0 ||
        flags != RRF_RT_REG_DWORD || *bytes != sizeof(DWORD) || index < 0) return ERROR_INVALID_PARAMETER;
    if (test_read_result[index] != ERROR_SUCCESS) return test_read_result[index];
    memcpy(data, &test_saved_speed[index], sizeof(DWORD));
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI test_reg_create_key(HKEY key, LPCWSTR path, DWORD reserved, LPWSTR name,
                                         DWORD options, REGSAM access, const LPSECURITY_ATTRIBUTES security,
                                         PHKEY result, LPDWORD disposition)
{
    (void)reserved; (void)name; (void)security; (void)disposition;
    if (key != HKEY_CURRENT_USER || wcscmp(path, L"Software\\PhoneKeyboardC") != 0 ||
        options != REG_OPTION_NON_VOLATILE || access != KEY_SET_VALUE) return ERROR_INVALID_PARAMETER;
    if (test_create_result != ERROR_SUCCESS) return test_create_result;
    *result = (HKEY)(ULONG_PTR)1;
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI test_reg_set_value(HKEY key, LPCWSTR name, DWORD reserved, DWORD type,
                                        const BYTE *data, DWORD bytes)
{
    (void)reserved;
    int index = setting_index(name);
    if (key != (HKEY)(ULONG_PTR)1 || type != REG_DWORD || bytes != sizeof(DWORD) || index < 0)
        return ERROR_INVALID_PARAMETER;
    if (test_write_result != ERROR_SUCCESS) return test_write_result;
    memcpy(&test_saved_speed[index], data, sizeof(DWORD));
    test_write_count++;
    test_read_result[index] = ERROR_SUCCESS;
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI test_reg_close_key(HKEY key) { (void)key; return ERROR_SUCCESS; }
static int WINAPI test_message_box(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags)
{
    (void)owner; (void)text; (void)title; (void)flags;
    test_warnings++;
    return IDOK;
}

static BOOL WINAPI test_track_menu(HMENU menu, UINT flags, int x, int y, int reserved, HWND owner, const RECT *rect)
{
    (void)menu; (void)flags; (void)x; (void)y; (void)reserved; (void)owner; (void)rect;
    return test_menu_command;
}

static uint64_t test_now_ms(void)
{
    return test_now;
}

static BOOL WINAPI test_get_cursor(POINT *point)
{
    *point = test_cursor;
    return test_cursor_ok;
}

static HRESULT WINAPI test_get_icon_rect(const NOTIFYICONIDENTIFIER *id, RECT *rect)
{
    (void)id;
    *rect = test_icon;
    return test_rect_ok ? S_OK : E_FAIL;
}

static BOOL WINAPI test_show_window(HWND window, int command)
{
    if (GetParent(window)) return ShowWindow(window, command);
    return TRUE;
}

static BOOL WINAPI test_foreground(HWND window) { (void)window; return TRUE; }
static HWND WINAPI test_focus(HWND window) { (void)window; return NULL; }

#define now_ms test_now_ms
#define GetCursorPos test_get_cursor
#define Shell_NotifyIconGetRect test_get_icon_rect
#define ShowWindow test_show_window
#define SetForegroundWindow test_foreground
#define SetFocus test_focus
#define RegGetValueW test_reg_get_value
#define RegCreateKeyExW test_reg_create_key
#define RegSetValueExW test_reg_set_value
#define RegCloseKey test_reg_close_key
#define MessageBoxW test_message_box
#define TrackPopupMenu test_track_menu
#include "../src/main.c"
#include "../src/card.c"

static int failures;

static void check(bool condition, const char *message)
{
    printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) {
        failures++;
    }
}

static void reset(App *app)
{
    memset(app, 0, sizeof(*app));
    app->instance = GetModuleHandleW(NULL);
    InitializeCriticalSection(&app->lock);
    InitializeCriticalSection(&app->log_lock);
    strcpy_s(app->pairing.address, sizeof(app->pairing.address), "192.168.0.17");
    strcpy_s(app->pairing.token, sizeof(app->pairing.token), "0123456789abcdef0123456789abcdef");
    app->pairing.port = 61252;
    app->tray_added = true;
    app->tray_version4 = true;
    g_app = app;
    test_now = 1000;
    test_cursor = (POINT){20, 20};
    test_icon = (RECT){10, 10, 30, 30};
    test_rect_ok = true;
    test_cursor_ok = true;
}

static void notify_icon(App *app, UINT event, UINT icon_id)
{
    WPARAM wparam = app->tray_version4 ? MAKELPARAM(test_cursor.x, test_cursor.y) : icon_id;
    LPARAM lparam = app->tray_version4 ? MAKELPARAM(event, icon_id) : event;
    tray_proc(app->tray, WM_APP_TRAY, wparam, lparam);
}

static void move_slider(App *app, bool scroll, int position, UINT event)
{
    HWND slider = scroll ? g_controls.scroll : g_controls.mouse;
    SendMessageW(slider, TBM_SETPOS, TRUE, position);
    SendMessageW(app->card, WM_HSCROLL, MAKEWPARAM(event, position), (LPARAM)slider);
}

int wmain(int argc, wchar_t **argv)
{
    App app;
    reset(&app);
    app.active = app.listener = INVALID_SOCKET;
    load_speed_settings(&app);
    check(app.mouse_speed_percent == 100 && app.scroll_speed_percent == 100,
          "missing settings use 1.0x for both multipliers");
    check(card_create(&app), "native main window and controls are created");
    if (!app.card) return 1;
    wchar_t title[64];
    GetWindowTextW(app.card, title, 64);
    check(wcscmp(title, L"手键") == 0, "the window is named 手键");
    LONG_PTR style = GetWindowLongPtrW(app.card, GWL_EXSTYLE);
    check(!(style & (WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED)),
          "main window accepts native controls and is not always on top");

    notify_icon(&app, NIN_POPUPOPEN, TRAY_ICON_ID);
    notify_icon(&app, WM_MOUSEMOVE, TRAY_ICON_ID);
    status_tick(&app);
    check(!app.card_visible, "tray hover never opens the main window");
    notify_icon(&app, NIN_SELECT, TRAY_ICON_ID + 1);
    check(!app.card_visible, "another tray icon cannot open the window");
    notify_icon(&app, NIN_SELECT, TRAY_ICON_ID);
    check(app.card_visible, "left clicking the tray opens the main window");
    test_now += 20000;
    notify_icon(&app, NIN_POPUPCLOSE, TRAY_ICON_ID);
    status_tick(&app);
    check(app.card_visible, "leaving the tray and waiting never hides the main window");
    notify_icon(&app, NIN_SELECT, TRAY_ICON_ID);
    check(app.card_visible, "clicking an open window's tray icon keeps it open");
    SendMessageW(app.card, WM_CLOSE, 0, 0);
    check(!app.card_visible && !app.should_quit, "close hides the window and keeps the app running");
    notify_icon(&app, WM_MOUSEMOVE, TRAY_ICON_ID);
    check(!app.card_visible, "hover does not reopen a closed main window");
    notify_icon(&app, NIN_KEYSELECT, TRAY_ICON_ID);
    check(app.card_visible, "keyboard activation of the tray reopens the window");
    app.tray_version4 = false;
    card_hide(&app);
    notify_icon(&app, WM_MOUSEMOVE, TRAY_ICON_ID);
    check(!app.card_visible, "legacy tray mouse movement does not show the window");
    notify_icon(&app, WM_LBUTTONUP, TRAY_ICON_ID);
    check(app.card_visible, "legacy tray left click still opens the window");
    app.tray_version4 = true;

    HMENU menu = create_tray_menu(&app);
    check(menu && GetMenuItemCount(menu) == 6 && !GetSubMenu(menu, 0),
          "tray menu contains open, refresh, copy and exit, without speed submenus");
    GetMenuStringW(menu, 0, title, 64, MF_BYPOSITION);
    check(wcscmp(title, L"打开主界面") == 0, "tray menu offers opening the main window");
    DestroyMenu(menu);
    card_hide(&app);
    test_menu_command = MENU_ID_SHOW;
    notify_icon(&app, WM_CONTEXTMENU, TRAY_ICON_ID);
    check(app.card_visible, "tray context menu can open the main window");
    card_hide(&app);
    tray_proc(app.tray, WM_APP_SHOW, 0, 0);
    check(app.card_visible, "relaunch show message reopens the existing instance");

    RECT before, after;
    GetWindowRect(app.card, &before);
    tray_proc(app.tray, WM_APP_STATUS, 0, 0);
    GetWindowRect(app.card, &after);
    check(EqualRect(&before, &after), "connection updates preserve the window position");
    POINT caption = {before.left + scale_px(25, g_layout.dpi), before.top + scale_px(20, g_layout.dpi)};
    check(SendMessageW(app.card, WM_NCHITTEST, 0, MAKELPARAM(caption.x, caption.y)) == HTCAPTION,
          "the title area can be dragged");
    POINT close = {before.left + scale_px(255, g_layout.dpi), before.top + scale_px(20, g_layout.dpi)};
    check(SendMessageW(app.card, WM_NCHITTEST, 0, MAKELPARAM(close.x, close.y)) == HTCLIENT,
          "the icon area stays clickable rather than dragging the window");

    SendMessageW(app.card, WM_COMMAND, CARD_ID_SETTINGS, 0);
    check(app.card_settings && (GetWindowLongPtrW(g_controls.mouse, GWL_STYLE) & WS_VISIBLE),
          "the settings icon opens the sliders");
    check(SendMessageW(g_controls.mouse, TBM_GETRANGEMIN, 0, 0) == 5 &&
          SendMessageW(g_controls.mouse, TBM_GETRANGEMAX, 0, 0) == 30 &&
          SendMessageW(g_controls.scroll, TBM_GETRANGEMIN, 0, 0) == 5 &&
          SendMessageW(g_controls.scroll, TBM_GETRANGEMAX, 0, 0) == 30,
          "both sliders use integer positions 5 to 30 for exact 0.1x steps");
    move_slider(&app, false, 17, TB_THUMBTRACK);
    check(app.mouse_speed_percent == 170 && app.scroll_speed_percent == 100 && test_write_count == 0,
          "dragging applies sensitivity immediately without repeated registry writes");
    move_slider(&app, false, 17, TB_THUMBTRACK);
    move_slider(&app, false, 17, TB_ENDTRACK);
    check(test_saved_speed[0] == 170 && test_write_count == 1,
          "release saves the final value even when its position no longer changes");
    move_slider(&app, false, 17, TB_ENDTRACK);
    check(test_write_count == 1, "duplicate release notifications do not save twice");
    move_slider(&app, true, 24, TB_THUMBPOSITION);
    check(app.scroll_speed_percent == 240 && app.mouse_speed_percent == 170 && test_saved_speed[1] == 240,
          "scroll multiplier applies and saves independently");
    SendMessageW(g_controls.mouse, WM_KEYDOWN, VK_RIGHT, 0);
    SendMessageW(g_controls.mouse, WM_KEYUP, VK_RIGHT, 0);
    check(app.mouse_speed_percent == 180 && test_saved_speed[0] == 180,
          "a native keyboard arrow advances the multiplier by exactly 0.1x and saves");
    move_slider(&app, true, 5, TB_ENDTRACK);
    check(app.scroll_speed_percent == 50 && test_saved_speed[1] == 50, "the minimum is 0.5x");
    move_slider(&app, true, 30, TB_ENDTRACK);
    check(app.scroll_speed_percent == 300 && test_saved_speed[1] == 300, "the maximum is 3.0x");

    move_slider(&app, false, 23, TB_THUMBTRACK);
    SendMessageW(app.card, WM_COMMAND, CARD_ID_CLOSE, 0);
    check(!app.card_visible && !app.card_settings && !app.should_quit && test_saved_speed[0] == 230,
          "closing during a drag saves the pending value and returns to the tray");
    card_show(&app);
    SendMessageW(app.card, WM_COMMAND, CARD_ID_SETTINGS, 0);
    check(SendMessageW(g_controls.mouse, TBM_GETPOS, 0, 0) == 23,
          "reopening settings preserves the saved multiplier");
    move_slider(&app, true, 12, TB_THUMBTRACK);
    SendMessageW(app.card, WM_ACTIVATE, WA_INACTIVE, 0);
    check(app.card_visible && test_saved_speed[1] == 120,
          "losing focus saves a pending slider without hiding the window");
    move_slider(&app, false, 16, TB_THUMBTRACK);
    SendMessageW(app.card, WM_COMMAND, CARD_ID_DONE, 0);
    check(!app.card_settings && test_saved_speed[0] == 160, "Done saves and returns to the QR page");
    SendMessageW(app.card, WM_COMMAND, CARD_ID_SETTINGS, 0);
    SendMessageW(app.card, WM_COMMAND, CARD_ID_BACK, 0);
    check(!app.card_settings, "the back icon returns to the QR page");

    load_speed_settings(&app);
    check(app.mouse_speed_percent == 160 && app.scroll_speed_percent == 120,
          "a fresh load restores both new decimal-step settings");
    test_saved_speed[0] = 75;
    test_saved_speed[1] = 125;
    load_speed_settings(&app);
    sync_controls(&app);
    check(app.mouse_speed_percent == 80 && app.scroll_speed_percent == 130 &&
          SendMessageW(g_controls.mouse, TBM_GETPOS, 0, 0) == 8 &&
          SendMessageW(g_controls.scroll, TBM_GETPOS, 0, 0) == 13,
          "legacy 0.75x and 1.25x restore as 0.8x and 1.3x in memory and controls");
    test_saved_speed[0] = 110;
    test_saved_speed[1] = 290;
    load_speed_settings(&app);
    check(app.mouse_speed_percent == 110 && app.scroll_speed_percent == 290,
          "all new 0.1x step settings are accepted on load");
    test_saved_speed[0] = 49;
    test_saved_speed[1] = 301;
    load_speed_settings(&app);
    check(app.mouse_speed_percent == 100 && app.scroll_speed_percent == 100,
          "saved values outside the allowed range use defaults");
    test_saved_speed[0] = 0xFFFFFFFF;
    test_read_result[1] = ERROR_UNSUPPORTED_TYPE;
    load_speed_settings(&app);
    check(app.mouse_speed_percent == 100 && app.scroll_speed_percent == 100,
          "corrupt DWORDs and incorrect registry types safely use defaults");

    SendMessageW(app.card, WM_COMMAND, CARD_ID_SETTINGS, 0);
    test_create_result = ERROR_ACCESS_DENIED;
    move_slider(&app, false, 19, TB_THUMBTRACK);
    check(app.mouse_speed_percent == 190 && test_warnings == 0,
          "dragging does not interrupt the user with persistence errors");
    move_slider(&app, false, 19, TB_ENDTRACK);
    move_slider(&app, false, 19, TB_ENDTRACK);
    check(app.mouse_speed_percent == 190 && test_warnings == 1,
          "failed persistence keeps the live value and warns once on release");
    test_create_result = ERROR_SUCCESS;
    test_write_result = ERROR_ACCESS_DENIED;
    move_slider(&app, true, 21, TB_ENDTRACK);
    check(app.scroll_speed_percent == 210 && test_warnings == 2,
          "registry write failure also preserves the live multiplier");
    test_write_result = ERROR_SUCCESS;

    int dpis[] = {96, 120, 144, 192};
    bool geometry_ok = true;
    for (int i = 0; i < 4; i++) {
        CardLayout layout;
        geometry_ok = geometry_ok && build_layout(&app, dpis[i], &layout) &&
            layout.win_w == scale_px(282, dpis[i]) && layout.win_h == scale_px(338, dpis[i]) &&
            layout.qr_x >= 0 && layout.qr_x + layout.qr_px <= layout.win_w &&
            layout.qr_y + layout.qr_px <= scale_px(295, dpis[i]);
    }
    check(geometry_ok, "282 by 338 layout and integer QR modules fit at 100, 125, 150 and 200 percent DPI");
    RECT suggested = {100, 100, 664, 776};
    SendMessageW(app.card, WM_DPICHANGED, MAKELONG(192, 192), (LPARAM)&suggested);
    GetWindowRect(app.card, &after);
    check(after.right - after.left == 564 && after.bottom - after.top == 676,
          "native window and controls resize on a monitor DPI change");
    suggested = (RECT){100, 100, 382, 438};
    SendMessageW(app.card, WM_DPICHANGED, MAKELONG(96, 96), (LPARAM)&suggested);
    if (argc > 1) {
        wchar_t path[MAX_PATH];
        app.card_settings = false;
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\main-window.bmp", argv[1]);
        check(card_render_to_bmp(&app, path), "main window snapshot rendered");
        app.card_settings = true;
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\settings-window.bmp", argv[1]);
        check(card_render_to_bmp(&app, path), "settings snapshot rendered");
    }
    app.should_quit = false;
    tray_proc(app.tray, WM_ENDSESSION, FALSE, 0);
    check(!app.should_quit, "a canceled Windows session ending leaves the app running");
    move_slider(&app, false, 27, TB_THUMBTRACK);
    tray_proc(app.tray, WM_ENDSESSION, TRUE, 0);
    check(app.should_quit && test_saved_speed[0] == 270,
          "Windows session ending synchronously saves a pending slider before quitting");
    card_destroy(&app);
    DeleteCriticalSection(&app.log_lock);
    DeleteCriticalSection(&app.lock);
    printf("%d failed check(s)\n", failures);
    return failures ? 1 : 0;
}
