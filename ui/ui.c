/* Screen manager: shows one view at a time and routes touch and knob input to it. */
#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static ui_view_t s_view = UI_VIEW_MENU;
static lv_obj_t *s_chip;
static lv_timer_t *s_chip_timer;
static bool s_chip_held;     /* a volume or skip label is standing in the slot */
static bool s_chip_resting;  /* ...or the slot is back to the play time */

static uint32_t s_last_input;
/* Two countdowns run on this dial and they are not the same countdown, which
 * is why there are two stamps.
 *
 * s_last_input above is the power one - the clock, then the screen going dark.
 * It is pushed forward for as long as the dial has a job on, music playing
 * included, because the whole of the 4 Nov rule in idle_check is that no
 * power countdown may run while the dial is doing something.
 *
 * s_still is the hand one: how long since anybody last touched the glass or
 * turned the knob. It is what the eight seconds back to Now Playing has always
 * meant to measure, and that countdown exists *only* while music is playing -
 * it is a return to the music, not a leaving of it. Measuring it on the power
 * stamp meant that from the moment a record started, the stamp was refreshed
 * twice a second and the eight seconds could never elapse: every screen
 * reached while playing stayed up for ever, which is what Darren found on the
 * tone screens (5 Nov 2026). So the hand stamp is pushed by real input and by
 * a job that owns the screen, and never by the music. */
static uint32_t s_still;
/* When the screen last came back from dark, and whether the amp has already
 * had its say since. Both only for the rule in ui_amp_state.
 *
 * The window is eight seconds because the first probe of the amp goes out the
 * instant the screen wakes (amp_awake notifies the task) and gives up after
 * four, so an amp that is not there has always said so well inside it. Past
 * that he is not arriving any more, he is using the dial.
 *
 * Still means a second and a bit since the last detent or touch. Scrolling the
 * menu lands one every few hundred milliseconds, so this tells a hand that has
 * stopped from a hand that is working, and errs towards saying nothing.
 *
 * True at boot: the opening sequence introduces the amp itself on a cold
 * start, and must not be second-guessed by this rule. */
#define AMP_INTRO_MS       8000
#define AMP_INTRO_STILL_MS 1200
static uint32_t s_woke_at;
static bool s_amp_intro_done = true;
static uint32_t s_sample_time;  /* set by ui_touch_time() for the next sample, else now */

void ui_touch_time(uint32_t ms) { s_sample_time = ms; }

/* Time of the touch sample being handled: its own timestamp if the caller gave
 * one, otherwise now. */
uint32_t ui_touch_sample_ms(void) { return s_sample_time ? s_sample_time : lv_tick_get(); }

static void sample_done(void) { s_sample_time = 0; }

static struct {
    bool down;
    bool moved;
    int x0, y0;
    uint32_t t0;      /* when the first contact was reported */
    bool decided;     /* the gesture has had its first real movement */
    bool from_edge;   /* and it looked like a finger coming over the top bezel */
    bool from_left;   /* or over the left one, heading in across the screen */
    bool from_right;  /* or over the right one (Now Playing's Music off shortcut) */
    uint32_t dwell_ms; /* how long it rested on the glass before moving */
    int samples;      /* how many times the panel reported it moving */
    float reach_px;   /* and the furthest it ever got from where it landed */
} s_touch;

bool ui_touch_from_edge(void) { return s_touch.from_edge; }
bool ui_touch_from_left_edge(void) { return s_touch.from_left; }
bool ui_touch_from_right_edge(void) { return s_touch.from_right; }
int ui_touch_samples(void) { return s_touch.samples; }
int ui_touch_reach_px(void) { return (int)lroundf(s_touch.reach_px); }

bool ui_touch_left_edge_pending(void)
{
    /* Still might be a swipe in over the left bezel. Albums and Now Playing
     * slide their artwork sideways under the finger, so they ask this rather
     * than the verdict itself: by the time the verdict is in, the art would
     * have shuffled across and have to snap back. */
    return s_touch.from_left ||
           (s_touch.decided && s_touch.x0 <= CORE_EDGE_PX &&
            s_touch.dwell_ms <= CORE_EDGE_DWELL_MS);
}

bool ui_touch_right_edge_pending(void)
{
    /* The mirror of ui_touch_left_edge_pending, for the same reason: Now
     * Playing drags the track strip sideways under the finger, and a swipe in
     * over the right bezel must not have shuffled the tracks across on its way
     * to the Music off question. */
    return s_touch.from_right ||
           (s_touch.decided && s_touch.x0 >= CORE_SCREEN - 1 - CORE_EDGE_PX &&
            s_touch.dwell_ms <= CORE_EDGE_DWELL_MS);
}

bool ui_touch_back_gesture(float dx, float dy, bool moved)
{
    return moved && (s_touch.from_edge || s_touch.from_left) &&
           sqrtf(dx * dx + dy * dy) > CORE_EDGE_TRAVEL_PX;
}
/* The same shape as ui_touch_back_gesture, for the right bezel: Now Playing's
 * shortcut to Music off (Darren, 7 Nov 2026). Nowhere else reads it, so coming
 * in over the right edge still means nothing on any other screen. */
bool ui_touch_off_gesture(float dx, float dy, bool moved)
{
    return moved && s_touch.from_right &&
           sqrtf(dx * dx + dy * dy) > CORE_EDGE_TRAVEL_PX;
}
uint32_t ui_touch_dwell_ms(void) { return s_touch.dwell_ms; }

lv_obj_t *ui_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void ui_anim(void *var, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
             float x1, float y1, float x2, float y2)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(&a, (int16_t)LV_BEZIER_VAL_FLOAT(x1), (int16_t)LV_BEZIER_VAL_FLOAT(y1),
                              (int16_t)LV_BEZIER_VAL_FLOAT(x2), (int16_t)LV_BEZIER_VAL_FLOAT(y2));
    lv_anim_start(&a);
}

/* ---- Rim -------------------------------------------------------------------
 * Painted by ui_overlay.c on top of every frame (see there for why). */
void ui_ring_show(bool show) { ui_overlay_rim(show); }

/* ---- Chip: the shared play time / volume / skip label -----------------------
 * While a track plays the slot shows the play time; a volume turn or a skip
 * stands in front of it for CORE_CHIP_BACK_MS and then hands it back (Darren,
 * 31 Oct 2026). With nothing playing there is nothing behind it, so it fades
 * away as it always did. */
