#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_panel_io.h>

#include "board.h"
#include "display.h"
#include "model.h"

#include "fonts/FreeSans12pt7b.h"
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold18pt7b.h"

// The ESP32-C6 has no FPU, so everything per pixel is integer. Geometry is in Q4 (1/16 px).

#define W LCD_SIZE
#define H LCD_SIZE
#define CX (W / 2)
#define CY (H / 2)
#define STRIP_ROWS 8
#define STRIPS (H / STRIP_ROWS)
#define STRIP_PIXELS (W * STRIP_ROWS)
#define FRAME_MS 33   // at most ~30 frames/s while the knob turns
#define ANIM_MS 100   // pulsing/blinking marks redraw at 10 frames/s
#define POINTER_STEP (0.5f * DEG_TO_RAD)

// Dial geometry, radii in px.
// Marks are flat-ended bars; the pointer is a rounded needle.
#define MARK_R0 98
#define MARK_R1 117
#define MARK_HW 8.0f
#define SEL_R0 91
#define SEL_R1 118
#define SEL_HW 11.0f
#define PTR_R0 66
#define PTR_R1 84
#define PTR_HW 3.0f

// Text box in the middle, in unrotated ("logical") pixels; kept inside the pointer's circle.
#define TW 126
#define TH 86

static const uint16_t C_TEXT = 0xFFFF;
static const uint16_t C_DIM = 0x8C71;
static const uint16_t C_CLAUDE = 0xDB8A;
static const uint16_t C_POINTER = 0xFFFF;

// ---------------------------------------------------------------------------------------
// Panel IO

static esp_lcd_panel_io_handle_t io;
static uint16_t *strip_buf[2];
static volatile bool dma_busy = false;

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    dma_busy = false;
    return false;
}

static void cmd(uint8_t c, const uint8_t *data = nullptr, size_t len = 0)
{
    esp_lcd_panel_io_tx_param(io, c, data, len);
}

// GC9A01 vendor init (same sequence as Arduino_GFX's Arduino_GC9A01): {cmd, n, data...}.
static const uint8_t init_seq[] = {
    0xEF, 0, 0xEB, 1, 0x14, 0xFE, 0, 0xEF, 0, 0xEB, 1, 0x14, 0x84, 1, 0x40, 0x85, 1, 0xFF,
    0x86, 1, 0xFF, 0x87, 1, 0xFF, 0x88, 1, 0x0A, 0x89, 1, 0x21, 0x8A, 1, 0x00, 0x8B, 1, 0x80,
    0x8C, 1, 0x01, 0x8D, 1, 0x01, 0x8E, 1, 0xFF, 0x8F, 1, 0xFF, 0xB6, 2, 0x00, 0x20,
    0x3A, 1, 0x05, // 16-bit colour
    0x90, 4, 0x08, 0x08, 0x08, 0x08, 0xBD, 1, 0x06, 0xBC, 1, 0x00, 0xFF, 3, 0x60, 0x01, 0x04,
    0xC3, 1, 0x13, 0xC4, 1, 0x13, 0xC9, 1, 0x22, 0xBE, 1, 0x11, 0xE1, 2, 0x10, 0x0E,
    0xDF, 3, 0x21, 0x0C, 0x02,
    0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A, 0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
    0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A, 0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
    0xED, 2, 0x1B, 0x0B, 0xAE, 1, 0x77, 0xCD, 1, 0x63,
    0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03, 0xE8, 1, 0x34,
    0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70,
    0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70,
    0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
    0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00,
    0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98,
    0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00, 0x98, 2, 0x3E, 0x07,
    0x35, 0,       // tearing effect line on
    0x36, 1, 0x08, // MADCTL: BGR panel
    0x21, 0,       // inversion on (IPS)
};

static void panel_init()
{
    spi_bus_config_t bus = {};
    bus.sclk_io_num = LCD_SCLK;
    bus.mosi_io_num = LCD_MOSI;
    bus.miso_io_num = -1;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = STRIP_PIXELS * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t cfg = {};
    cfg.cs_gpio_num = -1; // tied to GND
    cfg.dc_gpio_num = LCD_DC;
    cfg.spi_mode = 0;
    cfg.pclk_hz = LCD_SPI_HZ;
    cfg.trans_queue_depth = 4;
    cfg.on_color_trans_done = on_trans_done;
    cfg.lcd_cmd_bits = 8;
    cfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &cfg, &io));

    cmd(0x01); // software reset (RST is tied high)
    delay(120);
    for (size_t i = 0; i < sizeof(init_seq);) {
        uint8_t c = init_seq[i], n = init_seq[i + 1];
        cmd(c, &init_seq[i + 2], n);
        i += 2 + n;
    }
    cmd(0x11); // sleep out
    delay(120);
    cmd(0x29); // display on
}

