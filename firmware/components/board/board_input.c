/* Touch (CST816) and the rotary ring. */
#include "board.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "input";

#define PIN_I2C_SDA   11
#define PIN_I2C_SCL   12
#define PIN_TOUCH_RST 10
#define TOUCH_ADDR    0x15

/* ---- Touch ------------------------------------------------------------------ */
static i2c_master_dev_handle_t s_touch, s_haptic;
static i2c_master_bus_handle_t s_bus;
static QueueHandle_t s_touch_q;
static void touch_task(void *arg);

/* The two writes that put the chip into normal mode and stop it dozing off by
 * itself. Pulled out of board_touch_init because board_touch_off has to send
 * them again: coming out of the CST816's own deep sleep is a reset, and a reset
 * forgets both. */
static esp_err_t touch_configure(void)
{
    uint8_t normal[] = {0x00, 0x00};   /* normal operating mode */
    uint8_t no_sleep[] = {0xFE, 0x01}; /* don't auto-sleep: a sleeping CST816 misses the first touch */
    esp_err_t err = i2c_master_transmit(s_touch, normal, sizeof normal, 50);
    if (err == ESP_OK) err = i2c_master_transmit(s_touch, no_sleep, sizeof no_sleep, 50);
    return err;
}

/* Hold the chip in reset for a moment, which is how it is woken as well as
 * started. */
static void touch_reset(void)
{
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
}

/* Nothing polls the glass once the dial is in deep sleep, so the touch chip may
 * as well stop too - and it has to be told, because board_touch_init explicitly
 * turns its own auto-sleep off. Left running it would sit there drawing more
 * than the sleeping processor does, which would hand most of the saving back.
 *
 * Only for deep sleep. In light sleep the glass is still read every
 * BOARD_TOUCH_SLEEP_MS, because a tap is what wakes it. */
void board_touch_off(bool off)
{
    if (!s_touch) return;
    if (off) {
        uint8_t sleep_cmd[] = {0xA5, 0x03};
        i2c_master_transmit(s_touch, sleep_cmd, sizeof sleep_cmd, 50);
        return;
    }
    touch_reset();
    touch_configure();
}

esp_err_t board_touch_init(void)
{
    gpio_config_t rst = {.pin_bit_mask = 1ULL << PIN_TOUCH_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rst);
    touch_reset();

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TOUCH_ADDR, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev, &s_touch), TAG, "touch device");

    /* Light sleep isolates every pin by default (see PM_SLP_DISABLE_GPIO in
     * sdkconfig.defaults), and the dial now sleeps between every touch poll.
     * These three cannot be floated for those gaps: reset low is the CST816
     * being reset, over and over, and an I2C pair without its pull-ups can
     * look to a device like a start condition. */
    gpio_sleep_sel_dis(PIN_TOUCH_RST);
    gpio_sleep_sel_dis(PIN_I2C_SDA);
    gpio_sleep_sel_dis(PIN_I2C_SCL);

    esp_err_t err = touch_configure();
    ESP_LOGI(TAG, "CST816 touch %s", err == ESP_OK ? "ready" : esp_err_to_name(err));
    s_touch_q = xQueueCreate(64, sizeof(board_touch_t));
    xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL, 6, NULL, 0);
    return err;
}

static bool touch_read(int *x, int *y)
{
    uint8_t reg = 0x00, d[7] = {0};
    if (i2c_master_transmit_receive(s_touch, &reg, 1, d, sizeof d, 20) != ESP_OK) return false;
    if (!d[2]) return false;  /* number of touch points */
    *x = ((d[3] & 0x0F) << 8) | d[4];
    *y = ((d[5] & 0x0F) << 8) | d[6];
    if (*x >= BOARD_LCD_W) *x = BOARD_LCD_W - 1;
    if (*y >= BOARD_LCD_H) *y = BOARD_LCD_H - 1;
    return true;
}

/* 5 ms awake, so a 30 ms flick gets several samples. 60 ms asleep: the only
 * thing it has to catch then is the tap that wakes the knob, and a tap is on
 * the glass far longer than that. */
#define TOUCH_POLL_MS 5
#define TOUCH_SLEEP_POLL_MS 60
static volatile bool s_touch_asleep;

