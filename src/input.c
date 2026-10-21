/*
 * Keyboard and mouse injection through Win32 SendInput.
 *
 * Only the virtual-key values referenced by the phone UI are accepted, which
 * keeps the wire protocol from becoming an arbitrary input injection API.
 */
#include "app.h"

typedef struct {
    const char *code;
    unsigned short vk;
    unsigned char extended;
} KeyMap;

static const KeyMap KEY_MAP[] = {
    {"Escape", 0x1B, 0},
    {"F1", 0x70, 0},
    {"F2", 0x71, 0},
    {"F3", 0x72, 0},
    {"F4", 0x73, 0},
    {"F5", 0x74, 0},
    {"F6", 0x75, 0},
    {"F7", 0x76, 0},
    {"F8", 0x77, 0},
    {"F9", 0x78, 0},
    {"F10", 0x79, 0},
    {"F11", 0x7A, 0},
    {"F12", 0x7B, 0},
    {"PrintScreen", 0x2C, 1},
    {"ScrollLock", 0x91, 0},
    {"Pause", 0x13, 0},
    {"Backquote", 0xC0, 0},
    {"Digit1", 0x31, 0},
    {"Digit2", 0x32, 0},
    {"Digit3", 0x33, 0},
    {"Digit4", 0x34, 0},
    {"Digit5", 0x35, 0},
    {"Digit6", 0x36, 0},
    {"Digit7", 0x37, 0},
    {"Digit8", 0x38, 0},
    {"Digit9", 0x39, 0},
    {"Digit0", 0x30, 0},
    {"Minus", 0xBD, 0},
    {"Equal", 0xBB, 0},
    {"Backspace", 0x08, 0},
    {"Tab", 0x09, 0},
    {"KeyQ", 0x51, 0},
    {"KeyW", 0x57, 0},
    {"KeyE", 0x45, 0},
    {"KeyR", 0x52, 0},
    {"KeyT", 0x54, 0},
    {"KeyY", 0x59, 0},
    {"KeyU", 0x55, 0},
    {"KeyI", 0x49, 0},
    {"KeyO", 0x4F, 0},
    {"KeyP", 0x50, 0},
    {"BracketLeft", 0xDB, 0},
    {"BracketRight", 0xDD, 0},
    {"Backslash", 0xDC, 0},
    {"CapsLock", 0x14, 0},
    {"KeyA", 0x41, 0},
    {"KeyS", 0x53, 0},
    {"KeyD", 0x44, 0},
    {"KeyF", 0x46, 0},
    {"KeyG", 0x47, 0},
    {"KeyH", 0x48, 0},
    {"KeyJ", 0x4A, 0},
    {"KeyK", 0x4B, 0},
    {"KeyL", 0x4C, 0},
    {"Semicolon", 0xBA, 0},
    {"Quote", 0xDE, 0},
    {"Enter", 0x0D, 0},
    {"ShiftLeft", 0xA0, 0},
    {"KeyZ", 0x5A, 0},
    {"KeyX", 0x58, 0},
    {"KeyC", 0x43, 0},
    {"KeyV", 0x56, 0},
    {"KeyB", 0x42, 0},
    {"KeyN", 0x4E, 0},
    {"KeyM", 0x4D, 0},
    {"Comma", 0xBC, 0},
    {"Period", 0xBE, 0},
    {"Slash", 0xBF, 0},
    {"ShiftRight", 0xA1, 0},
    {"ControlLeft", 0xA2, 0},
    {"MetaLeft", 0x5B, 1},
    {"AltLeft", 0xA4, 0},
    {"Space", 0x20, 0},
    {"AltRight", 0xA5, 1},
    {"MetaRight", 0x5C, 1},
    {"ContextMenu", 0x5D, 1},
    {"ControlRight", 0xA3, 1},
    {"Insert", 0x2D, 1},
    {"Home", 0x24, 1},
    {"PageUp", 0x21, 1},
    {"Delete", 0x2E, 1},
    {"End", 0x23, 1},
    {"PageDown", 0x22, 1},
    {"ArrowLeft", 0x25, 1},
    {"ArrowUp", 0x26, 1},
    {"ArrowDown", 0x28, 1},
    {"ArrowRight", 0x27, 1},
    {"NumLock", 0x90, 1},
    {"NumpadDivide", 0x6F, 1},
    {"NumpadMultiply", 0x6A, 0},
    {"NumpadSubtract", 0x6D, 0},
    {"Numpad7", 0x67, 0},
    {"Numpad8", 0x68, 0},
    {"Numpad9", 0x69, 0},
    {"NumpadAdd", 0x6B, 0},
    {"Numpad4", 0x64, 0},
    {"Numpad5", 0x65, 0},
    {"Numpad6", 0x66, 0},
    {"Numpad1", 0x61, 0},
    {"Numpad2", 0x62, 0},
    {"Numpad3", 0x63, 0},
    {"NumpadEnter", 0x0D, 1},
    {"Numpad0", 0x60, 0},
    {"NumpadDecimal", 0x6E, 0},
};