// ---------------------------------------------------------------------------------------
// Scene

struct Capsule {
    int32_t ax, ay, bx, by; // end points, Q4
    int32_t ux, uy;         // unit direction a->b, Q14
    int32_t len, hw;        // Q4
    int16_t x0, y0, x1, y1; // bounding box, px
    uint16_t color;
    bool flat; // square ends instead of round caps
};

#define MAX_CAPS (MAX_SESSIONS + 1)
static Capsule caps[MAX_CAPS];
static int ncaps = 0;

// Rotation from logical to panel, Q14.
static int32_t rot_c, rot_s;

// Text: 8-bit coverage in logical pixels, one colour per row.
static uint8_t text_cov[TH][TW];
static uint16_t text_color[TH];
static int16_t text_x0, text_y0, text_x1, text_y1; // panel bounding box of the rotated text box

// What display_update() saw last; a frame renders it.
struct Inputs {
    bool hostSeen, highlight;
    int count, sel;
    SessionState states[MAX_SESSIONS];
    char name[sizeof(Session::name)];
    float pointer, origin, width;
};
static Inputs in, drawn;
static bool drawn_valid = false, text_valid = false;
static char text_key[64]; // what text_cov holds

static uint16_t state_color(SessionState s)
{
    switch (s) {
    case ST_RUNNING: return 0x2D7F; // blue
    case ST_PENDING: return 0xFD20; // amber: waiting for you
    case ST_DONE: return 0x2E8B;    // green
    default: return 0x6B4D;         // grey
    }
}

static const char *state_label(SessionState s)
{
    switch (s) {
    case ST_RUNNING: return "working";
    case ST_PENDING: return "needs you";
    case ST_DONE: return "done";
    default: return "idle";
    }
}

static uint16_t scale565(uint16_t c, int k /* 0..256 */)
{
    int r = (c >> 11) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
    return (r << 11) | (g << 5) | b;
}

static bool any_animated()
{
    if (!in.hostSeen) return false;
    for (int i = 0; i < in.count; i++)
        if (in.states[i] == ST_RUNNING || in.states[i] == ST_PENDING) return true;
    return false;
}

// Brightness of a mark (0..256) for its state, at time t.
static int mark_level(SessionState s, uint32_t t)
{
    switch (s) {
    case ST_PENDING: return (t / 250) % 2 ? 256 : 70;
    case ST_RUNNING: {
        int ph = t % 1400;
        int tri = ph < 700 ? ph : 1400 - ph; // 0..700
        return 140 + tri * 116 / 700;
    }
    default: return 256;
    }
}

// Panel position (Q4) of a point at radius r on physical angle theta (radians, clockwise from up).
static void polar(float theta, float r, int32_t *x, int32_t *y)
{
    float a = theta + LCD_ROTATE_DEG * DEG_TO_RAD;
    *x = (int32_t)((CX + r * sinf(a)) * 16);
    *y = (int32_t)((CY - r * cosf(a)) * 16);
}

static void add_capsule(float theta, float r0, float r1, float hw, uint16_t color, bool flat = true)
{
    if (ncaps >= MAX_CAPS) return;
    Capsule &c = caps[ncaps++];
    polar(theta, r0, &c.ax, &c.ay);
    polar(theta, r1, &c.bx, &c.by);
    float a = theta + LCD_ROTATE_DEG * DEG_TO_RAD;
    c.ux = (int32_t)(sinf(a) * 16384);
    c.uy = (int32_t)(-cosf(a) * 16384);
    c.len = (int32_t)((r1 - r0) * 16);
    c.hw = (int32_t)(hw * 16);
    int pad = (int)hw + 2;
    c.x0 = max(0, (int)(min(c.ax, c.bx) >> 4) - pad);
    c.x1 = min(W - 1, (int)(max(c.ax, c.bx) >> 4) + pad);
    c.y0 = max(0, (int)(min(c.ay, c.by) >> 4) - pad);
    c.y1 = min(H - 1, (int)(max(c.ay, c.by) >> 4) + pad);
    c.color = color;
    c.flat = flat;
}

// Dial angle of a knob angle measured from detent 0: session 1 sits at 12 o'clock and the
// rest follow clockwise, like hours on a watch.
static float to_screen(float a) { return DISPLAY_ANGLE_SIGN * a + DISPLAY_ANGLE_OFFSET_DEG * DEG_TO_RAD; }

// --- text -------------------------------------------------------------------------------
// Fonts are drawn at twice the target size and box-filtered down, which anti-aliases them.

