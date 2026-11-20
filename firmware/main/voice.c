/* Voice search (an experiment, 26 Oct 2026).
 *
 * Records from the board's PDM microphone (GPIO 45 clock, 46 data, from
 * Waveshare's audio demo) at 16 kHz, stops after a pause in speech or a tap,
 * sends the audio to Home Assistant's local speech-to-text (faster-whisper),
 * then searches: albums on the knob itself, ranked (the artist's own albums
 * first, words that only sound alike lower: speech-to-text writes "purple
 * reign"), which Music Assistant's search can't do ("prince purple rain" finds
 * nothing there); then tracks from Music Assistant, with the sound-alike words
 * corrected. Read-only: nothing is played or changed. */
#include "voice.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core.h"
#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "library.h"
#include "ma.h"
#include "secrets_gen.h"

static const char *TAG = "voice";

#define PIN_MIC_CLK 45
#define PIN_MIC_DATA 46
#define RATE 16000
#define FRAME 320               /* 20 ms */
#define MAX_MS 8000             /* longest recording */
#define NO_SPEECH_MS 5000       /* give up if nothing is said by then */
#define END_SILENCE_MS 900      /* this long quiet after speech: finished */
#define HA_STT SECRET_HA_SERVER "/api/stt/stt.faster_whisper"
#define MAX_ALBUMS 10
#define MAX_TRACKS 6
#define MAX_PARTIAL 6     /* albums matching all but one word: "new model army 225" */
#define MAX_PL_TRACKS 4   /* sets inside the playlists: a few, under the rest */
#define MAX_PL_PARTIAL 2  /* ...of which, at most, sets missing one of the words */
_Static_assert(MAX_ALBUMS + MAX_TRACKS + MAX_PL_TRACKS + MAX_PARTIAL <= UI_MAX_FOUND, "search screen too small");

typedef enum { EV_LEVEL, EV_SEARCHING, EV_TYPED, EV_RESULTS } ev_type_t;
typedef struct {
    ev_type_t type;
    float level;
} event_t;

static QueueHandle_t s_events;
static TaskHandle_t s_task;
static i2s_chan_handle_t s_rx;
static int16_t *s_audio;
static ma_conn_t *s_ma;
static atomic_int s_run;         /* 1 listening, 0 stop now, -1 cancelled */
static atomic_int s_session;     /* bumped by each listen and cancel; stale results are dropped */

/* Results handed to the LVGL thread (written before EV_RESULTS is posted) */
static char s_heard[96];
/* These live in PSRAM (allocated in voice_init): internal RAM is for Wi-Fi */
static ui_found_t *s_found;  /* MAX_ALBUMS + MAX_TRACKS + MAX_PL_TRACKS + MAX_PARTIAL */
static int s_nfound;
static const char *s_error;
#define RECORD_LEN 640
static char *s_record;             /* this attempt, for the console "voice" command */
static char (*s_history)[RECORD_LEN]; /* the last eight attempts */
static int s_history_n;
static char s_test[96];            /* test: words to search as if heard */
static int s_last_n;               /* samples in the last recording, for the console "stt" command */
static atomic_int s_stt_test;      /* console "stt": send the last recording again */

static void post(ev_type_t type, float level)
{
    event_t ev = {.type = type, .level = level};
    xQueueSend(s_events, &ev, 0);
}

