/* Voice search (an experiment, Darren 26 Oct 2026; easy to take out again).
 *
 * Search in the menu starts listening: "Listening", and the rim fills teal
 * with how loud the voice is. Tap when finished (it also stops by itself after
 * a pause). Then "Searching", then the results as a list that moves like the
 * menu, with the same detents: turn or drag, tap to open. An album opens its
 * tracks; a track opens its album on that track. Tap the words heard at the
 * top to search again. Swipe down from the top to go back to the menu, as on
 * the EQ (Darren, 26 Oct 2026); on the results, drags lower down move the list.
 * If nothing was heard, 8 s later it goes back by itself: to Now Playing while
 * music plays, otherwise to the menu (Darren, 27 Oct 2026).
 * Above the words heard, a microphone button shows that tapping there searches
 * again (an experiment, 27 Oct 2026; the version before is tagged
 * before-search-mic). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_list.h"

#define MAX_FOUND UI_MAX_FOUND
#define MIC_D 44   /* the search-again button */

typedef enum { ST_CLOSED, ST_LISTENING, ST_SEARCHING, ST_RESULTS, ST_MESSAGE } state_t;

static const ui_voice_t *s_voice;
static lv_obj_t *s_root, *s_status, *s_hint, *s_heard, *s_mic, *s_band;
static ui_list_t *s_list;
static ui_found_t *s_found;  /* MAX_FOUND, allocated with the screen (PSRAM, not internal RAM) */
static state_t s_state;
static float s_level;
static lv_timer_t *s_give_up;  /* "Didn't catch that" waiting to go back */

void ui_voice_init(const ui_voice_t *voice) { s_voice = voice; }

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *f, uint32_t colour, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));  /* one line, "..." when too long */
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_state(state_t st, const char *status, const char *hint)
{
    s_state = st;
    bool list = st == ST_RESULTS;
    list_show(s_list, list);
    show(s_mic, list);
    if (!list && s_band) {  /* listening, searching or a message: the words heard stay put */
        lv_obj_set_style_opa(s_band, LV_OPA_COVER, 0);
        show(s_band, true);
    }
    show(s_status, !list);
    show(s_hint, !list && hint);
    if (status) lv_label_set_text(s_status, status);
    if (hint) lv_label_set_text(s_hint, hint);
    if (st != ST_LISTENING) ui_overlay_arc(false, UI_TEAL, 0, 0);
    if (s_give_up) lv_timer_pause(s_give_up);  /* only "Didn't catch that" starts it */
}

/* Back to where the knob rests: Now Playing while music plays, else the menu. */
static void go_back(void)
{
    s_state = ST_CLOSED;
    ui_overlay_arc(false, UI_TEAL, 0, 0);
    if (np_playing()) np_open(np_player_album());
    else ui_show(UI_VIEW_MENU);
}

static void give_up(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (ui_view() == UI_VIEW_SEARCH && s_state == ST_MESSAGE) go_back();
}

static void wait_to_give_up(void)
{
    if (!s_give_up) {
        s_give_up = lv_timer_create(give_up, CORE_SEARCH_GIVE_UP_MS, NULL);
        lv_timer_set_auto_delete(s_give_up, false);  /* LVGL deletes a finished one-shot timer otherwise */
    }
    lv_timer_set_repeat_count(s_give_up, 1);
    lv_timer_reset(s_give_up);
    lv_timer_resume(s_give_up);
}

