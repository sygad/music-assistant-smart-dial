/* Joins Wi-Fi and checks that Music Assistant and Home Assistant answer.
 * Read-only: one GET to each, no tokens sent. Also sets the clock from the
 * internet (UK time, summer time included). */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "where.h"
#include "ma.h"
#include "net_check.h"
#include "secrets_gen.h"

static const char *TAG = "net";
static EventGroupHandle_t s_events;
static void (*s_on_connected)(void);
static void roam_if_better(void);
#define GOT_IP  BIT0
#define RECHECK BIT1  /* the Connections screen has asked for a fresh look */

/* What the screen says about the network (ui_net.c, and net_state() below).
 *
 * Starts as connecting, because that is true from the first instant of a boot
 * and a boot is what waking from deep sleep is: the radio is off and the dial
 * can reach nothing. The LVGL thread reads it, so it is an atomic rather than
 * a call into the UI from this task - the screens are only touched there. */
static _Atomic net_state_t s_state = NET_WIFI;

net_state_t net_state(void) { return atomic_load(&s_state); }

/* Say it again, for testing on the bench: the real path, driven from the
 * console, rather than a pretend one in the UI that could pass while this one
 * was broken. The wait is about as long as the real one. */
static void say_again_task(void *arg)
{
    (void)arg;
    atomic_store(&s_state, NET_WIFI);
    vTaskDelay(pdMS_TO_TICKS(2500));
    atomic_store(&s_state, NET_MUSIC_ASSISTANT);
    vTaskDelay(pdMS_TO_TICKS(700));
    atomic_store(&s_state, NET_HOME_ASSISTANT);
    vTaskDelay(pdMS_TO_TICKS(700));
    atomic_store(&s_state, NET_SPEAKER);
    vTaskDelay(pdMS_TO_TICKS(700));
    atomic_store(&s_state, NET_READY);
    vTaskDelete(NULL);
}

void net_say_again(void)
{
    xTaskCreate(say_again_task, "netsay", 2048, NULL, 3, NULL);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, GOT_IP);
    }
}

/* The clock, from Home Assistant rather than the internet (3 Nov 2026).
 *
 * SNTP is pointed at pool.ntp.org, and the knob is on the IoT network, which
 * is heavily firewalled - it got the time eventually but took about a minute,
 * and for that whole minute the clock screen showed "--:--". Darren came back
 * to a flat battery, plugged the knob in, asked for Music off and got four
 * dashes for a minute.
 *
 * Home Assistant answers in five seconds and every HTTP response it sends
 * carries the time in its Date header, so the probe below picks it up on the
 * way past. No extra request, no server to configure, and nothing that can be
 * firewalled, because the knob is already talking to it. SNTP is left running
 * to correct the drift afterwards; this is only about the first minute. */
static char s_http_date[48];

static esp_err_t grab_date(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "Date") == 0)
        snprintf(s_http_date, sizeof s_http_date, "%s", e->header_value);
    return ESP_OK;
}

/* Days since 1970-01-01 for a civil date, Howard Hinnant's algorithm. Done by
 * hand rather than with mktime because the timezone is already set to
 * Europe/London and an HTTP date is always GMT: mktime would read it as local
 * and put the knob an hour out all summer. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* "Tue, 03 Nov 2026 16:43:00 GMT" -> the system clock, if it is not set yet. */
static bool clock_from_http_date(bool force)
{
    if (!s_http_date[0]) {
        ESP_LOGW(TAG, "NETCHECK no Date header from Home Assistant, so the clock waits on SNTP");
        return false;
    }
    /* Logged either way: this is the one line that says whether the clock has
     * a local source, and a USB reset keeps the old time so the skip is the
     * normal case on the bench. */
    if (!force && time(NULL) > 1700000000) {
        ESP_LOGI(TAG, "NETCHECK clock already set; Home Assistant offered '%s'", s_http_date);
        return false;
    }
    struct tm tm = {0};
    if (!strptime(s_http_date, "%a, %d %b %Y %H:%M:%S", &tm)) return false;
    int64_t epoch = days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) * 86400 +
                    tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    struct timeval tv = {.tv_sec = (time_t)epoch};
    settimeofday(&tv, NULL);
    return true;
}

/* The same request as probe() but without the log line: this one runs every
 * hour for the clock and has nothing to report unless it changes something. */
static void probe_quiet(const char *url)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 5000, .event_handler = grab_date};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    if (esp_http_client_open(c, 0) == ESP_OK) esp_http_client_fetch_headers(c);
    esp_http_client_cleanup(c);
}

