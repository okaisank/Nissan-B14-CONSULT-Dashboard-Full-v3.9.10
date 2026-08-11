#include "dashboard.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9488.h"

static const char *TAG = "B14_TFT";

#define TFT_HOST        SPI2_HOST
#define TFT_PIN_MOSI    11
#define TFT_PIN_MISO    13
#define TFT_PIN_SCLK    12
#define TFT_PIN_CS      10
#define TFT_PIN_DC       9
#define TFT_PIN_RST      8
#define TFT_PIN_BL      18
#define TFT_BL_ACTIVE_LEVEL 1

/* v3.9.10 POWER-ON DISPLAY FIX
 * Keep the backlight dark while the LCD rail settles, force CS inactive,
 * hold the ILI9488 in hardware reset, then release reset before SPI init.
 * This avoids the white-screen race seen when TFT and ESP32 power up together. */
#define TFT_POWER_STABILIZE_MS   350
#define TFT_RESET_RELEASE_MS     180
#define TFT_PANEL_RESET_WAIT_MS  180
#define TFT_PANEL_INIT_WAIT_MS   120
#define TFT_INIT_RETRY_COUNT       3

#define TFT_H_RES      480
#define TFT_V_RES      320
#define TFT_CLOCK_HZ   (10 * 1000 * 1000)
#define STRIP_LINES     40
#define STRIP_PIXELS    (TFT_H_RES * STRIP_LINES)
#define TFT_MAX_XFER    (STRIP_PIXELS * 3 + 64)

#define RGB565(r,g,b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define C_BLACK       RGB565(0,0,0)
#define C_WHITE       RGB565(255,255,255)
#define C_BG          RGB565(7,10,18)
#define C_PANEL       RGB565(16,22,34)
#define C_PANEL2      RGB565(24,31,47)
#define C_STROKE      RGB565(52,64,92)
#define C_MUTED       RGB565(150,160,178)
#define C_CYAN        RGB565(70,210,255)
#define C_BLUE        RGB565(44,114,255)
#define C_GREEN       RGB565(52,232,120)
#define C_YELLOW      RGB565(255,220,80)
#define C_ORANGE      RGB565(255,157,43)
#define C_RED         RGB565(255,78,78)
#define C_PURPLE      RGB565(164,104,255)
#define C_ACCENT      RGB565(110,130,255)

static bool s_ready = false;
static volatile dash_page_t s_page = DASH_PAGE_MAIN;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_tx_done = NULL;
static SemaphoreHandle_t s_draw_mutex = NULL;
static uint16_t *s_strip = NULL;
static int64_t s_last_render_us = 0;
static volatile bool s_page_dirty = true;

