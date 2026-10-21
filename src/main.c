/*
 * Entry point: notification-area icon, speed settings and message loop.
 * The main window stays open until explicitly closed to the tray.
 */
#include "app.h"

static App *g_app;

/* ------------------------------------------------------------------ */
/* Tray icon geometry                                                 */
/* ------------------------------------------------------------------ */

void tray_refresh_rect(App *app)
{
    app->icon_rect_ok = false;
    if (!app->tray_added) {
        return;
    }
    NOTIFYICONIDENTIFIER identifier;
    memset(&identifier, 0, sizeof(identifier));
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = app->tray;
    identifier.uID = TRAY_ICON_ID;

    RECT rect;
    memset(&rect, 0, sizeof(rect));
    if (Shell_NotifyIconGetRect(&identifier, &rect) == S_OK &&
        rect.right > rect.left && rect.bottom > rect.top) {
        app->icon_rect = rect;
        app->icon_rect_ok = true;
    }
}

bool tray_icon_rect(App *app, RECT *out)
{
    if (!app->icon_rect_ok) {
        return false;
    }
    *out = app->icon_rect;
    return true;
}

/* Diagnostics channel for the automated tests: the current icon rectangle and
 * visibility of the main window, rewritten on every state change. The file is
 * written next to the target and then swapped in, so a reader never sees a
 * truncated file. */
void app_write_state(App *app)
{
    if (!app->state_file_path[0]) {
        return;
    }

    wchar_t temporary[MAX_PATH + 8];
    if (_snwprintf_s(temporary, MAX_PATH + 8, _TRUNCATE, L"%s.tmp", app->state_file_path) < 0) {
        return;
    }

    FILE *file = NULL;
    if (_wfopen_s(&file, temporary, L"w") != 0 || !file) {
        return;
    }
    fprintf(file, "rect=%ld,%ld,%ld,%ld\n",
            app->icon_rect_ok ? app->icon_rect.left : 0,
            app->icon_rect_ok ? app->icon_rect.top : 0,
            app->icon_rect_ok ? app->icon_rect.right : 0,
            app->icon_rect_ok ? app->icon_rect.bottom : 0);
    fprintf(file, "rect_ok=%d\n", app->icon_rect_ok ? 1 : 0);
    fprintf(file, "visible=%d\n", app->card_visible ? 1 : 0);
    fprintf(file, "settings=%d\n", app->card_settings ? 1 : 0);
    fprintf(file, "mouse_speed=%ld\n", InterlockedCompareExchange(&app->mouse_speed_percent, 0, 0));
    fprintf(file, "scroll_speed=%ld\n", InterlockedCompareExchange(&app->scroll_speed_percent, 0, 0));
    fclose(file);

    MoveFileExW(temporary, app->state_file_path, MOVEFILE_REPLACE_EXISTING);
}

/* ------------------------------------------------------------------ */
/* Commands                                                           */
/* ------------------------------------------------------------------ */

#define SETTINGS_KEY L"Software\\PhoneKeyboardC"

static int normalize_speed(int percent)
{
    if (percent < SPEED_MIN_PERCENT) percent = SPEED_MIN_PERCENT;
    if (percent > SPEED_MAX_PERCENT) percent = SPEED_MAX_PERCENT;
    return ((percent + SPEED_STEP_PERCENT / 2) / SPEED_STEP_PERCENT) * SPEED_STEP_PERCENT;
}

static int load_speed_setting(const wchar_t *name)
{
    DWORD value = 100;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD,
                     NULL, &value, &bytes) != ERROR_SUCCESS ||
        value < SPEED_MIN_PERCENT || value > SPEED_MAX_PERCENT) {
        return 100;
    }
    return normalize_speed((int)value);
}

static void load_speed_settings(App *app)
{
    InterlockedExchange(&app->mouse_speed_percent, load_speed_setting(L"MouseSpeedPercent"));
    InterlockedExchange(&app->scroll_speed_percent, load_speed_setting(L"ScrollSpeedPercent"));
}

static LSTATUS save_speed_setting(const wchar_t *name, int percent)
{
    HKEY key;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL,
                                    REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
    if (result != ERROR_SUCCESS) return result;
    DWORD value = (DWORD)percent;
    result = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(key);
    return result;
}

