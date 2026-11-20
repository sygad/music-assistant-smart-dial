/*
 * Waveshare ESP32-S3-Knob-Touch-LCD-1.8: display, touch, knob and backlight.
 *
 * Pins come from Waveshare's own demo for this board (ESP-IDF 08_LVGL_Test and
 * 04_Encoder_Test). The display's tearing-effect line is not connected on this
 * board, so frames can't be synchronised to the panel refresh.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_W 360
#define BOARD_LCD_H 360

/* Display + LVGL display driver. Render mode and SPI clock come from menuconfig
 * (Bathroom Knob → Display). */
lv_display_t *board_display_init(void);
const char *board_display_mode_name(void);
uint32_t board_display_pclk_mhz(void);

/* Rendered frames since boot, plus cumulative time spent rendering and sending. */
uint32_t board_display_frames(void);
void board_display_times(uint64_t *render_us, uint64_t *flush_us);
uint64_t board_display_flushed_px(void);  /* pixels sent to the panel since boot */
uint64_t board_display_post_us(void);     /* time spent painting the rim and arcs */
void board_display_log_invalidations(int n);  /* log the next n areas marked for redraw */

/* Called with each finished block of pixels just before it's sent, so the UI
 * can paint its top layer (the rim and arcs). */
typedef void (*board_post_render_cb_t)(void *px, const lv_area_t *area, int stride_px);
void board_display_set_post_render(board_post_render_cb_t cb);

/* Debugging: keep a copy of exactly what was sent to the panel (RGB565 in the
 * panel's byte order, 360x360). */
void board_display_shadow_enable(void);
const uint8_t *board_display_shadow(void);

void board_backlight(uint8_t duty);  /* 0 off .. 255 full */
/* Call FIRST, before anything else in app_main: holds the backlight pin low
 * from power-on, which it is not by itself. See the note in board_display.c. */
void board_backlight_off_early(void);

/* Low power: the screen off, and back on again. Going off it fades, which at
 * this size reads as the screen going to sleep rather than the knob dying.
 * Coming back it does nothing immediately - the backlight is raised by the
 * next finished frame, the same way it is at start-up, so what comes up is the
 * screen and never a flash of what was there when it went dark. */
void board_display_sleep(bool asleep);

/* CST816 touch controller on I2C, read every 5 ms by its own task so even a
 * 30 ms flick gets several samples. (Polled once per drawn frame, ~25 ms, most
 * of Darren's flicks got one sample and no speed.) */
typedef struct {
    uint32_t t_ms;  /* when the sample was taken */
    int16_t x, y;
    bool down;
} board_touch_t;
esp_err_t board_touch_init(void);
bool board_touch_next(board_touch_t *ev);  /* next change since the last call, if any */
/* Asleep, the panel is read every BOARD_TOUCH_SLEEP_MS instead of every 5 ms:
 * all it has to catch is the tap that wakes the knob, and the gap is what lets
 * the processor sit idle between taps. */
void board_touch_sleep(bool asleep);
/* Deep sleep only: the chip itself stopped, since nothing is going to read it
 * and board_touch_init deliberately disabled its own auto-sleep. Coming back is
 * a reset, so it is re-configured on the way out. */
void board_touch_off(bool off);

/* DRV2605 vibration driver (ERM motor), on the touch panel's I2C bus. Effects
 * are numbers from the DRV2605 ERM library (1 = strong click, 24 = sharp tick...). */
esp_err_t board_haptic_init(void);
void board_haptic(uint8_t effect);   /* play one effect; returns immediately */
/* Park the haptic driver while the screen is off: out of standby it draws a
 * couple of milliamps doing nothing. A click that lands while it is parked
 * wakes it first, so this is invisible from the outside. */
void board_haptic_sleep(bool asleep);

/* Rotary ring: two lines that each pulse low once per detent (not quadrature). */
void board_knob_init(void);
int board_knob_take(void);  /* detents since the last call, positive = clockwise */
/* Asleep, the 3 ms sampling timer stops and each line's falling edge raises an
 * interrupt instead, so a turn still wakes the knob instantly while nothing
 * runs in between. Direction survives; the count does not need to, because the
 * turn that wakes it is swallowed. */
void board_knob_sleep(bool asleep);
/* Both lines resting high: no hand on the knob. Asked before arming a
 * deep-sleep wake on them, which a line already low would trip at once. */
bool board_knob_at_rest(void);
uint64_t board_knob_pin_mask(void);  /* the two lines, for esp_sleep_enable_ext1_wakeup_io */

/* Battery: a 10K/10K divider across the 5 V rail into GPIO1 (ADC1 channel 0),
 * so the pin sees half the rail. The rail is the cell when it's running on its
 * own and the charger when it's plugged in. */
void board_battery_init(void);
int board_battery_mv(void);  /* the rail in millivolts, or -1 if it can't be read */