/* ----------------------------- 5x7 ASCII font ----------------------------- */
static void glyph5x7(char ch, uint8_t g[5])
{
    memset(g, 0, 5);
    unsigned char c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = (unsigned char)toupper(c);
    switch (c) {
        case ' ': break;
        case '!': g[2]=0x5F; break;
        case '"': g[1]=0x07; g[3]=0x07; break;
        case '#': g[0]=0x14; g[1]=0x7F; g[2]=0x14; g[3]=0x7F; g[4]=0x14; break;
        case '%': g[0]=0x23; g[1]=0x13; g[2]=0x08; g[3]=0x64; g[4]=0x62; break;
        case '&': g[0]=0x36; g[1]=0x49; g[2]=0x55; g[3]=0x22; g[4]=0x50; break;
        case '\'': g[2]=0x03; break;
        case '(': g[1]=0x1C; g[2]=0x22; g[3]=0x41; break;
        case ')': g[1]=0x41; g[2]=0x22; g[3]=0x1C; break;
        case '*': g[0]=0x14; g[1]=0x08; g[2]=0x3E; g[3]=0x08; g[4]=0x14; break;
        case '+': g[0]=0x08; g[1]=0x08; g[2]=0x3E; g[3]=0x08; g[4]=0x08; break;
        case ',': g[1]=0x80; g[2]=0x60; break;
        case '-': g[0]=0x08; g[1]=0x08; g[2]=0x08; g[3]=0x08; g[4]=0x08; break;
        case '.': g[1]=0x60; g[2]=0x60; break;
        case '/': g[0]=0x20; g[1]=0x10; g[2]=0x08; g[3]=0x04; g[4]=0x02; break;
        case '0': g[0]=0x3E; g[1]=0x51; g[2]=0x49; g[3]=0x45; g[4]=0x3E; break;
        case '1': g[0]=0x00; g[1]=0x42; g[2]=0x7F; g[3]=0x40; g[4]=0x00; break;
        case '2': g[0]=0x42; g[1]=0x61; g[2]=0x51; g[3]=0x49; g[4]=0x46; break;
        case '3': g[0]=0x21; g[1]=0x41; g[2]=0x45; g[3]=0x4B; g[4]=0x31; break;
        case '4': g[0]=0x18; g[1]=0x14; g[2]=0x12; g[3]=0x7F; g[4]=0x10; break;
        case '5': g[0]=0x27; g[1]=0x45; g[2]=0x45; g[3]=0x45; g[4]=0x39; break;
        case '6': g[0]=0x3C; g[1]=0x4A; g[2]=0x49; g[3]=0x49; g[4]=0x30; break;
        case '7': g[0]=0x01; g[1]=0x71; g[2]=0x09; g[3]=0x05; g[4]=0x03; break;
        case '8': g[0]=0x36; g[1]=0x49; g[2]=0x49; g[3]=0x49; g[4]=0x36; break;
        case '9': g[0]=0x06; g[1]=0x49; g[2]=0x49; g[3]=0x29; g[4]=0x1E; break;
        case ':': g[1]=0x36; g[2]=0x36; break;
        case ';': g[1]=0x80; g[2]=0x76; break;
        case '<': g[0]=0x08; g[1]=0x14; g[2]=0x22; g[3]=0x41; break;
        case '=': g[0]=0x14; g[1]=0x14; g[2]=0x14; g[3]=0x14; g[4]=0x14; break;
        case '>': g[1]=0x41; g[2]=0x22; g[3]=0x14; g[4]=0x08; break;
        case '?': g[0]=0x02; g[1]=0x01; g[2]=0x51; g[3]=0x09; g[4]=0x06; break;
        case '@': g[0]=0x32; g[1]=0x49; g[2]=0x79; g[3]=0x41; g[4]=0x3E; break;
        case 'A': g[0]=0x7E; g[1]=0x11; g[2]=0x11; g[3]=0x11; g[4]=0x7E; break;
        case 'B': g[0]=0x7F; g[1]=0x49; g[2]=0x49; g[3]=0x49; g[4]=0x36; break;
        case 'C': g[0]=0x3E; g[1]=0x41; g[2]=0x41; g[3]=0x41; g[4]=0x22; break;
        case 'D': g[0]=0x7F; g[1]=0x41; g[2]=0x41; g[3]=0x22; g[4]=0x1C; break;
        case 'E': g[0]=0x7F; g[1]=0x49; g[2]=0x49; g[3]=0x49; g[4]=0x41; break;
        case 'F': g[0]=0x7F; g[1]=0x09; g[2]=0x09; g[3]=0x09; g[4]=0x01; break;
        case 'G': g[0]=0x3E; g[1]=0x41; g[2]=0x49; g[3]=0x49; g[4]=0x7A; break;
        case 'H': g[0]=0x7F; g[1]=0x08; g[2]=0x08; g[3]=0x08; g[4]=0x7F; break;
        case 'I': g[0]=0x00; g[1]=0x41; g[2]=0x7F; g[3]=0x41; g[4]=0x00; break;
        case 'J': g[0]=0x20; g[1]=0x40; g[2]=0x41; g[3]=0x3F; g[4]=0x01; break;
        case 'K': g[0]=0x7F; g[1]=0x08; g[2]=0x14; g[3]=0x22; g[4]=0x41; break;
        case 'L': g[0]=0x7F; g[1]=0x40; g[2]=0x40; g[3]=0x40; g[4]=0x40; break;
        case 'M': g[0]=0x7F; g[1]=0x02; g[2]=0x0C; g[3]=0x02; g[4]=0x7F; break;
        case 'N': g[0]=0x7F; g[1]=0x04; g[2]=0x08; g[3]=0x10; g[4]=0x7F; break;
        case 'O': g[0]=0x3E; g[1]=0x41; g[2]=0x41; g[3]=0x41; g[4]=0x3E; break;
        case 'P': g[0]=0x7F; g[1]=0x09; g[2]=0x09; g[3]=0x09; g[4]=0x06; break;
        case 'Q': g[0]=0x3E; g[1]=0x41; g[2]=0x51; g[3]=0x21; g[4]=0x5E; break;
        case 'R': g[0]=0x7F; g[1]=0x09; g[2]=0x19; g[3]=0x29; g[4]=0x46; break;
        case 'S': g[0]=0x46; g[1]=0x49; g[2]=0x49; g[3]=0x49; g[4]=0x31; break;
        case 'T': g[0]=0x01; g[1]=0x01; g[2]=0x7F; g[3]=0x01; g[4]=0x01; break;
        case 'U': g[0]=0x3F; g[1]=0x40; g[2]=0x40; g[3]=0x40; g[4]=0x3F; break;
        case 'V': g[0]=0x1F; g[1]=0x20; g[2]=0x40; g[3]=0x20; g[4]=0x1F; break;
        case 'W': g[0]=0x3F; g[1]=0x40; g[2]=0x38; g[3]=0x40; g[4]=0x3F; break;
        case 'X': g[0]=0x63; g[1]=0x14; g[2]=0x08; g[3]=0x14; g[4]=0x63; break;
        case 'Y': g[0]=0x07; g[1]=0x08; g[2]=0x70; g[3]=0x08; g[4]=0x07; break;
        case 'Z': g[0]=0x61; g[1]=0x51; g[2]=0x49; g[3]=0x45; g[4]=0x43; break;
        case '[': g[1]=0x7F; g[2]=0x41; g[3]=0x41; break;
        case '\\': g[0]=0x02; g[1]=0x04; g[2]=0x08; g[3]=0x10; g[4]=0x20; break;
        case ']': g[1]=0x41; g[2]=0x41; g[3]=0x7F; break;
        case '_': g[0]=0x40; g[1]=0x40; g[2]=0x40; g[3]=0x40; g[4]=0x40; break;
        case '|': g[2]=0x7F; break;
        default: g[0]=0x7F; g[1]=0x41; g[2]=0x49; g[3]=0x41; g[4]=0x7F; break;
    }
}

static bool lcd_color_done_cb(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t *edata,
                              void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    BaseType_t hp = pdFALSE;
    if (sem != NULL) xSemaphoreGiveFromISR(sem, &hp);
    return hp == pdTRUE;
}