void app_set_speed(App *app, bool scroll, int percent, bool persist)
{
    percent = normalize_speed(percent);
    InterlockedExchange(scroll ? &app->scroll_speed_percent : &app->mouse_speed_percent, percent);
    app_write_state(app);
    if (!persist) return;
    LSTATUS result = save_speed_setting(scroll ? L"ScrollSpeedPercent" : L"MouseSpeedPercent", percent);
    app_log(app, "%s speed=%d%% save=%ld", scroll ? "scroll" : "mouse", percent, result);
    if (result != ERROR_SUCCESS) {
        MessageBoxW(app->card ? app->card : app->tray, L"设置已生效，但无法保存。下次启动时可能恢复为原来的速度。",
                    APP_TITLE, MB_OK | MB_ICONWARNING);
    }
}

void app_request_quit(App *app)
{
    if (app->should_quit) {
        return;
    }
    app->should_quit = true;
    PostQuitMessage(0);
}

static void copy_pairing_url(App *app)
{
    char url[PAIRING_URL_MAX];
    pairing_url(app, url, sizeof(url));
    if (!url[0]) {
        return;
    }
    wchar_t wide[PAIRING_URL_MAX];
    utf8_to_wide(url, wide, sizeof(wide) / sizeof(wide[0]));

    if (!OpenClipboard(app->tray)) {
        return;
    }
    EmptyClipboard();
    size_t bytes = (wcslen(wide) + 1) * sizeof(wchar_t);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (block) {
        void *target = GlobalLock(block);
        if (target) {
            memcpy(target, wide, bytes);
            GlobalUnlock(block);
            if (!SetClipboardData(CF_UNICODETEXT, block)) {
                GlobalFree(block);
            }
        } else {
            GlobalFree(block);
        }
    }
    CloseClipboard();
    app_log(app, "pairing url copied");
}

static void handle_command(App *app, int command)
{
    switch (command) {
    case MENU_ID_SHOW:
        card_show(app);
        break;
    case MENU_ID_REFRESH:
        server_rotate_token(app);
        app->card_settings = false;
        card_show(app);
        break;
    case MENU_ID_COPY:
        copy_pairing_url(app);
        break;
    case MENU_ID_QUIT:
        app_request_quit(app);
        break;
    default:
        break;
    }
}

static HMENU create_tray_menu(App *app)
{
    (void)app;
    HMENU menu = CreatePopupMenu();
    if (!menu) return NULL;
    AppendMenuW(menu, MF_STRING, MENU_ID_SHOW, L"打开主界面");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_ID_REFRESH, L"重新生成二维码");
    AppendMenuW(menu, MF_STRING, MENU_ID_COPY, L"复制链接");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_ID_QUIT, L"退出");
    return menu;
}

static void show_menu(App *app)
{
    POINT cursor;
    GetCursorPos(&cursor);
    HMENU menu = create_tray_menu(app);
    if (!menu) return;

    SetForegroundWindow(app->tray);
    int command = (int)TrackPopupMenu(menu,
                                      TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                      cursor.x, cursor.y, 0, app->tray, NULL);
    /* The shell wants a benign message after a tray menu closes. */
    PostMessageW(app->tray, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command) {
        handle_command(app, command);
    }
}

/* ------------------------------------------------------------------ */
/* Test deadlines and diagnostic tray geometry                       */
/* ------------------------------------------------------------------ */

static void status_tick(App *app)
{
    uint64_t now = now_ms();
    if (app->quit_at && now >= app->quit_at) {
        app_request_quit(app);
        return;
    }
    if (app->state_file_path[0]) {
        tray_refresh_rect(app);
        app_write_state(app);
    }
}

