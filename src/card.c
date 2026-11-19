/*
 * Compact main window: GDI painting with native buttons and trackbars.
 * QR modules use integer pixels; the window follows per-monitor DPI.
 *
 * Visual spec lives in docs/window-design.html:
 *   - 282 x 338 card, 14 px corners, 1 px hairline rim, vertical surface
 *     gradient and a soft inner highlight along the top edge
 *   - the code sits on a 228 px white tile (10 px corners, 1 px edge); the
 *     matrix itself stays 217 px so modules are 7 px and the quiet zone is
 *     three modules plus the tile padding
 *   - owner drawn controls: 28 px icon buttons, a 40 px primary button and a
 *     4 px track with an antialiased 18 px thumb
 *   - no painted shadow: at this size it reads as a grey outline, so the card
 *     separates itself with the hairline rim and the surface gradient only
 */
#include "app.h"
#include "qrcodegen.h"
#include <commctrl.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")

#define CARD_WIDTH_PX 282
#define CARD_HEIGHT_PX 338
#define CARD_RADIUS_PX 14
#define CARD_DWM_RADIUS_PX 8
#define CARD_HEADER_PX 44
#define CARD_TILE_PX 236
#define CARD_TILE_RADIUS_PX 10
#define CARD_TILE_PAD_PX 3
#define CARD_QUIET_MODULES 2

/* Header controls sit on a 44 px band; 28 px targets keep a 10 px inset. */
#define CARD_ICON_PX 28
#define CARD_ICON_Y 8
#define CARD_SETTINGS_X 214
#define CARD_CLOSE_X 244
#define CARD_BACK_X 10

/* Settings rows. Field one starts under the header at y = 64. */
#define FIELD_ONE_Y 64
#define FIELD_TWO_Y 165
#define FIELD_TEXT_H 21
#define FIELD_VALUE_W 120
#define FIELD_TICKS_DY 51
#define FIELD_TRACK_DY 38
#define TRACK_WINDOW_H 30
/* The thumb is centred on the track end, so the child window needs to be
 * wider than the track or half the thumb is clipped at either extreme. */
#define TRACK_PAD_PX 12
#define DONE_X 20
#define DONE_Y 278
#define DONE_W 242
#define DONE_H 40

#define UI_FACE L"Microsoft YaHei UI"   /* CJK */
#define UI_FACE_NUM L"Segoe UI"          /* digits and the multiplication sign */

#define CARD_ID_SETTINGS 1001
#define CARD_ID_CLOSE 1002
#define CARD_ID_BACK 1003
#define CARD_ID_DONE 1004
#define CARD_ID_MOUSE 1005
#define CARD_ID_SCROLL 1006

/* Palette ------------------------------------------------------------- */
#define COL_SURFACE_TOP_R 0xFB
#define COL_SURFACE_TOP_G 0xFC
#define COL_SURFACE_TOP_B 0xFD
#define COL_SURFACE_BOT_R 0xF3
#define COL_SURFACE_BOT_G 0xF5
#define COL_SURFACE_BOT_B 0xF7

#define COL_HAIRLINE RGB(0xE6, 0xE8, 0xEC)
#define COL_TILE_LINE RGB(0xE9, 0xEA, 0xEE)
#define COL_TITLE RGB(0x1F, 0x24, 0x2C)
#define COL_TEXT RGB(0x24, 0x27, 0x2D)
#define COL_TICKS RGB(0x6E, 0x76, 0x83)
#define COL_ICON RGB(0x5B, 0x64, 0x72)
#define COL_ICON_HOT RGB(0x1F, 0x24, 0x2C)
#define COL_ACCENT RGB(0x2B, 0x32, 0x40)
#define COL_ACCENT_PRESS RGB(0x1C, 0x21, 0x2B)
#define COL_TRACK RGB(0xE2, 0xE5, 0xEA)
#define COL_HOVER_BG RGB(0xF1, 0xF2, 0xF5)
#define COL_PRESS_BG RGB(0xE7, 0xE9, 0xED)
#define COL_QR RGB(0x10, 0x13, 0x17)
#define COL_THUMB_RING RGB(0x45, 0x4E, 0x5E)

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
    int tile_x;
    int tile_y;
    int tile_px;
    int tile_radius;
    bool connected;
} CardLayout;

typedef struct {
    int red, green, blue;
} CardColor;

static CardSurface g_surface;
static CardLayout g_layout;
static bool g_corners_by_dwm;
static struct {
    HWND settings, close, back, done, mouse, scroll;
    HFONT font;
    bool positioned;
    bool mouse_dirty, scroll_dirty;
    int hot_id;
    bool tracking[6];
} g_controls;

