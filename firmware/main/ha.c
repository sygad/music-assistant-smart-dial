/* Home Assistant's REST API: POST /api/services/<domain>/<service> with the
 * long-lived token.
 *
 * Nothing about the amp is worked out here. The power, the volume and the tone
 * controls are the helpers Darren's house already drives the Denon with, so
 * the knob sets a helper and his own automations do the fade, the switches and
 * the amp commands. The knob only ever says what it wants. */
#include "ha.h"
#include "memwatch.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "amp.h"
#include "cJSON.h"
#include "crash.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "ota.h"
#include "secrets_gen.h"
#include "ui.h"

static const char *TAG = "ha";

#define HA_BASE   SECRET_HA_SERVER "/api"
#define HA_MUSIC  "input_boolean.bathroom_music"
/* Updating over Wi-Fi: see ha.h. The switch asks, the text says from where,
 * and the two sensors are what the knob says back. */
#define HA_UPDATE_ASK    "input_boolean.bathroom_dial_update"
#define HA_UPDATE_URL    "input_text.bathroom_dial_firmware_url"
#define HA_FIRMWARE_WAS  "sensor.bathroom_dial_firmware"
#define HA_BATTERY_IS    "sensor.bathroom_dial_battery"
#define HA_BATTERY_MV_IS "sensor.bathroom_dial_battery_mv"
#define HA_RAM_IS        "sensor.bathroom_dial_ram"
#define HA_UPTIME_IS     "sensor.bathroom_dial_uptime"
#define HA_UPDATE_SAYS   "sensor.bathroom_dial_update"
/* Whether the amp can hear, for Home Assistant to wait on rather than guess
 * at - see say_amp. */
#define HA_AMP_IS        "sensor.bathroom_dial_amp"

/* A lost "off" leaves the amps powered until something else notices, while a
 * lost "on" is asked again by the next touch, so a failed call is retried. */
#define HA_TRIES     3
#define HA_RETRY_MS  1000
/* The volume is turned, not set: a hand on the knob moves it a dozen times a
 * second and only where it has got to matters. One request at a time, and no
 * closer together than this. */
#define HA_NUMBER_GAP_MS 150
/* How often the volume helper is read back, and how long after the knob's own
 * write to leave it alone. The write goes through Darren's Volume Slider
 * automation and his Denon script, and the amp can take seconds to answer, so
 * a read any sooner catches the value from before the one just sent and would
 * drag the arc backwards under the hand that is still turning. */
#define HA_VOLUME_POLL_MS   1000

/* The shortest gap between two volume writes (2 Nov 2026). Without it the knob
 * wrote the helper as fast as Home Assistant would answer - about seven times
 * a second while a hand is turning - and automation.bathroom_volume_slider is
 * mode restart, so each of those cancelled the HTTP request still in flight to
 * the Denon. A stream of aborted connections is what the amp cannot take: its
 * integration's poll started timing out, the entity flapped between on and
 * unavailable every fifteen seconds, and each recovery republished a stale
 * volume which Sync Volume From Amp wrote straight back into the helper. The
 * volume then fought the hand - 21 jumping to 72 and having to be wound back
 * down - which is what Darren met when he put the dial back on the wall.
 *
 * 250 ms, because the Denon answers an MV command in 13-17 ms and this gives
 * the script fifteen times that to finish before anything cancels it, while
 * still following the hand closely enough to feel live. Only the newest value
 * is ever held, so what lands is always where the hand has got to, and the
 * value a hand settles on is never dropped - only delayed, by at most this. */
#define HA_VOLUME_MIN_GAP_MS 250
#define HA_VOLUME_SETTLE_MS 2500
/* ...and how long to go on waiting for the knob's own write to come home
 * before believing the helper again (Darren, 2 Nov 2026). The amp dropped to
 * unavailable and back four times in a minute, and the denonavr integration
 * polls every ten seconds, so it answered with the volume from *before* the
 * dial's write long after the settle window had passed. Every one of those
 * stale answers was adopted as somebody else's hand: the volume walked itself
 * down a step a second and overruled the hand turning it up. A reading is
 * only news once the knob's own value has been seen to arrive - or once this
 * long has gone by, so a write that genuinely never lands cannot wedge the
 * dial deaf for good. */
#define HA_VOLUME_CONFIRM_MS 20000

/* How often the knob asks whether an update has been requested. Awake it
 * rides along with the volume poll; asleep the task wakes just for this,
 * because a dial on a wall must be updatable without somebody going to the
 * bathroom to wake it first. Ten seconds is unnoticeable to a hand on a
 * keyboard and costs one small request. */
#define HA_UPDATE_POLL_MS 10000

static const char *const NUMBER_ENTITY[HA_NUMBERS] = {
    [HA_VOLUME] = "input_number.bathroom_amp_volume",
    [HA_TREBLE] = "input_number.bathroom_amp_treble",
    [HA_BASS] = "input_number.bathroom_amp_bass",
    [HA_SUBWOOFER] = "input_number.bathroom_amp_subwoofer",
};