static const KeyMap *key_map_find(const char *code)
{
    if (!code) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(KEY_MAP) / sizeof(KEY_MAP[0]); i++) {
        if (strcmp(KEY_MAP[i].code, code) == 0) {
            return &KEY_MAP[i];
        }
    }
    return NULL;
}

static bool send_inputs(INPUT *inputs, UINT count)
{
    if (count == 0) {
        return true;
    }
    return SendInput(count, inputs, (int)sizeof(INPUT)) == count;
}

static INPUT make_mouse_input(int dx, int dy, DWORD data, DWORD flags)
{
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = (LONG)dx;
    input.mi.dy = (LONG)dy;
    input.mi.mouseData = data;
    input.mi.dwFlags = flags;
    return input;
}

static INPUT make_key_input(unsigned short vk, bool up, bool extended)
{
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = 0;
    DWORD flags = 0;
    if (up) {
        flags |= KEYEVENTF_KEYUP;
    }
    if (extended) {
        flags |= KEYEVENTF_EXTENDEDKEY;
    }
    input.ki.dwFlags = flags;
    return input;
}

bool input_send_relative_move(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return true;
    }
    INPUT input = make_mouse_input(dx, dy, 0, MOUSEEVENTF_MOVE);
    return send_inputs(&input, 1);
}

static bool click_button(int button)
{
    DWORD down;
    DWORD up;
    switch (button) {
    case 0:
        down = MOUSEEVENTF_LEFTDOWN;
        up = MOUSEEVENTF_LEFTUP;
        break;
    case 1:
        down = MOUSEEVENTF_RIGHTDOWN;
        up = MOUSEEVENTF_RIGHTUP;
        break;
    case 2:
        down = MOUSEEVENTF_MIDDLEDOWN;
        up = MOUSEEVENTF_MIDDLEUP;
        break;
    default:
        return false;
    }
    INPUT inputs[2];
    inputs[0] = make_mouse_input(0, 0, 0, down);
    inputs[1] = make_mouse_input(0, 0, 0, up);
    return send_inputs(inputs, 2);
}

static bool button_state(int button, bool down)
{
    DWORD flags;
    switch (button) {
    case 0:
        flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 1:
        flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case 2:
        flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    default:
        return false;
    }
    INPUT input = make_mouse_input(0, 0, 0, flags);
    return send_inputs(&input, 1);
}

static bool scroll_wheel(int dx, int dy)
{
    bool ok = true;
    if (dy != 0) {
        INPUT input = make_mouse_input(0, 0, (DWORD)dy, MOUSEEVENTF_WHEEL);
        ok = send_inputs(&input, 1) && ok;
    }
    if (dx != 0) {
        INPUT input = make_mouse_input(0, 0, (DWORD)dx, MOUSEEVENTF_HWHEEL);
        ok = send_inputs(&input, 1) && ok;
    }
    return ok;
}

static bool send_key(unsigned short vk, bool up, bool extended)
{
    INPUT input = make_key_input(vk, up, extended);
    return send_inputs(&input, 1);
}

static int button_from_name(const char *name)
{
    if (strcmp(name, "left") == 0) {
        return 0;
    }
    if (strcmp(name, "right") == 0) {
        return 1;
    }
    if (strcmp(name, "middle") == 0) {
        return 2;
    }
    return -1;
}

static int clamp_int(int value, int limit)
{
    if (value > limit) {
        return limit;
    }
    if (value < -limit) {
        return -limit;
    }
    return value;
}

static int scale_delta(int value, int percent, int *remainder, int limit)
{
    int scaled = clamp_int(value, limit) * percent + *remainder;
    *remainder = scaled % 100;
    return clamp_int(scaled / 100, limit);
}

/* ------------------------------------------------------------------ */
/* Session                                                            */
/* ------------------------------------------------------------------ */

void input_session_init(InputSession *session, bool dry_run)
{
    memset(session, 0, sizeof(*session));
    session->dry_run = dry_run;
    session->mouse_speed_percent = 100;
    session->scroll_speed_percent = 100;
    session->motion = motion_create(dry_run);
}

void input_session_set_speed(InputSession *session, int mouse_percent, int scroll_percent)
{
    if (mouse_percent < 50 || mouse_percent > 300) mouse_percent = 100;
    if (scroll_percent < 50 || scroll_percent > 300) scroll_percent = 100;
    if (session->mouse_speed_percent != mouse_percent) {
        session->mouse_speed_percent = mouse_percent;
        session->move_remainder_x = session->move_remainder_y = 0;
    }
    if (session->scroll_speed_percent != scroll_percent) {
        session->scroll_speed_percent = scroll_percent;
        session->wheel_remainder_x = session->wheel_remainder_y = 0;
    }
}

