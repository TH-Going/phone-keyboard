/*
 * Compact main window: GDI painting with native buttons and trackbars.
 * QR modules use integer pixels; the window follows per-monitor DPI.
 */
#include "app.h"
#include "qrcodegen.h"
#include <commctrl.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")

#define CARD_WIDTH_PX 282
#define CARD_HEIGHT_PX 338
#define CARD_QR_TARGET_PX 230
#define CARD_QUIET_MODULES 4
#define CARD_RADIUS_PX 10

#define CARD_ID_SETTINGS 1001
#define CARD_ID_CLOSE 1002
#define CARD_ID_BACK 1003
#define CARD_ID_DONE 1004
#define CARD_ID_MOUSE 1005
#define CARD_ID_SCROLL 1006

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    void *bits;
    int width;
    int height;
} CardSurface;

typedef struct {
    int dpi;
    int win_w;
    int win_h;
    int card_w;
    int card_h;
    int margin;
    int radius;
    int module_px;
    int qr_modules;   /* modules without the quiet zone */
    int qr_px;        /* side length including the quiet zone */
    int qr_x;
    int qr_y;
    bool connected;
} CardLayout;

static CardSurface g_surface;
static CardLayout g_layout;
static struct {
    HWND settings, close, back, done, mouse, scroll;
    HFONT font;
    bool positioned;
    bool mouse_dirty, scroll_dirty;
} g_controls;

/* ------------------------------------------------------------------ */
/* Small math helpers                                                  */
/* ------------------------------------------------------------------ */

static int scale_px(int base_px, int dpi)
{
    return (base_px * dpi + 48) / 96;
}

/* Signed distance to a rounded rectangle; negative inside. */
static double rounded_rect_sdf(double px, double py,
                               double left, double top, double right, double bottom,
                               double radius)
{
    double center_x = (left + right) * 0.5;
    double center_y = (top + bottom) * 0.5;
    double half_x = (right - left) * 0.5 - radius;
    double half_y = (bottom - top) * 0.5 - radius;
    double dx = fabs(px - center_x) - half_x;
    double dy = fabs(py - center_y) - half_y;
    double outside_x = dx > 0.0 ? dx : 0.0;
    double outside_y = dy > 0.0 ? dy : 0.0;
    double outside = sqrt(outside_x * outside_x + outside_y * outside_y);
    double inside = dx > dy ? dx : dy;
    if (inside > 0.0) {
        inside = 0.0;
    }
    return outside + inside - radius;
}

static void store_pixel(unsigned char *pixels, int width, int x, int y,
                        int red, int green, int blue, int alpha)
{
    if (alpha <= 0) {
        return;
    }
    if (alpha > 255) {
        alpha = 255;
    }
    /* Keep premultiplied BGRA for the off-screen snapshot compositor. */
    unsigned char *pixel = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
    pixel[0] = (unsigned char)((blue * alpha + 127) / 255);
    pixel[1] = (unsigned char)((green * alpha + 127) / 255);
    pixel[2] = (unsigned char)((red * alpha + 127) / 255);
    pixel[3] = (unsigned char)alpha;
}

/* ------------------------------------------------------------------ */
/* Layout                                                             */
/* ------------------------------------------------------------------ */

static UINT monitor_dpi(RECT reference)
{
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);

    static GetDpiForMonitorFn get_dpi_for_monitor;
    static bool resolved;

    if (!resolved) {
        resolved = true;
        HMODULE library = LoadLibraryW(L"shcore.dll");
        if (library) {
            get_dpi_for_monitor =
                (GetDpiForMonitorFn)(void *)GetProcAddress(library, "GetDpiForMonitor");
        }
    }

    UINT dpi_x = 96;
    UINT dpi_y = 96;
    HMONITOR monitor = MonitorFromRect(&reference, MONITOR_DEFAULTTONEAREST);
    if (get_dpi_for_monitor && SUCCEEDED(get_dpi_for_monitor(monitor, 0, &dpi_x, &dpi_y))) {
        return dpi_x;
    }
    return 96;
}

