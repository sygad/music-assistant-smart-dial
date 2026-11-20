/* Updating over Wi-Fi, on the screen (2 Nov 2026).
 *
 * A firmware update is the one thing the knob does that Darren cannot see
 * happening and must not interrupt, so it takes the whole screen: the word for
 * what is going on, the figure, and the rim filling as it arrives. Over
 * everything, on the top layer, the way a question is - and unlike a question
 * there is no answer to give, so while this is up the knob and the glass do
 * nothing at all (ui.c asks ui_updating() first) and the twenty-minute sleep
 * is held off. Nothing on the wall should go dark in the middle of being
 * written to.
 *
 * It is left up on a failure too, for long enough to be read, because a failed
 * update is silent otherwise: the knob simply carries on with the firmware it
 * already had and nothing would ever say why. */
#include <stdio.h>

#include "ui_internal.h"

/* Long enough to read and walk away from; the knob is still perfectly usable
 * underneath, running the firmware it already had. */
#define FAILED_HOLD_MS 10000

/* The per-cent sign is drawn in its own smaller label rather than as part of
 * the figure, because the 96 px display font does not have the glyph: that
 * font was generated with digits, a colon and a hyphen in it and nothing else
 * (tools/fonts.sh), so "42%" came out as "42" and an empty box. Adding the
 * glyph would mean regenerating the font, and the recipe only reproduces the
 * committed files with one exact pairing of Inter and the converter - any
 * other redraws every glyph slightly differently, which would quietly change
 * the clock and the scrubber too. A smaller sign beside a big figure is how
 * the knob shows a percentage anyway (the battery in Settings), so this is the
 * house style rather than a workaround. */
#define PCT_GAP  1   /* the sign hugs the figure: it is a unit, not a second word */
/* The sign rides at the top of the figure (Darren, 2 Nov 2026), so it reads as
 * a superscript rather than as a smaller word standing beside it: the top of
 * the "%" level with the top of the digits (design, 19 Nov 2026). Aligning the
 * two labels' tops is not the same as aligning the two glyphs' tops, because a
 * label carries space above its letters and the two fonts carry different
 * amounts. So the offset is read from the fonts rather than nudged by eye: the
 * old +8 dropped the sign into no-man's-land, neither on the baseline nor
 * level with the tops. "1" is the digit with a flat top - the round ones
 * overshoot by a pixel. */
static int32_t ink_top(const lv_font_t *f, uint32_t ch)
{
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(f, &g, ch, 0)) return 0;
    return lv_font_get_line_height(f) - f->base_line - (g.ofs_y + g.box_h);
}

static lv_obj_t *s_panel, *s_heading, *s_figure, *s_percent, *s_line;
static lv_timer_t *s_hide_timer;
static ui_update_t s_state;

static void hide(lv_timer_t *t)
{
    (void)t;
    if (s_panel) lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_hide_timer) lv_timer_pause(s_hide_timer);
    s_state = UI_UPDATE_NONE;
    ui_overlay_arc(false, UI_WHITE, 0, 0);
}

static void build(void)
{
    if (s_panel) return;
    s_panel = ui_plain(lv_layer_top());
    lv_obj_set_size(s_panel, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);

    /* The three lines sit where a question's do, so this looks like the rest
     * of the knob rather than like an error dialog from somewhere else. */
    s_heading = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_heading, &font_inter_regular_18, 0);
    lv_obj_set_style_text_color(s_heading, lv_color_hex(UI_WHITE), 0);

    s_figure = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_figure, &font_inter_extralight_96, 0);
    lv_obj_set_style_text_color(s_figure, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_width(s_figure, LV_SIZE_CONTENT);  /* the pair is centred together */

    s_percent = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_percent, &font_inter_light_36, 0);
    lv_obj_set_style_text_color(s_percent, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_width(s_percent, LV_SIZE_CONTENT);
    lv_label_set_text(s_percent, "%");

    s_line = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_line, &font_inter_regular_18, 0);
    lv_obj_set_style_text_color(s_line, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_text_align(s_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_line, CORE_SCREEN - 80);  /* a reason can run to two lines */

    s_hide_timer = lv_timer_create(hide, FAILED_HOLD_MS, NULL);
    lv_timer_set_auto_delete(s_hide_timer, false);
    lv_timer_pause(s_hide_timer);
}