/* Cached antialiased slider thumb, rebuilt when the DPI changes. */
static struct {
    HDC dc;
    HBITMAP bitmap;
    void *bits;
    int size;
    int diameter;
} g_thumb;

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

/* Source-over onto a premultiplied BGRA buffer. */
static void blend_pixel(unsigned char *pixels, int width, int x, int y,
                        int red, int green, int blue, int alpha)
{
    if (alpha <= 0) {
        return;
    }
    if (alpha > 255) {
        alpha = 255;
    }
    int keep = 255 - alpha;
    unsigned char *pixel = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
    pixel[0] = (unsigned char)((blue * alpha + pixel[0] * keep + 127) / 255);
    pixel[1] = (unsigned char)((green * alpha + pixel[1] * keep + 127) / 255);
    pixel[2] = (unsigned char)((red * alpha + pixel[2] * keep + 127) / 255);
    pixel[3] = (unsigned char)((alpha * 255 + pixel[3] * keep + 127) / 255);
}

static CardColor surface_color(int y, int height)
{
    double t = height > 1 ? (double)y / (double)(height - 1) : 0.0;
    CardColor color;
    color.red = (int)(COL_SURFACE_TOP_R + (COL_SURFACE_BOT_R - COL_SURFACE_TOP_R) * t + 0.5);
    color.green = (int)(COL_SURFACE_TOP_G + (COL_SURFACE_BOT_G - COL_SURFACE_TOP_G) * t + 0.5);
    color.blue = (int)(COL_SURFACE_TOP_B + (COL_SURFACE_BOT_B - COL_SURFACE_TOP_B) * t + 0.5);
    return color;
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

    /* DWM rounds at its own radius (about 8 px at 100%); the painted card has
     * to match, otherwise the leftover corner pixels stay unpainted. */
    layout->radius = scale_px(g_corners_by_dwm ? CARD_DWM_RADIUS_PX : CARD_RADIUS_PX,
                              layout->dpi);
    layout->card_w = layout->win_w = scale_px(CARD_WIDTH_PX, layout->dpi);
    layout->card_h = layout->win_h = scale_px(CARD_HEIGHT_PX, layout->dpi);

    /* The tile is centred in the space under the header, then lifted by
     * giving the bottom 18 px back to the window. */
    layout->tile_px = scale_px(CARD_TILE_PX, layout->dpi);
    layout->tile_radius = scale_px(CARD_TILE_RADIUS_PX, layout->dpi);

    /* Module size is derived from the tile, not from a fixed pixel target:
     * the symbol version follows the pairing URL, and a fixed target turns a
     * longer URL into smaller modules with a wide white mat around them. */
    int available = layout->tile_px - 2 * scale_px(CARD_TILE_PAD_PX, layout->dpi);
    layout->module_px = available / total_modules;
    if (layout->module_px < 1) {
        layout->module_px = 1;
    }
    layout->qr_px = layout->module_px * total_modules;
    layout->tile_x = (layout->win_w - layout->tile_px) / 2;
    int content = layout->win_h - scale_px(CARD_HEADER_PX, layout->dpi);
    int lift = scale_px(18, layout->dpi);
    int top_gap = (content - lift - layout->tile_px) / 2;
    if (top_gap < 0) {
        top_gap = 0;
    }
    layout->tile_y = scale_px(CARD_HEADER_PX, layout->dpi) + top_gap;

    layout->qr_x = layout->tile_x + (layout->tile_px - layout->qr_px) / 2;
    layout->qr_y = layout->tile_y + (layout->tile_px - layout->qr_px) / 2;

    layout->connected = server_connected(app);
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
    int highlight_rows = scale_px(46, layout->dpi);

    for (int y = 0; y < layout->win_h; y++) {
        CardColor base = surface_color(y, layout->win_h);
        for (int x = 0; x < layout->win_w; x++) {
            double distance = rounded_rect_sdf(x + 0.5, y + 0.5, left, top, right, bottom,
                                               (double)layout->radius);
            double coverage = 0.5 - distance;
            if (coverage > 1.0) {
                coverage = 1.0;
            }
            if (coverage <= 0.0) {
                continue;
            }
            int red = base.red, green = base.green, blue = base.blue;

            /* 1 px hairline just inside the edge, like a Win11 flyout. */
            if (distance > -1.6) {
                red = GetRValue(COL_HAIRLINE);
                green = GetGValue(COL_HAIRLINE);
                blue = GetBValue(COL_HAIRLINE);
            }

            /* Soft light from above: fades out over the top 46 px. It starts
             * inside the hairline so the 1 px rim still reads at the top. */
            if (y < highlight_rows && distance > -6.0 && distance <= -1.6) {
                double fade = 1.0 - (double)y / (double)highlight_rows;
                double lift = 0.62 * fade;
                red = (int)(red + (255 - red) * lift);
                green = (int)(green + (255 - green) * lift);
                blue = (int)(blue + (255 - blue) * lift);
            }

            store_pixel(pixels, layout->win_w, x, y, red, green, blue,
                        (int)(coverage * 255.0 + 0.5));
        }
    }
}

