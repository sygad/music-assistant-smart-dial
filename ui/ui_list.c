/* Two-line lists that move like the menu (see ui_list.h). Lifted from the search
 * results, 27 Oct 2026, so Playlists and later lists behave the same. */
#include "ui_list.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PILL_W 280  /* the menu's pill, the same on every screen (Darren, 28 Oct 2026) */
#define PILL_ONE 58  /* a name on its own: the menu's pill exactly */
#define PILL_TWO 80  /* with a detail line under it */
#define ROW_W 236  /* inside the pill, with room for its rounded ends */
#define TEXT_MAX 96

struct ui_list {
    lv_obj_t *pill, *list;
    lv_obj_t **rows, **titles, **details;
    char (*title_text)[TEXT_MAX], (*detail_text)[TEXT_MAX];  /* the whole lines, for scrolling */
    const lv_font_t **tfont, **dfont;  /* the size each line is already at */
    uint8_t *lit;  /* 255 for the row in the pill */
    lv_obj_t *heading;  /* fades with the position, see list_heading */
    int max, count, sel, highlight;
    int pitch;  /* a menu row for names alone, taller when a detail line goes under them */
    int8_t axis;  /* this gesture's direction once it has one: 0 undecided, 1 down, -1 across */
    float detent;
};

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *f, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_GREY), 0);
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

ui_list_t *list_create(lv_obj_t *parent, int max)
{
    ui_list_t *l = lv_malloc_zeroed(sizeof *l);
    l->max = max;
    l->pitch = CORE_MENU_PITCH;  /* list_fill raises it if the rows carry a detail line */
    l->detent = CORE_MENU_DETENT;
    l->rows = lv_malloc_zeroed(max * sizeof(lv_obj_t *));
    l->titles = lv_malloc_zeroed(max * sizeof(lv_obj_t *));
    l->details = lv_malloc_zeroed(max * sizeof(lv_obj_t *));
    l->title_text = lv_malloc_zeroed(max * TEXT_MAX);
    l->detail_text = lv_malloc_zeroed(max * TEXT_MAX);
    l->lit = lv_malloc_zeroed(max);
    l->tfont = lv_malloc_zeroed(max * sizeof(const lv_font_t *));
    l->dfont = lv_malloc_zeroed(max * sizeof(const lv_font_t *));

    l->pill = ui_plain(parent);
    lv_obj_set_pos(l->pill, (CORE_SCREEN - PILL_W) / 2, (CORE_SCREEN - PILL_ONE) / 2);
    lv_obj_set_size(l->pill, PILL_W, PILL_ONE);  /* resized by list_fill for what's in it */
    lv_obj_set_style_radius(l->pill, PILL_ONE / 2, 0);
    lv_obj_set_style_bg_color(l->pill, lv_color_hex(UI_TEAL), 0);
    lv_obj_set_style_bg_opa(l->pill, LV_OPA_COVER, 0);

    l->list = ui_plain(parent);
    lv_obj_set_user_data(l->list, l);
    lv_obj_set_size(l->list, CORE_SCREEN, max * l->pitch);
    for (int i = 0; i < max; i++) {
        /* a row holds both lines, so the fisheye sizes them together; each line
         * sits in a box that clips it, so the one in the pill can scroll */
        l->rows[i] = ui_plain(l->list);
        lv_obj_set_pos(l->rows[i], (CORE_SCREEN - ROW_W) / 2, i * l->pitch);
        lv_obj_set_size(l->rows[i], ROW_W, l->pitch);
        lv_obj_t *box = ui_plain(l->rows[i]);
        lv_obj_set_pos(box, 0, 3);
        lv_obj_set_size(box, ROW_W, lv_font_get_line_height(&font_inter_regular_24));
        l->titles[i] = text(box, &font_inter_regular_24, ROW_W);
        box = ui_plain(l->rows[i]);
        lv_obj_set_pos(box, 20, 32);
        lv_obj_set_size(box, ROW_W - 40, lv_font_get_line_height(&font_inter_regular_18));
        l->details[i] = text(box, &font_inter_regular_18, ROW_W - 40);
        show(l->rows[i], false);
    }
    return l;
}

