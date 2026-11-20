/* Screens 06 and 13-15: EQ overview and EQ focus (Option C).
 *
 * Overview: the album art under 75% black, the trident splitting the circle
 * into treble (upper left), bass (upper right) and subs (below); the volume arc
 * stays on the rim and the knob is the volume. Tap a zone to focus it.
 * Focus: that parameter's icon and a big back button; the arc shows its level
 * in its colour and the knob adjusts it, one step per click.
 *
 * Ranges are the Denon's (Darren's Home Assistant helpers): treble and bass
 * -6..+6 dB, subwoofer -12..+12 dB, one dB a step - which is as fine as the amp
 * goes, so a step can't be made smaller, only harder to reach. Treble and bass
 * span half as many dB as the subs over the same arc, so a click used to move
 * their arc twice as far; they now take two clicks per dB (31 Oct 2026), which
 * puts every parameter at 24 clicks end to end and moves the arc the same
 * distance on all three screens. The arc follows the hand on every click, so
 * while a half-dB is pending it sits half a step ahead of the amp.
 *
 * Values are kept on the knob for now; they go to the amp with the Home
 * Assistant connection. Icons and positions are exported from the Figma file
 * (frames 06, 13-15). */
#include <math.h>

#include "assets/assets.h"
#include "ui_internal.h"

#define TREBLE 0x00E58F
#define BASS   0xFF00FB
#define SUBS   0xFF8A1E
#define TRIDENT 0x919191

typedef struct {
    const char *name;
    const lv_image_dsc_t *icon;
    uint32_t colour;
    int min, max;      /* dB */
    int clicks;        /* knob clicks per dB */
    int ticks;         /* the value, in clicks from min */
    int over_x, over_y, focus_x, focus_y;  /* icon positions: overview, focus */
} param_t;

static param_t s_params[3] = {
    {"Treble", &img_treble, TREBLE, -6, 6, 2, 12, 78, 80, 154, 64},
    {"Bass", &img_bass, BASS, -6, 6, 2, 12, 225, 92, 152, 80},
    {"Subs", &img_subs, SUBS, -12, 12, 1, 12, 136, 233, 142, 85},
};

static const ui_library_t *s_lib;
static lv_obj_t *s_root, *s_art, *s_overview, *s_focus, *s_focus_icon;
static int s_album = -1, s_param = -1;  /* -1 = overview */
static int32_t s_level_shown;           /* x1000 of the arc currently drawn */

static lv_obj_t *icon(lv_obj_t *parent, const lv_image_dsc_t *img, uint32_t colour, int x, int y)
{
    lv_obj_t *o = lv_image_create(parent);
    lv_image_set_src(o, img);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_image_recolor(o, lv_color_hex(colour), 0);
    lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
    return o;
}

static int ticks_max(const param_t *p) { return (p->max - p->min) * p->clicks; }

/* The arc follows the clicks, not the whole dB, so every click moves it. */
static float level(const param_t *p) { return (float)p->ticks / (float)ticks_max(p); }

/* What the amp is set to: the nearest whole dB to where the hand has got. */
static int value_db(const param_t *p)
{
    return p->min + (p->ticks + p->clicks / 2) / p->clicks;
}

static void level_exec(void *var, int32_t v)
{
    (void)var;
    s_level_shown = v;
    if (s_param >= 0) ui_overlay_arc(true, s_params[s_param].colour, 0, v / 1000.0f);
}

bool eq_on_overview(void) { return s_param < 0; }

static void rim(void)
{
    if (ui_view() != UI_VIEW_EQ) return;
    if (s_param < 0) np_volume_rim();
    else ui_overlay_arc(true, s_params[s_param].colour, 0, s_level_shown / 1000.0f);
}

static void show_focus(int param)
{
    s_param = param;
    if (param < 0) {
        lv_obj_remove_flag(s_overview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_focus, LV_OBJ_FLAG_HIDDEN);
    } else {
        param_t *p = &s_params[param];
        lv_image_set_src(s_focus_icon, p->icon);
        lv_obj_set_pos(s_focus_icon, p->focus_x, p->focus_y);
        lv_obj_set_style_image_recolor(s_focus_icon, lv_color_hex(p->colour), 0);
        lv_obj_add_flag(s_overview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_focus, LV_OBJ_FLAG_HIDDEN);
        s_level_shown = (int32_t)lroundf(level(p) * 1000);
    }
    rim();
}