static void paint_tile(CardLayout *layout, unsigned char *pixels)
{
    double left = (double)layout->tile_x;
    double top = (double)layout->tile_y;
    double right = left + layout->tile_px;
    double bottom = top + layout->tile_px;
    double radius = (double)layout->tile_radius;
    int line_r = GetRValue(COL_TILE_LINE);
    int line_g = GetGValue(COL_TILE_LINE);
    int line_b = GetBValue(COL_TILE_LINE);

    for (int y = layout->tile_y - 2; y < layout->tile_y + layout->tile_px + 2; y++) {
        if (y < 0 || y >= layout->win_h) continue;
        for (int x = layout->tile_x - 2; x < layout->tile_x + layout->tile_px + 2; x++) {
            if (x < 0 || x >= layout->win_w) continue;
            double distance = rounded_rect_sdf(x + 0.5, y + 0.5, left, top, right, bottom, radius);
            double coverage = 0.5 - distance;
            if (coverage <= 0.0) continue;
            if (coverage > 1.0) coverage = 1.0;

            int red = 255, green = 255, blue = 255;
            if (distance > -1.4) {
                red = line_r;
                green = line_g;
                blue = line_b;
            }
            blend_pixel(pixels, layout->win_w, x, y, red, green, blue,
                        (int)(coverage * 255.0 + 0.5));
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

    HBRUSH brush = CreateSolidBrush(COL_QR);
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
                      int size, int weight, COLORREF color, UINT align, COLORREF background,
                      const wchar_t *face)
{
    HFONT font = CreateFontW(-scale_px(size, layout->dpi), 0, 0, 0, weight,
                             0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, face);
    HGDIOBJ previous = font ? SelectObject(dc, font) : NULL;
    /* ClearType blends its fringe against the background colour; leaving the
     * DC default (white) muddies text sitting on the tinted surface. */
    COLORREF back = background;
    if (back == CLR_NONE) {
        CardColor sampled = surface_color((rect.top + rect.bottom) / 2, layout->win_h);
        back = RGB(sampled.red, sampled.green, sampled.blue);
    }
    SetBkColor(dc, back);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (font) {
        SelectObject(dc, previous);
        DeleteObject(font);
    }
}

static void draw_system_icon(CardLayout *layout, HDC dc, RECT rect, const wchar_t *glyph,
                             int font_size, COLORREF color)
{
    HFONT font = CreateFontW(-scale_px(font_size, layout->dpi), 0, 0, 0, FW_NORMAL,
                             0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    if (!font) return;
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    DeleteObject(font);
}

/* Fill a rectangle with the surface gradient so owner drawn children blend
 * into the window instead of showing a flat patch. y_offset is the child's
 * top edge in card coordinates: child DCs start at zero, so sampling the
 * gradient with the child's own y would paint the colour of the window top. */
static void fill_surface(HDC dc, CardLayout *layout, RECT rect, int y_offset)
{
    for (int y = rect.top; y < rect.bottom; y++) {
        CardColor color = surface_color(y + y_offset, layout->win_h);
        HBRUSH brush = CreateSolidBrush(RGB(color.red, color.green, color.blue));
        if (!brush) continue;
        RECT row = {rect.left, y, rect.right, y + 1};
        FillRect(dc, &row, brush);
        DeleteObject(brush);
    }
}

/* ------------------------------------------------------------------ */
/* Antialiased rounded fills                                          */
/* ------------------------------------------------------------------ */

/* GDI's RoundRect() is not antialiased; a 10 px corner on the primary
 * button shows obvious stair steps. Render the shape into a cached 32-bit
 * buffer with a signed distance field and alpha blend it instead. */
static struct {
    HDC dc;
    HBITMAP bitmap;
    void *bits;
    int width;
    int height;
    int radius;
} g_round_cache;

static void round_cache_release(void)
{
    if (g_round_cache.dc && g_round_cache.bitmap) {
        SelectObject(g_round_cache.dc, GetStockObject(NULL_BRUSH));
    }
    if (g_round_cache.dc) DeleteDC(g_round_cache.dc);
    if (g_round_cache.bitmap) DeleteObject(g_round_cache.bitmap);
    memset(&g_round_cache, 0, sizeof(g_round_cache));
}

static bool round_cache_ensure(int width, int height, int radius)
{
    if (g_round_cache.bitmap && g_round_cache.width == width &&
        g_round_cache.height == height && g_round_cache.radius == radius) {
        return true;
    }
    round_cache_release();

    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        return false;
    }
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        DeleteObject(bitmap);
        return false;
    }
    SelectObject(dc, bitmap);
    g_round_cache.dc = dc;
    g_round_cache.bitmap = bitmap;
    g_round_cache.bits = bits;
    g_round_cache.width = width;
    g_round_cache.height = height;
    g_round_cache.radius = radius;
    return true;
}

/* top_fill/ bottom_fill give a barely visible vertical shift, standing in for
 * the 1 px highlight line that would need its own antialiasing pass. */
static void fill_round_rect(HDC dc, RECT rect, int radius, COLORREF top_fill, COLORREF bottom_fill)
{
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return;
    }
    if (!round_cache_ensure(width, height, radius)) {
        return;
    }
    unsigned char *pixels = (unsigned char *)g_round_cache.bits;
    memset(pixels, 0, (size_t)width * (size_t)height * 4);

    int tr = GetRValue(top_fill), tg = GetGValue(top_fill), tb = GetBValue(top_fill);
    int br = GetRValue(bottom_fill), bg = GetGValue(bottom_fill), bb = GetBValue(bottom_fill);
    double corner = (double)radius;

    for (int y = 0; y < height; y++) {
        double t = height > 1 ? (double)y / (double)(height - 1) : 0.0;
        int red = (int)(tr + (br - tr) * t + 0.5);
        int green = (int)(tg + (bg - tg) * t + 0.5);
        int blue = (int)(tb + (bb - tb) * t + 0.5);
        for (int x = 0; x < width; x++) {
            double distance = rounded_rect_sdf(x + 0.5, y + 0.5, 0.0, 0.0,
                                               (double)width, (double)height, corner);
            double coverage = 0.5 - distance;
            if (coverage <= 0.0) continue;
            if (coverage > 1.0) coverage = 1.0;
            blend_pixel(pixels, width, x, y, red, green, blue,
                        (int)(coverage * 255.0 + 0.5));
        }
    }

    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, rect.left, rect.top, width, height, g_round_cache.dc, 0, 0,
               width, height, blend);
}

