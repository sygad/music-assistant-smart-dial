/* Scripted performance run. Drives the real screens through simulated touch and
 * knob input and prints the frame rate of each step, e.g.
 *   BENCH album_drag_1   frames=17 ms=300 fps=56.7
 */
#include <math.h>
#include <stdio.h>

#include "ui_internal.h"

typedef struct {
    const char *name;
    uint32_t ms;
    void (*start)(void);
    void (*tick)(uint32_t t);
    void (*end)(void);
} step_t;

static uint32_t (*s_frames)(void);
static void (*s_done)(void);
static lv_timer_t *s_timer;
static int s_step;
static uint32_t s_t0, s_f0;
static int s_last_knob;

/* ---- Menu: a finger dragging the list down and up, then released --------- */
static void menu_drag_start(void) { ui_show(UI_VIEW_MENU); menu_goto(2); ui_touch_press(180, 180); }
static void menu_drag_tick(uint32_t t) { ui_touch_move(180, 180 + (int)lroundf(130.0f * sinf(6.2831853f * t / 1500.0f))); }
static void menu_drag_end(void) { ui_touch_release(180, 180 + 20); }
static void snap_last(void) { menu_goto(5); }
static void snap_first(void) { menu_goto(0); }
static void snap_albums(void) { menu_goto(2); }

/* ---- Album browse: a 220px swipe in 300ms, then the snap ------------------ */
static int s_dir;
static void albums_start(void) { ui_show(UI_VIEW_ALBUMS); }
static void swipe_start_next(void) { s_dir = -1; ui_touch_press(290, 180); }
static void swipe_start_prev(void) { s_dir = 1; ui_touch_press(70, 180); }
static void swipe_tick(uint32_t t)
{
    int x0 = s_dir < 0 ? 290 : 70;
    ui_touch_move(x0 + s_dir * (int)(220 * t / 300), 180);
}
static void swipe_end(void) { ui_touch_release(s_dir < 0 ? 70 : 290, 180); }

/* ---- Fling: a 240px flick in 60ms (4 px/ms), then the glide --------------- */
static void flick_start(void) { ui_touch_press(300, 180); }
static void flick_tick(uint32_t t) { ui_touch_move(300 - (int)(240 * (t > 60 ? 60 : t) / 60), 180); }
static void flick_end(void) { ui_touch_release(60, 180); }

/* ---- Now Playing: knob turned at 25 detents a second ---------------------- */
/* open the album (its track list loads), then tap to play: the knob is the volume */
static void np_start(void)
{
    ui_show(UI_VIEW_ALBUMS);
    ui_touch_press(180, 180);
    ui_touch_release(180, 180);
}
static void np_play(void)
{
    ui_touch_press(180, 100);
    ui_touch_release(180, 100);
    s_last_knob = 0;
}
static void knob_tick(uint32_t t)
{
    int n = (int)(t / 40);
    int dir = t < 1000 ? 1 : -1;
    if (n != s_last_knob) { ui_knob(dir); s_last_knob = n; }
}
static void back_to_menu(void)
{
    np_stop();  /* don't leave it "playing": that would send it back to Now Playing after 8 s */
    ui_show(UI_VIEW_MENU);
}

static const step_t STEPS[] = {
    {"menu_drag",     1500, menu_drag_start, menu_drag_tick, menu_drag_end},
    {"menu_snap_end", 400,  NULL, NULL, snap_last},
    {"menu_snap_5",   400,  snap_first, NULL, NULL},
    {"menu_snap_0",   400,  snap_albums, NULL, NULL},
    {"idle",          300,  albums_start, NULL, NULL},
    {"album_drag_1",  300,  swipe_start_next, swipe_tick, swipe_end},
    {"album_snap_1",  320,  NULL, NULL, NULL},
    {"album_drag_2",  300,  swipe_start_next, swipe_tick, swipe_end},
    {"album_snap_2",  320,  NULL, NULL, NULL},
    {"album_drag_3",  300,  swipe_start_next, swipe_tick, swipe_end},
    {"album_snap_3",  320,  NULL, NULL, NULL},
    {"album_drag_4",  300,  swipe_start_prev, swipe_tick, swipe_end},
    {"album_snap_4",  320,  NULL, NULL, NULL},
    {"album_drag_5",  300,  swipe_start_prev, swipe_tick, swipe_end},
    {"album_snap_5",  320,  NULL, NULL, NULL},
    {"fling_flick_1", 70,   flick_start, flick_tick, flick_end},
    {"fling_glide_1", 1500, NULL, NULL, NULL},
    {"fling_flick_2", 70,   flick_start, flick_tick, flick_end},
    {"fling_glide_2", 300,  NULL, NULL, NULL},
    {"fling_flick_3", 70,   flick_start, flick_tick, flick_end},
    {"fling_glide_3", 2500, NULL, NULL, NULL},
    {"open_album",    800,  np_start, NULL, NULL},
    {"knob_volume",   2000, np_play, knob_tick, back_to_menu},
};
#define STEP_COUNT ((int)(sizeof(STEPS) / sizeof(STEPS[0])))

static void begin(int i)
{
    s_step = i;
    if (STEPS[i].start) STEPS[i].start();
    s_t0 = lv_tick_get();
    s_f0 = s_frames();
}

static void run(lv_timer_t *t)
{
    const step_t *st = &STEPS[s_step];
    uint32_t elapsed = lv_tick_elaps(s_t0);
    if (elapsed < st->ms) {
        if (st->tick) st->tick(elapsed);
        return;
    }
    if (st->end) st->end();
    uint32_t frames = s_frames() - s_f0;
    printf("BENCH %-14s frames=%-3lu ms=%-5lu fps=%.1f\n", st->name, (unsigned long)frames,
           (unsigned long)elapsed, frames * 1000.0f / (float)elapsed);
    if (s_step + 1 < STEP_COUNT) {
        begin(s_step + 1);
        return;
    }
    lv_timer_delete(t);
    s_timer = NULL;
    if (s_done) s_done();
}

void ui_bench_start(uint32_t (*frames)(void), void (*done)(void))
{
    if (s_timer) return;
    s_frames = frames;
    s_done = done;
    begin(0);
    s_timer = lv_timer_create(run, 10, NULL);
}
