#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Making a sound: Music Assistant's own API, on its own task.
 *
 * The knob plays through the Bathroom Streamer, whose queue it owns. A
 * collection (an album or a playlist) is handed to Music Assistant whole, with
 * the track to start on, so the rest of it follows without the knob having to
 * ask for each track; what the player then does with it comes back through
 * ui_player_state.
 *
 * Every call here is made from the LVGL thread and returns at once: the uris
 * are resolved by the caller (library.c owns the track cache) and the work is
 * done on the playback task. */

void playback_start(void);  /* once Wi-Fi is up */

/* Play `track_uri` within `coll_uri`, from the start. A different track of
 * what is already playing, or the same one again from a pause, is taken as it
 * comes: the first is a change of track, the second is a resume. */
void playback_play(const char *coll_uri, const char *track_uri);
void playback_pause(void);
/* `after_ms` 0 stops now; Music off waits for Darren's amp fade to finish
 * first, so the music goes out with the amp. A play cancels a waiting stop. */
void playback_stop_after(int after_ms);
void playback_seek(int seconds);

/* Called on the LVGL thread, often: pushes anything the player has said since
 * the last call into ui_player_state. */
void playback_poll(void);
