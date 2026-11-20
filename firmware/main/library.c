/*
 * The album library, loaded live from Music Assistant.
 *
 * At start-up the whole album list is fetched, 200 at a time (title, artist,
 * id, cover id), then sorted here by artist and title, with compilations under
 * their own title (core_album_key). Track lists are fetched when an album
 * is opened and the last few are kept. Covers are fetched and decoded only
 * around where Album browse is.
 *
 * Covers: a pool of POOL buffers, each holding one album's cover ready to draw
 * (360x360, panel byte order). Albums that drift more than WINDOW away give
 * their buffer back. Sharp covers are only decoded once Album browse has
 * settled (decoding competes with drawing for PSRAM), except for the album a
 * fling is going to land on, which is fetched the moment the finger lifts.
 *
 * Playlists (27 Oct 2026): after the albums, every playlist and its track list
 * is fetched once (names, lengths, picture ids). Each playlist track has its
 * own picture (YouTube thumbnails: wide, some with black bars), loaded around
 * where Now Playing is (+/-1 track, a pool of PIC_POOL) and cropped to its
 * centre square between any bars, the same as the simulator does.
 *
 * Recent (27 Oct 2026): after the playlists, the tracks Music Assistant says
 * were played lately, partly played ones too, each with where it lives (a
 * library track on its album, a YouTube track on the playlist it's on),
 * fetched again each time Recent opens. What the knob plays stays on top
 * (simulated playback isn't seen by Music Assistant yet).
 *
 * Threads: the UI calls the library on the LVGL thread. One loader task on
 * core 0 does all network and decoding work and queues results, which the
 * LVGL thread applies in library_poll(). A cover buffer is only reused after
 * the UI has stopped pointing at it.
 */
#include "library.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ma.h"
#include "secrets_gen.h"

static const char *TAG = "library";

#define SIDE 360
#define COVER_BYTES (SIDE * SIDE * 2)
/* Two, not three. A third was tried and measured on 29 Oct 2026: 8.4 covers a
 * second against 10.8, with the same 1.3 actually working at once, so whatever
 * limits this is not the number of requests in the air. */
#define LOADERS 2
#define POOL 14          /* the window, plus one spare per loader */
#define WINDOW 5         /* settled: this many either side */
/* Moving, the window leans the way it is going. The covers behind have been
 * seen; the ones ahead have not, and at 5.3 albums a second against a loader
 * that manages 8.2, every cover spent behind him is one the leading edge
 * doesn't get (measured 28 Oct 2026). AHEAD + BEHIND + 1 must fit POOL. */
/* Deeper than it was, now that the download buffers are a quarter the size and
 * there is PSRAM to spare: more runway in front, and more of a cushion through
 * a change of direction. */
#define WINDOW_AHEAD  9
#define WINDOW_BEHIND 2
#define MOVING 0.05f     /* albums per second: below this, Album browse has settled */
#define SPEED_TAU_MS 300.0f  /* how long a reported speed is smoothed over, and lives for */
/* The biggest thing this has to hold is one cover or one playlist picture,
 * both asked for at 256 px: the largest in Darren's library is 47 KB, so 128
 * leaves plenty of room. It was 300 KB, sized for the 512 px covers, and three
 * of those would not fit beside everything else - this is what pays for the
 * third loader (29 Oct 2026). */
#define IMG_MAX (128 * 1024)
/* Albums a page: each page's JSON is parsed into cJSON nodes in PSRAM, and at
 * 200 that peak took the heap down to 4 bytes free and left it fragmented -
 * which is what made album, playlist and recent fetches fail at random
 * (measured 28 Oct 2026). Smaller pages, more round trips of a few tens of
 * milliseconds each. */
#define PAGE 50
#define TRACK_CACHE 8
#define PIC_POOL 3        /* playlist track pictures: the one on screen and one either side */
#define PIC_WINDOW 1

/* ---- Albums (written once by the loader, then read-only) ------------------ */
typedef struct {
    const char *title;
    const char *artist;  /* as shown: "Various Artists" for most compilations; NULL if none */
    const char *item_id;
    const char *proxy;  /* cover id, NULL if the album has no cover */
    const char *key;    /* browse order */
    char letter;
} album_t;

static album_t *s_albums;
static _Atomic int s_count;  /* albums loaded so far; entries below this never change */
static int s_total;

/* ---- Track lists (LVGL thread owns the cache) ----------------------------- */
typedef struct {
    int album;
    ui_tracks_t t;
    const char **uri;  /* each track's Music Assistant uri, to play it */
    uint32_t used;
} track_entry_t;
static track_entry_t s_tracks[TRACK_CACHE];
static _Atomic int s_tracks_wanted = -1;  /* album whose tracks the UI asked for */
static uint32_t s_tracks_clock;

/* ---- Covers ----------------------------------------------------------------- */
typedef struct {
    uint8_t *px;
    lv_image_dsc_t dsc;
    int album;  /* -1 when free */
} buf_t;

static buf_t s_pool[POOL];
static SemaphoreHandle_t s_pool_lock;
static int *s_showing;                 /* pool index shown for each album, -1 none (LVGL thread) */
static _Atomic uint8_t *s_have;        /* 1 if a cover is shown, read by the loader */
static _Atomic uint8_t *s_pending;     /* 1 while a cover is loading */
static _Atomic int s_focus_x100, s_speed_x100;
static _Atomic long long s_speed_us;  /* when that speed was reported */
static _Atomic int s_dir;             /* the way it last actually travelled: +1, -1, or 0 before it has */
static _Atomic int s_landing = -1;
static SemaphoreHandle_t s_wake[LOADERS];  /* one each: a binary semaphore only wakes one waiter */
static SemaphoreHandle_t s_pick_lock;      /* choosing an album and claiming its buffer */
static SemaphoreHandle_t s_small_lock;     /* s_small, the scratch both loaders would decode into */

static void wake_loaders(void)
{
    for (int i = 0; i < LOADERS; i++) xSemaphoreGive(s_wake[i]);
}
static library_stats_t s_stats;
static _Atomic bool s_paused;  /* test: stop loading covers */

/* ---- Loader -> LVGL thread ------------------------------------------------- */
typedef enum { EV_LIBRARY, EV_COVER, EV_TRACKS, EV_RADIO, EV_PLAYLISTS, EV_PICTURE, EV_RECENT } ev_type_t;
typedef struct {
    ev_type_t type;
    int album;       /* EV_PICTURE: the playlist (UI_PLAYLIST) */
    int buf;
    int track;       /* EV_PICTURE */
    ui_tracks_t tracks;
    const char **uri;  /* EV_TRACKS: each track's uri */
} event_t;

/* ---- Playlists (written once by the loader, then read-only) ------------------ */
typedef struct {
    const char *name, *id;
    const char *uri;       /* the playlist itself, to play it */
    const char *key;       /* the name folded, for voice search */
    ui_tracks_t t;
    const char **picture;  /* each track's picture id, NULL if none */
    uint8_t *pic_fail;     /* failed fetches, so a hiccup isn't the end of it */
    const char **track_id; /* each track's Music Assistant id */
    const char **uri_of;   /* ...and its uri, to start the playlist on it */
} playlist_t;

/* ---- Recent (LVGL thread; the loader fills s_recent_in once, then EV_RECENT) ---- */
#define MAX_RECENT 20
typedef struct {
    int coll;
    char track[96];  /* held here: refreshes come and go, nothing to free */
} recent_t;
/* In PSRAM, with the rest of the library (6 Nov 2026): 6 KB of text that only
 * the screens read. */
EXT_RAM_BSS_ATTR static recent_t s_recent[MAX_RECENT];
EXT_RAM_BSS_ATTR static recent_t s_recent_in[MAX_RECENT];
EXT_RAM_BSS_ATTR static recent_t s_mine[MAX_RECENT];  /* s_mine: played here */
static int s_nrecent, s_nrecent_in, s_nmine;
static _Atomic bool s_recent_wanted;  /* Recent opened: fetch it again */

