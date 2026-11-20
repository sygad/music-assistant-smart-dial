#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

typedef struct {
    uint32_t fulls, failures, abandoned;
    uint32_t fetch_ms, decode_ms;  /* totals */
    uint32_t idle_nothing, idle_nobuf;  /* a loader found no album to load, or no buffer to load it into */
} library_stats_t;

void library_init(void);               /* allocate the cover pool (before Wi-Fi) */
void library_start(void);              /* load the library and covers; call once Wi-Fi is up */
const ui_library_t *library_get(void);
void library_poll(void);               /* LVGL thread: apply finished loads */
library_stats_t library_stats(void);
void library_dump(void);
void library_list(int from, int n);    /* log albums in browse order */
void library_find(const char *text);   /* log the first album whose sort key contains text */
void library_pause(bool paused);       /* test: stop or resume loading covers */

/* Voice search: albums loaded, an album's sort key ("<artist>\x01<album>",
 * folded), the album with a Music Assistant id, an album's artist as shown.
 * Safe from any task once loaded. */
int library_loaded(void);
const char *library_key(int album);
int library_album_by_id(const char *item_id);
const char *library_artist(int album);

/* Playlists are searched alongside the albums: Darren asked for "boiler room"
 * and got nothing, because it is a playlist and only albums were ranked
 * (29 Oct 2026). Same key shape, with the name where an album's artist goes. */
int library_playlists(void);
const char *library_playlist_key(int p);
const char *library_playlist_name(int p);

/* ...and the tracks inside them, searched as well: the one Darren asked for
 * ("¥ØU$UK€ ¥UK1MAT$U") is a set on the Boiler Room playlist, not a playlist or
 * a library track, so nothing had ever looked at it (29 Oct 2026). */
int library_playlist_track_count(int p);
const char *library_playlist_track(int p, int track);

/* ---- What to play (31 Oct 2026) --------------------------------------------------
 * Music Assistant plays by uri. `coll` is an album or UI_PLAYLIST(p).
 * library_collection_uri writes the collection's own uri (false if it isn't
 * known yet); library_track_uri gives one track's, or NULL if that track list
 * isn't loaded. Both read the track cache, so call them on the LVGL thread. */
bool library_collection_uri(int coll, char *buf, size_t n);
const char *library_track_uri(int coll, int track);