/* ------------------------------------------------------------------ */
/* Tray window                                                        */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK tray_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    App *app = g_app;
    if (!app) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_APP_TRAY: {
        UINT icon_id = app->tray_version4 ? HIWORD(lparam) : (UINT)wparam;
        UINT event = app->tray_version4 ? LOWORD(lparam) : (UINT)lparam;
        if (icon_id != TRAY_ICON_ID) {
            break;
        }
        switch (event) {
        case NIN_POPUPOPEN:
        case WM_MOUSEMOVE:
        case NIN_POPUPCLOSE:
            break;
        case WM_LBUTTONUP:
            if (app->tray_version4) {
                break;
            }
            /* fall through */
        case NIN_SELECT:
        case NIN_KEYSELECT:
            card_show(app);
            break;
        case WM_RBUTTONUP:
            if (app->tray_version4) {
                break;
            }
            /* fall through */
        case WM_CONTEXTMENU:
            show_menu(app);
            break;
        case WM_LBUTTONDBLCLK:
            card_show(app);
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_TIMER:
        if (wparam == TIMER_STATUS) {
            status_tick(app);
            return 0;
        }
        if (wparam == TIMER_QUIT) {
            app_request_quit(app);
            return 0;
        }
        break;

    case WM_APP_STATUS:
        card_refresh(app);
        return 0;

    case WM_APP_REFRESH:
        card_refresh(app);
        return 0;

    case WM_APP_SHOW:
        card_show(app);
        return 0;

    case WM_APP_QUIT:
        app_request_quit(app);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ENDSESSION:
        if (wparam) {
            card_hide(app);
            app_request_quit(app);
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static bool tray_init(App *app)
{
    WNDCLASSEXW window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = tray_proc;
    window_class.hInstance = app->instance;
    window_class.lpszClassName = TRAY_CLASS;

    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    /* A hidden top-level window (never a message-only one) so that the shell
     * can hand it the foreground while the context menu is open. */
    app->tray = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, APP_TITLE, WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, app->instance, NULL);
    if (!app->tray) {
        return false;
    }

    app->icon = LoadIconW(app->instance, MAKEINTRESOURCEW(1));
    if (!app->icon) {
        app->icon = LoadIconW(NULL, IDI_APPLICATION);
    }

    NOTIFYICONDATAW data;
    memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = app->tray;
    data.uID = TRAY_ICON_ID;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = WM_APP_TRAY;
    data.hIcon = app->icon;
    wcscpy_s(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), APP_TITLE);
    app->tray_added = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;

    if (!app->tray_added) {
        return false;
    }

    /* Version 4 supports both mouse and keyboard activation. */
    data.uVersion = NOTIFYICON_VERSION_4;
    app->tray_version4 = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;

    if (app->quit_at || app->state_file_path[0]) SetTimer(app->tray, TIMER_STATUS, 250, NULL);
    tray_refresh_rect(app);
    app_write_state(app);
    return true;
}

static void tray_remove(App *app)
{
    if (!app->tray_added) {
        return;
    }
    NOTIFYICONDATAW data;
    memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = app->tray;
    data.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &data);
    app->tray_added = false;
}

/* ------------------------------------------------------------------ */
/* Command line                                                       */
/* ------------------------------------------------------------------ */

static bool argument_is(const wchar_t *argument, const wchar_t *name)
{
    return _wcsicmp(argument, name) == 0;
}

static void print_help(void)
{
    MessageBoxW(NULL,
                L"手键（纯 C 版）\n\n"
                L"Usage: PhoneKeyboardC.exe [options]\n\n"
                L"  --port <n>          listen on a fixed TCP port (default: auto)\n"
                L"  --token <32 hex>    use a fixed pairing token (default: random)\n"
                L"  --log <file>        append diagnostics to a UTF-8 log file\n"
                L"  --preview <ms>      show the main window (legacy option)\n"
                L"  --render-card <bmp> render the card to a BMP and exit\n"
                L"  --qr-dump <txt>     write the QR module matrix and exit\n"
                L"  --no-tray           do not create the notification-area icon\n"
                L"  --allow-multi       skip the single-instance guard\n"
                L"  --dry-run           never inject keyboard or mouse input\n"
                L"  --quit-after <ms>   exit automatically (testing)\n\n"
                L"启动后显示二维码主窗口；关闭窗口后继续在托盘运行。\n"
                L"单击托盘图标重新打开，右键菜单可换码、复制链接或退出。\n"
                L"窗口右上角的设置图标可调整鼠标灵敏度和滚动倍率。",
                APP_TITLE, MB_OK | MB_ICONINFORMATION);
}

