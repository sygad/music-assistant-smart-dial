/* Making a sound: Music Assistant's own API, on its own task.
 *
 * The knob owns the Bathroom Streamer's queue. A collection goes over whole -
 * the album's or playlist's uri, with the track to start on - so everything
 * after that track follows without the knob asking for it, and what the player
 * then does comes back the other way through ui_player_state. That is why the
 * progress bar can keep its own clock: it is corrected a second at a time by
 * something that actually knows.
 *
 * Changing track while the music is playing is never a hard cut (Darren):
 * Music Assistant's own crossfade only covers one queue item running into the
 * next, and a "play this now" starts a new stream, so the knob does it itself
 * by taking the streamer's volume down, switching, waiting for the new track
 * to be making a sound and bringing it back up. The streamer's volume, not the
 * Denon's: the Denon is what the knob's own volume means, and dipping that
 * would fight it.
 */
#include "playback.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ma.h"
#include "ui.h"

static const char *TAG = "play";

/* The one player the knob drives. A choice of player is still to come
 * (PLANNING.md), and this is the line it lifts out of. */
#define PLAYER MA_PLAYER  /* ma.h: net_check.c reads the same one */

#define PB_POLL_MS      1000   /* how often the player is asked where it has got to */
/* The fade, measured on the real thing (31 Oct 2026): one volume_set is a 30 ms
 * round trip, and a new track is making a sound about 660 ms after it is asked
 * for. Seven steps over 350 ms was neither long enough nor smooth enough -
 * Darren heard it as a very sharp roll off - and each step was two or three dB
 * near the top of the WiiM's curve, so it came down a staircase rather than a
 * ramp. Twenty steps over 600 ms is about half a dB a step, and the loop paces
 * itself against the clock rather than counting on the requests taking no time. */
#define PB_FADE_MS      600
#define PB_FADE_STEPS   20
/* How long after the ask the new track is actually coming out of the speakers.
 *
 * Measured, because asking turned out to be useless: Music Assistant reports
 * the new track as playing within about 35 ms and starts its clock, then
 * resets that clock to zero around 1.5 s - which is the player finally saying
 * where it really is. The first second and a half of "playing" is an estimate,
 * not a sound. The same 1.5 s shows up whether the queue is replaced or merely
 * jumped within (play_index), so it is the streamer opening a new stream and
 * there is nothing to be done about it.
 *
 * So the silence in the middle of a track change is real and about this long.
 * The fade back up is started so that its middle lands on the arrival: the
 * music comes in already rising instead of either appearing at full volume or
 * waiting out a fade that finished in silence. */
#define PB_SWITCH_MS    1500
#define PB_QUIET_MS     1500   /* after a play: the queue may still be answering for the old one */
/* A seek is not a play: the queue is the same one, only the position in it has
 * moved, so there is nothing to go quiet about. It used to take the play's
 * 1.5 s blindfold and the 1 s poll on top, and the knob sat there for 2.2 s
 * not admitting the seek had happened - about as long as the scrubber waits
 * before closing, so scrubbing felt like nothing was going on (Darren, 2 Nov
 * 2026). Instead the player is asked again straight away, every
 * PB_SEEK_POLL_MS, and a report still carrying the old position is passed over
 * rather than waited out: the position asked for is the only news worth
 * having. PB_SEEK_WAIT_MS later the knob takes whatever it is given, so a seek
 * the player quietly refused cannot leave the clock stuck for good. */
#define PB_SEEK_POLL_MS 250
#define PB_SEEK_WAIT_MS 4000   /* ...after which the seek is written off, and only logged as such */
#define PB_SEEK_NEAR_S  8      /* how far past the asked position still counts as having landed */
/* Pause, play and stop are asked for and then polled for straight away, and
 * Music Assistant answers for the moment before the tap: pausing showed the
 * play button, bounced back to the playing screen and then landed on paused
 * (Darren, 31 Oct 2026). So a report that still disagrees with what was just
 * asked for is held back until the player catches up - or until this long has
 * passed, after which the player is right and the ask was lost. */
#define PB_ECHO_MS      3000
#define PB_URI 160

