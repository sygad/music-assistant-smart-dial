/* Screen 03: Album browse, A-Z by album title.
 *
 * Swipe or turn the knob: one album, at whatever speed you like. A flick
 * glides albums, more the harder it is (2-8).
 *
 * The letter you are on sits in a badge at the top, and tapping it is the way
 * into letters: while it is up, a turn or a swipe steps one letter at a time,
 * and it drops back to albums shortly after you stop, on the first album of
 * the letter you left it on.
 *
 * Darren, 28 Oct 2026: letters used to be entered by how fast you turned or
 * flicked, and no threshold ever worked - the opening detents of a long fast
 * spin are the same as a flick, so it kept jumping letters when he only
 * wanted to scroll, and judging the turn once it ended made it late instead.
 * He also had no way of telling when the albums had crossed into a new
 * letter. One badge answers both: it says where you are, and tapping it says
 * what you want, so nothing has to guess at intent. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "ui_strip.h"

#define MARKER   0xFFFFFF  /* only used to clear the rim now */

static const ui_library_t *s_lib;
static lv_obj_t *s_root, *s_letter_view, *s_letter, *s_loading;
static lv_obj_t *s_badge, *s_badge_label;  /* the letter you are on, and the way into letters */
static strip_t *s_strip;
static uint32_t s_slide_ms = CORE_ALBUM_SLIDE_MS;
static uint32_t s_tau_ms = 450;     /* album glide time constant */
static float s_fling_min = 0.4f;    /* px/ms: slower than this pages by distance */
static float s_letter_v = 2.5f;     /* px/ms: faster than this flicks through letters */
static float s_carry;
static bool s_pressed_while_moving;
static float s_press_pos;

/* Letters: the first album of each letter, rebuilt when the library grows */
static int s_letter_first[27];
static char s_letter_char[27];
static int s_nletters;

/* Letters: entered by tapping the badge, left shortly after the input stops */
static bool s_letters;
static lv_timer_t *s_letters_rest;
static bool s_badge_tap;       /* this touch started on the badge */
static uint32_t s_letter_at;   /* when a letter last stepped: see CORE_LETTER_STEP_MS */

/* Spinning hard is the second way into letters, alongside tapping the badge.
 * It is only a way in: once there it behaves exactly as the tap does, a letter
 * per detent at whatever speed suits, and it drops back to the albums when the
 * turning stops. Darren, 29 Oct 2026: "it was only ever about getting into
 * that mode - I don't want to maintain that spin level". */
#define TURNS 48               /* detent times kept: 1.5 s at the fastest he spins */
static uint32_t s_turn[TURNS];
static int s_turn_at;

static float turn_rate(void)
{
    uint32_t now = lv_tick_get();
    int n = 0;
    for (int i = 0; i < TURNS; i++)
        if (s_turn[i] && lv_tick_diff(now, s_turn[i]) <= CORE_FLY_WINDOW_MS) n++;
    return n * 1000.0f / CORE_FLY_WINDOW_MS;
}

/* ---- Flick recording, for tuning to a real finger ------------------------ */
#define REC_GESTURES 10
#define REC_SAMPLES 40
typedef struct {
    int n;
    uint16_t t[REC_SAMPLES];   /* ms since press */
    int16_t dx[REC_SAMPLES];
    uint16_t lift_ms;          /* release time since press */
    float speed;               /* px/ms as measured at release */
    char outcome[24];
} rec_t;
UI_IN_PSRAM static rec_t s_rec[REC_GESTURES];
static int s_rec_next;
static rec_t *s_rec_cur;
static uint32_t s_press_t;

/* Finger samples for the release speed (press included, at dx 0) */
#define SAMPLES 24
static struct { uint32_t t; float dx; } s_samples[SAMPLES];
static int s_nsamples;

void ui_set_slide_ms(uint32_t ms) { s_slide_ms = ms < 50 ? 50 : ms > 1000 ? 1000 : ms; }
uint32_t ui_slide_ms(void) { return s_slide_ms; }
void ui_set_fling(uint32_t tau_ms, float min_speed, float letter_speed)
{
    s_tau_ms = tau_ms < 50 ? 50 : tau_ms;
    s_fling_min = min_speed;
    s_letter_v = letter_speed;
}
void ui_get_fling(uint32_t *tau_ms, float *min_speed, float *letter_speed)
{
    *tau_ms = s_tau_ms;
    *min_speed = s_fling_min;
    *letter_speed = s_letter_v;
}

