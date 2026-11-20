/* The power-on logo: "15 Peak Hold" from the Rotary Dial Media design file,
 * animated as a graphic equalizer settling into itself (Darren, 4 Nov 2026).
 *
 * It is not only decoration, and it is worth being honest about what it is
 * for. The amplifier takes up to two minutes to answer after it is switched
 * on and nothing the dial does can hurry it. What the dial can do is spend the
 * first seconds of that wait on something that looks like progress instead of
 * on a blank screen: a second and a half of logo, then Wi-Fi, then Music
 * Assistant, then Home Assistant, then the ticks. By the time Darren is
 * choosing a track he is three seconds in and the amp is three seconds closer,
 * "shortening the reality of the time it takes to connect to the amp from a
 * perception point of view". The steps are all real; the sequence just insists
 * on showing each of them (ui_net.c).
 *
 * The artwork is eight rounded bars in a 214x150 box at (73, 63), each with a
 * peak-hold cap above it, and ZEMVO underneath (the design file says MUSARO;
 * Darren renamed it on 4 Nov 2026) - geometry and colours taken
 * from the design's own SVG rather than eyeballed. It is drawn rather than
 * imported because it has to move: the bars dance and the caps hold the
 * highest point each bar has reached, which is what a peak-hold meter does and
 * what no static asset can do.
 *
 * It lands on the design exactly. The dance runs for DANCE_MS and then
 * everything eases to the published heights, so the last thing on screen
 * before the connecting sequence is the logo as drawn, not wherever the
 * animation happened to be.
 */
#include <math.h>

#include "ui_internal.h"

/* Doubled from 1500, and slowed rather than merely stretched. At a second and
 * a half it was over before it registered, and the bars moved fast enough to
 * read as agitated: "a little bit too jumpy, a little bit too hyperactive, not
 * something that exudes quality... I want it to feel luxurious and classy"
 * (Darren, 4 Nov 2026). Length alone would not have fixed that - a hyperactive
 * animation held twice as long is just twice as much of it - so the rates are
 * roughly halved and the envelope rises and settles gently at both ends. */
/* Longer, because it has a job beyond being pretty: "I also want it to be
 * displayed long enough for people to go, oh, yeah, that's music. I get it
 * now, yeah" (Darren, 4 Nov 2026). */
#define LOGO_MS   4500
/* A quarter still, a half moving, a quarter coming to rest.
 *
 * The four parts were picked one at a time and the shape that came out was
 * lopsided - a twentieth of it still, seven tenths of it dancing, the rest
 * landing: "The bell curve isn't evenly distributed. Very short, very short
 * start, very short end, very long in the middle... Maybe something like one
 * quarter, one half, one quarter in terms of the time" (Darren, 4 Nov 2026).
 *
 * So they are fractions of the whole now rather than four independent numbers.
 * Changing LOGO_MS keeps the shape; changing one of these on its own is the
 * mistake this replaces. */
#define INTRO_MS  (LOGO_MS / 4)   /* still, before anything moves */
#define DANCE_MS  (LOGO_MS / 2)   /* the equalizer... */
#define DWELL_MS  300             /* ...and the finished design, held at the end */
#define LAND_MS   (LOGO_MS - INTRO_MS - DANCE_MS - DWELL_MS)
                         /* ...and the rest is holding it there, so the thing
                          * the logo is FOR is actually seen. Landing on the
                          * last frame before it disappears would mean the
                          * design itself never appeared. */
#define STEP_MS   30

/* The design, at 360x360: the bar box at (73, 63), 214 x 150. */
#define BOX_X     73
#define BOX_Y     63
#define BOX_H     150
#define BARS      8
#define BAR_W     18
#define BAR_PITCH 28
#define BAR_R     9
#define CAP_H     6
#define CAP_R     3
#define CAP_GAP   9     /* the cap sits this far above the bar's top */
#define CAP_OPA   140   /* the design's 55% */
#define BAR_MIN   8     /* a bar never disappears entirely */