typedef enum { PB_PLAY, PB_PAUSE, PB_STOP, PB_SEEK } op_t;

typedef struct {
    op_t op;
    int arg;
    char coll[PB_URI];
    char track[PB_URI];
} cmd_t;

static QueueHandle_t s_cmds;
static int64_t s_stop_at;  /* a stop waiting for Darren's fade to finish; 0 none */
static ma_conn_t *s_ma;

/* What was last asked for, and what the player said about it. */
static char s_coll[PB_URI], s_track[PB_URI];
static bool s_paused_here;
static int64_t s_quiet_until;
/* The last seek asked for, so the report that carries it out can be timed. */
static int64_t s_seek_asked_at;
static int s_seek_to = -1;
static bool s_expect_playing;    /* what the last ask should have made of the player... */
static int64_t s_expect_until;   /* ...and how long to wait for it; 0 once it agrees */

static _Atomic bool s_rep_playing;
static _Atomic int s_rep_index;
static _Atomic uint32_t s_rep_elapsed_ms;
static _Atomic uint32_t s_rep_seq;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void expect(bool playing)
{
    s_expect_playing = playing;
    s_expect_until = now_ms() + PB_ECHO_MS;
}

static cJSON *arg_queue(void)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "queue_id", PLAYER);
    return a;
}

static void simple(const char *command)
{
    cJSON *r = ma_call(s_ma, command, arg_queue());
    cJSON_Delete(r);
}

/* The queue's state, as it is now. Any of the outputs may be NULL; `uri` is
 * the track it is actually on, which is the only way to tell the track just
 * asked for from the one still playing while the ask is being acted on. */
static bool queue_state(bool *playing, int *index, uint32_t *elapsed_ms, char *uri, size_t uri_max)
{
    cJSON *q = ma_call(s_ma, "player_queues/get", arg_queue());
    if (!q) return false;
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(q, "state");
    const cJSON *ix = cJSON_GetObjectItemCaseSensitive(q, "current_index");
    const cJSON *el = cJSON_GetObjectItemCaseSensitive(q, "elapsed_time");
    if (playing) *playing = cJSON_IsString(st) && !strcmp(st->valuestring, "playing");
    if (index) *index = cJSON_IsNumber(ix) ? ix->valueint : 0;
    if (elapsed_ms) *elapsed_ms = cJSON_IsNumber(el) ? (uint32_t)(el->valuedouble * 1000.0) : 0;
    if (uri && uri_max) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(q, "current_item");
        const cJSON *media = cJSON_GetObjectItemCaseSensitive(item, "media_item");
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(media, "uri");
        snprintf(uri, uri_max, "%s", cJSON_IsString(u) ? u->valuestring : "");
    }
    cJSON_Delete(q);
    return true;
}

static void streamer_volume(int level)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "player_id", PLAYER);
    cJSON_AddNumberToObject(a, "volume_level", level);
    cJSON *r = ma_call(s_ma, "players/cmd/volume_set", a);
    cJSON_Delete(r);
}

/* The streamer is held wide open and the Denon is the volume - one control,
 * and the last word is the amp (Darren, 31 Oct 2026). Two volumes in series is
 * a conflict, not a setting: the bathroom's streamer had been left at 69 by
 * something, so the whole dial was quietly 3 dB down against the other rooms,
 * whose streamers are both at 100.
 *
 * It is set rather than read, because whatever put it at 69 - a phone over
 * Bluetooth is the likely one - can do it again, and remembering the wrong
 * number would just preserve the fault. */
#define STREAMER_FULL 100

static void open_streamer_up(void)
{
    streamer_volume(STREAMER_FULL);
}

/* Paced against the clock, not against the step count: each request takes
 * about 30 ms of the 30 ms a step is allowed, so sleeping a fixed amount on top
 * of it would have made the fade twice as long as it says it is. */