static playlist_t *s_pl;
static _Atomic int s_npl;

typedef struct {
    uint8_t *px;
    lv_image_dsc_t dsc;
    int playlist, track;  /* playlist -1 when free */
} pic_t;

static pic_t s_pics[PIC_POOL];
static _Atomic int s_pic_pl = -1;          /* the playlist Now Playing is on */
static _Atomic int s_pic_focus_x100;
static QueueHandle_t s_events;

static int focus_album(void) { return (int)lroundf(atomic_load(&s_focus_x100) / 100.0f); }

/* How fast it is going, now rather than when it last said so.
 *
 * The reported speed is only refreshed while the strip is moving: when it
 * stops, the last word on the matter stands for ever. Spinning the knob hard
 * and letting go left the library believing it was still travelling at 34
 * albums a second, which put every album out of reach of the prefetch and
 * stopped covers loading altogether until it was rebooted (Darren, 28 Oct
 * 2026: "99% no album covers"). Reading it back decayed to nothing over the
 * same window the smoothing uses keeps the gap between knob detents bridged
 * and still comes to rest when he does. */
static float lib_speed(void)
{
    long long when = atomic_load(&s_speed_us);
    if (!when) return 0.0f;
    float age = (esp_timer_get_time() - when) / 1000.0f;  /* ms */
    if (age >= SPEED_TAU_MS) return 0.0f;
    return (atomic_load(&s_speed_x100) / 100.0f) * (1.0f - age / SPEED_TAU_MS);
}
/* The one definition of what is wanted. All three users go through here - what
 * to fetch, what to keep when it arrives, what to evict - so they cannot
 * disagree with each other, which is how covers got loaded and then thrown
 * away unseen before. */
/* Which way the window leans: the way it last travelled while it is moving,
 * and neither way once it has genuinely stopped. Leaning is worth it in
 * motion, because the covers behind have been seen and the ones ahead have
 * not. Standing still it costs: parked facing forwards with only two covers
 * kept behind, turning round meant scrolling straight into empty sleeves
 * (Darren, pausing on D and going back towards B, 29 Oct 2026). A pause this
 * long is a decision to stop, not the lull between two detents. */
#define SETTLED_MS 1000.0f

static int lean(void)
{
    long long when = atomic_load(&s_speed_us);
    if (!when) return 0;
    if ((esp_timer_get_time() - when) / 1000.0f > SETTLED_MS) return 0;
    return atomic_load(&s_dir);
}

static bool in_window(int album, int c)
{
    /* Which way it leans comes from the last direction it actually travelled,
     * not from how fast it is going this instant. Reading the speed here meant
     * the window flipped between leaning forward and leaning back many times a
     * second, because the strip reports a real speed at each detent and zero
     * the moment that detent lands. Every flip evicted one half of the pool
     * and asked for the other, so the loader spent its time re-fetching covers
     * it had just thrown away - idle 39% of the time while Darren scrolled,
     * and loading albums 319-321 and 308-310 alternately, over and over (29
     * Oct 2026, on a strong signal where there was nothing else left to blame). */
    int ahead = WINDOW, behind = WINDOW;
    int dir = lean();
    if (dir) {
        ahead = dir > 0 ? WINDOW_AHEAD : WINDOW_BEHIND;
        behind = dir > 0 ? WINDOW_BEHIND : WINDOW_AHEAD;
    }
    return album >= c - behind && album <= c + ahead;
}
static bool near_landing(int album)
{
    int l = atomic_load(&s_landing);
    return l >= 0 && album >= l - 1 && album <= l + 1;
}

static void *ps_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
/* NULL for NULL, and NULL when PSRAM is out - it used to memcpy into the NULL
 * and restart the dial (crash.c caught it in load_albums, 6 Nov 2026). */
static char *ps_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = ps_alloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- Lookups for voice search (any thread, once the library has loaded) ----- */
int library_album_by_id(const char *item_id)
{
    int n = atomic_load(&s_count);
    for (int i = 0; i < n; i++)
        if (!strcmp(s_albums[i].item_id, item_id)) return i;
    return -1;
}

const char *library_artist(int album) { return album >= 0 && album < atomic_load(&s_count) ? s_albums[album].artist : NULL; }

int library_loaded(void) { return atomic_load(&s_count); }
const char *library_key(int album) { return s_albums[album].key; }
int library_playlists(void) { return atomic_load(&s_npl); }
const char *library_playlist_key(int p) { return s_pl[p].key; }
const char *library_playlist_name(int p) { return s_pl[p].name; }
int library_playlist_track_count(int p) { return s_pl[p].t.count; }
const char *library_playlist_track(int p, int track) { return s_pl[p].t.title[track]; }

/* ---- UI side (LVGL thread) -------------------------------------------------- */
static int lib_count(void) { return atomic_load(&s_count); }
static const char *lib_title(int a) { return s_albums[a].title; }
static char lib_letter(int a) { return s_albums[a].letter; }

static const lv_image_dsc_t *lib_cover(int a)
{
    int b = s_showing[a];
    return b < 0 ? NULL : &s_pool[b].dsc;
}

static const ui_tracks_t *lib_tracks(int a)
{
    if (UI_IS_PLAYLIST(a)) return a - UI_PLAYLIST_BASE < atomic_load(&s_npl) ? &s_pl[a - UI_PLAYLIST_BASE].t : NULL;
    for (int i = 0; i < TRACK_CACHE; i++)
        if (s_tracks[i].album == a) {
            s_tracks[i].used = ++s_tracks_clock;
            return &s_tracks[i].t;
        }
    atomic_store(&s_tracks_wanted, a);
    wake_loaders();
    return NULL;
}

static void release_buf(int b)
{
    xSemaphoreTake(s_pool_lock, portMAX_DELAY);
    s_pool[b].album = -1;
    xSemaphoreGive(s_pool_lock);
    wake_loaders();
}

static void drop_far_covers(void)
{
    int c = focus_album(), n = lib_count();
    for (int a = 0; a < n; a++) {
        if (s_showing[a] >= 0 && !in_window(a, c) && !near_landing(a)) {
            int b = s_showing[a];
            s_showing[a] = -1;
            atomic_store(&s_have[a], 0);
            ui_cover_changed(a);
            release_buf(b);
        }
    }
}

/* How fast it is travelling, smoothed over about a third of a second.
 *
 * The reported speed can't be used as it stands. A swipe is one long drag and
 * reports a real speed throughout, but the knob snaps one album per click and
 * reports zero the instant each snap lands - so between clicks the library was
 * told it had stopped, went back to keeping covers evenly either side, and
 * spent half the loader on albums already passed. Swiping kept up and the dial
 * didn't (Darren, 28 Oct 2026). Smoothing reads a run of clicks as what it is:
 * sustained movement in one direction. */
static void lib_focus(float pos, float speed)
{
    static float s_rate;      /* albums a second, smoothed */
    static int64_t s_rate_us;
    int64_t now = esp_timer_get_time();
    float dt = s_rate_us ? (now - s_rate_us) / 1000000.0f : 0.0f;
    s_rate_us = now;
    if (dt > 0.0f) {
        float k = dt * 1000.0f / SPEED_TAU_MS;  /* how much of this sample to take */
        s_rate += (speed - s_rate) * (k > 1.0f ? 1.0f : k);
    } else {
        s_rate = speed;
    }
    /* whichever reads as moving faster: the smoothed run, or this very moment */
    float use = fabsf(speed) > fabsf(s_rate) ? speed : s_rate;

    atomic_store(&s_focus_x100, (int)lroundf(pos * 100));
    atomic_store(&s_speed_x100, (int)lroundf(use * 100));
    /* Only movement counts as movement. The strip ticks again every time a
     * cover arrives and lands on it, reporting a speed of zero - and stamping
     * the time here regardless meant the library thought it was still in
     * motion, so the window stayed leaning. A second later it would relax to
     * even, drop the covers on the leading side and fetch the ones behind,
     * which made a cover arrive, which stamped the time again: albums 44-46
     * and 33-35 loading alternately, for ever, while it sat still (29 Oct
     * 2026). */
    if (fabsf(use) > MOVING) {
        atomic_store(&s_speed_us, now);
        atomic_store(&s_dir, use > 0 ? 1 : -1);
    }
    if (fabsf(use) <= MOVING) atomic_store(&s_landing, -1);
    drop_far_covers();
    wake_loaders();
}

