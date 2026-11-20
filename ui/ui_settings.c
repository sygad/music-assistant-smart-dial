/* Settings (Darren, 28 Oct 2026): the knob's own details, the way IoT devices
 * show them — what's left in the battery, the MAC and IP addresses, and which
 * firmware is on it. Asked for after the battery surprises, so the charge is
 * the first line. Menu -> Settings, the item before Music off.
 *
 * Laid out like a row of Playlists (Darren, 28 Oct 2026, so it looks like the
 * rest of the knob), with the sizes the other way round (Darren: the reading
 * matters more than the word for it): the name first, in the small pale blue
 * of a list's second line, and the value under it in white, in a list title's
 * size. Nothing to choose, so there's no pill, no fisheye and no scrolling:
 * every row stays at full brightness and a swipe down goes back to the menu.
 * The knob pages (settings_page_knob, 16 Nov 2026): Settings is two pages now,
 * and a turn moves between them.
 *
 * Two of the three names have since gone (Darren, 16 Nov 2026): a battery with
 * a level in it and a dotted quad say what they are without being told, and
 * the words were taking the room the things needed. Firmware keeps its name,
 * because "0.9.299" on its own is a number from nowhere. What is left - the
 * pips, the battery, the address, the firmware pair - is spaced equally down
 * the glass, which is why the battery sits lower than it used to: it is no
 * longer tucked under a word, it is one of four things sharing the height. No heading either: a list's heading is
 * there to introduce a list that then moves away from it, and with nothing
 * moving, four rows and a heading left no air between the pairs.
 *
 * The charge has a battery beside it, filled to the level it reads; while it's
 * charging the bolt takes the place of the level, so neither is lost in the
 * other (the figure says the level anyway).
 *
 * The values come from the firmware (ui_device_init, ui_device_battery), which
 * is asked for them as the screen opens (ui_device_source) rather than keeping
 * them up to date behind the music. They are "--" until it says. The simulator
 * stands in with plausible ones. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

LV_IMAGE_DECLARE(img_bolt);

/* Three things worth knowing, spread evenly down the screen: what's left in
 * the battery, the address it answers on, and what's running. The MAC went
 * (Darren, 28 Oct 2026): the address is the useful one, and the MAC was wide
 * enough to hold the size of everything else down.
 *
 * The type is the knob's own, one standard across every screen: the reading in
 * Inter Light 36, the menu's own size, over the name in the Regular 18 that a
 * list's second line uses. */
enum { ROW_BATTERY, ROW_IP, ROW_FIRMWARE, ROWS };

#define ROW_W     290  /* the longest address at that size, with room to spare */
#define NAME_PAD   20
/* The four things on the glass - the pips, then the three readings - share the
 * height equally: same outer margins, same gap between each pair (Darren's
 * standing rule). The band (SETTINGS_BLOCK_TOP..BOT, shared with Connections)
 * is the one the three rows and their names used to fill, so the screen weighs
 * the same as it did; only what is inside it has changed. Each element is
 * measured by its line box rather than by its ink, so the battery and the
 * address - the same size of digit - get the same air. */
#define ELEMS 4  /* pips, battery, address, firmware pair */

/* The page pips: which of the two Settings pages this is, across the top, where
 * the word Battery used to be. The rim said this until 16 Nov 2026, as a
 * segment of a circle - a twelfth, and before that a half - and neither sat
 * right over a screen with no other arc on it. Dots do say it, because a row of
 * dots is only ever a position (Darren). */
#define PIP_GAP 13

/* The battery: a body with a nub, and inside it either the level or the bolt. */
#define BAT_W      50
#define BAT_H      26
#define BAT_BORDER  2
#define BAT_INSET   4  /* the border, plus a little air */
#define NUB_W       3
#define NUB_H      11
#define ICON_W     (BAT_W + NUB_W)
#define ICON_GAP   10  /* between the battery and the figure */

#define UNKNOWN "--"

static lv_obj_t *s_root, *s_value[ROWS], *s_icon, *s_level, *s_bolt, *s_body;
static settings_pips_t s_pips;
static char s_mac[24], s_ip[24], s_firmware[24];  /* the MAC is kept but not shown */
static void (*s_read)(void);  /* the firmware's reader, run as the screen opens */
static int s_battery = -1;
static bool s_charging;

/* One line of a row, as ui_list.c builds them: centred, clipped to its box. */
static lv_obj_t *line(lv_obj_t *parent, const lv_font_t *f, int w, uint32_t colour, int y, int x)
{
    lv_obj_t *box = ui_plain(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, lv_font_get_line_height(f));
    lv_obj_t *l = lv_label_create(box);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));  /* one line, "..." when too long */
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