/* ------------------------------------------------------------------ */
/* Slider thumb (antialiased, cached)                                 */
/* ------------------------------------------------------------------ */

static void thumb_release(void)
{
    if (g_thumb.dc && g_thumb.bitmap) {
        SelectObject(g_thumb.dc, GetStockObject(NULL_BRUSH));
    }
    if (g_thumb.dc) DeleteDC(g_thumb.dc);
    if (g_thumb.bitmap) DeleteObject(g_thumb.bitmap);
    memset(&g_thumb, 0, sizeof(g_thumb));
}

static bool thumb_build(int diameter, double ring, int pad)
{
    int size = diameter + pad * 2;
    if (g_thumb.bitmap && g_thumb.size == size && g_thumb.diameter == diameter) {
        return true;
    }
    thumb_release();

    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        return false;
    }
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        DeleteObject(bitmap);
        return false;
    }
    SelectObject(dc, bitmap);
    memset(bits, 0, (size_t)size * (size_t)size * 4);

    unsigned char *pixels = (unsigned char *)bits;
    double center = size * 0.5;
    double radius = diameter * 0.5;
    int ring_r = GetRValue(COL_THUMB_RING), ring_g = GetGValue(COL_THUMB_RING), ring_b = GetBValue(COL_THUMB_RING);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double dx = x + 0.5 - center;
            double dy = y + 0.5 - center;
            double distance = sqrt(dx * dx + dy * dy);

            /* Antialias both edges of the ring. Stepping straight from white
             * to the ring colour at its inner edge leaves a 1 px hard circle
             * inside a soft outer edge, which reads as "not round". */
            /* Contact shadow only under the lower arc: a shadow around the
             * whole perimeter reads as a light halo next to the white fill. */
            if (dy > radius * 0.35 && distance > radius - 0.5 && distance < radius + 2.0) {
                int alpha = (int)((1.0 - (distance - radius + 0.5) / 2.5) * 44.0);
                blend_pixel(pixels, size, x, y, 12, 16, 24, alpha);
            }

            double outer_coverage = radius - distance + 0.5;
            if (outer_coverage <= 0.0) continue;
            if (outer_coverage > 1.0) outer_coverage = 1.0;

            double ring_mix = distance - (radius - ring) + 0.5;
            if (ring_mix < 0.0) ring_mix = 0.0;
            if (ring_mix > 1.0) ring_mix = 1.0;

            int red = (int)(255 + (ring_r - 255) * ring_mix);
            int green = (int)(255 + (ring_g - 255) * ring_mix);
            int blue = (int)(255 + (ring_b - 255) * ring_mix);
            blend_pixel(pixels, size, x, y, red, green, blue,
                        (int)(outer_coverage * 255.0 + 0.5));
        }
    }

    g_thumb.dc = dc;
    g_thumb.bitmap = bitmap;
    g_thumb.bits = bits;
    g_thumb.size = size;
    g_thumb.diameter = diameter;
    return true;
}

