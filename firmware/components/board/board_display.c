#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "display";

#define PIN_LCD_CS   14
#define PIN_LCD_SCLK 13
#define PIN_LCD_D0   15
#define PIN_LCD_D1   16
#define PIN_LCD_D2   17
#define PIN_LCD_D3   18
#define PIN_LCD_RST  21
#define PIN_LCD_BL   47

#define FRAME_BYTES (BOARD_LCD_W * BOARD_LCD_H * 2)
#define PARTIAL_LINES 40

/* Panel start-up sequence from Waveshare's demo for this board. The panel's
 * generic ST77916 defaults only light a strip of the screen. */
static const st77916_lcd_init_cmd_t s_init_cmds[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0}, {0xF2, (uint8_t[]){0x28}, 1, 0}, {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0}, {0x83, (uint8_t[]){0xE0}, 1, 0}, {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0}, {0xF0, (uint8_t[]){0x00}, 1, 0}, {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0}, {0xB0, (uint8_t[]){0x56}, 1, 0}, {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0}, {0xB4, (uint8_t[]){0x87}, 1, 0}, {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0}, {0xB7, (uint8_t[]){0x40}, 1, 0}, {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0}, {0xBB, (uint8_t[]){0x08}, 1, 0}, {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0}, {0xC0, (uint8_t[]){0x80}, 1, 0}, {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0}, {0xC3, (uint8_t[]){0x80}, 1, 0}, {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0}, {0xC6, (uint8_t[]){0xA9}, 1, 0}, {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0}, {0xC9, (uint8_t[]){0xA9}, 1, 0}, {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0}, {0xD0, (uint8_t[]){0x91}, 1, 0}, {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0}, {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0}, {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0}, {0xF1, (uint8_t[]){0x10}, 1, 0}, {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0}, {0xF3, (uint8_t[]){0x10}, 1, 0}, {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0}, {0xE2, (uint8_t[]){0x00}, 1, 0}, {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0}, {0xE5, (uint8_t[]){0x06}, 1, 0}, {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0}, {0xE8, (uint8_t[]){0x05}, 1, 0}, {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0}, {0xEB, (uint8_t[]){0x00}, 1, 0}, {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0}, {0xEE, (uint8_t[]){0x00}, 1, 0}, {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0}, {0xF9, (uint8_t[]){0x00}, 1, 0}, {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0}, {0xFC, (uint8_t[]){0x00}, 1, 0}, {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0}, {0xFF, (uint8_t[]){0x00}, 1, 0}, {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0}, {0x62, (uint8_t[]){0x00}, 1, 0}, {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0}, {0x65, (uint8_t[]){0x00}, 1, 0}, {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0}, {0x68, (uint8_t[]){0x00}, 1, 0}, {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0}, {0x6B, (uint8_t[]){0x00}, 1, 0}, {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0}, {0x72, (uint8_t[]){0x00}, 1, 0}, {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0}, {0x75, (uint8_t[]){0x00}, 1, 0}, {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0}, {0x78, (uint8_t[]){0x00}, 1, 0}, {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0}, {0x7B, (uint8_t[]){0x00}, 1, 0}, {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0}, {0x82, (uint8_t[]){0x06}, 1, 0}, {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0}, {0x85, (uint8_t[]){0x04}, 1, 0}, {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0}, {0x88, (uint8_t[]){0x48}, 1, 0}, {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0}, {0x8B, (uint8_t[]){0x02}, 1, 0}, {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0}, {0x8E, (uint8_t[]){0x00}, 1, 0}, {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0}, {0x91, (uint8_t[]){0x00}, 1, 0}, {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0}, {0x94, (uint8_t[]){0xDA}, 1, 0}, {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0}, {0x97, (uint8_t[]){0x00}, 1, 0}, {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0}, {0x9A, (uint8_t[]){0x0C}, 1, 0}, {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0}, {0x9D, (uint8_t[]){0x04}, 1, 0}, {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0}, {0xA0, (uint8_t[]){0x48}, 1, 0}, {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0}, {0xA3, (uint8_t[]){0x02}, 1, 0}, {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0}, {0xA6, (uint8_t[]){0x00}, 1, 0}, {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0}, {0xA9, (uint8_t[]){0x00}, 1, 0}, {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0}, {0xAC, (uint8_t[]){0xD7}, 1, 0}, {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0}, {0xAF, (uint8_t[]){0x00}, 1, 0}, {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0}, {0xB2, (uint8_t[]){0x09}, 1, 0}, {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0}, {0xB5, (uint8_t[]){0x04}, 1, 0}, {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0}, {0xB8, (uint8_t[]){0x48}, 1, 0}, {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0}, {0xBB, (uint8_t[]){0x02}, 1, 0}, {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0}, {0xBE, (uint8_t[]){0x00}, 1, 0}, {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0}, {0xC1, (uint8_t[]){0x47}, 1, 0}, {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0}, {0xC4, (uint8_t[]){0x74}, 1, 0}, {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0}, {0xC7, (uint8_t[]){0x01}, 1, 0}, {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0}, {0xD0, (uint8_t[]){0x10}, 1, 0}, {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0}, {0xD3, (uint8_t[]){0x65}, 1, 0}, {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0}, {0xD6, (uint8_t[]){0x99}, 1, 0}, {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0}, {0xD9, (uint8_t[]){0xAA}, 1, 0}, {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0}, {0x21, (uint8_t[]){0x00}, 1, 0}, {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0}, {0x36, (uint8_t[]){0x00}, 1, 0},
};

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;  /* kept for the sleep commands below */
static lv_display_t *s_disp;
static volatile uint32_t s_frames;
static uint64_t s_render_us, s_flush_us, s_flushed_px, s_post_us;
static int64_t s_render_t0, s_flush_t0;
static bool s_backlight_on;
static bool s_panel_asleep;  /* the backlight must not come up behind a sleeping panel */
/* Transfers queued to the panel but not yet finished, and whether a whole
 * frame has been rendered. The backlight needs both: rendered is not the same
 * as arrived (see the first-frame note in render_event). */
