/* Ask first (27 Oct 2026): a question over whatever screen is showing, for
 * things that shouldn't happen by accident: "Music off", "Louder than 80%?".
 * Laid out like every list (Darren: as Playlists and the search results): the
 * question as the heading at the top, the answers in the shared list under it,
 * Cancel in the pill, the answer that does the thing ("Turn off", "Continue")
 * one click away: nothing happens by accident, and saying yes is a deliberate
 * turn of the knob (Darren, 29 Oct 2026). Only the pill answers a tap - a tap
 * anywhere else on the screen is ignored rather than choosing for you, which is
 * what let a tap on Cancel choose Continue and the volume go on to 100.
 *
 * A question may instead have ONE answer (ui_ask_one, Darren 30 Oct 2026, for
 * "Louder than 80%?"): no Cancel, nothing to move through, and the knob is
 * left to the screen underneath, so a hand already turning carries on setting
 * the volume instead of spinning into an answer it never meant to give. The
 * only way to say yes is to tap the one answer; left alone it goes by itself.
 * Swipe down from the top cancels, as it goes back everywhere,
 * and so does CORE_ASK_CANCEL_MS untouched. Built the first time it's needed
 * (internal RAM, as search).
 *
 * A question may also count down before it acts (count_ms, Darren 5 Nov 2026,
 * for Music off): the answer turns the same screen into the seconds left in
 * the middle, the rim full and winding back to 6 o'clock as they go, and "Tap
 * to cancel" under it. Nothing has been done yet - a tap anywhere or a turn of
 * the knob takes it back to where the question was asked, and only when the
 * rim is empty does the answer act. */
#include <math.h>
#include <string.h>

#include "ui_list.h"

#define TOP_BAND LIST_HEADING_BAND  /* the heading's band, as every list screen draws it */

static lv_obj_t *s_panel, *s_heading, *s_hint, *s_figure;
static bool s_one;  /* one answer, and the knob isn't ours */
static bool s_counting;  /* the answer is counting down (count_ms) */
static lv_anim_t s_count_anim;
static ui_list_t *s_list;
static lv_timer_t *s_timer;
static ui_ask_t s_ask;
static int s_press_y;  /* which answer the finger went down on */

static void count_exec(void *obj, int32_t left_ms);

static void hide(void)
{
    if (s_panel) lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_timer) lv_timer_pause(s_timer);
    if (s_counting) {
        s_counting = false;
        lv_anim_delete(s_panel, count_exec);
        ui_overlay_arc(false, UI_WHITE, 0, 0);  /* the spinner has the rim back, if it is armed */
    }
}

void ui_ask_cancel(void)
{
    if (!ui_asking()) return;
    hide();
    if (s_ask.cancelled) s_ask.cancelled();
}

static void timeout(lv_timer_t *t)
{
    (void)t;
    ui_ask_cancel();
}

static void build(void)
{
    if (s_panel) return;
    s_panel = ui_plain(lv_layer_top());
    lv_obj_set_size(s_panel, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_panel, UI_OPA_SCRIM, 0);
    s_list = list_create(s_panel, 2);
    /* the heading's band, in the place every list screen puts it */
    lv_obj_t *band = ui_plain(s_panel);
    lv_obj_set_size(band, CORE_SCREEN, TOP_BAND);
    /* No black of its own any more. The panel behind it is 90% black across
     * the whole screen, and a second black band on top of that would be 99%
     * there - a solid strip across the cover with faint art under the rest.
     * The band stays because it is the heading's box; a question has one or
     * two answers and the list never scrolls, so there is nothing to mask. */
    lv_obj_set_style_bg_opa(band, LV_OPA_TRANSP, 0);
    s_heading = lv_label_create(band);
    lv_obj_set_style_text_font(s_heading, &font_inter_regular_18, 0);
    lv_obj_set_style_text_color(s_heading, lv_color_hex(UI_WHITE), 0);
    /* the seconds left in a countdown, in the clock's figure */
    s_figure = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_figure, &font_inter_extralight_96, 0);
    lv_obj_set_style_text_color(s_figure, lv_color_hex(UI_WHITE), 0);
    s_hint = lv_label_create(s_panel);  /* the line under a one-answer question */
    lv_obj_set_style_text_font(s_hint, &font_inter_regular_18, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_WHITE), 0);
    s_timer = lv_timer_create(timeout, CORE_ASK_CANCEL_MS, NULL);
    lv_timer_set_auto_delete(s_timer, false);  /* LVGL deletes a finished one-shot timer otherwise */
}