/* Returns whether it answered at all, which is what the Connections screen
 * shows. It used to return nothing, because the log line was the whole point. */
static bool probe(const char *name, const char *url)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 5000, .event_handler = grab_date};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int64_t t0 = esp_timer_get_time();
    /* open + read headers only: perform() would try to answer HA's 401 login challenge */
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK && esp_http_client_fetch_headers(c) < 0) err = ESP_FAIL;
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NETCHECK %-15s reachable: HTTP %d in %lld ms (%s)", name, esp_http_client_get_status_code(c),
                 ms, url);
    } else {
        ESP_LOGE(TAG, "NETCHECK %-15s NOT reachable: %s after %lld ms (%s)", name, esp_err_to_name(err), ms, url);
    }
    esp_http_client_cleanup(c);
    return err == ESP_OK;
}

/* ---- The speaker ------------------------------------------------------------
 * Everything above answers the question "can the dial reach the house". This
 * answers the one that actually decides whether a turn of the knob makes a
 * sound: is the player the dial owns there to play to.
 *
 * It is asked of Music Assistant rather than of the streamer directly. Music
 * Assistant is the thing that would have to hand it the audio, its answer is
 * the one that matters, and it is reachable by name on a fixed address - the
 * streamer's own address is a DHCP lease the dial has no business knowing.
 *
 * One connection, opened once and kept: ma.h has no close, so opening one per
 * look would leak a client and its buffer every time the screen was opened. */
static ma_conn_t *s_ma;
static _Atomic int s_speaker = -1;  /* -1 not asked, 0 no, 1 yes */

static bool speaker_ok(void)
{
    if (!s_ma) s_ma = ma_open(16 * 1024);  /* 4.6 KB idle, measured 16 Nov 2026 */
    if (!s_ma) {
        ESP_LOGE(TAG, "NETCHECK speaker         NOT asked: no memory for a Music Assistant connection");
        return false;
    }
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "player_id", MA_PLAYER);
    int64_t t0 = esp_timer_get_time();
    cJSON *p = ma_call(s_ma, "players/get", a);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    /* No reply is not the same as "the speaker is away", but it has the same
     * consequence - nothing will play - so it is reported the same way. */
    if (!p) {
        ESP_LOGE(TAG, "NETCHECK speaker         NOT reachable: Music Assistant did not answer after %lld ms", ms);
        return false;
    }
    const cJSON *av = cJSON_GetObjectItemCaseSensitive(p, "available");
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(p, "name");
    bool ok = cJSON_IsTrue(av);
    if (ok)
        ESP_LOGI(TAG, "NETCHECK speaker         reachable: '%s' available in %lld ms",
                 cJSON_IsString(nm) ? nm->valuestring : MA_PLAYER, ms);
    else
        ESP_LOGE(TAG, "NETCHECK speaker         NOT reachable: '%s' is not available to Music Assistant (%lld ms)",
                 cJSON_IsString(nm) ? nm->valuestring : MA_PLAYER, ms);
    cJSON_Delete(p);
    return ok;
}

/* ---- What the Connections screen reads -------------------------------------- */
static _Atomic int s_ok_wifi = -1, s_ok_ma = -1, s_ok_ha = -1;
static _Atomic bool s_checking;

net_status_t net_status(void)
{
    return (net_status_t){.wifi = atomic_load(&s_ok_wifi),
                          .music_assistant = atomic_load(&s_ok_ma),
                          .home_assistant = atomic_load(&s_ok_ha),
                          .speaker = atomic_load(&s_speaker),
                          .checking = atomic_load(&s_checking)};
}

void net_status_refresh(void)
{
    /* Done on net_task rather than on a task of its own: the Music Assistant
     * connection above is not shared safely, and net_task is the only thing
     * that touches it. The screen gets "checking" until it lands. */
    atomic_store(&s_checking, true);
    if (s_events) xEventGroupSetBits(s_events, RECHECK);
}

/* Ask all four and remember the answers. */
static void look(void)
{
    wifi_ap_record_t ap;
    atomic_store(&s_ok_wifi, esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? 1 : 0);
    atomic_store(&s_ok_ma, probe("Music Assistant", SECRET_MA_SERVER "/info") ? 1 : 0);
    atomic_store(&s_ok_ha, probe("Home Assistant", SECRET_HA_SERVER "/api/") ? 1 : 0);
    atomic_store(&s_speaker, speaker_ok() ? 1 : 0);
    atomic_store(&s_checking, false);
}

