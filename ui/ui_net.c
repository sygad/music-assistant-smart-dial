/* Coming up: the connecting screens, between the logo and the menu.
 *
 * Waking from deep sleep is a boot: the radio is off, nothing is associated,
 * and for the first few seconds a turn of the knob reaches nothing at all.
 * Darren woke the dial, found himself on the menu and had no way of telling
 * whether it was back: "I have absolutely no idea if that's connected."
 *
 * What that grew into is not a progress report. It is the opening of the
 * thing: logo, then "Connecting to Wi-Fi", then the three ticks with the amp
 * still going underneath, then the menu - each held long enough to be read and
 * each crossfaded into the next. The amp takes up to two minutes to answer and
 * nothing here can hurry it; what these screens do is spend the front of that
 * wait on something that looks like progress, so that by the time he is
 * choosing a track the amp is most of the way there. "What I don't want is
 * these screens just smashing into each other and overwriting each other as
 * fast as you can possibly display them. That just cheapens the whole
 * experience" (Darren, 4 Nov 2026).
 *
 * So every screen has a floor on how long it is up, whether or not the thing
 * it describes has already finished. Wi-Fi is usually up before anyone sees
 * the Wi-Fi screen; it is shown anyway. Nothing claimed here is untrue - the
 * steps all happen - the sequence just refuses to be hurried through them.
 *
 * The rim carries the one thing that outlives all of it. It fades up and winds
 * up to speed as the dial connects, and then drops to half brightness and half
 * speed when the menu arrives, because from there the amp is background work
 * and a bright fast arc over a list is only distracting.
 */
#include "ui_internal.h"

/* One screen, not two. It was "Connecting to Wi-Fi", then that disappearing,
 * then a second screen already finished - and the join between them was pure
 * cost: a crossfade to show a list that had been true the whole time. Darren,
 * 4 Nov 2026: "What if we amalgamated that into one screen that the title was
 * connecting and it showed all the things that it was connecting to? And then
 * when all of them had been connected to successfully, the title then changed
 * to connected."
 *
 * So: the title says Connecting, the three things it is connecting to are
 * listed underneath from the first frame, each gets its tick in turn, and the
 * title becomes Connected when the last one lands. One screen, one
 * progression, and about a second and a half shorter than the pair it
 * replaces. */
#define MIN_MS         700    /* it cannot flash past even if everything is instant */
/* When each tick lands, measured from the moment the network really was ready
 * - not from when the screen appeared. If Wi-Fi is still coming up the rows
 * sit there unticked and the title stays Connecting, which is the truth. It
 * usually is ready already, because the logo has had three seconds. */
/* Seven tenths of a second apart. They were just over half that and they
 * rattled off; at this spacing each one is its own event, which is the whole
 * reason for showing them one at a time. */
/* Four rows since 16 Nov 2026: the speaker joined them. The three services
 * were all up that morning while the streamer was off the network, and a dial
 * that says Connected when nothing can play is worse than one that says
 * nothing at all. */
#define ROWS 4
static const uint32_t TICK_AT[ROWS] = {620, 1340, 2060, 2780};
/* The moment it is all connected, measured from the last tick landing.
 *
 * It was a crossfade from Connecting to Connected in place, and Darren, 5 Nov
 * 2026: "it's quite easy to miss, and it actually looks more like a glitch than
 * it does an update." The two words share seven letters, so only the ending
 * changed and mid-fade "ing" and "ed" sat on top of each other - which is what
 * a drawing fault looks like. And it all happened in the same instant the last
 * tick and the rim were changing too.
 *
 * So it is taken in turn, and given time to be seen:
 *   - the last tick lands, and the spinner on the rim stops where it is and
 *     turns green there
 *   - Connecting lifts away and is gone, while the green arc grows on round
 *     into a closed ring
 *   - a beat of nothing, then Connected rises into place in the same green,
 *     landing as the ring closes
 *   - the two are held together, green word and green ring
 *   - the ring fades as the word settles to white, and the finished screen
 *     rests a moment before it gives way.
 * The ring is the point: the rim is how the dial says everything, and the arc
 * that meant "working on it" closing into one that means "done" is the dial
 * saying it in its own language.
 *
 * Stop, then green, then grow - one at a time. The first version swept the arc
 * round and turned it green in the same movement, still going at the
 * spinner's speed, and the green whipping round into a full ring read as a
 * flash: "as soon as it's connected to green, the arc, whatever position it's
 * in, should then fade to green and just complete as a circle" (Darren, 5 Nov
 * 2026). */
