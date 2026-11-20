#pragma once

#include "core.h"
#include "ui.h"

/* ---- Design tokens (DESIGN-TOKENS.md) --------------------------------------- */
#define UI_TEAL         0x32ACA4   /* menu highlight pill          */
/* Not a heading colour any more (Darren, 10 Nov 2026: "all those headers and
 * text like it" read as grey and should be white). What is left is the three
 * places where grey means "not this one, not yet": a list row that isn't the
 * selected one, a network check that hasn't answered, and the track's total
 * beside the elapsed time it is there to contrast with. */
#define UI_GREY         0x919191   /* the dim half of a lit/unlit pair */
#define UI_WHITE        0xFFFFFF
#define UI_BLACK        0x000000
#define UI_VOLUME       0xECD613   /* volume arc                   */
#define UI_VOLUME_LOUD  0xE2402C   /* the same arc past the limit  */
/* One black scrim, at 80% (Darren, 13 Nov 2026). There were four: the title
 * band and the disc at 80%, the chip at 82%, the question panel at 85% and the
 * tone screens' scrim at 75% - four decisions taken on different days that read
 * as the same colour and could not be told apart on the glass. The question
 * panel lets a little more of the cover through than it did, which is the
 * direction it has been moving in anyway (it was solid, then 90%, then 85%),
 * and the tone screens are a shade darker. */
#define UI_OPA_SCRIM    204
/* The loading spinner. Blue because it is none of the other things the rim
 * says - teal is the menu's highlight, yellow the volume, red the limit - and
 * because Darren picked blue (4 Nov 2026). */
#define UI_LOADING      0x4D7DFF
/* A tick against something that is up and answering. Green because that is
 * what a tick is; nothing else on the dial uses it. */
#define UI_OK           0x34C759

/* The chip's pill: 14px text in a 28px box at y 50 in the design, twice made
 * 30% larger because it could not be read on the device (Darren, 26 Oct 2026).
 * Here rather than in ui.c because ui_net.c draws the same pill in the same
 * place and the two must not drift apart. */
#define UI_CHIP_H       47
#define UI_CHIP_Y       64

/* The menu's fisheye picks a real size per item instead of scaling one with a
 * transform, which cost a layer buffer per item per frame (28 Oct 2026). */
LV_FONT_DECLARE(font_inter_light_19)
LV_FONT_DECLARE(font_inter_light_20)
LV_FONT_DECLARE(font_inter_light_21)
LV_FONT_DECLARE(font_inter_light_22)
LV_FONT_DECLARE(font_inter_light_23)
LV_FONT_DECLARE(font_inter_light_24)
LV_FONT_DECLARE(font_inter_light_25)
LV_FONT_DECLARE(font_inter_light_26)
LV_FONT_DECLARE(font_inter_light_27)
LV_FONT_DECLARE(font_inter_light_28)
LV_FONT_DECLARE(font_inter_light_29)
LV_FONT_DECLARE(font_inter_light_30)
LV_FONT_DECLARE(font_inter_light_31)
LV_FONT_DECLARE(font_inter_light_32)
LV_FONT_DECLARE(font_inter_light_33)
LV_FONT_DECLARE(font_inter_light_34)
LV_FONT_DECLARE(font_inter_light_35)
LV_FONT_DECLARE(font_inter_light_36)
LV_FONT_DECLARE(font_inter_regular_10)
LV_FONT_DECLARE(font_inter_regular_11)
LV_FONT_DECLARE(font_inter_regular_12)
LV_FONT_DECLARE(font_inter_regular_13)
LV_FONT_DECLARE(font_inter_regular_14)
LV_FONT_DECLARE(font_inter_regular_15)
LV_FONT_DECLARE(font_inter_regular_16)
LV_FONT_DECLARE(font_inter_regular_17)
LV_FONT_DECLARE(font_inter_regular_19)
LV_FONT_DECLARE(font_inter_regular_20)
LV_FONT_DECLARE(font_inter_regular_21)
LV_FONT_DECLARE(font_inter_regular_22)
LV_FONT_DECLARE(font_inter_regular_23)
LV_FONT_DECLARE(font_inter_regular_24)
LV_FONT_DECLARE(font_inter_regular_26)
LV_FONT_DECLARE(font_inter_regular_18)
LV_FONT_DECLARE(font_inter_extralight_160)
LV_FONT_DECLARE(font_inter_extralight_96)

