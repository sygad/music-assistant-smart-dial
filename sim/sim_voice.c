/*
 * Voice search for the simulator. The page has no speech-to-text, so you type
 * what you'd say, while the knob is listening, and Enter is the end of the
 * sentence. Otherwise it follows the firmware's voice.c, except for the time
 * limits: typing takes longer than speaking (Darren, 27 Oct 2026: 5 s to start
 * and 8 s at most timed out mid-phrase on a phone), so 15 s to start and 30 s
 * at most here. Then a "searching" pause about as long as Home
 * Assistant's speech-to-text takes, then the same search: albums ranked on the
 * knob (core_rank_albums), then tracks, then albums that have all but one word.
 *
 * Tracks: the firmware asks Music Assistant (music/search, library only),
 * which matches the phrase inside track titles; here the same match runs over
 * the copied library.
 */
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "sim.h"

#define MAX_MS 30000       /* the knob: 8 s of speech */
#define NO_SPEECH_MS 15000 /* the knob: 5 s */
#define STT_MS 1100  /* Home Assistant's speech-to-text, measured 26 Oct 2026 */
#define MAX_ALBUMS 10
#define MAX_TRACKS 6
#define MAX_PARTIAL 6
#define MA_LIMIT 12  /* tracks Music Assistant returns per search */

typedef enum { IDLE, LISTENING, SEARCHING } state_t;
static state_t s_state;
static lv_timer_t *s_timer;
static uint32_t s_started, s_stopped;
static float s_level;
static char s_text[96];
static ui_found_t s_found[MAX_ALBUMS + MAX_TRACKS + MAX_PARTIAL];
static int s_nfound;

EM_JS(void, js_listening, (int on), { if (Module.onListening) Module.onListening(on); });

/* ---- Search (as voice.c) ------------------------------------------------------- */
static bool artist_has(const char *artist, const char *word)
{
    char f[96];
    core_fold(artist ? artist : "", f, sizeof f);
    return strstr(f, word) != NULL;
}

/* Tracks for the words. Music Assistant matches the phrase as a whole, so if
 * "prince purple rain" finds nothing, try "purple rain" by an artist with
 * "prince" in the name, and so on. */
static int search_tracks(char words[][32], int nwords)
{
    int found = 0;
    for (int skip = 0; skip < nwords && !found; skip++) {
        char phrase[128] = "";
        for (int w = skip; w < nwords; w++) {
            strlcat(phrase, words[w], sizeof phrase);
            if (w + 1 < nwords) strlcat(phrase, " ", sizeof phrase);
        }
        int returned = 0;
        for (int a = 0; a < sim_library_loaded() && returned < MA_LIMIT && found < MAX_TRACKS; a++) {
            for (int t = 0; t < sim_library_track_count(a) && returned < MA_LIMIT && found < MAX_TRACKS; t++) {
                char name[128];
                core_fold(sim_library_track(a, t), name, sizeof name);
                if (!strstr(name, phrase)) continue;
                returned++;
                bool by = true;
                for (int w = 0; w < skip && by; w++) by = artist_has(sim_library_track_artist(a, t), words[w]);
                if (!by) continue;
                ui_found_t *f = &s_found[s_nfound + found];
                f->album = a;
                snprintf(f->title, sizeof f->title, "%s", sim_library_track(a, t));
                snprintf(f->track, sizeof f->track, "%s", sim_library_track(a, t));
                /* the same song is often on several albums: say which */
                snprintf(f->detail, sizeof f->detail, "Track on %s", sim_library()->title(a));
                found++;
            }
        }
    }
    return found;
}

/* An album: its title, and its artist underneath (no "Album" label: Darren, 27 Oct 2026). */
static void add_album(int album)
{
    const char *artist = sim_library_artist(album);
    ui_found_t *f = &s_found[s_nfound++];
    f->album = album;
    snprintf(f->title, sizeof f->title, "%s", sim_library()->title(album));
    snprintf(f->detail, sizeof f->detail, "%s", artist ? artist : "");
    f->track[0] = 0;
}

static void search(const char *heard)
{
    char words[8][32], fixed[8][32];
    int nwords = core_search_words(heard, words, 8);
    s_nfound = 0;
    if (!nwords) return;
    static core_candidate_t cand[CORE_SEARCH_CANDIDATES];
    int nc = core_rank_albums(words, nwords, sim_library_loaded(), sim_library_key, cand, fixed);
    int na = 0, np = 0;
    for (int i = 0; i < nc && na < MAX_ALBUMS; i++)
        if (!cand[i].missing) { add_album(cand[i].album); na++; }
    /* Music Assistant gets the phrase as said, articles and all */
    char phrase[CORE_PHRASE_WORDS][32];
    int nphrase = core_search_phrase(heard, fixed, nwords, phrase, CORE_PHRASE_WORDS);
    int nt = search_tracks(phrase, nphrase);
    s_nfound += nt;
    for (int i = 0; i < nc && np < MAX_PARTIAL; i++)
        if (cand[i].missing) { add_album(cand[i].album); np++; }
    printf("VOICE \"%s\": %d albums, %d tracks, %d albums by part\n", heard, na, nt, np);
}

/* ---- Listening ------------------------------------------------------------------ */
static void finish(void)
{
    s_state = SEARCHING;
    s_stopped = lv_tick_get();
    js_listening(0);
    ui_voice_searching();
}

static void step(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    if (s_state == LISTENING) {
        /* the level the knob shows: room noise, and a peak with each key */
        s_level *= 0.8f;
        float noise = 0.04f + 0.03f * (float)(rand() % 100) / 100.0f;
        ui_voice_level(s_level > noise ? s_level : noise);
        uint32_t since = lv_tick_elaps(s_started);
        if ((!s_text[0] && since >= NO_SPEECH_MS) || since >= MAX_MS) finish();
    } else if (s_state == SEARCHING && now - s_stopped >= STT_MS) {
        s_state = IDLE;
        lv_timer_pause(s_timer);
        if (!s_text[0]) { ui_voice_results(NULL, NULL, 0, NULL); return; }
        size_t len = strlen(s_text);  /* "Purple rain." -> "Purple rain" */
        while (len && (s_text[len - 1] == '.' || s_text[len - 1] == ' ')) s_text[--len] = 0;
        search(s_text);
        ui_voice_results(s_text, s_found, s_nfound, NULL);
    }
}

static void listen(void)
{
    s_state = LISTENING;
    s_text[0] = 0;
    s_level = 0;
    s_started = lv_tick_get();
    if (!s_timer) s_timer = lv_timer_create(step, 20, NULL);  /* 20 ms frames, as the microphone */
    lv_timer_resume(s_timer);
    js_listening(1);
}

static void stop(void)
{
    if (s_state == LISTENING) finish();
}

static void cancel(void)
{
    s_state = IDLE;
    if (s_timer) lv_timer_pause(s_timer);
    js_listening(0);
}

/* From the page: what's been typed so far, and Enter. */
EMSCRIPTEN_KEEPALIVE void sim_voice_text(const char *text)
{
    if (s_state != LISTENING) return;
    snprintf(s_text, sizeof s_text, "%s", text);
    s_level = 0.55f + 0.4f * (float)(rand() % 100) / 100.0f;
}

EMSCRIPTEN_KEEPALIVE void sim_voice_enter(void) { stop(); }

static const ui_voice_t s_voice = {.listen = listen, .stop = stop, .cancel = cancel};
const ui_voice_t *sim_voice(void) { return &s_voice; }