static int count(void) { return s_lib->count(); }

/* ---- Letters ------------------------------------------------------------------ */
static void rebuild_letters(void)
{
    s_nletters = 0;
    char prev = 0;
    for (int i = 0; i < count(); i++) {
        char c = s_lib->letter(i);
        if (c != prev && s_nletters < 27) {
            s_letter_char[s_nletters] = c;
            s_letter_first[s_nletters++] = i;
            prev = c;
        }
    }
}

static int letter_index_of(int album)
{
    int k = 0;
    while (k + 1 < s_nletters && s_letter_first[k + 1] <= album) k++;
    return k;
}

static void show_letters(bool on)
{
    if (on) lv_obj_remove_flag(s_letter_view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_letter_view, LV_OBJ_FLAG_HIDDEN);
}

static void set_letter_k(int k)
{
    char t[2] = {s_letter_char[k], 0};
    if (lv_label_get_text(s_letter)[0] != t[0]) lv_label_set_text(s_letter, t);
}

/* ---- The letter badge ---------------------------------------------------------- */
#define BADGE_W 80
#define BADGE_H 50
#define BADGE_Y 18
/* Wider than it looks, so a thumb finds it: the whole top of the screen above
 * the cover is the badge as far as a touch is concerned. */
#define BADGE_HIT_W 150
#define BADGE_HIT_H 104

static bool on_badge(int x, int y)
{
    return s_nletters > 0 && !s_letters && abs(x - CORE_SCREEN / 2) <= BADGE_HIT_W / 2 && y <= BADGE_HIT_H;
}

static void set_badge(int album)
{
    if (s_nletters <= 0) return;
    char t[2] = {s_letter_char[letter_index_of(album)], 0};
    if (lv_label_get_text(s_badge_label)[0] != t[0]) lv_label_set_text(s_badge_label, t);
}

static void show_badge(bool on)
{
    if (on) lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
}

/* ---- Rim marker ---------------------------------------------------------------- */
/* No rim marker while browsing albums (Darren, 28 Oct 2026): the rim belongs
 * to what is inside a thing you have chosen - the tracks on an album, or the
 * tracks on a playlist, both of which Now Playing already shows - not to where
 * you are in the library. Leaving Now Playing clears whatever it left there. */
static void clear_rim(void) { ui_overlay_arc(false, MARKER, 0, 0); }

/* ---- Strip callbacks ----------------------------------------------------------- */
static int src_count(void *ctx) { (void)ctx; return count(); }
static const char *src_title(void *ctx, int i) { (void)ctx; return s_lib->title(i); }
static const char *src_sub(void *ctx, int i) { (void)ctx; return s_lib->artist(i); }
static const lv_image_dsc_t *src_cover(void *ctx, int i) { (void)ctx; return s_lib->cover(i); }

static void src_moved(void *ctx, float pos, float speed)
{
    (void)ctx;
    s_lib->focus(pos, speed);
    set_badge((int)lroundf(pos));
}

/* ---- Letters: one step per knob click or fast flick ------------------------------ */
static void letters_end(void)
{
    lv_timer_pause(s_letters_rest);
    if (!s_letters) return;
    s_letters = false;
    show_letters(false);  /* the covers of the letter's first albums are already loading behind */
    set_badge(strip_target(s_strip));
}

static void letters_rest(lv_timer_t *t)
{
    (void)t;
    letters_end();
}

/* Into letters, on the letter already on screen: nothing moves until a turn
 * or a swipe asks for a different one. */
static void letters_show(void)
{
    if (!s_nletters) return;
    s_letters = true;
    strip_stop(s_strip);
    set_letter_k(letter_index_of(strip_target(s_strip)));
    show_letters(true);
    ui_tick();
    lv_timer_set_period(s_letters_rest, CORE_LETTER_OPEN_MS);
    lv_timer_reset(s_letters_rest);
    lv_timer_resume(s_letters_rest);
}