/* A container with no theme styling: no padding, border, background or scrolling. */
lv_obj_t *ui_plain(lv_obj_t *parent);

/* Bring the screen back as a touch would, without there having been one: the
 * update overlay uses it, because an update behind a dark screen is worse than
 * useless (ui_update.c). */
void ui_force_wake(void);

/* Whether the amp can hear (see ui_amp_state in ui.h). The volume label asks,
 * because while the amp is deaf a number is a lie and "Amp connecting"
 * is the truth. */
ui_amp_t ui_amp(void);

/* Animate `var` from `from` to `to` along CSS cubic-bezier(x1, y1, x2, y2). */
void ui_anim(void *var, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
             float x1, float y1, float x2, float y2);

/* The 12px black rim (radius 168 to 180) drawn over album art. */
void ui_ring_show(bool show);

/* Rim and arc painted on top of each finished frame (ui_overlay.c). */
void ui_overlay_init(lv_obj_t *parent);
void ui_overlay_rim(bool on);
void ui_overlay_arc(bool on, uint32_t color_hex, float from, float to);  /* fractions of a turn from 6 o'clock */
/* The same arc in two colours: `color_hex` up to `split`, `above_hex` past it
 * (the volume arc turns red past the 80% limit). ui_overlay_arc is the plain
 * one-colour arc and clears any split. */
void ui_overlay_arc2(bool on, uint32_t color_hex, uint32_t above_hex, float split, float from, float to);
/* The rim's third meaning: not a level and not a fraction, but "working on it".
 * A short arc going round, breathing longer and shorter. While it runs it owns
 * the rim and plain arc calls are ignored, so opening a screen cannot stop it
 * (Darren, 4 Nov 2026). */
void ui_overlay_spin(bool on, uint32_t color_hex);
/* Brightness (0-255) and speed (1.0 = full). Changing speed does not move the
 * arc: its position is carried forward step by step, not derived from a clock. */
void ui_overlay_spin_style(uint8_t bright, float speed);
bool ui_overlay_spinning(void);
/* The spinner stopped where it is and drawn by the caller - how it closes into
 * a ring when the dial has connected. Fractions of a turn from 6 o'clock; a
 * span of a whole turn is a closed ring. ui_overlay_spin(false) ends it,
 * ui_overlay_spin_release sets the spinner going again. */
void ui_overlay_spin_hold(float from, float to, uint32_t color_hex, uint8_t bright);
void ui_overlay_spin_release(void);
void ui_overlay_spin_span(float *from, float *to);

uint32_t ui_touch_sample_ms(void);  /* time of the touch sample being handled */

void ui_tick(void);  /* a haptic tick: something snapped into place */

/* Something on screen takes `ms` to read (a scrolling title): the clock waits
 * for it while `what` is still showing. Done: `what` stopped (only if it's
 * still the one being read). */
void ui_reading(lv_obj_t *what, uint32_t ms);
void ui_reading_done(lv_obj_t *what);

/* Short label at the top of the screen (volume, skip): it holds for a moment
 * and then hands the slot back to the play time, or fades if nothing plays. */
void ui_chip(const char *text);
/* The same slot, drawn as "Volume" plus a number: the pill is one width, the
 * word is pinned at the left of it and the digits have a fixed slot of their
 * own, so turning the knob moves nothing but the digits (Darren, 10 Nov 2026).
 * `n` is the number alone, without the word. */
