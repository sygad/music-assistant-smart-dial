/* Connections: the second Settings screen (Darren, 16 Nov 2026).
 *
 * "The only time I see it right now is during that initial switch on." The
 * connecting sequence names Wi-Fi, Home Assistant, Music Assistant and the
 * speaker, ticks them off, and is gone in four seconds - and then there is no
 * way to ask the question again without power-cycling the dial. The morning
 * this was asked for, the bathroom streamer had been off the network since
 * 11:13 and every track buffered and skipped in silence; the one screen that
 * would have said so had come and gone hours earlier.
 *
 * So this is that list, standing still, on demand. Deliberately the same list
 * in the same order with the same tick, because it is meant to read as the
 * opening screen held rather than as a new kind of report - the same rows, and
 * the amp added at the end, which the opening cannot show because its wait
 * runs to two minutes.
 *
 * Nothing is kept up to date behind the music: the firmware is asked for a
 * fresh look as the screen opens (ui_status_source), the way Settings asks for
 * the battery. Four requests a minute all day would cost the battery for an
 * answer nobody is reading.
 *
 * The heading said "Checking" while that look was in flight, and on a network
 * this quick the look is over in a frame or two: what it actually did was flash
 * a word nobody could read as the screen arrived, which "looks glitchy"
 * (Darren, 16 Nov 2026). It is gone. The rows carry the last answers - minutes
 * old at the very worst, since the dial rechecks every minute anyway - and the
 * fresh ones replace them without the heading moving. Nothing here is ever
 * unknown in practice: the connecting sequence fills all five in before anyone
 * can reach this screen.
 *
 * A swipe right goes back to Settings, a swipe down goes to the menu, and the
 * knob pages between the two (settings_page_knob), with two dots across the top
 * saying which page this is - the right one lit here (16 Nov 2026).
 */
#include <math.h>
#include <string.h>

#include "ui_internal.h"

LV_IMAGE_DECLARE(img_tick);

/* The four the opening screen shows, in its order, and then the amplifier. */
enum { ROW_WIFI, ROW_HA, ROW_MA, ROW_SPEAKER, ROW_AMP, ROWS };
static const char *const NAMES[ROWS] = {"Wi-Fi", "Home Assistant", "Music Assistant", "Streamer", "Amplifier"};

/* The opening screen's own metrics, so the two read as one thing: its tick,
 * its gap, its row pitch (ui_net.c). Five rows rather than four, centred on
 * the glass under a heading of the same size the opening screen uses. */
#define TICK_W   23   /* img_tick is 23 x 18 */
#define TICK_GAP 13
#define ROW_H    38
#define LABEL_W  126  /* "Music Assistant" at 18px, as ui_net.c measures it */
/* Three things down the glass - the pips, the heading, the five rows - in the
 * band Settings uses, with the same gap between each pair, worked out at build
 * time from the fonts rather than typed. It was typed until 16 Nov 2026, and
 * what that cost was this screen's pips sitting 6 px lower than the other
 * page's, so they hopped as the knob paged between them (Darren). Now the top
 * of the band is where the pips go, on both pages, and nothing here can say
 * otherwise. The heading and the rows follow from it. */

static lv_obj_t *s_root, *s_title;
static settings_pips_t s_pips;
static lv_obj_t *s_tick[ROWS], *s_label[ROWS];
static void (*s_read)(void);        /* the firmware's look, run as the screen opens */
static int s_state[ROWS];           /* -1 not known yet, 0 away, 1 there */

/* A row is its name and, when the thing is there, a green tick beside it
 * (Darren, 16 Nov 2026) - the opening screen's own tick, in the opening
 * screen's own green, because it is the same answer to the same question and
 * white against grey left it reading as a list rather than as five verdicts.
 * White and ticked for there, grey and bare for away, where an unticked row
 * among ticked ones is the whole message. */
static void paint(void)
{
    if (!s_root) return;
    for (int i = 0; i < ROWS; i++) {
        bool there = s_state[i] == 1;
        lv_obj_set_style_opa(s_tick[i], there ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_label[i], lv_color_hex(there ? UI_WHITE : UI_GREY), 0);
    }
}