/* ---- Fisheye ------------------------------------------------------------------ */
/* The sizes are real fonts, one per pixel, picked by how far the row is from
 * the pill. Scaling one font with a transform instead made LVGL draw each row
 * through a layer buffer it allocated and composited every frame: 33 ms of
 * every frame on the knob (measured on the menu, 28 Oct 2026). Three ladders,
 * because a row is either a title over a detail line, or a single line at the
 * menu's size. */
/* A row's title is the menu's font at the menu's size, so a list and the menu
 * read as one design (Darren, 28 Oct 2026). The row is taller to carry it
 * (l->pitch), the detail line keeps its own size under it, and a row
 * with no detail is simply the title on its own. */
#define TITLE_PX   36
#define DETAIL_PX  18
#define SINGLE_PX  36

static const lv_font_t *const DETAIL_FONTS[] = {  /* Regular 11..18 */
    &font_inter_regular_11, &font_inter_regular_12, &font_inter_regular_13, &font_inter_regular_14,
    &font_inter_regular_15, &font_inter_regular_16, &font_inter_regular_17, &font_inter_regular_18};
static const lv_font_t *const SINGLE_FONTS[] = {  /* Light 22..36, the menu's own ladder */
    &font_inter_light_22, &font_inter_light_23, &font_inter_light_24, &font_inter_light_25,
    &font_inter_light_26, &font_inter_light_27, &font_inter_light_28, &font_inter_light_29,
    &font_inter_light_30, &font_inter_light_31, &font_inter_light_32, &font_inter_light_33,
    &font_inter_light_34, &font_inter_light_35, &font_inter_light_36};

static const lv_font_t *pick(const lv_font_t *const *table, int lo, int hi, int full, float scale)
{
    int px = (int)lroundf(full * scale);
    px = px < lo ? lo : px > hi ? hi : px;
    return table[px - lo];
}

/* Size a row's lines for where it is now, the pair centred on the row as the
 * transform's middle pivot used to leave them. */
/* Only when the size actually changes.
 *
 * Setting a label's font re-measures its text and works out where to put the
 * "...", which costs far more on a playlist's name than on a menu item's one
 * word. Doing it to both lines of every row on every frame made dragging a
 * list feel heavy next to the menu (Darren, 28 Oct 2026). A row only changes
 * size every few frames, and a row with no detail line never touches the
 * second label at all. */
static void row_fonts(ui_list_t *l, int i, float scale)
{
    bool single = !l->detail_text[i][0];
    const lv_font_t *tf = pick(SINGLE_FONTS, 22, 36, TITLE_PX, scale);  /* the menu's own */
    const lv_font_t *df = single ? NULL : pick(DETAIL_FONTS, 11, 18, DETAIL_PX, scale);
    if (tf == l->tfont[i] && df == l->dfont[i]) return;
    l->tfont[i] = tf;
    l->dfont[i] = df;

    int tlh = lv_font_get_line_height(tf);
    int dlh = df ? lv_font_get_line_height(df) : 0;

    lv_obj_set_style_text_font(l->titles[i], tf, 0);
    lv_obj_set_height(l->titles[i], tlh);
    lv_obj_set_height(lv_obj_get_parent(l->titles[i]), tlh);
    if (!df) return;  /* a name on its own: the second line is empty and stays put */
    lv_obj_set_style_text_font(l->details[i], df, 0);
    lv_obj_set_height(l->details[i], dlh);
    lv_obj_set_height(lv_obj_get_parent(l->details[i]), dlh);
}

/* Where a row's lines sit, from the exact scale rather than the whole-pixel
 * font picked for it: the glyphs step, their place doesn't (Darren, 28 Oct
 * 2026: the text jiggled as it crossed between sizes). Cheap enough to do on
 * every frame, unlike setting a font. */