void board_touch_sleep(bool asleep) { s_touch_asleep = asleep; }

static void touch_task(void *arg)
{
    (void)arg;
    bool was_down = false;
    int lx = -1, ly = -1;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        int x, y;
        bool down = touch_read(&x, &y);
        if (down != was_down || (down && (x != lx || y != ly))) {
            board_touch_t ev = {.t_ms = (uint32_t)(esp_timer_get_time() / 1000), .down = down,
                                .x = (int16_t)(down ? x : lx), .y = (int16_t)(down ? y : ly)};
            xQueueSend(s_touch_q, &ev, 0);
            if (down) { lx = x; ly = y; }
            was_down = down;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(s_touch_asleep ? TOUCH_SLEEP_POLL_MS : TOUCH_POLL_MS));
    }
}

bool board_touch_next(board_touch_t *ev) { return s_touch_q && xQueueReceive(s_touch_q, ev, 0) == pdTRUE; }

/* ---- Haptics: DRV2605 ----------------------------------------------------------- */
#define HAPTIC_ADDR 0x5A

static bool s_haptic_standby;

static esp_err_t haptic_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_haptic, b, 2, 20);
}

static uint8_t haptic_read(uint8_t reg)
{
    uint8_t v = 0;
    i2c_master_transmit_receive(s_haptic, &reg, 1, &v, 1, 20);
    return v;
}

esp_err_t board_haptic_init(void)
{
    if (!s_bus) return ESP_ERR_INVALID_STATE;  /* after board_touch_init */
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = HAPTIC_ADDR, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev, &s_haptic), TAG, "haptic device");
    esp_err_t err = haptic_write(0x01, 0x00);                 /* out of standby, internal trigger */
    if (err == ESP_OK) {
        haptic_write(0x02, 0x00);                             /* no real-time input */
        haptic_write(0x03, 0x01);                             /* ERM effect library 1 (Waveshare's demo) */
        haptic_write(0x0D, 0x00);                             /* no overdrive or sustain offsets */
        haptic_write(0x0E, 0x00);
        haptic_write(0x0F, 0x00);
        haptic_write(0x10, 0x00);                             /* no brake offset */
        haptic_write(0x13, 0x64);                             /* audio-to-vibe max (unused) */
        haptic_write(0x1A, haptic_read(0x1A) & 0x7F);         /* ERM, not LRA */
        haptic_write(0x1D, haptic_read(0x1D) | 0x20);         /* ERM open loop */
    }
    ESP_LOGI(TAG, "DRV2605 haptics %s", err == ESP_OK ? "ready" : esp_err_to_name(err));
    return err;
}

void board_haptic(uint8_t effect)
{
    if (!s_haptic) return;
    if (s_haptic_standby) board_haptic_sleep(false);  /* a click still has to buzz */
    haptic_write(0x04, effect);  /* waveform sequence: this effect, then stop */
    haptic_write(0x05, 0x00);
    haptic_write(0x0C, 0x01);    /* go */
}

/* The DRV2605 out of standby draws a couple of milliamps whether it is buzzing
 * or not, and board_haptic_init takes it out of standby at switch-on and never
 * puts it back. So it sat there drawing all night, on a dial whose whole job
 * overnight is to draw nothing.
 *
 * It is parked when the screen goes off rather than after each buzz: while
 * somebody is turning the knob it should simply be ready, and that is the few
 * minutes a day when its current does not matter. Coming out of standby costs
 * well under a millisecond, far below anything a finger can feel, so a click
 * that arrives while it is parked wakes it first and buzzes anyway.
 *
 * Found on 5 Nov 2026, looking for what was left of the drain after the second
 * processor was silenced. */
void board_haptic_sleep(bool asleep)
{
    if (!s_haptic || asleep == s_haptic_standby) return;
    s_haptic_standby = asleep;
    haptic_write(0x01, asleep ? 0x40 : 0x00);   /* bit 6 is STANDBY */
}

/* ---- Knob ---------------------------------------------------------------------
 * Each direction has its own line, pulled up, that goes low for the length of a
 * detent. Sampled every 3 ms; a pulse that stays low for at least two samples
 * counts when the line goes high again. GPIO 8 = clockwise, GPIO 7 = anticlockwise
 * (Waveshare's encoder demo). */