static int text_width(const GFXfont *f, const char *s, int n = -1)
{
    int w = 0;
    for (int i = 0; s[i] && (n < 0 || i < n); i++) {
        uint8_t ch = s[i];
        if (ch < f->first || ch > f->last) ch = '?';
        w += f->glyph[ch - f->first].xAdvance;
    }
    return w / 2;
}

static void draw_string(const GFXfont *f, const char *s, int n, int baseline)
{
    int x2 = (TW - text_width(f, s, n)) / 2 * 2; // cursor in 2x pixels
    int y2 = baseline * 2;
    for (int i = 0; s[i] && i < n; i++) {
        uint8_t ch = s[i];
        if (ch < f->first || ch > f->last) ch = '?';
        const GFXglyph &g = f->glyph[ch - f->first];
        const uint8_t *bits = f->bitmap + g.bitmapOffset;
        int bit = 0;
        for (int gy = 0; gy < g.height; gy++)
            for (int gx = 0; gx < g.width; gx++, bit++) {
                if (!(bits[bit >> 3] & (0x80 >> (bit & 7)))) continue;
                int px = (x2 + g.xOffset + gx) >> 1, py = (y2 + g.yOffset + gy) >> 1;
                if (px < 0 || px >= TW || py < 0 || py >= TH) continue;
                text_cov[py][px] = min(255, text_cov[py][px] + 64);
            }
        x2 += g.xAdvance;
    }
}

struct Line {
    const GFXfont *font;
    const char *s;
    int n;
    uint16_t color;
};

// Splits a session name over at most two lines that fit in `w` px.
static int wrap_name(const char *name, Line *out, int w)
{
    const GFXfont *f = &FreeSansBold18pt7b;
    int len = strlen(name);
    if (text_width(f, name) <= w) {
        out[0] = {f, name, len, C_TEXT};
        return 1;
    }
    int fit = 0;
    while (fit < len && text_width(f, name, fit + 1) <= w) fit++;
    int cut = fit;
    for (int i = fit; i > fit / 2; i--)
        if (name[i] == ' ' || name[i] == '-' || name[i] == '_' || name[i] == '/') {
            cut = name[i] == ' ' ? i : i + 1;
            break;
        }
    out[0] = {f, name, cut, C_TEXT};
    const char *rest = name + cut;
    while (*rest == ' ') rest++;
    int rn = strlen(rest);
    while (rn > 0 && text_width(f, rest, rn) > w) rn--;
    out[1] = {f, rest, rn, C_TEXT};
    return 2;
}

static void build_text()
{
    Line lines[5];
    int n = 0;
    char idx[16];
    if (!in.hostSeen) {
        lines[n++] = {&FreeSansBold18pt7b, "Claude Knob", -1, C_CLAUDE};
        lines[n++] = {&FreeSans12pt7b, "waiting for knobd", -1, C_DIM};
    } else if (in.count == 0) {
        lines[n++] = {&FreeSansBold12pt7b, "no sessions", -1, C_DIM};
    } else {
        snprintf(idx, sizeof(idx), "%d / %d", in.sel + 1, in.count);
        lines[n++] = {&FreeSans12pt7b, idx, -1, C_DIM};
        n += wrap_name(in.name, &lines[n], TW - 8);
        SessionState s = in.states[in.sel];
        lines[n++] = {&FreeSansBold12pt7b, state_label(s), -1, state_color(s)};
    }

    char key[sizeof(text_key)];
    snprintf(key, sizeof(key), "%d|%s|%s|%d", n, lines[0].s, n > 1 ? lines[1].s : "",
             in.hostSeen && in.count ? (int)in.states[in.sel] : -1);
    if (text_valid && !strcmp(key, text_key)) return;
    strlcpy(text_key, key, sizeof(text_key));
    text_valid = true;

    memset(text_cov, 0, sizeof(text_cov));
    memset(text_color, 0, sizeof(text_color));
    int total = 0;
    for (int i = 0; i < n; i++) total += lines[i].font->yAdvance / 2;
    int y = (TH - total) / 2;
    for (int i = 0; i < n; i++) {
        int lh = lines[i].font->yAdvance / 2;
        int nn = lines[i].n < 0 ? strlen(lines[i].s) : lines[i].n;
        draw_string(lines[i].font, lines[i].s, nn, y + lh * 3 / 4);
        for (int r = max(0, y); r < min(TH, y + lh); r++) text_color[r] = lines[i].color;
        y += lh;
    }
}