static void letters_step(int steps, uint32_t rest_ms)
{
    if (!s_nletters) return;
    if (!s_letters) {
        s_letters = true;
        strip_stop(s_strip);
    }
    int k = letter_index_of(strip_target(s_strip)) + steps;
    k = k < 0 ? 0 : k >= s_nletters ? s_nletters - 1 : k;
    int album = s_letter_first[k];
    strip_jump(s_strip, (float)album);
    set_letter_k(k);
    show_letters(true);
    ui_tick();
    lv_timer_set_period(s_letters_rest, rest_ms);
    lv_timer_reset(s_letters_rest);
    lv_timer_resume(s_letters_rest);
}

/* ---- View ------------------------------------------------------------------------ */
void albums_create(lv_obj_t *parent, const ui_library_t *lib)
{
    s_lib = lib;
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);  /* no fill: the screen behind is black */

    strip_src_t src = {.count = src_count, .title = src_title, .sub = src_sub, .cover = src_cover, .moved = src_moved};
    s_strip = strip_create(s_root, &src);

    s_loading = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_loading, &font_inter_regular_24, 0);
    lv_obj_set_style_text_color(s_loading, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_loading, "Loading albums");
    lv_obj_center(s_loading);

    /* The letter badge (not in the design): the same black 80% as the title band */
    s_badge = ui_plain(s_root);
    lv_obj_set_pos(s_badge, (CORE_SCREEN - BADGE_W) / 2, BADGE_Y);
    lv_obj_set_size(s_badge, BADGE_W, BADGE_H);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_badge, UI_OPA_SCRIM, 0);
    lv_obj_set_style_radius(s_badge, BADGE_H / 2, 0);
    s_badge_label = lv_label_create(s_badge);
    lv_obj_set_style_text_font(s_badge_label, &font_inter_light_36, 0);
    lv_obj_set_style_text_color(s_badge_label, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_badge_label, "A");
    lv_obj_center(s_badge_label);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);  /* until the letters are known */

    /* Letter view (not in the design): the big letter in the clock's thin Inter, on black */
    s_letter_view = ui_plain(s_root);
    lv_obj_set_size(s_letter_view, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_letter_view, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_letter_view, LV_OPA_COVER, 0);
    s_letter = lv_label_create(s_letter_view);
    lv_obj_set_style_text_font(s_letter, &font_inter_extralight_160, 0);
    lv_obj_set_style_text_color(s_letter, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_letter, "A");
    lv_obj_center(s_letter);
    lv_obj_add_flag(s_letter_view, LV_OBJ_FLAG_HIDDEN);

    s_letters_rest = lv_timer_create(letters_rest, CORE_KNOB_REST_MS, NULL);
    lv_timer_pause(s_letters_rest);
}

lv_obj_t *albums_root(void) { return s_root; }
int albums_current(void) { return strip_target(s_strip); }
bool albums_flying(void) { return s_letters; }  /* the test harness asks what is on screen */
float albums_turn_rate(void) { return turn_rate(); }

void ui_library_changed(void)
{
    rebuild_letters();
    if (count() > 0) lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
    show_badge(s_nletters > 0);
    set_badge(strip_target(s_strip));
    strip_refresh(s_strip, -1);
}

void albums_cover_changed(int album) { strip_refresh(s_strip, album); }

void ui_goto_album(int index)
{
    strip_jump(s_strip, (float)index);
    ui_show(UI_VIEW_ALBUMS);
}

void albums_enter(void)
{
    letters_end();  /* never come back to the screen with the big letter still up */
    if (!strip_moving(s_strip)) strip_jump(s_strip, (float)strip_target(s_strip));
    clear_rim();
}

/* ---- Touch ------------------------------------------------------------------------ */
static void sample(float dx)
{
    if (s_nsamples == SAMPLES) {
        for (int i = 1; i < SAMPLES; i++) s_samples[i - 1] = s_samples[i];
        s_nsamples--;
    }
    s_samples[s_nsamples].t = ui_touch_sample_ms();
    s_samples[s_nsamples].dx = dx;
    s_nsamples++;
}

