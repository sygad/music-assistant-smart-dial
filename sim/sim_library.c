/*
 * The album library for the simulator: a copy of Darren's Music Assistant
 * library (tools/sim_data.py), handed over by the page at start-up.
 *
 * Behaves like the firmware's library.c: the albums arrive all at once after
 * the UI is up, sorted the same way (core_album_key); a track list is "loaded"
 * a moment after the UI first asks for it; covers use the same pool of 12
 * buffers, the same window of +/-4 albums around Album browse, and load only
 * once browsing has settled, except around where a fling will land. The page
 * fetches and decodes each cover (the knob's loader task does that on core 0);
 * one at a time, as on the knob.
 *
 * Playlists (27 Oct 2026): their track lists "load" like an album's, and each
 * track's own picture is loaded around where Now Playing is (+/-2 tracks, in a
 * pool of 6), cropped to a square by the page.
 *
 * Recent (27 Oct 2026): the tracks recently played, fetched from Music
 * Assistant through the page's server each time Recent opens; what's played
 * on the simulator stays on top (Music Assistant never hears of it).
 */
#include <emscripten.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "sim.h"

#define SIDE 360
#define COVER_BYTES (SIDE * SIDE * 2)
#define POOL 12
#define WINDOW 4
#define MOVING 0.05f
/* How long a track list takes to "load". Music Assistant is usually quicker;
 * the demo uses 2 s so the loading bar can be seen (Darren, 27 Oct 2026). */
#define TRACKS_MS 2000

typedef struct {
    char *title, *artist, *id, *key;
    char letter;
    bool has_cover;
    int ntracks;
    char **track, **track_artist;
    uint16_t *seconds;
    ui_tracks_t t;
    bool tracks_loaded;
    uint32_t tracks_asked;  /* when the UI first asked, 0 if it hasn't */
} album_t;

static album_t *s_albums;
static int s_count, s_loading;  /* s_count: what the UI sees; s_loading: being handed over */

typedef struct {
    uint8_t px[COVER_BYTES];
    lv_image_dsc_t dsc;
    int album;  /* -1 when free */
} buf_t;

static buf_t s_pool[POOL];
static int *s_showing;  /* pool index shown for each album, -1 none */
static uint8_t *s_have, *s_pending;
static float s_focus, s_speed;
static int s_landing = -1;
static int s_loading_cover = -1;  /* the one cover being fetched */

EM_JS(void, js_load_cover, (int album, const char *id), { Module.loadCover(album, UTF8ToString(id)); });
EM_JS(void, js_load_picture, (int playlist, int track, const char *id), {
    Module.loadPicture(playlist, track, UTF8ToString(id));
});

/* ---- Playlists ------------------------------------------------------------------- */
#define PIC_POOL 6
#define PIC_WINDOW 2

typedef struct {
    char *name, *id;
    int ntracks;
    char **track, **track_artist, **track_id;
    uint16_t *seconds;
    bool *has_picture;
    ui_tracks_t t;
    bool tracks_loaded;
    uint32_t tracks_asked;
} playlist_t;

static playlist_t *s_pl;
static int s_npl, s_pl_loading;

typedef struct {
    uint8_t px[COVER_BYTES];
    lv_image_dsc_t dsc;
    int playlist, track;  /* playlist -1 when free */
} pic_t;

static pic_t s_pics[PIC_POOL];
static int s_pic_pl = -1;          /* the playlist Now Playing is on */
static float s_pic_focus;
static int s_pic_loading_pl = -1, s_pic_loading_track = -1;

static char *dup(const char *s) { return strdup(s ? s : ""); }

static int pic_find(int pl, int track)
{
    for (int b = 0; b < PIC_POOL; b++)
        if (s_pics[b].playlist == pl && s_pics[b].track == track) return b;
    return -1;
}

/* ---- Handed over by the page ------------------------------------------------- */
EMSCRIPTEN_KEEPALIVE void sim_lib_begin(int n)
{
    s_albums = calloc(n, sizeof(album_t));
    s_showing = malloc(n * sizeof(int));
    s_have = calloc(n, 1);
    s_pending = calloc(n, 1);
    for (int i = 0; i < n; i++) s_showing[i] = -1;
    s_loading = 0;
}