#define SWAP_AT        400    /* after the last tick: the old word goes */
#define WORD_OUT_MS    450
#define WORD_IN_AT     1050   /* ...the new word starts rising, after a beat of empty */
#define WORD_IN_MS     600
#define LIFT           12     /* how far the words travel, in pixels */
#define GLIDE_MS       250    /* the arc coasting to a stop rather than halting dead */
#define GREEN_MS       500    /* ...and turning green where it stopped */
#define GROW_MS        1100   /* then growing on round into a ring, landing with the word */
#define CLOSE_MS       (GREEN_MS + GROW_MS)
#define RING_FADE_AT   2700   /* held together until here */
#define RING_FADE_MS   700
#define AFTER_LAST_MS  3800   /* ...and the finished screen gives way */
/* ...and then the amp gets a screen of its own. It was a grey line at the foot
 * of the connected list, which made no sense there: the list is three things
 * that are done and the amp is one thing that is not. "That now no longer
 * makes any sense on this screen. So I'd like to remove that and we'll handle
 * that separately" (Darren, 4 Nov 2026). */
#define AMP_MS         2600
/* ...and when it finally answers, said once and briefly. Long enough to read
 * four words, not long enough to be in the way. */
#define AMPDONE_MS     2000
/* The failure, when there is nothing left to try: long enough to read twice,
 * then out of the way so the dial can be used and can get some sleep. */
#define PROBLEM_MS     10000
#define FAILED_MIN_MS  6000   /* a fault has to be read and understood */
/* Everything on this screen is paced off the logo in front of it. "You've got
 * the pace right on the animation of the logo, but then it seems to speed up
 * to almost rushed proportions on the preceding screens after it. So take the
 * pace of the logo animation and then bring that across to the other sections"
 * (Darren, 4 Nov 2026). A dissolve rather than a cut, and an unhurried one. */
#define FADE_MS        650
#define STEP_MS        50
/* The arc means one task, and it starts and stops with that task.
 *
 * It used to spin straight through the whole opening, and Darren put his
 * finger on why that was wrong: "the screens almost look fake because the
 * spinning arc is meant to represent it's doing something... If there's no
 * break in the continuous spinning of that, it almost is like, which one is
 * the actual real thing?" (4 Nov 2026). So it winds up while the three are
 * connecting, stops when the last tick lands, and starts again for the amp,
 * which is a different job. The pause is what makes the ticks mean anything. */
#define RAMP_MS        2000   /* up, as the three connect */
#define ARC_IN_MS      1100   /* and back up for the amp, unhurried */
#define SPIN_BRIGHT_LO 70
#define SPIN_SPEED_LO  0.35f
/* ...and backs off once the menu has his attention. */
#define SPIN_BRIGHT_BG 128
#define SPIN_SPEED_BG  0.5f

#define TICK_W   23   /* img_tick is 23 x 18 */
#define TICK_GAP 13
#define ROW_H    38
/* The fourth row made the block taller, so the whole composition moved up by
 * half a row rather than letting it drift down the glass: the title and the
 * rows together sat 92 to 259 with three rows, and sit 73 to 278 with four -
 * the same centre, the same 23 px of air under the title. */
#define TITLE_Y  73
#define ROWS_Y   141

/* Where the sequence has got to. The network's own state is read through
 * ui_net_show; this is what is on the glass. */
typedef enum { SEQ_IDLE, SEQ_SHOWING, SEQ_AMP, SEQ_AMPDONE, SEQ_PROBLEM, SEQ_LEAVING, SEQ_DONE } seq_t;

static lv_obj_t *s_panel, *s_ticks, *s_title_doing, *s_title_done, *s_ampscreen, *s_ampdone;
static lv_obj_t *s_problem, *s_problem_sub;
static bool s_problem_restarting;
/* The three rows, kept so each can be ticked off in turn. */
static lv_obj_t *s_row_tick[ROWS], *s_row_label[ROWS];
static int s_ticked;  /* how many have landed */
static lv_timer_t *s_timer;
static seq_t s_seq;
static uint32_t s_since;       /* when the screen went up */
static uint32_t s_ready_at;    /* ...and when the network was really up, 0 until it is */
static enum { ARC_UP, ARC_RING, ARC_OFF, ARC_BG } s_arc_phase;
static int s_finale;           /* how far through the connected moment: 0 not begun, 4 the ring fading */
static int s_ring_var;         /* the ring's animations hang off this */
static float s_ring_f0, s_ring_t0, s_ring_v0;  /* the spinner's arc when it stopped, and its speed */
static uint32_t s_arc_at;      /* when the arc's current phase began */
static uint8_t s_arc_from;     /* ...and how bright it was then */
static bool s_logo_dropped;    /* the logo underneath has been taken away */
static ui_net_t s_net = UI_NET_WIFI;  /* the network's real state, as last told */
static bool s_force_amp;       /* bench only: see ui_test_connected */
/* Whether the speaker is there, as the firmware last said. True until told
 * otherwise, so a dial that has not been told shows the old sequence rather
 * than accusing a speaker that is probably fine. */
