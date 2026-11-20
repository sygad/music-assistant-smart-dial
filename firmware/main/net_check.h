#pragma once

#include <stdbool.h>

/* Wi-Fi, and whether Music Assistant and Home Assistant answer (net_check.c).
 *
 * There was no header here while the only thing anyone needed was "start it":
 * app_main declared the three functions itself. The screen now says whether
 * the dial is on the network (4 Nov 2026, ui_net.c) and that needs a shared
 * type, so the declarations moved somewhere both files can read them. */

/* Named to match ui_net_t one for one, so app_main's net_apply() is a
 * translation and nothing more - the same arrangement ota.c and amp.c use.
 * In the order net_task really goes through them: an address first, then the
 * two services, each named on the screen as it is asked (4 Nov 2026 - one
 * word saying "Connecting" told Darren nothing about what to). */
typedef enum {
    NET_WIFI,
    NET_MUSIC_ASSISTANT,
    NET_HOME_ASSISTANT,
    NET_SPEAKER,
    NET_READY,
    NET_FAILED
} net_state_t;

/* The speaker is the fourth stage, and it is the one that matters.
 *
 * 16 Nov 2026: the Bathroom Streamer dropped off the network at 11:13 and the
 * dial went on saying "Connected" all afternoon, because all it had ever
 * checked was Wi-Fi, Music Assistant and Home Assistant - all three of which
 * were perfectly fine. Music Assistant took each track, could not hand it to a
 * speaker that was not there, and advanced the queue: a cover, a moment of
 * buffering, then the next track, over and over, in silence.
 *
 * So the dial asks Music Assistant whether its own player is available before
 * it claims to be connected. One POST to the API it already talks to, reading
 * one field - players/get -> "available" - which was false while the streamer
 * was down and true once it came back. */
void net_check_start(void (*on_connected)(void));
net_state_t net_state(void);
void net_say_again(void);  /* console `net`: run the two lines again, on the bench */
void net_scan_log(void);
void net_allow_roam(bool may);

/* What was reachable the last time each one was asked, for the Connections
 * screen (ui_status.c). Not kept up to date behind the music: the screen asks
 * for a fresh look as it opens, the way Settings asks for the battery, because
 * four requests a minute all day is a cost paid for nothing most of the time.
 * Each is -1 until it has been asked, 0 for no and 1 for yes. */
typedef struct {
    int wifi, music_assistant, home_assistant, speaker;
    bool checking;  /* a look is in flight: the screen says so rather than lying */
} net_status_t;
net_status_t net_status(void);
void net_status_refresh(void);  /* ask all four again, on a task of its own */