/* The latest value asked for, per helper, and whether it is still to go out.
 * A value rather than a queue: when the hand has moved on, the step it passed
 * through is not worth a request. */
static _Atomic int s_number[HA_NUMBERS];
static _Atomic bool s_number_due[HA_NUMBERS];

/* The volume read back. s_volume_known is the value the knob and Home
 * Assistant last agreed on - whichever of them said it - so a reading that
 * matches it is the knob's own write coming home and means nothing. */
static _Atomic int s_volume_known = -1;
static _Atomic int s_volume_moved = -1;
static _Atomic bool s_volume_follow = true;
static int64_t s_volume_settled_at;  /* ha_task only, as are the three below */
static bool s_volume_unconfirmed;    /* the knob's own write has yet to come home */
static int64_t s_volume_asked_at;
static int64_t s_volume_sent_at;
static int s_volume_seen = -1;       /* what the previous poll read */

/* One slot, overwritten: while a call is in flight any number of asks can pile
 * up behind it and only the last of them means anything. */
static QueueHandle_t s_power;
static TaskHandle_t s_task;
/* Whether the last "on" actually reached Home Assistant. The screen is live
 * and being used while Wi-Fi is still coming up, so an ask can be made with
 * nowhere to send it, and the repeat window in app_main must not count that
 * as done (Darren, 4 Nov 2026). */
static _Atomic bool s_power_landed;

static bool post_json(const char *url, const char *body, char *out, int out_max)
{
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_POST, .timeout_ms = 8000,
                                    .buffer_size_tx = 2048};  /* the token alone is ~400 characters */
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", "Bearer " SECRET_HA_TOKEN);
    int len = (int)strlen(body);
    bool ok = false;
    if (esp_http_client_open(c, len) == ESP_OK && esp_http_client_write(c, body, len) == len &&
        esp_http_client_fetch_headers(c) >= 0) {
        int status = esp_http_client_get_status_code(c);
        ok = status == 200;
        if (out && out_max > 0) {
            int n = 0, r;
            while (n < out_max - 1 && (r = esp_http_client_read(c, out + n, out_max - 1 - n)) > 0) n += r;
            out[n] = 0;
        }
        if (!ok) ESP_LOGW(TAG, "%s: HTTP %d", url, status);
    } else {
        ESP_LOGW(TAG, "%s: request failed", url);
    }
    esp_http_client_cleanup(c);
    return ok;
}

static bool call_service(const char *domain, const char *service, const char *entity, const char *extra)
{
    char url[128], body[160];
    snprintf(url, sizeof url, HA_BASE "/services/%s/%s", domain, service);
    snprintf(body, sizeof body, "{\"entity_id\":\"%s\"%s}", entity, extra ? extra : "");
    return post_json(url, body, NULL, 0);
}

bool ha_read_number(ha_number_t which, int *value)
{
    if (which < 0 || which >= HA_NUMBERS) return false;
    char url[160];
    snprintf(url, sizeof url, HA_BASE "/states/%s", NUMBER_ENTITY[which]);
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_GET, .timeout_ms = 8000,
                                    .buffer_size_tx = 2048};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Authorization", "Bearer " SECRET_HA_TOKEN);
    char body[512];
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0 &&
        esp_http_client_get_status_code(c) == 200) {
        int n = 0, r;
        while (n < (int)sizeof body - 1 && (r = esp_http_client_read(c, body + n, (int)sizeof body - 1 - n)) > 0)
            n += r;
        body[n] = 0;
        /* "state":"-4.0" - the one field worth a parser is a number in quotes */
        const char *p = strstr(body, "\"state\":\"");
        if (p) {
            *value = (int)lroundf(strtof(p + 9, NULL));
            ok = true;
        }
    }
    esp_http_client_cleanup(c);
    if (!ok) ESP_LOGW(TAG, "read %s failed", NUMBER_ENTITY[which]);
    return ok;
}

/* Any entity's state as text, for the two helpers the update mechanism reads:
 * one holds a URL and the other "on" or "off", so neither is a number.
 * Blocking, like ha_read_number, and on the same task. */
static bool read_state(const char *entity_id, char *out, int out_max)
{
    char url[160];
    snprintf(url, sizeof url, HA_BASE "/states/%s", entity_id);
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_GET, .timeout_ms = 8000,
                                    .buffer_size_tx = 2048};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Authorization", "Bearer " SECRET_HA_TOKEN);
    char body[768];  /* a URL plus the attributes Home Assistant wraps it in */
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0 &&
        esp_http_client_get_status_code(c) == 200) {
        int n = 0, r;
        while (n < (int)sizeof body - 1 && (r = esp_http_client_read(c, body + n, (int)sizeof body - 1 - n)) > 0)
            n += r;
        body[n] = 0;
        const char *p = strstr(body, "\"state\":\"");
        if (p) {
            p += 9;
            int i = 0;
            /* The state is the first JSON string in the answer and holds no
             * escapes worth handling: a URL has none, and "on" has none. */
            while (p[i] && p[i] != '"' && i < out_max - 1) { out[i] = p[i]; i++; }
            out[i] = 0;
            ok = true;
        }
    }
    esp_http_client_cleanup(c);
    if (!ok) ESP_LOGW(TAG, "read %s failed", entity_id);
    return ok;
}

