/* Where the dial's amp and streamer are (22 Nov 2026).
 *
 * Both used to be fixed in the firmware from config/secrets.local, so someone
 * who built the dial with no streamer and no amp, and bought one later, had to
 * plug the dial back into a computer and rebuild it. Darren: there should be
 * nothing to rebuild for that.
 *
 * So the dial asks Home Assistant. Two text helpers, named from the room like
 * everything else:
 *
 *   input_text.<room>_dial_amp_address   e.g. http://192.168.1.50, or empty
 *   input_text.<room>_dial_player        the player's name in Music Assistant
 *                                        ("Bathroom Streamer"), or its id, or empty
 *
 * A helper that holds something wins. One that is empty, missing or can't be
 * read falls back to AMP_SERVER and MA_PLAYER from secrets.local, so a dial
 * built with them set (Darren's) needs no helpers at all.
 *
 * Read once at start-up, before anything uses them, and never changed while
 * the dial runs: everything that reads them can then hold the pointer without
 * a lock. A change in Home Assistant is noticed by where_check and answered
 * with a restart, which reads them again. */
#pragma once

#include <stdbool.h>

void where_load(void);   /* blocking: once, on the network task, before the speaker check */
void where_check(void);  /* blocking: now and then while awake; restarts the dial on a change */

const char *where_amp(void);     /* the amp's address, or "" for no amp */
const char *where_player(void);  /* the Music Assistant player id, or "" for none */

/* Nobody knows a player's id: Music Assistant shows names. So the helper may
 * hold the name as Music Assistant shows it, and where_load turns it into the
 * id, once, by asking for every player. A name that matches nothing is kept as
 * it is, and the start-up check then says "No streamer". */