static bool build_layout(App *app, int dpi, CardLayout *layout)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];

    memset(layout, 0, sizeof(*layout));
    layout->dpi = dpi > 0 ? dpi : 96;

    char url[PAIRING_URL_MAX];
    pairing_url(app, url, sizeof(url));
    if (!url[0]) {
        return false;
    }

    if (!qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        return false;
    }

    layout->qr_modules = qrcodegen_getSize(qr);
    int total_modules = layout->qr_modules + CARD_QUIET_MODULES * 2;
    int target = scale_px(CARD_QR_TARGET_PX, layout->dpi);
    layout->module_px = target / total_modules;
    if (layout->module_px < 1) {
        layout->module_px = 1;
    }
    layout->qr_px = layout->module_px * total_modules;

    layout->radius = scale_px(CARD_RADIUS_PX, layout->dpi);
    layout->card_w = layout->win_w = scale_px(CARD_WIDTH_PX, layout->dpi);
    layout->card_h = layout->win_h = scale_px(CARD_HEIGHT_PX, layout->dpi);
    layout->qr_x = (layout->win_w - layout->qr_px) / 2;
    layout->qr_y = scale_px(53, layout->dpi) + (target - layout->qr_px) / 2;

    layout->connected = server_connected(app);

    /* Keep the QR matrix around for painting by re-encoding from the same URL. */
    return true;
}

/* ------------------------------------------------------------------ */
/* Painting                                                           */
/* ------------------------------------------------------------------ */

static void paint_background(CardLayout *layout, unsigned char *pixels)
{
    double left = (double)layout->margin;
    double top = (double)layout->margin;
    double right = (double)(layout->margin + layout->card_w);
    double bottom = (double)(layout->margin + layout->card_h);

    for (int y = 0; y < layout->win_h; y++) {
        for (int x = 0; x < layout->win_w; x++) {
            double distance = rounded_rect_sdf(x + 0.5, y + 0.5, left, top, right, bottom,
                                               (double)layout->radius);
            double coverage = 0.5 - distance;
            if (coverage > 1.0) {
                coverage = 1.0;
            }
            if (coverage <= 0.0) {
                /* Soft shadow below the card, fading out over a few pixels. */
                if (distance > 0.5 && distance < (double)layout->margin + 2.0) {
                    double falloff = 1.0 - (distance - 0.5) / ((double)layout->margin + 1.5);
                    if (falloff > 0.0) {
                        int alpha = (int)(falloff * falloff * 60.0);
                        store_pixel(pixels, layout->win_w, x, y, 0, 0, 0, alpha);
                    }
                }
                continue;
            }
            int alpha = (int)(coverage * 255.0 + 0.5);
            if (alpha > 255) {
                alpha = 255;
            }
            /* 1 px hairline just inside the edge, like a Win11 flyout. */
            if (distance > -1.25) {
                store_pixel(pixels, layout->win_w, x, y, 0xD8, 0xD8, 0xDE, alpha);
            } else {
                store_pixel(pixels, layout->win_w, x, y, 0xFF, 0xFF, 0xFF, alpha);
            }
        }
    }
}

static void paint_qr(CardLayout *layout, HDC dc, App *app)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    char url[PAIRING_URL_MAX];
    pairing_url(app, url, sizeof(url));
    if (!url[0]) {
        return;
    }
    if (!qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        return;
    }

    int modules = qrcodegen_getSize(qr);
    if (modules != layout->qr_modules) {
        return;
    }

    HBRUSH brush = CreateSolidBrush(RGB(0x0B, 0x0B, 0x0C));
    if (!brush) {
        return;
    }
    int origin_x = layout->qr_x + CARD_QUIET_MODULES * layout->module_px;
    int origin_y = layout->qr_y + CARD_QUIET_MODULES * layout->module_px;
    for (int y = 0; y < modules; y++) {
        for (int x = 0; x < modules; x++) {
            if (!qrcodegen_getModule(qr, x, y)) {
                continue;
            }
            RECT cell;
            cell.left = origin_x + x * layout->module_px;
            cell.top = origin_y + y * layout->module_px;
            cell.right = cell.left + layout->module_px;
            cell.bottom = cell.top + layout->module_px;
            FillRect(dc, &cell, brush);
        }
    }
    DeleteObject(brush);
}

static RECT scaled_rect(CardLayout *layout, int x, int y, int width, int height)
{
    RECT rect = {scale_px(x, layout->dpi), scale_px(y, layout->dpi),
                 scale_px(x + width, layout->dpi), scale_px(y + height, layout->dpi)};
    return rect;
}