/* ---- Recording -------------------------------------------------------------- */
static int record(void)
{
    int16_t frame[FRAME];
    int n = 0, frames = 0, loud = 0, quiet = 0;
    float noise = 0, peak_rms = 0;
    float hp_x = 0, hp_y = 0;  /* high-pass state: the microphone's output sits on a constant offset */
    char trace[420] = "";      /* loudness every 100 ms, for tuning */
    float trace_max = 0;
    bool speaking = false;
    i2s_channel_enable(s_rx);
    while (atomic_load(&s_run) == 1 && n + FRAME <= RATE * MAX_MS / 1000) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, frame, sizeof frame, &got, 200) != ESP_OK || got < sizeof frame) continue;
        double sum = 0;
        for (int i = 0; i < FRAME; i++) {
            float x = frame[i];
            hp_y = x - hp_x + 0.995f * hp_y;  /* about 13 Hz */
            hp_x = x;
            float y = fmaxf(-32768.0f, fminf(32767.0f, hp_y));
            s_audio[n + i] = (int16_t)y;
            sum += (double)y * y;
        }
        n += FRAME;
        float rms = sqrtf((float)(sum / FRAME));
        peak_rms = fmaxf(peak_rms, rms);
        frames++;
        trace_max = fmaxf(trace_max, rms);
        if (frames % 5 == 0 && strlen(trace) + 6 < sizeof trace) {
            char v[8];
            snprintf(v, sizeof v, "%d ", (int)trace_max);
            strlcat(trace, v, sizeof trace);
            trace_max = 0;
        }
        if (frames <= 5) continue;  /* the filter settles */
        if (frames <= 15) {  /* the next 200 ms set the room's noise level */
            noise += rms / 10;
            continue;
        }
        float threshold = fmaxf(noise * 3, 60);  /* a quiet room reads about 10 (26 Oct 2026) */
        if (!speaking) {
            loud = rms > threshold ? loud + 1 : 0;
            if (loud >= 3) speaking = true;
            if (!speaking && frames * 20 >= NO_SPEECH_MS) break;
        } else {
            quiet = rms < threshold * 0.7f ? quiet + 1 : 0;
            if (quiet * 20 >= END_SILENCE_MS) break;
        }
        if (frames % 3 == 0) post(EV_LEVEL, fminf(1.0f, rms / (threshold * 6)));
    }
    i2s_channel_disable(s_rx);
    ESP_LOGI(TAG, "VOICE recorded %d ms, speech %s, noise %.0f, loudest %.0f", n * 1000 / RATE,
             speaking ? "yes" : "no", noise, peak_rms);
    ESP_LOGI(TAG, "VOICE levels %s", trace);
    snprintf(s_record, RECORD_LEN, "recorded %d ms, speech %s, noise %.0f, loudest %.0f; levels %s",
             n * 1000 / RATE, speaking ? "yes" : "no", noise, peak_rms, trace);
    return speaking ? n : 0;
}

/* Whisper copes better with a full-scale recording than a quiet one. */
static void normalise(int n)
{
    int peak = 1;
    for (int i = 0; i < n; i++) peak = abs(s_audio[i]) > peak ? abs(s_audio[i]) : peak;
    float gain = fminf(16.0f, 23000.0f / peak);
    for (int i = 0; i < n; i++) s_audio[i] = (int16_t)(s_audio[i] * gain);
}

/* ---- Speech to text ------------------------------------------------------------ */
/* 30 s, not 15: faster-whisper unloads its model when it has been idle, and
 * the first recording after that waited 18 s for an answer (29 Oct 2026). */
static bool speech_to_text(int n, char *out, size_t max)
{
    esp_http_client_config_t cfg = {.url = HA_STT, .method = HTTP_METHOD_POST, .timeout_ms = 30000,
                                    .buffer_size_tx = 2048};
    ESP_LOGI(TAG, "VOICE sending %d bytes; internal RAM free %u, largest block %u", n * 2,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "speech-to-text: no memory for the request");
        return false;
    }
    esp_http_client_set_header(c, "Authorization", "Bearer " SECRET_HA_TOKEN);
    esp_http_client_set_header(c, "X-Speech-Content",
                               "format=wav; codec=pcm; sample_rate=16000; bit_rate=16; channel=1; language=en");
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    int len = n * 2, sent = 0;
    bool ok = false;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(c, len);
    if (err != ESP_OK) ESP_LOGE(TAG, "speech-to-text: connect failed: %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        while (sent < len) {
            int w = esp_http_client_write(c, (const char *)s_audio + sent, len - sent > 4096 ? 4096 : len - sent);
            if (w <= 0) break;
            sent += w;
        }
        if (sent != len) ESP_LOGE(TAG, "speech-to-text: sent %d of %d bytes", sent, len);
        if (sent == len && esp_http_client_fetch_headers(c) >= 0 && esp_http_client_get_status_code(c) == 200) {
            char body[512];
            int r = esp_http_client_read(c, body, sizeof body - 1);
            body[r > 0 ? r : 0] = 0;
            cJSON *j = cJSON_Parse(body);
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(j, "text");
            if (cJSON_IsString(text)) {
                snprintf(out, max, "%s", text->valuestring);
                ok = true;
            }
            cJSON_Delete(j);
        } else {
            ESP_LOGW(TAG, "speech-to-text: HTTP %d", esp_http_client_get_status_code(c));
        }
    }
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "VOICE heard \"%s\" in %lld ms", ok ? out : "", (esp_timer_get_time() - t0) / 1000);
    return ok;
}

