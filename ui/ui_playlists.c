/* Playlists (Darren, 27 Oct 2026): the playlists in a list like the search
 * results (the pill, the fisheye), the name on its own (Darren, 28 Oct 2026:
 * the number of tracks told him nothing he wanted). Turn or
 * drag, tap to open: the playlist's tracks in Now Playing, each with its own
 * picture. Swipe down from the top to go back to the menu, as on search and the
 * EQ. The list is built the first time it opens (internal RAM, as search). */
#include <math.h>
#include <stdio.h>

#include "ui_list.h"

#define MAX_PLAYLISTS 24
#define TOP_BAND LIST_HEADING_BAND  /* the heading's band, as every list screen draws it */

static const ui_library_t *s_lib;
static lv_obj_t *s_root, *s_heading, *s_empty;
static ui_list_t *s_list;
static char (*s_detail)[24];
static int s_sel;

void playlists_create(lv_obj_t *parent, const ui_library_t *lib)
{
    s_lib = lib;
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);
}

lv_obj_t *playlists_root(void) { return s_root; }

static int count(void)
{
    int n = s_lib->playlists ? s_lib->playlists() : 0;
    return n > MAX_PLAYLISTS ? MAX_PLAYLISTS : n;
}

static void build(void)
{
    if (s_list) return;
    s_detail = lv_malloc_zeroed(MAX_PLAYLISTS * sizeof *s_detail);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    s_list = list_create(s_root, MAX_PLAYLISTS);

    s_empty = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_empty, &font_inter_light_36, 0);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_empty, "No playlists");
    lv_obj_center(s_empty);

    /* the heading sits on a black band the list scrolls under */
    lv_obj_t *band = ui_plain(s_root);
    lv_obj_set_size(band, CORE_SCREEN, TOP_BAND);
    lv_obj_set_style_bg_color(band, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    list_heading(s_list, band);  /* there on the first item, fading as the list moves */
    s_heading = lv_label_create(band);
    lv_obj_set_style_text_font(s_heading, &font_inter_regular_18, 0);
    lv_obj_set_style_text_color(s_heading, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_heading, "Playlists");
    lv_obj_align(s_heading, LV_ALIGN_TOP_MID, 0, 74);
}

static void fill(void)
{
    int n = count();
    const char *titles[MAX_PLAYLISTS], *details[MAX_PLAYLISTS];
    for (int i = 0; i < n; i++) {
        int k = s_lib->playlist_size ? s_lib->playlist_size(i) : -1;
        s_detail[i][0] = 0;  /* the name on its own: a row like a menu item */
        (void)k;
        titles[i] = s_lib->playlist_name(i);
        details[i] = s_detail[i];
    }
    list_show(s_list, n > 0);
    if (n) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    list_fill(s_list, n, titles, details, s_sel);
}

void playlists_open(void)
{
    build();
    fill();
    ui_show(UI_VIEW_PLAYLISTS);
}

void playlists_enter(void) { ui_overlay_arc(false, UI_WHITE, 0, 0); }

void ui_playlists_changed(void)
{
    if (s_list && ui_view() == UI_VIEW_PLAYLISTS) fill();
}

/* ---- Input ------------------------------------------------------------------ */
void playlists_press(int y)
{
    (void)y;  /* the way back is the bezel, not anywhere on the glass: see ui_list.h */
    if (s_list) list_press(s_list);
}

void playlists_drag(float dx, float dy)
{
    if (!list_vertical(s_list, dx, dy)) return;
    list_drag(s_list, dy);
}

void playlists_release(float dx, float dy, bool moved)
{
    bool down = moved && dy > CORE_SWIPE_V_PX && fabsf(dy) > fabsf(dx);
    if (ui_touch_back_gesture(dx, dy, moved) ||
        (down && !list_count(s_list))) {  /* back: see ui_list.h */
        ui_show(UI_VIEW_MENU);
        return;
    }
    if (moved && !list_vertical(s_list, dx, dy)) {  /* not a list drag: settle where it was */
        list_settle(s_list);
        return;
    }
    if (list_release(s_list, dy, moved)) {
        s_sel = list_selected(s_list);
        np_open_from_playlists(s_sel);
    }
    s_sel = list_selected(s_list);
}

void playlists_knob(int steps)
{
    list_knob(s_list, steps);
    s_sel = list_selected(s_list);
}