static void row_place(ui_list_t *l, int i, float scale)
{
    bool single = !l->detail_text[i][0];
    float tlh = lv_font_get_line_height(&font_inter_light_36) * scale;
    float dlh = single ? 0.0f : lv_font_get_line_height(&font_inter_regular_18) * scale;
    float top = (l->pitch - (tlh + dlh)) / 2.0f;
    lv_obj_set_y(lv_obj_get_parent(l->titles[i]), (int32_t)lroundf(top));
    if (!single) lv_obj_set_y(lv_obj_get_parent(l->details[i]), (int32_t)lroundf(top + tlh));
}

/* Row colours: white in the pill; outside it the fisheye's grey for the
 * distance, the detail line a quarter darker. */
static void row_paint(ui_list_t *l, int i, float d)
{
    int g = core_fisheye_grey(d), gd = g * 3 / 4;
    lv_color_t grey = lv_color_make((uint8_t)g, (uint8_t)g, (uint8_t)g);
    lv_color_t dim = lv_color_make((uint8_t)gd, (uint8_t)gd, (uint8_t)gd);
    lv_obj_set_style_text_color(l->titles[i], lv_color_mix(lv_color_hex(UI_WHITE), grey, l->lit[i]), 0);
    lv_obj_set_style_text_color(l->details[i], lv_color_mix(lv_color_hex(0xD8E8EC), dim, l->lit[i]), 0);
}

