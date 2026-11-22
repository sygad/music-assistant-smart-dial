/* Where the amp and streamer are: see where.h. */
#include "where.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha.h"
#include "secrets_gen.h"

static const char *TAG = "where";

#define WHERE_AMP_HELPER    "input_text." SECRET_DIAL_ROOM "_dial_amp_address"
#define WHERE_PLAYER_HELPER "input_text." SECRET_DIAL_ROOM "_dial_player"

/* Long enough for any address or player id Home Assistant's input_text will
 * hold by default (100 characters). */
#define WHERE_MAX 128

/* How often a running dial looks for a change, while its screen is on. A
 * minute is soon enough for someone who has just typed the address in, and
 * costs two small requests. Asleep it doesn't look at all: the battery matters
 * more, and the next wake looks straight away. */
#define WHERE_CHECK_MS 60000

static char s_amp[WHERE_MAX] = SECRET_AMP_SERVER;
static char s_player[WHERE_MAX] = SECRET_MA_PLAYER;

const char *where_amp(void) { return s_amp; }
const char *where_player(void) { return s_player; }

/* What the helper says the value should be: its text if it holds any,
 * otherwise the built-in one. False only when Home Assistant couldn't be
 * asked, which says nothing about what the user wants. */
static bool wanted(const char *helper, const char *built_in, char *out, int out_max)
{
    char got[WHERE_MAX];
    if (!ha_read_text(helper, got, sizeof got)) {
        /* A helper that was never made reads as a failure too. That is the
         * ordinary case on a dial set up from secrets.local, so it is not a
         * fault: fall back, and say so once at start-up. */
        snprintf(out, out_max, "%s", built_in);
        return false;
    }
    /* Home Assistant's own words for "nothing here" are not an address. */
    bool empty = !got[0] || !strcmp(got, "unknown") || !strcmp(got, "unavailable");
    snprintf(out, out_max, "%s", empty ? built_in : got);
    return true;
}

static void trim_slash(char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == '/') s[--n] = 0;
}

void where_load(void)
{
    bool a = wanted(WHERE_AMP_HELPER, SECRET_AMP_SERVER, s_amp, sizeof s_amp);
    bool p = wanted(WHERE_PLAYER_HELPER, SECRET_MA_PLAYER, s_player, sizeof s_player);
    trim_slash(s_amp);
    ESP_LOGI(TAG, "amp: %s (%s)", s_amp[0] ? s_amp : "none", a ? "Home Assistant asked" : "no helper, built in");
    ESP_LOGI(TAG, "player: %s (%s)", s_player[0] ? s_player : "none", p ? "Home Assistant asked" : "no helper, built in");
}

void where_check(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time() / 1000;
    if (last && now - last < WHERE_CHECK_MS) return;
    last = now;

    char amp[WHERE_MAX], player[WHERE_MAX];
    bool a = wanted(WHERE_AMP_HELPER, SECRET_AMP_SERVER, amp, sizeof amp);
    bool p = wanted(WHERE_PLAYER_HELPER, SECRET_MA_PLAYER, player, sizeof player);
    trim_slash(amp);
    /* Only an answer counts. Home Assistant being away for a moment must not
     * look like the helper being emptied, or every blip would restart the
     * dial back onto its built-in values. */
    bool amp_moved = a && strcmp(amp, s_amp) != 0;
    bool player_moved = p && strcmp(player, s_player) != 0;
    if (!amp_moved && !player_moved) return;

    ESP_LOGW(TAG, "changed in Home Assistant (amp '%s' -> '%s', player '%s' -> '%s'): restarting to use it",
             s_amp, amp, s_player, player);
    vTaskDelay(pdMS_TO_TICKS(200));  /* let the line reach the console */
    esp_restart();
}