EMSCRIPTEN_KEEPALIVE void sim_lib_album(const char *name, const char *sort, const char *artist, const char *artist_sort,
                                        const char *id, int has_cover, int ntracks)
{
    album_t *a = &s_albums[s_loading++];
    a->title = dup(name);
    a->artist = artist && *artist ? dup(artist) : NULL;
    a->id = dup(id);
    a->has_cover = has_cover;
    char key[200];
    core_album_key(artist_sort && *artist_sort ? artist_sort : artist, sort && *sort ? sort : name,
                   core_is_compilation(artist, name), key, sizeof key);
    a->key = dup(key);
    a->letter = core_letter(key);
    a->track = calloc(ntracks ? ntracks : 1, sizeof(char *));
    a->track_artist = calloc(ntracks ? ntracks : 1, sizeof(char *));
    a->seconds = calloc(ntracks ? ntracks : 1, sizeof(uint16_t));
}

EMSCRIPTEN_KEEPALIVE void sim_lib_track(const char *name, int seconds, const char *artist)
{
    album_t *a = &s_albums[s_loading - 1];
    a->track[a->ntracks] = dup(name);
    a->track_artist[a->ntracks] = dup(artist);
    a->seconds[a->ntracks] = (uint16_t)seconds;
    a->ntracks++;
}

static int album_cmp(const void *x, const void *y)
{
    return strcmp(((const album_t *)x)->key, ((const album_t *)y)->key);
}

EMSCRIPTEN_KEEPALIVE void sim_lib_playlists(int n)
{
    s_pl = calloc(n ? n : 1, sizeof(playlist_t));
    s_pl_loading = 0;
    s_npl = 0;
    (void)n;
}

EMSCRIPTEN_KEEPALIVE void sim_lib_playlist(const char *name, const char *id, int ntracks)
{
    playlist_t *p = &s_pl[s_pl_loading++];
    p->name = dup(name);
    p->id = dup(id);
    int n = ntracks ? ntracks : 1;
    p->track = calloc(n, sizeof(char *));
    p->track_artist = calloc(n, sizeof(char *));
    p->track_id = calloc(n, sizeof(char *));
    p->seconds = calloc(n, sizeof(uint16_t));
    p->has_picture = calloc(n, sizeof(bool));
}

EMSCRIPTEN_KEEPALIVE void sim_lib_playlist_track(const char *name, int seconds, const char *artist, const char *id,
                                                 int has_picture)
{
    playlist_t *p = &s_pl[s_pl_loading - 1];
    int i = p->ntracks++;
    p->track[i] = dup(name);
    p->track_artist[i] = dup(artist);
    p->track_id[i] = dup(id);
    p->seconds[i] = (uint16_t)(seconds > 65535 ? 65535 : seconds);
    p->has_picture[i] = has_picture;
}

EMSCRIPTEN_KEEPALIVE void sim_lib_done(int radio)
{
    qsort(s_albums, s_loading, sizeof(album_t), album_cmp);
    for (int i = 0; i < s_loading; i++) {
        album_t *a = &s_albums[i];
        a->t = (ui_tracks_t){.count = a->ntracks, .title = (const char *const *)a->track, .seconds = a->seconds};
    }
    s_count = s_loading;
    for (int i = 0; i < s_pl_loading; i++) {
        playlist_t *p = &s_pl[i];
        p->t = (ui_tracks_t){.count = p->ntracks, .title = (const char *const *)p->track, .seconds = p->seconds};
    }
    s_npl = s_pl_loading;
    ui_library_changed();
    ui_menu_radio(radio);
    ui_playlists_changed();
}

/* ---- Lookups for voice search ------------------------------------------------- */
int sim_library_loaded(void) { return s_count; }
const char *sim_library_key(int a) { return s_albums[a].key; }
const char *sim_library_artist(int a) { return a >= 0 && a < s_count ? s_albums[a].artist : NULL; }
int sim_library_track_count(int a) { return s_albums[a].ntracks; }
const char *sim_library_track(int a, int t) { return s_albums[a].track[t]; }
const char *sim_library_track_artist(int a, int t) { return s_albums[a].track_artist[t]; }