/* Size, shade and space every row by where it is now, relative to the pill, as the menu. */
static void fisheye(ui_list_t *l)
{
    /* the position just set, not the laid-out one, which only catches up on
     * the next frame (the first results were sized for the old position) */
    int32_t top = lv_obj_get_style_y(l->list, 0);
    for (int i = 0; i < l->count; i++) {
        float centre = (float)(top + i * l->pitch + l->pitch / 2);
        float ds = (centre - CORE_SCREEN / 2.0f) / l->pitch, d = fabsf(ds);
        float scale = core_fisheye_scale(d);
        row_fonts(l, i, scale);
        row_place(l, i, scale);
        float shift = (core_fisheye_offset(ds) - ds) * l->pitch;  /* closer together as they shrink */
        lv_obj_set_y(l->rows[i], i * l->pitch + (int32_t)lroundf(shift));
        row_paint(l, i, d);
    }
    if (l->heading) {
        float pos = (float)(core_menu_y_at(0, l->pitch) - top) / l->pitch;  /* 0 on the first item, 1 on the second */
        float k = pos <= 0 ? 1 : pos >= 1 ? 0 : 1 - pos;
        lv_obj_set_style_opa(l->heading, (lv_opa_t)lroundf(255 * k), 0);
        /* faded right out it's hidden: LVGL would otherwise still build a layer for it */
        if (k <= 0) lv_obj_add_flag(l->heading, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(l->heading, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- Scrolling lines ----------------------------------------------------------- */
/* The width of the box a line sits in, as set: the laid-out width is still 0
 * when a list is filled the moment it's built (Playlists). */
static int32_t box_w(lv_obj_t *label) { return lv_obj_get_style_width(lv_obj_get_parent(label), 0); }

/* A line too long for its box scrolls while its row is in the pill, like the
 * album titles (core_marquee); otherwise it ends in "...". */
static void marquee_exec(void *label, int32_t t)
{
    int over = lv_obj_get_style_width(label, 0) - box_w(label);
    lv_obj_set_x(label, (int32_t)lroundf(core_marquee_offset(over, (uint32_t)t)));
}

static void line_rest(lv_obj_t *label)
{
    lv_anim_delete(label, marquee_exec);
    lv_obj_set_x(label, 0);
    lv_obj_set_width(label, box_w(label));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    ui_reading_done(label);
}

/* `full` is the whole line: once LVGL has shortened a label it holds the
 * "..." in its own text, which would measure as fitting. */
static uint32_t line_scroll(lv_obj_t *label, const char *full)  /* returns one pass, 0 if it fits */
{
    lv_point_t size;
    lv_text_get_size(&size, full, lv_obj_get_style_text_font(label, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int box = box_w(label), over = size.x - box;
    if (over <= 0) return 0;
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(label, full);  /* puts back what the "..." replaced */
    lv_obj_set_width(label, size.x);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, marquee_exec);
    uint32_t cycle = core_marquee_cycle_ms(over);
    lv_anim_set_values(&a, 0, (int32_t)cycle);
    lv_anim_set_duration(&a, cycle);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    return cycle;
}

static void highlight(ui_list_t *l, int index)
{
    if (index != l->highlight) {
        ui_tick();
        if (l->highlight >= 0 && l->highlight < l->count) {
            line_rest(l->titles[l->highlight]);
            line_rest(l->details[l->highlight]);
        }
    }
    l->highlight = index;
    for (int i = 0; i < l->count; i++) l->lit[i] = i == index ? 255 : 0;
    fisheye(l);
}

/* The row in the pill starts its scroll (after the pause) only once the list
 * has come to rest, so the pause is all there after landing (Darren, 27 Oct). */
static void scroll_selected(ui_list_t *l)
{
    int s = l->sel;
    if (s < 0 || s >= l->count || lv_anim_get(l->titles[s], marquee_exec) || lv_anim_get(l->details[s], marquee_exec))
        return;  /* already scrolling */
    uint32_t t = line_scroll(l->titles[s], l->title_text[s]);
    uint32_t d = line_scroll(l->details[s], l->detail_text[s]);
    if (t || d) ui_reading(l->titles[s], CORE_CLOCK_READ_PASSES * (t > d ? t : d));
}

/* ---- Moving ------------------------------------------------------------------------ */
static void list_y(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
    fisheye(lv_obj_get_user_data(obj));
}

static void snapped(lv_anim_t *a) { scroll_selected(lv_obj_get_user_data(a->var)); }

static void snap_to(ui_list_t *l, int index)
{
    l->sel = index;
    highlight(l, index);
    ui_anim(l->list, list_y, lv_obj_get_style_y(l->list, 0), core_menu_y_at(index, l->pitch), CORE_MENU_SNAP_MS + 60,
            CORE_MENU_SNAP_EASE);
    lv_anim_t *a = lv_anim_get(l->list, list_y);
    if (a) lv_anim_set_completed_cb(a, snapped);
    else scroll_selected(l);
}

void list_fill(ui_list_t *l, int n, const char *const *title, const char *const *detail, int sel)
{
    l->count = n > l->max ? l->max : n;

    /* A name on its own is a menu row, pill and all; a detail line under it
     * needs the height for both (Darren, 28 Oct 2026). */
    bool two_line = false;
    for (int i = 0; i < l->count && !two_line; i++) two_line = detail[i] && detail[i][0];
    l->pitch = two_line ? CORE_LIST_PITCH : CORE_MENU_PITCH;
    int pill_h = two_line ? PILL_TWO : PILL_ONE;
    lv_obj_set_pos(l->pill, (CORE_SCREEN - PILL_W) / 2, (CORE_SCREEN - pill_h) / 2);
    lv_obj_set_size(l->pill, PILL_W, pill_h);
    lv_obj_set_style_radius(l->pill, pill_h / 2, 0);
    lv_obj_set_size(l->list, CORE_SCREEN, l->max * l->pitch);
    for (int i = 0; i < l->max; i++) {
        l->tfont[i] = l->dfont[i] = NULL;  /* sizes are set again for the new rows */
        lv_obj_set_pos(l->rows[i], (CORE_SCREEN - ROW_W) / 2, i * l->pitch);
        lv_obj_set_size(l->rows[i], ROW_W, l->pitch);
    }

    for (int i = 0; i < l->max; i++) {
        bool on = i < l->count;
        show(l->rows[i], on);
        line_rest(l->titles[i]);
        line_rest(l->details[i]);
        if (!on) continue;
        snprintf(l->title_text[i], TEXT_MAX, "%s", title[i]);
        snprintf(l->detail_text[i], TEXT_MAX, "%s", detail[i] ? detail[i] : "");
        /* the sizes and the places they sit are row_fonts' job, as the fisheye
         * changes them with every frame */
        lv_label_set_text(l->titles[i], l->title_text[i]);
        lv_label_set_text(l->details[i], l->detail_text[i]);
    }
    l->sel = sel < 0 ? 0 : sel >= l->count ? (l->count ? l->count - 1 : 0) : sel;
    l->highlight = -1;
    lv_anim_delete(l->list, list_y);
    list_y(l->list, core_menu_y_at(l->sel, l->pitch));
    highlight(l, l->sel);
    scroll_selected(l);
}

void list_show(ui_list_t *l, bool on)
{
    show(l->pill, on);
    show(l->list, on);
}

int list_count(const ui_list_t *l) { return l->count; }
int list_selected(const ui_list_t *l) { return l->sel; }

void list_press(ui_list_t *l)
{
    l->axis = 0;
    lv_anim_delete(l->list, list_y);
    list_y(l->list, core_menu_y_at(l->sel, l->pitch));
}

/* Whether this gesture is the list's, decided once and then kept.
 *
 * Asking "is dy bigger than dx" on every sample instead made a swipe stall:
 * a finger starts off wandering, so the first samples are often more across
 * than down, and the list sat still until the downward part clearly won. It
 * felt like it had to be shoved (Darren, 28 Oct 2026). Now the first sample
 * past the drag threshold settles it and the rest of the gesture follows. */
bool list_vertical(ui_list_t *l, float dx, float dy)
{
    if (!l->axis && core_is_drag(dx, dy)) l->axis = fabsf(dy) >= fabsf(dx) ? 1 : -1;
    /* undecided still follows: the movement is tiny either way */
    return l->axis >= 0;
}

void list_drag(ui_list_t *l, float dy)
{
    int h;
    float y = core_list_drag_at(l->sel, dy, l->count, l->detent, &h, l->pitch);
    list_y(l->list, (int32_t)lroundf(y));
    if (h != l->highlight) highlight(l, h);
}

bool list_release(ui_list_t *l, float dy, bool moved)
{
    if (!moved) return l->count > 0;
    snap_to(l, core_menu_release_at(l->sel, dy, l->count, l->pitch));
    return false;
}

void list_settle(ui_list_t *l) { snap_to(l, l->sel); }

void list_knob(ui_list_t *l, int steps)
{
    if (!l->count) return;
    int i = l->sel + steps;
    snap_to(l, i < 0 ? 0 : i >= l->count ? l->count - 1 : i);
}

int list_row_at(const ui_list_t *l, int y)
{
    /* the positions just set by the fisheye, as it reads them: the laid-out
     * ones only catch up on the next frame */
    int32_t top = lv_obj_get_style_y(l->list, 0);
    for (int i = 0; i < l->count; i++) {
        float centre = (float)(top + lv_obj_get_style_y(l->rows[i], 0) + l->pitch / 2);
        if (fabsf(centre - (float)y) <= l->pitch / 2.0f) return i;
    }
    return -1;  /* between rows, or away from the list altogether */
}

void list_detent(ui_list_t *l, float k) { l->detent = k < 0 ? 0 : k > 1 ? 1 : k; }

void list_heading(ui_list_t *l, lv_obj_t *band)
{
    l->heading = band;
    fisheye(l);
}

bool list_heading_shown(const ui_list_t *l)
{
    return !l->heading || (!lv_obj_has_flag(l->heading, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_style_opa(l->heading, 0) >= LV_OPA_50);
}