static void draw_text(CardLayout *layout, HDC dc, const wchar_t *text, RECT rect,
                      int size, int weight, COLORREF color, UINT align)
{
    HFONT font = CreateFontW(-scale_px(size, layout->dpi), 0, 0, 0, weight,
                             0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ previous = font ? SelectObject(dc, font) : NULL;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (font) {
        SelectObject(dc, previous);
        DeleteObject(font);
    }
}

static void draw_line(HDC dc, int x1, int y1, int x2, int y2)
{
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
}

static void draw_system_icon(CardLayout *layout, HDC dc, RECT rect, const wchar_t *glyph,
                             int font_size)
{
    HFONT font = CreateFontW(-scale_px(font_size, layout->dpi), 0, 0, 0, FW_NORMAL,
                             0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    if (!font) return;
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x24, 0x27, 0x2D));
    DrawTextW(dc, glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    DeleteObject(font);
}

static void draw_slider_thumb(CardLayout *layout, HDC dc, int center_x, int center_y)
{
    int outer_size = scale_px(15, layout->dpi);
    int outer_left = center_x - outer_size / 2;
    int outer_top = center_y - outer_size / 2;
    RECT outer = {outer_left, outer_top, outer_left + outer_size, outer_top + outer_size};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(0xE6, 0xE8, 0xEC));
    HGDIOBJ old_brush = SelectObject(dc, white);
    HGDIOBJ old_pen = SelectObject(dc, border);
    int outer_corner = scale_px(4, layout->dpi);
    RoundRect(dc, outer.left, outer.top, outer.right, outer.bottom,
              outer_corner, outer_corner);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(border);
    DeleteObject(white);

    int inner_size = scale_px(7, layout->dpi);
    int inner_left = center_x - inner_size / 2;
    int inner_top = center_y - inner_size / 2;
    RECT inner = {inner_left, inner_top, inner_left + inner_size, inner_top + inner_size};
    HBRUSH dark = CreateSolidBrush(RGB(0x31, 0x38, 0x44));
    old_brush = SelectObject(dc, dark);
    old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    int inner_corner = scale_px(2, layout->dpi);
    RoundRect(dc, inner.left, inner.top, inner.right, inner.bottom,
              inner_corner, inner_corner);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(dark);
}

static void paint_button(CardLayout *layout, HDC dc, RECT rect, int id, UINT state)
{
    bool primary = id == CARD_ID_DONE;
    COLORREF fill = primary ? RGB(0x31, 0x38, 0x44) :
                    (state & ODS_SELECTED) ? RGB(0xF4, 0xF5, 0xF7) : RGB(255, 255, 255);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN border = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, border);
    int radius = scale_px(6, layout->dpi);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_pen);
    DeleteObject(border);
    if (primary) {
        draw_text(layout, dc, L"完成", rect, 13, FW_NORMAL, RGB(255, 255, 255), DT_CENTER);
    } else {
        int cx = (rect.left + rect.right) / 2, cy = (rect.top + rect.bottom) / 2;
        int half = scale_px(5, layout->dpi), span = scale_px(7, layout->dpi);
        if (id == CARD_ID_CLOSE) {
            /* The close glyph has a narrower font bounding box than the
             * gear, so its point size is increased to match the visible
             * icon bounds while keeping the same system font and stroke. */
            draw_system_icon(layout, dc, rect, L"\xE711", 15);
        } else if (id == CARD_ID_BACK) {
            HPEN pen = CreatePen(PS_SOLID, scale_px(2, layout->dpi), RGB(0x31, 0x38, 0x44));
            old_pen = SelectObject(dc, pen);
            draw_line(dc, cx - span, cy, cx + span, cy);
            draw_line(dc, cx - span, cy, cx - span + half, cy - half);
            draw_line(dc, cx - span, cy, cx - span + half, cy + half);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        } else {
            draw_system_icon(layout, dc, rect, L"\xE713", 13);
        }
    }
    SelectObject(dc, old_brush);
    DeleteObject(brush);
}

static void paint_slider(CardLayout *layout, HDC dc, RECT client, int position, int minimum, int maximum)
{
    /* Erase the complete child surface before drawing the new thumb. Trackbar's
     * default item invalidation only covers the new thumb, so old positions
     * otherwise accumulate as a dotted trail during a drag. */
    HBRUSH background = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &client, background);
    DeleteObject(background);

    int padding = scale_px(8, layout->dpi);
    int left = client.left + padding;
    int right = client.right - padding;
    int cy = (client.top + client.bottom) / 2;
    int center = left + MulDiv(right - left, position - minimum, maximum - minimum);
    int half_track = scale_px(2, layout->dpi);
    RECT inactive = {left, cy - half_track, right, cy + half_track};
    HBRUSH brush = CreateSolidBrush(RGB(0xF0, 0xF1, 0xF4));
    FillRect(dc, &inactive, brush);
    DeleteObject(brush);
    RECT active = inactive;
    active.right = center;
    brush = CreateSolidBrush(RGB(0x31, 0x38, 0x44));
    FillRect(dc, &active, brush);
    DeleteObject(brush);

    draw_slider_thumb(layout, dc, center, cy);
}