void albums_press(int x, int y)
{
    /* Only note where the touch started. Opening the letters here meant a
     * swipe that began at the top of the screen - which is how a swipe down
     * to the menu naturally starts - was swallowed by the badge, and there
     * was no way back out of Albums (Darren, 29 Oct 2026). A drag that starts
     * on the badge is just a drag; only a tap opens the letters. */
    s_badge_tap = on_badge(x, y);
    s_pressed_while_moving = strip_moving(s_strip);
    s_carry = strip_moving(s_strip) ? strip_speed(s_strip) : 0.0f;
    strip_stop(s_strip);  /* a touch stops a glide where it is */
    s_press_pos = strip_pos(s_strip);
    s_nsamples = 0;
    s_press_t = ui_touch_sample_ms();
    sample(0);
    s_rec_cur = &s_rec[s_rec_next];
    s_rec_next = (s_rec_next + 1) % REC_GESTURES;
    s_rec_cur->n = 0;
    s_rec_cur->outcome[0] = 0;
}

void albums_drag(float dx, float dy)
{
    (void)dy;
    if (!count() || s_letters) return;  /* the strip is stopped while letters are up */
    if (ui_touch_left_edge_pending()) return;  /* leaving the screen, not moving an album */
    float before = strip_pos(s_strip);
    uint32_t dt = lv_tick_diff(ui_touch_sample_ms(), s_samples[s_nsamples - 1].t);
    strip_drag(s_strip, s_press_pos - dx / CORE_SCREEN);
    s_lib->focus(strip_pos(s_strip), dt ? (strip_pos(s_strip) - before) * 1000.0f / dt : 0.1f);
    sample(dx);
    if (s_rec_cur && s_rec_cur->n < REC_SAMPLES) {
        s_rec_cur->t[s_rec_cur->n] = (uint16_t)lv_tick_diff(ui_touch_sample_ms(), s_press_t);
        s_rec_cur->dx[s_rec_cur->n] = (int16_t)dx;
        s_rec_cur->n++;
    }
}

static void record(const char *outcome)
{
    if (!s_rec_cur) return;
    s_rec_cur->lift_ms = (uint16_t)lv_tick_diff(ui_touch_sample_ms(), s_press_t);
    s_rec_cur->speed = s_rec_cur->lift_ms ? s_rec_cur->n ? fabsf((float)s_rec_cur->dx[s_rec_cur->n - 1]) / s_rec_cur->lift_ms : 0 : 0;
    snprintf(s_rec_cur->outcome, sizeof s_rec_cur->outcome, "%s", outcome);
}

/* A single flick: glide 2-8 albums, more the harder, adding to a glide that's
 * still moving the same way. */
static void album_flick(float dx, float speed)
{
    int n = core_flick_albums(speed);
    float dist = (float)(dx < 0 ? n : -n);
    if (s_carry * dist > 0) dist += s_carry * s_tau_ms;
    int target = (int)lroundf(strip_pos(s_strip) + dist);
    target = target < 0 ? 0 : target >= count() ? count() - 1 : target;
    char o[24];
    snprintf(o, sizeof o, "flick %+d", target - (int)lroundf(s_press_pos));
    record(o);
    s_lib->landing(target);
    strip_glide(s_strip, target, s_tau_ms);
}