static void lib_landing(int album)
{
    atomic_store(&s_landing, album);
    wake_loaders();
}

/* ---- Playlists, UI side (LVGL thread) ------------------------------------------- */
static int lib_playlists(void) { return atomic_load(&s_npl); }
static const char *lib_playlist_name(int p) { return s_pl[p].name; }
static int lib_playlist_size(int p) { return s_pl[p].t.count; }

static int pic_find(int pl, int track)
{
    for (int b = 0; b < PIC_POOL; b++)
        if (s_pics[b].playlist == pl && s_pics[b].track == track) return b;
    return -1;
}

static bool pic_wanted(int pl, int track)
{
    int c = (int)lroundf(atomic_load(&s_pic_focus_x100) / 100.0f);
    return pl == atomic_load(&s_pic_pl) && track >= c - PIC_WINDOW && track <= c + PIC_WINDOW;
}

static const lv_image_dsc_t *lib_track_cover(int pl, int track)
{
    int b = pic_find(pl, track);
    return b < 0 ? NULL : &s_pics[b].dsc;
}

static void lib_track_focus(int pl, float track)
{
    atomic_store(&s_pic_pl, pl);
    atomic_store(&s_pic_focus_x100, (int)lroundf(track * 100));
    for (int b = 0; b < PIC_POOL; b++)
        if (s_pics[b].playlist >= 0 && !pic_wanted(s_pics[b].playlist, s_pics[b].track)) {
            int p = s_pics[b].playlist, t = s_pics[b].track;
            xSemaphoreTake(s_pool_lock, portMAX_DELAY);
            s_pics[b].playlist = -1;
            xSemaphoreGive(s_pool_lock);
            ui_track_cover_changed(p, t);
        }
    wake_loaders();
}

/* ---- Recent, UI side (LVGL thread) ---------------------------------------------- */
static const char *lib_artist(int a) { return library_artist(a); }
static int lib_recent(void) { return s_nrecent; }
static int lib_recent_item(int i, const char **track)
{
    *track = s_recent[i].track[0] ? s_recent[i].track : NULL;
    return s_recent[i].coll;
}

/* Put a track at the front of `list`: the same track already there moves up,
 * a full list drops its oldest. */
static void list_front(recent_t *list, int *count, int coll, const char *track)
{
    int at = 0;
    while (at < *count && !(list[at].coll == coll && !strcmp(list[at].track, track ? track : ""))) at++;
    if (at == *count) {
        if (*count < MAX_RECENT) (*count)++;
        else at = MAX_RECENT - 1;
    }
    memmove(&list[1], &list[0], at * sizeof(recent_t));
    list[0].coll = coll;
    snprintf(list[0].track, sizeof list[0].track, "%s", track ? track : "");
}

/* The knob started playing: to the top, and kept there over Music
 * Assistant's list until playback is real. */
static void lib_played(int coll, const char *track)
{
    list_front(s_mine, &s_nmine, coll, track);
    list_front(s_recent, &s_nrecent, coll, track);
    ui_recent_changed();
}

static void lib_recent_refresh(void)
{
    atomic_store(&s_recent_wanted, true);
    wake_loaders();
}

static const ui_library_t s_lib = {
    .count = lib_count, .title = lib_title, .letter = lib_letter, .cover = lib_cover,
    .tracks = lib_tracks, .focus = lib_focus, .landing = lib_landing,
    .playlists = lib_playlists, .playlist_name = lib_playlist_name, .playlist_size = lib_playlist_size,
    .track_cover = lib_track_cover, .track_focus = lib_track_focus,
    .artist = lib_artist, .recent = lib_recent, .recent_item = lib_recent_item, .played = lib_played,
    .recent_refresh = lib_recent_refresh,
};

const ui_library_t *library_get(void) { return &s_lib; }

static void store_tracks(int album, ui_tracks_t t, const char **uri)
{
    int slot = 0;
    for (int i = 1; i < TRACK_CACHE; i++)
        if (s_tracks[i].used < s_tracks[slot].used) slot = i;
    track_entry_t *e = &s_tracks[slot];
    if (e->album >= 0) {
        for (int i = 0; i < e->t.count; i++) {
            free((void *)e->t.title[i]);
            if (e->uri) free((void *)e->uri[i]);
        }
        free((void *)e->t.title);
        free((void *)e->t.seconds);
        free((void *)e->uri);
    }
    e->album = album;
    e->t = t;
    e->uri = uri;
    e->used = ++s_tracks_clock;
}

/* ---- What to play (31 Oct 2026) --------------------------------------------------
 * Music Assistant plays by uri, so every track carries its own and a collection
 * carries one too: an album's is made from its id, a playlist keeps the one it
 * came with (a playlist's tracks are not all from the library, so theirs cannot
 * be made up either). Read on the LVGL thread, where the track cache lives, so
 * what the player task is handed is already resolved. */
bool library_collection_uri(int coll, char *buf, size_t n)
{
    if (UI_IS_PLAYLIST(coll)) {
        int p = coll - UI_PLAYLIST_BASE;
        if (p >= atomic_load(&s_npl) || !s_pl[p].uri || !*s_pl[p].uri) return false;
        snprintf(buf, n, "%s", s_pl[p].uri);
        return true;
    }
    if (coll < 0 || coll >= atomic_load(&s_count)) return false;
    snprintf(buf, n, "library://album/%s", s_albums[coll].item_id);
    return true;
}

const char *library_track_uri(int coll, int track)
{
    if (track < 0) return NULL;
    if (UI_IS_PLAYLIST(coll)) {
        int p = coll - UI_PLAYLIST_BASE;
        if (p >= atomic_load(&s_npl) || track >= s_pl[p].t.count) return NULL;
        const char *u = s_pl[p].uri_of ? s_pl[p].uri_of[track] : NULL;
        return u && *u ? u : NULL;
    }
    for (int i = 0; i < TRACK_CACHE; i++)
        if (s_tracks[i].album == coll && track < s_tracks[i].t.count) {
            const char *u = s_tracks[i].uri ? s_tracks[i].uri[track] : NULL;
            return u && *u ? u : NULL;
        }
    return NULL;
}