/* ---- Search ------------------------------------------------------------------- */
static bool artist_has(const cJSON *track, const char *word)
{
    const cJSON *a = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(track, "artists"), 0);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
    if (!cJSON_IsString(name)) return false;
    char f[96];
    core_fold(name->valuestring, f, sizeof f);
    return strstr(f, word) != NULL;
}

/* Tracks for the words. Music Assistant matches the phrase as a whole, so if
 * "prince purple rain" finds nothing, try "purple rain" by an artist with
 * "prince" in the name, and so on. */
static int search_tracks(char words[][32], int nwords)
{
    int found = 0;
    /* Made in voice_init, while PSRAM is whole: asked for here, the first
     * search after a long session found no 96 KB in one piece and restarted
     * the dial (6 Nov 2026). Making it opens nothing - the socket is opened by
     * the first request, here, after the recording is uploaded: an open
     * connection costs internal RAM, which is what the upload runs on
     * (29 Oct 2026). */
    if (!s_ma) return 0;
    for (int skip = 0; skip < nwords && !found; skip++) {
        char phrase[128] = "";
        for (int w = skip; w < nwords; w++) {
            strlcat(phrase, words[w], sizeof phrase);
            if (w + 1 < nwords) strlcat(phrase, " ", sizeof phrase);
        }
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "search_query", phrase);
        cJSON *types = cJSON_AddArrayToObject(args, "media_types");
        cJSON_AddItemToArray(types, cJSON_CreateString("track"));
        cJSON_AddNumberToObject(args, "limit", 12);
        cJSON_AddBoolToObject(args, "library_only", true);
        cJSON *r = ma_call(s_ma, "music/search", args);
        const cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(r, "tracks"))
        {
            if (found >= MAX_TRACKS) break;
            bool by = true;
            for (int w = 0; w < skip && by; w++) by = artist_has(t, words[w]);
            const cJSON *album = cJSON_GetObjectItemCaseSensitive(t, "album");
            const cJSON *aid = cJSON_GetObjectItemCaseSensitive(album, "item_id");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
            if (!by || !cJSON_IsString(aid) || !cJSON_IsString(name)) continue;
            int idx = library_album_by_id(aid->valuestring);
            if (idx < 0) continue;
            ui_found_t *f = &s_found[s_nfound + found];
            const cJSON *a = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(t, "artists"), 0);
            const cJSON *aname = cJSON_GetObjectItemCaseSensitive(a, "name");
            f->album = idx;
            snprintf(f->title, sizeof f->title, "%s", name->valuestring);
            snprintf(f->track, sizeof f->track, "%s", name->valuestring);
            /* the same song is often on several albums: say which */
            (void)aname;
            snprintf(f->detail, sizeof f->detail, "Track on %s", library_get()->title(idx));
            found++;
        }
        cJSON_Delete(r);
    }
    return found;
}

/* Albums and playlists are ranked together, as one run of numbers: the albums
 * in browse order, then the playlists after them. */
static int s_nalbums;  /* where the playlists start, held still for one search */

static const char *search_key(int i)
{
    return i < s_nalbums ? library_key(i) : library_playlist_key(i - s_nalbums);
}

/* The tracks inside the playlists, as one run of numbers for ranking. They are
 * not library tracks, so Music Assistant's search never returns them. */
static void pl_track_at(int i, int *pl, int *tr)
{
    int np = library_playlists();
    for (int p = 0; p < np; p++) {
        int n = library_playlist_track_count(p);
        if (i < n) { *pl = p; *tr = i; return; }
        i -= n;
    }
    *pl = *tr = -1;
}

