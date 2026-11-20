/*
 * Bathroom Music Knob: screens.
 *
 * Shared by the firmware and the Mac simulator. All LVGL calls must come from
 * the thread that runs lv_timer_handler().
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* A table that lives in PSRAM on the dial (6 Nov 2026, docs/MEMORY.md).
 * Internal RAM is the dial's scarce memory - Wi-Fi, sockets and the display's
 * DMA buffers must have it and nothing else can stand in - so a static table
 * that only the screens read goes in PSRAM instead. Only for zero-initialised
 * statics that nothing touches from an interrupt or while flash is being
 * written. In the simulator it means nothing. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define UI_IN_PSRAM EXT_RAM_BSS_ATTR
#else
#define UI_IN_PSRAM
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* An album's tracks, once loaded. */
typedef struct {
    int count;
    const char *const *title;
    const uint16_t *seconds;
} ui_tracks_t;

/* Playlists (27 Oct 2026) are shown and played like albums. Anything that
 * takes an album also takes a playlist as UI_PLAYLIST(p): its tracks, and what
 * Now Playing and the player hold. */
#define UI_PLAYLIST_BASE 0x100000
#define UI_PLAYLIST(p) (UI_PLAYLIST_BASE + (p))
#define UI_IS_PLAYLIST(c) ((c) >= UI_PLAYLIST_BASE)

/* The album library, supplied by the firmware (or the simulator). The UI asks
 * for covers and track lists as it needs them and says where the user is, so
 * the supplier can load around that position. All calls happen on the LVGL
 * thread. Albums are in browse order (A-Z by album title). */
typedef struct {
    int (*count)(void);                       /* grows while the library loads */
    const char *(*title)(int album);
    char (*letter)(int album);                /* 'A'-'Z' or '#' */
    const lv_image_dsc_t *(*cover)(int album); /* 360x360, or NULL if not loaded yet */
    const ui_tracks_t *(*tracks)(int album);  /* NULL until loaded; asking starts the load */
    /* Where Album browse is: position in albums (fractional while moving) and
     * speed in albums per second (0 when settled). */
    void (*focus)(float position, float speed);
    void (*landing)(int album);               /* where a fling is going to stop */

    /* Playlists, in Music Assistant's order. May be NULL (none). tracks()
     * takes UI_PLAYLIST(p). Each track has its own picture (a playlist's
     * tracks come from all over), asked for by track_cover and loaded around
     * where Now Playing is (track_focus). */
    int (*playlists)(void);                   /* grows when they've loaded */
    const char *(*playlist_name)(int p);
    int (*playlist_size)(int p);              /* its number of tracks */
    const lv_image_dsc_t *(*track_cover)(int playlist, int track);  /* 360x360, or NULL */
    void (*track_focus)(int playlist, float track);

    /* Recently played tracks (27 Oct 2026), newest first: each with the album or
     * playlist (UI_PLAYLIST(p)) it lives in. played() says the knob started
     * playing a track, so it goes to the top. May be NULL. */
    const char *(*artist)(int album);          /* as shown; NULL if none */
    int (*recent)(void);
    int (*recent_item)(int i, const char **track);
    void (*played)(int coll, const char *track);
    /* Recent was opened: fetch the latest from Music Assistant (Darren, 27 Oct
     * 2026: plays from anywhere should show), then ui_recent_changed(). */
    void (*recent_refresh)(void);
} ui_library_t;

typedef enum {
    UI_VIEW_MENU, UI_VIEW_ALBUMS, UI_VIEW_NOWPLAYING, UI_VIEW_EQ, UI_VIEW_SEARCH, UI_VIEW_PLAYLISTS, UI_VIEW_RECENT,
    UI_VIEW_SETTINGS, UI_VIEW_STATUS
} ui_view_t;

/* Build every screen. `lib` must stay valid for the life of the UI. */
void ui_init(const ui_library_t *lib);

/* Calls from the library, on the LVGL thread. */
void ui_cover_changed(int album);   /* a cover loaded or was dropped */
void ui_tracks_changed(int album);  /* an album's track list loaded */
void ui_library_changed(void);      /* more albums loaded */
void ui_menu_radio(bool available);  /* whether Music Assistant has radio stations */