void library_poll(void)
{
    /* Keep the window honest whether or not it is moving. Eviction used to
     * happen only when the strip reported a move, so once it stopped, the pool
     * kept whatever it was holding - and after a run forwards that is eight
     * covers ahead and two behind. Turning round then started with two covers
     * of runway and spent the next six seconds catching up, which is what
     * Darren saw as gaps whenever he changed direction. Standing still the
     * window is even, so a few seconds of stopping is enough to be ready for
     * either way (29 Oct 2026). */
    static int64_t s_balance_at;
    int64_t now = esp_timer_get_time();
    if (now >= s_balance_at) {
        s_balance_at = now + 200000;  /* five times a second is plenty, and costs nothing */
        drop_far_covers();
        wake_loaders();
    }
    event_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case EV_LIBRARY:
            ui_library_changed();
            break;
        case EV_RADIO:
            ui_menu_radio(ev.album != 0);
            break;
        case EV_PLAYLISTS:
            ui_playlists_changed();
            break;
        case EV_RECENT:
            memcpy(s_recent, s_recent_in, sizeof s_recent);
            s_nrecent = s_nrecent_in;
            for (int i = s_nmine - 1; i >= 0; i--) list_front(s_recent, &s_nrecent, s_mine[i].coll, s_mine[i].track);
            ui_recent_changed();
            break;
        case EV_PICTURE: {
            /* the loader filled pool slot ev.buf, marked (-2, track) while loading */
            bool keep = pic_wanted(ev.album, ev.track) && pic_find(ev.album, ev.track) < 0;
            xSemaphoreTake(s_pool_lock, portMAX_DELAY);
            s_pics[ev.buf].playlist = keep ? ev.album : -1;
            s_pics[ev.buf].track = ev.track;
            xSemaphoreGive(s_pool_lock);
            if (keep) ui_track_cover_changed(ev.album, ev.track);
            wake_loaders();
            break;
        }
        case EV_TRACKS:
            store_tracks(ev.album, ev.tracks, ev.uri);
            ui_tracks_changed(ev.album);
            break;
        case EV_COVER: {
            atomic_store(&s_pending[ev.album], 0);
            bool wanted = (in_window(ev.album, focus_album()) || near_landing(ev.album)) && s_showing[ev.album] < 0;
            if (!wanted) {
                release_buf(ev.buf);
                break;
            }
            s_showing[ev.album] = ev.buf;
            atomic_store(&s_have[ev.album], 1);
            ui_cover_changed(ev.album);
            break;
        }
        }
    }
}

/* ---- Loader (core 0) -------------------------------------------------------- */
static ma_conn_t *s_ma;                 /* the API connection: lists and track lists */
static ma_conn_t *s_conn[LOADERS];      /* a connection each, for the covers */
static uint8_t *s_img[LOADERS];  /* the JPEG each loader is downloading */
static uint8_t *s_small;  /* scratch for covers smaller than 360 px */

static void post(event_t ev) { xQueueSend(s_events, &ev, portMAX_DELAY); }

static const char *json_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int album_cmp(const void *a, const void *b)
{
    return strcmp(((const album_t *)a)->key, ((const album_t *)b)->key);
}

/* One album off the stream (ma_each). The pages used to be parsed whole: fifty
 * albums with all their metadata became one cJSON tree several times the size
 * of the text, and that tree is what took PSRAM down to 20 KB free at boot and
 * left what remained in pieces - so the next large request, voice search's,
 * found nothing big enough and the dial restarted (6 Nov 2026). Streamed, only
 * one album is ever a tree. */
typedef struct {
    int loaded;
    bool out_of_memory;
} album_load_t;

static bool take_album(const cJSON *item, int index, void *ctx)
{
    (void)index;
    album_load_t *L = ctx;
    if (L->loaded >= s_total) return false;
    const char *name = json_str(item, "name"), *sort = json_str(item, "sort_name"), *id = json_str(item, "item_id");
    if (!name || !id) return true;
    const cJSON *artist = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(item, "artists"), 0);
    const char *artist_name = json_str(artist, "name");
    const char *artist_sort = json_str(artist, "sort_name");
    const char *proxy = NULL;
    const cJSON *imgs = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item, "metadata"), "images");
    const cJSON *img;
    cJSON_ArrayForEach(img, imgs) if ((proxy = json_str(img, "proxy_id"))) break;
    char key[200];
    core_album_key(artist_sort ? artist_sort : artist_name, sort ? sort : name,
                   core_is_compilation(artist_name, name), key, sizeof key);
    album_t *a = &s_albums[L->loaded];
    a->title = ps_strdup(name);
    a->artist = ps_strdup(artist_name);
    a->item_id = ps_strdup(id);
    a->proxy = ps_strdup(proxy);
    a->key = ps_strdup(key);
    if (!a->title || !a->item_id || !a->key || (artist_name && !a->artist) || (proxy && !a->proxy)) {
        /* out of PSRAM: stop with the albums we have, rather than keep one half made */
        heap_caps_free((void *)a->title);
        heap_caps_free((void *)a->artist);
        heap_caps_free((void *)a->item_id);
        heap_caps_free((void *)a->proxy);
        heap_caps_free((void *)a->key);
        memset(a, 0, sizeof *a);
        L->out_of_memory = true;
        return false;
    }
    a->letter = core_letter(key);
    L->loaded++;
    return true;
}

static bool load_albums(void)
{
    cJSON *n = ma_call(s_ma, "music/albums/count", NULL);
    if (!cJSON_IsNumber(n)) {
        cJSON_Delete(n);
        return false;
    }
    s_total = n->valueint;
    cJSON_Delete(n);
    s_albums = heap_caps_calloc(s_total, sizeof(album_t), MALLOC_CAP_SPIRAM);
    s_showing = heap_caps_malloc(s_total * sizeof(int), MALLOC_CAP_SPIRAM);
    s_have = heap_caps_calloc(s_total, 1, MALLOC_CAP_SPIRAM);
    s_pending = heap_caps_calloc(s_total, 1, MALLOC_CAP_SPIRAM);
    if (!s_albums || !s_showing || !s_have || !s_pending) {
        ESP_LOGE(TAG, "no PSRAM for a library of %d albums", s_total);
        heap_caps_free(s_albums);
        heap_caps_free(s_showing);
        heap_caps_free(s_have);
        heap_caps_free(s_pending);
        s_albums = NULL;
        s_showing = NULL;
        s_have = s_pending = NULL;
        return false;
    }
    for (int i = 0; i < s_total; i++) s_showing[i] = -1;

    int64_t t0 = esp_timer_get_time();
    album_load_t L = {0};
    while (L.loaded < s_total && !L.out_of_memory) {
        cJSON *args = cJSON_CreateObject();
        cJSON_AddNumberToObject(args, "limit", PAGE);
        cJSON_AddNumberToObject(args, "offset", L.loaded);
        cJSON_AddStringToObject(args, "order_by", "sort_name");
        int was = L.loaded;
        int got = ma_each(s_ma, "music/albums/library_items", args, take_album, &L);
        if (got <= 0 || L.loaded == was) break;
    }
    if (L.out_of_memory) ESP_LOGE(TAG, "out of PSRAM after %d of %d albums", L.loaded, s_total);
    int loaded = L.loaded;
    /* the order is only known once everything is in, so the UI sees it all at once */
    qsort(s_albums, loaded, sizeof(album_t), album_cmp);
    atomic_store(&s_count, loaded);
    post((event_t){.type = EV_LIBRARY});
    ESP_LOGI(TAG, "LIBRARY %d albums loaded in %lld ms", loaded, (esp_timer_get_time() - t0) / 1000);

    /* Radio is in the menu only when Music Assistant has stations (Darren, 26 Oct 2026) */
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "limit", 1);
    cJSON *radios = ma_call(s_ma, "music/radios/library_items", args);
    int has_radio = cJSON_GetArraySize(radios) > 0;
    cJSON_Delete(radios);
    post((event_t){.type = EV_RADIO, .album = has_radio});
    ESP_LOGI(TAG, "LIBRARY radio stations: %s", has_radio ? "yes" : "none");
    return loaded > 0;
}

/* Every playlist with its tracks, after the albums (10 playlists, ~600 tracks
 * for Darren: a few seconds). Returns false when Music Assistant didn't
 * answer, so the loader can come back to it: a Wi-Fi hiccup here used to empty
 * Playlists for the rest of the session, with only a reboot to fix it
 * (Darren, 28 Oct 2026). */
/* Tracks arrive one at a time and the length is not known in advance, so the
 * two arrays double as they fill. */
typedef struct {
    ui_tracks_t t;
    int room;
    const char **uri;      /* every track's own uri: what gets played */
    const char **picture;  /* playlists only: each track's artwork */
    const char **track_id;
} gather_t;

