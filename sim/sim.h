/* The simulator's stand-ins for the firmware's library (library.c) and voice
 * search (voice.c). */
#pragma once

#include "ui.h"

const ui_library_t *sim_library(void);
void sim_library_poll(void);  /* LVGL thread: deliver track lists that have "loaded" */

/* For voice search: albums loaded, an album's browse key and artist, its tracks
 * with their artists. */
int sim_library_loaded(void);
const char *sim_library_key(int album);
const char *sim_library_artist(int album);
int sim_library_track_count(int album);
const char *sim_library_track(int album, int track);
const char *sim_library_track_artist(int album, int track);

const ui_voice_t *sim_voice(void);
