/*
 * Pairing server: a tiny HTTP/1.1 + WebSocket endpoint.
 *
 * GET /c/<token>              phone controller page (token gated)
 * GET /assets/controller.css  phone stylesheet
 * GET /assets/controller.js   phone controller script
 * GET /ws/<token>             WebSocket upgrade for input events
 */
#include "app.h"
#include "web_assets.h"

#define MAX_CONNECTIONS 16
#define REQUEST_MAX 16384
#define FRAME_MAX 8192
#define PING_INTERVAL_MS 15000
#define PONG_TIMEOUT_MS 45000

typedef struct {
    App *app;
    SOCKET socket;
    uint64_t id;
    uint64_t last_pong_ms;
    uint64_t last_ping_ms;
    size_t buffer_len;
    unsigned char buffer[REQUEST_MAX];
    unsigned char payload[FRAME_MAX + 64];
    InputSession session;
    CRITICAL_SECTION input_lock;
    volatile LONG superseded;
    volatile LONG refs;
} Connection;

static volatile LONG g_next_id = 1;
static Connection *g_active_connection = NULL;

static void connection_add_ref(Connection *connection)
{
    InterlockedIncrement(&connection->refs);
}

static void connection_release(Connection *connection)
{
    if (InterlockedDecrement(&connection->refs) == 0) {
        DeleteCriticalSection(&connection->input_lock);
        free(connection);
    }
}