static bool gather_room(gather_t *g, bool with_ids)
{
    if (g->t.count < g->room) return true;
    int want = g->room ? g->room * 2 : 16;
    void *title = heap_caps_realloc((void *)g->t.title, sizeof(char *) * want, MALLOC_CAP_SPIRAM);
    void *secs = heap_caps_realloc((void *)g->t.seconds, sizeof(uint16_t) * want, MALLOC_CAP_SPIRAM);
    void *uri = heap_caps_realloc((void *)g->uri, sizeof(char *) * want, MALLOC_CAP_SPIRAM);
    if (!title || !secs || !uri) return false;
    g->t.title = title;
    g->t.seconds = secs;
    g->uri = uri;
    if (with_ids) {
        void *pic = heap_caps_realloc((void *)g->picture, sizeof(char *) * want, MALLOC_CAP_SPIRAM);
        void *id = heap_caps_realloc((void *)g->track_id, sizeof(char *) * want, MALLOC_CAP_SPIRAM);
        if (!pic || !id) return false;
        g->picture = pic;
        g->track_id = id;
    }
    g->room = want;
    return true;
}

static bool take_pl_track(const cJSON *tr, int index, void *ctx)
{
    (void)index;
    gather_t *g = ctx;
    const char *tn = json_str(tr, "name");
    if (!tn) return true;
    if (!gather_room(g, true)) return false;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(tr, "duration");
    const char *pic = NULL;
    const cJSON *img;
    cJSON_ArrayForEach(img, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(tr, "metadata"), "images"))
        if ((pic = json_str(img, "proxy_id"))) break;
    int i = g->t.count++;
    const char *tid = json_str(tr, "item_id"), *uri = json_str(tr, "uri");
    g->track_id[i] = ps_strdup(tid ? tid : "");
    g->uri[i] = ps_strdup(uri ? uri : "");
    ((const char **)g->t.title)[i] = ps_strdup(tn);
    int sec = cJSON_IsNumber(d) ? d->valueint : 0;
    ((uint16_t *)g->t.seconds)[i] = (uint16_t)(sec > 65535 ? 65535 : sec);
    g->picture[i] = pic ? ps_strdup(pic) : NULL;
    return true;
}

static bool take_track(const cJSON *tr, int index, void *ctx)
{
    (void)index;
    gather_t *g = ctx;
    const char *name = json_str(tr, "name");
    if (!name) return true;
    if (!gather_room(g, false)) return false;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(tr, "duration");
    const char *uri = json_str(tr, "uri");
    ((const char **)g->t.title)[g->t.count] = ps_strdup(name);
    ((uint16_t *)g->t.seconds)[g->t.count] = (uint16_t)(cJSON_IsNumber(d) ? d->valueint : 0);
    g->uri[g->t.count] = ps_strdup(uri ? uri : "");
    g->t.count++;
    return true;
}

static bool load_playlists(void)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "limit", 100);
    cJSON *list = ma_call(s_ma, "music/playlists/library_items", args);
    if (!list) return false;
    int n = cJSON_GetArraySize(list);
    s_pl = heap_caps_calloc(n ? n : 1, sizeof(playlist_t), MALLOC_CAP_SPIRAM);
    int k = 0;
    const cJSON *pl;
    cJSON_ArrayForEach(pl, list)
    {
        const char *name = json_str(pl, "name"), *id = json_str(pl, "item_id"), *prov = json_str(pl, "provider");
        const char *uri = json_str(pl, "uri");
        if (!name || !id || !prov) continue;
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "item_id", id);
        cJSON_AddStringToObject(a, "provider_instance_id_or_domain", prov);
        gather_t g = {0};
        int got = ma_each(s_ma, "music/playlists/playlist_tracks", a, take_pl_track, &g);
        playlist_t *p = &s_pl[k];
        p->name = ps_strdup(name);
        p->id = ps_strdup(id);
        p->uri = ps_strdup(uri ? uri : "");
        char key[200];  /* no artist, so the name goes where a compilation's title does */
        core_album_key(name, name, true, key, sizeof key);
        p->key = ps_strdup(key);
        p->t = g.t;
        p->track_id = g.track_id;
        p->uri_of = g.uri;
        p->picture = g.picture;
        p->pic_fail = heap_caps_calloc(g.t.count ? g.t.count : 1, 1, MALLOC_CAP_SPIRAM);
        if (!p->t.title) {  /* an empty playlist still has to be openable */
            p->t.title = ps_alloc(sizeof(char *));
            p->t.seconds = ps_alloc(sizeof(uint16_t));
            p->track_id = ps_alloc(sizeof(char *));
            p->uri_of = ps_alloc(sizeof(char *));
            p->picture = ps_alloc(sizeof(char *));
        }
        if (got < 0 || p->t.count != got)
            ESP_LOGW(TAG, "PLAYLIST %s: kept %d of %d", name, p->t.count, got);
        ESP_LOGI(TAG, "PLAYLIST %s: %d tracks", name, p->t.count);
        k++;
    }
    cJSON_Delete(list);
    atomic_store(&s_npl, k);
    post((event_t){.type = EV_PLAYLISTS});
    ESP_LOGI(TAG, "LIBRARY %d playlists", k);
    return true;
}

/* Recently played tracks, newest first. Partly played ones too: Music
 * Assistant counts only finished ones by default, and a DJ set takes an hour. */
static bool load_recent(void)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "limit", 50);
    cJSON *types = cJSON_AddArrayToObject(args, "media_types");
    cJSON_AddItemToArray(types, cJSON_CreateString("track"));
    cJSON_AddBoolToObject(args, "fully_played_only", false);
    cJSON *list = ma_call(s_ma, "music/recently_played_items", args);
    if (!list) return false;
    int npl = atomic_load(&s_npl), n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        if (n >= MAX_RECENT) break;
        const char *kind = json_str(it, "media_type"), *id = json_str(it, "item_id"), *prov = json_str(it, "provider");
        const char *name = json_str(it, "name");
        if (!kind || !id || !prov) continue;
        bool library = !strcmp(prov, "library");
        int coll = -1;
        const char *track = NULL;
        if (strcmp(kind, "track") || !name) continue;
        if (library) {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "item_id", id);
            cJSON_AddStringToObject(a, "provider_instance_id_or_domain", "library");
            cJSON *t = ma_call(s_ma, "music/tracks/get", a);
            const char *album = json_str(cJSON_GetObjectItemCaseSensitive(t, "album"), "item_id");
            if (album) coll = library_album_by_id(album);
            cJSON_Delete(t);
            track = name;
        } else {
            for (int i = 0; i < npl && coll < 0; i++)
                for (int k = 0; k < s_pl[i].t.count && coll < 0; k++)
                    if (!strcmp(s_pl[i].track_id[k], id)) coll = UI_PLAYLIST(i);
            track = name;
        }
        if (coll < 0) continue;
        bool seen = false;  /* the same track again */
        for (int k = 0; k < n; k++) seen |= s_recent_in[k].coll == coll && !strcmp(s_recent_in[k].track, track);
        if (seen) continue;
        s_recent_in[n].coll = coll;
        snprintf(s_recent_in[n].track, sizeof s_recent_in[n].track, "%s", track);
        n++;
    }
    cJSON_Delete(list);
    s_nrecent_in = n;
    post((event_t){.type = EV_RECENT});
    ESP_LOGI(TAG, "LIBRARY %d recently played", n);
    return true;
}