/* ---- Amp power (31 Oct 2026) ---------------------------------------------------
 * The bathroom's music toggle is gone from the dashboard: the amps are turned
 * on by using the knob and off by leaving it alone (Darren). So the knob is
 * the only thing that powers them, and by the time anyone has browsed to a
 * track and tapped play, the amp and the sub are already up and waiting.
 *
 * `on` is called on every touch and every detent that does something - not the
 * tap or turn that only wakes the clock, which is someone glancing at the time.
 * It is called often, so the supplier decides how often that is worth sending.
 * `off` comes from Music off and from the twenty-minute sleep, the only two
 * ways the knob ever says the music is finished with.
 *
 * May be left unset (the simulator does: it has no business switching Darren's
 * amps on). */
typedef struct {
    void (*on)(void);
    void (*off)(void);
} ui_power_t;
void ui_power_init(const ui_power_t *power);

/* ---- The player (31 Oct 2026) --------------------------------------------------
 * Everything the knob does to make a sound. The supplier plays it; the screens
 * only say what was asked for. All calls are on the LVGL thread, and all of
 * them must return at once - the supplier queues the work.
 *
 * `coll` is an album, or UI_PLAYLIST(p); `track` is its index in that
 * collection's track list. Picking a track from Recent is passed on as the
 * album or playlist that track lives in, so the player never has to know what
 * Recent is.
 *
 * play() is also resume: the same collection and track again, with the music
 * paused where it left off, carries on rather than starting over. Nothing is
 * sent while a finger is still moving through tracks - only the one it settles
 * on, CORE_TRACK_MARK_HOLD_MS after it lands (PLANNING.md "Changing track or
 * album while playing").
 *
 * May be left unset (the simulator does: it plays nothing). */
typedef struct {
    void (*play)(int coll, int track);
    void (*pause)(void);
    /* Stop, `after_ms` from now: Music off lets Darren's fade run first, so
     * the music goes out with the amp rather than being cut dead. */
    void (*stop)(int after_ms);
    void (*seek)(int seconds);   /* from the start of the track */
    void (*volume)(int percent); /* 0-100 as the knob counts it */
    void (*eq)(int param, int db);  /* 0 treble, 1 bass, 2 subwoofer */
} ui_player_t;
void ui_player_init(const ui_player_t *player);

/* What the player is really doing, from the supplier. `playing` false with
 * `advanced` 0 is a pause; `advanced` is how many tracks on from the one the
 * knob asked for the player has got (its own move to the next track at the end
 * of one), and `elapsed_ms` is where it is in that track. Called as often as
 * the supplier likes; the screens keep their own clock between calls so the
 * progress bar stays smooth. */
void ui_player_state(bool playing, int advanced, uint32_t elapsed_ms);

/* ---- Low power (30 Oct 2026) ---------------------------------------------------
 * The clock comes up a minute after the last touch, and only with nothing
 * playing: music is never interrupted by it (Darren, 4 Nov 2026). This
 * countdown then runs behind the clock. CORE_SLEEP_MS after that last touch or
 * detent, which means with nothing playing by definition, the UI says "asleep":
 * the
 * supplier turns the screen off and takes everything it can down with it. The
 * first touch or detent after that says "awake" and is swallowed, exactly as a
 * tap on the clock is: it brings the screen back on the view that was up and
 * changes nothing.
 *
 * Music playing holds the screen on Now Playing, full stop: no clock, no dark
 * screen, nothing. Music *starting* from anywhere wakes it, because the clock
 * gives way to music beginning; music merely continuing does not wake a screen
 * that is already off, which is a different thing. The
 * amps are only let go on the way down if nothing is playing. May be left
 * unset (the simulator does). */
void ui_sleep_init(void (*set_asleep)(bool asleep));
/* ...and CORE_DEEP_SLEEP_MS after that same last touch, with nothing playing,
 * the UI asks the supplier to stop the dial properly: deep sleep, from which
 * only a turn of the knob (or the check-in timer) brings it back, and that by
 * restarting. The supplier never returns from this. */