static void draw_slider_thumb(CardLayout *layout, HDC dc, int center_x, int center_y)
{
    int diameter = scale_px(20, layout->dpi);
    /* The ring needs a solid core: with 2 px the antialiased inner and
     * outer edges eat most of it and the ring reads as a broken outline. */
    double ring = 2.6 * (double)layout->dpi / 96.0;
    int pad = scale_px(4, layout->dpi);
    if (!thumb_build(diameter, ring, pad)) {
        return;
    }
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, center_x - g_thumb.size / 2, center_y - g_thumb.size / 2,
               g_thumb.size, g_thumb.size, g_thumb.dc, 0, 0, g_thumb.size, g_thumb.size,
               blend);
}

/* ------------------------------------------------------------------ */
/* Controls                                                           */
/* ------------------------------------------------------------------ */

static void paint_track(CardLayout *layout, HDC dc, int left, int right, int center_y,
                        int position, int minimum, int maximum)
{
    int half = scale_px(2, layout->dpi);
    int radius = scale_px(2, layout->dpi);
    int center = left + MulDiv(right - left, position - minimum, maximum - minimum);

    RECT track_rect = {left, center_y - half, right, center_y + half};
    fill_round_rect(dc, track_rect, radius, COL_TRACK, COL_TRACK);

    int active_right = center > left + radius ? center : left + radius;
    RECT active_rect = {left, center_y - half, active_right, center_y + half};
    fill_round_rect(dc, active_rect, radius, COL_ACCENT, COL_ACCENT);

    draw_slider_thumb(layout, dc, center, center_y);
}

static void paint_button(CardLayout *layout, HDC dc, RECT rect, int id, UINT state, int y_offset)
{
    bool primary = id == CARD_ID_DONE;
    bool hot = g_controls.hot_id == id;
    bool pressed = (state & ODS_SELECTED) != 0;

    if (primary) {
        COLORREF fill = pressed ? COL_ACCENT_PRESS : COL_ACCENT;
        COLORREF top = pressed ? RGB(0x24, 0x2A, 0x36) : RGB(0x33, 0x3B, 0x4A);
        int radius = scale_px(10, layout->dpi);
        fill_round_rect(dc, rect, radius, top, fill);
        draw_text(layout, dc, L"完成", rect, 13, FW_MEDIUM, RGB(255, 255, 255), DT_CENTER, fill, UI_FACE);
        return;
    }

    fill_surface(dc, layout, rect, y_offset);
    if (hot || pressed) {
        int radius = scale_px(8, layout->dpi);
        COLORREF hot_fill = pressed ? COL_PRESS_BG : COL_HOVER_BG;
        fill_round_rect(dc, rect, radius, hot_fill, hot_fill);
    }

    /* One icon family for all three controls: the values below are tuned so
     * the ink heights match at every DPI (the glyphs do not share one box). */
    COLORREF ink = (hot || pressed) ? COL_ICON_HOT : COL_ICON;
    if (id == CARD_ID_CLOSE) {
        draw_system_icon(layout, dc, rect, L"\xE8BB", 13, ink);   /* ChromeClose */
    } else if (id == CARD_ID_BACK) {
        draw_system_icon(layout, dc, rect, L"\xE76B", 13, ink);   /* ChevronLeft */
    } else {
        draw_system_icon(layout, dc, rect, L"\xE713", 16, ink);   /* Setting */
    }
}