static void slider_draw(HWND window, HDC dc, App *app)
{
    RECT client;
    GetClientRect(window, &client);
    int minimum = (int)SendMessageW(window, TBM_GETRANGEMIN, 0, 0);
    int maximum = (int)SendMessageW(window, TBM_GETRANGEMAX, 0, 0);
    int position = (int)SendMessageW(window, TBM_GETPOS, 0, 0);
    paint_slider(&g_layout, dc, client, position, minimum, maximum);
    (void)app;
}

static LRESULT CALLBACK slider_subclass_proc(HWND window, UINT message, WPARAM wparam,
                                             LPARAM lparam, UINT_PTR subclass_id,
                                             DWORD_PTR reference)
{
    App *app = (App *)reference;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        slider_draw(window, dc, app);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        slider_draw(window, (HDC)wparam, app);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(window, NULL, TRUE);
        break;
    case WM_THEMECHANGED:
    case WM_UPDATEUISTATE:
        InvalidateRect(window, NULL, TRUE);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

static void paint_text(CardLayout *layout, HDC dc, App *app)
{
    COLORREF text = RGB(0x24, 0x27, 0x2D), muted = RGB(0x7B, 0x81, 0x8C);
    draw_text(layout, dc, APP_TITLE, scaled_rect(layout, 16, 6, 180, 30), 12, FW_MEDIUM, text, DT_LEFT);
    paint_button(layout, dc, scaled_rect(layout, 209, 6, 30, 30), CARD_ID_SETTINGS, 0);
    paint_button(layout, dc, scaled_rect(layout, 242, 6, 30, 30), CARD_ID_CLOSE, 0);
    if (!app->card_settings) {
        return;
    }
    paint_button(layout, dc, scaled_rect(layout, 16, 46, 30, 30), CARD_ID_BACK, 0);
    draw_text(layout, dc, L"设置", scaled_rect(layout, 53, 42, 204, 38), 15, FW_MEDIUM, text, DT_LEFT);
    for (int i = 0; i < 2; i++) {
        int y = 87 + i * 81;
        int percent = (int)InterlockedCompareExchange(i ? &app->scroll_speed_percent : &app->mouse_speed_percent, 0, 0);
        if (percent < SPEED_MIN_PERCENT || percent > SPEED_MAX_PERCENT) percent = 100;
        wchar_t value[16];
        _snwprintf_s(value, 16, _TRUNCATE, L"%d.%d×", percent / 100, (percent % 100) / 10);
        draw_text(layout, dc, i ? L"滚动倍率" : L"鼠标灵敏度", scaled_rect(layout, 25, y, 160, 22), 13, FW_MEDIUM, text, DT_LEFT);
        draw_text(layout, dc, value, scaled_rect(layout, 185, y, 72, 22), 15, FW_NORMAL, text, DT_RIGHT);
        draw_text(layout, dc, L"0.5×", scaled_rect(layout, 25, y + 55, 100, 16), 11, FW_NORMAL, muted, DT_LEFT);
        draw_text(layout, dc, L"3×", scaled_rect(layout, 157, y + 55, 100, 16), 11, FW_NORMAL, muted, DT_RIGHT);
        RECT track = scaled_rect(layout, 33, y + 38, 216, 4);
        int x = track.left + MulDiv(track.right - track.left, percent - SPEED_MIN_PERCENT,
                                    SPEED_MAX_PERCENT - SPEED_MIN_PERCENT);
        int left = track.left;
        int right = track.right;
        int cy = scale_px(y + 40, layout->dpi);
        HBRUSH inactive = CreateSolidBrush(RGB(0xF0, 0xF1, 0xF4));
        RECT inactive_rect = {left, cy - scale_px(2, layout->dpi), right, cy + scale_px(2, layout->dpi)};
        FillRect(dc, &inactive_rect, inactive);
        DeleteObject(inactive);
        HBRUSH active = CreateSolidBrush(RGB(0x31, 0x38, 0x44));
        RECT active_rect = inactive_rect;
        active_rect.right = x;
        FillRect(dc, &active_rect, active);
        DeleteObject(active);
        draw_slider_thumb(layout, dc, x, cy);
    }
    paint_button(layout, dc, scaled_rect(layout, 25, 287, 232, 36), CARD_ID_DONE, 0);
}

static void paint_card(App *app, CardLayout *layout, CardSurface *surface)
{
    memset(surface->bits, 0, (size_t)surface->width * (size_t)surface->height * 4);
    unsigned char *pixels = (unsigned char *)surface->bits;

    paint_background(layout, pixels);
    if (!app->card_settings) paint_qr(layout, surface->dc, app);
    paint_text(layout, surface->dc, app);

    /* GDI leaves the alpha byte undefined. Restore it only well inside the
     * card: the antialiased rim and the shadow keep the alpha painted above,
     * so transparent rounded corners stay transparent. */
    double left_edge = (double)layout->margin;
    double top_edge = (double)layout->margin;
    double right_edge = (double)(layout->margin + layout->card_w);
    double bottom_edge = (double)(layout->margin + layout->card_h);
    for (int y = 0; y < surface->height; y++) {
        unsigned char *row = pixels + (size_t)y * (size_t)surface->width * 4;
        for (int x = 0; x < surface->width; x++) {
            double distance = rounded_rect_sdf(x + 0.5, y + 0.5, left_edge, top_edge,
                                               right_edge, bottom_edge, (double)layout->radius);
            if (distance < -1.0) {
                row[3] = 0xFF;
            }
            row += 4;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Surfaces                                                           */
/* ------------------------------------------------------------------ */

static void surface_destroy(CardSurface *surface)
{
    if (surface->dc && surface->bitmap) {
        SelectObject(surface->dc, GetStockObject(NULL_BRUSH));
    }
    if (surface->dc) {
        DeleteDC(surface->dc);
    }
    if (surface->bitmap) {
        DeleteObject(surface->bitmap);
    }
    memset(surface, 0, sizeof(*surface));
}

static bool surface_create(CardSurface *surface, int width, int height)
{
    if (surface->bitmap && surface->width == width && surface->height == height) {
        return true;
    }
    surface_destroy(surface);

    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return false;
    }
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        DeleteObject(bitmap);
        return false;
    }
    SelectObject(dc, bitmap);

    surface->dc = dc;
    surface->bitmap = bitmap;
    surface->bits = bits;
    surface->width = width;
    surface->height = height;
    return true;
}

/* ------------------------------------------------------------------ */
/* Window                                                             */
/* ------------------------------------------------------------------ */

static void commit_speed(App *app, bool scroll)
{
    bool *dirty = scroll ? &g_controls.scroll_dirty : &g_controls.mouse_dirty;
    if (!*dirty) return;
    *dirty = false;
    int percent = (int)InterlockedCompareExchange(scroll ? &app->scroll_speed_percent : &app->mouse_speed_percent, 0, 0);
    app_set_speed(app, scroll, percent, true);
}

static void sync_controls(App *app)
{
    int show = app->card_settings ? SW_SHOWNA : SW_HIDE;
    ShowWindow(g_controls.back, show);
    ShowWindow(g_controls.done, show);
    ShowWindow(g_controls.mouse, show);
    ShowWindow(g_controls.scroll, show);
    SendMessageW(g_controls.mouse, TBM_SETPOS, TRUE,
                  InterlockedCompareExchange(&app->mouse_speed_percent, 0, 0) / SPEED_STEP_PERCENT);
    SendMessageW(g_controls.scroll, TBM_SETPOS, TRUE,
                  InterlockedCompareExchange(&app->scroll_speed_percent, 0, 0) / SPEED_STEP_PERCENT);
}

static void set_settings_page(App *app, bool settings)
{
    commit_speed(app, false);
    commit_speed(app, true);
    app->card_settings = settings;
    sync_controls(app);
    InvalidateRect(app->card, NULL, FALSE);
    SetFocus(settings ? g_controls.mouse : g_controls.settings);
    app_write_state(app);
}

static bool layout_controls(App *app, int dpi)
{
    if (!build_layout(app, dpi, &g_layout)) return false;
    const struct { HWND window; int x, y, width, height; } controls[] = {
        {g_controls.settings, 209, 6, 30, 30}, {g_controls.close, 242, 6, 30, 30},
        {g_controls.back, 16, 46, 30, 30}, {g_controls.done, 25, 287, 232, 36},
        {g_controls.mouse, 25, 112, 232, 30}, {g_controls.scroll, 25, 193, 232, 30},
    };
    HFONT font = CreateFontW(-scale_px(13, dpi), 0, 0, 0, FW_NORMAL,
                             0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    for (int i = 0; i < (int)(sizeof(controls) / sizeof(controls[0])); i++) {
        RECT rect = scaled_rect(&g_layout, controls[i].x, controls[i].y, controls[i].width, controls[i].height);
        SetWindowPos(controls[i].window, NULL, rect.left, rect.top,
                      rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(controls[i].window, WM_SETFONT, (WPARAM)font, FALSE);
    }
    if (g_controls.font) DeleteObject(g_controls.font);
    g_controls.font = font;
    SendMessageW(g_controls.mouse, TBM_SETTHUMBLENGTH, scale_px(16, dpi), 0);
    SendMessageW(g_controls.scroll, TBM_SETTHUMBLENGTH, scale_px(16, dpi), 0);
    HRGN region = CreateRoundRectRgn(0, 0, g_layout.win_w + 1, g_layout.win_h + 1,
                                    g_layout.radius * 2, g_layout.radius * 2);
    if (region && !SetWindowRgn(app->card, region, TRUE)) DeleteObject(region);
    sync_controls(app);
    return true;
}

static LRESULT CALLBACK card_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    }
    App *app = (App *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT paint;
        HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : (HDC)wparam;
        if (build_layout(app, g_layout.dpi, &g_layout) &&
            surface_create(&g_surface, g_layout.win_w, g_layout.win_h)) {
            paint_card(app, &g_layout, &g_surface);
            BitBlt(dc, 0, 0, g_layout.win_w, g_layout.win_h, g_surface.dc, 0, 0, SRCCOPY);
        }
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        if (point.y >= 0 && point.y < scale_px(42, g_layout.dpi) &&
            point.x >= 0 && point.x < scale_px(205, g_layout.dpi)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK:
        if (wparam == HTCAPTION) return 0;
        break;
    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lparam;
        if (layout_controls(app, HIWORD(wparam))) {
            SetWindowPos(window, NULL, suggested->left, suggested->top,
                          g_layout.win_w, g_layout.win_h, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case CARD_ID_SETTINGS: set_settings_page(app, true); return 0;
        case CARD_ID_BACK:
        case CARD_ID_DONE: set_settings_page(app, false); return 0;
        case CARD_ID_CLOSE:
        case IDCANCEL: SendMessageW(window, WM_CLOSE, 0, 0); return 0;
        case IDOK:
            if (app->card_settings) set_settings_page(app, false);
            return 0;
        default: break;
        }
        break;
    case WM_HSCROLL: {
        HWND slider = (HWND)lparam;
        if (slider != g_controls.mouse && slider != g_controls.scroll) break;
        bool scroll = slider == g_controls.scroll;
        int percent = (int)SendMessageW(slider, TBM_GETPOS, 0, 0) * SPEED_STEP_PERCENT;
        LONG current = InterlockedCompareExchange(scroll ? &app->scroll_speed_percent : &app->mouse_speed_percent, 0, 0);
        if (percent != current) {
            if (scroll) g_controls.scroll_dirty = true;
            else g_controls.mouse_dirty = true;
            app_set_speed(app, scroll, percent, false);
            RECT value = scaled_rect(&g_layout, 185, scroll ? 168 : 87, 72, 22);
            InvalidateRect(window, &value, FALSE);
        }
        InvalidateRect(slider, NULL, TRUE);
        UpdateWindow(slider);
        /* Native trackbars send ENDTRACK after pointer or keyboard changes.
         * Commit page/arrow changes too, while a continuous drag stays cheap. */
        if (LOWORD(wparam) != TB_THUMBTRACK) commit_speed(app, scroll);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lparam;
        if (item->CtlType == ODT_BUTTON) {
            paint_button(&g_layout, item->hDC, item->rcItem, (int)item->CtlID, item->itemState);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wparam, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            commit_speed(app, false);
            commit_speed(app, true);
        }
        break;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_MINIMIZE) {
            SendMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_CLOSE:
        card_hide(app);
        if (!app->tray_added) app_request_quit(app);
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool card_create(App *app)
{
    INITCOMMONCONTROLSEX common = {sizeof(common), ICC_BAR_CLASSES};
    if (!InitCommonControlsEx(&common)) return false;
    WNDCLASSEXW window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_DROPSHADOW;
    window_class.lpfnWndProc = card_proc;
    window_class.hInstance = app->instance;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hIcon = LoadIconW(app->instance, MAKEINTRESOURCEW(1));
    window_class.lpszClassName = CARD_CLASS;

    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    app->card = CreateWindowExW(
        WS_EX_APPWINDOW, CARD_CLASS, APP_TITLE,
        WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        0, 0, CARD_WIDTH_PX, CARD_HEIGHT_PX, NULL, NULL, app->instance, app);
    if (!app->card) {
        return false;
    }

    const struct { int id; const wchar_t *label; HWND *target; } buttons[] = {
        {CARD_ID_SETTINGS, L"设置", &g_controls.settings},
        {CARD_ID_CLOSE, L"关闭主界面，保留托盘运行", &g_controls.close},
        {CARD_ID_BACK, L"返回主页面", &g_controls.back},
        {CARD_ID_DONE, L"完成", &g_controls.done},
    };
    for (int i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++) {
        *buttons[i].target = CreateWindowExW(0, L"BUTTON", buttons[i].label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
            app->card, (HMENU)(INT_PTR)buttons[i].id, app->instance, NULL);
        if (!*buttons[i].target) { card_destroy(app); return false; }
    }
    const struct { int id; const wchar_t *label; HWND *target; } sliders[] = {
        {CARD_ID_MOUSE, L"鼠标灵敏度", &g_controls.mouse},
        {CARD_ID_SCROLL, L"滚动倍率", &g_controls.scroll},
    };
    for (int i = 0; i < 2; i++) {
        *sliders[i].target = CreateWindowExW(0, TRACKBAR_CLASSW, sliders[i].label,
            WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH,
            0, 0, 1, 1, app->card, (HMENU)(INT_PTR)sliders[i].id, app->instance, NULL);
        if (!*sliders[i].target) { card_destroy(app); return false; }
        SendMessageW(*sliders[i].target, TBM_SETRANGE, FALSE,
                      MAKELPARAM(SPEED_MIN_PERCENT / SPEED_STEP_PERCENT, SPEED_MAX_PERCENT / SPEED_STEP_PERCENT));
        SendMessageW(*sliders[i].target, TBM_SETLINESIZE, 0, 1);
        SendMessageW(*sliders[i].target, TBM_SETPAGESIZE, 0, 1);
        if (!SetWindowSubclass(*sliders[i].target, slider_subclass_proc,
                               (UINT_PTR)sliders[i].id, (DWORD_PTR)app)) {
            card_destroy(app);
            return false;
        }
    }
    SendMessageW(app->card, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    SetWindowPos(g_controls.done, g_controls.scroll, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!layout_controls(app, 96)) { card_destroy(app); return false; }
    return true;
}

void card_destroy(App *app)
{
    if (g_controls.mouse) RemoveWindowSubclass(g_controls.mouse, slider_subclass_proc, CARD_ID_MOUSE);
    if (g_controls.scroll) RemoveWindowSubclass(g_controls.scroll, slider_subclass_proc, CARD_ID_SCROLL);
    if (app->card) {
        DestroyWindow(app->card);
        app->card = NULL;
    }
    surface_destroy(&g_surface);
    if (g_controls.font) DeleteObject(g_controls.font);
    memset(&g_controls, 0, sizeof(g_controls));
}

static void card_position(CardLayout *layout, RECT icon, int *out_x, int *out_y)
{
    int anchor_x = icon.left + (icon.right - icon.left) / 2;
    int x = anchor_x - layout->win_w / 2;
    int y = icon.top - layout->card_h - layout->margin - scale_px(4, layout->dpi);

    MONITORINFO monitor_info;
    memset(&monitor_info, 0, sizeof(monitor_info));
    monitor_info.cbSize = sizeof(monitor_info);
    HMONITOR monitor = MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &monitor_info)) {
        const RECT *work = &monitor_info.rcWork;
        if (y < work->top) {
            /* Taskbar on top: put the card below the icon instead. */
            y = icon.bottom + layout->margin + scale_px(4, layout->dpi);
        }
        if (x + layout->win_w > work->right) {
            x = work->right - layout->win_w;
        }
        if (x < work->left) {
            x = work->left;
        }
        if (y + layout->win_h > work->bottom) {
            y = work->bottom - layout->win_h;
        }
        if (y < work->top) {
            y = work->top;
        }
    }
    *out_x = x;
    *out_y = y;
}

void card_show(App *app)
{
    if (!app->card) {
        return;
    }

    if (!g_controls.positioned) {
        tray_refresh_rect(app);
        RECT icon;
        if (!tray_icon_rect(app, &icon)) {
            POINT cursor = {0, 0};
            GetCursorPos(&cursor);
            icon = (RECT){cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
        }
        if (!layout_controls(app, (int)monitor_dpi(icon))) return;
        int x, y;
        card_position(&g_layout, icon, &x, &y);
        SetWindowPos(app->card, NULL, x, y, g_layout.win_w, g_layout.win_h, SWP_NOZORDER | SWP_NOACTIVATE);
        g_controls.positioned = true;
    }
    sync_controls(app);
    app->card_visible = true;
    ShowWindow(app->card, SW_SHOWNORMAL);
    SetForegroundWindow(app->card);
    SetFocus(app->card_settings ? g_controls.mouse : g_controls.settings);
    InvalidateRect(app->card, NULL, FALSE);
    app_log(app, "card show %dx%d dpi=%d", g_layout.win_w, g_layout.win_h, g_layout.dpi);
    app_write_state(app);
}

void card_hide(App *app)
{
    commit_speed(app, false);
    commit_speed(app, true);
    if (!app->card_visible) {
        return;
    }
    if (app->card) {
        ShowWindow(app->card, SW_HIDE);
    }
    app->card_visible = false;
    app->card_settings = false;
    app_log(app, "card hide");
    app_write_state(app);
}

void card_refresh(App *app)
{
    if (!app->card_visible) {
        return;
    }
    InvalidateRect(app->card, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* QR matrix dump (cross-checked against the Rust reference encoder)   */
/* ------------------------------------------------------------------ */

bool card_dump_qr(App *app, const wchar_t *path)
{
    char url[PAIRING_URL_MAX];
    pairing_url(app, url, sizeof(url));
    if (!url[0]) {
        return false;
    }

    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        return false;
    }
    int size = qrcodegen_getSize(qr);

    FILE *file = NULL;
    if (_wfopen_s(&file, path, L"w") != 0 || !file) {
        return false;
    }
    fprintf(file, "url=%s\n", url);
    fprintf(file, "size=%d\n", size);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            fputc(qrcodegen_getModule(qr, x, y) ? '1' : '0', file);
        }
        fputc('\n', file);
    }
    fclose(file);
    return true;
}

/* ------------------------------------------------------------------ */
/* Off-screen renderer (used by the test harness)                      */
/* ------------------------------------------------------------------ */

bool card_render_to_bmp(App *app, const wchar_t *path)
{
    CardLayout layout;
    if (!build_layout(app, 96, &layout)) {
        return false;
    }

    CardSurface surface;
    memset(&surface, 0, sizeof(surface));
    if (!surface_create(&surface, layout.win_w, layout.win_h)) {
        return false;
    }
    paint_card(app, &layout, &surface);

    int width = layout.win_w;
    int height = layout.win_h;
    int row_bytes = width * 3;
    int padding = (4 - (row_bytes % 4)) % 4;
    int image_size = (row_bytes + padding) * height;

    unsigned char *composited = (unsigned char *)malloc((size_t)row_bytes * (size_t)height);
    if (!composited) {
        surface_destroy(&surface);
        return false;
    }

    /* Flatten onto a light gray background so transparency is visible. */
    const unsigned char *pixels = (const unsigned char *)surface.bits;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const unsigned char *source = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
            int alpha = source[3];
            int blue = source[0];
            int green = source[1];
            int red = source[2];
            int out_blue = blue + (0xE8 * (255 - alpha) + 127) / 255;
            int out_green = green + (0xE8 * (255 - alpha) + 127) / 255;
            int out_red = red + (0xE8 * (255 - alpha) + 127) / 255;
            unsigned char *target = composited + ((size_t)(height - 1 - y) * (size_t)row_bytes + (size_t)x * 3);
            target[0] = (unsigned char)(out_blue > 255 ? 255 : out_blue);
            target[1] = (unsigned char)(out_green > 255 ? 255 : out_green);
            target[2] = (unsigned char)(out_red > 255 ? 255 : out_red);
        }
    }

    BITMAPFILEHEADER file_header;
    memset(&file_header, 0, sizeof(file_header));
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + (DWORD)image_size;

    BITMAPINFOHEADER info_header;
    memset(&info_header, 0, sizeof(info_header));
    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = width;
    info_header.biHeight = height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 24;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = (DWORD)image_size;

    FILE *file = NULL;
    bool ok = _wfopen_s(&file, path, L"wb") == 0 && file != NULL;
    if (ok) {
        ok = fwrite(&file_header, sizeof(file_header), 1, file) == 1;
        ok = ok && fwrite(&info_header, sizeof(info_header), 1, file) == 1;
        for (int y = 0; y < height && ok; y++) {
            const unsigned char *row = composited + (size_t)y * (size_t)row_bytes;
            ok = fwrite(row, (size_t)row_bytes, 1, file) == 1;
            if (padding > 0 && ok) {
                unsigned char zero[3] = {0, 0, 0};
                ok = fwrite(zero, (size_t)padding, 1, file) == 1;
            }
        }
        fclose(file);
    }

    free(composited);
    surface_destroy(&surface);
    app_log(app, "card render %s -> %s", ok ? "ok" : "failed", "bmp");
    return ok;
}
