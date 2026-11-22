/* Where the amp and streamer are: see where.h. */
#include "where.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha.h"
#include "ma.h"
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
/* What the player was given as, before it was turned into an id: a name or an
 * id. where_check compares this, so a change is seen without asking Music
 * Assistant for every player once a minute. */
static char s_player_said[WHERE_MAX] = SECRET_MA_PLAYER;

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

/* An address as people type it: "192.168.1.50" as well as
 * "http://192.168.1.50/". Spaces and a trailing slash go, and a bare address
 * gets the http:// the status page needs. */
static void tidy_amp(char *s, int max)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '/' || s[n - 1] == ' ')) s[--n] = 0;
    size_t lead = strspn(s, " ");
    if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
    if (s[0] && strncmp(s, "http://", 7) && strncmp(s, "https://", 8)) {
        size_t len = strlen(s);
        if (len + 7 < (size_t)max) {
            memmove(s + 7, s, len + 1);
            memcpy(s, "http://", 7);
        }
    }
}

/* Names in Music Assistant use a typographic apostrophe (Darren's MacBook
 * Pro), which nobody types. Compare with it read as a plain one, and without
 * caring about case. */
static bool same_name(const char *a, const char *b)
{
    for (;;) {
        int ca, cb;
        if ((unsigned char)a[0] == 0xE2 && (unsigned char)a[1] == 0x80 && (unsigned char)a[2] == 0x99) { ca = '\''; a += 3; }
        else ca = tolower((unsigned char)*a++);
        if ((unsigned char)b[0] == 0xE2 && (unsigned char)b[1] == 0x80 && (unsigned char)b[2] == 0x99) { cb = '\''; b += 3; }
        else cb = tolower((unsigned char)*b++);
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

typedef struct {
    const char *said;
    char id[WHERE_MAX];
    char name[WHERE_MAX];
} find_t;

static bool find_one(const cJSON *item, int index, void *ctx)
{
    (void)index;
    find_t *f = ctx;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "player_id");
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(item, "name");
    if (!cJSON_IsString(id)) return true;
    bool hit = !strcmp(id->valuestring, f->said) || (cJSON_IsString(nm) && same_name(nm->valuestring, f->said));
    if (!hit) return true;
    snprintf(f->id, sizeof f->id, "%s", id->valuestring);
    snprintf(f->name, sizeof f->name, "%s", cJSON_IsString(nm) ? nm->valuestring : "");
    return false;  /* found: stop reading */
}

/* The id for what the user typed, a name or an id. Unchanged if Music
 * Assistant doesn't know it, or can't be asked. */
static void to_player_id(const char *said, char *out, int out_max)
{
    snprintf(out, out_max, "%s", said);
    if (!said[0]) return;
    /* One player is about 2.5 KB of JSON and ma_each holds only one at a time.
     * Opened for this and closed again, so it costs nothing once the dial is
     * running. */
    ma_conn_t *c = ma_open(8 * 1024);
    if (!c) {
        ESP_LOGW(TAG, "no memory to look the player up; using '%s' as it is", said);
        return;
    }
    find_t f = {.said = said};
    int n = ma_each(c, "players/all", NULL, find_one, &f);
    ma_close(c);
    if (f.id[0]) {
        if (strcmp(f.id, said)) ESP_LOGI(TAG, "player '%s' is %s", f.name, f.id);
        snprintf(out, out_max, "%s", f.id);
    } else if (n >= 0) {
        ESP_LOGW(TAG, "Music Assistant has no player called '%s' (%d players)", said, n);
    } else {
        ESP_LOGW(TAG, "couldn't ask Music Assistant for its players; using '%s' as it is", said);
    }
}

void where_load(void)
{
    bool a = wanted(WHERE_AMP_HELPER, SECRET_AMP_SERVER, s_amp, sizeof s_amp);
    bool p = wanted(WHERE_PLAYER_HELPER, SECRET_MA_PLAYER, s_player_said, sizeof s_player_said);
    to_player_id(s_player_said, s_player, sizeof s_player);
    tidy_amp(s_amp, sizeof s_amp);
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
    tidy_amp(amp, sizeof amp);
    /* Only an answer counts. Home Assistant being away for a moment must not
     * look like the helper being emptied, or every blip would restart the
     * dial back onto its built-in values. */
    bool amp_moved = a && strcmp(amp, s_amp) != 0;
    bool player_moved = p && strcmp(player, s_player_said) != 0;
    if (!amp_moved && !player_moved) return;

    ESP_LOGW(TAG, "changed in Home Assistant (amp '%s' -> '%s', player '%s' -> '%s'): restarting to use it",
             s_amp, amp, s_player_said, player);
    vTaskDelay(pdMS_TO_TICKS(200));  /* let the line reach the console */
    esp_restart();
}