static void fade_to(int from, int to)
{
    int64_t t0 = now_ms();
    for (int i = 1; i <= PB_FADE_STEPS; i++) {
        streamer_volume(from + (to - from) * i / PB_FADE_STEPS);
        int64_t wait = t0 + (int64_t)PB_FADE_MS * i / PB_FADE_STEPS - now_ms();
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

static void load(const char *coll, const char *track)
{
    cJSON *a = arg_queue();
    cJSON *media = cJSON_AddArrayToObject(a, "media");
    cJSON_AddItemToArray(media, cJSON_CreateString(coll));
    cJSON_AddStringToObject(a, "option", "replace");
    if (*track) cJSON_AddStringToObject(a, "start_item", track);
    cJSON *r = ma_call(s_ma, "player_queues/play_media", a);
    cJSON_Delete(r);
}

/* Wait until the new track is actually making a sound, so the fade back up
 * does not run over silence while Music Assistant is still opening the stream. */
/* Hold the silence until the music is nearly here, counted from `from`, which
 * is when the switch was asked for. */
static void wait_for_sound(int64_t from)
{
    int64_t wait = from + PB_SWITCH_MS - PB_FADE_MS / 2 - now_ms();
    if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
}

static void do_play(const cmd_t *c)
{
    bool same = !strcmp(s_coll, c->coll) && !strcmp(s_track, c->track);
    if (same && s_paused_here) {  /* a tap on a paused track carries on where it left off */
        simple("player_queues/play");
        s_paused_here = false;
        s_quiet_until = now_ms() + PB_QUIET_MS;
        expect(true);
        ESP_LOGI(TAG, "RESUME");
        return;
    }
    bool sounding = false;
    queue_state(&sounding, NULL, NULL, NULL, 0);
    /* Not fading? Then make sure the streamer is where it belongs. A fade that
     * was interrupted - the request after it failed, the knob restarted - would
     * otherwise leave it at nothing, and the room would stay silent with
     * everything else looking right. */
    int64_t down = now_ms(), fade = down;
    if (!sounding) open_streamer_up();
    else {
        fade_to(STREAMER_FULL, 0);
        fade = now_ms();
    }
    load(c->coll, c->track);
    snprintf(s_coll, sizeof s_coll, "%s", c->coll);
    snprintf(s_track, sizeof s_track, "%s", c->track);
    s_paused_here = false;
    if (sounding) {
        wait_for_sound(fade);
        int64_t t = now_ms();
        fade_to(0, STREAMER_FULL);
        ESP_LOGI(TAG, "CROSSFADE out %d ms, silent %d ms, in %d ms over the arrival",
                 (int)(fade - down), (int)(t - fade), (int)(now_ms() - t));
    }
    s_quiet_until = now_ms() + PB_QUIET_MS;
    expect(true);
    ESP_LOGI(TAG, "PLAY %s from %s", c->coll, c->track);
}

static void run(const cmd_t *c)
{
    switch (c->op) {
    case PB_PLAY:
        s_stop_at = 0;  /* changed their mind during the fade */
        do_play(c);
        break;
    case PB_PAUSE:
        simple("player_queues/pause");
        s_paused_here = true;
        expect(false);
        ESP_LOGI(TAG, "PAUSE");
        break;
    case PB_STOP:
        if (c->arg > 0) {  /* let the amp fade first: the music goes out with it */
            s_stop_at = now_ms() + c->arg;
            ESP_LOGI(TAG, "STOP in %d ms", c->arg);
            return;
        }
        s_stop_at = 0;
        simple("player_queues/stop");
        open_streamer_up();
        s_paused_here = false;
        s_coll[0] = s_track[0] = 0;
        atomic_store(&s_rep_playing, false);
        atomic_fetch_add(&s_rep_seq, 1);
        expect(false);
        ESP_LOGI(TAG, "STOP");
        break;
    case PB_SEEK: {
        cJSON *a = arg_queue();
        cJSON_AddNumberToObject(a, "position", c->arg);
        int64_t t0 = now_ms();
        cJSON *r = ma_call(s_ma, "player_queues/seek", a);
        cJSON_Delete(r);
        /* Timed because the hunt lives or dies on it (Darren, 2 Nov 2026). */
        ESP_LOGI(TAG, "SEEK %d in %lld ms", c->arg, now_ms() - t0);
        s_seek_asked_at = now_ms();
        s_seek_to = c->arg;
        break;
    }
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    s_ma = ma_open(16 * 1024);
    open_streamer_up();  /* the Denon is the volume; the streamer stays wide open */
    int64_t next_poll = 0;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmds, &c, pdMS_TO_TICKS(100)) == pdTRUE) {
            run(&c);
            next_poll = 0;  /* ask again straight away */
            continue;
        }
        if (s_stop_at && now_ms() >= s_stop_at) {
            s_stop_at = 0;
            run(&(cmd_t){.op = PB_STOP});
        }
        bool seeking = s_seek_to >= 0;
        if (!*s_coll || now_ms() < next_poll) continue;
        next_poll = now_ms() + (seeking ? PB_SEEK_POLL_MS : PB_POLL_MS);
        bool playing = false;
        int index = 0;
        uint32_t elapsed = 0;
        if (!queue_state(&playing, &index, &elapsed, NULL, 0)) continue;
        if (now_ms() < s_quiet_until) continue;  /* still answering for the queue before this one */
        if (seeking) {
            /* Timed, and nothing more. Holding the whole report back until the
             * seek landed starved the knob of everything else in it - whether
             * the music is playing at all, which track it is on - and a hunt
             * restarts that blackout with every throw, so the knob went deaf
             * for as long as the hunt lasted: the sweeping bar stuck on, the
             * clock stopped, the dial picking tracks instead of scrubbing
             * (Darren, 2 Nov 2026). A position that is still the one from
             * before the seek is the UI's to disregard, and it is the only
             * field that is stale. */
            int at = (int)(elapsed / 1000u);
            if (at >= s_seek_to && at <= s_seek_to + PB_SEEK_NEAR_S) {
                ESP_LOGI(TAG, "SEEK landed at %u ms, %lld ms after the ask",
                         (unsigned)elapsed, now_ms() - s_seek_asked_at);
                s_seek_to = -1;
            } else if (now_ms() - s_seek_asked_at >= PB_SEEK_WAIT_MS) {
                ESP_LOGW(TAG, "SEEK to %d never landed; at %d", s_seek_to, at);
                s_seek_to = -1;
            }
        }
        if (s_expect_until) {
            if (playing == s_expect_playing) s_expect_until = 0;  /* the player has caught up */
            else if (now_ms() < s_expect_until) continue;         /* still answering for before the tap */
            else s_expect_until = 0;                              /* it isn't going to: take its word */
        }
        atomic_store(&s_rep_playing, playing);
        atomic_store(&s_rep_index, index);
        atomic_store(&s_rep_elapsed_ms, elapsed);
        atomic_fetch_add(&s_rep_seq, 1);
    }
}