/* Final heights, read off the SVG: 150 minus each bar's top (120, 92, 58, 38,
 * 64, 86, 106, 124). */
static const int FINAL[BARS] = {30, 58, 92, 112, 86, 64, 44, 26};
static const uint32_t COLOUR[BARS] = {
    0xFF2D78, 0xFF00FB, 0x4D7DFF, 0x00C8FF, 0x00E58F, 0xECD613, 0xFF8A1E, 0xE2402C,
};
/* A spectrum analyser, not a row of bars growing into place.
 *
 * What it did before was rise and settle, and Darren called it: "they just
 * sort of grow to a position and stay there. That doesn't really represent the
 * fidelity of the music playing across the equalizer." A meter does not grow.
 * It jumps, drops, overshoots and disagrees with itself across the band, and
 * the caps hang in the air over the bars that have fallen away - which is the
 * one thing that says "music" without a note being played.
 *
 * So each bar is three sine components beating against each other at rates
 * that share no common multiple, plus a pulse that lifts the whole band every
 * half second or so. Left is bass and moves slowly and widely; right is treble
 * and flickers. Nothing above about 6 Hz, because the bars are redrawn 30
 * times a second and faster than that aliases into noise rather than reading
 * as fast. */
/* Knocked back about a fifth from where they started: "the animation is just
 * a little bit too quick in terms of the music equalizer going up and down. A
 * fraction slower on that" (Darren, 4 Nov 2026). */
static const float F1[BARS] = {0.95f, 1.22f, 1.50f, 1.78f, 2.05f, 2.33f, 2.60f, 2.92f};
static const float F2[BARS] = {2.05f, 2.45f, 2.92f, 3.32f, 3.71f, 4.11f, 4.42f, 4.74f};
static const float F3[BARS] = {0.40f, 0.49f, 0.58f, 0.68f, 0.77f, 0.83f, 0.88f, 0.95f};
#define BEAT_HZ   1.45f  /* the pulse through the whole band */
static const float PHASE[BARS] = {0.0f, 2.1f, 4.0f, 1.2f, 5.4f, 3.3f, 0.7f, 2.8f};
static const float PHASE2[BARS] = {1.7f, 4.9f, 0.4f, 3.6f, 2.2f, 5.8f, 1.1f, 4.4f};
static const float PHASE3[BARS] = {3.1f, 0.8f, 5.2f, 2.5f, 4.6f, 1.4f, 3.9f, 0.2f};

static lv_obj_t *s_panel, *s_bar[BARS], *s_cap[BARS], *s_word;
static lv_timer_t *s_timer;
static uint32_t s_from;
static float s_peak[BARS];       /* what the caps are holding, in pixels */
static float s_peak_land[BARS];  /* ...and where each one was when the landing began */
/* Running, and on screen. They are not the same thing: when the animation
 * finishes the logo stays up at full opacity while the connecting screen fades
 * in over the top of it, and is only taken away once it is covered. Hiding it
 * the instant it finished is what made the Wi-Fi screen look like it had "car
 * crashed into it" (Darren, 4 Nov 2026). */
static bool s_up;
static bool s_visible;

bool ui_logo_showing(void) { return s_up; }

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, int r, uint32_t colour, lv_opa_t opa)
{
    lv_obj_t *o = ui_plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    return o;
}

