/* Updating the knob over Wi-Fi (2 Nov 2026).
 *
 * The knob is mounted on the bathroom wall, so a cable is no longer an answer.
 * Home Assistant says where the new firmware is and when to fetch it; this
 * fetches it, writes it to the slot that isn't running, and restarts into it.
 *
 * Nothing about where the firmware lives is decided here. The URL is read from
 * a Home Assistant helper at the moment of the update, so the file can move -
 * off the Mac, into Home Assistant's own www folder, anywhere - without the
 * knob ever needing a new firmware to be told about it. That is the whole
 * point: this must be the last mechanism that ever needs a USB cable. */
#pragma once

#include <stdbool.h>

/* Where an update has got to. The UI shows it; ha.c reports it to Home
 * Assistant so Darren can watch from his phone rather than the bathroom. */
typedef enum {
    OTA_IDLE,
    OTA_CONNECTING,  /* asking for the file */
    OTA_DOWNLOADING, /* writing it to the spare slot */
    OTA_INSTALLING,  /* written and verified; about to restart */
    OTA_FAILED,
} ota_state_t;

/* Why it failed, for the one line the screen and Home Assistant show. */
typedef enum {
    OTA_FAIL_NONE,
    OTA_FAIL_NO_URL,   /* the helper was empty */
    OTA_FAIL_HTTP,     /* could not fetch it, or the server said no */
    OTA_FAIL_NOT_KNOB, /* a file that is not this knob's firmware */
    OTA_FAIL_SHORT,    /* the download stopped early */
    OTA_FAIL_FLASH,    /* the write itself failed */
    /* The three below were all OTA_FAIL_FLASH until 4 Nov 2026, when an update
     * to a dial on a wall failed with "writing it failed" and there was no
     * cable to read a log with. One word for five different faults meant the
     * only channel back - the Home Assistant sensor - could not say which, so
     * they are told apart here. */
    OTA_FAIL_NO_RAM,   /* no internal memory for the download buffer */
    OTA_FAIL_NO_TASK,  /* the update could not be started at all (6 Nov 2026) */
    OTA_FAIL_WEDGED,   /* an earlier attempt never finished and never let go */
    OTA_FAIL_BEGIN,    /* the spare slot would not open (erase failed) */
    OTA_FAIL_BOOT,     /* written and verified, but the bootloader would not be pointed at it */
} ota_fail_t;

/* Called early in app_main, before Wi-Fi. If the running slot has never proved
 * itself, this starts the clock that puts the old firmware back: see ota.c. */
void ota_boot_check(void);

/* Wi-Fi is up and Home Assistant answered, which is the only test that
 * matters - a build that can do this can be updated again. Marks the running
 * slot good and stops the rollback clock. Safe to call more than once. */
void ota_validate(void);

/* Fetch and install the firmware at `url` (http://...). Returns at once; the
 * work happens on its own task. Ignored while an update is already running. */
void ota_begin(const char *url);

/* An update is in flight: the knob must not sleep, and nothing else should be
 * talking to the network. */
bool ota_busy(void);

/* For the screen and for Home Assistant. `percent` is 0-100 while
 * downloading and -1 otherwise. */
ota_state_t ota_state(int *percent, ota_fail_t *why);

/* The one line that says what is happening, for logs and Home Assistant. */
const char *ota_fail_text(ota_fail_t why);

/* How many updates have been asked for since this firmware booted. Reported
 * alongside a failure so that two identical failures in a row are two
 * different things to Home Assistant, which records nothing when a state is
 * re-posted unchanged (6 Nov 2026). */
unsigned ota_attempt(void);