static void show(const ui_ask_t *q, bool one)
{
    build();
    hide();  /* a countdown still running ends here */
    s_ask = *q;
    s_one = one;
    lv_obj_add_flag(s_figure, LV_OBJ_FLAG_HIDDEN);
    list_show(s_list, true);
    lv_label_set_text(s_heading, q->title);
    lv_obj_align(s_heading, LV_ALIGN_TOP_MID, 0, 74);
    if (one) {
        const char *titles[1] = {q->button}, *details[1] = {""};
        list_fill(s_list, 1, titles, details, 0);
    } else {
        const char *titles[2] = {"Cancel", q->button}, *details[2] = {"", ""};
        list_fill(s_list, 2, titles, details, 0);  /* Cancel in the pill */
    }
    lv_label_set_text(s_hint, one && q->hint ? q->hint : "");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 262);
    lv_timer_set_period(s_timer, one ? CORE_ASK_ONE_MS : CORE_ASK_CANCEL_MS);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    lv_timer_set_repeat_count(s_timer, 1);
    lv_timer_reset(s_timer);
    lv_timer_resume(s_timer);
}

void ui_ask(const ui_ask_t *q) { show(q, false); }
void ui_ask_one(const ui_ask_t *q) { show(q, true); }
bool ui_ask_takes_knob(void) { return !s_one; }
bool ui_ask_is(const ui_ask_t *q) { return ui_asking() && s_ask.yes == q->yes; }
void ui_ask_keepalive(void)
{
    if (ui_asking()) lv_timer_reset(s_timer);
}

bool ui_asking(void) { return s_panel && !lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN); }
bool ui_ask_counting(void) { return ui_asking() && s_counting; }

/* ---- The countdown ------------------------------------------------------------- */
static void count_exec(void *obj, int32_t left_ms)
{
    (void)obj;
    /* 5, 4, 3, 2, 1: a whole second is still to come while its figure shows */
    char figure[4];
    lv_snprintf(figure, sizeof figure, "%d", (int)((left_ms + 999) / 1000));
    if (strcmp(lv_label_get_text(s_figure), figure)) lv_label_set_text(s_figure, figure);
    ui_overlay_arc(true, UI_WHITE, 0, (float)left_ms / (float)s_ask.count_ms);
}

static void count_done(lv_anim_t *a)
{
    (void)a;
    if (!s_counting) return;  /* deleted by a cancel, not run out */
    hide();
    if (s_ask.yes) s_ask.yes();
}

static void count_start(void)
{
    s_counting = true;
    lv_timer_pause(s_timer);  /* the countdown is its own timeout */
    list_show(s_list, false);
    lv_label_set_text(s_heading, s_ask.counting ? s_ask.counting : s_ask.button);
    lv_obj_align(s_heading, LV_ALIGN_TOP_MID, 0, 74);
    lv_label_set_text(s_figure, "");
    lv_obj_remove_flag(s_figure, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_figure, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_hint, "Tap to cancel");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 262);
    lv_anim_init(&s_count_anim);
    lv_anim_set_var(&s_count_anim, s_panel);
    lv_anim_set_exec_cb(&s_count_anim, count_exec);
    lv_anim_set_values(&s_count_anim, s_ask.count_ms, 0);
    lv_anim_set_duration(&s_count_anim, s_ask.count_ms);
    lv_anim_set_path_cb(&s_count_anim, lv_anim_path_linear);
    lv_anim_set_completed_cb(&s_count_anim, count_done);
    lv_anim_start(&s_count_anim);
    count_exec(NULL, s_ask.count_ms);  /* full and showing 5 from the first frame */
}

/* ---- Input, as on Playlists ---------------------------------------------------- */
void ui_ask_press(int y)
{
    if (s_counting) return;  /* the release is what cancels */
    lv_timer_reset(s_timer);  /* a touch gives it longer */
    s_press_y = y;
    list_press(s_list);
}

void ui_ask_drag(float dx, float dy)
{
    if (s_counting) return;
    if (!list_vertical(s_list, dx, dy)) return;
    list_drag(s_list, dy);
}

void ui_ask_release(float dx, float dy, bool moved)
{
    if (s_counting) {  /* a tap or a swipe, anywhere: not yet */
        ui_ask_cancel();
        return;
    }
    lv_timer_reset(s_timer);
    bool down = moved && dy > CORE_SWIPE_V_PX && fabsf(dy) > fabsf(dx);
    if (ui_touch_back_gesture(dx, dy, moved)) {  /* back: see ui_list.h */
        ui_ask_cancel();
        return;
    }
    if (moved && !list_vertical(s_list, dx, dy)) {
        list_settle(s_list);
        return;
    }
    if (!list_release(s_list, dy, moved)) return;
    if (list_row_at(s_list, s_press_y) != list_selected(s_list)) return;  /* only the pill answers */
    if (!s_one && list_selected(s_list) == 0) {  /* Cancel */
        ui_ask_cancel();
        return;
    }
    if (s_ask.count_ms > 0) {
        count_start();
        return;
    }
    hide();
    if (s_ask.yes) s_ask.yes();
}

void ui_ask_knob(int steps)
{
    if (s_counting) {  /* a turn takes it back, as a tap does */
        ui_ask_cancel();
        return;
    }
    lv_timer_reset(s_timer);
    list_knob(s_list, steps);
}