/* The microphone: a capsule in a cradle on a stand, white, in a 44 px circle. */
static void draw_mic(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int cx = a.x1 + MIC_D / 2, y = a.y1;

    lv_draw_rect_dsc_t body;
    lv_draw_rect_dsc_init(&body);
    body.bg_color = lv_color_hex(UI_WHITE);
    body.radius = LV_RADIUS_CIRCLE;
    lv_area_t capsule = {cx - 5, y + 9, cx + 5, y + 27};
    lv_draw_rect(layer, &body, &capsule);

    lv_draw_arc_dsc_t cradle;
    lv_draw_arc_dsc_init(&cradle);
    cradle.color = lv_color_hex(UI_WHITE);
    cradle.width = 3;
    cradle.rounded = 1;
    cradle.center.x = cx;
    cradle.center.y = y + 20;
    cradle.radius = 10;
    cradle.start_angle = 0;  /* the lower half */
    cradle.end_angle = 180;
    lv_draw_arc(layer, &cradle);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(UI_WHITE);
    line.width = 3;
    line.round_start = line.round_end = 1;
    line.p1.x = cx; line.p1.y = y + 31; line.p2.x = cx; line.p2.y = y + 36;  /* stand */
    lv_draw_line(layer, &line);
    line.p1.x = cx - 6; line.p1.y = y + 36; line.p2.x = cx + 6; line.p2.y = y + 36;  /* base */
    lv_draw_line(layer, &line);
}

/* The screen's labels are built the first time Search opens, not at start-up:
 * small LVGL objects come out of the internal RAM Wi-Fi needs while it joins. */
void search_create(lv_obj_t *parent)
{
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);
}