static bool s_speaker_there = true;

LV_IMAGE_DECLARE(img_tick);

bool ui_net_showing(void) { return s_seq != SEQ_IDLE && s_seq != SEQ_DONE; }

static void opa_exec(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

static void fade_to(lv_obj_t *o, lv_opa_t to)
{
    ui_anim(o, opa_exec, lv_obj_get_style_opa(o, 0), to, FADE_MS, 0.4f, 0.0f, 0.2f, 1.0f);
}

/* ---- Building ---------------------------------------------------------------- */
/* A tick, drawn rather than typed: the dial's fonts are generated subsets of
 * Inter (tools/fonts.sh) and have no check glyph, and regenerating them to add
 * one would redraw every other glyph slightly differently - the clock and the
 * scrubber included. Two LVGL line segments came out hard-edged and lumpy at
 * this size ("like you've painted them with a potato"), so it is an
 * anti-aliased mask from tools/tick_icon.py, drawn as an image with its colour
 * applied, the way every other icon on the dial is. */
static lv_obj_t *tick(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *t = lv_image_create(parent);
    lv_image_set_src(t, &img_tick);
    lv_obj_set_style_image_recolor(t, lv_color_hex(UI_OK), 0);
    lv_obj_set_style_image_recolor_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_pos(t, x, y);
    return t;
}

static void done_row(int i, const char *text, int left, int top)
{
    s_row_tick[i] = tick(s_ticks, left, top + i * ROW_H + 3);
    lv_obj_t *l = lv_label_create(s_ticks);
    lv_obj_set_style_text_font(l, &font_inter_regular_18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, left + TICK_W + TICK_GAP, top + i * ROW_H);
    s_row_label[i] = l;
}

/* All three named and waiting, none of them ticked. The row is there from the
 * first frame - it is the tick that arrives, so what he watches is three
 * things being finished rather than a finished list being presented. */
static void rows_waiting(void)
{
    s_ticked = 0;
    for (int i = 0; i < ROWS; i++) {
        lv_obj_set_style_opa(s_row_tick[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_row_label[i], lv_color_hex(UI_GREY), 0);
    }
}

static void row_done(int i)
{
    lv_obj_set_style_text_color(s_row_label[i], lv_color_hex(UI_WHITE), 0);
    /* Plain ease-out. An overshooting curve was tried first and the tick never
     * appeared at all: LVGL's cubic bezier takes its control points as fixed
     * point in [0,1] and a y of 1.2 does not survive the conversion (found on
     * the bench, 4 Nov 2026). */
    ui_anim(s_row_tick[i], opa_exec, LV_OPA_TRANSP, LV_OPA_COVER, 380, 0.25f, 0.1f, 0.25f, 1.0f);
}

static void build(void)
{
    if (s_panel) return;
    s_panel = ui_plain(lv_layer_top());
    lv_obj_set_size(s_panel, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_panel, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    /* Takes no input: a finger goes to the screen underneath, which is the
     * difference between telling Darren something and standing in his way. */
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);

    s_ticks = ui_plain(s_panel);
    lv_obj_set_size(s_ticks, CORE_SCREEN, CORE_SCREEN);

    /* Two titles in the same place, crossfaded. One label with its text
     * swapped would change word mid-fade; two labels cross cleanly, and the
     * word that arrives is the whole point of the screen. */
    s_title_doing = lv_label_create(s_ticks);
    lv_obj_set_style_text_font(s_title_doing, &font_inter_light_36, 0);
    lv_obj_set_style_text_color(s_title_doing, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_title_doing, "Connecting");
    lv_obj_align(s_title_doing, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    s_title_done = lv_label_create(s_ticks);
    lv_obj_set_style_text_font(s_title_done, &font_inter_light_36, 0);
    lv_obj_set_style_text_color(s_title_done, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_title_done, "Connected");
    lv_obj_align(s_title_done, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    lv_obj_set_style_opa(s_title_done, LV_OPA_TRANSP, 0);

    /* The rows are set as one block and centred as one, so the ticks line up
     * under each other instead of each row centring on its own width. */
    const int left = (CORE_SCREEN - (TICK_W + TICK_GAP + 126)) / 2;  /* 126: "Music Assistant" at 18px */
    const int top = ROWS_Y;
    done_row(0, "Wi-Fi", left, top);
    done_row(1, "Home Assistant", left, top);
    done_row(2, "Music Assistant", left, top);
    done_row(3, "Streamer", left, top);

    /* The amp's own screen, after the four are ticked. Not a fourth row with
     * a tick to wait for - that wait runs to two minutes and Darren is not
     * going to stand there watching for it - and not a footnote under a list
     * of finished things either. A screen that says what is happening and how
     * long it will feel like, and then gives way to the menu. */
    s_ampscreen = ui_plain(s_panel);
    lv_obj_set_size(s_ampscreen, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_opa(s_ampscreen, LV_OPA_TRANSP, 0);
    lv_obj_t *a1 = lv_label_create(s_ampscreen);
    lv_obj_set_style_text_font(a1, &font_inter_light_28, 0);
    lv_obj_set_style_text_color(a1, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_text_align(a1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(a1, CORE_SCREEN - 110);
    lv_label_set_text(a1, "Connecting to\nthe amplifier");
    lv_obj_align(a1, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *a2 = lv_label_create(s_ampscreen);
    lv_obj_set_style_text_font(a2, &font_inter_regular_16, 0);
    lv_obj_set_style_text_color(a2, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_text_align(a2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(a2, CORE_SCREEN - 120);
    lv_label_set_text(a2, "This will take a short while");
    lv_obj_align_to(a2, a1, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);

    /* And the end of that story, for whenever it arrives. The same shape as
     * the three that went before it, so the amp joining reads as the last item
     * on the same list rather than as a different kind of announcement. */
    s_ampdone = ui_plain(s_panel);
    lv_obj_set_size(s_ampdone, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_opa(s_ampdone, LV_OPA_TRANSP, 0);
    lv_obj_t *d1 = lv_label_create(s_ampdone);
    lv_obj_set_style_text_font(d1, &font_inter_light_36, 0);
    lv_obj_set_style_text_color(d1, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(d1, "Connected");
    lv_obj_align(d1, LV_ALIGN_CENTER, 0, -34);
    tick(s_ampdone, left, 210);
    lv_obj_t *d2 = lv_label_create(s_ampdone);
    lv_obj_set_style_text_font(d2, &font_inter_regular_18, 0);
    lv_obj_set_style_text_color(d2, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(d2, "Amplifier");
    lv_obj_set_pos(d2, left + TICK_W + TICK_GAP, 207);

    /* And when it does not come at all. Three minutes is long past the two the
     * Denon takes from cold, so something is actually wrong - and the dial
     * says so and sets about fixing it rather than quietly going to sleep on
     * somebody who is standing there waiting (Darren, 4 Nov 2026). */
    s_problem = ui_plain(s_panel);
    lv_obj_set_size(s_problem, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_opa(s_problem, LV_OPA_TRANSP, 0);
    lv_obj_t *p1 = lv_label_create(s_problem);
    lv_obj_set_style_text_font(p1, &font_inter_light_28, 0);
    lv_obj_set_style_text_color(p1, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_text_align(p1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(p1, CORE_SCREEN - 110);
    lv_label_set_text(p1, "Problem connecting\nto the amplifier");
    lv_obj_align(p1, LV_ALIGN_CENTER, 0, -14);
    s_problem_sub = lv_label_create(s_problem);
    lv_obj_set_style_text_font(s_problem_sub, &font_inter_regular_16, 0);
    lv_obj_set_style_text_color(s_problem_sub, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_text_align(s_problem_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_problem_sub, CORE_SCREEN - 120);
    lv_label_set_text(s_problem_sub, "Restarting");
    lv_obj_align_to(s_problem_sub, p1, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);
}

/* ---- The sequence ------------------------------------------------------------ */
static void enter(seq_t to)
{
    s_seq = to;
    s_since = lv_tick_get();
}

static void arc_phase(int phase, uint8_t from)
{
    s_arc_phase = phase;
    s_arc_at = lv_tick_get();
    s_arc_from = from;
}

/* Brightness and speed for this moment, and the arc switched off at the end of
 * a fade rather than cut. */
static void arc_step(void)
{
    uint32_t ms = lv_tick_elaps(s_arc_at);
    switch (s_arc_phase) {
    case ARC_UP: {
        float u = ms < RAMP_MS ? (float)ms / RAMP_MS : 1.0f;
        ui_overlay_spin_style((uint8_t)(SPIN_BRIGHT_LO + (255 - SPIN_BRIGHT_LO) * u),
                              SPIN_SPEED_LO + (1.0f - SPIN_SPEED_LO) * u);
        return;
    }
    case ARC_BG:
        ui_overlay_spin_style(SPIN_BRIGHT_BG, SPIN_SPEED_BG);
        return;
    default:
        return;
    }
}

static void start(void)
{
    build();
    rows_waiting();
    lv_obj_set_style_opa(s_ampscreen, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_title_doing, "Connecting");
    lv_obj_set_style_opa(s_title_doing, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_title_done, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(s_title_doing, 0, 0);
    lv_obj_set_style_translate_y(s_title_done, 0, 0);
    lv_obj_set_style_opa(s_ticks, LV_OPA_COVER, 0);
    s_ready_at = 0;
    s_finale = 0;
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    /* Faded in over the logo, which is still sitting there at full opacity
     * underneath: one layer blending rather than two, and the menu never shows
     * through the join. The logo is dropped once this is opaque. */
    lv_obj_set_style_opa(s_panel, LV_OPA_TRANSP, 0);
    fade_to(s_panel, LV_OPA_COVER);
    s_logo_dropped = false;
    /* The arc belongs to the sequence from here, not only to the amp: the dial
     * is loading, and that is what the rim is for. It comes up out of almost
     * nothing rather than snapping on at full tilt (Darren, 4 Nov 2026). */
    ui_overlay_spin_style(SPIN_BRIGHT_LO, SPIN_SPEED_LO);
    ui_overlay_spin(true, UI_LOADING);
    arc_phase(ARC_UP, SPIN_BRIGHT_LO);
    enter(SEQ_SHOWING);
}

/* ---- The connected moment (see SWAP_AT) -------------------------------------- */
static void lift_exec(void *obj, int32_t v) { lv_obj_set_style_translate_y(obj, v, 0); }

static uint32_t mix_hex(uint32_t a, uint32_t b, float u)  /* u = 0 is a, 1 is b */
{
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    return lv_color_to_u32(lv_color_mix(lv_color_hex(b), lv_color_hex(a), (uint8_t)(u * 255.0f + 0.5f))) & 0xFFFFFF;
}

/* The ring closing, 0 to 1000 over CLOSE_MS, in two movements that never
 * overlap. First the arc coasts to a stop - both ends together, so it keeps
 * its length and does not halt dead - and turns from blue to green where it
 * stands. Only then does its leading end set off, from still, and grow on
 * round to meet the other end, easing in and out. */
static float smooth(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }

static void ring_close_exec(void *var, int32_t v)
{
    (void)var;
    float ms = v / 1000.0f * CLOSE_MS;
    float g = ms < GLIDE_MS ? ms / GLIDE_MS : 1.0f;
    /* ease-out from the spinner's speed to nothing covers half the distance it
     * would have gone */
    float moved = s_ring_v0 * GLIDE_MS / 2.0f * (1.0f - (1.0f - g) * (1.0f - g));
    float tail = s_ring_f0 + moved, from_lead = s_ring_t0 + moved;
    float grow = smooth((ms - GREEN_MS) / GROW_MS);
    float lead = v >= 1000 ? tail + 1.0f : from_lead + (tail + 1.0f - from_lead) * grow;
    ui_overlay_spin_hold(tail, lead, mix_hex(UI_LOADING, UI_OK, smooth(ms / GREEN_MS)), 255);
}

/* ...and fading, 1000 to 0, the word letting go of the green as it does. */
static void ring_fade_exec(void *var, int32_t v)
{
    (void)var;
    float u = v / 1000.0f;
    float f0, f1;
    ui_overlay_spin_span(&f0, &f1);
    ui_overlay_spin_hold(f0, f1, UI_OK, (uint8_t)(255.0f * u + 0.5f));
    lv_obj_set_style_text_color(s_title_done, lv_color_hex(mix_hex(UI_WHITE, UI_OK, u)), 0);
    if (v <= 0) {
        ui_overlay_spin(false, UI_LOADING);
        s_arc_phase = ARC_OFF;
    }
}

/* Whatever is left of it, gone: the sequence is moving on or being cut short.
 * The spinner goes back to spinning rather than vanishing - if this is an
 * interruption, the job it stands for may not be finished. */
static void ring_stop(void)
{
    lv_anim_delete(&s_ring_var, NULL);
    if (s_arc_phase == ARC_RING) {
        ui_overlay_spin_release();
        s_arc_phase = ARC_UP;
    }
}

static void title_out(void)
{
    ui_anim(s_title_doing, opa_exec, lv_obj_get_style_opa(s_title_doing, 0), LV_OPA_TRANSP, WORD_OUT_MS,
            0.4f, 0.0f, 0.9f, 0.6f);
    ui_anim(s_title_doing, lift_exec, 0, -LIFT, WORD_OUT_MS, 0.4f, 0.0f, 0.9f, 0.6f);
}

/* The moment the last tick lands: the spinner stops, from exactly where it is. */
static void ring_close(void)
{
    ui_overlay_spin_span(&s_ring_f0, &s_ring_t0);
    s_ring_v0 = 1.0f / 1100.0f;  /* a turn per SPIN_TURN_MS at full speed, which it is by now */
    s_arc_phase = ARC_RING;
    ui_anim(&s_ring_var, ring_close_exec, 0, 1000, CLOSE_MS, 0.0f, 0.0f, 1.0f, 1.0f);
}

static void title_in(void)
{
    /* The word that arrives is the whole point of the screen, so it has to be
     * the true one. With the speaker away there is nothing green to say: the
     * three services are up, the dial is on the network, and it still cannot
     * play a note. */
    lv_label_set_text(s_title_done, s_speaker_there ? "Connected" : "No streamer");
    lv_obj_set_style_text_color(s_title_done, lv_color_hex(s_speaker_there ? UI_OK : UI_WHITE), 0);
    lv_obj_set_style_translate_y(s_title_done, LIFT, 0);
    ui_anim(s_title_done, opa_exec, LV_OPA_TRANSP, LV_OPA_COVER, WORD_IN_MS, 0.2f, 0.6f, 0.3f, 1.0f);
    ui_anim(s_title_done, lift_exec, LIFT, 0, WORD_IN_MS, 0.2f, 0.6f, 0.3f, 1.0f);
}

static void ring_fade(void)
{
    ui_anim(&s_ring_var, ring_fade_exec, 1000, 0, RING_FADE_MS, 0.4f, 0.0f, 0.6f, 1.0f);
}

/* ...and then the amp, if it is still coming. If it is already in there is
 * nothing to say and the menu comes straight up. */
static void to_amp(void)
{
    ring_stop();  /* long gone by now; this is only so it cannot linger */
    fade_to(s_ticks, LV_OPA_TRANSP);
    fade_to(s_ampscreen, LV_OPA_COVER);
    /* A new job, so the arc starts again from nothing rather than carrying on
     * as though the last one never ended. */
    ui_overlay_spin_style(0, SPIN_SPEED_LO);
    ui_overlay_spin(true, UI_LOADING);
    s_arc_phase = ARC_UP;
    s_arc_at = lv_tick_get() - (RAMP_MS - ARC_IN_MS);  /* the same ramp, taken faster */
    enter(SEQ_AMP);
}

static void to_menu(void)
{
    ring_stop();
    fade_to(s_panel, LV_OPA_TRANSP);
    /* The amp is still going; it just stops being the thing on the screen, so
     * the arc drops to half brightness and half speed. If the amp is already
     * in, there is nothing left to say and the rim goes quiet altogether. */
    arc_phase(ARC_BG, SPIN_BRIGHT_BG);
    ui_overlay_spin_style(SPIN_BRIGHT_BG, SPIN_SPEED_BG);
    if (ui_amp() == UI_AMP_READY) { ui_overlay_spin(false, UI_LOADING); s_arc_phase = ARC_OFF; }
    enter(SEQ_LEAVING);
}

static void step(lv_timer_t *t)
{
    uint32_t ms = lv_tick_elaps(s_since);

    /* A question or a firmware update owns the whole screen; this gets out of
     * the way and gives up on the rest of the sequence rather than queueing
     * behind it. */
    if (ui_updating() || ui_asking()) {
        ring_stop();
        lv_anim_delete(s_panel, opa_exec);
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        ui_logo_hide();
        lv_timer_pause(t);
        s_seq = SEQ_DONE;
        return;
    }

    arc_step();

    switch (s_seq) {
    case SEQ_SHOWING: {
        if (!s_logo_dropped && ms >= FADE_MS) {
            s_logo_dropped = true;
            ui_logo_hide();  /* fully covered by now */
        }
        /* No network: nothing is going to tick, so once the fault has been
         * read the dial gets out of the way and lets him use what does work. */
        if (s_net == UI_NET_FAILED) {
            if (ms >= FAILED_MIN_MS) to_menu();
            return;
        }
        /* Until the network really is up, the rows sit there untouched and the
         * title stays Connecting. That is not theatre, it is the truth, and it
         * is the only part of this screen that waits on anything. */
        if (!s_ready_at) {
            if (s_net != UI_NET_READY) return;
            s_ready_at = lv_tick_get();
        }
        uint32_t since_ready = lv_tick_elaps(s_ready_at);
        /* One tick at a time, as though each were finishing while he watches.
         * They finished a while ago - usually before the logo did - and the
         * screen has simply stopped pretending that telling him and doing it
         * are the same moment. */
        /* The speaker's row is the one that can fail to arrive. When it does
         * not, the row stays grey and unticked and the sequence carries on
         * without it - an unticked row beside three ticked ones says it
         * better than any sentence would. */
        const int want = s_speaker_there ? ROWS : ROWS - 1;
        while (s_ticked < want && since_ready >= TICK_AT[s_ticked]) row_done(s_ticked++);
        if (s_ticked < want) return;
        uint32_t after = since_ready - TICK_AT[want - 1];
        /* And the ring only closes green when everything it stands for is in.
         * A green ring under "No streamer" would be the screen contradicting
         * itself; the spinner keeps spinning instead, because the job it
         * stands for genuinely has not finished. */
        if (s_finale == 0) { if (s_speaker_there) ring_close(); s_finale = 1; }
        if (s_finale == 1 && after >= SWAP_AT) { title_out(); s_finale = 2; }
        if (s_finale == 2 && after >= WORD_IN_AT) { title_in(); s_finale = 3; }
        if (s_finale == 3 && after >= RING_FADE_AT) { if (s_speaker_there) ring_fade(); s_finale = 4; }
        if (after < AFTER_LAST_MS) return;
        if (ms < MIN_MS) return;
        if (ui_amp() == UI_AMP_READY) to_menu();
        else to_amp();
        return;
    }
    case SEQ_AMP:
        if (ms < AMP_MS) return;
        to_menu();
        return;
    case SEQ_PROBLEM:
        /* Restarting: the dial reboots out from under this, so there is
         * nothing to wait for. Otherwise it has been read and the dial gets
         * out of the way - leaving a full screen up would stop it sleeping. */
        if (s_problem_restarting || ms < PROBLEM_MS) return;
        to_menu();
        return;
    case SEQ_AMPDONE:
        if (ms < AMPDONE_MS) return;
        fade_to(s_panel, LV_OPA_TRANSP);
        enter(SEQ_LEAVING);
        return;
    case SEQ_LEAVING:
        if (ms < FADE_MS) return;
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(t);
        s_seq = SEQ_DONE;
        return;
    default:
        lv_timer_pause(t);
        return;
    }
}

/* Not this boot. The six-hourly check-in wakes behind a dark screen, says what
 * its charge is and goes back down with nobody in the room - and the opening
 * was running in full behind that dark screen, four and a half seconds of logo
 * at thirty frames a second plus the rim spinning, four times a day, on a
 * battery. Nothing was visible and nothing was free. */
void ui_net_skip(void) { s_seq = SEQ_DONE; }

/* The amp answered, and somebody is still looking at the dial (ui.c checks).
 * Once per boot: an amp that drops and comes back in the middle of an evening
 * is not news worth a whole screen. */
static bool s_said_amp;

/* The amp has gone away and is being waited for again, on a dial that is
 * already up. Darren, 4 Nov 2026: the screen had slept, the twenty-minute rule
 * had powered everything off, he turned the knob and landed on the menu with a
 * blue arc on it and nothing to say what it was. "In the cold boot power up,
 * you see the whole connecting to the amplifier. That's missing from this kind
 * of warm start."
 *
 * So it gets the same introduction the cold start gives it: the amp screen for
 * a moment, the arc coming up out of nothing underneath it, and then back to
 * whatever he was looking at with the arc running quietly behind.
 *
 * This only fires when the amp really is not answering. A warm amp replies to
 * the first probe and goes straight from UNKNOWN to READY without passing
 * through CONNECTING, so waking next to an amp that is already on does not
 * throw a screen at him. */
void ui_net_amp_connecting(void)
{
    if (s_seq != SEQ_DONE) return;  /* the opening is running; it does this itself */
    if (ui_updating() || ui_asking()) return;
    build();
    s_said_amp = false;  /* and the landing is worth saying again */
    lv_obj_set_style_opa(s_ticks, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_ampdone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_problem, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_ampscreen, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_panel, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    fade_to(s_panel, LV_OPA_COVER);
    ui_overlay_spin_style(0, SPIN_SPEED_LO);
    ui_overlay_spin(true, UI_LOADING);
    s_arc_phase = ARC_UP;
    s_arc_at = lv_tick_get() - (RAMP_MS - ARC_IN_MS);
    enter(SEQ_AMP);
    if (s_timer) lv_timer_resume(s_timer);
}

/* Bench: show it now, however the real one has gone. */
void ui_test_amp_connected(void)
{
    s_said_amp = false;
    s_seq = SEQ_DONE;
    build();
    ui_net_amp_connected();
}

/* The amp never came. Said plainly, and left up: what happens next is a
 * restart, and the screen stays on it until that happens. */
void ui_net_amp_problem(bool restarting)
{
    build();
    lv_label_set_text(s_problem_sub, restarting ? "Restarting"
                                                : "Check the amplifier is switched on");
    ui_overlay_spin(false, UI_LOADING);
    lv_obj_set_style_opa(s_ticks, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_ampscreen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_ampdone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_problem, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_panel, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    fade_to(s_panel, LV_OPA_COVER);
    /* SEQ_PROBLEM rather than SEQ_DONE so this still counts as the sequence
     * being up: ui.c leaves the rim alone while it is, and the arc stays off.
     * The job failed; nothing should still be spinning as though it had not. */
    s_problem_restarting = restarting;
    enter(SEQ_PROBLEM);
    if (s_timer) lv_timer_resume(s_timer);
}

void ui_net_amp_connected(void)
{
    if (s_said_amp || !s_panel) return;
    if (s_seq != SEQ_DONE) return;  /* the opening is still running; it will show it in order */
    if (ui_updating() || ui_asking()) return;
    s_said_amp = true;

    /* Nothing left to wait for, so nothing left spinning. */
    ui_overlay_spin(false, UI_LOADING);
    lv_obj_set_style_opa(s_ticks, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_ampscreen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_ampdone, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_panel, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    fade_to(s_panel, LV_OPA_COVER);
    enter(SEQ_AMPDONE);
    if (s_timer) lv_timer_resume(s_timer);
}

void ui_net_show(ui_net_t state)
{
    /* The logo has the screen first and nothing goes over it. The sequence
     * starts from the beginning once it is done, however far the network has
     * really got in the meantime - which is the point of the logo. */
    if (ui_logo_showing()) return;

    s_net = state;
    if (s_seq == SEQ_IDLE) {
        start();
        if (!s_timer) {
            s_timer = lv_timer_create(step, STEP_MS, NULL);
            lv_timer_set_auto_delete(s_timer, false);
        }
        lv_timer_resume(s_timer);
        return;
    }
    /* A network that drops and comes back is not worth replaying the opening
     * for; the rim already says the dial is working on something. */
    if (s_seq == SEQ_SHOWING && state == UI_NET_FAILED && !s_ready_at)
        lv_label_set_text(s_title_doing, "No network");
}

void ui_net_speaker(bool there)
{
    if (there == s_speaker_there) return;
    s_speaker_there = there;
    /* If the sequence has already settled on a word, say the other one. The
     * speaker answers last, so this is the ordinary case on a dial whose
     * streamer has gone: the screen is still up when the answer lands. */
    if (s_seq == SEQ_SHOWING && s_finale >= 3) title_in();
}

/* ---- Bench ------------------------------------------------------------------- */
/* The connected screen, as a still. In use it is up for two seconds and then
 * crossfades away, which is no use for judging how it sits on the glass. */
void ui_test_connected(bool amp_ready)
{
    build();
    s_force_amp = amp_ready;
    if (s_timer) lv_timer_pause(s_timer);
    bool amp = amp_ready || ui_amp() == UI_AMP_READY;
    rows_waiting();
    for (int i = 0; i < ROWS; i++) row_done(i);
    s_ticked = ROWS;
    lv_obj_set_style_opa(s_title_doing, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_title_done, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(s_title_done, 0, 0);
    lv_obj_set_style_text_color(s_title_done, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_opa(s_ampscreen, amp ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_ticks, amp ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    s_seq = SEQ_DONE;  /* the poll leaves it alone; it is a still, not a run */
    s_force_amp = false;
}