static void net_task(void *arg)
{
    (void)arg;
    if (xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000)) & GOT_IP) {
        roam_if_better();
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            ESP_LOGI(TAG, "NETCHECK wifi '%s' signal %d dBm, channel %d, access point " MACSTR, (char *)ap.ssid,
                     ap.rssi, ap.primary, MAC2STR(ap.bssid));
        /* Each one named on the screen as it is asked. They answer in tens of
         * milliseconds when all is well, which is too fast to read - the UI
         * holds each name up for a readable moment (ui_net.c). */
        atomic_store(&s_ok_wifi, 1);
        atomic_store(&s_state, NET_MUSIC_ASSISTANT);
        atomic_store(&s_ok_ma, probe("Music Assistant", SECRET_MA_SERVER "/info") ? 1 : 0);
        atomic_store(&s_state, NET_HOME_ASSISTANT);
        atomic_store(&s_ok_ha, probe("Home Assistant", SECRET_HA_SERVER "/api/") ? 1 : 0);
        /* And the speaker, which is the stage that was missing: Wi-Fi, Music
         * Assistant and Home Assistant were all fine on 16 Nov 2026 while the
         * streamer was off the network, and the dial said Connected. */
        /* Where the amp and the streamer are, before anything asks after
         * either: Home Assistant has just answered, so this is the first
         * moment its helpers can be read (where.h). */
        where_load();
        atomic_store(&s_state, NET_SPEAKER);
        atomic_store(&s_speaker, speaker_ok() ? 1 : 0);
        if (clock_from_http_date(false)) {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            ESP_LOGI(TAG, "NETCHECK clock set from Home Assistant: %02d:%02d:%02d %d/%d/%d", tm.tm_hour, tm.tm_min,
                     tm.tm_sec, tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
        }
        /* Wi-Fi up and Home Assistant answering: this is the moment the dial
         * can do anything at all, and the moment the screen says so. */
        atomic_store(&s_state, NET_READY);
        if (s_on_connected) s_on_connected();
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            ESP_LOGI(TAG, "NETCHECK clock set: %02d:%02d:%02d %d/%d/%d", tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_mday,
                     tm.tm_mon + 1, tm.tm_year + 1900);
            ESP_LOGI(TAG, "NETCHECK internal RAM free %u, largest block %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        } else {
            ESP_LOGW(TAG, "NETCHECK clock not set within 15 s (keeps trying)");
        }
    } else {
        /* Thirty seconds and no address. The screen has been saying
         * "Connecting" all that time and must not go on saying it for ever -
         * a line that never resolves is the fault we fixed this morning. */
        atomic_store(&s_state, NET_FAILED);
        atomic_store(&s_ok_wifi, 0);
        atomic_store(&s_ok_ma, 0);
        atomic_store(&s_ok_ha, 0);
        atomic_store(&s_speaker, 0);
        ESP_LOGE(TAG, "NETCHECK wifi '%s' did not connect within 30 s", SECRET_WIFI_SSID);
    }
    /* Keep checking: the knob stays put, but access points come and go - and
     * keep the clock honest, because nothing else does. SNTP is pointed at
     * pool.ntp.org and never syncs from this network at all (3 Nov 2026: it
     * warns at every boot and the clock screen sat on "--:--" for a minute),
     * so without this the time would be set once and then drift untouched.
     * Home Assistant's Date header is good to the second, which is all a clock
     * face needs, and it is the one source already proven to be reachable. */
    for (int tick = 0; ; tick++) {
        /* A minute, or sooner if the Connections screen has been opened. The
         * screen's look happens here because the Music Assistant connection
         * belongs to this task and to nothing else. */
        if (xEventGroupWaitBits(s_events, RECHECK, pdTRUE, pdTRUE, pdMS_TO_TICKS(60000)) & RECHECK) {
            look();
            tick--;  /* an answer for the screen is not a minute of the clock's */
            continue;
        }
        roam_if_better();
        if (tick % 60 == 59) {  /* hourly */
            s_http_date[0] = 0;
            probe_quiet(SECRET_HA_SERVER "/api/");
            if (clock_from_http_date(true)) ESP_LOGI(TAG, "NETCHECK clock nudged back into line");
        }
    }
}