static void connection_dispose_input(Connection *connection)
{
    EnterCriticalSection(&connection->input_lock);
    input_session_dispose(&connection->session);
    LeaveCriticalSection(&connection->input_lock);
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                     */
/* ------------------------------------------------------------------ */

static bool send_all(SOCKET socket_handle, const void *data, size_t length)
{
    const char *cursor = (const char *)data;
    size_t sent = 0;
    while (sent < length) {
        int chunk = (int)((length - sent) > 32768 ? 32768 : (length - sent));
        int written = send(socket_handle, cursor + sent, chunk, 0);
        if (written <= 0) {
            return false;
        }
        sent += (size_t)written;
    }
    return true;
}

static bool token_equal(const char *left, const char *right)
{
    if (!left || !right) {
        return false;
    }
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    if (left_len != right_len) {
        return false;
    }
    unsigned char difference = 0;
    for (size_t i = 0; i < left_len; i++) {
        difference |= (unsigned char)(left[i] ^ right[i]);
    }
    return difference == 0;
}

static void write_status(SOCKET socket_handle, int status, const char *reason,
                         const char *content_type, const void *body, size_t body_len,
                         bool head_only, const char *extra_headers)
{
    char header[768];
    _snprintf_s(header, sizeof(header), _TRUNCATE,
                "HTTP/1.1 %d %s\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "X-Content-Type-Options: nosniff\r\n"
                "Cache-Control: no-store\r\n"
                "%s"
                "\r\n",
                status, reason, content_type, body_len,
                extra_headers ? extra_headers : "");
    if (send_all(socket_handle, header, strlen(header)) && !head_only && body && body_len > 0) {
        send_all(socket_handle, body, body_len);
    }
}

static const char *find_header(const char *request, size_t request_len, const char *name)
{
    size_t name_len = strlen(name);
    size_t position = 0;

    /* Skip the request line. */
    while (position + 1 < request_len) {
        if (request[position] == '\r' && request[position + 1] == '\n') {
            position += 2;
            break;
        }
        position++;
    }

    while (position < request_len) {
        size_t line_start = position;
        while (position + 1 < request_len) {
            if (request[position] == '\r' && request[position + 1] == '\n') {
                break;
            }
            position++;
        }
        size_t line_len = position - line_start;
        if (line_len == 0) {
            return NULL;
        }
        if (line_len > name_len && _strnicmp(request + line_start, name, name_len) == 0 &&
            request[line_start + name_len] == ':') {
            const char *value = request + line_start + name_len + 1;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            return value;
        }
        position += 2;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Active connection bookkeeping                                      */
/* ------------------------------------------------------------------ */

static bool install_active(App *app, Connection *connection)
{
    Connection *previous = NULL;
    EnterCriticalSection(&app->lock);
    if (InterlockedCompareExchange(&app->server_stop, 0, 0)) {
        LeaveCriticalSection(&app->lock);
        return false;
    }
    previous = g_active_connection;
    if (previous) {
        connection_add_ref(previous);
    }
    connection_add_ref(connection); /* active reference */
    g_active_connection = connection;
    app->active = connection->socket;
    app->active_id = connection->id;
    InterlockedExchange(&app->connected, 1);
    LeaveCriticalSection(&app->lock);

    if (previous) {
        /* Only one controller at a time; the previous one is dropped. */
        InterlockedExchange(&previous->superseded, 1);
        EnterCriticalSection(&previous->input_lock);
        input_release_all(&previous->session);
        LeaveCriticalSection(&previous->input_lock);
        shutdown(previous->socket, SD_BOTH);
        connection_release(previous); /* drop temporary reference */
        connection_release(previous); /* drop previous active reference */
    }
    PostMessageW(app->tray, WM_APP_STATUS, 1, 0);
    return true;
}

static void remove_active(App *app, Connection *connection)
{
    bool cleared = false;
    EnterCriticalSection(&app->lock);
    if (g_active_connection == connection) {
        g_active_connection = NULL;
        app->active = INVALID_SOCKET;
        app->active_id = 0;
        InterlockedExchange(&app->connected, 0);
        cleared = true;
    }
    LeaveCriticalSection(&app->lock);

    if (cleared) {
        connection_release(connection); /* drop active reference */
        PostMessageW(app->tray, WM_APP_STATUS, 0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* WebSocket                                                          */
/* ------------------------------------------------------------------ */

/* Returns 1 when a full frame is available, 0 when more data is needed and
 * -1 when the frame is malformed. */
static int parse_frame(const unsigned char *data, size_t length, int *opcode,
                       unsigned char *payload, size_t payload_capacity,
                       size_t *payload_length, size_t *consumed)
{
    if (length < 2) {
        return 0;
    }
    int fin = (data[0] & 0x80) != 0;
    int frame_opcode = data[0] & 0x0F;
    int masked = (data[1] & 0x80) != 0;
    uint64_t payload_len = data[1] & 0x7F;
    size_t offset = 2;

    if (payload_len == 126) {
        if (length < offset + 2) {
            return 0;
        }
        payload_len = ((uint64_t)data[offset] << 8) | data[offset + 1];
        offset += 2;
    } else if (payload_len == 127) {
        if (length < offset + 8) {
            return 0;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | data[offset + i];
        }
        offset += 8;
    }

    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (length < offset + 4) {
            return 0;
        }
        memcpy(mask, data + offset, 4);
        offset += 4;
    }

    if (payload_len > payload_capacity || payload_len > (uint64_t)REQUEST_MAX) {
        return -1;
    }
    if (length < offset + (size_t)payload_len) {
        return 0;
    }

    for (size_t i = 0; i < (size_t)payload_len; i++) {
        payload[i] = masked ? (unsigned char)(data[offset + i] ^ mask[i % 4]) : data[offset + i];
    }
    *opcode = frame_opcode;
    *payload_length = (size_t)payload_len;
    *consumed = offset + (size_t)payload_len;
    (void)fin;
    return 1;
}

static bool ws_send_frame(SOCKET socket_handle, int opcode, const void *data, size_t length)
{
    unsigned char header[10];
    size_t header_len = 2;
    header[0] = (unsigned char)(0x80 | (opcode & 0x0F));
    if (length < 126) {
        header[1] = (unsigned char)length;
    } else if (length <= 0xFFFF) {
        header[1] = 126;
        header[2] = (unsigned char)(length >> 8);
        header[3] = (unsigned char)(length);
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (unsigned char)((uint64_t)length >> (8 * (7 - i)));
        }
        header_len = 10;
    }
    if (!send_all(socket_handle, header, header_len)) {
        return false;
    }
    return length == 0 || send_all(socket_handle, data, length);
}

static bool ws_handshake(Connection *connection)
{
    const char *key = find_header((const char *)connection->buffer, connection->buffer_len,
                                  "Sec-WebSocket-Key");
    if (!key) {
        return false;
    }

    char value[64];
    size_t used = 0;
    while (key[used] && key[used] != '\r' && key[used] != '\n' && used + 1 < sizeof(value)) {
        value[used] = key[used];
        used++;
    }
    while (used > 0 && (value[used - 1] == ' ' || value[used - 1] == '\t')) {
        used--;
    }
    value[used] = 0;
    if (used == 0) {
        return false;
    }

    char combined[128];
    _snprintf_s(combined, sizeof(combined), _TRUNCATE,
                "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", value);

    unsigned char digest[20];
    sha1_digest((const unsigned char *)combined, strlen(combined), digest);

    char accept[64];
    base64_encode(digest, sizeof(digest), accept, sizeof(accept));

    char response[256];
    _snprintf_s(response, sizeof(response), _TRUNCATE,
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n"
                "\r\n",
                accept);
    if (!send_all(connection->socket, response, strlen(response))) {
        return false;
    }

    /* Anything the client sent after the handshake belongs to the first frame. */
    const char *terminator = NULL;
    for (size_t i = 0; i + 3 < connection->buffer_len; i++) {
        if (memcmp(connection->buffer + i, "\r\n\r\n", 4) == 0) {
            terminator = (const char *)connection->buffer + i + 4;
            break;
        }
    }
    if (!terminator) {
        return false;
    }
    size_t header_len = (size_t)(terminator - (const char *)connection->buffer);
    size_t remaining = connection->buffer_len - header_len;
    if (remaining > 0) {
        memmove(connection->buffer, connection->buffer + header_len, remaining);
    }
    connection->buffer_len = remaining;
    return true;
}

static void ws_session(Connection *connection)
{
    App *app = connection->app;
    SOCKET socket_handle = connection->socket;

    int timeout = 250;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    connection->last_pong_ms = now_ms();
    connection->last_ping_ms = now_ms();

    if (!install_active(app, connection)) {
        connection_dispose_input(connection);
        return;
    }
    app_log(app, "ws connected id=%llu", (unsigned long long)connection->id);

    bool running = true;
    while (running && !InterlockedCompareExchange(&app->server_stop, 0, 0)) {
        for (;;) {
            if (InterlockedCompareExchange(&connection->superseded, 0, 0)) {
                running = false;
                break;
            }
            int opcode = 0;
            size_t payload_len = 0;
            size_t consumed = 0;
            int parsed = parse_frame(connection->buffer, connection->buffer_len, &opcode,
                                     connection->payload, sizeof(connection->payload) - 1,
                                     &payload_len, &consumed);
            if (parsed == 0) {
                break;
            }
            if (parsed < 0) {
                running = false;
                break;
            }

            if (payload_len < sizeof(connection->payload)) {
                connection->payload[payload_len] = 0;
            }

            switch (opcode) {
            case 0x1: /* text */
                if (payload_len > 0 && payload_len <= FRAME_MAX) {
                    EnterCriticalSection(&connection->input_lock);
                    if (InterlockedCompareExchange(&connection->superseded, 0, 0)) {
                        LeaveCriticalSection(&connection->input_lock);
                        running = false;
                        break;
                    }
                    input_session_set_speed(&connection->session,
                        (int)InterlockedCompareExchange(&app->mouse_speed_percent, 0, 0),
                        (int)InterlockedCompareExchange(&app->scroll_speed_percent, 0, 0));
                    if (!input_apply_json(&connection->session, (const char *)connection->payload)) {
                        input_release_all(&connection->session);
                    }
                    LeaveCriticalSection(&connection->input_lock);
                }
                break;
            case 0x8: /* close */
                ws_send_frame(socket_handle, 0x8, NULL, 0);
                running = false;
                break;
            case 0x9: /* ping */
                ws_send_frame(socket_handle, 0xA, connection->payload, payload_len);
                break;
            case 0xA: /* pong */
                connection->last_pong_ms = now_ms();
                break;
            default:
                break;
            }

            if (consumed >= connection->buffer_len) {
                connection->buffer_len = 0;
            } else {
                memmove(connection->buffer, connection->buffer + consumed,
                        connection->buffer_len - consumed);
                connection->buffer_len -= consumed;
            }

            if (!running) {
                break;
            }
        }

        if (!running) {
            break;
        }

        int received = recv(socket_handle, (char *)connection->buffer + connection->buffer_len,
                            (int)(sizeof(connection->buffer) - connection->buffer_len), 0);
        if (received > 0) {
            connection->buffer_len += (size_t)received;
            if (connection->buffer_len >= sizeof(connection->buffer)) {
                break;
            }
            continue;
        }
        if (received == 0) {
            break;
        }

        int error = WSAGetLastError();
        if (error != WSAETIMEDOUT && error != WSAEWOULDBLOCK) {
            break;
        }

        uint64_t now = now_ms();
        if (now - connection->last_ping_ms >= PING_INTERVAL_MS) {
            if (!ws_send_frame(socket_handle, 0x9, NULL, 0)) {
                break;
            }
            connection->last_ping_ms = now;
        }
        if (now - connection->last_pong_ms >= PONG_TIMEOUT_MS) {
            break;
        }
    }

    app_log(app, "ws closed id=%llu", (unsigned long long)connection->id);
    connection_dispose_input(connection);
    remove_active(app, connection);
}

/* ------------------------------------------------------------------ */
/* HTTP                                                               */
/* ------------------------------------------------------------------ */

static void handle_http(Connection *connection, const char *method,
                        const char *path, bool head_only)
{
    App *app = connection->app;
    SOCKET socket_handle = connection->socket;

    char token[TOKEN_HEX_CHARS + 1];
    EnterCriticalSection(&app->lock);
    memcpy(token, app->pairing.token, sizeof(token));
    LeaveCriticalSection(&app->lock);

    char page_path[TOKEN_HEX_CHARS + 8];
    _snprintf_s(page_path, sizeof(page_path), _TRUNCATE, "/c/%s", token);

    const char *static_headers =
        "Content-Security-Policy: default-src 'self'; img-src 'self' data:; "
        "style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self' ws:; "
        "base-uri 'none'; frame-ancestors 'none'; form-action 'none'\r\n"
        "Referrer-Policy: no-referrer\r\n";

    if (strcmp(path, page_path) == 0) {
        write_status(socket_handle, 200, "OK", "text/html; charset=utf-8",
                     web_html_bytes, web_html_len, head_only, static_headers);
        return;
    }
    if (strcmp(path, "/assets/controller.css") == 0) {
        write_status(socket_handle, 200, "OK", "text/css; charset=utf-8",
                     web_css_bytes, web_css_len, head_only, NULL);
        return;
    }
    if (strcmp(path, "/assets/controller.js") == 0) {
        write_status(socket_handle, 200, "OK", "text/javascript; charset=utf-8",
                     web_js_bytes, web_js_len, head_only, NULL);
        return;
    }

    app_log(app, "http %s %s -> 404", method, path);
    write_status(socket_handle, 404, "Not Found", "text/plain; charset=utf-8",
                 "Not Found", 9, head_only, NULL);
}

static bool has_request_end(const unsigned char *data, size_t length)
{
    for (size_t i = 0; i + 3 < length; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            return true;
        }
    }
    return false;
}

static DWORD WINAPI connection_thread(LPVOID parameter)
{
    Connection *connection = (Connection *)parameter;
    App *app = connection->app;
    SOCKET socket_handle = connection->socket;

    int timeout = 15000;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    while (connection->buffer_len < sizeof(connection->buffer) - 1) {
        int received = recv(socket_handle, (char *)connection->buffer + connection->buffer_len,
                            (int)(sizeof(connection->buffer) - 1 - connection->buffer_len), 0);
        if (received <= 0) {
            break;
        }
        connection->buffer_len += (size_t)received;
        if (has_request_end(connection->buffer, connection->buffer_len)) {
            break;
        }
    }
    connection->buffer[connection->buffer_len] = 0;

    if (!has_request_end(connection->buffer, connection->buffer_len)) {
        closesocket(socket_handle);
        connection_dispose_input(connection);
        connection_release(connection);
        return 0;
    }

    /* Parse "METHOD TARGET HTTP/1.1" without touching the header block. */
    char request_line[512];
    size_t line_len = 0;
    while (line_len < connection->buffer_len && line_len + 1 < sizeof(request_line)) {
        if (connection->buffer[line_len] == '\r' && connection->buffer[line_len + 1] == '\n') {
            break;
        }
        request_line[line_len] = (char)connection->buffer[line_len];
        line_len++;
    }
    request_line[line_len] = 0;

    char method[8];
    char target[400];
    method[0] = 0;
    target[0] = 0;
    if (sscanf_s(request_line, "%7s %399s", method, (unsigned)sizeof(method),
                 target, (unsigned)sizeof(target)) != 2) {
        closesocket(socket_handle);
        connection_dispose_input(connection);
        connection_release(connection);
        return 0;
    }

    bool head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head_only) {
        write_status(socket_handle, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                     "Method Not Allowed", 18, false, NULL);
        closesocket(socket_handle);
        connection_dispose_input(connection);
        connection_release(connection);
        return 0;
    }

    char path[400];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s", target);
    char *query = strchr(path, '?');
    if (query) {
        *query = 0;
    }

    if (strncmp(path, "/ws/", 4) == 0) {
        char token[TOKEN_HEX_CHARS + 1];
        EnterCriticalSection(&app->lock);
        memcpy(token, app->pairing.token, sizeof(token));
        LeaveCriticalSection(&app->lock);

        if (!token_equal(path + 4, token)) {
            app_log(app, "ws rejected");
            write_status(socket_handle, 401, "Unauthorized", "text/plain; charset=utf-8",
                         "Unauthorized", 12, false, NULL);
        } else if (!head_only && ws_handshake(connection)) {
            ws_session(connection);
            closesocket(socket_handle);
            connection_release(connection);
            return 0;
        } else {
            write_status(socket_handle, 400, "Bad Request", "text/plain; charset=utf-8",
                         "Bad Request", 11, false, NULL);
        }
        closesocket(socket_handle);
        connection_dispose_input(connection);
        connection_release(connection);
        return 0;
    }

    handle_http(connection, method, path, head_only);

    closesocket(socket_handle);
    connection_dispose_input(connection);
    connection_release(connection);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Listener                                                           */
/* ------------------------------------------------------------------ */

static DWORD WINAPI listener_thread(LPVOID parameter)
{
    App *app = (App *)parameter;

    for (;;) {
        if (InterlockedCompareExchange(&app->server_stop, 0, 0)) {
            break;
        }
        SOCKET client = accept(app->listener, NULL, NULL);
        if (client == INVALID_SOCKET) {
            break;
        }

        Connection *connection = (Connection *)calloc(1, sizeof(Connection));
        if (!connection) {
            closesocket(client);
            continue;
        }
        connection->app = app;
        connection->socket = client;
        connection->id = (uint64_t)InterlockedIncrement(&g_next_id);
        InitializeCriticalSection(&connection->input_lock);
        connection->refs = 1; /* connection thread owns the initial reference */

        input_session_init(&connection->session, app->dry_run);

        HANDLE thread = CreateThread(NULL, 0, connection_thread, connection, 0, NULL);
        if (!thread) {
            connection_dispose_input(connection);
            closesocket(client);
            connection_release(connection);
            continue;
        }
        CloseHandle(thread);
    }
    return 0;
}

bool server_start(App *app)
{
    app->listener = INVALID_SOCKET;
    app->active = INVALID_SOCKET;
    app->active_id = 0;
    g_active_connection = NULL;

    char address[64];
    if (!net_local_ipv4(address, sizeof(address))) {
        app_log(app, "no routable IPv4 address, using loopback");
    }

    if (app->forced_token[0]) {
        memcpy(app->pairing.token, app->forced_token, sizeof(app->pairing.token));
    } else {
        make_token(app->pairing.token);
    }
    _snprintf_s(app->pairing.address, sizeof(app->pairing.address), _TRUNCATE, "%s", address);
    app->pairing.port = app->forced_port;

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        app_log(app, "socket() failed: %d", WSAGetLastError());
        return false;
    }

    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));

    struct sockaddr_in bind_address;
    memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_address.sin_port = htons((unsigned short)(app->forced_port > 0 ? app->forced_port : 0));

    if (bind(listener, (const struct sockaddr *)&bind_address, sizeof(bind_address)) != 0) {
        app_log(app, "bind failed: %d", WSAGetLastError());
        closesocket(listener);
        return false;
    }

    int address_len = (int)sizeof(bind_address);
    if (getsockname(listener, (struct sockaddr *)&bind_address, &address_len) != 0) {
        closesocket(listener);
        return false;
    }
    app->pairing.port = (int)ntohs(bind_address.sin_port);

    if (listen(listener, 8) != 0) {
        app_log(app, "listen failed: %d", WSAGetLastError());
        closesocket(listener);
        return false;
    }

    app->listener = listener;

    HANDLE thread = CreateThread(NULL, 0, listener_thread, app, 0, NULL);
    if (!thread) {
        closesocket(listener);
        app->listener = INVALID_SOCKET;
        return false;
    }
    CloseHandle(thread);

    app_log(app, "server listening on %s:%d token=%s", app->pairing.address, app->pairing.port,
            app->pairing.token);
    return true;
}