/* ---- Covers ------------------------------------------------------------------------ */
static int focus_album(void) { return (int)lroundf(s_focus); }
static bool in_window(int album, int c) { return album >= c - WINDOW && album <= c + WINDOW; }
static bool near_landing(int album) { return s_landing >= 0 && album >= s_landing - 1 && album <= s_landing + 1; }

static bool needs_cover(int a)
{
    return a >= 0 && a < s_count && s_albums[a].has_cover && !s_have[a] && !s_pending[a];
}

/* What to load next: the landing album of a fling first, then (once settled)
 * the nearest album in the window, looking ahead in the direction of travel. */
static int next_cover(void)
{
    if (s_landing >= 0) {
        static const int around[] = {0, 1, -1};
        for (int i = 0; i < 3; i++)
            if (needs_cover(s_landing + around[i])) return s_landing + around[i];
    }
    if (fabsf(s_speed) > MOVING) return -1;
    int c = focus_album(), dir = s_speed < 0 ? -1 : 1;
    static const int order[] = {0, 1, -1, 2, -2, 3, -3, 4, -4};
    for (int i = 0; i < 9; i++)
        if (needs_cover(c + order[i] * dir)) return c + order[i] * dir;
    return -1;
}

static int free_buf(void)
{
    for (int b = 0; b < POOL; b++)
        if (s_pool[b].album < 0) return b;
    return -1;
}

static void load_next_cover(void)
{
    if (s_loading_cover >= 0 || free_buf() < 0) return;
    int a = next_cover();
    if (a < 0) return;
    s_pending[a] = 1;
    s_loading_cover = a;
    js_load_cover(a, s_albums[a].id);
}

/* The page decoded a cover: 360x360 RGBA, or NULL if it failed. */
EMSCRIPTEN_KEEPALIVE void sim_cover_ready(int album, const uint8_t *rgba)
{
    s_loading_cover = -1;
    s_pending[album] = 0;
    bool wanted = (in_window(album, focus_album()) || near_landing(album)) && s_showing[album] < 0;
    int b = free_buf();
    if (!rgba) s_have[album] = 1;  /* broken: don't retry in a loop; the placeholder stays */
    if (rgba && wanted && b >= 0) {
        uint8_t *px = s_pool[b].px;
        for (int i = 0; i < SIDE * SIDE; i++, rgba += 4) {
            uint16_t c = (uint16_t)((rgba[0] >> 3) << 11 | (rgba[1] >> 2) << 5 | rgba[2] >> 3);
            *px++ = (uint8_t)(c >> 8);  /* panel byte order, as the knob stores covers */
            *px++ = (uint8_t)c;
        }
        s_pool[b].album = album;
        s_showing[album] = b;
        s_have[album] = 1;
        ui_cover_changed(album);
    }
    load_next_cover();
}

static void drop_far_covers(void)
{
    int c = focus_album();
    for (int a = 0; a < s_count; a++) {
        if (s_showing[a] >= 0 && !in_window(a, c) && !near_landing(a)) {
            int b = s_showing[a];
            s_showing[a] = -1;
            s_have[a] = 0;
            ui_cover_changed(a);
            s_pool[b].album = -1;
        }
    }
}

/* ---- The UI's side ------------------------------------------------------------------ */
static int lib_count(void) { return s_count; }
static const char *lib_title(int a) { return s_albums[a].title; }
static char lib_letter(int a) { return s_albums[a].letter; }

static const lv_image_dsc_t *lib_cover(int a)
{
    int b = s_showing[a];
    return b < 0 ? NULL : &s_pool[b].dsc;
}

static const ui_tracks_t *lib_tracks(int a)
{
    if (UI_IS_PLAYLIST(a)) {
        playlist_t *p = &s_pl[a - UI_PLAYLIST_BASE];
        if (p->tracks_loaded) return &p->t;
        if (!p->tracks_asked) p->tracks_asked = lv_tick_get() | 1;
        return NULL;
    }
    album_t *al = &s_albums[a];
    if (al->tracks_loaded) return &al->t;
    if (!al->tracks_asked) al->tracks_asked = lv_tick_get() | 1;
    return NULL;
}

static void lib_focus(float pos, float speed)
{
    s_focus = pos;
    s_speed = speed;
    if (fabsf(speed) <= MOVING) s_landing = -1;
    drop_far_covers();
    load_next_cover();
}