static void build_battery(lv_obj_t *row)
{
    s_icon = ui_plain(row);
    lv_obj_set_size(s_icon, ICON_W, BAT_H);

    s_body = ui_plain(s_icon);
    lv_obj_set_pos(s_body, 0, 0);
    lv_obj_set_size(s_body, BAT_W, BAT_H);
    lv_obj_set_style_radius(s_body, 6, 0);
    lv_obj_set_style_border_width(s_body, BAT_BORDER, 0);
    lv_obj_set_style_border_color(s_body, lv_color_hex(UI_WHITE), 0);

    lv_obj_t *nub = ui_plain(s_icon);
    lv_obj_set_pos(nub, BAT_W, (BAT_H - NUB_H) / 2);
    lv_obj_set_size(nub, NUB_W, NUB_H);
    lv_obj_set_style_radius(nub, 1, 0);
    lv_obj_set_style_bg_color(nub, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);

    s_level = ui_plain(s_icon);
    lv_obj_set_pos(s_level, BAT_INSET, BAT_INSET);
    lv_obj_set_style_radius(s_level, 2, 0);
    lv_obj_set_style_bg_color(s_level, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_bg_opa(s_level, LV_OPA_COVER, 0);

    s_bolt = lv_image_create(s_icon);
    lv_image_set_src(s_bolt, &img_bolt);
    lv_obj_set_style_image_recolor(s_bolt, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_image_recolor_opa(s_bolt, LV_OPA_COVER, 0);
    lv_obj_align_to(s_bolt, s_body, LV_ALIGN_CENTER, 0, 0);
}

/* The battery and the figure side by side, the pair centred in the row. */
static void place_battery(void)
{
    bool known = s_battery >= 0 || s_charging;
    if (known) lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);

    int inner = BAT_W - 2 * BAT_INSET;
    int level = s_battery <= 0 ? 0 : (s_battery * inner + 50) / 100;
    if (level && level < 3) level = 3;  /* nearly empty still shows something */
    lv_obj_set_size(s_level, level, BAT_H - 2 * BAT_INSET);
    bool bolt = known && s_charging;
    if (bolt) lv_obj_remove_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
    if (bolt || !level) lv_obj_add_flag(s_level, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_level, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_get_parent(s_value[ROW_BATTERY]);
    lv_obj_update_layout(s_root);
    int32_t text = lv_obj_get_width(box);
    int32_t group = known ? ICON_W + ICON_GAP + text : text;
    int32_t x = (ROW_W - group) / 2;
    /* Inside its own row now, so the reading starts at the top of it and the
     * battery is centred on the digits beside it. */
    lv_obj_set_pos(s_icon, x, (lv_font_get_line_height(&font_inter_light_36) - BAT_H) / 2);
    lv_obj_set_pos(box, known ? x + ICON_W + ICON_GAP : x, 0);
}

static void refresh(void)
{
    if (!s_root) return;
    /* On the charger the figure would be the rail, not the cell, so there is
     * no honest number to show: the battery and its bolt say it's charging and
     * that's the whole story (Darren, 28 Oct 2026). A figure appears when it
     * runs on its own, which is the only time the cell can be read. */
    char charge[8];
    snprintf(charge, sizeof charge, "%d%%", s_battery);
    const char *values[ROWS] = {s_charging ? "" : s_battery >= 0 ? charge : UNKNOWN, s_ip, s_firmware};
    for (int i = 0; i < ROWS; i++)
        lv_label_set_text(s_value[i], i == ROW_BATTERY ? values[i] : values[i][0] ? values[i] : UNKNOWN);
    place_battery();
}

void settings_create(lv_obj_t *parent)
{
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    const int name_h = lv_font_get_line_height(&font_inter_regular_18);
    const int value_h = lv_font_get_line_height(&font_inter_light_36);
    /* pips, battery, address, firmware (its name over its value) */
    const int height[ELEMS] = {SETTINGS_PIP_D, value_h, value_h, name_h + value_h};
    int used = 0;
    for (int i = 0; i < ELEMS; i++) used += height[i];
    const int gap = (SETTINGS_BLOCK_BOT - SETTINGS_BLOCK_TOP - used) / (ELEMS - 1);

    settings_pips_create(&s_pips, s_root);  /* which puts them at the top of the band */
    int y = SETTINGS_BLOCK_TOP + height[0] + gap;

    for (int i = 0; i < ROWS; i++) {
        lv_obj_t *row = ui_plain(s_root);
        lv_obj_set_pos(row, (CORE_SCREEN - ROW_W) / 2, y);
        lv_obj_set_size(row, ROW_W, height[i + 1]);
        /* Only the firmware is named: a level in a battery and a dotted quad
         * are their own labels, a version number is not (Darren, 16 Nov 2026). */
        int value_y = 0;
        if (i == ROW_FIRMWARE) {
            lv_obj_t *name = line(row, &font_inter_regular_18, ROW_W - 2 * NAME_PAD, 0xD8E8EC, 0, NAME_PAD);
            lv_label_set_text(name, "Firmware");
            value_y = name_h;
        }
        s_value[i] = line(row, &font_inter_light_36, ROW_W, UI_WHITE, value_y, 0);
        /* the charge sits beside its battery, so that line is only as wide as
         * its own text and the two are placed together (place_battery) */
        if (i == ROW_BATTERY) {
            lv_obj_set_width(s_value[i], LV_SIZE_CONTENT);
            lv_obj_set_width(lv_obj_get_parent(s_value[i]), LV_SIZE_CONTENT);
            build_battery(row);
        }
        y += height[i + 1] + gap;
    }
    refresh();
}

lv_obj_t *settings_root(void) { return s_root; }

void settings_open(void) { ui_show(UI_VIEW_SETTINGS); }

void settings_enter(void)
{
    ui_overlay_arc(false, UI_WHITE, 0, 0);  /* the pips say which page; the rim says nothing */
    settings_pips_set(&s_pips, SETTINGS_PAGE_SETTINGS);
    if (s_read) s_read();  /* fills the values in, which refreshes as it goes */
    refresh();
}

/* ---- What the firmware tells it ---------------------------------------------- */
void ui_device_source(void (*read)(void)) { s_read = read; }

void ui_device_init(const char *mac, const char *ip, const char *firmware)
{
    snprintf(s_mac, sizeof s_mac, "%s", mac ? mac : "");
    snprintf(s_ip, sizeof s_ip, "%s", ip ? ip : "");
    snprintf(s_firmware, sizeof s_firmware, "%s", firmware ? firmware : "");
    refresh();
}

void ui_device_battery(int percent, bool charging)
{
    s_battery = percent < 0 ? -1 : percent > 100 ? 100 : percent;
    s_charging = charging;
    refresh();
}

/* ---- The two pages (Darren, 16 Nov 2026) --------------------------------------
 * "I can only get to it by swiping. I would expect to be able to rotate the
 * dial and see the arc segment, as on previous screens, that was a denotation
 * of multiple tracks within an album. This can now be a denotation of multiple
 * sections within a category."
 *
 * The knob moves between them, a page per flick, and stops at either end rather
 * than wrapping, as every list on the dial does.
 *
 * Which page it is was on the rim for an afternoon and is now two dots across
 * the top, the lit one on the left for this page and on the right for
 * Connections. The rim had it first as a segment of the whole, the way Now
 * Playing marks a track in an album, and two pages honestly drawn made each one
 * a semicircle: not a marker but a state, "overpowering and stupid". Faking it
 * at a twelfth made it the right size and the wrong thing - a short arc hanging
 * off the bottom of a screen with nothing else on the rim. Dots carry no such
 * meaning: a row of them is a position and nothing else (Darren, 16 Nov 2026).
 *
 * They live here, in the first page, and Connections builds its own pair from
 * the same two calls, so the two pages cannot drift apart in size, spacing or
 * colour. */
#define PAGES 2

void settings_pips_create(settings_pips_t *p, lv_obj_t *parent)
{
    const int w = PAGES * SETTINGS_PIP_D + (PAGES - 1) * PIP_GAP;
    for (int i = 0; i < PAGES; i++) {
        lv_obj_t *d = ui_plain(parent);
        lv_obj_set_pos(d, (CORE_SCREEN - w) / 2 + i * (SETTINGS_PIP_D + PIP_GAP), SETTINGS_BLOCK_TOP);
        lv_obj_set_size(d, SETTINGS_PIP_D, SETTINGS_PIP_D);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        p->dot[i] = d;
    }
    settings_pips_set(p, SETTINGS_PAGE_SETTINGS);
}

void settings_pips_set(const settings_pips_t *p, int page)
{
    for (int i = 0; i < PAGES; i++)
        lv_obj_set_style_bg_color(p->dot[i], lv_color_hex(i == page ? UI_WHITE : UI_GREY), 0);
}

void settings_page_knob(int steps, int page)
{
    int want = page + (steps > 0 ? 1 : -1);
    if (want < 0 || want >= PAGES) return;
    ui_show(want == SETTINGS_PAGE_STATUS ? UI_VIEW_STATUS : UI_VIEW_SETTINGS);
}

/* ---- Input ------------------------------------------------------------------- */
void settings_release(float dx, float dy, bool moved)
{
    if (ui_touch_back_gesture(dx, dy, moved) ||  /* in over a bezel, see ui.h */
        (moved && dy > CORE_SWIPE_V_PX && fabsf(dy) > fabsf(dx))) { ui_show(UI_VIEW_MENU); return; }
    /* Settings is two pages now: this one, and Connections (ui_status.c, 16 Nov
     * 2026). A swipe left goes on to it, a swipe right there comes back, and
     * down from either goes to the menu - so the pair behaves as one place
     * with a second page rather than as two menu items. */
    if (moved && dx < -CORE_SWIPE_H_PX && fabsf(dx) > fabsf(dy)) ui_show(UI_VIEW_STATUS);
}