static void load_tracks(int album)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "item_id", s_albums[album].item_id);
    cJSON_AddStringToObject(args, "provider_instance_id_or_domain", "library");
    int64_t t0 = esp_timer_get_time();
    gather_t g = {0};
    int got = ma_each(s_ma, "music/albums/album_tracks", args, take_track, &g);
    if (!g.t.title) {  /* nothing at all: still give the screen something to show */
        g.t.title = heap_caps_malloc(sizeof(char *), MALLOC_CAP_SPIRAM);
        g.t.seconds = heap_caps_malloc(sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        g.uri = heap_caps_calloc(1, sizeof(char *), MALLOC_CAP_SPIRAM);
    }
    if (got < 0 || g.t.count != got)
        ESP_LOGW(TAG, "TRACKS album %d: kept %d of %d", album, g.t.count, got);
    ESP_LOGI(TAG, "TRACKS album %d: %d tracks in %lld ms", album, g.t.count, (esp_timer_get_time() - t0) / 1000);
    post((event_t){.type = EV_TRACKS, .album = album, .tracks = g.t, .uri = g.uri});
}

/* Nearest-pixel enlarge, for covers smaller than 360 px. */
/* Blow a small cover up to fill the screen, in the buffer it was decoded into.
 *
 * Every cover takes this path now that they come at 256 px, and going through
 * one shared scratch meant the two loaders decoded one at a time: 1.67 of them
 * busy instead of 2, which was the difference between keeping ahead of Darren
 * and losing by a cover a second (29 Oct 2026). A second scratch would cost
 * 253 KB that the PSRAM has not got.
 *
 * Done backwards, in place, which is safe for enlarging: every pixel's source
 * sits at or before its destination, so working from the last pixel down, the
 * source has not been written yet. */
static void enlarge_from(const uint8_t *src, int sw, int sh, uint8_t *dst)
{
    int16_t xs[SIDE];
    for (int x = 0; x < SIDE; x++) xs[x] = (int16_t)(x * sw / SIDE);
    const uint16_t *s16 = (const uint16_t *)src;
    uint16_t *d16 = (uint16_t *)dst;
    for (int y = 0; y < SIDE; y++) {
        const uint16_t *row = s16 + (y * sh / SIDE) * sw;
        uint16_t *o = d16 + y * SIDE;
        for (int x = 0; x < SIDE; x++) o[x] = row[xs[x]];
    }
}

static void enlarge(int sw, int sh, uint8_t *buf)
{
    int16_t xs[SIDE];  /* on the stack: both loaders are in here at once */
    for (int x = 0; x < SIDE; x++) xs[x] = (int16_t)(x * sw / SIDE);
    uint16_t *px = (uint16_t *)buf;
    for (int y = SIDE - 1; y >= 0; y--) {
        const uint16_t *row = px + (y * sh / SIDE) * sw;
        uint16_t *out = px + y * SIDE;
        for (int x = SIDE - 1; x >= 0; x--) out[x] = row[xs[x]];
    }
}

/* Decode to 360x360 RGB565 (panel order). Scales down while decoding when the
 * source is big enough (~100 ms for 512 px); otherwise decodes and enlarges. */
static bool decode(const uint8_t *jpg, int len, uint8_t *out)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    jpeg_dec_handle_t h;
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)jpg, .inbuf_len = len};
    jpeg_dec_header_info_t info;
    if (jpeg_dec_open(&cfg, &h) != JPEG_ERR_OK) return false;
    bool ok = jpeg_dec_parse_header(h, &io, &info) == JPEG_ERR_OK;
    jpeg_dec_close(h);
    if (!ok) return false;
    bool big = info.width >= SIDE && info.height >= SIDE;
    if (!big && (info.width > 512 || info.height > 512)) return false;
    cfg.scale.width = big ? SIDE : 0;
    cfg.scale.height = big ? SIDE : 0;
    /* A cover at least 360 px decodes straight into its own pool buffer, so
     * two loaders can do that at once. Anything smaller goes through s_small,
     * which there is only one of, so that path is taken one at a time. */
    /* Where it decodes decides whether the two loaders can work at once. A
     * cover that fits the pool buffer - 360 px square or less once scaled -
     * goes straight into its own, and is blown up there; nothing is shared, so
     * both loaders decode together. Anything else has to go through the one
     * shared scratch, one at a time. The pool buffer holds SIDE x SIDE pixels
     * and no more: decoding a wider, shorter cover into it would run off the
     * end of it and into the next buffer. */
    bool fits = !big && (long)info.width * info.height <= (long)SIDE * SIDE;
    uint8_t *into = (big || fits) ? out : s_small;
    if (into == s_small) xSemaphoreTake(s_small_lock, portMAX_DELAY);
    ok = jpeg_dec_open(&cfg, &h) == JPEG_ERR_OK;
    if (ok) {
        io = (jpeg_dec_io_t){.inbuf = (uint8_t *)jpg, .inbuf_len = len};
        ok = jpeg_dec_parse_header(h, &io, &info) == JPEG_ERR_OK;
        io.outbuf = into;
        if (ok) ok = jpeg_dec_process(h, &io) == JPEG_ERR_OK;
        jpeg_dec_close(h);
        if (ok && !big) {
            if (into == out) enlarge(info.width, info.height, out);
            else enlarge_from(s_small, info.width, info.height, out);
        }
    }
    if (into == s_small) xSemaphoreGive(s_small_lock);
    return ok;
}

/* A playlist track's picture: the decoded thumbnail (w x h, RGB565 big-endian)
 * cropped to the square in its middle, between any black bars above and
 * below, and scaled to 360x360. */
static bool dark_row(const uint8_t *src, int w, int y)
{
    uint32_t sum = 0;
    for (int x = 0; x < w; x += 4) {
        uint16_t c = (uint16_t)(src[(y * w + x) * 2] << 8 | src[(y * w + x) * 2 + 1]);
        sum += ((c >> 11) & 0x1F) * 8 + ((c >> 5) & 0x3F) * 4 + (c & 0x1F) * 8;
    }
    return sum / (3 * ((w + 3) / 4)) < 18;
}

static void crop_square(const uint8_t *src, int w, int h, uint8_t *dst)
{
    int top = 0, bottom = h - 1;
    while (top < h / 3 && dark_row(src, w, top)) top++;
    while (bottom > 2 * h / 3 && dark_row(src, w, bottom)) bottom--;
    int content = bottom - top + 1, side = content < w ? content : w;
    int x0 = (w - side) / 2, y0 = top + (content - side) / 2;
    const uint16_t *s16 = (const uint16_t *)src;
    uint16_t *d16 = (uint16_t *)dst;
    for (int y = 0; y < SIDE; y++) {
        const uint16_t *row = s16 + (y0 + y * side / SIDE) * w + x0;
        for (int x = 0; x < SIDE; x++) d16[y * SIDE + x] = row[x * side / SIDE];
    }
}

static bool decode_picture(const uint8_t *jpg, int len, uint8_t *out)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    jpeg_dec_handle_t h;
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)jpg, .inbuf_len = len};
    jpeg_dec_header_info_t info;
    if (jpeg_dec_open(&cfg, &h) != JPEG_ERR_OK) return false;
    xSemaphoreTake(s_small_lock, portMAX_DELAY);  /* shared with the cover loaders */
    bool ok = jpeg_dec_parse_header(h, &io, &info) == JPEG_ERR_OK && info.width <= 512 && info.height <= 512;
    io.outbuf = s_small;
    if (ok) ok = jpeg_dec_process(h, &io) == JPEG_ERR_OK;
    jpeg_dec_close(h);
    if (ok) crop_square(s_small, info.width, info.height, out);
    xSemaphoreGive(s_small_lock);  /* playlist pictures still use the shared scratch */
    return ok;
}

