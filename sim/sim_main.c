/*
 * Bathroom Music Knob: the browser simulator.
 *
 * Runs the knob's own screens (ui/ and core/, unchanged) in a web page, built
 * with Emscripten. Stands in for the board: the display is a 360x360 canvas,
 * drawn the way the firmware draws it (LVGL settings from the firmware's
 * sdkconfig, partial rendering in 40-line blocks, panel byte order, the rim
 * painted on each finished block); touch and the knob come from the page.
 *
 * The page (web/sim.js) runs the loop: each animation frame it calls
 * sim_frame(), then copies the frame from sim_pixels() when sim_frame says
 * something changed.
 */
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "sim.h"
#include "ui.h"
#include "ui_internal.h"  /* for ui_asking and np_scrubbing: neither overlay is a view */

#define W 360
#define H 360
#define LINES 40  /* as the firmware's partial buffers (board_display.c) */

static uint8_t s_rgba[W * H * 4];
static uint8_t s_buf1[W * LINES * 2], s_buf2[W * LINES * 2];
static bool s_dirty;

/* LVGL's clock. Normally the wall clock, but a capture can drive it instead.
 *
 * A browser throttles a hidden tab's timers to about one a second, so a
 * capture taken with the pane hidden advanced LVGL by a second per frame
 * rather than by twenty milliseconds: detents arrived seconds apart and were
 * swallowed, playback ran on, and no two takes of the same script came out
 * the same (10 Nov 2026). Driven, a capture is the same every time whatever
 * the browser is doing. */
static bool s_tick_driven;
static uint32_t s_tick_at;

static uint32_t tick_ms(void)
{
    return s_tick_driven ? s_tick_at : (uint32_t)emscripten_get_now();
}

EMSCRIPTEN_KEEPALIVE void sim_tick_drive(int on)
{
    if (on && !s_tick_driven) s_tick_at = (uint32_t)emscripten_get_now();
    s_tick_driven = on != 0;
}

EMSCRIPTEN_KEEPALIVE void sim_tick_step(int ms) { s_tick_at += (uint32_t)ms; }
EMSCRIPTEN_KEEPALIVE int sim_tick_now(void) { return (int)tick_ms(); }

/* Like the firmware's flush: paint the rim and arc into the finished block
 * (panel byte order), then "send" it, here to the RGBA frame the page shows. */
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int w = lv_area_get_width(a);
    ui_overlay_apply(px, a, w);
    for (int y = a->y1; y <= a->y2; y++) {
        const uint8_t *src = px + (size_t)(y - a->y1) * w * 2;
        uint8_t *dst = s_rgba + ((size_t)y * W + a->x1) * 4;
        for (int x = 0; x < w; x++, src += 2, dst += 4) {
            uint16_t c = (uint16_t)(src[0] << 8 | src[1]);  /* big-endian RGB565 */
            uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            dst[0] = (uint8_t)(r << 3 | r >> 2);
            dst[1] = (uint8_t)(g << 2 | g >> 4);
            dst[2] = (uint8_t)(b << 3 | b >> 2);
            dst[3] = 255;
        }
    }
    s_dirty = true;
    lv_display_flush_ready(d);
}

EM_JS(void, js_haptic, (int effect), { if (Module.onHaptic) Module.onHaptic(effect); });
static void haptic(uint8_t effect) { js_haptic(effect); }

EMSCRIPTEN_KEEPALIVE void sim_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_buf1, s_buf2, sizeof s_buf1, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ui_haptic_init(haptic);
    ui_init(sim_library());
    ui_voice_init(sim_voice());
    /* Settings: the firmware's own details on the knob (its MAC, the address
     * it got on the home network, the firmware on it, the charge left). Stand-
     * ins here, so the screen can be judged with real-looking values. */
    ui_device_init("7C:DF:A1:2B:4E:90", "192.168.1.84", "0.9.0");
    /* What the knob itself reads while it's plugged in: the divider is across
     * the 5 V rail, so the charger reads as full and the bolt says why. A
     * different figure looked like the simulator knew something the knob
     * didn't (Darren, 28 Oct 2026). */
    ui_device_battery(100, true);
    /* Connections, the second Settings page: a stand-in in which everything is
     * there, so the screen can be judged. sim_status below puts any of them
     * away - the case the screen exists for. */
    ui_status_set(1, 1, 1, 1, false);
}