void status_create(lv_obj_t *parent)
{
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    settings_pips_create(&s_pips, s_root);  /* at the top of the band, as on Settings */

    /* pips, heading, rows: equal gaps, the last row's text ending the band. */
    const int title_h = lv_font_get_line_height(&font_inter_light_28);
    const int rows_h = (ROWS - 1) * ROW_H + lv_font_get_line_height(&font_inter_regular_18);
    const int gap = (SETTINGS_BLOCK_BOT - SETTINGS_BLOCK_TOP - SETTINGS_PIP_D - title_h - rows_h) / 2;
    const int title_y = SETTINGS_BLOCK_TOP + SETTINGS_PIP_D + gap;
    const int rows_y = title_y + title_h + gap;

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, &font_inter_light_28, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_title, "Connections");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, title_y);

    /* Set as one block and centred as one, so the ticks line up under each
     * other instead of each row centring on its own width (ui_net.c). */
    const int left = (CORE_SCREEN - (TICK_W + TICK_GAP + LABEL_W)) / 2;
    for (int i = 0; i < ROWS; i++) {
        s_tick[i] = lv_image_create(s_root);
        lv_image_set_src(s_tick[i], &img_tick);
        lv_obj_set_style_image_recolor(s_tick[i], lv_color_hex(UI_OK), 0);  /* ui_net.c's tick */
        lv_obj_set_style_image_recolor_opa(s_tick[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(s_tick[i], left, rows_y + i * ROW_H + 3);

        lv_obj_t *l = lv_label_create(s_root);
        lv_obj_set_style_text_font(l, &font_inter_regular_18, 0);
        lv_label_set_text(l, NAMES[i]);
        lv_obj_set_pos(l, left + TICK_W + TICK_GAP, rows_y + i * ROW_H);
        s_label[i] = l;

        s_state[i] = -1;
    }
    paint();
}

lv_obj_t *status_root(void) { return s_root; }

void status_open(void) { ui_show(UI_VIEW_STATUS); }

void status_enter(void)
{
    ui_overlay_arc(false, UI_WHITE, 0, 0);  /* the pips say which page; the rim says nothing */
    settings_pips_set(&s_pips, SETTINGS_PAGE_STATUS);
    /* The amp is the one the dial already knows without asking, because it has
     * been watching it since boot (ui_amp). */
    s_state[ROW_AMP] = ui_amp() == UI_AMP_READY ? 1 : 0;
    paint();
    if (s_read) s_read();  /* which lands through ui_status_set, often after a moment */
}

/* ---- What the firmware tells it ---------------------------------------------- */
void ui_status_source(void (*read)(void)) { s_read = read; }

/* `checking` says a look is in flight. Nothing is done with it: the screen
 * showed it as a heading and that heading was the flash (see the top of this
 * file). It stays in the call because the firmware knows it and a future
 * something-is-wrong state may want it. */
void ui_status_set(int wifi, int home_assistant, int music_assistant, int speaker, bool checking)
{
    (void)checking;
    s_state[ROW_WIFI] = wifi;
    s_state[ROW_HA] = home_assistant;
    s_state[ROW_MA] = music_assistant;
    s_state[ROW_SPEAKER] = speaker;
    s_state[ROW_AMP] = ui_amp() == UI_AMP_READY ? 1 : 0;
    paint();
}

/* ---- Input ------------------------------------------------------------------- */
void status_release(float dx, float dy, bool moved)
{
    if (ui_touch_back_gesture(dx, dy, moved)) { ui_show(UI_VIEW_SETTINGS); return; }
    if (!moved) return;
    /* Down to the menu, as from Settings. Right is back to the page this came
     * from, which is the only other place a swipe can go. */
    if (dy > CORE_SWIPE_V_PX && fabsf(dy) > fabsf(dx)) ui_show(UI_VIEW_MENU);
    else if (dx > CORE_SWIPE_H_PX) ui_show(UI_VIEW_SETTINGS);
}