/* The next picture to load: the track on screen, then its neighbours. */
static bool next_picture(int *pl, int *track)
{
    int p = atomic_load(&s_pic_pl);
    if (p < 0 || p - UI_PLAYLIST_BASE >= atomic_load(&s_npl)) return false;
    const playlist_t *list = &s_pl[p - UI_PLAYLIST_BASE];
    int c = (int)lroundf(atomic_load(&s_pic_focus_x100) / 100.0f);
    /* Nearest first, and only as far out as PIC_WINDOW: asking for one the
     * window doesn't want meant it was thrown away the moment it arrived, so
     * the pool churned on tracks it would never keep and the one on screen
     * waited behind them (Darren, 28 Oct 2026: no picture on a playlist). */
    static const int order[] = {0, 1, -1};
    _Static_assert(sizeof order / sizeof *order == 2 * PIC_WINDOW + 1, "picture order must match PIC_WINDOW");
    for (int i = 0; i < (int)(sizeof order / sizeof *order); i++) {
        int t = c + order[i];
        if (t < 0 || t >= list->t.count || !list->picture[t]) continue;
        bool have = false;
        xSemaphoreTake(s_pool_lock, portMAX_DELAY);
        for (int b = 0; b < PIC_POOL; b++)
            have |= (s_pics[b].playlist == p || s_pics[b].playlist == -2) && s_pics[b].track == t;
        xSemaphoreGive(s_pool_lock);
        if (have) continue;
        *pl = p;
        *track = t;
        return true;
    }
    return false;
}

static void load_picture(int pl, int track)
{
    int b = -1;
    xSemaphoreTake(s_pool_lock, portMAX_DELAY);
    for (int k = 0; k < PIC_POOL && b < 0; k++)
        if (s_pics[k].playlist == -1) { b = k; s_pics[k].playlist = -2; s_pics[k].track = track; }  /* loading */
    xSemaphoreGive(s_pool_lock);
    if (b < 0) {
        xSemaphoreTake(s_wake[0], pdMS_TO_TICKS(100));
        return;
    }
    char url[160];
    snprintf(url, sizeof url, SECRET_MA_SERVER "/imageproxy/%s?size=256",
             s_pl[pl - UI_PLAYLIST_BASE].picture[track]);
    int n = ma_get(s_conn[0], url, s_img[0], IMG_MAX, NULL, NULL);
    if (n <= 0 || !decode_picture(s_img[0], n, s_pics[b].px)) {
        /* Two more goes before the placeholder is permanent: one failure used
         * to blank that set's picture for the rest of the session, so a Wi-Fi
         * hiccup while a playlist opened could never be recovered from without
         * a reboot (29 Oct 2026). */
        playlist_t *list = &s_pl[pl - UI_PLAYLIST_BASE];
        bool done = !list->pic_fail || ++list->pic_fail[track] >= 3;
        ESP_LOGW(TAG, "PICTURE %d/%d failed%s", pl - UI_PLAYLIST_BASE, track, done ? ", giving up on it" : ", will retry");
        if (done) list->picture[track] = NULL;
        xSemaphoreTake(s_pool_lock, portMAX_DELAY);
        s_pics[b].playlist = -1;
        xSemaphoreGive(s_pool_lock);
        if (!done) xSemaphoreTake(s_wake[0], pdMS_TO_TICKS(2000));  /* space the goes out: a hiccup lasts longer than a loop */
        return;
    }
    post((event_t){.type = EV_PICTURE, .album = pl, .track = track, .buf = b});
}

static int take_free_buf(int album)
{
    int got = -1;
    xSemaphoreTake(s_pool_lock, portMAX_DELAY);
    for (int b = 0; b < POOL && got < 0; b++)
        if (s_pool[b].album < 0) { s_pool[b].album = album; got = b; }
    xSemaphoreGive(s_pool_lock);
    return got;
}

static bool needs_cover(int a)
{
    return a >= 0 && a < lib_count() && s_albums[a].proxy && !atomic_load(&s_have[a]) && !atomic_load(&s_pending[a]);
}

/* What to load next: the landing album of a fling first, then (once settled)
 * the nearest album in the window, looking ahead in the direction of travel. */
static int next_cover(void)
{
    int l = atomic_load(&s_landing);
    if (l >= 0) {
        static const int around[] = {0, 1, -1};
        for (int i = 0; i < 3; i++)
            if (needs_cover(l + around[i])) return l + around[i];
    }
    /* No speed cutoff. A cap here turned "the loader cannot quite keep up"
     * into "the loader does nothing at all", which is far worse to look at:
     * above it, not one cover was fetched. The window decides instead - the
     * nearest album ahead is always worth starting, and one that scrolls past
     * before it arrives is abandoned on its own merits. */
    int c = focus_album(), dir = lean() < 0 ? -1 : 1;
    /* Ahead of the scroll first, and as far ahead as in_window will keep.
     * Stopping at 4 while the window reached 8 left the far half of the window
     * empty however long it had (28 Oct 2026).
     *
     * in_window has the last word on every candidate. While it is moving the
     * window leans forward, 8 ahead and 2 behind, but this order reaches 5
     * behind - so the loader kept picking an album three, four or five back,
     * which cover_still_wanted threw out on its first poll, before a single
     * byte had arrived. It then picked the same album again, in a loop:
     * 22308 covers abandoned against 101 loaded, and nothing on screen while
     * Darren turned the knob (measured 28 Oct 2026). The two must agree, so
     * only one of them decides. */
    static const int order[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -2, -3, -4, -5};
    _Static_assert(WINDOW_AHEAD <= 9 && WINDOW_BEHIND <= 5 && WINDOW <= 5,
                   "the fetch order must reach as far as the window");
    _Static_assert(WINDOW_AHEAD + WINDOW_BEHIND + 1 <= POOL, "the window must fit the pool");
    for (int i = 0; i < (int)(sizeof order / sizeof *order); i++) {
        int a = c + order[i] * dir;
        if (needs_cover(a) && in_window(a, c)) return a;
    }
    return -1;
}

static bool cover_still_wanted(void *arg)
{
    int a = (int)(intptr_t)arg;
    /* The same question next_cover asks, so a cover is never started on one
     * rule and dropped by another. */
    return near_landing(a) || in_window(a, focus_album());
}

/* Choosing an album and claiming the buffer for it is one step: with two
 * loaders, deciding outside the lock let both land on the same album and
 * fetch it twice. Returns -1 when there is nothing to do, or nothing free to
 * do it in. */
static int claim_cover(int *buf)
{
    xSemaphoreTake(s_pick_lock, portMAX_DELAY);
    int a = atomic_load(&s_paused) ? -1 : next_cover();
    int b = a >= 0 ? take_free_buf(a) : -1;
    if (b >= 0) atomic_store(&s_pending[a], 1);
    else if (a < 0) s_stats.idle_nothing++;  /* which of the two keeps a loader idle */
    else s_stats.idle_nobuf++;
    xSemaphoreGive(s_pick_lock);
    if (b < 0) return -1;
    *buf = b;
    return a;
}