void server_stop(App *app)
{
    InterlockedExchange(&app->server_stop, 1);

    if (app->listener != INVALID_SOCKET) {
        SOCKET listener = app->listener;
        app->listener = INVALID_SOCKET;
        closesocket(listener);
    }

    Connection *active_connection = NULL;
    EnterCriticalSection(&app->lock);
    active_connection = g_active_connection;
    if (active_connection) {
        connection_add_ref(active_connection);
        g_active_connection = NULL;
    }
    SOCKET active = app->active;
    app->active = INVALID_SOCKET;
    app->active_id = 0;
    InterlockedExchange(&app->connected, 0);
    LeaveCriticalSection(&app->lock);

    if (active_connection) {
        InterlockedExchange(&active_connection->superseded, 1);
        EnterCriticalSection(&active_connection->input_lock);
        input_release_all(&active_connection->session);
        LeaveCriticalSection(&active_connection->input_lock);
    }
    if (active != INVALID_SOCKET) {
        shutdown(active, SD_BOTH);
    }
    if (active_connection) {
        connection_release(active_connection); /* drop active reference */
    }
    app_log(app, "server stopped");
}

void server_rotate_token(App *app)
{
    char token[TOKEN_HEX_CHARS + 1];
    make_token(token);

    Connection *active_connection = NULL;
    EnterCriticalSection(&app->lock);
    memcpy(app->pairing.token, token, sizeof(token));
    active_connection = g_active_connection;
    if (active_connection) {
        connection_add_ref(active_connection);
        g_active_connection = NULL;
    }
    SOCKET active = app->active;
    app->active = INVALID_SOCKET;
    app->active_id = 0;
    InterlockedExchange(&app->connected, 0);
    LeaveCriticalSection(&app->lock);

    if (active_connection) {
        InterlockedExchange(&active_connection->superseded, 1);
        EnterCriticalSection(&active_connection->input_lock);
        input_release_all(&active_connection->session);
        LeaveCriticalSection(&active_connection->input_lock);
    }
    if (active != INVALID_SOCKET) {
        shutdown(active, SD_BOTH);
    }
    if (active_connection) {
        connection_release(active_connection); /* drop active reference */
    }
    app_log(app, "token rotated: %s", token);
    if (app->tray) {
        PostMessageW(app->tray, WM_APP_REFRESH, 0, 0);
    }
}

bool server_connected(App *app)
{
    return InterlockedCompareExchange(&app->connected, 0, 0) != 0;
}