static volatile int s_inflight;
static volatile bool s_frame_rendered;
static board_post_render_cb_t s_post_render;
static uint8_t *s_shadow;

void board_display_shadow_enable(void)
{
    if (!s_shadow) s_shadow = heap_caps_aligned_calloc(64, 1, FRAME_BYTES, MALLOC_CAP_SPIRAM);
}

const uint8_t *board_display_shadow(void) { return s_shadow; }

void board_display_set_post_render(board_post_render_cb_t cb) { s_post_render = cb; }

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    (void)io;
    (void)e;
    s_flush_us += esp_timer_get_time() - s_flush_t0;
    if (s_inflight > 0) s_inflight--;
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
#if CONFIG_KNOB_RENDER_DIRECT
    /* Direct mode: LVGL has updated only what changed in the full frame. Send the
     * whole frame once, after the last area, in one top-to-bottom pass. */
    if (!lv_display_flush_is_last(d)) {
        lv_display_flush_ready(d);
        return;
    }
    uint8_t *frame = lv_display_get_buf_active(d)->data;
    const lv_area_t all = {0, 0, BOARD_LCD_W - 1, BOARD_LCD_H - 1};
    if (s_post_render) s_post_render(frame, &all, BOARD_LCD_W);
    s_flush_t0 = esp_timer_get_time();
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_W, BOARD_LCD_H, frame);
#else
    int w = lv_area_get_width(a);
    s_flushed_px += (uint64_t)w * lv_area_get_height(a);
    int64_t tp = esp_timer_get_time();
    if (s_post_render) s_post_render(px, a, w);
    s_post_us += esp_timer_get_time() - tp;
    if (s_shadow)
        for (int y = a->y1; y <= a->y2; y++)
            memcpy(s_shadow + ((size_t)y * BOARD_LCD_W + a->x1) * 2, px + (size_t)(y - a->y1) * w * 2, (size_t)w * 2);
    s_flush_t0 = esp_timer_get_time();
    s_inflight++;
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
#endif
}

/* The panel only accepts windows that start on an even pixel and have an even
 * width and height. Odd ones smear or drift. */
static volatile int s_log_invalidations;
void board_display_log_invalidations(int n) { s_log_invalidations = n; }

