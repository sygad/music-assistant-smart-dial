/* Screen 02: Menu. A fixed teal pill with the list scrolling behind it.
 *
 * Darren's order (27 Oct 2026), weighted on the middle: Search, Albums, Recent
 * (the most used, so the menu starts there), Playlists, then Radio only when
 * Music Assistant has stations, Settings and Music off. "Recent" because
 * "Recently played" is wider than the pill. Every item opens. Music off asks
 * first
 * ("Music off?": Turn off or Cancel, ui_ask), then stops the music and shows
 * the clock (Darren, 27 Oct 2026). Settings (28 Oct 2026) shows the knob's own
 * details and goes no further (ui_settings.c).
 *
 * Fisheye (an experiment, same day; the version before it is tagged
 * before-menu-fisheye): items get smaller and slightly darker the further they
 * are from the pill, continuously as the list moves. */
#include <math.h>

#include "ui_internal.h"

enum { ITEM_SEARCH, ITEM_ALBUMS, ITEM_RECENT, ITEM_PLAYLISTS, ITEM_RADIO, ITEM_SETTINGS, ITEM_OFF, ITEMS_ALL };
static const char *const NAMES[ITEMS_ALL] = {"Search", "Albums",   "Recent", "Playlists",
                                             "Radio",  "Settings", "Music off"};

static lv_obj_t *s_root, *s_list, *s_labels[ITEMS_ALL];
static int s_items[ITEMS_ALL];  /* the items shown, in order */
static int s_count;
static bool s_radio;
static int s_sel, s_highlight;
static float s_dist[ITEMS_ALL];  /* each item's distance from the pill, in items */
static int s_text_top;            /* the text's top within its 60 px line */

static int index_of(int item)
{
    for (int i = 0; i < s_count; i++)
        if (s_items[i] == item) return i;
    return 0;
}

static int item_of(lv_obj_t *label)
{
    for (int i = 0; i < ITEMS_ALL; i++)
        if (s_labels[i] == label) return i;
    return 0;
}

/* Text colour runs from the item's grey (0) to white (255); the grey darkens
 * with distance from the pill. */
static void paint(lv_obj_t *label)
{
    int32_t v = (int32_t)(intptr_t)lv_obj_get_user_data(label);
    int g = core_fisheye_grey(s_dist[item_of(label)]);
    lv_color_t grey = lv_color_make((uint8_t)g, (uint8_t)g, (uint8_t)g);
    lv_obj_set_style_text_color(label, lv_color_mix(lv_color_hex(UI_WHITE), grey, (uint8_t)v), 0);
}

static void label_mix(void *label, int32_t v)
{
    lv_obj_set_user_data(label, (void *)(intptr_t)v);
    paint(label);
}

/* The fisheye's sizes are real fonts, one per pixel from 22 to 36, picked by
 * how far the item is from the pill. Scaling one font with a transform instead
 * made LVGL draw every item through a layer buffer it allocated and composited
 * each frame: 33 ms of every frame on the knob, menu drag 14 fps against 39
 * with the transform taken out (measured 28 Oct 2026). */
#define MENU_PX_MIN 22
#define MENU_PX_MAX 36

static const lv_font_t *const MENU_FONTS[] = {
    &font_inter_light_22, &font_inter_light_23, &font_inter_light_24, &font_inter_light_25,
    &font_inter_light_26, &font_inter_light_27, &font_inter_light_28, &font_inter_light_29,
    &font_inter_light_30, &font_inter_light_31, &font_inter_light_32, &font_inter_light_33,
    &font_inter_light_34, &font_inter_light_35, &font_inter_light_36};

static const lv_font_t *menu_font(float scale)
{
    int px = (int)lroundf(MENU_PX_MAX * scale);
    px = px < MENU_PX_MIN ? MENU_PX_MIN : px > MENU_PX_MAX ? MENU_PX_MAX : px;
    return MENU_FONTS[px - MENU_PX_MIN];
}

/* Where an item's line sits in its slot: centred on the pitch, as CSS centres
 * the content area in a line box.
 *
 * Worked out from the exact scale rather than from the whole-pixel font that
 * was picked for it. The two differ by up to half a pixel, and taking it from
 * the font made an item hop sideways of its glide every time it crossed into
 * the next size (Darren, 28 Oct 2026: "jiggling about"). The glyphs still
 * change in whole pixels; only their place is continuous. */
static int text_top(const lv_font_t *f) { return (int)lroundf((CORE_MENU_PITCH - lv_font_get_line_height(f)) / 2.0f); }

static int text_top_at(float scale)
{
    float lh = lv_font_get_line_height(&font_inter_light_36) * scale;
    return (int)lroundf((CORE_MENU_PITCH - lh) / 2.0f);
}

/* Size and shade every item by where it is now, relative to the pill. */
static void fisheye(void)
{
    /* the position just set, not the laid-out one, which only catches up on
     * the next frame (the first results were sized for the old position) */
    int32_t top = lv_obj_get_style_y(s_list, 0);
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *l = s_labels[s_items[i]];
        float centre = (float)(top + i * CORE_MENU_PITCH + CORE_MENU_PITCH / 2);
        float ds = (centre - CORE_SCREEN / 2.0f) / CORE_MENU_PITCH, d = fabsf(ds);
        s_dist[s_items[i]] = d;
        float scale = core_fisheye_scale(d);
        lv_obj_set_style_text_font(l, menu_font(scale), 0);
        float shift = (core_fisheye_offset(ds) - ds) * CORE_MENU_PITCH;  /* closer together as they shrink */
        lv_obj_set_y(l, i * CORE_MENU_PITCH + text_top_at(scale) + (int32_t)lroundf(shift));
        paint(l);
    }
}