static void lib_landing(int album)
{
    s_landing = album;
    load_next_cover();
}

/* ---- Recent ------------------------------------------------------------------------ */
#define MAX_RECENT 20
typedef struct {
    int coll;
    char *track;  /* the track last played in it, or NULL */
} recent_t;
static recent_t s_recent[MAX_RECENT], s_mine[MAX_RECENT];  /* s_mine: played here, newest first */
static int s_nrecent, s_nmine;

EM_JS(void, js_refresh_recent, (void), { if (Module.refreshRecent) Module.refreshRecent(); });

/* Put a track at the front of `list`: the same track already there moves up;
 * a full list drops its oldest. */
static void list_front(recent_t *list, int *count, int coll, const char *track)
{
    int at = 0;
    while (at < *count && !(list[at].coll == coll && list[at].track && track && !strcmp(list[at].track, track))) at++;
    if (at == *count) {
        if (*count < MAX_RECENT) (*count)++;
        else free(list[--at].track);
        list[at].track = NULL;
    }
    char *was = list[at].track;
    memmove(&list[1], &list[0], at * sizeof(recent_t));
    list[0].coll = coll;
    list[0].track = track ? dup(track) : NULL;
    free(was);
}

static void recent_front(int coll, const char *track) { list_front(s_recent, &s_nrecent, coll, track); }

/* A fresh list from Music Assistant is coming (oldest first, through
 * sim_lib_recent): start afresh; what was played here goes back on top in
 * sim_lib_recent_done. */
EMSCRIPTEN_KEEPALIVE void sim_lib_recent_reset(void)
{
    for (int i = 0; i < s_nrecent; i++) free(s_recent[i].track);
    s_nrecent = 0;
}

/* From the page, oldest first, so each goes to the front: an album or a
 * playlist by its Music Assistant id. */
EMSCRIPTEN_KEEPALIVE void sim_lib_recent(int is_playlist, const char *id, const char *track)
{
    int coll = -1;
    if (is_playlist) {
        for (int i = 0; i < s_pl_loading && coll < 0; i++)
            if (!strcmp(s_pl[i].id, id)) coll = UI_PLAYLIST(i);
    } else {
        for (int a = 0; a < s_loading && coll < 0; a++)
            if (!strcmp(s_albums[a].id, id)) coll = a;  /* after sorting: sim_lib_done comes first */
    }
    if (coll >= 0) recent_front(coll, track && *track ? track : NULL);
}

EMSCRIPTEN_KEEPALIVE void sim_lib_recent_done(void)
{
    for (int i = s_nmine - 1; i >= 0; i--) recent_front(s_mine[i].coll, s_mine[i].track);  /* played here: on top */
    ui_recent_changed();
}

static const char *lib_artist(int a) { return a >= 0 && a < s_count ? s_albums[a].artist : NULL; }
static int lib_recent(void) { return s_nrecent; }
static int lib_recent_item(int i, const char **track)
{
    *track = s_recent[i].track;
    return s_recent[i].coll;
}
static void lib_played(int coll, const char *track)
{
    list_front(s_mine, &s_nmine, coll, track);
    recent_front(coll, track);
    ui_recent_changed();
}

static void lib_recent_refresh(void) { js_refresh_recent(); }

static int lib_playlists(void) { return s_npl; }
static const char *lib_playlist_name(int p) { return s_pl[p].name; }
static int lib_playlist_size(int p) { return s_pl[p].ntracks; }

static const lv_image_dsc_t *lib_track_cover(int pl, int track)
{
    int b = pic_find(pl, track);
    return b < 0 ? NULL : &s_pics[b].dsc;
}

static bool pic_wanted(int pl, int track)
{
    return pl == s_pic_pl && track >= (int)lroundf(s_pic_focus) - PIC_WINDOW &&
           track <= (int)lroundf(s_pic_focus) + PIC_WINDOW;
}