/* The title alone, where an album's own title goes: a set is ranked on what it
 * is called, not on which playlist it sits in - otherwise every Boiler Room set
 * would answer "boiler room" and push the playlist itself out of sight. */
static const char *pl_track_key(int i)
{
    static char key[160];
    int p, t;
    pl_track_at(i, &p, &t);
    key[0] = '\x01';
    core_fold(p < 0 ? "" : library_playlist_track(p, t), key + 1, sizeof key - 1);
    return key;
}

static core_candidate_t *s_pl_cand;  /* the sets ranked, kept for the second pass */
static core_candidate_t *s_cand;     /* the albums and playlists ranked */
static int s_pl_nc;

/* The ranked sets, either the ones with every word or the ones missing one. */
static int add_playlist_tracks(bool missing, int max)
{
    int n = 0;
    for (int i = 0; i < s_pl_nc && n < max; i++) {
        if (!s_pl_cand[i].missing != !missing) continue;
        int p, t;
        pl_track_at(s_pl_cand[i].album, &p, &t);
        if (p < 0) continue;
        const char *title = library_playlist_track(p, t);
        ui_found_t *f = &s_found[s_nfound++];
        f->album = UI_PLAYLIST(p);
        snprintf(f->title, sizeof f->title, "%s", title);
        snprintf(f->track, sizeof f->track, "%s", title);
        snprintf(f->detail, sizeof f->detail, "On %s", library_playlist_name(p));
        n++;
    }
    return n;
}

/* The sets that have every word. Those missing one wait until after the albums
 * that are missing one (add_playlist_partials): "boil a room" should land on
 * the Boiler Room playlist, not on a set that happens to be on it. */
static int search_playlist_tracks(char words[][32], int nwords)
{
    int total = 0, np = library_playlists();
    s_pl_nc = 0;
    for (int p = 0; p < np; p++) total += library_playlist_track_count(p);
    if (!total) return 0;
    if (!s_pl_cand) return 0;  /* voice_init */
    char spelt[8][32];  /* the album pass already fixed the spelling; this one's is dropped */
    s_pl_nc = core_rank_albums(words, nwords, total, pl_track_key, s_pl_cand, spelt);
    return add_playlist_tracks(false, MAX_PL_TRACKS);
}

/* ...and then the ones missing a word, a couple at most: "pan pot live at
 * fabric" came back as "pan pot wide at fabric", and one wrong word out of
 * five shouldn't lose the set (Darren, 29 Oct 2026). */
static int add_playlist_partials(int room)
{
    return add_playlist_tracks(true, room < MAX_PL_PARTIAL ? room : MAX_PL_PARTIAL);
}

/* An album: its title, and its artist underneath (no "Album" label: Darren, 27 Oct 2026).
 * A playlist has no artist, so it says so instead; without that a playlist and
 * an album of the same name would look identical. */
static void add_album(int i)
{
    ui_found_t *f = &s_found[s_nfound++];
    f->track[0] = 0;
    if (i >= s_nalbums) {
        int pl = i - s_nalbums;
        f->album = UI_PLAYLIST(pl);
        snprintf(f->title, sizeof f->title, "%s", library_playlist_name(pl));
        snprintf(f->detail, sizeof f->detail, "Playlist");
        return;
    }
    const char *artist = library_artist(i);
    f->album = i;
    snprintf(f->title, sizeof f->title, "%s", library_get()->title(i));
    snprintf(f->detail, sizeof f->detail, "%s", artist ? artist : "");
}

