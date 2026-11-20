#pragma once

#include <stdbool.h>

/* Home Assistant, over its REST API with the long-lived token.
 *
 * The amp lives in Home Assistant, not in the knob: the power, the volume and
 * the three tone controls are all helpers Darren's house already drives the
 * Denon with, so the knob sets the helper and his own automations do the rest.
 *
 * Everything here is queued and sent from ha.c's own task, so nothing blocks
 * the LVGL thread. */

/* Before anything else, at boot: lets the knob be used while Wi-Fi is still
 * coming up and have the amps asked for all the same. */
void ha_hold_asks(void);

void ha_start(void);  /* once Wi-Fi is up */

/* input_boolean.bathroom_music: on powers the Denon and the bathroom sub and
 * sets the starting volume, off fades the music down and cuts them, both
 * through Darren's existing "Music Fade Off" automation. Only the latest ask
 * is kept, so a run of them costs one request. */
void ha_music_power(bool on);

/* Whether the last "on" reached Home Assistant. False while one is still on
 * its way out, and false if it could not be sent at all - so a caller that
 * repeats the ask on a timer knows not to start the clock on an ask that went
 * nowhere. */
bool ha_music_power_landed(void);

/* An input_number helper: the volume (0-98, Denon MV) and the three tone
 * controls. One pending value per helper, so turning the knob fast sends where
 * it has got to rather than every step on the way. */
typedef enum { HA_VOLUME, HA_TREBLE, HA_BASS, HA_SUBWOOFER, HA_NUMBERS } ha_number_t;
void ha_set_number(ha_number_t which, int value);

/* Read one back, blocking, for the task that asks. Returns false if Home
 * Assistant didn't answer. Used once at start-up to put the knob's EQ where
 * the amp already is. */
bool ha_read_number(ha_number_t which, int *value);

/* ---- Following the volume (2 Nov 2026) ----------------------------------------
 * The volume helper is read back as well as written, because the knob is not
 * the only thing that moves it: the bathroom dashboard has a slider, the Denon
 * has a remote, and Darren's own automations set the starting volume at
 * switch-on. Without the read the knob opened at its assumed starting volume
 * over an amp that was somewhere else entirely, and the dashboard slider moved
 * the amp while the knob's arc stayed put.
 *
 * Only the volume. The tone controls are read once at start-up and have no
 * second control to drift away from. */

/* The latest reading the knob did not ask for, in Denon MV. False when there
 * is nothing new. Call it from the thread that owns the screen. */
bool ha_volume_moved(int *denon_mv);

/* Stop reading while the screen is off: there is nothing to keep in step with
 * a display nobody can see, and the low-power state is meant to be quiet. */
void ha_volume_follow(bool on);

/* ---- Updating over Wi-Fi (2 Nov 2026) -----------------------------------------
 * The knob is on the bathroom wall and the USB cable is not coming back, so
 * Home Assistant is how it is told to update itself. Two helpers:
 *
 *   input_boolean.bathroom_dial_update        on: fetch and install
 *   input_text.bathroom_dial_firmware_url     where the firmware is
 *
 * ha.c polls them - every second while the screen is on and every ten seconds
 * while it is asleep, because an update must not have to wait for somebody to
 * walk into the bathroom and wake it. Turning the switch on is the whole
 * gesture; the knob turns it back off itself before it starts, so a reboot
 * into the new firmware cannot be read as another request.
 *
 * The URL is read at the moment of the update and never stored in the
 * firmware, so the firmware can be moved anywhere - the Mac, Home Assistant's
 * own www folder - without needing a new firmware to say where it went.
 *
 * It also reports back, so none of this needs a trip upstairs to watch:
 *
 *   sensor.bathroom_dial_firmware   the version now running
 *   sensor.bathroom_dial_update     idle / fetching / 42% / installing / failed: why
 */
void ha_report_firmware(void);  /* blocking: from a task of your own, once Wi-Fi is up */
/* sensor.bathroom_dial_last_restart: why the dial restarted, and a crash's
 * message, task and backtrace if it fell over. Blocking, as above. */
void ha_report_restart(void);

/* The battery, from the thread that reads the board. Stored and reported by
 * ha.c's own task, so this returns at once and can be called from the LVGL
 * thread. Reported as sensor.bathroom_dial_battery, so how long a charge
 * actually lasts on the wall becomes a graph rather than a guess (3 Nov 2026,
 * after it went flat in a day). */
/* The charge, and the resting rail in millivolts it was worked out from:
 * nine times the resolution of the percentage and with no scaling at all,
 * so a real discharge curve can be drawn from a run (6 Nov 2026). */
void ha_battery(int percent, int mv, bool charging);

/* True once, when something other than the dial has turned the bathroom music
 * off - the twenty-minute empty-room rule, or the dashboard. The dial follows
 * it rather than finding out by watching the amp disappear. */
bool ha_music_turned_off(void);
