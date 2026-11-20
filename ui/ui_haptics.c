/* Haptic feedback: a tick from the knob's vibration motor when something
 * snaps into place. Darren picked the sharp click (demo number 2, 26 Oct);
 * the demo (console "haptic demo") still plays all six candidates, numbered. */
#include <stdio.h>

#include "ui_internal.h"

/* DRV2605 ERM library effects to try, in demo order */
static const struct { uint8_t effect; const char *name; } CANDIDATES[] = {
    {1, "strong click"}, {4, "sharp click"}, {21, "medium click"},
    {24, "sharp tick"}, {26, "light tick"}, {7, "soft bump"},
};
#define NCAND ((int)(sizeof CANDIDATES / sizeof CANDIDATES[0]))

static void (*s_play)(uint8_t effect);
static uint8_t s_effect = 4;  /* sharp click; the feel Darren picked, kept whether on or off */
static bool s_on = true;      /* the firmware sets this from what was saved on the knob */
static lv_obj_t *s_demo, *s_num, *s_caption;
static lv_timer_t *s_demo_timer;
static int s_step;

void ui_haptic_init(void (*play)(uint8_t effect)) { s_play = play; }
/* Off without forgetting: the effect stays as it was, so turning it back on
 * gives the same feel rather than a default (Darren, 28 Oct 2026). */
void ui_haptic_enable(bool on) { s_on = on; }
bool ui_haptic_enabled(void) { return s_on; }
void ui_haptic_set(uint8_t effect) { s_effect = effect; }
uint8_t ui_haptic_effect(void) { return s_effect; }

void ui_tick(void)
{
    if (s_play && s_on && s_effect) s_play(s_effect);
}

/* ---- Demo ------------------------------------------------------------------------ */
static void demo_step(lv_timer_t *t)
{
    int c = s_step / 4, k = s_step % 4;  /* every 400 ms: three plays, then a gap */
    s_step++;
    if (c >= NCAND) {
        lv_timer_delete(t);
        s_demo_timer = NULL;
        lv_obj_add_flag(s_demo, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (k == 0) {
        char n[12];
        snprintf(n, sizeof n, "%d", c + 1);
        lv_label_set_text(s_num, n);
        lv_obj_center(s_num);
        printf("HAPTIC demo %d: effect %d (%s)\n", c + 1, CANDIDATES[c].effect, CANDIDATES[c].name);
    }
    if (k < 3 && s_play) s_play(CANDIDATES[c].effect);
}

void ui_haptic_demo(void)
{
    if (s_demo_timer) return;
    if (!s_demo) {
        s_demo = ui_plain(lv_screen_active());
        lv_obj_set_size(s_demo, CORE_SCREEN, CORE_SCREEN);
        lv_obj_set_style_bg_color(s_demo, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_bg_opa(s_demo, LV_OPA_COVER, 0);
        s_num = lv_label_create(s_demo);
        lv_obj_set_style_text_font(s_num, &font_inter_extralight_96, 0);
        lv_obj_set_style_text_color(s_num, lv_color_hex(UI_WHITE), 0);
        s_caption = lv_label_create(s_demo);
        lv_obj_set_style_text_font(s_caption, &font_inter_regular_24, 0);
        lv_obj_set_style_text_color(s_caption, lv_color_hex(UI_WHITE), 0);
        lv_label_set_text(s_caption, "Which feels right?");
        lv_obj_align(s_caption, LV_ALIGN_CENTER, 0, 90);
    }
    lv_obj_move_foreground(s_demo);
    lv_obj_remove_flag(s_demo, LV_OBJ_FLAG_HIDDEN);
    s_step = 0;
    s_demo_timer = lv_timer_create(demo_step, 400, NULL);
    demo_step(s_demo_timer);
}

uint8_t ui_haptic_candidate(int n) { return n >= 1 && n <= NCAND ? CANDIDATES[n - 1].effect : 0; }