void albums_release(float dx, float dy, bool moved)
{
    if (!count()) return;
    uint32_t down_ms = lv_tick_diff(ui_touch_sample_ms(), s_press_t);
    bool badge = s_badge_tap;
    s_badge_tap = false;
    if (!moved) {
        if (badge) { record("letters"); letters_show(); return; }
        if (s_letters) {
            lv_timer_set_period(s_letters_rest, CORE_LETTER_SWIPE_MS);  /* a finger: allow for the next swipe */
            lv_timer_reset(s_letters_rest);
            return;
        }
        record(s_pressed_while_moving ? "stopped" : "tap");
        if (s_pressed_while_moving) strip_snap(s_strip, (int)lroundf(strip_pos(s_strip)), s_slide_ms);
        else np_open_from_albums(strip_target(s_strip));
        return;
    }
    /* The way out of Albums is checked before anything else, so it is there
     * whatever else is on screen. Either way out: the swipe down, and the
     * second one in over the left bezel (ui_touch_back_gesture in ui.h),
     * which has to come first because it is a sideways gesture and sideways on
     * this screen otherwise moves an album. */
    if (ui_touch_back_gesture(dx, dy, moved) ||
        core_album_release(dx, dy) == CORE_ALBUM_TO_MENU) {
        record("to menu");
        letters_end();
        strip_jump(s_strip, (float)lroundf(strip_pos(s_strip)));
        ui_show(UI_VIEW_MENU);
        return;
    }
    /* Letters are up: one letter a swipe, however hard it was thrown. */
    if (s_letters) {
        if (fabsf(dx) >= fabsf(dy) && fabsf(dx) >= CORE_FLICK_MIN_PX) {
            letters_step(dx < 0 ? 1 : -1, CORE_LETTER_SWIPE_MS);
        } else {
            lv_timer_set_period(s_letters_rest, CORE_LETTER_SWIPE_MS);
            lv_timer_reset(s_letters_rest);
        }
        return;
    }
    if (down_ms < CORE_FLICK_MS && fabsf(dx) >= CORE_FLICK_MIN_PX && fabsf(dx) >= fabsf(dy)) {
        album_flick(dx, fabsf(dx) / (float)(down_ms < 20 ? 20 : down_ms));
        return;
    }
    /* a swipe: one album, as the design */
    int target;
    switch (core_album_release(dx, dy)) {
    case CORE_ALBUM_NEXT: target = (int)lroundf(s_press_pos) + 1; break;
    case CORE_ALBUM_PREV: target = (int)lroundf(s_press_pos) - 1; break;
    default: target = (int)lroundf(strip_pos(s_strip)); break;
    }
    record(target == (int)lroundf(s_press_pos) ? "stay" : "swipe");
    strip_snap(s_strip, target, s_slide_ms);
}

void ui_test_fling(float v)
{
    /* test: a single flick at speed v px/ms (forwards for negative v); |v| >= 10 steps a letter */
    if (!count()) return;
    s_carry = 0;
    s_press_pos = strip_pos(s_strip);
    float dx = v < 0 ? -1.0f : 1.0f;
    if (fabsf(v) >= 10.0f) letters_step(dx < 0 ? 1 : -1, CORE_FLICK_REST_MS);
    else album_flick(dx * 80.0f, fabsf(v));
}

/* ---- Knob ------------------------------------------------------------------------- */
/* The detents have stopped, so the turn they formed can be judged: short and
 * fast is a flick, and jumps a letter per detent; anything longer was a swipe
 * and has already scrolled the albums it asked for. */
void albums_knob(int steps)
{
    if (!count() || !steps) return;
    int dir = steps > 0 ? 1 : -1;
    if (s_letters) {  /* already there, however he got there: a detent is a letter */
        uint32_t now = lv_tick_get();
        if (lv_tick_diff(now, s_letter_at) < CORE_LETTER_STEP_MS) {
            lv_timer_reset(s_letters_rest);  /* still turning, so stay in letters */
            return;
        }
        s_letter_at = now;
        letters_step(dir, CORE_KNOB_REST_MS);
        return;
    }
    s_turn[s_turn_at] = lv_tick_get();
    s_turn_at = (s_turn_at + 1) % TURNS;
    if (turn_rate() >= CORE_FLY_IN) {
        for (int i = 0; i < TURNS; i++) s_turn[i] = 0;  /* a fresh spin is needed to come back */
        letters_show();
        s_letter_at = lv_tick_get();
        letters_step(dir, CORE_KNOB_REST_MS);  /* and this detent is the first letter */
        return;
    }
    strip_snap(s_strip, strip_target(s_strip) + steps, s_slide_ms);
}

/* ---- Recorded flicks, for the console ---------------------------------------- */
void ui_dump_flicks(void (*print)(const char *line))
{
    char line[220];
    for (int g = 0; g < REC_GESTURES; g++) {
        rec_t *r = &s_rec[(s_rec_next + g) % REC_GESTURES];
        if (!r->outcome[0]) continue;
        int n = snprintf(line, sizeof line, "FLICK %-11s speed %.2f px/ms, %d samples, lifted at %u ms:",
                         r->outcome, r->speed, r->n, r->lift_ms);
        for (int i = 0; i < r->n && n < (int)sizeof line - 12; i++)
            n += snprintf(line + n, sizeof line - n, " %u:%d", r->t[i], r->dx[i]);
        print(line);
    }
}