void ui_chip_volume(const char *n);
/* Put the play time back in that slot (or take it away): called as the second
 * ticks over, and when playback starts or stops. */
void ui_chip_rest(void);
/* Empty the slot now, whatever is in it: the view that owns it changed, or the
 * scrubber has taken the play time over. */
void ui_chip_off(void);

/* ---- Views. Each takes its gesture as offsets from where the touch began. ---- */
void menu_create(lv_obj_t *parent);
lv_obj_t *menu_root(void);
void menu_press(void);
void menu_drag(float dy);
void menu_release(float dy, bool moved);
void ui_music_off(void);   /* stop the music (Music off, confirmed) and show the clock */
/* Put the "Music off?" question up: the menu's own item, and Now Playing's
 * swipe in over the right bezel, which is a shortcut to this and nothing more. */
void ui_ask_music_off(void);

/* Ask first (ui_ask.c): a question over the screen, laid out like a list: the
 * question as the heading, then `button` and "Cancel". */
typedef struct {
    const char *title, *button;
    const char *hint;           /* a grey line under the answer (one-answer questions) */
    void (*yes)(void);          /* `button` chosen */
    void (*cancelled)(void);    /* Cancel, a swipe down or left alone; may be NULL */
    /* Non-zero: `button` doesn't act at once but counts down this long on the
     * same screen, the rim winding back from full to empty, and a tap or a
     * turn in that time cancels. `counting` heads the countdown. */
    int count_ms;
    const char *counting;
} ui_ask_t;
void ui_ask(const ui_ask_t *q);
/* The same question with one answer and no Cancel: the knob is left alone, so
 * the screen underneath keeps it and a turn can't choose anything. Nothing
 * happens unless the answer is tapped; it goes by itself as any question does. */
void ui_ask_one(const ui_ask_t *q);
bool ui_ask_takes_knob(void);  /* false while a one-answer question is up */
bool ui_ask_is(const ui_ask_t *q);  /* is this the question that's up? */
void ui_ask_keepalive(void);   /* input elsewhere: the question waits longer */
bool ui_asking(void);   /* a question, or its countdown, has the screen */
bool ui_ask_counting(void);
void ui_ask_cancel(void);
void ui_ask_press(int y);
void ui_ask_drag(float dx, float dy);
void ui_ask_release(float dx, float dy, bool moved);
void ui_ask_knob(int steps);
void np_volume_relock(void);  /* the 80% limit back (Music off) */
void menu_goto(int index);   /* animated move to an item */
void menu_knob(int steps);

void albums_create(lv_obj_t *parent, const ui_library_t *lib);
lv_obj_t *albums_root(void);
void albums_enter(void);
void albums_press(int x, int y);
void albums_drag(float dx, float dy);
void albums_release(float dx, float dy, bool moved);
int albums_current(void);
bool albums_flying(void);
float albums_turn_rate(void);
void albums_knob(int steps);
void albums_cover_changed(int album);

void np_create(lv_obj_t *parent, const ui_library_t *lib);
void np_cover_changed(int album);
lv_obj_t *np_root(void);
void np_open(int album);     /* show an album's tracks */
void np_enter(void);         /* the view became visible */
void np_press(int x, int y);
void np_drag(float dx, float dy);
void np_release(int x, int y, float dx, float dy, bool moved);
void np_knob(int steps);
bool np_playing(void);
void np_amp_changed(void);  /* the play disc is dim while the amp cannot play */
/* The play time for the label slot ("1:23"), or NULL when Now Playing isn't
 * the screen or nothing is playing on it. */
const char *np_play_time(void);
/* The track has been asked for but the player hasn't started it yet: the slot
 * shows a sweeping bar instead of a time standing still. */
bool np_loading(void);
/* The scrubber is up: the track open full screen, a detent or a swipe moving
 * the position (the test harness asks what is on screen). */