/* The next picture to load: the track on screen, then its neighbours. */
static void load_next_picture(void)
{
    if (s_pic_loading_pl >= 0 || s_pic_pl < 0) return;
    playlist_t *p = &s_pl[s_pic_pl - UI_PLAYLIST_BASE];
    int c = (int)lroundf(s_pic_focus);
    static const int order[] = {0, 1, -1, 2, -2};
    for (int i = 0; i < 5; i++) {
        int t = c + order[i];
        if (t < 0 || t >= p->ntracks || !p->has_picture[t] || pic_find(s_pic_pl, t) >= 0) continue;
        int b = -1;
        for (int k = 0; k < PIC_POOL && b < 0; k++)
            if (s_pics[k].playlist < 0) b = k;
        if (b < 0) return;
        s_pic_loading_pl = s_pic_pl;
        s_pic_loading_track = t;
        js_load_picture(s_pic_pl, t, p->track_id[t]);
        return;
    }
}

/* The page cropped and decoded a picture: 360x360 RGBA, or NULL. */
EMSCRIPTEN_KEEPALIVE void sim_picture_ready(int pl, int track, const uint8_t *rgba)
{
    s_pic_loading_pl = s_pic_loading_track = -1;
    if (!rgba) s_pl[pl - UI_PLAYLIST_BASE].has_picture[track] = false;  /* broken: keep the placeholder */
    int b = -1;
    for (int k = 0; k < PIC_POOL && b < 0; k++)
        if (s_pics[k].playlist < 0) b = k;
    if (rgba && b >= 0 && pic_wanted(pl, track) && pic_find(pl, track) < 0) {
        uint8_t *px = s_pics[b].px;
        for (int i = 0; i < SIDE * SIDE; i++, rgba += 4) {
            uint16_t c = (uint16_t)((rgba[0] >> 3) << 11 | (rgba[1] >> 2) << 5 | rgba[2] >> 3);
            *px++ = (uint8_t)(c >> 8);
            *px++ = (uint8_t)c;
        }
        s_pics[b].playlist = pl;
        s_pics[b].track = track;
        ui_track_cover_changed(pl, track);
    }
    load_next_picture();
}

static void lib_track_focus(int pl, float track)
{
    s_pic_pl = pl;
    s_pic_focus = track;
    for (int b = 0; b < PIC_POOL; b++)
        if (s_pics[b].playlist >= 0 && !pic_wanted(s_pics[b].playlist, s_pics[b].track)) {
            int p = s_pics[b].playlist, t = s_pics[b].track;
            s_pics[b].playlist = -1;
            ui_track_cover_changed(p, t);
        }
    load_next_picture();
}

static const ui_library_t s_lib = {
    .count = lib_count, .title = lib_title, .letter = lib_letter, .cover = lib_cover,
    .tracks = lib_tracks, .focus = lib_focus, .landing = lib_landing,
    .playlists = lib_playlists, .playlist_name = lib_playlist_name, .playlist_size = lib_playlist_size,
    .track_cover = lib_track_cover, .track_focus = lib_track_focus,
    .artist = lib_artist, .recent = lib_recent, .recent_item = lib_recent_item, .played = lib_played,
    .recent_refresh = lib_recent_refresh,
};

const ui_library_t *sim_library(void)
{
    for (int b = 0; b < PIC_POOL; b++) {
        s_pics[b].playlist = -1;
        s_pics[b].dsc = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
                       .w = SIDE, .h = SIDE, .stride = SIDE * 2},
            .data_size = COVER_BYTES,
            .data = s_pics[b].px,
        };
    }
    for (int b = 0; b < POOL; b++) {
        s_pool[b].album = -1;
        s_pool[b].dsc = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
                       .w = SIDE, .h = SIDE, .stride = SIDE * 2},
            .data_size = COVER_BYTES,
            .data = s_pool[b].px,
        };
    }
    return &s_lib;
}

void sim_library_poll(void)
{
    for (int a = 0; a < s_count; a++) {
        album_t *al = &s_albums[a];
        if (al->tracks_asked && !al->tracks_loaded && lv_tick_elaps(al->tracks_asked) >= TRACKS_MS) {
            al->tracks_loaded = true;
            ui_tracks_changed(a);
        }
    }
    for (int i = 0; i < s_npl; i++) {
        playlist_t *p = &s_pl[i];
        if (p->tracks_asked && !p->tracks_loaded && lv_tick_elaps(p->tracks_asked) >= TRACKS_MS) {
            p->tracks_loaded = true;
            ui_tracks_changed(UI_PLAYLIST(i));
        }
    }
    load_next_cover();
    load_next_picture();
}