#define PIN_KNOB_CW  8
#define PIN_KNOB_CCW 7
#define KNOB_SAMPLE_US 3000
#define KNOB_MIN_LOW 2

static atomic_int s_knob;

typedef struct {
    int pin;
    int dir;
    int low;
} knob_line_t;

static knob_line_t s_lines[2] = {{PIN_KNOB_CW, +1, 0}, {PIN_KNOB_CCW, -1, 0}};

static void knob_sample(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2; i++) {
        knob_line_t *l = &s_lines[i];
        if (gpio_get_level(l->pin) == 0) {
            if (l->low < 255) l->low++;
        } else {
            if (l->low >= KNOB_MIN_LOW) atomic_fetch_add(&s_knob, l->dir);
            l->low = 0;
        }
    }
}

static esp_timer_handle_t s_knob_timer;

void board_knob_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_KNOB_CW) | (1ULL << PIN_KNOB_CCW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    /* ...and these two least of all: they are the light-sleep wake source, so
     * isolating them would be isolating the one thing meant to end the sleep. */
    gpio_sleep_sel_dis(PIN_KNOB_CW);
    gpio_sleep_sel_dis(PIN_KNOB_CCW);
    const esp_timer_create_args_t args = {.callback = knob_sample, .name = "knob"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_knob_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_knob_timer, KNOB_SAMPLE_US));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));  /* used while asleep */
}

/* Asleep, sampling two lines 333 times a second is the one thing that would
 * keep the processor out of its idle state the whole time, so the timer stops
 * and the lines raise an interrupt instead. A detent pulls its line low, so a
 * falling edge is a detent; no debounce, because the only job here is to wake
 * the knob and the turn that wakes it is swallowed by the UI. */
/* Level-triggered, not edge-triggered, since 30 Oct 2026 when this was written:
 * a falling edge is never seen in light sleep, because the GPIO peripheral has
 * no clock while the processor is stopped. Only a level does. So the line being
 * low is the trigger, and the handler's first job is to switch itself off -
 * otherwise a detent holding the line low for a few milliseconds would re-enter
 * the handler for as long as it lasted. One detent is all this has to catch:
 * it wakes the dial, board_knob_sleep(false) puts the sampling timer back, and
 * the UI swallows the turn that did the waking. */
static void IRAM_ATTR knob_isr(void *arg)
{
    const knob_line_t *l = arg;
    gpio_intr_disable(l->pin);
    atomic_fetch_add(&s_knob, l->dir);
}

/* Both lines resting high, which is what a knob nobody is touching looks like.
 * Asked before arming a deep-sleep wake on those same lines: arming it with a
 * line already low would wake the dial again the instant it went under. */
bool board_knob_at_rest(void)
{
    return gpio_get_level(PIN_KNOB_CW) && gpio_get_level(PIN_KNOB_CCW);
}

uint64_t board_knob_pin_mask(void) { return (1ULL << PIN_KNOB_CW) | (1ULL << PIN_KNOB_CCW); }

void board_knob_sleep(bool asleep)
{
    if (asleep) {
        esp_timer_stop(s_knob_timer);
        for (int i = 0; i < 2; i++) {
            s_lines[i].low = 0;
            gpio_set_intr_type(s_lines[i].pin, GPIO_INTR_LOW_LEVEL);
            gpio_isr_handler_add(s_lines[i].pin, knob_isr, &s_lines[i]);
            gpio_intr_enable(s_lines[i].pin);
            /* ...and the same level is what brings the processor out of light
             * sleep. Without this the turn would not be felt until something
             * else happened to wake it. */
            gpio_wakeup_enable(s_lines[i].pin, GPIO_INTR_LOW_LEVEL);
        }
        return;
    }
    for (int i = 0; i < 2; i++) {
        gpio_wakeup_disable(s_lines[i].pin);
        gpio_intr_disable(s_lines[i].pin);
        gpio_isr_handler_remove(s_lines[i].pin);
        gpio_set_intr_type(s_lines[i].pin, GPIO_INTR_DISABLE);
        s_lines[i].low = 0;
    }
    esp_timer_start_periodic(s_knob_timer, KNOB_SAMPLE_US);
}

int board_knob_take(void) { return atomic_exchange(&s_knob, 0); }