static void chip_opa(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

static void chip_fade_out(void)
{
    s_chip_resting = false;
    ui_anim(s_chip, chip_opa, lv_obj_get_style_opa(s_chip, 0), LV_OPA_TRANSP, CORE_LABEL_FADE_MS,
            0.25f, 0.1f, 0.25f, 1.0f);
}

static void chip_hide(lv_timer_t *t)
{
    lv_timer_pause(t);
    s_chip_held = false;
    ui_chip_rest();  /* back to the play time, or away if nothing is playing */
}

/* The design's label is 14px text in a 28px box at y 50. On the device it was
 * too small to read: 30% larger made it 18px in 36 (Darren, 26 Oct 2026), then
 * 30% larger again, 24px in 47, brought down to y 64 so it isn't cramped
 * against the rim (Darren, same day; to be tuned by eye). The two figures live
 * in ui_internal.h: the network pill is the same pill in the same place. */
#define CHIP_H UI_CHIP_H
#define CHIP_Y UI_CHIP_Y

/* ---- Waiting for the music (Darren, 2 Nov 2026) -------------------------------
 * Picking a track off Recent leaves three or four seconds before Music
 * Assistant has the stream open, and a time standing still at 0:00 reads as a
 * frozen screen. The slot shows a bar sweeping instead, the same one the title
 * band uses while an album's track list is on its way, so the wait looks like
 * work rather than a fault. */
#define CHIP_LOAD_W 72
#define CHIP_LOAD_SEG_W 24
#define CHIP_LOAD_SWEEP_MS 700
static lv_obj_t *s_chip_load, *s_chip_loadseg;

/* ---- Nothing moves but the digits (Darren, 10 Nov 2026) -----------------------
 * The pill sizes itself to its text, so every click of the knob redrew it a
 * few pixels wider or narrower - "Volume 7", "Volume 70", "Volume 100" - and
 * a pill centred on the screen moves at both ends when it does. Turning the
 * knob made it jiggle.
 *
 * Holding the pill at one width was only half of it: the string was still
 * centred in that width, so a narrower number slid the word "Volume" along
 * with it. The word has to be nailed down too, which it cannot be while word
 * and number are one piece of text. So they are two: the word sits at the
 * left of a pill of fixed width and never moves, and the number has a slot of
 * its own after it, as wide as "100" will ever be, with the digits centred in
 * that slot. The pill, the word and the slot all stay put; only the digits
 * inside the slot change.
 *
 * Everything else in this slot (the play time, the skip count, "Amp not
 * ready") still sizes to its own text: those change rarely, and "Amp not
 * ready" is wider than any volume. */
static lv_obj_t *s_chip_num;   /* the digits, in their own fixed slot */
static int32_t s_chip_vol_w;   /* the whole pill, word and slot and padding */

/* Put text in the slot at `w` px wide, or sized to the text when w is 0. The
 * number slot goes away unless the volume puts it back. */
static void chip_text(const char *text, int32_t w)
{
    lv_obj_add_flag(s_chip_num, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_chip, text);
    lv_obj_set_style_text_align(s_chip, LV_TEXT_ALIGN_AUTO, 0);
    lv_obj_set_width(s_chip, w > 0 ? w : LV_SIZE_CONTENT);
    lv_obj_align(s_chip, LV_ALIGN_TOP_MID, 0, CHIP_Y);
}

/* "Volume" pinned at the left of the pill, `n` centred in the slot after it. */
static void chip_volume_text(const char *n)
{
    chip_text("Volume", s_chip_vol_w);
    lv_label_set_text(s_chip_num, n);
    lv_obj_remove_flag(s_chip_num, LV_OBJ_FLAG_HIDDEN);
}

static void chip_seg_x(void *obj, int32_t v) { lv_obj_set_x(obj, v); }

static void chip_loading(bool show)
{
    if (show == !lv_obj_has_flag(s_chip_load, LV_OBJ_FLAG_HIDDEN)) return;  /* already so */
    if (!show) {
        lv_anim_delete(s_chip_loadseg, chip_seg_x);
        lv_obj_add_flag(s_chip_load, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_chip_load, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_chip_loadseg);
    lv_anim_set_exec_cb(&a, chip_seg_x);
    lv_anim_set_values(&a, 0, CHIP_LOAD_W - CHIP_LOAD_SEG_W);
    lv_anim_set_duration(&a, CHIP_LOAD_SWEEP_MS);
    lv_anim_set_reverse_duration(&a, CHIP_LOAD_SWEEP_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void chip_create(lv_obj_t *parent)
{
    const lv_font_t *f = &font_inter_regular_24;
    s_chip = lv_label_create(parent);
    lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_chip, f, 0);
    lv_obj_set_style_text_color(s_chip, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_bg_color(s_chip, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_chip, UI_OPA_SCRIM, 0);
    lv_obj_set_style_radius(s_chip, CHIP_H / 2, 0);
    lv_obj_set_style_pad_hor(s_chip, CHIP_H / 2, 0);
    lv_obj_set_style_pad_top(s_chip, (CHIP_H - lv_font_get_line_height(f)) / 2, 0);
    lv_obj_set_height(s_chip, CHIP_H);
    lv_obj_set_style_opa(s_chip, LV_OPA_TRANSP, 0);
    lv_obj_align(s_chip, LV_ALIGN_TOP_MID, 0, CHIP_Y);
    /* The volume's two fixed pieces, measured off the font once: the word, and
     * the slot the digits sit in. "Volume " is where the slot starts and never
     * moves; "100" is as wide as the number will ever be. */
    lv_point_t word, num;
    lv_text_get_size(&word, "Volume ", f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&num, "100", f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    s_chip_vol_w = CHIP_H + word.x + num.x;  /* pad_hor is CHIP_H / 2 each side */
    s_chip_num = lv_label_create(s_chip);
    lv_obj_remove_flag(s_chip_num, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_chip_num, f, 0);
    lv_obj_set_style_text_color(s_chip_num, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_text_align(s_chip_num, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_chip_num, num.x);
    /* Top-left of the word's own text, not the middle of the pill: the chip's
     * pad_top is what puts the text where it is, and the digits must sit on
     * the same line as the word. */
    lv_obj_align(s_chip_num, LV_ALIGN_TOP_LEFT, word.x, 0);
    lv_obj_add_flag(s_chip_num, LV_OBJ_FLAG_HIDDEN);
    s_chip_timer = lv_timer_create(chip_hide, CORE_LABEL_HOLD_MS, NULL);
    lv_timer_set_auto_delete(s_chip_timer, false);  /* LVGL deletes a finished one-shot timer otherwise */
    lv_timer_pause(s_chip_timer);

    /* On the chip's own pill, so it reads over a bright cover exactly as the
     * time it stands in for does: the same padding either side of the bar. */
    s_chip_load = ui_plain(parent);
    lv_obj_set_size(s_chip_load, CHIP_LOAD_W + CHIP_H, CHIP_H);
    lv_obj_set_style_radius(s_chip_load, CHIP_H / 2, 0);
    lv_obj_set_style_bg_color(s_chip_load, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_chip_load, UI_OPA_SCRIM, 0);
    lv_obj_align(s_chip_load, LV_ALIGN_TOP_MID, 0, CHIP_Y);
    lv_obj_t *track = ui_plain(s_chip_load);
    lv_obj_set_size(track, CHIP_LOAD_W, 4);
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_20, 0);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, 0);
    s_chip_loadseg = ui_plain(track);
    lv_obj_set_pos(s_chip_loadseg, 0, 0);
    lv_obj_set_size(s_chip_loadseg, CHIP_LOAD_SEG_W, 4);
    lv_obj_set_style_radius(s_chip_loadseg, 2, 0);
    lv_obj_set_style_bg_color(s_chip_loadseg, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_bg_opa(s_chip_loadseg, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_chip_load, LV_OBJ_FLAG_HIDDEN);
}

/* The play time, when it is the screen's own: no fade, so handing the slot
 * back from the volume is a change of text and not a blink. */
void ui_chip_rest(void)
{
    if (s_chip_held) return;  /* the volume has the slot for now */
    if (np_loading()) {       /* waiting on the player: the sweeping bar has it */
        if (lv_obj_get_style_opa(s_chip, 0) != LV_OPA_TRANSP) chip_fade_out();
        chip_loading(true);
        return;
    }
    chip_loading(false);
    const char *t = np_play_time();
    if (!t) {
        if (lv_obj_get_style_opa(s_chip, 0) != LV_OPA_TRANSP) chip_fade_out();
        return;
    }
    chip_text(t, 0);
    if (!s_chip_resting) {
        s_chip_resting = true;
        ui_anim(s_chip, chip_opa, lv_obj_get_style_opa(s_chip, 0), LV_OPA_COVER, CORE_LABEL_FADE_MS,
                0.25f, 0.1f, 0.25f, 1.0f);
    }
}

/* Take the slot away at once, whatever is in it: the view that owns it has
 * changed, or Now Playing has put the play time somewhere bigger (scrubbing). */
void ui_chip_off(void)
{
    lv_anim_delete(s_chip, chip_opa);
    lv_obj_set_style_opa(s_chip, LV_OPA_TRANSP, 0);
    lv_timer_pause(s_chip_timer);
    chip_loading(false);
    s_chip_held = s_chip_resting = false;
}

/* `text` NULL: the caller sets the pill's contents itself after this (the
 * volume, which is a word and a number rather than one string). */
static void chip_show(const char *text, int32_t w)
{
    s_chip_held = true;
    s_chip_resting = false;
    chip_loading(false);  /* the volume stands in front of the wait as it does the time */
    if (text) chip_text(text, w);
    if (lv_obj_get_style_opa(s_chip, 0) != LV_OPA_COVER || lv_anim_get(s_chip, chip_opa))
        ui_anim(s_chip, chip_opa, lv_obj_get_style_opa(s_chip, 0), LV_OPA_COVER, CORE_LABEL_FADE_MS,
                0.25f, 0.1f, 0.25f, 1.0f);
    lv_timer_set_period(s_chip_timer,
                        np_play_time() || np_loading() ? CORE_CHIP_BACK_MS : CORE_LABEL_HOLD_MS);
    lv_timer_set_repeat_count(s_chip_timer, 1);
    lv_timer_reset(s_chip_timer);
    lv_timer_resume(s_chip_timer);
}

void ui_chip(const char *text) { chip_show(text, 0); }

/* `n` is the number alone: the word is the chip's own text and stays put. */
void ui_chip_volume(const char *n) { chip_show(NULL, 0); chip_volume_text(n); }

/* ---- Views ------------------------------------------------------------------ */
void ui_show(ui_view_t view)
{
    s_view = view;
    /* Opening a screen is something that has just happened, so the eight
     * seconds back to Now Playing start again from here. Every screen change
     * in normal use comes from a touch or a detent, which already did this;
     * what it fixes is a screen opened any other way - a console command, a
     * test - being snatched back by the next idle check half a second later
     * (31 Oct 2026). */
    s_last_input = s_still = lv_tick_get();
    /* The slot belongs to Now Playing (prototype #chip) and is lent to the tone
     * screens: the overview's knob is the volume, so the volume pill belongs
     * there, and the amp may still have something to say. What is *not* lent is
     * the play time, which is Now Playing's own - np_play_time stops answering
     * the moment the view changes. Left standing, it sat over the EQ at full
     * opacity until the next second turned over and only then faded, which read
     * as the time flashing over the tone screen on the way in (Darren, 5 Nov
     * 2026). So the test is whether something is being *held* in the slot: a
     * label put there deliberately - the volume, a skip, the amp - travels, and
     * anything else, the play time included, goes. */
    if (view != UI_VIEW_NOWPLAYING && (view != UI_VIEW_EQ || !s_chip_held))
        ui_chip_off();
    lv_obj_t *roots[] = {menu_root(),      albums_root(),    np_root(),     eq_root(),
                         search_root(),    playlists_root(), recent_root(), settings_root(),
                         status_root()};
    for (int i = 0; i < (int)(sizeof roots / sizeof *roots); i++) {
        if (i == (int)view) lv_obj_remove_flag(roots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(roots[i], LV_OBJ_FLAG_HIDDEN);
    }
    ui_ring_show(view != UI_VIEW_MENU);
    /* the rim shows what the knob does: albums -> position in the library,
     * tracks -> which track, playing -> volume; the menu has none */
    if (view == UI_VIEW_MENU) ui_overlay_arc(false, UI_WHITE, 0, 0);
    if (view == UI_VIEW_ALBUMS) albums_enter();
    if (view == UI_VIEW_NOWPLAYING) np_enter();
    if (view == UI_VIEW_EQ) eq_enter();
    if (view == UI_VIEW_SEARCH) search_enter();
    if (view == UI_VIEW_PLAYLISTS) playlists_enter();
    if (view == UI_VIEW_RECENT) recent_enter();
    if (view == UI_VIEW_SETTINGS) settings_enter();
    if (view == UI_VIEW_STATUS) status_enter();
}

/* Opening a screen the way a finger does. Four screens build themselves the
 * first time they are opened - the list, its heading band, even the root's
 * background colour - so ui_show alone lands on a bare root with no children
 * and nothing in it. On the dial that looked like a broken Playlists screen
 * and cost a false bug report; through the menu it was always fine, because
 * the menu calls these (found by another session, 2 Nov 2026). */
void ui_open(ui_view_t view)
{
    switch (view) {
    case UI_VIEW_SEARCH: search_open(); break;
    case UI_VIEW_PLAYLISTS: playlists_open(); break;
    case UI_VIEW_RECENT: recent_open(); break;
    case UI_VIEW_SETTINGS: settings_open(); break;
    case UI_VIEW_STATUS: status_open(); break;
    default: ui_show(view); break;
    }
}

ui_view_t ui_view(void) { return s_view; }
int ui_album_index(void) { return albums_current(); }
bool ui_album_flying(void) { return albums_flying(); }
float ui_album_turn_rate(void) { return albums_turn_rate(); }

void ui_cover_changed(int album)
{
    albums_cover_changed(album);
    np_cover_changed(album);
    eq_cover_changed(album);
}

/* ---- Reading ------------------------------------------------------------------
 * The clock comes after a time without input, but not while a long title is
 * still scrolling on screen: someone may be reading it without touching
 * anything (Darren, 27 Oct 2026). One title at a time: the latest to start. */
static lv_obj_t *s_read_obj;
static uint32_t s_read_from, s_read_ms;

void ui_reading(lv_obj_t *what, uint32_t ms)
{
    s_read_obj = what;
    s_read_from = lv_tick_get();
    s_read_ms = ms;
}

void ui_reading_done(lv_obj_t *what)
{
    if (s_read_obj == what) s_read_obj = NULL;
}

static bool reading(void)
{
    return s_read_obj && lv_obj_is_visible(s_read_obj) && lv_tick_elaps(s_read_from) < s_read_ms;
}

/* Still coming up, so not yet idle. Nothing is playing because nothing *can*
 * play yet: the amp was asked for and has not answered.
 *
 * No time limit on this. There was one, and it meant the dial gave up waiting
 * and went dark in front of somebody who was still waiting with it. What
 * happens after three minutes is that the dial decides the connection has
 * failed and fixes it (app_main, CORE_AMP_GIVE_UP_MS) - not that it stops
 * caring.
 *
 * But only until the amp has answered once. "This is only meant to not work
 * when we're still trying to connect. Once it's connected, that's it...
 * And if I decide to do nothing, then it should power down" (Darren, 4 Nov
 * 2026). An amp that drops an hour later is a different thing entirely: the
 * dial has long since done its job, and holding the screen on for that would
 * flatten the battery over a fault nobody is standing in front of. */
/* How long the current wait for the amp has been going. Not used for any
 * countdown - see the one rule in idle_check - but app_main measures the
 * three-minute give-up with it. */
static int s_amp_secs;

/* The clock covers the screen like a screensaver: everything underneath is
 * hidden (so it isn't redrawn) and the rim arcs are off, but nothing is reset. */
static void sleep_to_clock(void)
{
    lv_obj_t *roots[] = {menu_root(),      albums_root(),    np_root(),     eq_root(),
                         search_root(),    playlists_root(), recent_root(), settings_root(),
                         status_root()};
    for (int i = 0; i < (int)(sizeof roots / sizeof *roots); i++) lv_obj_add_flag(roots[i], LV_OBJ_FLAG_HIDDEN);
    ui_chip_off();
    ui_overlay_arc(false, UI_WHITE, 0, 0);
    clock_show();
}

/* ---- Amp power --------------------------------------------------------------
 * On through use, off through disuse: see ui_power_init. */
static const ui_power_t *s_power;
void ui_power_init(const ui_power_t *power) { s_power = power; }

/* Someone is using the knob, so the amps should be on by the time they want
 * them. Called from every touch and every detent that isn't the swallowed wake,
 * which is why it does nothing itself beyond passing it on. */
/* Whether the amps have been let go since anybody last asked for them, so the
 * idle countdown that lets them go does it once and not every two seconds. */
static bool s_power_let_go;
_Static_assert(CORE_AMP_LET_GO_MS > CORE_SLEEP_MS && CORE_AMP_LET_GO_MS < CORE_DEEP_SLEEP_MS,
               "the amps are let go after the screen sleeps and before the dial goes under");

static void power_wanted(void)
{
    s_power_let_go = false;
    if (s_power && s_power->on) s_power->on();
}

static void power_finished(void)
{
    s_power_let_go = true;
    if (s_power && s_power->off) s_power->off();
}

/* Music off, confirmed: the music stops, the amp and the sub are switched off
 * in Home Assistant (input_boolean.bathroom_music, which fades it down first),
 * and the clock comes up over the menu. */
/* The clock after Music off waits a moment (see CORE_MUSIC_OFF_CLOCK_MS), so
 * the screen does not vanish the instant the question is answered. */
static lv_timer_t *s_off_clock_timer;

static void off_clock_start(void);
static bool dial_busy(void);

/* The same rule as idle_check's, and for the same reason. This timer is a
 * second door to the clock and it was built without the rule on it: it only
 * ever cancelled if a hand touched the glass or turned the knob inside its
 * three seconds, so anything else the dial was in the middle of - still
 * coming up, still finding the amp, a question on screen, firmware being
 * written - counted for nothing and the clock arrived over the top of it
 * (Darren, 5 Nov 2026). The countdown does not run while the dial is doing
 * something, wherever the countdown lives.
 *
 * Whether music is playing is deliberately NOT part of this one. It is the
 * only door whose whole subject is the music having gone off, and after Music
 * off the music keeps playing through the ten-second fade - so counting that
 * as busy would hold the clock back until the fade ended, which is not what
 * CORE_MUSIC_OFF_CLOCK_MS was chosen to do. */
static void off_to_clock(lv_timer_t *t)
{
    (void)t;
    if (dial_busy()) {
        off_clock_start();  /* ...and ask again in another CORE_MUSIC_OFF_CLOCK_MS */
        return;
    }
    lv_timer_pause(s_off_clock_timer);
    if (!clock_showing()) sleep_to_clock();
}

static void off_clock_start(void)
{
    if (!s_off_clock_timer) {
        s_off_clock_timer = lv_timer_create(off_to_clock, CORE_MUSIC_OFF_CLOCK_MS, NULL);
        lv_timer_set_auto_delete(s_off_clock_timer, false);
    }
    lv_timer_set_repeat_count(s_off_clock_timer, 1);
    lv_timer_reset(s_off_clock_timer);
    lv_timer_resume(s_off_clock_timer);
}

/* Any use of the dial inside that window cancels it: somebody is still here,
 * and the ordinary idle timer will bring the clock up in its own time. */
static void off_clock_cancel(void)
{
    if (s_off_clock_timer) lv_timer_pause(s_off_clock_timer);
}

/* Home Assistant turned the music off - the twenty-minute empty-room rule, or
 * the dashboard. It has already faded the volume, stopped the stream and
 * powered the amp and the sub down, so there is nothing to send: all that is
 * left is for the dial to stop believing it is playing, hand the volume limit
 * back and get out of the way, exactly as its own Music off does (Darren,
 * 4 Nov 2026). */
void ui_music_off_elsewhere(void)
{
    if (ui_ask_counting()) ui_ask_cancel();  /* already done out there: nothing left to count down to */
    np_stop_after(0);  /* no fade: the fade has already happened, out there */
    np_volume_relock();
    off_clock_start();
}

void ui_music_off(void)
{
    /* The amp is handed to Darren's own fade, and the music is left playing
     * through it: what you hear is the room going quiet over ten seconds, not
     * a track stopped dead and then a fade of nothing (31 Oct 2026). */
    np_stop_after(CORE_FADE_OFF_MS);
    np_volume_relock();  /* the 80% limit comes back */
    power_finished();
    /* The countdown was the moment to see it happen (CORE_MUSIC_OFF_COUNT_MS),
     * so the clock comes now - unless the dial is in the middle of something,
     * when the ordinary wait-and-ask-again applies. */
    if (dial_busy()) off_clock_start();
    else if (!clock_showing()) sleep_to_clock();
}

/* ---- Low power ---------------------------------------------------------------
 * Only ever entered from the clock, and the clock is only ever up with the
 * music off, so this cannot catch anything that is playing. Nothing is reset:
 * the view, the album position and the loaded covers are all still there when
 * it comes back. */
static void (*s_set_asleep)(bool);
static void (*s_deep_sleep)(void);
static bool s_asleep;
static bool s_was_playing;  /* so music STARTING can be told from music still playing */
static lv_timer_t *s_idle_timer;

void ui_sleep_init(void (*set_asleep)(bool asleep)) { s_set_asleep = set_asleep; }
void ui_deep_sleep_init(void (*deep_sleep)(void)) { s_deep_sleep = deep_sleep; }
bool ui_asleep(void) { return s_asleep; }

static void go_to_sleep(void)
{
    s_asleep = true;
    /* The amps are not let go here. The screen going dark is about the dial's
     * battery; the amps run off the mains and are let go on their own, longer
     * countdown - see CORE_AMP_LET_GO_MS. Letting them go here meant a cold
     * start that Darren walked away from (to get his clothes, not to babysit
     * the dial) switched the amp off two minutes after it had finally answered,
     * so he came back to the whole two-minute wait again (5 Nov 2026). */
    clock_dark(true);
    /* Nothing is being drawn now, so the idle check itself can slow down. */
    if (s_idle_timer) lv_timer_set_period(s_idle_timer, CORE_SLEEP_CHECK_MS);
    if (s_set_asleep) s_set_asleep(true);
}

void ui_test_sleep(void)
{
    if (s_asleep) return;
    if (!clock_showing()) sleep_to_clock();
    go_to_sleep();
}

/* Straight to deep sleep, instead of two hours from now. Nothing about it can
 * be tried on the bench otherwise. */
void ui_test_deep_sleep(void)
{
    if (!s_asleep) ui_test_sleep();
    if (!np_playing() && !s_power_let_go) power_finished();  /* nothing goes under holding the amps */
    if (s_deep_sleep) s_deep_sleep();
}

static void wake(void)
{
    /* Arriving at a dial that was showing nothing of use - dark, or behind the
     * clock - is the one moment the amp is allowed a whole screen to itself.
     * See ui_amp_state. wake() is also called on every ordinary touch and
     * detent, so this has to be the coming-back case only. */
    if (s_asleep || clock_showing()) {
        s_woke_at = lv_tick_get();
        s_amp_intro_done = false;
    }
    if (s_asleep) {
        s_asleep = false;
        if (s_set_asleep) s_set_asleep(false);  /* the screen is still dark */
        clock_dark(false);
        if (s_idle_timer) lv_timer_set_period(s_idle_timer, CORE_IDLE_CHECK_MS);
    }
    clock_hide();
    ui_show(s_view);  /* back to exactly the screen that was up */
}

/* The update overlay brings the screen back itself: it is the one thing that
 * appears without anybody having touched the knob. */
void ui_force_wake(void) { wake(); }



/* ---- Whether the amp can hear (see ui_amp_state in ui.h) ---------------------- */
static ui_amp_t s_amp = UI_AMP_UNKNOWN;

ui_amp_t ui_amp(void) { return s_amp; }

/* The pill belongs to Now Playing and the tone screen, the same rule ui_show
 * uses: anywhere else it has been deliberately taken down, so raising it again
 * put "Connecting to the amp" over the menu Darren had just turned the music
 * off from (4 Nov 2026). The clock, a question and the update screen are none
 * of them the place for it either. */
static bool amp_may_say(void)
{
    /* Not over the network line either: they are the same pill in the same
     * place, and the amp cannot be reached before the network is up anyway, so
     * "Amp connecting" on top of "Connecting" would be two lines fighting over
     * one slot to say the same thing (4 Nov 2026). */
    return (s_view == UI_VIEW_NOWPLAYING || s_view == UI_VIEW_EQ) && !clock_showing() &&
           !ui_asking() && !ui_updating() && !ui_update_failed() && !ui_net_showing();
}

void ui_amp_state(ui_amp_t state, int waiting_secs)
{
    /* Not shown any more - the spinning rim says it better than a counter did -
     * but still counted: it is what bounds the screen being held on. */
    s_amp_secs = waiting_secs;
    ui_amp_t was = s_amp;
    s_amp = state;
    /* The play disc is live only when a press would really play something, and
     * the note in its place goes with it, so Now Playing has to be told. */
    if (state != was) np_amp_changed();

    /* The amp has landed. Measured on a genuinely cold start, 4 Nov 2026: the
     * amps were asked for at 6 s and the Denon answered at 132 s - and the
     * screen went off at 133. The dial waited two minutes for the one thing it
     * was waiting for, got it, and went dark within a second without saying
     * so, because the idle countdown had been running from boot the whole
     * time. So: say it, and start the countdown from there. Only while there
     * is somebody to tell - asleep or behind the clock, that is a full screen
     * for an empty room. */
    if (state == UI_AMP_READY && was == UI_AMP_CONNECTING && !s_asleep && !clock_showing())
        ui_net_amp_connected();
    /* ...and the other end of it. An amp that stops answering on a dial that
     * is already up gets the same introduction the cold start gives it, rather
     * than a blue arc appearing on the menu with nothing to explain it.
     *
     * But only on arrival, and only if he has not already started doing
     * something. Darren, 5 Nov 2026: he power-cycled, watched the whole
     * opening, landed on the menu, "and when I started to try and scroll
     * through the menu just to select which section I wanted to go into, it
     * then represented the connecting to the amplifier screen. Wasn't
     * expecting that, don't want that."
     *
     * What had happened: the amp answered, the screen went off two minutes
     * later, and the screen going off forgets the amp (amp.c - it is usually
     * powered down with the music, so a remembered "ready" would be a lie by
     * morning). His next turn woke it, the first probe found the Denon not
     * answering yet, and UNKNOWN -> CONNECTING threw a full screen over the
     * menu he was by then already scrolling.
     *
     * So: the amp may have the screen when he arrives and is still looking at
     * it, and never once his hand is moving. A finger down or a detent in the
     * last moment means he is using the dial, and the rim spinning behind what
     * he is doing is exactly what the rim is for. Once per arrival, too: it is
     * an introduction, not a running commentary. */
    if (state == UI_AMP_CONNECTING && was != UI_AMP_CONNECTING && !s_asleep && !clock_showing() &&
        !s_amp_intro_done && lv_tick_elaps(s_woke_at) < AMP_INTRO_MS && !s_touch.down &&
        lv_tick_elaps(s_last_input) >= AMP_INTRO_STILL_MS) {
        s_amp_intro_done = true;
        ui_net_amp_connecting();
    }

    /* The rim says the dial has not finished, on whatever screen he wanders
     * onto, and it outlives the connecting screen. And none of it while the
     * connecting sequence is up - the arc is that sequence's then, and it
     * stops and starts with the job on screen rather than running straight
     * through (ui_net.c).
     *
     * CONNECTING is the only state that has the arc: anything else takes it
     * down, UI_AMP_UNKNOWN included. UNKNOWN used to be left alone on the
     * grounds that an amp nobody has asked has no opinion - but UNKNOWN is
     * exactly what the dial goes back to when it sleeps (amp.c forgets what it
     * knew, because the amp is usually powered down with the music), and it is
     * also where it sits whenever the music is off, since the amp is only ever
     * asked while there is music to hear (amp_follow in app_main). So a wait
     * that was interrupted by the screen going dark left the blue arc spinning
     * with nothing behind it: it came back on the next wake, it followed him
     * onto every screen, and nothing anywhere could explain it, because as far
     * as the rest of the dial was concerned the amp was not being connected to
     * at all - no connecting screen, no "Connecting to the amp" on Now
     * Playing (Darren, 8 Nov 2026). Turning it off costs nothing at boot: the
     * arc is not armed yet, and ui_overlay_spin ignores a state it is already
     * in. */
    if (!ui_net_showing()) ui_overlay_spin(state == UI_AMP_CONNECTING, UI_LOADING);

    /* What the screen says about it is now Now Playing's job, in the middle of
     * the screen where the play disc would have been (s_amp_note there).
     * "Connecting to the amp" at 24 px is wider than the chip's pill can be on
     * a 360 px round screen and it clipped at both ends (Darren, 4 Nov 2026) -
     * the same fault the seconds on the end of it caused this morning, and the
     * second time this slot has been asked to hold a sentence. It is not a
     * place for one.
     *
     * The arrival is still worth a chip: "Connected" is three syllables, it
     * fits, and it is news rather than a state. */
    if (state == UI_AMP_READY && was == UI_AMP_CONNECTING && amp_may_say()) ui_chip("Connected");
}

/* The clock is a screensaver drawn over everything, and ui_show does not take
 * it down - only a touch or a detent does, through wake(). So a screen opened
 * from the console changed what was underneath while the glass went on showing
 * the clock, and every scripted screenshot came back as the time (2 Nov 2026).
 * Anything that drives the UI from the console now comes through here first. */
void ui_test_wake(void)
{
    wake();
}

bool ui_clock_up(void) { return clock_showing(); }

/* The scrubber is a full-screen panel inside Now Playing rather than a view of
 * its own, so a capture taken over it would otherwise be labelled "nowplaying"
 * - the same shape of wrong as a clock frame labelled "albums". */
bool ui_scrubbing(void) { return np_scrubbing(); }

/* Leaving the knob alone, in order: after eight seconds it returns to Now
 * Playing if something is playing, so the knob's resting job is always the
 * volume (decided 26 Oct 2026); after a minute the clock comes up over
 * whatever is showing, playing or not; two minutes after the last touch the
 * screen goes off; at a quarter of an hour, with nothing playing, the amps
 * are let go (CORE_AMP_LET_GO_MS); and two hours after it, the dial stops
 * properly.
 *
 * Playing used to exempt a dial from the last two of those. It no longer does:
 * the screen being off has never stopped the music, and the exemption was what
 * let one unattended stream keep a dial lit until it was flat (4 Nov 2026). */
/* Everything that counts as the dial having a job on, in ONE place, because
 * there is more than one countdown that has to ask. Music playing is not in
 * here - the two callers disagree about it, and each says so where it asks.
 * Anything added later that keeps the dial busy belongs in this list and
 * nowhere else. */
/* The part of that list which owns the screen or the hand: something is drawn
 * over the top, or a finger is down. The amp is deliberately not in it. An amp
 * that is still being found holds off the power countdowns - that is the whole
 * of the 4 Nov rule, and it stands - but it does not stop the dial coming back
 * to the music, because coming back takes nothing away and the rim goes on
 * saying the amp has not answered wherever you are.
 *
 * It matters because the bathroom Denon drops out and reconnects on its own.
 * Watched over USB on 5 Nov 2026: with the amp flapping, the amp alone held
 * every screen open for ever, and the tone screens Darren reported were
 * exactly that. The simulator could not have shown this - it has no amp. */
static bool screen_busy(void)
{
    return ui_logo_showing()   /* the opening is running */
           || ui_net_showing() /* ...or one of its screens */
           || ui_updating()    /* firmware being written */
           || ui_asking()      /* a question is up */
           || search_busy()    /* listening */
           || s_touch.down;    /* a finger is on the glass */
}

static bool dial_busy(void)
{
    return screen_busy() || ui_amp() == UI_AMP_CONNECTING;  /* ...or still finding the amp */
}

/* What the eight-second return is looking at, for the console. The dial is the
 * only place some of these are ever true - no amp, no network and no question
 * exists in the simulator - and without being able to read them, a screen that
 * will not time out can only be guessed at (5 Nov 2026). */
void ui_idle_state(bool *playing, bool *screen, bool *amp, uint32_t *still_ms)
{
    if (playing) *playing = np_playing();
    if (screen) *screen = screen_busy();
    if (amp) *amp = ui_amp() == UI_AMP_CONNECTING;
    if (still_ms) *still_ms = lv_tick_elaps(s_still);
}

static void idle_check(lv_timer_t *t)
{
    (void)t;
    /* Music *starting* anywhere brings the screen up; music still playing does
     * not. That distinction is the whole of it. This used to ask only whether
     * something was playing, and because the question came round twice a
     * second, a stream that kept running pushed the clock aside again the
     * instant it returned - so the sleep below was never reached and the dial
     * stayed fully lit until the battery was gone. It needs a stream that
     * outlives its listener to bite, which is exactly what happens when the
     * amp is powered down and the music carries on silently (4 Nov 2026). */
    bool playing = np_playing();
    bool just_started = playing && !s_was_playing;
    s_was_playing = playing;

    /* NO COUNTDOWN RUNS WHILE THE DIAL IS DOING SOMETHING.
     *
     * One rule, in one place, because it has been learned three separate times
     * in one evening and patched three separate ways: the amp landing after
     * two minutes and the screen going dark a second later; the clock arriving
     * the instant a two-track album finished; the clock coming up over a warm
     * start that was still looking for the amp. Every one of them was the same
     * thing - a countdown that had been quietly running while the dial was
     * busy, so that the moment it stopped being busy the countdown was already
     * spent.
     *
     * Darren, 4 Nov 2026, having said it before: "if the player is doing
     * something like connecting to an amplifier, connecting to the Wi-Fi,
     * playing the music, no fucking timers should be running for a fucking
     * countdown."
     *
     * So the countdown does not run. s_last_input is pushed forward for as
     * long as the dial has a job on, which means the minute to the clock and
     * the two to the screen going off both begin at the moment the last job
     * ends, whatever that job was and whether or not anybody touched the
     * glass. Anything added later that keeps the dial busy belongs in this
     * list and nowhere else. */
    bool busy = playing || dial_busy();  /* music is on, or the dial has a job */
    if (busy) s_last_input = lv_tick_get();
    /* The hand stamp takes the job but not the music: a record playing is not
     * somebody standing at the dial, and the eight seconds below is the only
     * thing that reads it. See s_still where it is declared. */
    if (screen_busy()) s_still = lv_tick_get();

    /* Writing firmware: the screen stays on and stays where it is, however
     * long it takes and whether or not anyone is in the room. */
    if (ui_updating()) {
        s_last_input = s_still = lv_tick_get();
        return;
    }
    if (clock_showing()) {
        /* The amps, a quarter of an hour after the last touch with nothing on.
         * Same stamp as the screen's countdowns, so it begins when the last job
         * ended - for a cold start, when the amp finally answered. */
        if (!just_started && !playing && !s_power_let_go &&
            lv_tick_elaps(s_last_input) >= CORE_AMP_LET_GO_MS)
            power_finished();
        if (just_started) {
            wake();  /* music from anywhere: the clock gives way to it */
        } else if (!s_asleep) {
            /* Never while something is playing. With the clock no longer
             * coming up over music (below) this cannot be reached anyway -
             * sleep only ever runs from the clock - and it is kept as the
             * second half of one rule rather than as a second rule: the screen
             * does not go dark on a record that is playing, however it got
             * here. */
            if (!playing && lv_tick_elaps(s_last_input) >= CORE_SLEEP_MS) go_to_sleep();
        } else if (s_deep_sleep && !playing && lv_tick_elaps(s_last_input) >= CORE_DEEP_SLEEP_MS) {
            /* Hours untouched: stop properly. Not while anything is playing -
             * deep sleep takes Wi-Fi and Music Assistant with it, and a dial
             * that cannot be turned down is worse than one that is merely
             * asleep. */
            s_deep_sleep();
        }
        return;
    }
    if (s_touch.down || search_busy() || ui_asking()) return;  /* no jumps while listening or asking */

    /* ---- The hand timer -------------------------------------------------
     *
     * Eight seconds since anybody touched the glass or turned the knob. This
     * is not the idle timer below and must not be confused with it: it never
     * takes the screen away, it only puts back a screen that was opened and
     * then left. Darren, 5 Nov 2026, on why they are two different things:
     * one is "a completely no activity", the other "a subactivity, but
     * nothing's happening".
     *
     * The tone screens unwind whether or not a record is on. The EQ is
     * somewhere you went into from the music, so treble, bass or subs falls
     * back to the tone overview and the overview back to Now Playing, and the
     * dial is never left sitting on a screen nobody is using.
     *
     * The lists only unwind while something is playing. With nothing playing
     * there is nothing to come back to - Now Playing would be as dead an end
     * as the list is, and the idle timer below is the right thing to take it.
     *
     * An automatic step costs the idle timer nothing: its stamp is put back
     * where it was, so the clock still arrives a minute after the last real
     * touch rather than a minute after the dial last moved itself. */
    if (lv_tick_elaps(s_still) >= CORE_IDLE_RETURN_MS) {
        uint32_t power_was = s_last_input;
        bool unwound = false;
        if (s_view == UI_VIEW_EQ) {
            unwound = eq_step_back();  /* treble, bass or subs -> the tone overview */
            if (!unwound) {            /* ...and the overview -> the music */
                /* With a record on, back to the record, wherever the EQ was
                 * opened from. With nothing on there is no playing album to
                 * ask for - np_player_album() would be -1 - so Now Playing is
                 * simply shown again, holding the album the EQ was opened over. */
                if (playing) np_open(np_player_album());
                else ui_show(UI_VIEW_NOWPLAYING);
                unwound = true;
            }
        } else if (playing && !np_showing_player() && s_view != UI_VIEW_SEARCH) {
            np_open(np_player_album());  /* reading a list or results takes a while, so not those */
            unwound = true;
        }
        if (unwound) {
            s_last_input = power_was;
            s_still = lv_tick_get();  /* the next eight seconds start here */
            return;
        }
    }

    /* ---- The idle timer --------------------------------------------------
     *
     * Nothing is happening at all: no music, no job, no hand. Only then does
     * the clock come up, and the screen go dark a while after it. Its stamp is
     * held forward for as long as the dial has anything on, so the minute
     * begins when everything has stopped - see s_last_input where it is
     * declared, and the one rule above. */
    if (!playing && lv_tick_elaps(s_last_input) >= CORE_CLOCK_IDLE_MS && !reading()) sleep_to_clock();
}

void ui_init(const ui_library_t *lib)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    menu_create(scr);
    albums_create(scr, lib);
    np_create(scr, lib);
    eq_create(scr, lib);
    search_create(scr);
    playlists_create(scr, lib);
    recent_create(scr, lib);
    settings_create(scr);
    status_create(scr);
    ui_overlay_init(scr);
    chip_create(scr);
    clock_create();
    ui_show(UI_VIEW_MENU);
    /* ...and straight over the top of it, before a single frame is drawn. The
     * dial is always connecting at this point - it has just started - and
     * waiting for the first poll to put this up meant the menu was briefly on
     * the glass first: "a momentary flash of the menu and then it went
     * straight into connecting to Wi-Fi. That seemed glitchy" (Darren, 4 Nov
     * 2026). The menu is still built underneath, ready for when this goes. */
    ui_logo_show();  /* ...and the connecting sequence behind it, once it is done */
    s_idle_timer = lv_timer_create(idle_check, CORE_IDLE_CHECK_MS, NULL);
}

/* ---- Input ------------------------------------------------------------------ */
void ui_touch_press(int x, int y)
{
    s_last_input = s_still = lv_tick_get();
    if (clock_showing()) {  /* a tap only wakes it; the rest of this touch is ignored */
        wake();
        sample_done();
        return;
    }
    power_wanted();
    s_touch.down = true;
    s_touch.moved = false;
    s_touch.x0 = x;
    s_touch.y0 = y;
    s_touch.t0 = ui_touch_sample_ms();
    s_touch.decided = false;
    s_touch.from_edge = false;
    s_touch.from_left = false;
    s_touch.from_right = false;
    s_touch.dwell_ms = 0;
    s_touch.samples = 0;
    s_touch.reach_px = 0.0f;
    s_last_input = s_still = lv_tick_get();
    off_clock_cancel();  /* still being used: the Music off clock can wait */
    if (ui_updating()) { sample_done(); return; }  /* nothing to answer, and nothing to interrupt */
    if (ui_asking()) { ui_ask_press(y); sample_done(); return; }  /* the question has the screen */
    if (s_view == UI_VIEW_MENU) menu_press();
    else if (s_view == UI_VIEW_ALBUMS) albums_press(x, y);
    else if (s_view == UI_VIEW_NOWPLAYING) np_press(x, y);
    else if (s_view == UI_VIEW_SEARCH) search_press(y);
    else if (s_view == UI_VIEW_PLAYLISTS) playlists_press(y);
    else if (s_view == UI_VIEW_RECENT) recent_press(y);
    sample_done();
}

void ui_touch_move(int x, int y)
{
    if (!s_touch.down) { sample_done(); return; }
    float dx = (float)(x - s_touch.x0), dy = (float)(y - s_touch.y0);
    float reach = sqrtf(dx * dx + dy * dy);
    s_touch.samples++;
    if (reach > s_touch.reach_px) s_touch.reach_px = reach;
    if (core_is_drag(dx, dy)) s_touch.moved = true;
    if (!s_touch.decided && s_touch.moved) {
        /* when it first moved, which is all the dwell is: see ui_touch_from_edge */
        s_touch.decided = true;
        s_touch.dwell_ms = lv_tick_diff(ui_touch_sample_ms(), s_touch.t0);
    }
    /* Has it come in off an edge? Asked on every sample once the swipe has
     * gone far enough to mean anything, and only ever answered yes: a gesture
     * that does not look like one yet may still become one. A finger rolling
     * in over the top right of a round screen does exactly that - it travels
     * sideways along the rim first and only then turns in - which cost nine of
     * Darren's twenty-two swipes when this was decided once and for all at the
     * 5 px where a touch first counts as a drag (30 Oct 2026).
     *
     * What is asked is how much of the travel is away from the edge it came
     * over, which is the thing that separates coming in over the bezel from
     * sliding along it. Asking instead whether it heads for the middle of the
     * screen looked equivalent and is not: from a point low down the left rim
     * the middle is down and to the right, so an ordinary scroll down the left
     * of the screen counted as inward and threw you out of the screen. */
    if (s_touch.decided && s_touch.dwell_ms <= CORE_EDGE_DWELL_MS && reach >= CORE_EDGE_TRAVEL_PX) {
        if (s_touch.y0 <= CORE_EDGE_PX && dy >= CORE_EDGE_INWARD * reach) s_touch.from_edge = true;
        if (s_touch.x0 <= CORE_EDGE_PX && dx >= CORE_EDGE_INWARD * reach) s_touch.from_left = true;
        if (s_touch.x0 >= CORE_SCREEN - 1 - CORE_EDGE_PX && -dx >= CORE_EDGE_INWARD * reach)
            s_touch.from_right = true;
    }
    s_last_input = s_still = lv_tick_get();
    if (ui_updating()) { sample_done(); return; }
    if (ui_asking()) { ui_ask_drag(dx, dy); sample_done(); return; }
    if (s_view == UI_VIEW_MENU) menu_drag(dy);
    else if (s_view == UI_VIEW_ALBUMS) albums_drag(dx, dy);
    else if (s_view == UI_VIEW_NOWPLAYING) np_drag(dx, dy);
    else if (s_view == UI_VIEW_SEARCH) search_drag(dx, dy);
    else if (s_view == UI_VIEW_PLAYLISTS) playlists_drag(dx, dy);
    else if (s_view == UI_VIEW_RECENT) recent_drag(dx, dy);
    sample_done();
}

void ui_touch_release(int x, int y)
{
    if (!s_touch.down) { sample_done(); return; }
    s_touch.down = false;
    s_last_input = s_still = lv_tick_get();
    float dx = (float)(x - s_touch.x0), dy = (float)(y - s_touch.y0);
    if (ui_updating()) { sample_done(); return; }
    if (ui_asking()) {
        ui_ask_release(dx, dy, s_touch.moved);
        sample_done();
        return;
    }
    switch (s_view) {
    case UI_VIEW_MENU: menu_release(dy, s_touch.moved); break;
    case UI_VIEW_ALBUMS: albums_release(dx, dy, s_touch.moved); break;
    case UI_VIEW_NOWPLAYING: np_release(x, y, dx, dy, s_touch.moved); break;
    case UI_VIEW_EQ: eq_release(x, y, dx, dy, s_touch.moved); break;
    case UI_VIEW_SEARCH: search_release(x, y, dx, dy, s_touch.moved); break;
    case UI_VIEW_PLAYLISTS: playlists_release(dx, dy, s_touch.moved); break;
    case UI_VIEW_RECENT: recent_release(dx, dy, s_touch.moved); break;
    case UI_VIEW_SETTINGS: settings_release(dx, dy, s_touch.moved); break;
    case UI_VIEW_STATUS: status_release(dx, dy, s_touch.moved); break;
    }
    sample_done();
}

/* The knob moves through whatever is on screen; on Now Playing while music
 * plays, it's the volume (decided 26 Oct 2026). */
void ui_knob(int steps)
{
    if (!steps) return;
    s_last_input = s_still = lv_tick_get();
    off_clock_cancel();
    if (ui_updating()) return;  /* the knob does nothing while firmware is being written */
    if (clock_showing()) { wake(); return; }  /* a turn only wakes it */
    power_wanted();
    if (ui_asking()) {
        if (ui_ask_takes_knob()) { ui_ask_knob(steps); return; }
        ui_ask_keepalive();  /* a one-answer question leaves the knob to the screen under it */
    }
    switch (s_view) {
    case UI_VIEW_MENU: menu_knob(steps); break;
    case UI_VIEW_ALBUMS: albums_knob(steps); break;
    case UI_VIEW_NOWPLAYING: np_knob(steps); break;
    case UI_VIEW_EQ: eq_knob(steps); break;
    case UI_VIEW_SEARCH: search_knob(steps); break;
    case UI_VIEW_PLAYLISTS: playlists_knob(steps); break;
    case UI_VIEW_RECENT: recent_knob(steps); break;
    /* Settings and Connections are two pages of one place: a turn moves between
     * them (Darren, 16 Nov 2026), with the rim showing which. */
    case UI_VIEW_SETTINGS: settings_page_knob(steps, SETTINGS_PAGE_SETTINGS); break;
    case UI_VIEW_STATUS: settings_page_knob(steps, SETTINGS_PAGE_STATUS); break;
    }
}