/* Connections: drive the five rows from the page.
 *   Module.ccall("sim_status", null, ["number","number","number","number","number"],
 *                [1, 1, 1, 0, 0])   // speaker away
 * Each is -1 not yet asked, 0 away, 1 there; the last argument is "checking". */
EMSCRIPTEN_KEEPALIVE void sim_status(int wifi, int ha, int ma, int speaker, int checking)
{
    ui_status_set(wifi, ha, ma, speaker, checking != 0);
}

/* The opening sequence's fourth row: whether the speaker is there. */
EMSCRIPTEN_KEEPALIVE void sim_net_speaker(int there) { ui_net_speaker(there != 0); }

/* Settings: set the battery from the page, so both the level and the charging
 * bolt can be seen without a rebuild:
 *   Module.ccall("sim_device_battery", null, ["number", "number"], [40, 0]) */
EMSCRIPTEN_KEEPALIVE void sim_device_battery(int percent, int charging)
{
    ui_device_battery(percent, charging != 0);
}

/* ---- Filming the opening ---------------------------------------------------------
 * The connecting screens are built into this simulator - they are ui/ui_net.c,
 * compiled in like every other screen. What is missing is the thing that sets
 * them off: on the dial the network layer calls ui_net_show() as each service
 * lands, and there is no network layer here, so nothing ever called it and the
 * simulator went from the logo straight to the menu.
 *
 * These let the page drive the real sequence instead of a stand-in: the real
 * screens, the real crossfades, the real arc winding up, stopping, turning
 * green and closing into a ring. One state at a time, exactly as the firmware
 * feeds them.
 *
 *   Module.ccall("sim_net", "number", ["number"], [0])
 *       0 Wi-Fi, 1 Music Assistant, 2 Home Assistant, 3 speaker, 4 ready,
 *       5 failed.
 *       Returns 0 while the logo still has the screen (the dial behaves the
 *       same way: the sequence starts from the beginning once the logo is
 *       done), so call it until it returns 1.
 *   Module.ccall("sim_amp", null, ["number"], [2])
 *       0 unknown, 1 connecting, 2 ready - the last of these is what ends the
 *       opening and hands over to the menu.
 */
static bool s_net_driven;

EMSCRIPTEN_KEEPALIVE int sim_net(int state)
{
    if (ui_logo_showing()) return 0;
    s_net_driven = true;
    ui_net_show((ui_net_t)state);
    return 1;
}

EMSCRIPTEN_KEEPALIVE void sim_amp(int state) { ui_amp_state((ui_amp_t)state, 0); }

/* One pass of the LVGL loop. Returns 1 if the frame changed. */
EMSCRIPTEN_KEEPALIVE int sim_frame(void)
{
    /* The opening animation runs itself out and then stays on the glass: on
     * the dial it is ui_net.c fading the connecting screen in over the top
     * that takes it away, and there is no network here to connect to. Without
     * a stand-in the simulator sat behind the logo for ever and nothing under
     * it could be reached (5 Nov 2026). ui_logo_showing() goes false the
     * moment the animation has finished, which is the same moment the dial
     * would be covering it. */
    if (!s_net_driven && !ui_logo_showing()) ui_logo_hide();
    sim_library_poll();
    lv_timer_handler();
    bool d = s_dirty;
    s_dirty = false;
    return d;
}

EMSCRIPTEN_KEEPALIVE uint8_t *sim_pixels(void) { return s_rgba; }

/* Input from the page. `ms` is the event's own time, on the same clock as
 * LVGL's tick, as the firmware stamps touch samples. */
EMSCRIPTEN_KEEPALIVE void sim_touch(int what, int x, int y, double ms)
{
    ui_touch_time((uint32_t)ms);
    if (what == 0) ui_touch_press(x, y);
    else if (what == 1) ui_touch_move(x, y);
    else ui_touch_release(x, y);
}

EMSCRIPTEN_KEEPALIVE void sim_knob(int steps) { ui_knob(steps); }
EMSCRIPTEN_KEEPALIVE int sim_view(void) { return (int)ui_view(); }
/* Whether the question overlay has the screen. It sits on top of a view
 * rather than being one, so sim_view cannot see it, and the page's canvas
 * is no help to a test running with the tab hidden. */