static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    if (s_log_invalidations > 0) {
        s_log_invalidations--;
        ESP_LOGI(TAG, "INVALIDATE %d,%d -> %d,%d (%dx%d)", a->x1, a->y1, a->x2, a->y2, lv_area_get_width(a),
                 lv_area_get_height(a));
    }
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static void render_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START:
        s_render_t0 = esp_timer_get_time();
        /* The backlight comes up here rather than at RENDER_READY below,
         * because rendering a frame is not the same as that frame being on the
         * glass. The screen is sent in strips over DMA and the last of them is
         * still in flight when rendering finishes, so lighting up at
         * RENDER_READY showed a part-drawn screen with the panel's power-on
         * contents underneath the rest - "some weird half-drawn flash of
         * something... it looks like the screen crashed" (Darren, 4 Nov 2026).
         *
         * Waiting for the start of the NEXT frame with nothing in flight means
         * a whole frame has arrived. It costs one frame of darkness, about
         * fifteen milliseconds, which nobody can see; a part-drawn screen is
         * the thing that gets noticed. */
        if (!s_backlight_on && !s_panel_asleep && s_frame_rendered && s_inflight == 0) {
            s_backlight_on = true;
            /* What is actually on the glass at the instant it is lit, said
             * once. Darren still sees a white flash at switch-on and the
             * backlight pin is not the cause - it reads low from power-on -
             * so the question is what the panel is showing when the light
             * arrives (4 Nov 2026). */
            static bool measured;
            if (!measured && s_shadow) {
                measured = true;
                uint64_t sum = 0;
                for (size_t i = 0; i < (size_t)BOARD_LCD_W * BOARD_LCD_H; i++) sum += s_shadow[i];
                ESP_LOGI(TAG, "lighting up: mean pixel 0x%04X (black 0x0000, white 0xFFFF)",
                         (unsigned)(sum / ((size_t)BOARD_LCD_W * BOARD_LCD_H)));
            }
            board_backlight(255);
        }
        break;
    case LV_EVENT_RENDER_READY:
        s_render_us += esp_timer_get_time() - s_render_t0;
        s_frames++;
        /* A frame has been drawn; whether it has arrived is RENDER_START's
         * question, above. Not while the panel is asleep either - a frame
         * drawn behind a dark screen would otherwise light it, which happens
         * for real on the timer check-in: it boots, draws the clock and must
         * stay dark (4 Nov 2026). */
        /* Said once, at boot. It is the measurement that settled this: the
         * screen is sent in 40-line strips and the last one is still on its
         * way when rendering finishes, so the old backlight-at-RENDER_READY
         * lit a frame missing its bottom strip. */
        static bool said;
        if (!said) {
            said = true;
            ESP_LOGI(TAG, "first frame rendered with %d transfers still in flight", s_inflight);
        }
        s_frame_rendered = true;
        break;
    default:
        break;
    }
}

/* Hold the backlight pin low before anything else runs.
 *
 * This was written to explain the white flash Darren sees at switch-on, on the
 * theory that the pin floats and the backlight driver pulls it up until
 * backlight_init gets to it. The measurement below says otherwise: the pin
 * reads LOW when app_main first looks at it, so nothing was pulling it up and
 * the flash is not this. The theory was wrong and is recorded here so nobody
 * spends another afternoon on it.
 *
 * The call stays because it is right anyway - the pin that drives the
 * backlight should be held, not assumed - and it costs microseconds. */
void board_backlight_off_early(void)
{
    /* What the pin was doing before we touched it, said once. This is the
     * measurement the whole explanation rests on: if it reads high here, it
     * has been high - and the screen lit - since the rail came up. */
    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&in);
    ESP_LOGI(TAG, "backlight pin reads %d before it is driven", gpio_get_level(PIN_LCD_BL));

    gpio_config_t c = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
    gpio_set_level(PIN_LCD_BL, 0);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 50 * 1000,
        .clk_cfg = LEDC_SLOW_CLK_RC_FAST,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_3,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
    /* Light sleep isolates every pin by default (see PM_SLP_DISABLE_GPIO in
     * sdkconfig.defaults). Not this one: floated, the backlight driver is free
     * to pull it up and the screen glows at nothing all night. */
    gpio_sleep_sel_dis(PIN_LCD_BL);
}