/* Scan for our network. Returns how many access points were found. */
static uint16_t scan(wifi_ap_record_t *recs, uint16_t max)
{
    wifi_scan_config_t sc = {.ssid = (uint8_t *)SECRET_WIFI_SSID, .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    uint16_t n = max;
    esp_wifi_scan_get_ap_records(&n, recs);
    return n;
}

/* Move to a clearly stronger access point. Even with sort-by-signal, the knob
 * sometimes joined the far AP (-81 dBm) with a near one at -55 dBm. */
#define ROAM_MARGIN_DB 8
/* Roaming costs about three and a half seconds with no network at all, so it
 * waits until the knob has been left alone. Landing in the middle of a scroll
 * cost Darren 27 covers (29 Oct 2026). */
static volatile bool s_may_roam = true;
void net_allow_roam(bool may) { s_may_roam = may; }

static void roam_if_better(void)
{
    wifi_ap_record_t cur, recs[8];
    if (!s_may_roam || esp_wifi_sta_get_ap_info(&cur) != ESP_OK) return;
    uint16_t n = scan(recs, 8);
    if (n == 0 || !memcmp(recs[0].bssid, cur.bssid, 6)) return;
    /* Judge both from the same scan. The RSSI of the access point you are on,
     * as esp_wifi_sta_get_ap_info reports it, reads several dB lower than the
     * same access point in a scan - so whichever one it was on always looked
     * like the weaker of the two, and it spent the evening hopping between a
     * pair of them, each hop another three and a half second hole (Darren's
     * lounge, 29 Oct 2026: -80 to -72, then -82 back to -74, then -89 to -76,
     * all between the same two access points). */
    int mine = cur.rssi;
    for (uint16_t i = 0; i < n; i++)
        if (!memcmp(recs[i].bssid, cur.bssid, 6)) { mine = recs[i].rssi; break; }
    if (recs[0].rssi < mine + ROAM_MARGIN_DB) return;
    ESP_LOGI(TAG, "ROAM from " MACSTR " (%d dBm) to " MACSTR " (%d dBm, channel %d)", MAC2STR(cur.bssid), mine,
             MAC2STR(recs[0].bssid), recs[0].rssi, recs[0].primary);
    wifi_config_t cfg;
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    memcpy(cfg.sta.bssid, recs[0].bssid, 6);
    cfg.sta.bssid_set = true;
    cfg.sta.channel = recs[0].primary;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    xEventGroupClearBits(s_events, GOT_IP);
    esp_wifi_disconnect();  /* the disconnect handler reconnects, now to that AP */
    xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    cfg.sta.bssid_set = false;  /* free to roam again next time */
    cfg.sta.channel = 0;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

/* Every access point broadcasting our network, strongest first. */
void net_scan_log(void)
{
    EXT_RAM_BSS_ATTR static wifi_ap_record_t recs[16];  /* a console command: not internal RAM */
    uint16_t n = scan(recs, 16);
    wifi_ap_record_t cur;
    bool have = esp_wifi_sta_get_ap_info(&cur) == ESP_OK;
    ESP_LOGI(TAG, "SCAN %u access points for '%s':", n, SECRET_WIFI_SSID);
    for (int i = 0; i < n; i++)
        ESP_LOGI(TAG, "SCAN   " MACSTR "  %4d dBm  channel %2d%s", MAC2STR(recs[i].bssid), recs[i].rssi, recs[i].primary,
                 have && !memcmp(cur.bssid, recs[i].bssid, 6) ? "  <- connected" : "");
}

void net_check_start(void (*on_connected)(void))
{
    s_on_connected = on_connected;
    s_events = xEventGroupCreate();
    /* cJSON's PSRAM hooks, for the speaker probe. library.c calls this too,
     * but not until the library loads - which is after Wi-Fi is up and so
     * after the speaker has already been asked about. Setting the same hooks
     * twice costs nothing; asking cJSON for internal RAM would cost the dial
     * the memory LVGL draws from. */
    ma_init();
    ESP_ERROR_CHECK(esp_netif_init());
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);  /* Europe/London */
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");  /* starts once Wi-Fi has an address */
    esp_netif_sntp_init(&sntp);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, SECRET_WIFI_SSID, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, SECRET_WIFI_PASSWORD, sizeof cfg.sta.password);
    /* Scan every channel and join the strongest access point. The default joins
     * the first one heard, which left the knob on a distant AP at -87 dBm. */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); /* no power-save latency on a mains-powered knob */
    /* stack in PSRAM, like the loader's: internal RAM is what LVGL draws from,
     * and it was getting down to 88 bytes free while Darren scrolled, which is
     * where the missing track lists and the mangled text came from (29 Oct
     * 2026). This task only does network work, so it can live there. */
    xTaskCreatePinnedToCoreWithCaps(net_task, "netcheck", 8192, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
}