static void search(const char *heard)
{
    char words[CORE_SEARCH_WORDS][32], fixed[CORE_SEARCH_WORDS][32];
    int nwords = core_search_words(heard, words, CORE_SEARCH_WORDS);
    s_nfound = 0;
    if (!nwords) return;
    core_candidate_t *cand = s_cand;  /* voice_init */
    if (!cand) return;
    /* ranked on the knob (core_rank_albums: the rules and the library, no training) */
    s_nalbums = library_loaded();
    int64_t t0 = esp_timer_get_time();
    int nc = core_rank_albums(words, nwords, s_nalbums + library_playlists(), search_key, cand, fixed);
    int rank_ms = (int)((esp_timer_get_time() - t0) / 1000);
    int na = 0, np = 0;
    for (int i = 0; i < nc && na < MAX_ALBUMS; i++)
        if (!cand[i].missing) { add_album(cand[i].album); na++; }
    /* Music Assistant gets the phrase as said, articles and all */
    char phrase[CORE_PHRASE_WORDS][32];
    int nphrase = core_search_phrase(heard, fixed, nwords, phrase, CORE_PHRASE_WORDS);
    int nt = search_tracks(phrase, nphrase);
    s_nfound += nt;
    int ns = search_playlist_tracks(words, nwords);
    for (int i = 0; i < nc && np < MAX_PARTIAL; i++)
        if (cand[i].missing) { add_album(cand[i].album); np++; }
    ns += add_playlist_partials(MAX_PL_TRACKS - ns);
    char corrected[128] = "";
    for (int w = 0; w < nwords; w++) {
        strlcat(corrected, fixed[w], sizeof corrected);
        if (w + 1 < nwords) strlcat(corrected, " ", sizeof corrected);
    }
    ESP_LOGI(TAG, "VOICE \"%s\" (searched as \"%s\"): %d albums or playlists, %d tracks, %d in playlists, %d by part, ranked in %d ms",
             heard, corrected, na, nt, ns, np, rank_ms);
    for (int i = 0; i < s_nfound && i < 3; i++)
        ESP_LOGI(TAG, "VOICE   %d. %s (%s)", i + 1, s_found[i].title, s_found[i].detail);
    char more[200];
    snprintf(more, sizeof more, " | heard \"%s\" (as \"%s\"): %d albums or playlists, %d tracks, %d in playlists, %d by part",
             heard, corrected, na, nt, ns, np);
    strlcat(s_record, more, RECORD_LEN);
}

static void remember(void)
{
    snprintf(s_history[s_history_n++ % 8], RECORD_LEN, "%s", s_record);
}

/* ---- Task ----------------------------------------------------------------------- */
static void voice_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int session = atomic_load(&s_session);
        s_error = NULL;
        s_heard[0] = 0;
        s_nfound = 0;
        if (atomic_exchange(&s_stt_test, 0)) {  /* console "stt": the upload on its own */
            int n = s_last_n;
            if (!n) {  /* nothing recorded yet: two seconds of tone, which Whisper hears as nothing */
                n = RATE * 2;
                for (int i = 0; i < n; i++) s_audio[i] = (int16_t)(6000 * sinf(i * 0.05f));
            }
            char heard[96] = "";
            bool ok = speech_to_text(n, heard, sizeof heard);
            ESP_LOGI(TAG, "VOICE stt test: %s, heard \"%s\"", ok ? "worked" : "FAILED", heard);
            continue;
        }
        if (s_test[0]) {  /* typed words instead of speech: the microphone stays off */
            post(EV_TYPED, 0);
            snprintf(s_record, RECORD_LEN, "typed");
            snprintf(s_heard, sizeof s_heard, "%s", s_test);
            s_test[0] = 0;
            search(s_heard);
            remember();
            post(EV_RESULTS, 0);
            continue;
        }
        int n = record();
        s_last_n = n;
        if (atomic_load(&s_run) < 0 || session != atomic_load(&s_session)) continue;  /* cancelled */
        post(EV_SEARCHING, 0);
        if (n) {
            normalise(n);
            if (!speech_to_text(n, s_heard, sizeof s_heard)) s_error = "Couldn't reach Home Assistant";
            else {
                size_t len = strlen(s_heard);  /* "Purple rain." -> "Purple rain" */
                while (len && (s_heard[len - 1] == '.' || s_heard[len - 1] == ' ')) s_heard[--len] = 0;
                search(s_heard);
            }
        }
        if (s_error) strlcat(s_record, " | Home Assistant didn't answer", RECORD_LEN);
        remember();
        if (session == atomic_load(&s_session)) post(EV_RESULTS, 0);
    }
}

/* Test: send the last recording to Home Assistant again (console "stt"). */
void voice_test_stt(void)
{
    atomic_store(&s_stt_test, 1);
    xTaskNotifyGive(s_task);
}