static void build_scene(uint32_t t)
{
    ncaps = 0;
    if (in.hostSeen) {
        for (int k = 0; k < in.count; k++) {
            float theta = to_screen(k * in.width);
            uint16_t c = scale565(state_color(in.states[k]), mark_level(in.states[k], t));
            if (in.highlight && k == in.sel) add_capsule(theta, SEL_R0, SEL_R1, SEL_HW, c);
            else add_capsule(theta, MARK_R0, MARK_R1, MARK_HW, scale565(c, 200));
        }
    }
    add_capsule(to_screen(in.pointer - in.origin), PTR_R0, PTR_R1, PTR_HW, C_POINTER, false);
    build_text();
}

// ---------------------------------------------------------------------------------------
// Rasterizer

static inline uint16_t blend(uint16_t d, uint16_t s, int a /* 0..256 */)
{
    int dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
    int sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31;
    dr += (sr - dr) * a >> 8;
    dg += (sg - dg) * a >> 8;
    db += (sb - db) * a >> 8;
    return (dr << 11) | (dg << 5) | db;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1UL << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

static void raster_capsule(const Capsule &c, uint16_t *buf, int y0)
{
    int ya = max((int)c.y0, y0), yb = min((int)c.y1, y0 + STRIP_ROWS - 1);
    for (int y = ya; y <= yb; y++) {
        uint16_t *row = buf + (y - y0) * W;
        int32_t py = (y << 4) + 8 - c.ay;
        for (int x = c.x0; x <= c.x1; x++) {
            int32_t px = (x << 4) + 8 - c.ax;
            int32_t along = (px * c.ux + py * c.uy) >> 14;
            int32_t d;
            if (c.flat) {
                // Box: distance past the nearest edge, sides or ends.
                int32_t perp = abs((py * c.ux - px * c.uy) >> 14);
                int32_t past = max(-along, along - c.len) + c.hw; // ends sit hw outside the box test
                d = max(perp, past);
            } else if (along <= 0) {
                d = isqrt(px * px + py * py);
            } else if (along >= c.len) {
                int32_t qx = (x << 4) + 8 - c.bx, qy = (y << 4) + 8 - c.by;
                d = isqrt(qx * qx + qy * qy);
            } else {
                d = abs((py * c.ux - px * c.uy) >> 14);
            }
            int32_t a = (c.hw + 8 - d) * 16;
            if (a <= 0) continue;
            row[x] = a >= 256 ? c.color : blend(row[x], c.color, a);
        }
    }
}

static void raster_text(uint16_t *buf, int y0)
{
    int ya = max((int)text_y0, y0), yb = min((int)text_y1, y0 + STRIP_ROWS - 1);
    for (int y = ya; y <= yb; y++) {
        uint16_t *row = buf + (y - y0) * W;
        int32_t dy = (y - CY) * 256 + 128;
        for (int x = text_x0; x <= text_x1; x++) {
            int32_t dx = (x - CX) * 256 + 128;
            // panel -> logical (inverse rotation), Q8, relative to the text box's corner
            int32_t u = ((dx * rot_c + dy * rot_s) >> 14) + TW * 128 - 128;
            int32_t v = ((dy * rot_c - dx * rot_s) >> 14) + TH * 128 - 128;
            int ui = u >> 8, vi = v >> 8;
            if (ui < -1 || ui >= TW || vi < -1 || vi >= TH) continue;
            int fu = u & 255, fv = v & 255;
            int c00 = 0, c10 = 0, c01 = 0, c11 = 0;
            bool l = ui >= 0, r = ui + 1 < TW, t = vi >= 0, b = vi + 1 < TH;
            if (t && l) c00 = text_cov[vi][ui];
            if (t && r) c10 = text_cov[vi][ui + 1];
            if (b && l) c01 = text_cov[vi + 1][ui];
            if (b && r) c11 = text_cov[vi + 1][ui + 1];
            int top = c00 * 256 + (c10 - c00) * fu, bot = c01 * 256 + (c11 - c01) * fu;
            int cov = (top * 256 + (bot - top) * fv) >> 16; // 0..255
            if (cov < 4) continue;
            int vr = constrain((v + 128) >> 8, 0, TH - 1);
            row[x] = blend(row[x], text_color[vr], cov + (cov >> 7));
        }
    }
}

static void render_strip(int strip, uint16_t *buf)
{
    int y0 = strip * STRIP_ROWS;
    memset(buf, 0, STRIP_PIXELS * 2);
    raster_text(buf, y0);
    for (int i = 0; i < ncaps; i++) raster_capsule(caps[i], buf, y0);
    for (int i = 0; i < STRIP_PIXELS; i++) buf[i] = __builtin_bswap16(buf[i]); // panel is big-endian
}

// ---------------------------------------------------------------------------------------
// Frame pipeline: render strip n+1 while strip n is on the wire.

static bool frame_active = false;
static int next_render = 0, next_send = 0;
static int ready_buf = -1;     // rendered, waiting to be sent
static int inflight_buf = -1;  // being sent
static uint32_t last_frame = 0, last_anim = 0;
static bool backlight_on = false;

static void send_strip(int b)
{
    if (next_send == 0) {
        uint8_t win[4] = {0, 0, (W - 1) >> 8, (W - 1) & 0xFF};
        cmd(0x2A, win, 4);
        win[3] = (H - 1) & 0xFF;
        win[2] = (H - 1) >> 8;
        cmd(0x2B, win, 4);
    }
    dma_busy = true;
    inflight_buf = b;
    esp_lcd_panel_io_tx_color(io, next_send == 0 ? 0x2C : 0x3C, strip_buf[b], STRIP_PIXELS * 2);
    next_send++;
}

void display_init()
{
    ledcAttach(LCD_BL, 5000, 8);
    ledcWrite(LCD_BL, 0);

    float r = LCD_ROTATE_DEG * DEG_TO_RAD;
    rot_c = (int32_t)(cosf(r) * 16384);
    rot_s = (int32_t)(sinf(r) * 16384);
    // Bounding box of the rotated text box.
    float hx = TW / 2.0f, hy = TH / 2.0f;
    float ex = fabsf(hx * cosf(r)) + fabsf(hy * sinf(r)), ey = fabsf(hx * sinf(r)) + fabsf(hy * cosf(r));
    text_x0 = max(0, (int)(CX - ex) - 1);
    text_x1 = min(W - 1, (int)(CX + ex) + 1);
    text_y0 = max(0, (int)(CY - ey) - 1);
    text_y1 = min(H - 1, (int)(CY + ey) + 1);

    for (int i = 0; i < 2; i++) strip_buf[i] = (uint16_t *)heap_caps_malloc(STRIP_PIXELS * 2, MALLOC_CAP_DMA);
    panel_init();
}

void display_update(bool highlight, float pointer, float origin, float width)
{
    in.hostSeen = model.hostSeen;
    in.count = model.hostSeen ? model.count : 0;
    in.sel = in.count ? constrain(model.sel, 0, in.count - 1) : 0;
    in.highlight = highlight;
    for (int i = 0; i < in.count; i++) in.states[i] = model.items[i].state;
    strlcpy(in.name, in.count ? model.items[in.sel].name : "", sizeof(in.name));
    in.pointer = pointer;
    in.origin = origin;
    in.width = width;
}

static bool needs_frame(uint32_t now)
{
    if (!drawn_valid) return true;
    if (any_animated() && now - last_anim >= ANIM_MS) return true;
    if (fabsf((in.pointer - in.origin) - (drawn.pointer - drawn.origin)) >= POINTER_STEP) return true;
    if (in.width != drawn.width) return true;
    if (in.hostSeen != drawn.hostSeen || in.highlight != drawn.highlight || in.count != drawn.count ||
        in.sel != drawn.sel || strcmp(in.name, drawn.name))
        return true;
    return memcmp(in.states, drawn.states, sizeof(SessionState) * in.count) != 0;
}

void display_step()
{
    if (!io || !strip_buf[0] || !strip_buf[1]) return;
    uint32_t now = millis();

    if (!frame_active) {
        if (now - last_frame < FRAME_MS || !needs_frame(now)) return;
        build_scene(now);
        drawn = in;
        drawn_valid = true;
        if (any_animated()) last_anim = now;
        last_frame = now;
        frame_active = true;
        next_render = next_send = 0;
        ready_buf = inflight_buf = -1;
    }

    if (inflight_buf >= 0 && !dma_busy) inflight_buf = -1;

    // Send the rendered strip as soon as the wire is free.
    if (ready_buf >= 0 && inflight_buf < 0) {
        int b = ready_buf;
        ready_buf = -1;
        send_strip(b);
    }
    // Render the next strip into the buffer that isn't in use (one per call keeps it short).
    if (ready_buf < 0 && next_render < STRIPS) {
        int b = inflight_buf == 0 ? 1 : 0;
        render_strip(next_render++, strip_buf[b]);
        ready_buf = b;
        if (inflight_buf < 0) {
            ready_buf = -1;
            send_strip(b);
        }
    }

    if (next_send == STRIPS && inflight_buf < 0 && ready_buf < 0) {
        frame_active = false;
        if (!backlight_on) {
            ledcWrite(LCD_BL, 255);
            backlight_on = true;
        }
    }
}
