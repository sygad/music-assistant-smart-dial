/* Screen 01: the clock. Shown over whatever screen is up after a spell of no
 * input while no music plays, like a screensaver: the screen underneath is
 * kept as it was, and a tap or a turn of the knob only wakes it (Darren,
 * 26 Oct 2026). Design: black, Inter ExtraLight 96 white, centred. */
#include <stdio.h>
#include <time.h>

#include "ui_internal.h"

static lv_obj_t *s_clock, *s_time;
static lv_timer_t *s_tick;
static int s_minute_shown = -1;

static void refresh(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2025) {  /* not set from the internet yet */
        if (s_minute_shown != -2) lv_label_set_text(s_time, "--:--");
        s_minute_shown = -2;
        return;
    }
    int minute = tm.tm_hour * 60 + tm.tm_min;
    if (minute == s_minute_shown) return;
    s_minute_shown = minute;
    char buf[8];
    snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_time, buf);
}

void clock_create(void)
{
    s_clock = ui_plain(lv_layer_top());
    lv_obj_set_size(s_clock, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_clock, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_clock, LV_OPA_COVER, 0);
    s_time = lv_label_create(s_clock);
    lv_obj_set_style_text_font(s_time, &font_inter_extralight_96, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(UI_WHITE), 0);
    /* The font's line box is shorter than Figma's, so the design's y 122 left
     * the digits (72 px tall) at rows 122-193 on the knob: centred, they start at 144. */
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    s_tick = lv_timer_create(refresh, 1000, NULL);
    lv_timer_pause(s_tick);
}

bool clock_showing(void) { return !lv_obj_has_flag(s_clock, LV_OBJ_FLAG_HIDDEN); }

void clock_show(void)
{
    s_minute_shown = -1;
    refresh(NULL);
    lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(s_tick);
}

/* The screen is off under the clock: stop the minute tick, since nothing can
 * be seen and a label redraw is a frame nobody wanted. Coming back out, the
 * time is refreshed before the backlight does (clock_dark is called while the
 * screen is still dark), so there is no stale minute on the way up. */
void clock_dark(bool dark)
{
    if (dark) {
        lv_timer_pause(s_tick);
        return;
    }
    s_minute_shown = -1;
    refresh(NULL);
    lv_timer_resume(s_tick);
}

void clock_hide(void)
{
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_tick);
}