/* Test: search typed words as if they'd been heard (console "heard <words>"). */
void voice_test_heard(const char *text)
{
    snprintf(s_test, sizeof s_test, "%s", text);
    atomic_fetch_add(&s_session, 1);
    atomic_store(&s_run, -1);  /* end any recording in progress */
    xTaskNotifyGive(s_task);
}

/* Forget what was heard: the history and the last results (console "voice clear"). */
void voice_clear(void)
{
    for (int i = 0; i < 8; i++) s_history[i][0] = 0;
    s_history_n = 0;
    s_record[0] = 0;
    s_heard[0] = 0;
    ESP_LOGI(TAG, "VOICE history cleared");
}

static void start_listening(void)
{
    atomic_fetch_add(&s_session, 1);
    atomic_store(&s_run, 1);
    xTaskNotifyGive(s_task);
}

static void stop_listening(void) { atomic_store(&s_run, 0); }

static void cancel_listening(void)
{
    atomic_fetch_add(&s_session, 1);
    atomic_store(&s_run, -1);
}

static const ui_voice_t s_voice = {.listen = start_listening, .stop = stop_listening, .cancel = cancel_listening};

/* The microphone's DMA memory must be internal: taken before Wi-Fi starts. */
void voice_mic_init(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 4;
    chan.dma_frame_num = FRAME;
    if (i2s_new_channel(&chan, NULL, &s_rx) != ESP_OK) {
        s_rx = NULL;
        return;
    }
    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = PIN_MIC_CLK, .din = PIN_MIC_DATA},
    };
    if (i2s_channel_init_pdm_rx_mode(s_rx, &pdm) != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
}

const ui_voice_t *voice_init(void)
{
    s_audio = heap_caps_malloc(RATE * MAX_MS / 1000 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_found = heap_caps_calloc(MAX_ALBUMS + MAX_TRACKS + MAX_PL_TRACKS + MAX_PARTIAL, sizeof(ui_found_t),
                               MALLOC_CAP_SPIRAM);
    s_record = heap_caps_calloc(1, RECORD_LEN, MALLOC_CAP_SPIRAM);
    s_history = heap_caps_calloc(8, RECORD_LEN, MALLOC_CAP_SPIRAM);
    s_events = xQueueCreate(16, sizeof(event_t));
    /* Everything a search needs, taken now while PSRAM is whole rather than at
     * the first search, when a session of covers has left it in pieces. */
    s_cand = heap_caps_malloc(CORE_SEARCH_CANDIDATES * sizeof(core_candidate_t), MALLOC_CAP_SPIRAM);
    s_pl_cand = heap_caps_malloc(CORE_SEARCH_CANDIDATES * sizeof(core_candidate_t), MALLOC_CAP_SPIRAM);
    /* 48 KB: twelve tracks came back as 14 KB, the largest 2.6 KB (6 Nov 2026),
     * so even twelve of the largest fit with room over */
    s_ma = ma_open(48 * 1024);
    if (!s_rx || !s_audio || !s_found || !s_record || !s_history) {
        ESP_LOGW(TAG, "no microphone: voice search off");
        return NULL;
    }
    if (xTaskCreatePinnedToCoreWithCaps(voice_task, "voice", 16384, NULL, 3, &s_task, 0, MALLOC_CAP_SPIRAM) != pdPASS)
        return NULL;
    ESP_LOGI(TAG, "microphone ready");
    return &s_voice;
}

void voice_log(void)
{
    int first = s_history_n > 8 ? s_history_n - 8 : 0;
    if (s_history_n == 0) ESP_LOGI(TAG, "VOICE nothing yet");
    for (int i = first; i < s_history_n; i++) ESP_LOGI(TAG, "VOICE %d: %s", i + 1, s_history[i % 8]);
}

void voice_poll(void)
{
    event_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case EV_LEVEL: ui_voice_level(ev.level); break;
        case EV_SEARCHING: ui_voice_searching(); break;
        case EV_TYPED: ui_voice_typed(); break;
        case EV_RESULTS: ui_voice_results(s_heard, s_found, s_nfound, s_error); break;
        }
    }
}