static void build(void)
{
    if (s_status) return;
    s_found = lv_malloc(MAX_FOUND * sizeof(ui_found_t));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    s_status = text(s_root, &font_inter_light_36, UI_WHITE, 300);
    lv_obj_set_pos(s_status, 30, 150);
    s_hint = text(s_root, &font_inter_regular_18, UI_WHITE, 240);
    lv_obj_set_pos(s_hint, 60, 205);

    s_list = list_create(s_root, MAX_FOUND);
    /* the words heard sit on a black band the list scrolls under */
    lv_obj_t *band = s_band = ui_plain(s_root);
    lv_obj_set_size(band, CORE_SCREEN, 112);
    lv_obj_set_style_bg_color(band, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    s_mic = ui_plain(band);
    lv_obj_set_pos(s_mic, (CORE_SCREEN - MIC_D) / 2, 20);
    lv_obj_set_size(s_mic, MIC_D, MIC_D);
    lv_obj_set_style_radius(s_mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic, lv_color_hex(UI_TEAL), 0);
    lv_obj_set_style_bg_opa(s_mic, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_mic, draw_mic, LV_EVENT_DRAW_MAIN, NULL);
    show(s_mic, false);
    s_heard = text(band, &font_inter_regular_18, UI_WHITE, 220);
    lv_obj_set_pos(s_heard, 70, 74);
    list_heading(s_list, band);  /* the words heard: there on the first result, fading as the list moves */
}

lv_obj_t *search_root(void) { return s_root; }

void search_open(void)
{
    build();
    s_level = 0;
    lv_label_set_text(s_heard, "");
    set_state(ST_LISTENING, "Listening", "Tap when you've finished");
    ui_show(UI_VIEW_SEARCH);
    if (s_voice) s_voice->listen();
    else set_state(ST_MESSAGE, "No microphone", "Swipe down to go back");
}

void search_enter(void)
{
    if (s_state == ST_LISTENING) ui_overlay_arc(true, UI_TEAL, 0, s_level);
    else ui_overlay_arc(false, UI_TEAL, 0, 0);
}

bool search_busy(void) { return ui_view() == UI_VIEW_SEARCH && (s_state == ST_LISTENING || s_state == ST_SEARCHING); }

void ui_voice_level(float level)
{
    if (s_state != ST_LISTENING) return;  /* also before the screen is built (ST_CLOSED) */
    /* quick to rise, slower to fall, so the rim follows the voice without flicker */
    s_level = level > s_level ? level : s_level * 0.8f + level * 0.2f;
    if (ui_view() == UI_VIEW_SEARCH) ui_overlay_arc(true, UI_TEAL, 0, s_level < 0.02f ? 0.02f : s_level);
}

void ui_voice_typed(void)
{
    build();
    lv_label_set_text(s_heard, "");
    set_state(ST_SEARCHING, "Searching", NULL);
    ui_show(UI_VIEW_SEARCH);
}

void ui_voice_searching(void)
{
    if (s_state == ST_LISTENING) set_state(ST_SEARCHING, "Searching", NULL);
}

/* ---- Results ----------------------------------------------------------------- */
void ui_voice_results(const char *heard, const ui_found_t *found, int n, const char *error)
{
    if (s_state != ST_SEARCHING) return;  /* cancelled meanwhile */
    static char quoted[120];
    if (heard && *heard) snprintf(quoted, sizeof quoted, "\xE2\x80\x9C%s\xE2\x80\x9D", heard);
    else quoted[0] = 0;
    lv_label_set_text(s_heard, quoted);
    if (error) {
        set_state(ST_MESSAGE, "Didn't work", error);
        return;
    }
    if (!heard || !*heard) {
        set_state(ST_MESSAGE, "Didn't catch that", "Tap to try again");
        wait_to_give_up();
        return;
    }
    if (n > MAX_FOUND) n = MAX_FOUND;
    if (!n) {
        set_state(ST_MESSAGE, "Nothing found", "Tap to try again");
        return;
    }
    const char *titles[MAX_FOUND], *details[MAX_FOUND];
    for (int i = 0; i < n; i++) {
        s_found[i] = found[i];
        titles[i] = s_found[i].title;
        details[i] = s_found[i].detail;
    }
    set_state(ST_RESULTS, NULL, NULL);
    list_fill(s_list, n, titles, details, 0);
}

static void open_selected(void)
{
    const ui_found_t *f = &s_found[list_selected(s_list)];
    np_open_track(f->album, f->track[0] ? f->track : NULL);
}

/* ---- Input ------------------------------------------------------------------ */
#define TOP_BAND LIST_HEADING_BAND  /* the words heard sit on this band */
static bool s_top_tap;  /* this touch started on the words heard: a tap searches again */

static void leave(void)
{
    if (s_voice && (s_state == ST_LISTENING || s_state == ST_SEARCHING)) s_voice->cancel();
    s_state = ST_CLOSED;
    ui_overlay_arc(false, UI_TEAL, 0, 0);
    ui_show(UI_VIEW_MENU);
}

void search_press(int y)
{
    s_top_tap = y < TOP_BAND && list_heading_shown(s_list);  /* the words heard, while they're there */
    if (s_give_up && !lv_timer_get_paused(s_give_up)) lv_timer_reset(s_give_up);  /* a touch gives it longer */
    if (s_state == ST_RESULTS) list_press(s_list);
}

void search_drag(float dx, float dy)
{
    if (s_state != ST_RESULTS || !list_vertical(s_list, dx, dy)) return;
    list_drag(s_list, dy);
}

void search_release(int x, int y, float dx, float dy, bool moved)
{
    (void)x;
    (void)y;
    bool down = moved && dy > CORE_SWIPE_V_PX && fabsf(dy) > fabsf(dx);
    /* back: a swipe down from the top; with no list on screen, any swipe down */
    if (ui_touch_back_gesture(dx, dy, moved) ||
        (down && s_state != ST_RESULTS)) {
        leave();
        return;
    }
    if (moved && (s_state != ST_RESULTS || !list_vertical(s_list, dx, dy))) {  /* not a list drag: settle where it was */
        if (s_state == ST_RESULTS) list_settle(s_list);
        return;
    }
    switch (s_state) {
    case ST_CLOSED:
        break;
    case ST_LISTENING:
        if (!moved && s_voice) s_voice->stop();
        break;
    case ST_SEARCHING:
        break;
    case ST_MESSAGE:
        if (!moved) search_open();
        break;
    case ST_RESULTS:
        if (moved) list_release(s_list, dy, true);
        else if (s_top_tap) search_open();  /* the words heard, at the top: search again */
        else open_selected();
        break;
    }
}

void ui_search_detent(float k)
{
    build();
    list_detent(s_list, k);
}

void search_knob(int steps)
{
    if (s_state == ST_RESULTS) list_knob(s_list, steps);
}