void input_release_all(InputSession *session)
{
    motion_cancel(session->motion);
    session->move_remainder_x = session->move_remainder_y = 0;
    session->wheel_remainder_x = session->wheel_remainder_y = 0;

    for (int i = session->key_count - 1; i >= 0; i--) {
        if (!session->dry_run) {
            send_key(session->keys[i].vk, true, session->keys[i].extended);
        }
    }
    session->key_count = 0;

    for (int bit = 0; bit < 3; bit++) {
        unsigned int mask = 1u << bit;
        if ((session->buttons & mask) &&
            (session->dry_run || button_state(bit, false))) {
            session->buttons &= ~mask;
        }
    }
}

void input_session_dispose(InputSession *session)
{
    if (!session) {
        return;
    }
    input_release_all(session);
    motion_destroy(session->motion);
    session->motion = NULL;
}

bool input_apply_json(InputSession *session, const char *json)
{
    if (!session || !session->motion) {
        return false;
    }

    char tag[24];
    if (!json_string(json, "t", tag, sizeof(tag))) {
        return false;
    }

    bool is_move = strcmp(tag, "move") == 0;
    bool is_release = strcmp(tag, "release") == 0;

    /* Pending movement must land before anything that changes the interaction. */
    if (!is_move && !is_release) {
        if (!motion_flush(session->motion)) {
            input_release_all(session);
            return false;
        }
    }

    if (is_move) {
        int dx = 0;
        int dy = 0;
        if (!json_int(json, "dx", &dx) || !json_int(json, "dy", &dy)) {
            return false;
        }
        dx = scale_delta(dx, session->mouse_speed_percent, &session->move_remainder_x, 600);
        dy = scale_delta(dy, session->mouse_speed_percent, &session->move_remainder_y, 600);
        return motion_push(session->motion, dx, dy);
    }

    if (is_release) {
        input_release_all(session);
        return session->buttons == 0;
    }

    if (strcmp(tag, "click") == 0) {
        char name[16];
        if (!json_string(json, "button", name, sizeof(name))) {
            return false;
        }
        int button = button_from_name(name);
        if (button < 0) {
            return false;
        }
        if (session->dry_run) {
            return true;
        }
        return click_button(button);
    }

    if (strcmp(tag, "button") == 0) {
        char name[16];
        bool down = false;
        if (!json_string(json, "button", name, sizeof(name)) ||
            !json_bool(json, "down", &down)) {
            return false;
        }
        int button = button_from_name(name);
        if (button < 0) {
            return false;
        }
        unsigned int mask = 1u << button;
        bool held = (session->buttons & mask) != 0;
        if (held == down) {
            return true;
        }
        if (!session->dry_run && !button_state(button, down)) {
            return false;
        }
        if (down) {
            session->buttons |= mask;
        } else {
            session->buttons &= ~mask;
        }
        return true;
    }

    if (strcmp(tag, "wheel") == 0) {
        int dx = 0;
        int dy = 0;
        if (!json_int(json, "dx", &dx)) {
            dx = 0;
        }
        if (!json_int(json, "dy", &dy)) {
            dy = 0;
        }
        if (session->dry_run) {
            return true;
        }
        dx = scale_delta(dx, session->scroll_speed_percent, &session->wheel_remainder_x, 4800);
        dy = scale_delta(dy, session->scroll_speed_percent, &session->wheel_remainder_y, 4800);
        return scroll_wheel(dx, dy);
    }

    if (strcmp(tag, "key") == 0) {
        char action[16];
        char code[32];
        if (!json_string(json, "action", action, sizeof(action)) ||
            !json_string(json, "code", code, sizeof(code))) {
            return false;
        }
        const KeyMap *mapped = key_map_find(code);
        if (!mapped) {
            return false;
        }

        if (strcmp(action, "down") == 0) {
            for (int i = 0; i < session->key_count; i++) {
                if (strcmp(session->keys[i].code, code) == 0) {
                    return true;
                }
            }
            if (session->key_count >= MAX_HELD_KEYS) {
                return false;
            }
            HeldKey *held = &session->keys[session->key_count++];
            _snprintf_s(held->code, sizeof(held->code), _TRUNCATE, "%s", code);
            held->vk = mapped->vk;
            held->extended = mapped->extended != 0;
            if (session->dry_run) {
                return true;
            }
            return send_key(mapped->vk, false, mapped->extended != 0);
        }

        if (strcmp(action, "up") == 0) {
            for (int i = 0; i < session->key_count; i++) {
                if (strcmp(session->keys[i].code, code) == 0) {
                    HeldKey held = session->keys[i];
                    session->keys[i] = session->keys[session->key_count - 1];
                    session->key_count--;
                    if (session->dry_run) {
                        return true;
                    }
                    return send_key(held.vk, true, held.extended);
                }
            }
            return true;
        }
        return false;
    }

    return false;
}