EMSCRIPTEN_KEEPALIVE int sim_asking(void) { return ui_asking() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int sim_scrubbing(void) { return np_scrubbing() ? 1 : 0; }

/* How the last gesture was read: a finger coming over the bezel or one put down
 * on the glass, and the dwell it was judged on. The firmware logs the same two
 * with every touch; here they let a scripted gesture check the verdict. */
EMSCRIPTEN_KEEPALIVE int sim_touch_from_edge(void) { return ui_touch_from_edge() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int sim_touch_dwell_ms(void) { return (int)ui_touch_dwell_ms(); }

/* ---- For the flow map and the redline document (7 Nov 2026) -----------------
 * Screens the page cannot otherwise reach: the opening sequence (the firmware
 * drives it on the dial), the update screen, the amp's waits, the clock and
 * the scrubber. These only call what ui.h already offers for tests, so no
 * screen behaviour changes; they exist so a scripted capture can stop on every
 * screen instead of waiting for the real thing. */
EMSCRIPTEN_KEEPALIVE void sim_logo_show(void) { ui_logo_show(); }
EMSCRIPTEN_KEEPALIVE void sim_logo_hold(void) { ui_test_logo_hold(); }
EMSCRIPTEN_KEEPALIVE void sim_logo_at(int ms) { ui_test_logo_at((uint32_t)ms); }
EMSCRIPTEN_KEEPALIVE void sim_logo_hide(void) { ui_logo_hide(); }
EMSCRIPTEN_KEEPALIVE void sim_net_show(int state) { ui_net_show((ui_net_t)state); }
EMSCRIPTEN_KEEPALIVE int sim_net_showing(void) { return ui_net_showing() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void sim_net_amp_connecting(void) { ui_net_amp_connecting(); }
EMSCRIPTEN_KEEPALIVE void sim_net_amp_connected(void) { ui_net_amp_connected(); }
EMSCRIPTEN_KEEPALIVE void sim_net_amp_problem(int restarting) { ui_net_amp_problem(restarting != 0); }
EMSCRIPTEN_KEEPALIVE void sim_amp_state(int state, int waiting_secs) { ui_amp_state((ui_amp_t)state, waiting_secs); }
EMSCRIPTEN_KEEPALIVE void sim_amp_note(int on) { ui_test_amp_note(on != 0); }
EMSCRIPTEN_KEEPALIVE void sim_update_show(int state, int percent, const char *why) { ui_update_show((ui_update_t)state, percent, why); }
EMSCRIPTEN_KEEPALIVE void sim_music_off_elsewhere(void) { ui_music_off_elsewhere(); }
EMSCRIPTEN_KEEPALIVE void sim_show(int view) { ui_show((ui_view_t)view); }
EMSCRIPTEN_KEEPALIVE void sim_open(int view) { ui_open((ui_view_t)view); }
EMSCRIPTEN_KEEPALIVE void sim_goto_album(int index) { ui_goto_album(index); }
EMSCRIPTEN_KEEPALIVE void sim_goto_playlist(int p) { ui_goto_playlist(p); }
EMSCRIPTEN_KEEPALIVE void sim_test_sleep(void) { ui_test_sleep(); }
EMSCRIPTEN_KEEPALIVE void sim_test_deep_sleep(void) { ui_test_deep_sleep(); }
EMSCRIPTEN_KEEPALIVE void sim_test_wake(void) { ui_test_wake(); }
EMSCRIPTEN_KEEPALIVE int sim_clock_up(void) { return ui_clock_up() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void sim_scrub_open(void) { ui_test_scrub_open(); }
EMSCRIPTEN_KEEPALIVE void sim_volume_set(int percent) { ui_volume_set(percent); }
EMSCRIPTEN_KEEPALIVE void sim_eq_set(int param, int db) { ui_eq_set(param, db); }
EMSCRIPTEN_KEEPALIVE void sim_test_fling(double speed) { ui_test_fling((float)speed); }
EMSCRIPTEN_KEEPALIVE void sim_haptic_demo(void) { ui_haptic_demo(); }

int main(void) { return 0; }  /* the page calls sim_init once the module is ready */