static void highlight(int index)
{
    if (index != s_highlight) ui_tick();  /* each item reaching the pill */
    s_highlight = index;
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *l = s_labels[s_items[i]];  /* by position in the list, not by item */
        int32_t now = (int32_t)(intptr_t)lv_obj_get_user_data(l);
        int32_t target = i == index ? 255 : 0;
        /* CSS `transition: color .18s` with the default `ease` curve */
        if (now != target) ui_anim(l, label_mix, now, target, CORE_MENU_COLOUR_MS, 0.25f, 0.1f, 0.25f, 1.0f);
    }
}

static void list_y(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
    fisheye();
}

static void snap_to(int index)
{
    s_sel = index;
    /* magnetic: a touch past the pill, then caught (was the design's plain ease) */
    ui_anim(s_list, list_y, lv_obj_get_style_y(s_list, 0), core_menu_y(index), CORE_MENU_SNAP_MS + 60, CORE_MENU_SNAP_EASE);
    highlight(index);
}

void menu_create(lv_obj_t *parent)
{
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    lv_obj_t *pill = ui_plain(s_root);
    lv_obj_set_pos(pill, (CORE_SCREEN - 280) / 2, 151);
    lv_obj_set_size(pill, 280, 58);  /* the same pill on every screen (Darren, 28 Oct 2026) */
    lv_obj_set_style_radius(pill, 29, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(UI_TEAL), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);

    s_list = ui_plain(s_root);
    const lv_font_t *f = &font_inter_light_36;
    for (int i = 0; i < ITEMS_ALL; i++) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(l, NAMES[i]);
        lv_obj_set_style_text_font(l, f, 0);
        s_labels[i] = l;
    }
    ui_menu_radio(false);
}

/* ---- Music off: ask first ---------------------------------------------------- */
static const ui_ask_t MUSIC_OFF = {.title = "Music off?",
                                   .button = "Turn off",
                                   .yes = ui_music_off,
                                   .count_ms = CORE_MUSIC_OFF_COUNT_MS,
                                   .counting = "Turning off"};

/* The one definition of the question, so the menu's Music off and Now
 * Playing's swipe in over the right bezel are the same thing: same wording,
 * same countdown, same ui_music_off at the end of it. */
void ui_ask_music_off(void) { ui_ask(&MUSIC_OFF); }

/* Lay out the items shown, keeping the one in the pill. */
void ui_menu_radio(bool available)
{
    int keep = s_count ? s_items[s_sel] : ITEM_RECENT;
    s_radio = available;
    s_count = 0;
    for (int i = 0; i < ITEMS_ALL; i++) {
        bool shown = i != ITEM_RADIO || s_radio;
        if (shown) s_items[s_count++] = i;
        lv_obj_t *l = s_labels[i];
        if (shown) lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    }
    /* CSS centres the font's content area in the 60px line box */
    s_text_top = text_top(&font_inter_light_36);
    s_sel = s_highlight = index_of(keep);
    lv_obj_set_size(s_list, CORE_SCREEN, s_count * CORE_MENU_PITCH);
    lv_obj_set_pos(s_list, 0, core_menu_y(s_sel));
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *l = s_labels[s_items[i]];
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, i * CORE_MENU_PITCH + s_text_top);
        lv_anim_delete(l, label_mix);
        lv_obj_set_user_data(l, (void *)(intptr_t)(i == s_sel ? 255 : 0));
    }
    fisheye();
}

lv_obj_t *menu_root(void) { return s_root; }

void menu_press(void)
{
    /* Like removing the CSS transition: a snap in progress jumps to its end. */
    lv_anim_delete(s_list, list_y);
    list_y(s_list, core_menu_y(s_sel));
}

void menu_drag(float dy)
{
    int h;
    float y = core_menu_drag(s_sel, dy, s_count, &h);
    list_y(s_list, (int32_t)lroundf(y));
    if (h != s_highlight) highlight(h);
}

void menu_release(float dy, bool moved)
{
    if (moved) {
        snap_to(core_menu_release(s_sel, dy, s_count));
        return;
    }
    if (s_items[s_sel] == ITEM_ALBUMS) ui_show(UI_VIEW_ALBUMS);
    if (s_items[s_sel] == ITEM_SEARCH) search_open();
    if (s_items[s_sel] == ITEM_PLAYLISTS) playlists_open();
    if (s_items[s_sel] == ITEM_RECENT) recent_open();
    if (s_items[s_sel] == ITEM_SETTINGS) settings_open();
    if (s_items[s_sel] == ITEM_OFF) ui_ask_music_off();
}

void menu_goto(int index) { snap_to(index < 0 ? 0 : index >= s_count ? s_count - 1 : index); }

/* One item per click, with the same snap as a drag. */
void menu_knob(int steps) { menu_goto(s_sel + steps); }
