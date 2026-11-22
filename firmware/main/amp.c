/* Is the amp actually listening? See amp.h for why this exists at all. */
#include "amp.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "where.h"

static const char *TAG = "amp";

/* The amp's own status page, the same one Home Assistant's integration reads
 * and the cheapest thing it serves: a few hundred bytes, answered in 13-17 ms
 * by a well amp. The address is the Denon's, fixed in the house as Home
 * Assistant's is. */
#define AMP_STATUS_PATH "/goform/formMainZone_MainZoneXmlStatusLite.xml"

/* Short, because a deaf amp does not refuse the connection - it accepts
 * nothing and leaves you waiting. Four seconds is long enough for a well amp
 * on a bad Wi-Fi moment and short enough that the screen is not stuck on one
 * answer for ages. */
#define AMP_TIMEOUT_MS 4000

/* While waiting: often enough that "connected" appears the moment it is true.
 * Once it has answered: rarely, just enough to notice it going deaf again. */
#define AMP_WAIT_POLL_MS  2000
#define AMP_READY_POLL_MS 30000
/* A single missed answer from a well amp is a blip on the way to it, not a
 * fault. Two in a row is the amp. */
#define AMP_MISSES_BEFORE_LOST 2

/* No amp at all: no address in Home Assistant's amp helper or in AMP_SERVER
 * (where.h). A dial built
 * with only Music Assistant behind it is told the amp is there and listening,
 * for good, so play, the volume arc and the tone screen all behave as they do
 * with an amp. The volume still goes where it always goes; with no amp behind
 * it, nothing is made louder (Darren, 22 Nov 2026: "fake amp it is then"). */
#define AMP_NONE (where_amp()[0] == '\0')

static _Atomic int s_state = AMP_UNKNOWN;
/* Awake at boot: the screen comes on with the dial and set_asleep keeps this
 * in step from then on, but it is never called for the screen already being
 * on, so a false here left a freshly booted dial never probing the amp at all
 * - no "Connecting to the amp" however long the wait, and amp_ready() false
 * for good, which holds every volume write (Darren, 4 Nov 2026). */
static _Atomic bool s_awake = true;
static _Atomic int64_t s_waiting_since;
static TaskHandle_t s_task;

amp_state_t amp_get_state(void) { return AMP_NONE ? AMP_READY : (amp_state_t)atomic_load(&s_state); }
bool amp_ready(void) { return amp_get_state() == AMP_READY; }

int amp_connecting_secs(void)
{
    int64_t since = atomic_load(&s_waiting_since);
    if (!since) return 0;
    return (int)((esp_timer_get_time() / 1000 - since) / 1000);
}

void amp_awake(bool awake)
{
    if (AMP_NONE) return;  /* nothing to ask, so nothing to forget */
    if (atomic_exchange(&s_awake, awake) == awake) return;
    if (!awake) {
        /* Asleep: forget what was known. The amp is usually powered down with
         * the music, so a remembered "ready" would be a lie by morning. */
        atomic_store(&s_state, AMP_UNKNOWN);
        atomic_store(&s_waiting_since, 0);
    } else if (s_task) {
        xTaskNotifyGive(s_task);  /* look straight away rather than on the next tick */
    }
}

/* One question, one answer. True only if the amp served the page. */
static bool amp_answers(void)
{
    char url[160];
    snprintf(url, sizeof url, "%s" AMP_STATUS_PATH, where_amp());
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = AMP_TIMEOUT_MS,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0)
        ok = esp_http_client_get_status_code(c) == 200;
    esp_http_client_cleanup(c);
    return ok;
}

static void amp_task(void *arg)
{
    (void)arg;
    int misses = 0;
    for (;;) {
        bool awake = atomic_load(&s_awake);
        int state = atomic_load(&s_state);
        int wait_ms = !awake ? 60000 : state == AMP_READY ? AMP_READY_POLL_MS : AMP_WAIT_POLL_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));

        if (!atomic_load(&s_awake)) {
            misses = 0;
            continue;
        }

        bool answered = amp_answers();

        /* Told to stop while that question was in the air: throw the answer
         * away rather than write a state nobody asked for. A question takes up
         * to four seconds, so Music off landed in the middle of one and the
         * answer that came back afterwards put "Connecting to the amp" on the
         * screen a second after the music went off - and left it there, since
         * nothing probes again to take it back (Darren, 4 Nov 2026). */
        if (!atomic_load(&s_awake)) {
            misses = 0;
            continue;
        }

        if (answered) {
            misses = 0;
            if (atomic_exchange(&s_state, AMP_READY) != AMP_READY) {
                ESP_LOGI(TAG, "the amp is answering%s",
                         amp_connecting_secs() ? " (after waiting)" : "");
                atomic_store(&s_waiting_since, 0);
            }
            continue;
        }

        if (++misses < AMP_MISSES_BEFORE_LOST && atomic_load(&s_state) == AMP_READY) continue;
        if (atomic_exchange(&s_state, AMP_CONNECTING) != AMP_CONNECTING) {
            ESP_LOGW(TAG, "the amp is not answering: booting, or it has hung");
            atomic_store(&s_waiting_since, esp_timer_get_time() / 1000);
        }
    }
}

void amp_start(void)
{
    if (s_task) return;
    if (AMP_NONE) {
        ESP_LOGI(TAG, "no amp address set: the amp is taken as always there");
        return;
    }
    /* Low priority and a PSRAM stack: nothing waits on this, and the answer is
     * only ever read, never blocked on. */
    xTaskCreatePinnedToCoreWithCaps(amp_task, "amp", 4096, NULL, 2, &s_task, 0, MALLOC_CAP_SPIRAM);
}