/* AN UPDATE THAT FAILED IS NOT AN UPDATE IN PROGRESS (6 Nov 2026).
 *
 * This used to be `s_state != UI_UPDATE_NONE`, which counted the FAILED screen
 * as a job the dial was in the middle of. And a failure is permanent by
 * design - ui_update_show refuses to hide it, so Darren sees what happened
 * rather than a screen that has quietly tidied itself up. The two together
 * meant that a failed update pinned the dial AWAKE, SCREEN LIT, FOREVER:
 * screen_busy() stayed true, so idle_check pushed the idle stamp forward and
 * returned before reaching a single sleep check, and the knob and the glass
 * were both held off as well.
 *
 * Which is exactly what Darren walked in on, on the morning of 6 Nov: "I'm
 * just looking at a black screen with the words update failed and writing it
 * failed written on it", poking at a dial that could not respond. The reported
 * charge went from 71% to 49% in the forty-five minutes it sat there lit.
 *
 * So the message stays - that part was right and is unchanged - but it stops
 * being a job. The dial dims, sleeps and deep-sleeps on its ordinary timers
 * with the failure still on the screen underneath, and a turn of the knob
 * brings it back and shows it still there. */
bool ui_updating(void)
{
    return s_state == UI_UPDATE_CONNECTING || s_state == UI_UPDATE_DOWNLOADING ||
           s_state == UI_UPDATE_INSTALLING;
}

bool ui_update_failed(void) { return s_state == UI_UPDATE_FAILED; }

void ui_update_show(ui_update_t state, int percent, const char *why)
{
    if (state == UI_UPDATE_NONE) {
        if (s_state != UI_UPDATE_NONE && s_state != UI_UPDATE_FAILED) hide(NULL);
        return;
    }
    build();
    bool first = s_state == UI_UPDATE_NONE;
    s_state = state;

    char figure[8] = "";
    const char *heading = "Firmware update", *line = "";
    switch (state) {
    case UI_UPDATE_CONNECTING:
        line = "Fetching";  /* the heading already says what of */
        break;
    case UI_UPDATE_DOWNLOADING:
        /* Clamped because it is a figure on a screen, not a measurement: a
         * bad content length cannot be allowed to print four digits. */
        if (percent >= 0) snprintf(figure, sizeof figure, "%d", percent > 100 ? 100 : percent);
        line = "Do not power off";
        break;
    case UI_UPDATE_INSTALLING:
        snprintf(figure, sizeof figure, "100");
        line = "Restarting";
        break;
    case UI_UPDATE_FAILED:
        heading = "Update failed";
        line = why && why[0] ? why : "";
        break;
    case UI_UPDATE_NONE:
        break;
    }

    lv_label_set_text(s_heading, heading);
    lv_obj_align(s_heading, LV_ALIGN_TOP_MID, 0, 74);
    /* Figure and sign centred as one group, so 9, 42 and 100 all sit in the
     * middle of the screen rather than the figure alone doing so. */
    lv_label_set_text(s_figure, figure);
    if (figure[0]) {
        lv_obj_remove_flag(s_percent, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(s_panel);
        int32_t sign = lv_obj_get_width(s_percent);
        lv_obj_align(s_figure, LV_ALIGN_CENTER, -(PCT_GAP + sign) / 2, 0);
        int32_t rise = ink_top(&font_inter_extralight_96, '1') - ink_top(&font_inter_light_36, '%');
        lv_obj_align_to(s_percent, s_figure, LV_ALIGN_OUT_RIGHT_TOP, PCT_GAP, rise);
    } else {
        lv_obj_add_flag(s_percent, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_figure, LV_ALIGN_CENTER, 0, 0);
    }
    lv_label_set_text(s_line, line);
    lv_obj_align(s_line, LV_ALIGN_TOP_MID, 0, 262);

    /* The rim says the same thing as the figure, and is the part that can be
     * read from the doorway. */
    if (state == UI_UPDATE_DOWNLOADING && percent >= 0)
        ui_overlay_arc(true, UI_TEAL, 0, percent / 100.0f);
    else if (state == UI_UPDATE_INSTALLING)  /* done downloading: success, so green */
        ui_overlay_arc(true, UI_OK, 0, 1.0f);
    else
        ui_overlay_arc(false, UI_WHITE, 0, 0);

    if (first) {
        lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        ui_force_wake();  /* an update that happens behind a dark screen is worse than useless */
    }
    lv_obj_move_foreground(s_panel);

    /* A failure clears itself; the states on the way to a restart never do -
     * the restart is what takes them away. */
    if (state == UI_UPDATE_FAILED) {
        lv_timer_set_repeat_count(s_hide_timer, 1);
        lv_timer_reset(s_hide_timer);
        lv_timer_resume(s_hide_timer);
    } else {
        lv_timer_pause(s_hide_timer);
    }
}