void board_backlight(uint8_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

/* 16 steps over a quarter of a second. Called from the drawing thread with
 * nothing left to draw, so the short blocking fade costs nothing. */
#define FADE_STEPS 16
#define FADE_STEP_MS 15

/* Dark is not off. Taking the backlight to zero leaves the ST77916 itself
 * running - its oscillator, its charge pump and its row and column drivers -
 * and that costs about as much again as the processor does. Measured on the
 * wall on 3 Nov 2026: asleep with only the backlight down, the dial still
 * emptied the cell in five and a half hours, losing 19% an hour while it was
 * doing nothing at all.
 *
 * So the panel is told to stop as well: display off (0x28) blanks it, and
 * sleep-in (0x10) is the one that actually shuts the internal supplies down.
 * Sleep-out needs 120 ms before the panel will accept pixels again, which is
 * waited out here rather than discovered as a first frame that never arrives;
 * the backlight still comes up on its own with the first finished frame, so
 * nothing of the wait is visible. */
#define PANEL_SLEEP_IN  0x10
#define PANEL_SLEEP_OUT 0x11
#define PANEL_SLEEP_OUT_MS 120

void board_display_sleep(bool asleep)
{
    if (!asleep) {
        if (!s_panel_asleep) return;
        if (s_io) {
            esp_lcd_panel_io_tx_param(s_io, PANEL_SLEEP_OUT, NULL, 0);
            vTaskDelay(pdMS_TO_TICKS(PANEL_SLEEP_OUT_MS));
        }
        if (s_panel) esp_lcd_panel_disp_on_off(s_panel, true);
        s_panel_asleep = false;
        s_backlight_on = false;  /* the next frame that has actually arrived brings it up */
        s_frame_rendered = false;  /* ...and waking gets the same rule as booting */
        return;
    }
    if (s_panel_asleep) return;
    /* Only fade from a brightness that is actually there. On the timer check-in
     * the dial is put to sleep before its first frame, so the backlight is
     * already at zero, and fading would mean winding it up to full first. */
    if (s_backlight_on) {
        for (int i = FADE_STEPS - 1; i >= 0; i--) {
            board_backlight((uint8_t)(255 * i / FADE_STEPS));
            vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS));
        }
    }
    board_backlight(0);
    s_backlight_on = false;
    s_frame_rendered = false;
    s_panel_asleep = true;
    /* Only now the glass is dark: a panel switched off before the fade would
     * blank rather than fade. */
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
    if (s_io) esp_lcd_panel_io_tx_param(s_io, PANEL_SLEEP_IN, NULL, 0);
}

static void *frame_alloc(size_t bytes, bool psram)
{
    void *p = psram ? heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_SPIRAM)
                    : heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!p) ESP_LOGE(TAG, "could not allocate %u-byte frame buffer", (unsigned)bytes);
    return p;
}

lv_display_t *board_display_init(void)
{
    backlight_init();

    const spi_bus_config_t bus = ST77916_PANEL_BUS_QSPI_CONFIG(PIN_LCD_SCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2,
                                                               PIN_LCD_D3, BOARD_LCD_W * PARTIAL_LINES * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    s_disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, on_trans_done, s_disp);
    io_cfg.pclk_hz = CONFIG_KNOB_LCD_PCLK_MHZ * 1000 * 1000;
    io_cfg.trans_queue_depth = 16;
    io_cfg.flags.psram_dma_direct = 1; /* DMA straight out of PSRAM frame buffers */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    st77916_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    s_io = io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* The panel takes big-endian RGB565; LVGL renders that order directly. */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(s_disp, render_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, render_event, LV_EVENT_RENDER_READY, NULL);

#if CONFIG_KNOB_RENDER_PARTIAL
    size_t bytes = BOARD_LCD_W * PARTIAL_LINES * 2;
    lv_display_set_buffers(s_disp, frame_alloc(bytes, false), frame_alloc(bytes, false), bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
#elif CONFIG_KNOB_RENDER_FULL
    lv_display_set_buffers(s_disp, frame_alloc(FRAME_BYTES, true), frame_alloc(FRAME_BYTES, true), FRAME_BYTES,
                           LV_DISPLAY_RENDER_MODE_FULL);
#else
    lv_display_set_buffers(s_disp, frame_alloc(FRAME_BYTES, true), frame_alloc(FRAME_BYTES, true), FRAME_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
#endif
    ESP_LOGI(TAG, "ST77916 360x360, %s, QSPI %d MHz", board_display_mode_name(), CONFIG_KNOB_LCD_PCLK_MHZ);
    return s_disp;
}

const char *board_display_mode_name(void)
{
#if CONFIG_KNOB_RENDER_PARTIAL
    return "partial (2 x 40 lines, internal RAM)";
#elif CONFIG_KNOB_RENDER_FULL
    return "full (2 frames in PSRAM, full redraw)";
#else
    return "direct (2 frames in PSRAM, changes only)";
#endif
}

uint32_t board_display_pclk_mhz(void) { return CONFIG_KNOB_LCD_PCLK_MHZ; }
uint32_t board_display_frames(void) { return s_frames; }

void board_display_times(uint64_t *render_us, uint64_t *flush_us)
{
    *render_us = s_render_us;
    *flush_us = s_flush_us;
}

uint64_t board_display_flushed_px(void) { return s_flushed_px; }
uint64_t board_display_post_us(void) { return s_post_us; }