void playback_start(void)
{
    if (s_cmds) return;
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    xTaskCreatePinnedToCoreWithCaps(playback_task, "playback", 6144, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
}

static void queue_cmd(const cmd_t *c)
{
    if (s_cmds) xQueueSend(s_cmds, c, 0);
}

void playback_play(const char *coll_uri, const char *track_uri)
{
    cmd_t c = {.op = PB_PLAY};
    snprintf(c.coll, sizeof c.coll, "%s", coll_uri ? coll_uri : "");
    snprintf(c.track, sizeof c.track, "%s", track_uri ? track_uri : "");
    if (!*c.coll) return;
    queue_cmd(&c);
}

void playback_pause(void) { queue_cmd(&(cmd_t){.op = PB_PAUSE}); }
void playback_stop_after(int after_ms) { queue_cmd(&(cmd_t){.op = PB_STOP, .arg = after_ms}); }
void playback_seek(int seconds) { queue_cmd(&(cmd_t){.op = PB_SEEK, .arg = seconds < 0 ? 0 : seconds}); }

void playback_poll(void)
{
    static uint32_t seen;
    uint32_t seq = atomic_load(&s_rep_seq);
    if (seq == seen) return;
    seen = seq;
    ui_player_state(atomic_load(&s_rep_playing), atomic_load(&s_rep_index), atomic_load(&s_rep_elapsed_ms));
}