void eq_create(lv_obj_t *parent, const ui_library_t *lib)
{
    s_lib = lib;
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);
    s_art = lv_image_create(s_root);
    lv_obj_set_pos(s_art, 0, 0);
    lv_obj_t *scrim = ui_plain(s_root);
    lv_obj_set_size(scrim, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(scrim, UI_OPA_SCRIM, 0);

    s_overview = ui_plain(s_root);
    lv_obj_set_size(s_overview, CORE_SCREEN, CORE_SCREEN);
    icon(s_overview, &img_trident, TRIDENT, 71, 48);
    for (int i = 0; i < 3; i++) icon(s_overview, s_params[i].icon, s_params[i].colour, s_params[i].over_x, s_params[i].over_y);

    s_focus = ui_plain(s_root);
    lv_obj_set_size(s_focus, CORE_SCREEN, CORE_SCREEN);
    s_focus_icon = icon(s_focus, &img_treble, TREBLE, 154, 64);
    lv_obj_t *back = ui_plain(s_focus);  /* Figma: 87, 206, 184 x 85; solid black, white arrow */
    lv_obj_set_pos(back, 87, 206);
    lv_obj_set_size(back, 184, 85);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    icon(back, &img_back, 0xFFFFFF, 0, 0);
    lv_obj_add_flag(s_focus, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *eq_root(void) { return s_root; }

void eq_cover_changed(int album)
{
    if (album != s_album) return;
    const lv_image_dsc_t *img = np_cover();  /* the album's cover, or the playlist track's picture */
    if (img) {
        lv_image_set_src(s_art, img);
        lv_obj_remove_flag(s_art, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_art, LV_OBJ_FLAG_HIDDEN);
    }
}

void eq_open(int album)
{
    s_album = album;
    eq_cover_changed(album);
    show_focus(-1);
    ui_show(UI_VIEW_EQ);
}

void eq_enter(void) { rim(); }

/* Left alone: one step back, the way the back button goes. False on the
 * overview, where there is nowhere left to go but the music, and the idle
 * check takes it from there. */
bool eq_step_back(void)
{
    if (s_param < 0) return false;
    show_focus(-1);
    return true;
}

/* Which zone of the trident a point is in: angle around the junction (178.7, 181.9)
 * from straight up, clockwise; the diagonals run at about -120 and +119 degrees. */
static int zone(int x, int y)
{
    float a = atan2f((float)x - 178.7f, 181.9f - (float)y) * 57.29578f;
    if (a > -120.0f && a <= 0.0f) return 0;  /* treble */
    if (a > 0.0f && a < 119.0f) return 1;    /* bass */
    return 2;                                /* subs */
}

void eq_release(int x, int y, float dx, float dy, bool moved)
{
    if (moved) {
        if (ui_touch_back_gesture(dx, dy, moved) ||  /* in over a bezel, see ui.h */
            (fabsf(dy) > fabsf(dx) && fabsf(dy) > CORE_SWIPE_V_PX && dy > 0)) ui_show(UI_VIEW_NOWPLAYING);  /* drag down */
        return;
    }
    if (s_param < 0) {
        show_focus(zone(x, y));
        ui_tick();
    } else if (x >= 87 && x <= 271 && y >= 206 && y <= 291) {
        show_focus(-1);  /* back */
        ui_tick();
    }
}

void eq_knob(int steps)
{
    if (s_param < 0) {
        np_volume_step(steps);  /* the overview keeps the volume arc, so the knob is the volume */
        return;
    }
    param_t *p = &s_params[s_param];
    int t = p->ticks + steps, top = ticks_max(p);
    t = t < 0 ? 0 : t > top ? top : t;
    if (t == p->ticks) return;
    int was = value_db(p);
    p->ticks = t;
    if (value_db(p) != was) np_eq_changed(s_param, value_db(p));  /* two clicks a dB: only whole ones go out */
    ui_tick();
    ui_anim(&s_level_shown, level_exec, s_level_shown, (int32_t)lroundf(level(p) * 1000), CORE_ARC_MS, 0, 0, 1, 1);
}

int ui_eq_value(int param) { return param >= 0 && param < 3 ? value_db(&s_params[param]) : 0; }

/* Where the amp actually is, read once at start-up: the knob has no business
 * showing 0 dB over an amp sitting at -4, and the first click would then drag
 * it there (31 Oct 2026). Nothing is sent back - this is the amp telling the
 * knob, not the other way round. */
void ui_eq_set(int param, int db)
{
    if (param < 0 || param >= 3) return;
    param_t *p = &s_params[param];
    if (db < p->min) db = p->min;
    if (db > p->max) db = p->max;
    p->ticks = (db - p->min) * p->clicks;
    if (s_param == param) {
        s_level_shown = (int32_t)lroundf(level(p) * 1000);
        ui_overlay_arc(true, p->colour, 0, s_level_shown / 1000.0f);
    }
}