static void drain_tx_sem(void)
{
    if (!s_tx_done) return;
    while (xSemaphoreTake(s_tx_done, 0) == pdTRUE) { }
}

static esp_err_t draw_bitmap_wait(int x1, int y1, int x2, int y2, const uint16_t *pixels)
{
    if (!s_panel || !pixels) return ESP_ERR_INVALID_STATE;
    drain_tx_sem();
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, pixels);
    if (err != ESP_OK) return err;
    if (s_tx_done && xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "LCD transfer timeout x=%d y=%d w=%d h=%d", x1, y1, x2-x1, y2-y1);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_strip || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > TFT_H_RES) w = TFT_H_RES - x;
    if (y + h > TFT_V_RES) h = TFT_V_RES - y;
    if (w <= 0 || h <= 0) return;

    int yy = y;
    int left = h;
    while (left > 0) {
        int lines = left > STRIP_LINES ? STRIP_LINES : left;
        size_t count = (size_t)w * (size_t)lines;
        for (size_t i = 0; i < count; ++i) s_strip[i] = color;
        if (draw_bitmap_wait(x, yy, x + w, yy + lines, s_strip) != ESP_OK) return;
        yy += lines;
        left -= lines;
    }
}

static void clear_screen(uint16_t color)
{
    fill_rect(0, 0, TFT_H_RES, TFT_V_RES, color);
}

static void render_text_into(uint16_t *buf, int bw, int bh, int x,
                             const char *text, int scale, uint16_t fg)
{
    if (!buf || !text || scale < 1) return;
    int pen_x = x;
    const int cw = 6 * scale;
    for (const char *p = text; *p; ++p) {
        if (pen_x + 5 * scale > bw) break;
        uint8_t g[5];
        glyph5x7(*p, g);
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if ((g[col] >> row) & 0x01) {
                    int px0 = pen_x + col * scale;
                    int py0 = row * scale;
                    for (int sy = 0; sy < scale; ++sy) {
                        int py = py0 + sy;
                        if (py < 0 || py >= bh) continue;
                        for (int sx = 0; sx < scale; ++sx) {
                            int px = px0 + sx;
                            if (px >= 0 && px < bw) buf[py * bw + px] = fg;
                        }
                    }
                }
            }
        }
        pen_x += cw;
    }
}

static int text_w(const char *text, int scale)
{
    if (!text) return 0;
    return (int)strlen(text) * 6 * scale;
}

static void draw_text_xy(int x, int y, const char *text, int scale, uint16_t fg, uint16_t bg)
{
    if (!s_strip || !text || scale < 1) return;
    int h = 7 * scale + 2;
    int w = text_w(text, scale) + 4;
    if (w < 8) w = 8;
    if (w > TFT_H_RES) w = TFT_H_RES;
    if (h > STRIP_LINES) h = STRIP_LINES;
    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; ++i) s_strip[i] = bg;
    render_text_into(s_strip, w, h, 2, text, scale, fg);
    (void)draw_bitmap_wait(x, y, x + w, y + h, s_strip);
}

static void draw_text_right(int x_right, int y, const char *text, int scale, uint16_t fg, uint16_t bg)
{
    int w = text_w(text, scale) + 4;
    draw_text_xy(x_right - w, y, text, scale, fg, bg);
}