void ui_deep_sleep_init(void (*deep_sleep)(void));
bool ui_asleep(void);
/* Settings (28 Oct 2026): the knob's own details, shown on Menu -> Settings.
 * The firmware says who it is (each string copied; "--" until then), and the
 * charge left (-1: unknown), with whether it's on charge.
 *
 * Read on the way in, not kept up to date: the screen is looked at now and
 * then, and the ADC read and the address lookups have no business running
 * while music is being browsed (Darren, 28 Oct 2026). */
void ui_device_source(void (*read)(void));  /* called as Settings opens */
void ui_device_init(const char *mac, const char *ip, const char *firmware);
void ui_device_battery(int percent, bool charging);

/* ---- Connections, the second Settings screen (ui_status.c) -------------------
 * The opening sequence's list, standing still and on demand: Wi-Fi, Home
 * Assistant, Music Assistant, the speaker, and the amplifier. Asked for 16 Nov
 * 2026, after the streamer spent an afternoon off the network and the only
 * screen that would have said so had gone four seconds after switch-on.
 *
 * Read as the screen opens, not kept warm behind the music: `read` is run on
 * entry and lands through ui_status_set whenever the answers arrive. Each of
 * the four is -1 for not yet asked, 0 for away and 1 for there. */
void ui_status_source(void (*read)(void));
void ui_status_set(int wifi, int home_assistant, int music_assistant, int speaker, bool checking);

void ui_playlists_changed(void);     /* the playlists loaded */
void ui_recent_changed(void);        /* the recently played list loaded or changed */
void ui_track_cover_changed(int playlist, int track);  /* a playlist track's picture loaded or was dropped */

/* ---- Voice search (experiment, 26 Oct 2026) ------------------------------------
 * The firmware listens and searches; the UI shows it. The UI calls these: */
typedef struct {
    void (*listen)(void);  /* start listening */
    void (*stop)(void);    /* stop listening now and search what was heard */
    void (*cancel)(void);  /* stop and forget it */
} ui_voice_t;
void ui_voice_init(const ui_voice_t *voice);

/* ...and the firmware answers with these, on the LVGL thread. */
typedef struct {
    int album;             /* in browse order */
    char title[96];        /* album or track title */
    char detail[96];       /* an album's artist ("Prince"), or "Track on Purple Rain" */
    char track[96];        /* a track's title, to land on it; "" for an album */
} ui_found_t;
void ui_voice_level(float level);  /* 0..1 while listening */
void ui_voice_searching(void);     /* stopped listening, working on it */
void ui_voice_typed(void);         /* test: typed words are being searched (opens Search, no listening) */
/* What was heard and what it found. heard NULL: nothing was heard;
 * n 0: nothing found; error set: it went wrong (shown as is). */
void ui_voice_results(const char *heard, const ui_found_t *found, int n, const char *error);
/* The most results a search can show: albums and playlists, tracks from Music
 * Assistant, tracks inside the playlists, then the ones missing a word. The
 * search screen and the searcher both size themselves from this. */
#define UI_MAX_FOUND 26

/* Fling tuning: the album glide's time constant (how long it keeps going), the
 * finger speed (px/ms) above which a release flings instead of paging, and the
 * speed above which it moves through letters instead. */
void ui_set_fling(uint32_t tau_ms, float min_speed, float letter_speed);

/* Tuning: how strongly the search results hold at each item while dragged
 * (0 follows the finger; the menu's is CORE_MENU_DETENT). */
void ui_search_detent(float k);
void ui_get_fling(uint32_t *tau_ms, float *min_speed, float *letter_speed);

/* Knob detents since the last call: positive = clockwise. */
void ui_knob(int steps);

/* ---- Updating over Wi-Fi (2 Nov 2026) -----------------------------------------
 * The firmware says how an update is going and this shows it, over everything,
 * on the whole screen. While it is up the knob and the glass do nothing and
 * the knob will not go to sleep: nothing screwed to a wall should go dark
 * halfway through being written to. The simulator never calls it.
 *
 * `percent` is 0-100 while downloading and -1 otherwise; `why` is the one line
 * to show under "Update failed" and is ignored otherwise. UI_UPDATE_NONE takes
 * it away. LVGL thread, as everything here. */