static void load_cover(int me, int album, int b)
{
    char url[160];
    /* 256, not 512. Music Assistant serves only those two sizes - everything
     * between is refused outright, which is what made an earlier try at 360
     * fail on every cover. Across 40 of Darren's covers 512 px averages 51 KB
     * and 256 px averages 24 KB, and the dial's problem is bytes over the air,
     * not the server: it manages under 1 Mbit/s while he is scrolling, against
     * 38 from a Mac on the same network (measured 29 Oct 2026). The screen
     * shows 360 px, so these are scaled up and a little softer. */
    snprintf(url, sizeof url, SECRET_MA_SERVER "/imageproxy/%s?size=256", s_albums[album].proxy);
    int64_t t0 = esp_timer_get_time();
    int n = ma_get(s_conn[me], url, s_img[me], IMG_MAX, cover_still_wanted, (void *)(intptr_t)album);
    int64_t t1 = esp_timer_get_time();
    bool ok = n > 0 && decode(s_img[me], n, s_pool[b].px);
    int64_t t2 = esp_timer_get_time();
    if (!ok) {
        if (n == -2) {
            s_stats.abandoned++;  /* a lost count here only skews the diagnostics */
        }
        else if (n == -3) {
            /* The network, not the cover. Writing these off was what left
             * holes in the covers after a Wi-Fi hiccup: one roam between
             * access points took the link down for three and a half seconds,
             * and the 27 albums that happened to be loading were marked as
             * having no artwork for the rest of the session (Darren, 29 Oct
             * 2026). They are simply left wanted, after a pause long enough
             * that the loaders don't spin on a link that is still down. */
            s_stats.failures++;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else {
            s_stats.failures++;
            atomic_store(&s_have[album], 1);  /* broken: don't retry in a loop; the placeholder stays */
            ESP_LOGW(TAG, "COVER %d failed: %d bytes back", album, n);
        }
        atomic_store(&s_pending[album], 0);
        release_buf(b);
        return;
    }
    xSemaphoreTake(s_pick_lock, portMAX_DELAY);  /* both loaders keep the same tally */
    s_stats.fulls++;
    s_stats.fetch_ms += (uint32_t)((t1 - t0) / 1000);
    s_stats.decode_ms += (uint32_t)((t2 - t1) / 1000);
    xSemaphoreGive(s_pick_lock);
    ESP_LOGI(TAG, "COVER %d %d bytes, fetch %lld ms, decode %lld ms", album, n, (t1 - t0) / 1000, (t2 - t1) / 1000);
    post((event_t){.type = EV_COVER, .album = album, .buf = b});
}

static void loader_task(void *arg)
{
    (void)arg;
    while (!load_albums()) {
        ESP_LOGW(TAG, "album list failed, retrying");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    /* The playlists and the recently played are fetched in here, behind the
     * covers, and retried on a timer. Retrying them before this loop instead
     * stopped covers and track lists loading at all while Music Assistant was
     * unreachable, which left Albums stuck on "loading albums" with no way out
     * (Darren, 28 Oct 2026). Nothing the screen is waiting on may queue behind
     * a list that cannot be fetched. */
    bool want_playlists = true, want_recent = true;
    int64_t next_list_try = 0;
    for (;;) {
        int t = atomic_exchange(&s_tracks_wanted, -1);
        if (t >= 0 && t < lib_count()) {
            load_tracks(t);
            continue;
        }
        int b, a = claim_cover(&b);
        if (a >= 0) {
            load_cover(0, a, b);
            continue;
        }
        int pl, track;
        if (!atomic_load(&s_paused) && next_picture(&pl, &track)) {
            load_picture(pl, track);
            continue;
        }
        if (atomic_exchange(&s_recent_wanted, false)) want_recent = true;  /* Recent was opened */
        if ((want_playlists || want_recent) && esp_timer_get_time() >= next_list_try) {
            if (want_playlists) want_playlists = !load_playlists();
            else want_recent = !load_recent();
            if (want_playlists || want_recent) {
                ESP_LOGW(TAG, "%s not loaded, trying again in 5 s", want_playlists ? "playlists" : "recently played");
                next_list_try = esp_timer_get_time() + 5000000;
            }
            continue;
        }
        xSemaphoreTake(s_wake[0], pdMS_TO_TICKS(500));
    }
}

/* The second loader does covers and nothing else. A cover costs about 56 ms
 * waiting on Music Assistant to resize it, 41 ms pulling it down and 27 ms
 * decoding (measured over 209 covers, 28 Oct 2026), so a single loader spent
 * most of its time with the network idle. Two of them put one cover's wait
 * under another's transfer. The album list, the track lists and the playlist
 * pictures stay on the first loader: they are one at a time by nature and
 * share the connection its API calls use. */
static void cover_loader(void *arg)
{
    int me = (int)(intptr_t)arg;
    for (;;) {
        int b, a = claim_cover(&b);
        if (a >= 0) load_cover(me, a, b);
        else xSemaphoreTake(s_wake[me], pdMS_TO_TICKS(500));
    }
}

void library_init(void)
{
    ma_init();
    s_pool_lock = xSemaphoreCreateMutex();
    s_pick_lock = xSemaphoreCreateMutex();
    s_small_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < LOADERS; i++) s_wake[i] = xSemaphoreCreateBinary();
    s_events = xQueueCreate(POOL + 8, sizeof(event_t));
    for (int i = 0; i < TRACK_CACHE; i++) s_tracks[i].album = -1;
    for (int b = 0; b < POOL; b++) {
        s_pool[b].px = jpeg_calloc_align(COVER_BYTES, 16);
        s_pool[b].album = -1;
        s_pool[b].dsc = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
                       .w = SIDE, .h = SIDE, .stride = SIDE * 2},
            .data_size = COVER_BYTES,
            .data = s_pool[b].px,
        };
    }
    for (int b = 0; b < PIC_POOL; b++) {
        s_pics[b].px = jpeg_calloc_align(COVER_BYTES, 16);
        s_pics[b].playlist = -1;
        s_pics[b].dsc = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
                       .w = SIDE, .h = SIDE, .stride = SIDE * 2},
            .data_size = COVER_BYTES,
            .data = s_pics[b].px,
        };
    }
    for (int i = 0; i < LOADERS; i++) s_img[i] = heap_caps_malloc(IMG_MAX, MALLOC_CAP_SPIRAM);
    s_small = jpeg_calloc_align(512 * 512 * 2, 16);
}

void library_start(void)
{
    ESP_LOGI(TAG, "cover pool %d x %d KB, internal RAM free %u KB", POOL, COVER_BYTES / 1024,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    /* 64 KB, not 512. Measured 6 Nov 2026: a page of 50 albums is 62 KB and is
     * now streamed (one album, under 3 KB, is all that has to fit), and every
     * reply still read whole is under 11 KB - the playlists, the recently
     * played, a single track. The other 448 KB sat empty all session while the
     * dial ran out of PSRAM and restarted. A reply that ever outgrows it says
     * so in the log ("response truncated") rather than failing quietly. */
    s_ma = ma_open(64 * 1024);
    /* A connection each for the images, and s_ma kept for the API alone. They
     * used to share one, which was harmless while every fetch closed it -
     * but now that a cover leaves its connection open, the next API call
     * reused a handle still part-way through a GET and came back empty.
     * Anthony Chandler's album has 82 tracks in Music Assistant and the knob
     * showed none of them (29 Oct 2026). Images go straight into s_img, so
     * these need no response buffer of their own. */
    for (int i = 0; i < LOADERS; i++) s_conn[i] = ma_open(0);
    /* stack in PSRAM: internal RAM is needed by Wi-Fi and the display, and these
     * tasks only do network and decoding work (no flash writes) */
    if (xTaskCreatePinnedToCoreWithCaps(loader_task, "library", 10240, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "could not start the loader");
    for (int i = 1; i < LOADERS; i++)
        if (xTaskCreatePinnedToCoreWithCaps(cover_loader, "covers", 10240, (void *)(intptr_t)i, 4, NULL, 0,
                                            MALLOC_CAP_SPIRAM) != pdPASS)
            ESP_LOGE(TAG, "could not start cover loader %d", i);
}

library_stats_t library_stats(void) { return s_stats; }
void library_list(int from, int n)
{
    for (int a = from; a < from + n && a < lib_count(); a++)
        ESP_LOGI(TAG, "LIST %4d %c  %s", a, s_albums[a].letter, s_albums[a].key);
}

void library_find(const char *text)
{
    for (int a = 0; a < lib_count(); a++)
        if (strstr(s_albums[a].key, text)) { library_list(a, 1); return; }
}

void library_pause(bool paused) { atomic_store(&s_paused, paused); wake_loaders(); }

void library_dump(void)
{
    char line[160];
    int n = 0;
    for (int b = 0; b < POOL; b++) n += snprintf(line + n, sizeof line - n, "%d ", s_pool[b].album);
    ESP_LOGI(TAG, "POOL albums: %s| focus %.2f speed %.2f landing %d, %d albums", line,
             atomic_load(&s_focus_x100) / 100.0f, lib_speed(), atomic_load(&s_landing),
             lib_count());
}