static void paint_slider(CardLayout *layout, HDC dc, RECT client, int y_offset,
                         int position, int minimum, int maximum)
{
    /* Paint the parent gradient instead of a flat patch; the trackbar's own
     * invalidation only covers the new thumb, so the whole client is redrawn. */
    fill_surface(dc, layout, client, y_offset);
    int center_y = (client.top + client.bottom) / 2;
    int pad = scale_px(TRACK_PAD_PX, layout->dpi);
    paint_track(layout, dc, client.left + pad, client.right - pad, center_y,
                position, minimum, maximum);
}

static void slider_draw(HWND window, HDC dc, App *app)
{
    RECT client;
    GetClientRect(window, &client);
    int minimum = (int)SendMessageW(window, TBM_GETRANGEMIN, 0, 0);
    int maximum = (int)SendMessageW(window, TBM_GETRANGEMAX, 0, 0);
    int position = (int)SendMessageW(window, TBM_GETPOS, 0, 0);
    POINT origin = {0, 0};
    MapWindowPoints(window, GetParent(window), &origin, 1);
    paint_slider(&g_layout, dc, client, origin.y, position, minimum, maximum);
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
        /* The custom track has no focus ring; the thumb itself is the cue.
         * Swallowing this keeps the platform from painting a dotted box. */
        InvalidateRect(window, NULL, TRUE);
        return 0;
    case WM_KILLFOCUS:
    case WM_THEMECHANGED:
    case WM_UPDATEUISTATE:
        InvalidateRect(window, NULL, TRUE);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

/* Hover feedback for the owner drawn buttons. */
static int control_slot(int id)
{
    switch (id) {
    case CARD_ID_SETTINGS: return 0;
    case CARD_ID_CLOSE: return 1;
    case CARD_ID_BACK: return 2;
    case CARD_ID_DONE: return 3;
    default: return -1;
    }
}

static LRESULT CALLBACK button_subclass_proc(HWND window, UINT message, WPARAM wparam,
                                             LPARAM lparam, UINT_PTR subclass_id,
                                             DWORD_PTR reference)
{
    int id = (int)subclass_id;
    int slot = control_slot(id);
    (void)reference;
    switch (message) {
    case WM_MOUSEMOVE:
        if (slot >= 0 && !g_controls.tracking[slot]) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            g_controls.tracking[slot] = true;
        }
        if (g_controls.hot_id != id) {
            g_controls.hot_id = id;
            InvalidateRect(window, NULL, TRUE);
        }
        break;
    case WM_MOUSELEAVE:
        if (slot >= 0) g_controls.tracking[slot] = false;
        if (g_controls.hot_id == id) {
            g_controls.hot_id = 0;
            InvalidateRect(window, NULL, TRUE);
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

static void paint_text(CardLayout *layout, HDC dc, App *app, bool snapshot)
{
    if (!app->card_settings) {
        if (snapshot) {
            paint_button(layout, dc, scaled_rect(layout, CARD_SETTINGS_X, CARD_ICON_Y,
                                                 CARD_ICON_PX, CARD_ICON_PX), CARD_ID_SETTINGS, 0, 0);
            paint_button(layout, dc, scaled_rect(layout, CARD_CLOSE_X, CARD_ICON_Y,
                                                 CARD_ICON_PX, CARD_ICON_PX), CARD_ID_CLOSE, 0, 0);
        }
        draw_text(layout, dc, APP_TITLE, scaled_rect(layout, 16, 0, 180, CARD_HEADER_PX),
                  13, FW_NORMAL, COL_TITLE, DT_LEFT, CLR_NONE, UI_FACE);
        return;
    }

    if (snapshot) {
        paint_button(layout, dc, scaled_rect(layout, CARD_BACK_X, CARD_ICON_Y,
                                             CARD_ICON_PX, CARD_ICON_PX), CARD_ID_BACK, 0, 0);
        paint_button(layout, dc, scaled_rect(layout, CARD_CLOSE_X, CARD_ICON_Y,
                                             CARD_ICON_PX, CARD_ICON_PX), CARD_ID_CLOSE, 0, 0);
    }
    draw_text(layout, dc, L"设置", scaled_rect(layout, 40, 0, 180, CARD_HEADER_PX),
              15, FW_NORMAL, COL_TITLE, DT_LEFT, CLR_NONE, UI_FACE);

    for (int i = 0; i < 2; i++) {
        int y = i ? FIELD_TWO_Y : FIELD_ONE_Y;
        int percent = (int)InterlockedCompareExchange(i ? &app->scroll_speed_percent
                                                        : &app->mouse_speed_percent, 0, 0);
        if (percent < SPEED_MIN_PERCENT || percent > SPEED_MAX_PERCENT) percent = 100;
        wchar_t value[16];
        _snwprintf_s(value, 16, _TRUNCATE, L"%d.%d×", percent / 100, (percent % 100) / 10);

        /* Weight 500/600 maps onto the Bold face here (the family ships 400
         * and 700 only), so labels stay at 400 and hierarchy comes from size
         * and colour instead of a synthetic semi-bold. */
        draw_text(layout, dc, i ? L"滚动倍率" : L"鼠标灵敏度",
                  scaled_rect(layout, 20, y, 140, FIELD_TEXT_H), 13, FW_NORMAL, COL_TEXT, DT_LEFT, CLR_NONE, UI_FACE);
        draw_text(layout, dc, value,
                  scaled_rect(layout, 142, y - 1, FIELD_VALUE_W, FIELD_TEXT_H + 2),
                  15, FW_NORMAL, COL_TITLE, DT_RIGHT, CLR_NONE, UI_FACE_NUM);
        draw_text(layout, dc, L"0.5×", scaled_rect(layout, 20, y + FIELD_TICKS_DY, 60, 16),
                  11, FW_NORMAL, COL_TICKS, DT_LEFT, CLR_NONE, UI_FACE_NUM);
        draw_text(layout, dc, L"3×", scaled_rect(layout, 202, y + FIELD_TICKS_DY, 60, 16),
                  11, FW_NORMAL, COL_TICKS, DT_RIGHT, CLR_NONE, UI_FACE_NUM);

        if (snapshot) {
            int center_y = scale_px(y + FIELD_TRACK_DY, layout->dpi);
            paint_track(layout, dc, scale_px(20, layout->dpi), scale_px(262, layout->dpi),
                        center_y, percent / SPEED_STEP_PERCENT,
                        SPEED_MIN_PERCENT / SPEED_STEP_PERCENT,
                        SPEED_MAX_PERCENT / SPEED_STEP_PERCENT);
        }
    }

    if (snapshot) {
        paint_button(layout, dc, scaled_rect(layout, DONE_X, DONE_Y, DONE_W, DONE_H),
                     CARD_ID_DONE, 0, 0);
    }
}

static void paint_card(App *app, CardLayout *layout, CardSurface *surface, bool snapshot)
{
    memset(surface->bits, 0, (size_t)surface->width * (size_t)surface->height * 4);
    unsigned char *pixels = (unsigned char *)surface->bits;

    paint_background(layout, pixels);
    if (!app->card_settings) {
        paint_tile(layout, pixels);
        paint_qr(layout, surface->dc, app);
    }
    paint_text(layout, surface->dc, app, snapshot);

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
/* Window shape                                                       */
/* ------------------------------------------------------------------ */

/* A window region is a 1-bit mask, so a rounded corner comes out as a
 * staircase. Windows 11 can round the window itself, antialiased; only fall
 * back to a region when that call is unavailable (Windows 10). */
static bool enable_dwm_corners(HWND window)
{
    typedef HRESULT(WINAPI * DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    static DwmSetWindowAttributeFn set_attribute;
    static bool resolved;

    if (!resolved) {
        resolved = true;
        HMODULE library = LoadLibraryW(L"dwmapi.dll");
        if (library) {
            set_attribute =
                (DwmSetWindowAttributeFn)(void *)GetProcAddress(library, "DwmSetWindowAttribute");
        }
    }
    if (!set_attribute) {
        return false;
    }
    /* 33 = DWMWA_WINDOW_CORNER_PREFERENCE, 2 = DWMWCP_ROUND */
    int preference = 2;
    return SUCCEEDED(set_attribute(window, 33, &preference, sizeof(preference)));
}

static void apply_window_shape(App *app, CardLayout *layout)
{
    if (!app->card) {
        return;
    }
    g_corners_by_dwm = enable_dwm_corners(app->card);
    if (g_corners_by_dwm) {
        /* Let DWM clip; a leftover region would win and bring the staircase back. */
        SetWindowRgn(app->card, NULL, TRUE);
        return;
    }
    HRGN region = CreateRoundRectRgn(0, 0, layout->win_w + 1, layout->win_h + 1,
                                     layout->radius * 2, layout->radius * 2);
    if (region && !SetWindowRgn(app->card, region, TRUE)) DeleteObject(region);
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
    /* The gear and the back arrow are the same affordance; never both. */
    ShowWindow(g_controls.settings, app->card_settings ? SW_HIDE : SW_SHOWNA);
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
        {g_controls.settings, CARD_SETTINGS_X, CARD_ICON_Y, CARD_ICON_PX, CARD_ICON_PX},
        {g_controls.close, CARD_CLOSE_X, CARD_ICON_Y, CARD_ICON_PX, CARD_ICON_PX},
        {g_controls.back, CARD_BACK_X, CARD_ICON_Y, CARD_ICON_PX, CARD_ICON_PX},
        {g_controls.done, DONE_X, DONE_Y, DONE_W, DONE_H},
        {g_controls.mouse, DONE_X - TRACK_PAD_PX,
         FIELD_ONE_Y + FIELD_TRACK_DY - TRACK_WINDOW_H / 2,
         DONE_W + TRACK_PAD_PX * 2, TRACK_WINDOW_H},
        {g_controls.scroll, DONE_X - TRACK_PAD_PX,
         FIELD_TWO_Y + FIELD_TRACK_DY - TRACK_WINDOW_H / 2,
         DONE_W + TRACK_PAD_PX * 2, TRACK_WINDOW_H},
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
    SendMessageW(g_controls.mouse, TBM_SETTHUMBLENGTH, scale_px(18, dpi), 0);
    SendMessageW(g_controls.scroll, TBM_SETTHUMBLENGTH, scale_px(18, dpi), 0);
    apply_window_shape(app, &g_layout);
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
        bool ready = build_layout(app, g_layout.dpi, &g_layout) &&
                     surface_create(&g_surface, g_layout.win_w, g_layout.win_h);
        int saved = 0;
        if (ready && message == WM_PRINTCLIENT) {
            /* DWM does the rounding on screen; a print target has no DWM, so
             * clip the corners here or captures come out with black corners. */
            saved = SaveDC(dc);
            HRGN clip = CreateRoundRectRgn(0, 0, g_layout.win_w + 1, g_layout.win_h + 1,
                                           g_layout.radius * 2, g_layout.radius * 2);
            if (clip) {
                SelectClipRgn(dc, clip);
                DeleteObject(clip);
            }
        }
        if (ready) {
            paint_card(app, &g_layout, &g_surface, false);
            BitBlt(dc, 0, 0, g_layout.win_w, g_layout.win_h, g_surface.dc, 0, 0, SRCCOPY);
        }
        if (saved) RestoreDC(dc, saved);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        int drag_left = app->card_settings ? scale_px(40, g_layout.dpi) : 0;
        if (point.y >= 0 && point.y < scale_px(CARD_HEADER_PX, g_layout.dpi) &&
            point.x >= drag_left && point.x < scale_px(205, g_layout.dpi)) return HTCAPTION;
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
            RECT value = scaled_rect(&g_layout, 142,
                                     (scroll ? FIELD_TWO_Y : FIELD_ONE_Y) - 1,
                                     FIELD_VALUE_W, FIELD_TEXT_H + 2);
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
            POINT origin = {0, 0};
            MapWindowPoints(item->hwndItem, window, &origin, 1);
            paint_button(&g_layout, item->hDC, item->rcItem, (int)item->CtlID,
                         item->itemState, origin.y);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        CardColor color = surface_color(0, g_layout.win_h);
        static HBRUSH background;
        if (background) DeleteObject(background);
        background = CreateSolidBrush(RGB(color.red, color.green, color.blue));
        SetBkColor((HDC)wparam, RGB(color.red, color.green, color.blue));
        return (LRESULT)background;
    }
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
        if (!SetWindowSubclass(*buttons[i].target, button_subclass_proc,
                               (UINT_PTR)buttons[i].id, (DWORD_PTR)app)) {
            card_destroy(app);
            return false;
        }
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
    if (g_controls.settings) RemoveWindowSubclass(g_controls.settings, button_subclass_proc, CARD_ID_SETTINGS);
    if (g_controls.close) RemoveWindowSubclass(g_controls.close, button_subclass_proc, CARD_ID_CLOSE);
    if (g_controls.back) RemoveWindowSubclass(g_controls.back, button_subclass_proc, CARD_ID_BACK);
    if (g_controls.done) RemoveWindowSubclass(g_controls.done, button_subclass_proc, CARD_ID_DONE);
    if (app->card) {
        DestroyWindow(app->card);
        app->card = NULL;
    }
    surface_destroy(&g_surface);
    round_cache_release();
    thumb_release();
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
    paint_card(app, &layout, &surface, true);

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