static void build(void)
{
    if (s_panel) return;
    s_panel = ui_plain(lv_layer_top());
    lv_obj_set_size(s_panel, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < BARS; i++) {
        int x = BOX_X + i * BAR_PITCH;
        /* Both start at the bottom and grow upwards, so y is set each step. */
        s_bar[i] = block(s_panel, x, BOX_Y + BOX_H - BAR_MIN, BAR_W, BAR_MIN, BAR_R, COLOUR[i], LV_OPA_COVER);
        s_cap[i] = block(s_panel, x, BOX_Y + BOX_H, BAR_W, CAP_H, CAP_R, COLOUR[i], CAP_OPA);
    }

    /* MUSARO, at the design's 232.5 with the tracking it is drawn with. */
    s_word = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_word, &font_inter_light_28, 0);
    lv_obj_set_style_text_color(s_word, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_text_letter_space(s_word, 9, 0);
    lv_label_set_text(s_word, "ZEMVO");
    lv_obj_align(s_word, LV_ALIGN_TOP_MID, 4, 233);  /* +4: the tracking hangs off the last letter */
}

/* The dance: a rising envelope with a decaying wobble on top, so the bars come
 * up fast, overshoot, and are nearly settled by the time the landing takes
 * over. Each bar has its own rate and phase or they move as one block, which
 * reads as a wobble rather than as a meter. */
/* Time into the animation proper: zero for the whole of the intro, so the
 * first thing on screen is the logo at rest rather than the logo mid-move. */
static uint32_t danced(uint32_t ms) { return ms <= INTRO_MS ? 0 : ms - INTRO_MS; }

static float dance(int i, uint32_t ms)
{
    float t = ms / 1000.0f;
    float u = (float)ms / DANCE_MS;
    if (u > 1.0f) u = 1.0f;

    /* The music coming up, over the first part of a second. Everything below
     * is multiplied by it, so every bar starts on the floor - added instead,
     * each bar's first frame was its own phase offset and the logo looked like
     * it had been running before you looked at it (Darren, 4 Nov 2026). */
    float in = u < 0.12f ? u / 0.12f : 1.0f;
    in = in * in * (3.0f - 2.0f * in);

    const float TAU = 6.2831853f;
    float v = 0.46f + 0.30f * sinf(TAU * F1[i] * t + PHASE[i])
                    + 0.16f * sinf(TAU * F2[i] * t + PHASE2[i])
                    + 0.14f * sinf(TAU * F3[i] * t + PHASE3[i]);
    /* The pulse: short lifts through the whole band rather than a hum, so it
     * reads as a beat and not as everything breathing together. */
    float p = sinf(TAU * BEAT_HZ * t * 0.5f);
    if (p < 0.0f) p = 0.0f;
    v += 0.20f * p * p * p * p;

    /* Each bar swings around its published height rather than creeping up to
     * it, and the tall ones are allowed to hit the ceiling, which is what a
     * meter does when the music is loud. */
    float h = FINAL[i] * 1.55f * v * in;
    if (h < BAR_MIN) h = BAR_MIN;
    if (h > BOX_H) h = BOX_H;
    return h;
}

/* Where bar i should be at this moment. Up to DANCE_MS it is a meter reacting
 * to music that is not there; after that it walks from wherever the dance left
 * it onto the published height, so the last frame is the design exactly. */
static float level(int i, uint32_t ms)
{
    uint32_t t = danced(ms);
    if (t < DANCE_MS) return dance(i, t);
    float u = (float)(t - DANCE_MS) / LAND_MS;
    if (u > 1.0f) u = 1.0f;
    float ease = 1.0f - (1.0f - u) * (1.0f - u);
    float was = dance(i, DANCE_MS);
    return was + (FINAL[i] - was) * ease;
}

/* ~45 px a second: half the old rate, so a cap hangs in the air noticeably
 * longer above a bar that has dropped away from under it. During the landing
 * the caps are eased onto the design instead of falling, so a slow fall cannot
 * leave one stranded above its bar when the animation ends. */
#define PEAK_FALL_PX_PER_STEP (0.045f * STEP_MS)

/* The cap for bar i. While the bars are dancing it is a peak-hold meter:
 * straight up with the bar, slowly down after it. Once the landing starts it
 * is eased onto the design with everything else, so the last frame has every
 * cap exactly 9 px above its bar however high the dance threw it. */
