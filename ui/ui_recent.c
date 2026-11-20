/* Recent (Darren, 27 Oct 2026): the tracks played lately, newest first, in a
 * list like Playlists: each track's name with where it lives under it (its
 * album or playlist), fetched afresh each time Recent opens. Tap to play
 * through the Recent list itself in Now Playing, starting from that track
 * (each with its own picture). Swipe down from the top to go back to the
 * menu. Opens on the newest. The list is built the first time it opens
 * (internal RAM, as search). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_list.h"

#define MAX_RECENT 20
#define TOP_BAND LIST_HEADING_BAND  /* the heading's band, as every list screen draws it */

static const ui_library_t *s_lib;
static lv_obj_t *s_root, *s_empty;
static ui_list_t *s_list;
static int s_coll[MAX_RECENT], s_shown = -1;  /* s_shown: rows on screen, -1 before the first fill */
UI_IN_PSRAM static char s_track[MAX_RECENT][96];  /* the tracks on screen, copied: the library's text may go */

void recent_create(lv_obj_t *parent, const ui_library_t *lib)
{
    s_lib = lib;
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);
}

lv_obj_t *recent_root(void) { return s_root; }

static void build(void)
{
    if (s_list) return;
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    s_list = list_create(s_root, MAX_RECENT);

    s_empty = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_empty, &font_inter_light_36, 0);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_empty, "Nothing yet");
    lv_obj_center(s_empty);

    /* the heading sits on a black band the list scrolls under, as on Playlists */
    lv_obj_t *band = ui_plain(s_root);
    lv_obj_set_size(band, CORE_SCREEN, TOP_BAND);
    lv_obj_set_style_bg_color(band, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    list_heading(s_list, band);  /* there on the first item, fading as the list moves */
    lv_obj_t *heading = lv_label_create(band);
    lv_obj_set_style_text_font(heading, &font_inter_regular_18, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(heading, "Recent");
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 74);
}

static void fill(void)
{
    int n = s_lib->recent ? s_lib->recent() : 0;
    if (n > MAX_RECENT) n = MAX_RECENT;
    /* unchanged (back from the clock, say): leave it exactly as it was */
    bool same = n == s_shown;
    for (int i = 0; i < n && same; i++) {
        const char *t;
        same = s_lib->recent_item(i, &t) == s_coll[i] && !strcmp(t ? t : "", s_track[i]);
    }
    if (same) return;
    s_shown = n;
    const char *titles[MAX_RECENT], *details[MAX_RECENT];
    for (int i = 0; i < n; i++) {
        const char *track;
        int c = s_coll[i] = s_lib->recent_item(i, &track);
        snprintf(s_track[i], sizeof s_track[i], "%s", track ? track : "");
        const char *where = UI_IS_PLAYLIST(c) ? s_lib->playlist_name(c - UI_PLAYLIST_BASE) : s_lib->title(c);
        titles[i] = s_track[i][0] ? s_track[i] : where;
        details[i] = s_track[i][0] ? where : "";
    }
    list_show(s_list, n > 0);
    if (n) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    list_fill(s_list, n, titles, details, 0);  /* the newest first */
}

void recent_open(void)
{
    build();
    if (s_lib->recent_refresh) s_lib->recent_refresh();  /* the latest from Music Assistant, shown when it arrives */
    ui_show(UI_VIEW_RECENT);
}

/* Fresh each time it comes into view, from the menu or back from Now Playing:
 * what was just played is at the top. */
void recent_enter(void)
{
    ui_overlay_arc(false, UI_WHITE, 0, 0);
    fill();
}

void ui_recent_changed(void)
{
    if (s_list && ui_view() == UI_VIEW_RECENT) fill();
}

/* ---- Input, as on Playlists ---------------------------------------------------- */
void recent_press(int y)
{
    (void)y;  /* the way back is the bezel, not anywhere on the glass: see ui_list.h */
    if (s_list) list_press(s_list);
}

void recent_drag(float dx, float dy)
{
    if (!list_vertical(s_list, dx, dy)) return;
    list_drag(s_list, dy);
}

void recent_release(float dx, float dy, bool moved)
{
    bool down = moved && dy > CORE_SWIPE_V_PX && fabsf(dy) > fabsf(dx);
    if (ui_touch_back_gesture(dx, dy, moved) ||
        (down && !list_count(s_list))) {  /* back: see ui_list.h */
        ui_show(UI_VIEW_MENU);
        return;
    }
    if (moved && !list_vertical(s_list, dx, dy)) {
        list_settle(s_list);
        return;
    }
    if (list_release(s_list, dy, moved)) {
        np_open_recent(list_selected(s_list));  /* the list itself, from that track */
    }
}

void recent_knob(int steps) { list_knob(s_list, steps); }