static bool option_value(int argc, wchar_t **argv, int *index, wchar_t *out, size_t cap)
{
    if (*index + 1 >= argc) {
        return false;
    }
    *index += 1;
    wcsncpy_s(out, cap, argv[*index], _TRUNCATE);
    return true;
}

static void parse_arguments(App *app, int argc, wchar_t **argv)
{
    for (int i = 1; i < argc; i++) {
        const wchar_t *argument = argv[i];
        if (argument_is(argument, L"--help") || argument_is(argument, L"-h") ||
            argument_is(argument, L"/?")) {
            print_help();
            ExitProcess(0);
        } else if (argument_is(argument, L"--port")) {
            wchar_t value[32];
            if (option_value(argc, argv, &i, value, sizeof(value) / sizeof(value[0]))) {
                app->forced_port = _wtoi(value);
            }
        } else if (argument_is(argument, L"--token")) {
            wchar_t value[64];
            if (option_value(argc, argv, &i, value, sizeof(value) / sizeof(value[0]))) {
                char utf8[TOKEN_HEX_CHARS + 1];
                wide_to_utf8(value, utf8, sizeof(utf8));
                bool valid = strlen(utf8) == TOKEN_HEX_CHARS;
                for (size_t c = 0; valid && c < TOKEN_HEX_CHARS; c++) {
                    valid = isxdigit((unsigned char)utf8[c]) != 0;
                }
                if (valid) {
                    _snprintf_s(app->forced_token, sizeof(app->forced_token), _TRUNCATE, "%s", utf8);
                }
            }
        } else if (argument_is(argument, L"--log")) {
            option_value(argc, argv, &i, app->log_path,
                         sizeof(app->log_path) / sizeof(app->log_path[0]));
        } else if (argument_is(argument, L"--render-card")) {
            option_value(argc, argv, &i, app->render_card_path,
                         sizeof(app->render_card_path) / sizeof(app->render_card_path[0]));
        } else if (argument_is(argument, L"--qr-dump")) {
            option_value(argc, argv, &i, app->qr_dump_path,
                         sizeof(app->qr_dump_path) / sizeof(app->qr_dump_path[0]));
        } else if (argument_is(argument, L"--state-file")) {
            option_value(argc, argv, &i, app->state_file_path,
                         sizeof(app->state_file_path) / sizeof(app->state_file_path[0]));
        } else if (argument_is(argument, L"--preview")) {
            wchar_t value[32];
            if (option_value(argc, argv, &i, value, sizeof(value) / sizeof(value[0]))) {
                app->preview_ms = (DWORD)_wtoi(value);
            }
        } else if (argument_is(argument, L"--quit-after")) {
            wchar_t value[32];
            if (option_value(argc, argv, &i, value, sizeof(value) / sizeof(value[0]))) {
                DWORD milliseconds = (DWORD)_wtoi(value);
                app->quit_at = now_ms() + milliseconds;
            }
        } else if (argument_is(argument, L"--no-tray")) {
            app->no_tray = true;
        } else if (argument_is(argument, L"--dry-run")) {
            app->dry_run = true;
        } else if (argument_is(argument, L"--allow-multi")) {
            app->allow_multi = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

static void fill_preview_pairing(App *app)
{
    char address[64];
    if (!net_local_ipv4(address, sizeof(address))) {
        _snprintf_s(address, sizeof(address), _TRUNCATE, "192.168.1.23");
    }
    _snprintf_s(app->pairing.address, sizeof(app->pairing.address), _TRUNCATE, "%s", address);
    app->pairing.port = app->forced_port > 0 ? app->forced_port : 54321;
    if (app->forced_token[0]) {
        memcpy(app->pairing.token, app->forced_token, sizeof(app->pairing.token));
    } else {
        make_token(app->pairing.token);
    }
}

static void render_preview_and_exit(App *app)
{
    fill_preview_pairing(app);
    bool ok = card_render_to_bmp(app, app->render_card_path);
    ExitProcess(ok ? 0 : 1);
}

static void dump_qr_and_exit(App *app)
{
    fill_preview_pairing(app);
    bool ok = card_dump_qr(app, app->qr_dump_path);
    ExitProcess(ok ? 0 : 1);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance,
                    PWSTR command_line, int show_command)
{
    (void)previous_instance;
    (void)command_line;
    (void)show_command;

    App *app = (App *)calloc(1, sizeof(App));
    if (!app) {
        return 1;
    }
    g_app = app;
    app->instance = instance;
    InitializeCriticalSection(&app->lock);
    InitializeCriticalSection(&app->log_lock);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        parse_arguments(app, argc, argv);
        LocalFree(argv);
    }

    if (app->log_path[0]) {
        app_log_open(app, app->log_path);
    }
    app_log(app, "starting (pid=%lu dry_run=%d)", GetCurrentProcessId(), app->dry_run);

    typedef BOOL(WINAPI * SetDpiAwarenessFn)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetDpiAwarenessFn set_dpi_awareness =
            (SetDpiAwarenessFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (set_dpi_awareness) {
            set_dpi_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    WSADATA winsock_data;
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        app_log(app, "WSAStartup failed");
        return 1;
    }

    HANDLE single_instance = NULL;
    if (!app->allow_multi) {
        single_instance = CreateMutexW(NULL, TRUE, L"Local\\PhoneKeyboardC-SingleInstance");
        if (!single_instance) {
            return 1;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            app_log(app, "another instance is already running");
            HWND existing = FindWindowW(TRAY_CLASS, NULL);
            if (existing) {
                DWORD process_id = 0;
                GetWindowThreadProcessId(existing, &process_id);
                if (process_id) AllowSetForegroundWindow(process_id);
                PostMessageW(existing, WM_APP_SHOW, 0, 0);
            }
            return 0;
        }
    }

    if (app->render_card_path[0]) {
        render_preview_and_exit(app);
    }
    if (app->qr_dump_path[0]) {
        dump_qr_and_exit(app);
    }

    load_speed_settings(app);
    if (!server_start(app)) {
        MessageBoxW(NULL, L"Cannot start the pairing server.", APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    app_log(app, "server started on port %d", app->pairing.port);

    if (!card_create(app)) {
        MessageBoxW(NULL, L"Cannot create the QR card window.", APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    app_log(app, "card window created");

    if (!app->no_tray) {
        if (!tray_init(app)) {
            MessageBoxW(NULL, L"Cannot add the notification-area icon.", APP_TITLE,
                        MB_OK | MB_ICONERROR);
            return 1;
        }
        app_log(app, "tray icon added");
    } else {
        /* Still need a message window for timers and posted updates. */
        WNDCLASSEXW window_class;
        memset(&window_class, 0, sizeof(window_class));
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = tray_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = TRAY_CLASS;
        RegisterClassExW(&window_class);
        app->tray = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, APP_TITLE, WS_POPUP,
                                    0, 0, 0, 0, NULL, NULL, instance, NULL);
        if (app->tray) {
            if (app->quit_at || app->state_file_path[0]) SetTimer(app->tray, TIMER_STATUS, 250, NULL);
        }
    }

    if (!app->no_tray || app->preview_ms > 0) {
        card_show(app);
    }

    app_log(app, "ready");
    {

        MSG message;
        while (GetMessageW(&message, NULL, 0, 0) > 0) {
            if (app->card_visible && IsDialogMessageW(app->card, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    app_log(app, "message loop finished");

    app->should_quit = true;
    if (app->tray) {
        KillTimer(app->tray, TIMER_STATUS);
    }
    card_hide(app);
    card_destroy(app);
    tray_remove(app);
    server_stop(app);
    /* Let connection threads release any keys or buttons still held down. */
    Sleep(200);

    if (single_instance) {
        ReleaseMutex(single_instance);
        CloseHandle(single_instance);
    }
    if (app->tray) {
        DestroyWindow(app->tray);
        app->tray = NULL;
    }
    WSACleanup();
    app_log(app, "exit");
    app_log_close(app);
    DeleteCriticalSection(&app->log_lock);
    DeleteCriticalSection(&app->lock);
    free(app);
    g_app = NULL;
    return 0;
}