static void advance_peak(int i, float h, uint32_t ms)
{
    uint32_t t = danced(ms);
    if (t < DANCE_MS) {
        if (h > s_peak[i]) s_peak[i] = h;
        else s_peak[i] -= PEAK_FALL_PX_PER_STEP;
        if (s_peak[i] < h) s_peak[i] = h;
        s_peak_land[i] = s_peak[i];  /* where the landing starts from */
        return;
    }
    float u = (float)(t - DANCE_MS) / LAND_MS;
    if (u > 1.0f) u = 1.0f;
    float ease = 1.0f - (1.0f - u) * (1.0f - u);
    s_peak[i] = s_peak_land[i] + (FINAL[i] - s_peak_land[i]) * ease;
    if (s_peak[i] < h) s_peak[i] = h;
}

/* Bar i at height h, with its cap sitting above whatever s_peak[i] holds. */
static void place(int i, float h)
{
    int ih = (int)(h + 0.5f);
    lv_obj_set_pos(s_bar[i], BOX_X + i * BAR_PITCH, BOX_Y + BOX_H - ih);
    lv_obj_set_height(s_bar[i], ih);
    int cap_y = BOX_Y + BOX_H - (int)(s_peak[i] + 0.5f) - CAP_GAP - CAP_H;
    if (cap_y < BOX_Y - CAP_GAP - CAP_H) cap_y = BOX_Y - CAP_GAP - CAP_H;
    lv_obj_set_y(s_cap[i], cap_y);
}

static void step(lv_timer_t *t)
{
    uint32_t ms = lv_tick_elaps(s_from);
    bool last = ms >= LOGO_MS;
    if (last) ms = LOGO_MS;

    for (int i = 0; i < BARS; i++) {
        float h = level(i, ms);
        advance_peak(i, h, ms);
        place(i, h);
    }

    if (!last) return;
    /* Done: the design exactly, and left there. ui_net.c fades its first
     * screen in over the top and calls ui_logo_hide() once it is covered. */
    lv_timer_pause(t);
    s_up = false;
}

void ui_logo_hide(void)
{
    if (!s_visible) return;
    s_visible = false;
    s_up = false;
    if (s_timer) lv_timer_pause(s_timer);
    if (s_panel) lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

/* Frozen at one moment of the animation, for looking at rather than catching.
 * The whole thing lasts a second and a half and a screenshot over USB takes
 * longer than that, so without this the dance could only be judged by eye on
 * the device, one run at a time. The peaks are what make it need replaying
 * rather than evaluating: a cap holds the highest point its bar has reached,
 * which depends on every step before this one, so the steps are walked through
 * here exactly as the timer would have walked them. */
void ui_test_logo_at(uint32_t at_ms)
{
    build();
    if (s_timer) lv_timer_pause(s_timer);
    s_up = s_visible = true;
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    if (at_ms > LOGO_MS) at_ms = LOGO_MS;
    for (int i = 0; i < BARS; i++) s_peak[i] = BAR_MIN;
    for (uint32_t ms = 0;; ms += STEP_MS) {
        if (ms > at_ms) ms = at_ms;
        for (int i = 0; i < BARS; i++) {
            float h = level(i, ms);
            advance_peak(i, h, ms);
            place(i, h);
        }
        if (ms >= at_ms) break;
    }
}

void ui_test_logo_hold(void) { ui_test_logo_at(LOGO_MS); }

void ui_logo_show(void)
{
    build();
    for (int i = 0; i < BARS; i++) s_peak[i] = BAR_MIN;
    s_from = lv_tick_get();
    s_up = s_visible = true;
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    if (!s_timer) {
        s_timer = lv_timer_create(step, STEP_MS, NULL);
        lv_timer_set_auto_delete(s_timer, false);
    }
    lv_timer_resume(s_timer);
    step(s_timer);  /* on screen now, not a frame from now */
}