/* Tell Home Assistant something about the knob, as a sensor of its own, so
 * what the knob is doing can be watched from a phone instead of the bathroom.
 * A state set this way lives until Home Assistant restarts, which is all it
 * needs to - it is set again on every boot and on every step of an update. */
static void report(const char *entity_id, const char *state, const char *name, const char *icon)
{
    char url[160], body[320];
    snprintf(url, sizeof url, HA_BASE "/states/%s", entity_id);
    snprintf(body, sizeof body,
             "{\"state\":\"%s\",\"attributes\":{\"friendly_name\":\"%s\",\"icon\":\"%s\"}}",
             state, name, icon);
    post_json(url, body, NULL, 0);
}

void ha_report_firmware(void)
{
    /* The slot goes with the version, and not just as a curiosity: Home
     * Assistant records nothing at all when a state is posted with the value
     * and attributes it already had, so re-reporting the same version after a
     * reboot left no trace whatsoever and an update that had plainly worked
     * looked like nothing had happened (2 Nov 2026). The slot alternates on
     * every successful update, so there is always something new to say. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    char url[160], body[320];
    snprintf(url, sizeof url, HA_BASE "/states/%s", HA_FIRMWARE_WAS);
    snprintf(body, sizeof body,
             "{\"state\":\"%s\",\"attributes\":{\"friendly_name\":\"Dial Firmware\","
             "\"icon\":\"mdi:chip\",\"slot\":\"%s\"}}",
             esp_app_get_description()->version, running ? running->label : "?");
    post_json(url, body, NULL, 0);
}

/* Why the dial last restarted, and if it fell over, where (crash.h). A crash
 * goes in as the state "panic" with the message, the task and the call chain
 * as attributes; the chain decodes against the build's ELF with addr2line. */
#define HA_RESTART_WAS "sensor.bathroom_dial_last_restart"

void ha_report_restart(void)
{
    static char reason[120], task[16], pcs[240];
    bool crashed = crash_take(reason, sizeof reason, task, sizeof task, pcs, sizeof pcs);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "state", crash_reset_name());
    cJSON *at = cJSON_AddObjectToObject(body, "attributes");
    cJSON_AddStringToObject(at, "friendly_name", "Dial Last Restart");
    cJSON_AddStringToObject(at, "icon", crashed ? "mdi:alert-octagon" : "mdi:restart");
    cJSON_AddStringToObject(at, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(at, "crash", crashed ? reason : "");
    cJSON_AddStringToObject(at, "task", crashed ? task : "");
    cJSON_AddStringToObject(at, "backtrace", crashed ? pcs : "");
    cJSON_AddNumberToObject(at, "internal_ram_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return;
    if (crashed) ESP_LOGW(TAG, "last boot crashed in %s: %s at %s", task, reason, pcs);
    post_json(HA_BASE "/states/" HA_RESTART_WAS, json, NULL, 0);
    cJSON_free(json);
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void send_numbers(void)
{
    for (int i = 0; i < HA_NUMBERS; i++) {
        if (!atomic_exchange(&s_number_due[i], false)) continue;
        /* Too soon after the last one: put it back and let the loop come round
         * again. The value itself is never lost - ha_set_number keeps only the
         * newest, so what finally goes is where the hand has got to. */
        if (i == HA_VOLUME && now_ms() - s_volume_sent_at < HA_VOLUME_MIN_GAP_MS) {
            atomic_store(&s_number_due[i], true);
            continue;
        }
        /* The amp cannot hear yet (amp.h): hold the volume rather than spend it
         * on an amp that will throw it away. Held, not dropped - the newest
         * value is kept, so the moment the amp answers, where the hand got to
         * while it was deaf is what lands. */
        if (i == HA_VOLUME && !amp_ready()) {
            atomic_store(&s_number_due[i], true);
            continue;
        }
        int value = atomic_load(&s_number[i]);
        char extra[32];
        snprintf(extra, sizeof extra, ",\"value\":%d", value);
        call_service("input_number", "set_value", NUMBER_ENTITY[i], extra);
        if (i == HA_VOLUME) {
            s_volume_sent_at = now_ms();
            atomic_store(&s_volume_known, value);
            s_volume_settled_at = now_ms() + HA_VOLUME_SETTLE_MS;
            s_volume_unconfirmed = true;
            s_volume_asked_at = now_ms();
            s_volume_seen = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(HA_NUMBER_GAP_MS));
    }
}

/* Ask what the volume is now, and keep it only if it is news. At start-up
 * nothing has been agreed yet, so the first answer is always news: that is
 * what puts the knob where the amp already is. */
static void read_volume_back(void)
{
    if (!atomic_load(&s_volume_follow) || now_ms() < s_volume_settled_at) return;
    int mv;
    if (!ha_read_number(HA_VOLUME, &mv)) { s_volume_seen = -1; return; }
    if (mv == atomic_load(&s_volume_known)) {  /* the knob's own value coming home */
        s_volume_unconfirmed = false;
        s_volume_seen = mv;
        return;
    }
    /* Our own write has not come home yet, so a different value is the one it
     * is replacing rather than a hand somewhere else. */
    if (s_volume_unconfirmed && now_ms() - s_volume_asked_at < HA_VOLUME_CONFIRM_MS) return;
    /* And only a value two polls agree on: a single odd answer is the amp
     * coming back from a drop-out, not somebody moving the volume. */
    if (mv != s_volume_seen) { s_volume_seen = mv; return; }
    s_volume_unconfirmed = false;
    atomic_store(&s_volume_known, mv);
    atomic_store(&s_volume_moved, mv);
    ESP_LOGI(TAG, "volume read back as %d", mv);
}

/* ---- Updating over Wi-Fi: the asking end (see ha.h) --------------------------- */
static int s_said_state = -1, s_said_percent = -2;
static unsigned s_said_attempt;  /* so a repeat of the same failure is still news */

/* The battery, as the screen reads it. Stored here by the LVGL thread (the ADC
 * is read there, with everything else that touches the board) and reported by
 * the task below. Added 3 Nov 2026 after the knob went flat in a day on the
 * wall and nobody could say whether that was normal, because nothing anywhere
 * recorded the charge over time. A number in Home Assistant turns the question
 * into a graph. */
static _Atomic int s_batt_percent = -1;
static _Atomic int s_batt_mv = -1;
static _Atomic bool s_batt_charging;
/* Each sensor's own last-said value and the moment it said it (see "one
 * sensor, one function" below): the charge and the rail keep theirs here
 * because ha_battery writes the values from the LVGL thread, and the RAM and
 * the uptime keep theirs inside their own functions. */
static int s_said_charge = -2;
static bool s_said_charging;
static int64_t s_said_charge_at;
static int s_said_rail = -1;
static int64_t s_said_rail_at;

/* ---- Reporting: one sensor, one function, one clock (7 Nov 2026) --------------
 * Each of these says one thing to Home Assistant. Each keeps its own clock and
 * its own reasons to stay quiet, and update_task calls every one of them
 * directly. Nothing is reported from inside the reporting of something else.
 *
 * It was not built that way, and the whole of 7 Nov 2026 went on the
 * consequence. The RAM and the uptime were posted from inside the charge's
 * function, behind its early return - and the charge is only measured with the
 * screen off (battery_report in app_main.c: a loaded rail says nothing about
 * the cell). So on a boot woken by a hand there was no charge figure, that
 * function returned at its first line, and the two sensors sitting behind it
 * were never sent at all. The dial went silent about itself for exactly as
 * long as somebody was using it, and tools/fw.sh read that silence as deep
 * sleep and refused to publish to a dial that was lit and in Darren's hand.
 *
 * No bundling, no shared gate, no ordering between them: a sensor with nothing
 * to say stops, and takes nothing else with it.
 *
 * They are all said again on a five minute clock whether the value moved or
 * not. A sensor the dial creates by posting to it exists only in Home
 * Assistant's running state - restart HA and it is gone, not unavailable,
 * gone, until the dial next posts. Posting only on a change meant a steady
 * charge never brought the sensor back, which ate a drain measurement when the
 * auto-update automation restarted HA mid-test (5 Nov 2026). Twelve posts an
 * hour each, against the sixty the ten-second poll already makes while the
 * screen is on, and none at all while the dial is asleep, because the task is
 * not running to make them. */
#define SAY_AGAIN_US  (5 * 60 * 1000000LL)

/* Whether this sensor's own clock has come round. A pure question: the caller
 * keeps the stamp and decides what to do, so one sensor can never move
 * another's clock on or hold it back. */
static bool say_again(int64_t said_at)
{
    return !said_at || esp_timer_get_time() - said_at >= SAY_AGAIN_US;
}

void ha_battery(int percent, int mv, bool charging)
{
    atomic_store(&s_batt_percent, percent);
    atomic_store(&s_batt_mv, mv);
    atomic_store(&s_batt_charging, charging);
}

/* The charge, as a percentage. Nothing has measured it until the screen has
 * been off once this boot, and until then this says nothing: that is the
 * screen's doing, not a fault here, and no other sensor waits on it. */
static void say_charge(void)
{
    int pc = atomic_load(&s_batt_percent);
    bool charging = atomic_load(&s_batt_charging);
    if (pc < 0) return;
    bool news = pc != s_said_charge || charging != s_said_charging;
    if (!news && !say_again(s_said_charge_at)) return;
    s_said_charge = pc;
    s_said_charging = charging;
    s_said_charge_at = esp_timer_get_time();
    char url[160], body[320], text[8];
    snprintf(text, sizeof text, "%d", pc > 100 ? 100 : pc);  /* a percentage, not a measurement */
    snprintf(url, sizeof url, HA_BASE "/states/%s", HA_BATTERY_IS);
    snprintf(body, sizeof body,
             "{\"state\":\"%s\",\"attributes\":{\"friendly_name\":\"Dial Battery\","
             "\"device_class\":\"battery\",\"unit_of_measurement\":\"%%\","
             "\"state_class\":\"measurement\",\"charging\":%s}}",
             text, charging ? "true" : "false");
    post_json(url, body, NULL, 0);
}

/* The rail itself, unscaled, and its own sensor rather than a tail on the
 * charge's (7 Nov 2026). The percentage is 3300-4200 mV read as a straight
 * line, which a lithium cell is not; these are the millivolts the resting rail
 * actually measured, at nine times the percentage's resolution, so the shape
 * of a discharge can be read off a graph instead of inferred from a figure
 * that quantises to 9 mV (6 Nov 2026). */
static void say_rail(void)
{
    int mv = atomic_load(&s_batt_mv);
    if (mv <= 0) return;
    bool news = mv != s_said_rail;
    if (!news && !say_again(s_said_rail_at)) return;
    s_said_rail = mv;
    s_said_rail_at = esp_timer_get_time();
    char url[160], body[320];
    snprintf(url, sizeof url, HA_BASE "/states/%s", HA_BATTERY_MV_IS);
    snprintf(body, sizeof body,
             "{\"state\":\"%d\",\"attributes\":{\"friendly_name\":\"Dial Battery Millivolts\","
             "\"device_class\":\"voltage\",\"unit_of_measurement\":\"mV\","
             "\"state_class\":\"measurement\",\"icon\":\"mdi:sine-wave\"}}",
             mv);
    post_json(url, body, NULL, 0);
}

/* The largest contiguous block of internal RAM (6 Nov 2026).
 *
 * Because the one thing that would have answered the OTA question in a minute
 * instead of two mornings is a graph of this. The dial runs with about 2.5 KB
 * as its biggest internal block and has been down to 228 bytes, which is close
 * enough to the edge that it is worth watching rather than assuming: Wi-Fi
 * failing to get a buffer looks like a network fault, not a memory one.
 *
 * The LARGEST BLOCK, not the free total, is the state. The total is what made
 * this invisible - plenty free, none of it usable in one piece - so the number
 * that matters is the one an allocation actually has to fit in. The total and
 * the lowest the heap has ever been go in as attributes. PSRAM goes in beside
 * it: it was the heap that ran out on voice search that same morning, and
 * nothing in Home Assistant could have shown it. below_floor is the one to put
 * an alert on (memwatch.h).
 *
 * Said on the clock and nothing else. It had a dedupe of its own once - only a
 * move of 256 bytes counted as news - which could only ever stop the sensor
 * being said at all, and a sensor that is never said is a sensor that never
 * comes back from a Home Assistant restart. */
static void say_ram(void)
{
    static int64_t said_at;
    if (!say_again(said_at)) return;
    said_at = esp_timer_get_time();
    mem_now_t m = mem_now();
    bool low = mem_below_floor();
    if (low)
        ESP_LOGW(TAG, "memory below its floor: internal block %u (floor %u), PSRAM block %u (floor %u)",
                 (unsigned)m.int_largest, (unsigned)MEM_FLOOR_INTERNAL_BLOCK, (unsigned)m.ps_largest,
                 (unsigned)MEM_FLOOR_PSRAM_BLOCK);
    char url[160], body[480];
    snprintf(url, sizeof url, HA_BASE "/states/%s", HA_RAM_IS);
    snprintf(body, sizeof body,
             "{\"state\":\"%u\",\"attributes\":{\"friendly_name\":\"Dial Internal RAM Block\","
             "\"unit_of_measurement\":\"B\",\"state_class\":\"measurement\",\"icon\":\"mdi:memory\","
             "\"free_total\":%u,\"lowest_ever\":%u,"
             "\"psram_block\":%u,\"psram_free\":%u,\"psram_lowest_ever\":%u,\"below_floor\":%s}}",
             (unsigned)m.int_largest, (unsigned)m.int_free, (unsigned)m.int_lowest, (unsigned)m.ps_largest,
             (unsigned)m.ps_free, (unsigned)m.ps_lowest, low ? "true" : "false");
    post_json(url, body, NULL, 0);
}

/* How long since this boot, in seconds (6 Nov 2026).
 *
 * Because the question "did the dial deep-sleep last night?" could not be
 * answered from anything the dial already said, and it is the question that
 * decides the battery. sensor.bathroom_dial_last_restart looked like the
 * answer and is not: Home Assistant records nothing when a state is posted
 * with the value and the attributes it already had, so a run of identical
 * overnight boots leaves one record or none - the same trap ha_report_firmware
 * has a comment about, fallen into again.
 *
 * Uptime cannot be ambiguous. It climbs while the dial runs and drops to
 * nothing every time it boots, and a deep-sleep wake is a boot. One graph of
 * this says exactly when the dial went under and how long for, with no
 * inference and nothing to argue about.
 *
 * It is also the only honest answer to "is the dial on the network?", and
 * tools/fw.sh asks it that before publishing. Every post is a different
 * number, so Home Assistant always records one, and its timestamp is always
 * real. The charge cannot answer that question - it is not measured while the
 * dial is in use, and an unchanged state posted again leaves no mark at all. */
static void say_uptime(void)
{
    static int64_t said_at;
    if (!say_again(said_at)) return;
    said_at = esp_timer_get_time();
    char text[16];
    snprintf(text, sizeof text, "%lld", esp_timer_get_time() / 1000000);
    char url[160], body[320];
    snprintf(url, sizeof url, HA_BASE "/states/%s", HA_UPTIME_IS);
    snprintf(body, sizeof body,
             "{\"state\":\"%s\",\"attributes\":{\"friendly_name\":\"Dial Uptime\","
             "\"device_class\":\"duration\",\"unit_of_measurement\":\"s\","
             "\"state_class\":\"measurement\",\"icon\":\"mdi:timer-outline\"}}",
             text);
    post_json(url, body, NULL, 0);
}

/* Whether the amp can hear, as a sensor in Home Assistant (10 Nov 2026).
 *
 * The dial is the only thing in the house that asks the amp the honest
 * question - one GET on its status page, every two seconds (amp.h) - and until
 * now it kept the answer to itself. Home Assistant had no way to ask, so
 * script.bathroom_music_amp_ready counted 150 seconds from the mains switch
 * instead and assumed the amp was deaf for all of them. On 10 Nov 2026 the amp
 * answered at 56 seconds, Darren set the volume in the shower a second later,
 * and at 151 seconds the script wrote the 30 starting level over him. The dial
 * had known the amp was awake for a minute and a half and had nowhere to say
 * it. This is that nowhere fixed: one probe, one answer, and the script waits
 * on this sensor instead of on a clock.
 *
 * Only on a change, like say_update, and only remembered once the post has
 * landed - a transition lost to a Wi-Fi blip would leave Home Assistant
 * waiting out the whole timeout for nothing.
 *
 * "unknown" means nothing is being asked: the music is off, or the screen is
 * (amp_awake). A dial that is asleep, flat or rebooting leaves this sensor at
 * whatever it last said, which is why the script keeps a timeout behind its
 * wait rather than trusting this forever. */
static void say_amp(void)
{
    static int said = -1;
    amp_state_t st = amp_get_state();
    if ((int)st == said) return;
    static const char *NAME[] = {
        [AMP_UNKNOWN] = "unknown",
        [AMP_CONNECTING] = "connecting",
        [AMP_READY] = "ready",
    };
    char url[160], body[320];
    snprintf(url, sizeof url, HA_BASE "/states/%s", HA_AMP_IS);
    snprintf(body, sizeof body,
             "{\"state\":\"%s\",\"attributes\":{\"friendly_name\":\"Dial Amp\","
             "\"icon\":\"mdi:amplifier\",\"waiting_s\":%d}}",
             NAME[st], amp_connecting_secs());
    if (post_json(url, body, NULL, 0)) said = (int)st;
}

/* What the knob says it is doing, as a sensor in Home Assistant. Only on a
 * change, so a download reports about once a second rather than per read. */
static void say_update(void)
{
    int pc;
    ota_fail_t why;
    ota_state_t st = ota_state(&pc, &why);
    unsigned try = ota_attempt();
    if ((int)st == s_said_state && pc == s_said_percent && try == s_said_attempt) return;
    s_said_state = (int)st;
    s_said_percent = pc;
    s_said_attempt = try;
    char text[96];
    switch (st) {
    case OTA_CONNECTING: snprintf(text, sizeof text, "fetching"); break;
    case OTA_DOWNLOADING: snprintf(text, sizeof text, "%d%%", pc < 0 ? 0 : pc); break;
    case OTA_INSTALLING: snprintf(text, sizeof text, "installing"); break;
    /* With the attempt number, so asking again after a failure says something
     * new even when it fails the same way. Without it the 6 Nov failure was
     * invisible: tools/fw.sh waits for a state different from the one already
     * there, and got the same words back for five minutes. */
    case OTA_FAILED: snprintf(text, sizeof text, "failed: %s (try %u)", ota_fail_text(why), try); break;
    case OTA_IDLE:
    default: snprintf(text, sizeof text, "idle"); break;
    }
    report(HA_UPDATE_SAYS, text, "Dial Update", "mdi:cloud-download");
}

/* Has an update been asked for? The switch is turned off here, before the
 * download starts, for two reasons: it tells Darren the knob heard him, and a
 * switch left on would be read as another request by the new firmware the
 * moment it booted - an update loop that would need a cable to break. */
static void check_update(void)
{
    char ask[8] = "";
    if (!read_state(HA_UPDATE_ASK, ask, sizeof ask)) return;
    if (strcmp(ask, "on") != 0) return;
    ESP_LOGI(TAG, "an update has been asked for");
    call_service("input_boolean", "turn_off", HA_UPDATE_ASK, NULL);
    char url[256] = "";
    if (!read_state(HA_UPDATE_URL, url, sizeof url) || strncmp(url, "http", 4) != 0) {
        ESP_LOGE(TAG, "%s does not hold a firmware address (\"%s\")", HA_UPDATE_URL, url);
        ota_begin("");  /* says so on the screen and in Home Assistant, rather than nothing */
        return;
    }
    ota_begin(url);
}

/* The update check gets a task of its own (2 Nov 2026). It was sharing the
 * task that sends the volume, and both of its calls are blocking HTTP with an
 * eight second timeout, so a slow answer from Home Assistant sat directly in
 * front of the next volume write. The volume is the one thing here with a hand
 * waiting on it; an update check can afford to wait for anything. Keeping them
 * apart means a stall in one can never be felt in the other. */
/* Whether the music has been turned off by something other than the dial.
 *
 * The dial writes input_boolean.bathroom_music and never used to read it, so
 * it had no idea when anything else changed it - and something else does, on
 * a timer: Music Auto Off turns it off after twenty minutes of an empty
 * bathroom, which fades the volume, stops the stream and powers the amp and
 * the sub down. All of that is clean. The dial simply was not part of it, and
 * went on believing music was playing and asking an amp that had just lost
 * power whether it was there (Darren, 4 Nov 2026: "severing the connection
 * between the dial and the amps with an automation just targeting the amps,
 * that's not clean").
 *
 * Read on the same poll as the update switch, so it costs no extra wake-ups. */
static _Atomic bool s_music_off_seen;
static char s_music_was[8];
/* Nothing is known about the helper until it has been read once. */
static bool s_music_read;
/* When the dial last asked for the music on, so a read that crosses its own
 * write can be told from a helper that is genuinely off. */
static _Atomic int64_t s_music_on_at;

bool ha_music_turned_off(void) { return atomic_exchange(&s_music_off_seen, false); }

/* How long after asking for the music on a reading of "off" is disbelieved.
 * The ask goes through a queue and up to HA_TRIES attempts, and this poll's
 * HTTP read can have been issued before any of that landed. The ask is
 * repeated every 30 s while the dial is in use, so nothing is lost by waiting
 * out one window - whereas acting on the stale answer powers the amps down
 * under a hand that has just reached for them. */
#define HA_MUSIC_ON_SETTLE_MS 5000

static void check_music(void)
{
    char st[8];
    if (!read_state(HA_MUSIC, st, sizeof st)) return;
    bool off = !strcmp(st, "off");

    /* The first answer of a boot is the state of the helper, not a change to
     * it (Darren, 5 Nov 2026: a cold start reached the menu and jumped
     * straight to the clock). s_music_was starts empty, so "off" did not match
     * it and the dial reported the music as having just been turned off -
     * every boot, with nothing having happened. On its own that was harmless,
     * because the dial only acts on it once it believes the music is on; but
     * the first touch of a cold start turns the music on, and this poll runs
     * about where the opening sequence ends, so the two met and ui.c put the
     * clock up three seconds into the menu. A dial that has just started has
     * no previous belief for anything to have changed from. */
    if (!s_music_read) {
        s_music_read = true;
        snprintf(s_music_was, sizeof s_music_was, "%s", st);
        return;
    }

    /* ...and the same meeting again, later: the dial asks for the music on and
     * this read catches the helper from before the ask arrived. Leave
     * s_music_was alone so a helper that really is off is still caught on the
     * next poll. */
    if (off && now_ms() - atomic_load(&s_music_on_at) < HA_MUSIC_ON_SETTLE_MS) return;

    if (off && strcmp(s_music_was, "off") == 0) return;  /* already knew */
    if (off) {
        atomic_store(&s_music_off_seen, true);
        ESP_LOGI(TAG, "music was turned off in Home Assistant");
    }
    snprintf(s_music_was, sizeof s_music_was, "%s", st);
}

static void update_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (ota_busy()) {
            say_update();  /* the download owns the network; just report it */
        } else {
            check_update();
            check_music();
            /* One call per sensor, each with its own clock inside it: none of
             * them can silence another (see "one sensor, one function"). */
            say_update();
            say_amp();
            say_charge();
            say_rail();
            say_ram();
            say_uptime();
        }
        /* Ten seconds while the screen is on, a minute while it is off. It was
         * ten seconds either way when this task was written (2 Nov 2026), which
         * is 8,640 radio wake-ups a day for a knob that spends nearly all of
         * them asleep on a wall - and the day after, Darren found it flat. A
         * minute still means an update lands unattended; it just stops the
         * knob waking six times a minute all night for nothing.
         *
         * The wait is at the end rather than the start (4 Nov 2026) so the
         * first look happens as soon as there is a network. That matters for
         * the deep-sleep check-in, which wakes behind a dark screen, has about
         * half a minute to find out whether a firmware is waiting, and then
         * goes back down for hours. */
        vTaskDelay(pdMS_TO_TICKS(atomic_load(&s_volume_follow) ? HA_UPDATE_POLL_MS
                                                               : HA_UPDATE_POLL_MS * 6));
    }
}

static void ha_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* A wait with a timeout rather than forever: the same task that sends
         * also does the reading, so a quiet second is a read. */
        /* Asleep and following nothing: wait to be poked, as this task always
         * did. The update check no longer lives here, so there is nothing left
         * that needs a heartbeat (update_task above). */
        TickType_t wait = atomic_load(&s_volume_follow) ? pdMS_TO_TICKS(HA_VOLUME_POLL_MS)
                                                        : portMAX_DELAY;
        /* Unless a volume is waiting out its minimum gap: come back the moment
         * it is allowed to go, rather than a whole poll later. */
        if (atomic_load(&s_number_due[HA_VOLUME])) {
            int64_t due_in = HA_VOLUME_MIN_GAP_MS - (now_ms() - s_volume_sent_at);
            if (due_in < 1) due_in = 1;
            /* Waiting on the amp rather than on the gap: look again soon, so a
             * held volume goes out as the amp comes up rather than a poll later. */
            if (!amp_ready() && due_in < 1000) due_in = 1000;
            if (pdMS_TO_TICKS(due_in) < wait) wait = pdMS_TO_TICKS(due_in);
        }
        ulTaskNotifyTake(pdTRUE, wait);
        /* An update owns the network while it runs. */
        if (ota_busy()) continue;
        /* Here as well as in update_task, because update_task's own clock is
         * ten seconds while the screen is on and a minute while it is off,
         * and Home Assistant is waiting on this to set the volume. This task
         * wakes every second while the dial is following the volume - which is
         * exactly the switch-on the script is waiting through - so "ready"
         * reaches Home Assistant about a second after the amp answers rather
         * than up to ten. Costs nothing on a quiet loop: say_amp returns
         * without a request unless the answer has changed. */
        say_amp();
        uint8_t on;
        if (xQueueReceive(s_power, &on, 0) == pdTRUE) {
            bool sent = false;
            /* Timed from the ask, not from the answer: the read that has to be
             * disbelieved is one already in flight by the time this returns. */
            if (on) atomic_store(&s_music_on_at, now_ms());
            for (int attempt = 0; attempt < HA_TRIES; attempt++) {
                if (call_service("input_boolean", on ? "turn_on" : "turn_off", HA_MUSIC, NULL)) {
                    ESP_LOGI(TAG, "POWER %s", on ? "on" : "off");
                    sent = true;
                    break;
                }
                if (uxQueueMessagesWaiting(s_power)) break;  /* a newer ask has overtaken it */
                if (attempt + 1 < HA_TRIES) vTaskDelay(pdMS_TO_TICKS(HA_RETRY_MS));
            }
            if (on) {
                atomic_store(&s_power_landed, sent);
                if (!sent) ESP_LOGW(TAG, "POWER on did not land; the next touch asks again");
            } else if (!sent) {
                /* Said out loud, because the dial's quarter-hour let-go asks
                 * only once: an off that failed here used to leave nothing in
                 * the log, just an amp still warm (5 Nov 2026). Home
                 * Assistant's twenty minutes of empty room still catches it. */
                ESP_LOGW(TAG, "POWER off did not land; the amps stay on until Music Auto Off");
            }
        }
        send_numbers();
        read_volume_back();
    }
}