typedef enum {
    UI_UPDATE_NONE,
    UI_UPDATE_CONNECTING,
    UI_UPDATE_DOWNLOADING,
    UI_UPDATE_INSTALLING,
    UI_UPDATE_FAILED,
} ui_update_t;
void ui_update_show(ui_update_t state, int percent, const char *why);
bool ui_updating(void);   /* an update is RUNNING - not one that failed and is still on screen */
bool ui_update_failed(void);

/* ---- Whether the amp can hear (2 Nov 2026) -------------------------------------
 * The Denon serves no HTTP for about two minutes after its mains comes on, and
 * since Music off powers it down, every Music on opens a fresh two-minute hole
 * in which the volume cannot be set at all. Nothing on screen said so: the
 * dial browsed, the arc moved, and the room ignored it. Darren asked for the
 * screen to say it is connecting, and to say when it has connected, so a turn
 * means something from that moment on.
 *
 * The firmware finds this out by asking the amp itself (firmware/main/amp.h);
 * this is only where it says so. While connecting, the volume label says that
 * instead of a number - the arc still moves, because the turn is not lost, it
 * is held and sent the moment the amp answers. Call it often: the label is
 * kept alive while the wait lasts. The simulator never calls it. */
typedef enum { UI_AMP_UNKNOWN, UI_AMP_CONNECTING, UI_AMP_READY } ui_amp_t;
void ui_amp_state(ui_amp_t state, int waiting_secs);

/* ---- Whether the dial is on the network (ui_net.c) ---------------------------
 * Waking from deep sleep is a boot: the radio is off, nothing is associated,
 * and for the first few seconds a turn of the knob reaches nothing at all.
 * Darren woke the dial, looked at the menu and had no way of telling whether
 * it was back on the network or not (4 Nov 2026), so the screen now says.
 *
 * A screen of its own, laid over everything the way the clock is, naming the
 * thing it is waiting for - Wi-Fi, then Music Assistant, then Home Assistant -
 * with the rim spinning underneath to say that something is going on. Then
 * "Connected", held long enough to read and gone. "Connecting" on its own
 * answered nothing: Darren asked what it was connecting to, and what it was
 * connected to when it said so (4 Nov 2026).
 *
 * It takes no input and blocks nothing: the knob and the glass go on reaching
 * the screen underneath the whole time, and it never stays.
 *
 * The amplifier is not one of these stages. That wait runs to two minutes
 * while the amp powers up, so it keeps the small line on Now Playing instead
 * (ui_amp_state above).
 *
 * Call it often, from the thread that owns the screen: the stages are timed
 * from here, the same way the amp's line is kept alive. The simulator never
 * calls it. */
/* In the order they really happen, which is also the order they are shown. */
/* ---- The power-on logo (ui_logo.c) -------------------------------------------
 * "15 Peak Hold" from the design file, animated as an equalizer settling into
 * itself over a second and a half, and then gone. It is the first thing on the
 * glass, before the connecting sequence, and it is buying that sequence - and
 * the amp behind it - a head start that the dial cannot buy any other way. */
void ui_logo_show(void);
/* What the eight-second return to Now Playing is looking at this moment. */
void ui_idle_state(bool *playing, bool *screen_busy, bool *amp_connecting, uint32_t *still_ms);
bool ui_logo_showing(void);
/* Take it away: the connecting screen calls this once it has faded in over the
 * top, so the two never cut against each other. */
void ui_logo_hide(void);
/* Test: the finished design, held still - it is on screen for two tenths of a
 * second in use, which is no use for looking at it. */
void ui_test_logo_hold(void);
/* Test: frozen at one moment of the animation, peaks walked up to it. */
void ui_test_logo_at(uint32_t ms);