bool np_scrubbing(void);
int np_player_album(void);
bool np_showing_player(void);
void np_volume_step(int steps);  /* the volume knob, also used by the EQ overview */
void np_volume_rim(void);        /* draw the volume arc on the rim */
/* The EQ moved: pass it on to the player (ui.h's ui_player_t). */
void np_eq_changed(int param, int db);

void search_create(lv_obj_t *parent);
lv_obj_t *search_root(void);
void search_open(void);
void search_enter(void);
bool search_busy(void);  /* listening or searching: no idle jumps */
void search_press(int y);
void search_drag(float dx, float dy);
void search_release(int x, int y, float dx, float dy, bool moved);
void search_knob(int steps);
void np_open_track(int album, const char *track);  /* from search; NULL track: the first */
void np_open_from_albums(int album);
void np_open_from_playlists(int playlist);
void np_open_recent(int at);  /* the Recent list itself, on its item `at` */

void recent_create(lv_obj_t *parent, const ui_library_t *lib);
lv_obj_t *recent_root(void);
void recent_open(void);
void recent_enter(void);
void recent_press(int y);
void recent_drag(float dx, float dy);
void recent_release(float dx, float dy, bool moved);
void recent_knob(int steps);
const lv_image_dsc_t *np_cover(void);  /* the picture of the track on screen */

void playlists_create(lv_obj_t *parent, const ui_library_t *lib);
lv_obj_t *playlists_root(void);
void playlists_open(void);
void playlists_enter(void);
void playlists_press(int y);
void playlists_drag(float dx, float dy);
void playlists_release(float dx, float dy, bool moved);
void playlists_knob(int steps);

void settings_create(lv_obj_t *parent);
lv_obj_t *settings_root(void);
void settings_open(void);
void settings_enter(void);
void settings_release(float dx, float dy, bool moved);
/* Settings is two pages: this one and Connections (ui_status.c). The knob moves
 * between them and two dots across the top of each say which one. Both are in
 * ui_settings.c so the page count lives in one place. */
enum { SETTINGS_PAGE_SETTINGS, SETTINGS_PAGE_STATUS };
/* Both pages hang their contents in the same band and put their pips at the top
 * of it, so the dots do not move as the knob pages between them - they did, by
 * 6 px, because each page had its own idea of where the top was (Darren, 16 Nov
 * 2026). The y is not a parameter for the same reason: there is one, and it is
 * settings_pips_create's to know. */
#define SETTINGS_BLOCK_TOP  41
#define SETTINGS_BLOCK_BOT 317
#define SETTINGS_PIP_D       9
#define SETTINGS_PIPS 2
typedef struct { lv_obj_t *dot[SETTINGS_PIPS]; } settings_pips_t;
void settings_pips_create(settings_pips_t *p, lv_obj_t *parent);
void settings_pips_set(const settings_pips_t *p, int page);
void settings_page_knob(int steps, int page);

void status_create(lv_obj_t *parent);
lv_obj_t *status_root(void);
void status_open(void);
void status_enter(void);
void status_release(float dx, float dy, bool moved);

void clock_create(void);
bool clock_showing(void);
void clock_show(void);
void clock_hide(void);
/* The screen has gone off under the clock (or come back): the clock stops
 * being redrawn while nobody can see it. */
void clock_dark(bool dark);

void eq_create(lv_obj_t *parent, const ui_library_t *lib);
lv_obj_t *eq_root(void);
void eq_open(int album);
void eq_enter(void);
void eq_cover_changed(int album);
void eq_release(int x, int y, float dx, float dy, bool moved);
void eq_knob(int steps);
bool eq_on_overview(void);  /* the overview, where the rim is the volume's */
bool eq_step_back(void);    /* idle: focus -> overview; false if already there */
void np_stop(void);
/* ...but leave the music playing for `ms` first, so Music off fades out. */
void np_stop_after(int ms);