static void draw_rect_outline(int x, int y, int w, int h, uint16_t color)
{
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

static void draw_hbar(int x, int y, int w, int h, uint16_t fill, uint16_t base, float pct)
{
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    fill_rect(x, y, w, h, base);
    int fw = (int)(pct * (float)w + 0.5f);
    if (fw > 0) fill_rect(x, y, fw, h, fill);
}

static char s_cache[32][40];

static void cache_reset(void)
{
    memset(s_cache, 0, sizeof(s_cache));
}

static bool cache_changed(int slot, const char *text)
{
    if (slot < 0 || slot >= (int)(sizeof(s_cache)/sizeof(s_cache[0]))) return true;
    if (!text) text = "";
    if (strncmp(s_cache[slot], text, sizeof(s_cache[slot])) == 0) return false;
    snprintf(s_cache[slot], sizeof(s_cache[slot]), "%s", text);
    return true;
}

static void draw_card_static(int x, int y, int w, int h,
                             const char *title, const char *unit,
                             uint16_t accent)
{
    fill_rect(x, y, w, h, C_PANEL);
    draw_rect_outline(x, y, w, h, C_STROKE);
    fill_rect(x + 1, y + 1, 5, h - 2, accent);
    if (title && *title) draw_text_xy(x + 12, y + 7, title, 1, C_MUTED, C_PANEL);
    if (unit && *unit) draw_text_xy(x + 12, y + h - 17, unit, 1, C_MUTED, C_PANEL);
    fill_rect(x + 12, y + h - 7, w - 24, 3, C_PANEL2);
}

static void update_card_value(int slot, int x, int y, int w, int h,
                              const char *value, int scale,
                              uint16_t accent, float bar_pct)
{
    if (!cache_changed(slot, value)) return;

    int vy = y + 22;
    int vh = 7 * scale + 5;
    if (vy + vh > y + h - 17) vh = (y + h - 17) - vy;
    if (vh < 8) vh = 8;
    fill_rect(x + 10, vy, w - 20, vh, C_PANEL);
    draw_text_xy(x + 12, vy + 2, value, scale, C_WHITE, C_PANEL);

    if (bar_pct >= 0.0f) {
        draw_hbar(x + 12, y + h - 7, w - 24, 3, accent, C_PANEL2, bar_pct);
    }
}

static void update_card_aux(int slot, int x, int y, int w, int h,
                            const char *text, uint16_t color)
{
    if (!cache_changed(slot, text)) return;
    fill_rect(x + 10, y + h - 19, w - 20, 12, C_PANEL);
    if (text && *text) draw_text_xy(x + 12, y + h - 17, text, 1, color, C_PANEL);
}

static void draw_header_static(void)
{
    fill_rect(0, 0, TFT_H_RES, 14, C_PANEL2);
    draw_text_xy(6, 3, "NISSAN B14 CONSULT", 1, C_MUTED, C_PANEL2);
}

static void update_header_status(const dashboard_view_t *v)
{
    char line[64];
    snprintf(line, sizeof(line), "USB:%s ECU:%s IGN:%s %s",
             v->ftdi_connected ? "OK" : "--",
             v->sensor_valid ? "LIVE" : (v->consult_session_active ? "SYNC" : "--"),
             v->ign_seen_on ? (v->ign_on ? "ON" : "OFF") : "--",
             v->parser_mode ? v->parser_mode : "SEARCH");
    if (!cache_changed(30, line)) return;
    fill_rect(270, 1, 208, 12, C_PANEL2);
    draw_text_right(474, 3, line, 1, v->sensor_valid ? C_GREEN : C_CYAN, C_PANEL2);
}

static void draw_footer(dash_page_t page, const char *hint)
{
    char b[64];
    fill_rect(0, 302, TFT_H_RES, 18, C_PANEL2);
    snprintf(b, sizeof(b), "PAGE %d/4", (int)page + 1);
    draw_text_xy(8, 306, b, 1, C_ACCENT, C_PANEL2);
    if (hint) draw_text_right(472, 306, hint, 1, C_MUTED, C_PANEL2);
}

/* Title scale is intentionally 2. This fixes the previous title/subtitle
 * collision at the top of ENGINE DATA / SETTINGS / DRIVE DASH / TRIP LOGGER.
 */
static void draw_page_title(const char *title, const char *subtitle)
{
    fill_rect(0, 16, TFT_H_RES, 34, C_BG);
    fill_rect(10, 21, 4, 23, C_ACCENT);
    draw_text_xy(22, 19, title, 2, C_WHITE, C_BG);
    if (subtitle && *subtitle) draw_text_xy(22, 37, subtitle, 1, C_MUTED, C_BG);
}

static void draw_static_main(void)
{
    draw_page_title("DRIVE DASH", "NISSAN B14 - LIVE ROAD VIEW");
    draw_card_static(18, 58, 140, 104, "SPEED", "KM/H", C_GREEN);
    draw_card_static(170, 58, 140, 104, "RPM", "RPM", C_RED);
    draw_card_static(322, 58, 140, 104, "FUEL RATE", "L/H", C_YELLOW);
    draw_card_static(18, 174, 140, 104, "COOLANT", "DEG C", C_ORANGE);
    draw_card_static(170, 174, 140, 104, "BATTERY", "V", C_CYAN);
    draw_card_static(322, 174, 140, 104, "TRIP COST", "BAHT", C_PURPLE);
    draw_footer(DASH_PAGE_MAIN, "TURN PAGE   PRESS SET");
}

static void draw_static_engine(void)
{
    draw_page_title("ENGINE DATA", "ENGINEERING UNITS + RAW TRACE");
    draw_card_static(18, 58, 136, 70, "RPM", "RPM", C_RED);
    draw_card_static(172, 58, 90, 70, "SPD", "KM/H", C_GREEN);
    draw_card_static(274, 58, 90, 70, "TPS", "", C_GREEN);
    draw_card_static(376, 58, 86, 70, "BAT", "V", C_CYAN);
    draw_card_static(18, 140, 106, 70, "INJ", "MS", C_GREEN);
    draw_card_static(136, 140, 106, 70, "DUTY", "%", C_PURPLE);
    draw_card_static(254, 140, 106, 70, "MAF EST", "", C_BLUE);
    draw_card_static(372, 140, 90, 70, "IGN", "+BTDC/-ATDC", C_ORANGE);
    draw_card_static(18, 222, 106, 70, "TEMP", "C", C_ORANGE);
    draw_card_static(136, 222, 106, 70, "O2 STATE", "V / R-L", C_PURPLE);
    draw_card_static(254, 222, 106, 70, "AAC/CORR", "% / %", C_BLUE);
    draw_card_static(372, 222, 90, 70, "FUEL", "L/H", C_YELLOW);
    draw_footer(DASH_PAGE_ENGINE, "TURN PAGE   PRESS SET");
}

static void draw_static_trip(void)
{
    draw_page_title("TRIP LOGGER", "DISTANCE - FUEL - COST SUMMARY");
    draw_card_static(18, 58, 140, 104, "DISTANCE", "KM", C_GREEN);
    draw_card_static(170, 58, 140, 104, "FUEL USED", "L", C_YELLOW);
    draw_card_static(322, 58, 140, 104, "TRIP COST", "BAHT", C_PURPLE);
    draw_card_static(18, 174, 140, 104, "AVG ECON", "KM/L", C_CYAN);
    draw_card_static(170, 174, 140, 104, "DAY TRIPS", "COUNT", C_BLUE);
    draw_card_static(322, 174, 140, 104, "LIFETIME", "COUNT", C_RED);
    draw_footer(DASH_PAGE_TRIP, "TURN PAGE   HOLD BACK");
}

static void draw_static_settings(void)
{
    draw_page_title("SETTINGS", "TURN MOVE - PRESS SELECT - HOLD BACK");
    fill_rect(18, 58, 444, 234, C_PANEL);
    draw_rect_outline(18, 58, 444, 234, C_STROKE);
    draw_footer(DASH_PAGE_SETTINGS, "PRESS SET   HOLD BACK");
}

static void draw_static_page(dash_page_t page)
{
    clear_screen(C_BG);
    cache_reset();
    draw_header_static();
    switch (page) {
        case DASH_PAGE_ENGINE:   draw_static_engine(); break;
        case DASH_PAGE_TRIP:     draw_static_trip(); break;
        case DASH_PAGE_SETTINGS: draw_static_settings(); break;
        case DASH_PAGE_MAIN:
        default:                 draw_static_main(); break;
    }
}

static void render_main(const dashboard_view_t *v)
{
    char b1[32], b2[32], b3[32], b4[32], b5[32], b6[32];
    if (!v->sensor_valid) {
        update_card_value(0, 18, 58, 140, 104, "--", 3, C_GREEN, 0.0f);
        update_card_value(1, 170, 58, 140, 104, "--", 3, C_RED, 0.0f);
        update_card_value(2, 322, 58, 140, 104, "--", 3, C_YELLOW, 0.0f);
        update_card_value(3, 18, 174, 140, 104, "--", 3, C_ORANGE, 0.0f);
        update_card_value(4, 170, 174, 140, 104, "--", 3, C_CYAN, 0.0f);
        update_card_value(5, 322, 174, 140, 104, "--", 3, C_PURPLE, 0.0f);
        return;
    }

    snprintf(b1, sizeof(b1), "%d", v->speed_kmh);
    snprintf(b2, sizeof(b2), "%.0f", v->rpm);
    snprintf(b3, sizeof(b3), "%d", v->ect_c);
    snprintf(b4, sizeof(b4), "%.1f", v->battery_v);
    snprintf(b5, sizeof(b5), "%.2f", v->fuel_lph);
    snprintf(b6, sizeof(b6), "%.2f", v->trip_cost_baht);

    update_card_value(0, 18, 58, 140, 104, b1, 3, C_GREEN, v->speed_kmh / 180.0f);
    update_card_value(1, 170, 58, 140, 104, b2, 3, C_RED, v->rpm / 8000.0f);
    update_card_value(2, 322, 58, 140, 104, b5, 3, C_YELLOW, v->fuel_lph / 6.0f);
    update_card_value(3, 18, 174, 140, 104, b3, 3, C_ORANGE, (v->ect_c - 40) / 100.0f);
    update_card_value(4, 170, 174, 140, 104, b4, 3, C_CYAN, (v->battery_v - 10.0f) / 5.0f);
    update_card_value(5, 322, 174, 140, 104, b6, 3, C_PURPLE, v->trip_cost_baht / 500.0f);
}

static void render_engine(const dashboard_view_t *v)
{
    char a[32], b[32], cval[32], d[32], maf[32], f[32], ign[32], fuel[32];
    char tps[32], duty[32], o2[32], aac_corr[32];
    char tps_aux[32], maf_aux[32];

    snprintf(a, sizeof(a), "%.0f", v->rpm);
    snprintf(b, sizeof(b), "%d", v->speed_kmh);
    snprintf(cval, sizeof(cval), "%.2f", v->injector_ms);
    snprintf(d, sizeof(d), "%.1f", v->battery_v);
    if (v->maf_gps_valid) snprintf(maf, sizeof(maf), "%.1f", v->maf_gps_est);
    else snprintf(maf, sizeof(maf), "--");
    snprintf(f, sizeof(f), "%d", v->ect_c);
    snprintf(ign, sizeof(ign), "%+d", v->ignition_deg);
    snprintf(fuel, sizeof(fuel), "%.2f", v->fuel_lph);
    snprintf(tps, sizeof(tps), "%.0f", v->tps_pct);
    snprintf(duty, sizeof(duty), "%.1f", v->injector_duty);
    snprintf(tps_aux, sizeof(tps_aux), "%% RAW%.2fV", v->tps_v);
    snprintf(maf_aux, sizeof(maf_aux), "G/S RAW%.2fV", v->maf_v);

    if (v->visible_mask & (1u << 0)) {
        const char state = v->o2_rich ? 'R' : (v->o2_lean ? 'L' : 'M');
        snprintf(o2, sizeof(o2), "%c %.2f", state, v->o2_v);
    } else {
        snprintf(o2, sizeof(o2), "--");
    }

    const bool show_aac = (v->visible_mask & (1u << 1)) != 0;
    const bool show_af = (v->visible_mask & (1u << 2)) != 0;
    if (show_aac && show_af) snprintf(aac_corr, sizeof(aac_corr), "%.0f/%+.0f", v->aac_pct, v->fuel_corr_pct);
    else if (show_aac) snprintf(aac_corr, sizeof(aac_corr), "%.0f/--", v->aac_pct);
    else if (show_af) snprintf(aac_corr, sizeof(aac_corr), "--/%+.0f", v->fuel_corr_pct);
    else snprintf(aac_corr, sizeof(aac_corr), "--");

    update_card_value(0, 18, 58, 136, 70, a, 2, C_RED, v->rpm / 8000.0f);
    update_card_value(1, 172, 58, 90, 70, b, 2, C_GREEN, v->speed_kmh / 180.0f);
    update_card_value(2, 274, 58, 90, 70, tps, 2, C_GREEN, v->tps_pct / 100.0f);
    update_card_value(3, 376, 58, 86, 70, d, 2, C_CYAN, (v->battery_v - 10.0f) / 5.0f);
    update_card_value(4, 18, 140, 106, 70, cval, 2, C_GREEN, v->injector_ms / 10.0f);
    update_card_value(5, 136, 140, 106, 70, duty, 2, C_PURPLE, v->injector_duty / 100.0f);
    update_card_value(6, 254, 140, 106, 70, maf, 2, C_BLUE, v->maf_gps_valid ? v->maf_gps_est / 80.0f : 0.0f);
    update_card_value(7, 372, 140, 90, 70, ign, 2, C_ORANGE, (v->ignition_deg + 20) / 80.0f);
    update_card_value(8, 18, 222, 106, 70, f, 2, C_ORANGE, (v->ect_c - 40) / 100.0f);
    update_card_value(9, 136, 222, 106, 70, o2, 2, C_PURPLE,
                      (v->visible_mask & (1u << 0)) ? v->o2_v : 0.0f);
    update_card_value(10, 254, 222, 106, 70, aac_corr, 2, C_BLUE,
                      show_aac ? v->aac_pct / 100.0f : (show_af ? (v->fuel_corr_pct + 50.0f) / 100.0f : 0.0f));
    update_card_value(11, 372, 222, 90, 70, fuel, 2, C_YELLOW, v->fuel_lph / 6.0f);

    /* Small trace lines keep the original sensor voltages visible without
       sacrificing the larger engineering values. */
    update_card_aux(28, 274, 58, 90, 70, tps_aux, C_MUTED);
    update_card_aux(29, 254, 140, 106, 70, maf_aux, C_MUTED);
}

static void render_trip(const dashboard_view_t *v)
{
    char a[32], b[32], c[32], d[32], e[32], f[32], status[64];
    if (v->trip_active) {
        snprintf(status, sizeof(status), "ACTIVE #%lu", (unsigned long)v->trip_id);
    } else if (v->trip_showing_last) {
        snprintf(status, sizeof(status), "LAST #%lu %s",
                 (unsigned long)v->trip_id,
                 v->trip_end_reason ? v->trip_end_reason : "SAVED");
    } else {
        snprintf(status, sizeof(status), "NO SAVED TRIP");
    }
    if (cache_changed(23, status)) {
        fill_rect(285, 35, 178, 13, C_BG);
        draw_text_right(460, 37, status, 1, v->trip_active ? C_GREEN : C_CYAN, C_BG);
    }
    snprintf(a, sizeof(a), "%.2f", v->trip_distance_km);
    snprintf(b, sizeof(b), "%.3f", v->trip_fuel_l);
    snprintf(c, sizeof(c), "%.2f", v->trip_cost_baht);
    snprintf(d, sizeof(d), "%.2f", v->trip_avg_km_l);
    snprintf(e, sizeof(e), "%lu", (unsigned long)v->daily_trips);
    snprintf(f, sizeof(f), "%lu", (unsigned long)v->lifetime_trips);

    update_card_value(0, 18, 58, 140, 104, a, 3, C_GREEN, v->trip_distance_km / 500.0);
    update_card_value(1, 170, 58, 140, 104, b, 3, C_YELLOW, v->trip_fuel_l / 50.0);
    update_card_value(2, 322, 58, 140, 104, c, 3, C_PURPLE, v->trip_cost_baht / 1000.0);
    update_card_value(3, 18, 174, 140, 104, d, 3, C_CYAN, v->trip_avg_km_l / 30.0);
    update_card_value(4, 170, 174, 140, 104, e, 3, C_BLUE, v->daily_trips / 20.0f);
    update_card_value(5, 322, 174, 140, 104, f, 3, C_RED, v->lifetime_trips / 1000.0f);
}

static void render_settings(const dashboard_view_t *v)
{
    static const char *items[] = {
        "FUEL PRICE", "CALIBRATION", "SHOW O2", "SHOW AAC",
        "SHOW CORR", "NEXT DAY", "RETURN"
    };
    char row[72], key[80];
    for (int i = 0; i < 7; ++i) {
        int y = 66 + i * 30;
        uint16_t bg = (i == v->settings_index) ? C_PANEL2 : C_PANEL;
        uint16_t fg = (i == v->settings_index) ? C_YELLOW : C_WHITE;
        if (i == 0) {
            snprintf(row, sizeof(row), "%s  %.2f%s", items[i],
                     (v->settings_editing && i == v->settings_index) ? v->edit_value : v->fuel_price,
                     (v->settings_editing && i == v->settings_index) ? " EDIT" : "");
        } else if (i == 1) {
            snprintf(row, sizeof(row), "%s  %.3f%s", items[i],
                     (v->settings_editing && i == v->settings_index) ? v->edit_value : v->calibration,
                     (v->settings_editing && i == v->settings_index) ? " EDIT" : "");
        } else if (i == 2) {
            snprintf(row, sizeof(row), "%s  %s", items[i], (v->visible_mask & (1u << 0)) ? "ON" : "OFF");
        } else if (i == 3) {
            snprintf(row, sizeof(row), "%s  %s", items[i], (v->visible_mask & (1u << 1)) ? "ON" : "OFF");
        } else if (i == 4) {
            snprintf(row, sizeof(row), "%s  %s", items[i], (v->visible_mask & (1u << 2)) ? "ON" : "OFF");
        } else if (i == 5) {
            snprintf(row, sizeof(row), "%s  %s", items[i], v->current_date ? v->current_date : "----");
        } else {
            snprintf(row, sizeof(row), "%s", items[i]);
        }
        snprintf(key, sizeof(key), "%c%s", (i == v->settings_index) ? '>' : ' ', row);
        if (!cache_changed(i, key)) continue;
        fill_rect(28, y, 424, 26, bg);
        if (i == v->settings_index) {
            fill_rect(28, y, 5, 26, C_ACCENT);
            draw_rect_outline(28, y, 424, 26, C_STROKE);
        }
        draw_text_xy(40, y + 6, row, 2, fg, bg);
    }

    char mode[64];
    snprintf(mode, sizeof(mode), "NO RTC | IGN:%s", v->ign_seen_on ? (v->ign_on ? "ON" : "OFF") : "NOT WIRED");
    if (cache_changed(21, mode)) {
        fill_rect(28, 278, 250, 14, C_PANEL);
        draw_text_xy(32, 280, mode, 1, C_MUTED, C_PANEL);
    }

    if (v->just_saved) {
        if (cache_changed(20, "SAVED")) {
            fill_rect(320, 276, 120, 18, C_GREEN);
            draw_text_xy(352, 280, "SAVED", 1, C_BLACK, C_GREEN);
        }
    } else if (cache_changed(20, "NOSAVE")) {
        fill_rect(320, 276, 120, 18, C_PANEL);
    }
}

static void tft_backlight_set(bool on)
{
    const int level = on ? TFT_BL_ACTIVE_LEVEL : !TFT_BL_ACTIVE_LEVEL;
    gpio_set_level((gpio_num_t)TFT_PIN_BL, level);
}

static esp_err_t tft_power_on_prepare(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << TFT_PIN_BL) |
                        (1ULL << TFT_PIN_CS) |
                        (1ULL << TFT_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;

    /* Hide the uncontrolled white panel while both supplies are ramping. */
    tft_backlight_set(false);

    /* Prevent accidental SPI command selection during ESP32 boot. */
    gpio_set_level((gpio_num_t)TFT_PIN_CS, 1);

    /* Hold ILI9488 in reset until VCC and ESP32 GPIO states are stable. */
    gpio_set_level((gpio_num_t)TFT_PIN_RST, 0);
    ESP_LOGI(TAG, "POWER-ON: BL=OFF CS=HIGH RST=LOW; settle %d ms",
             TFT_POWER_STABILIZE_MS);
    vTaskDelay(pdMS_TO_TICKS(TFT_POWER_STABILIZE_MS));

    gpio_set_level((gpio_num_t)TFT_PIN_RST, 1);
    ESP_LOGI(TAG, "POWER-ON: RST=HIGH; recovery %d ms",
             TFT_RESET_RELEASE_MS);
    vTaskDelay(pdMS_TO_TICKS(TFT_RESET_RELEASE_MS));
    return ESP_OK;
}

static esp_err_t tft_panel_reset_init_with_retry(void)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= TFT_INIT_RETRY_COUNT; ++attempt) {
        ESP_LOGI(TAG, "ILI9488 init attempt %d/%d", attempt, TFT_INIT_RETRY_COUNT);
        err = esp_lcd_panel_reset(s_panel);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(TFT_PANEL_RESET_WAIT_MS));
            err = esp_lcd_panel_init(s_panel);
        }
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(TFT_PANEL_INIT_WAIT_MS));
            return ESP_OK;
        }

        ESP_LOGW(TAG, "ILI9488 init attempt %d failed: %s",
                 attempt, esp_err_to_name(err));
        tft_backlight_set(false);
        gpio_set_level((gpio_num_t)TFT_PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level((gpio_num_t)TFT_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(TFT_RESET_RELEASE_MS));
    }
    return err;
}