typedef enum {
    UI_NET_WIFI,
    UI_NET_MUSIC_ASSISTANT,
    UI_NET_HOME_ASSISTANT,
    UI_NET_SPEAKER,
    UI_NET_READY,
    UI_NET_FAILED,
} ui_net_t;
void ui_net_show(ui_net_t state);
/* Whether the speaker Music Assistant plays to is actually there. The three
 * services can all be up while the thing that makes the sound is not, and on
 * 16 Nov 2026 they were: the dial said Connected and every track buffered and
 * skipped. So the fourth row only ticks when this is true, and when it is
 * false the screen says so instead of saying Connected. */
void ui_net_speaker(bool there);
/* This boot is nobody's: the check-in wake runs behind a dark screen, so the
 * opening is not worth the power it burns. */
void ui_net_skip(void);
/* Home Assistant turned the music off - the twenty-minute empty-room rule, or
 * the dashboard. It has already faded, stopped and powered things down out
 * there; this is the dial joining in rather than finding out by watching the
 * amp disappear. */
void ui_music_off_elsewhere(void);
/* The amp finally answered, with somebody still watching: say so, briefly, and
 * hand the screen back. */
/* The amp has gone away and is being waited for again on a dial that is
 * already up: the warm start gets the same introduction the cold one does. */
void ui_net_amp_connecting(void);
void ui_net_amp_connected(void);
/* ...and when it never answers at all. `restarting` says whether the dial is
 * about to power cycle the amps and reboot, or has already tried that. */
void ui_net_amp_problem(bool restarting);
/* Test: that screen now, without waiting two minutes for a cold amp. */
void ui_test_amp_connected(void);
bool ui_net_showing(void);  /* so the amp's line does not sit on top of this one */

/* Touch input, in screen coordinates. The firmware feeds these from the touch
 * panel through LVGL, and the bench feeds them directly to simulate gestures. */
void ui_touch_time(uint32_t ms);  /* optional: when the next touch sample was taken */
void ui_touch_press(int x, int y);
void ui_touch_move(int x, int y);
void ui_touch_release(int x, int y);

/* Whether this touch came in over a bezel rather than being put down on the
 * glass: its first contact was within CORE_EDGE_PX of an edge and it set off
 * for the middle of the screen within CORE_EDGE_DWELL_MS, all three measured
 * off Darren's own fingers on the knob. ui_touch_from_edge is the top bezel,
 * ui_touch_from_left_edge the left one. Decided on the gesture's first real
 * movement and kept until the next press, so either can be read in a release.
 *
 * ui_touch_back_gesture is the one a screen asks: this gesture came in over
 * either bezel and has carried far enough to be meant, so leave. It is the
 * only way back off a list screen, and the second way back everywhere else.
 * The left bezel is there because coming in from the left is what people
 * already do on a phone, and the swipe in over the top is not obvious to
 * anyone who has not been shown it (Darren, 30 Oct 2026, who asked for a
 * two-finger swipe for this: the CST816 in the knob is single-touch and never
 * reports a second finger, so this is the fail-safe instead).
 *
 * It asks for distance travelled rather than distance travelled downwards,
 * because a swipe in over a corner of the round screen is diagonal and the
 * downward part of it alone can fall short, and it asks for much less of it
 * than an ordinary swipe needs, because leaving a screen is a flick and half
 * of Darren's were under 40 px (both measured 30 Oct 2026, from the swipes he
 * reported as hit and miss). See CORE_EDGE_TRAVEL_PX. */
bool ui_touch_from_edge(void);
bool ui_touch_from_left_edge(void);
bool ui_touch_left_edge_pending(void);  /* for a screen that slides something sideways */
bool ui_touch_back_gesture(float dx, float dy, bool moved);

/* The right bezel, which is not a way back: it is Now Playing's shortcut to
 * the Music off question, and only Now Playing reads it, only while music is
 * playing (Darren, 7 Nov 2026 - so the room can be switched off without going
 * back out through the menu). Same thresholds as the back gesture, mirrored.
 * ui_touch_right_edge_pending is the one the screen asks while the finger is
 * still moving, so the track strip does not slide on the way to the question. */
bool ui_touch_from_right_edge(void);
bool ui_touch_right_edge_pending(void);
bool ui_touch_off_gesture(float dx, float dy, bool moved);
/* What the panel actually saw of this touch: how many times it reported the
 * finger moving, and the furthest it ever got from where it landed. The
 * firmware logs both with every touch. They are here because a swipe the panel
 * barely saw is indistinguishable from a tap at the point the list decides
 * which it was (Darren, 30 Oct 2026: scrolling a list sometimes opens a
 * playlist instead). */