/* The ask queue exists from boot, before the network. The dial is touchable
 * within a second of the plug going in and Wi-Fi takes several, so the first
 * touch used to find no queue and its "on" was dropped without a word - the
 * reason a cold-started dial browsed for half a minute with the amps still
 * off (Darren, 4 Nov 2026). Held here instead, and sent by ha_task the moment
 * it starts. */
void ha_hold_asks(void)
{
    if (!s_power) s_power = xQueueCreate(1, sizeof(uint8_t));
}

void ha_start(void)
{
    if (s_task) return;
    ha_hold_asks();
    xTaskCreatePinnedToCoreWithCaps(ha_task, "ha", 5120, NULL, 4, &s_task, 0, MALLOC_CAP_SPIRAM);
    /* Lower priority than the volume task: nothing waits on an update check. */
    xTaskCreatePinnedToCoreWithCaps(update_task, "haupd", 5120, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
}

static void poke(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void ha_music_power(bool on)
{
    uint8_t v = on ? 1 : 0;
    if (!s_power) return;
    if (on) atomic_store(&s_power_landed, false);  /* until it is seen to go out */
    xQueueOverwrite(s_power, &v);
    poke();
}

bool ha_music_power_landed(void) { return atomic_load(&s_power_landed); }

bool ha_volume_moved(int *denon_mv)
{
    int mv = atomic_exchange(&s_volume_moved, -1);
    if (mv < 0) return false;
    if (denon_mv) *denon_mv = mv;
    return true;
}

void ha_volume_follow(bool on)
{
    if (atomic_exchange(&s_volume_follow, on) == on) return;
    /* Waking: the task is parked on an endless wait, and whatever moved the
     * volume while the screen was off is worth catching up with at once. */
    if (on) poke();
}

void ha_set_number(ha_number_t which, int value)
{
    if (which < 0 || which >= HA_NUMBERS) return;
    atomic_store(&s_number[which], value);
    atomic_store(&s_number_due[which], true);
    poke();
}