bool dashboard_init(void)
{
    esp_err_t power_err = tft_power_on_prepare();
    if (power_err != ESP_OK) {
        ESP_LOGE(TAG, "TFT power-on GPIO preparation failed: %s", esp_err_to_name(power_err));
        return false;
    }

    s_draw_mutex = xSemaphoreCreateMutex();
    s_tx_done = xSemaphoreCreateBinary();
    if (!s_draw_mutex || !s_tx_done) {
        ESP_LOGE(TAG, "TFT semaphore allocation failed");
        return false;
    }

    s_strip = (uint16_t *)heap_caps_malloc(STRIP_PIXELS * sizeof(uint16_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_strip) {
        ESP_LOGE(TAG, "TFT DMA strip allocation failed (%u bytes)",
                 (unsigned)(STRIP_PIXELS * sizeof(uint16_t)));
        return false;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = TFT_PIN_MOSI,
        .miso_io_num = TFT_PIN_MISO,
        .sclk_io_num = TFT_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_MAX_XFER,
    };
    esp_err_t err = spi_bus_initialize(TFT_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = TFT_PIN_DC,
        .cs_gpio_num = TFT_PIN_CS,
        .pclk_hz = TFT_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = lcd_color_done_cb,
        .user_ctx = s_tx_done,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel IO failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TFT_PIN_RST,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#else
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
#endif
        .bits_per_pixel = 18,
    };
    err = esp_lcd_new_panel_ili9488(s_io, &panel_cfg, STRIP_PIXELS, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ILI9488 panel create failed: %s", esp_err_to_name(err));
        return false;
    }

    err = tft_panel_reset_init_with_retry();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ILI9488 reset/init failed after %d attempts: %s",
                 TFT_INIT_RETRY_COUNT, esp_err_to_name(err));
        tft_backlight_set(false);
        return false;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_set_gap(s_panel, 0, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(s_panel, true));

    /* Backlight remains OFF until the complete splash frame is already in GRAM.
     * This prevents a visible white flash even if the LCD controller powered first. */
    tft_backlight_set(false);
    clear_screen(C_BLACK);

    s_ready = true;
    if (xSemaphoreTake(s_draw_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        draw_static_page(DASH_PAGE_MAIN);
        xSemaphoreGive(s_draw_mutex);
    }
    ESP_LOGI(TAG, "TFT v3.9.10 POWER-ON FIX ready | BL GPIO18 | RST GPIO8 | 480x320 | SPI2 10MHz");
    return true;
}

void dashboard_show_splash(const char *brand, uint32_t ms)
{
    if (!s_ready || !s_draw_mutex) return;
    if (!brand || !*brand) brand = "JOEVOHAN@261";

    if (xSemaphoreTake(s_draw_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        clear_screen(C_BLACK);

        /* Premium boot frame: simple direct-draw geometry keeps it reliable
         * without LVGL, SD, fonts or external assets. */
        fill_rect(0, 0, TFT_H_RES, 5, C_CYAN);
        fill_rect(0, TFT_V_RES - 5, TFT_H_RES, 5, C_BLUE);
        draw_rect_outline(20, 24, 440, 272, C_STROKE);
        draw_rect_outline(24, 28, 432, 264, C_PANEL2);

        fill_rect(48, 76, 384, 3, C_BLUE);
        fill_rect(96, 239, 288, 2, C_CYAN);

        /* Bold logo: the shadow pass is drawn first; the main pass is centered.
         * Scale 5 fits the 40-line DMA strip renderer and gives the boot brand more presence. */
        const int logo_scale = 5;
        const int logo_w = text_w(brand, logo_scale) + 4;
        const int logo_x = (TFT_H_RES - logo_w) / 2;
        draw_text_xy(logo_x + 2, 121 + 2, brand, logo_scale, C_BLUE, C_BLACK);
        draw_text_xy(logo_x, 121, brand, logo_scale, C_WHITE, C_BLACK);

        const char *sub = "NISSAN B14 CONSULT DASHBOARD";
        const int sub_w = text_w(sub, 2) + 4;
        draw_text_xy((TFT_H_RES - sub_w) / 2, 176, sub, 2, C_CYAN, C_BLACK);

        const char *mode = "ENG UNITS  |  FLASH  |  NO SD  |  NO RTC";
        const int mode_w = text_w(mode, 1) + 4;
        draw_text_xy((TFT_H_RES - mode_w) / 2, 211, mode, 1, C_MUTED, C_BLACK);

        draw_text_xy(28, 34, "SYSTEM BOOT", 1, C_MUTED, C_BLACK);
        draw_text_right(452, 34, "V3.9.10", 1, C_GREEN, C_BLACK);
        xSemaphoreGive(s_draw_mutex);

        /* The first visible frame is now the completed JOEVOHAN@261 splash. */
        tft_backlight_set(true);
        ESP_LOGI(TAG, "POWER-ON: splash ready; BL=ON");
    }

    if (ms) vTaskDelay(pdMS_TO_TICKS(ms));
    cache_reset();
    s_page_dirty = true;
}

void dashboard_set_page(dash_page_t page)
{
    if (!s_ready || (int)page < 0 || page >= DASH_PAGE_COUNT) return;
    if (s_page != page) s_page_dirty = true;
    s_page = page;
}

void dashboard_render(const dashboard_view_t *v)
{
    if (!s_ready || !v || !s_draw_mutex) return;
    int64_t now = esp_timer_get_time();
    if (!s_page_dirty && now - s_last_render_us < 400000) return; /* 2.5 Hz; values only */
    s_last_render_us = now;

    if (xSemaphoreTake(s_draw_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (s_page_dirty) {
        draw_static_page(s_page);
        s_page_dirty = false;
    }

    update_header_status(v);
    switch (s_page) {
        case DASH_PAGE_ENGINE:   render_engine(v); break;
        case DASH_PAGE_TRIP:     render_trip(v); break;
        case DASH_PAGE_SETTINGS: render_settings(v); break;
        case DASH_PAGE_MAIN:
        default:                 render_main(v); break;
    }
    xSemaphoreGive(s_draw_mutex);
}

void dashboard_show_message(const char *title, const char *message, uint32_t ms)
{
    if (!s_ready || !s_draw_mutex) return;
    if (xSemaphoreTake(s_draw_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        fill_rect(40, 110, 400, 100, C_PANEL2);
        draw_rect_outline(40, 110, 400, 100, C_ACCENT);
        draw_text_xy(58, 130, title ? title : "INFO", 2, C_CYAN, C_PANEL2);
        draw_text_xy(58, 162, message ? message : "", 1, C_WHITE, C_PANEL2);
        xSemaphoreGive(s_draw_mutex);
    }
    if (ms) vTaskDelay(pdMS_TO_TICKS(ms));
}