int ui_touch_samples(void);
int ui_touch_reach_px(void);
/* How long this touch rested on the glass before its first real movement: 0 for
 * a finger already moving when the panel first saw it. The firmware logs it with
 * every touch, so the two thresholds above can be set from real fingers. */
uint32_t ui_touch_dwell_ms(void);

void ui_show(ui_view_t view);
void ui_open(ui_view_t view);
ui_view_t ui_view(void);
int ui_album_index(void);  /* the album Album browse is settling on */
bool ui_album_flying(void);  /* spun hard enough that letters have replaced the covers */
float ui_album_turn_rate(void);  /* detents a second, as the switch into letters sees it */

/* Album and track slide time in ms. The design says 260 (albums); adjustable
 * so it can be tuned by feel. */
void ui_set_slide_ms(uint32_t ms);
uint32_t ui_slide_ms(void);

/* Jump straight to an album in Album browse (screenshots and tests). */
void ui_goto_album(int index);
/* Open Now Playing on a playlist, as tapping it does (screenshots and tests). */
void ui_goto_playlist(int p);

/* Haptics: `play` makes the motor play a DRV2605 effect number. Off (0) until
 * one is chosen; ui_haptic_candidate(n) is the effect shown as n in the demo. */
void ui_haptic_init(void (*play)(uint8_t effect));
void ui_haptic_set(uint8_t effect);
/* Silence the motor without losing which effect is chosen. */
void ui_haptic_enable(bool on);
bool ui_haptic_enabled(void);
uint8_t ui_haptic_effect(void);
uint8_t ui_haptic_candidate(int n);
void ui_haptic_demo(void);

/* EQ levels set on the knob: 0 treble, 1 bass, 2 subwoofer, in dB. ui_eq_set
 * puts the knob where the amp already is, read once at start-up; it sends
 * nothing back. */
int ui_eq_value(int param);
void ui_eq_set(int param, int db);

/* The volume as Home Assistant has it, for the times something other than the
 * knob moved it: the bathroom dashboard's slider, the Denon's own remote, or
 * the starting volume Darren's automations set at switch-on. 0-100 as the knob
 * counts it. The arc follows and nothing is sent back - this is the amp
 * telling the knob, not the other way round. */
void ui_volume_set(int percent);

/* Paint the rim and arc into a finished block of pixels (panel byte order,
 * RGB565 swapped) just before it's sent. Call from the display flush. */
void ui_overlay_apply(void *px, const lv_area_t *area, int stride_px);
float ui_overlay_arc_fraction(void);  /* -1 when no arc is shown */

/* Automated performance run: scripted drags, swipes, snaps and knob turns.
 * Calls `done` when finished. `frames` returns the display's rendered-frame
 * counter, so each step can report its frame rate. */
void ui_bench_start(uint32_t (*frames)(void), void (*done)(void));

/* The last few Album browse gestures, finger samples and what they did. */
void ui_dump_flicks(void (*print)(const char *line));

/* Test: the clock and then the screen off, now, without the CORE_SLEEP_MS wait. */
void ui_test_sleep(void);
/* Test: all the way to deep sleep, now, without the CORE_DEEP_SLEEP_MS wait. */
void ui_test_deep_sleep(void);
void ui_test_wake(void);
bool ui_clock_up(void);
bool ui_scrubbing(void);

/* Test: release a fling in Album browse at this finger speed (px/ms). */
void ui_test_fling(float speed);

/* Test: the connected screen, held for half a minute so it can be looked at
 * rather than caught. It is gone in under two seconds in real use. */
void ui_test_connected(bool amp_ready);
/* Test: the "why there is no play button" pill on Now Playing. */
void ui_test_amp_note(bool on);
/* Test: open the scrubber without a finger; the knob then moves the position. */
void ui_test_scrub_open(void);  /* true: as it looks once the amp is in too */

#ifdef __cplusplus
}
#endif
