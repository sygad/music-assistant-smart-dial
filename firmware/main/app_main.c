/*
 * Bathroom Music Knob: hardware test firmware.
 *
 * Menu, Album browse and Now Playing with Darren's whole library, loaded live
 * from Music Assistant (albums, track lists, covers). Playback is simulated on
 * the knob. Has a USB console for scripted tests, screenshots and tuning.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "core.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "amp.h"
#include "crash.h"
#include "ha.h"
#include "library.h"
#include "memwatch.h"
#include "net_check.h"
#include "ota.h"
#include "playback.h"
#include "voice.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "ui.h"
#include "ui_internal.h"  /* for ui_asking: the question overlay is not a view */

static void device_read(void);  /* Settings' details, also on the console */
static void power_on(void);     /* the bathroom amps, also on the console */
static void power_off(void);
static void eq_seed_apply(void);  /* the amp's tone controls, read once at start-up */
static void volume_follow_apply(void);  /* the volume, whenever something else moved it */
static void ota_apply(void);            /* how an update is going, onto the screen */
static void battery_report(void);       /* the charge, to Home Assistant, once a minute */
static void amp_apply(void);            /* whether the amp can hear, onto the screen */
static void net_apply(void);            /* whether the dial is on the network, onto the screen */
static void status_apply(void);         /* ...and the Connections screen's five rows, while it is open */
static void amp_follow(void);           /* whether to ask it at all */
static void music_follow(void);         /* ...and when Home Assistant turns the music off */
static uint32_t s_power_on_at;
static bool s_amps_quiet;  /* console "amps quiet": power_on does nothing until restart */

/* Kept on the knob, so it survives a reflash: whether the motor ticks. The
 * effect itself is not stored - it never changes - so turning it back on gives
 * the same feel as before (Darren, 28 Oct 2026). */
#define SETTINGS_NS "knob"
#define HAPTICS_KEY "haptics"

static void haptics_save(bool on)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, HAPTICS_KEY, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static bool haptics_saved(void)
{
    nvs_handle_t h;
    uint8_t on = 1;  /* never set: the motor ticks, as it always did */
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, HAPTICS_KEY, &on);
        nvs_close(h);
    }
    return on != 0;
}
void screenshot_dump(const char *name);
void screenshot_dump_raw(const char *name, const uint8_t *frame, int w, int h);

static const char *TAG = "knob";

/* ---- Input: polled from the LVGL thread so the UI is only touched there ------ */
static bool s_touched;
static bool s_bench_running;
/* Console lines waiting to run on the LVGL thread. A queue, so commands typed
 * while a long one (a screenshot) runs aren't lost. */
/* The screen's shadow copy, for screenshots over the console only: taken
 * the first time one is asked for, and the whole screen redrawn into it.
 * Returns true if it was switched on just now, so it is not filled yet. */
#define SHADOW_FILL_MS 300
static char s_frame_name[24];

static bool shadow_on(void)
{
    if (board_display_shadow()) return false;
    board_display_shadow_enable();
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    return true;
}

static void frame_later(lv_timer_t *t)
{
    (void)t;
    if (board_display_shadow())
        screenshot_dump_raw(s_frame_name, board_display_shadow(), BOARD_LCD_W, BOARD_LCD_H);
    else ESP_LOGW(TAG, "no shadow frame: no PSRAM for it");
}

/* A typed command. 64, not 32: "heard i hate models warehouse leeds" was cut
 * off mid-word, so a typed search wasn't testing what speech really sends
 * (29 Oct 2026). */
#define CMD_LEN 64
static QueueHandle_t s_cmds;
static volatile bool s_scan_requested;  /* the Wi-Fi scan blocks, so it runs on the console task */
static void bench_done(void);
static void shots_start(void);
static uint32_t frames(void);

/* Console commands, typed on the USB serial port:
 *   bench          scripted frame-rate test
 *   tap x y        a tap; drag x y dx dy: a drag (test)
 *   shots          screenshot of each screen - MAKES SOUND: step 41 taps an
 *                  album open, which plays it and powers the bathroom amps
 *   frame <name>   what was actually sent to the panel
 *   slide <ms>     album slide time
 *   np             tap the middle of the screen (opens an album, then plays it)
 *   view <name>    open a screen: menu, albums, np, eq, search, playlists, recent, settings
 *   knob <n>       turn the knob n detents (negative = anticlockwise)
 *   spin <n> <ms>  n detents, one every ms, like a hand turning it
 *   fling <tau> <min> <letters>  glide time constant (ms), flick and letter speeds (px/ms)
 *   flicks         the last few Album browse gestures, as recorded
 *   fling <v>      test: a fling at finger speed v px/ms (negative = forwards)
 *   album <n>      jump to album n
 *   flick [n] [back]   n quick flicks forwards (or back), a quarter second apart
 *   stats          cover loading counts and times
 *   sleep          straight to the clock and the screen off (a tap or a turn wakes it)
 *   deep           straight to deep sleep, instead of two hours from now (a TURN wakes it)
 *   power on/off   the bathroom amps, as a touch or Music off would (test)
 *   haptic demo    play the six candidate ticks, numbered on screen
 *   pick <n>       use demo candidate n for the ticks; haptic <effect> for any DRV2605 effect */
/* spin: a hand on the knob. It also counts the detents that land on an album
 * whose cover has not arrived, which is exactly what shows up as a gap, so a
 * change can be tried against going one way, the other way and turning round
 * before Darren ever sees it (29 Oct 2026). */
static int s_spin_dir, s_spin_miss, s_spin_done, s_spin_total, s_spin_flew;
static float s_spin_peak;
static uint32_t s_spin_gap, s_spin_at, s_spin_best, s_spin_worst, s_spin_from;
static lv_timer_t *s_spin_timer;  /* one at a time: two of these ran together and both counted */
static void spin_step(lv_timer_t *t)
{
    uint32_t now = lv_tick_get();  /* what the hand actually did, not what was asked for */
    if (s_spin_done) {
        uint32_t d = lv_tick_diff(now, s_spin_at);
        if (d > s_spin_worst) s_spin_worst = d;
        if (d < s_spin_best || !s_spin_best) s_spin_best = d;
    }
    s_spin_at = now;
    ui_knob(s_spin_dir);
    s_spin_done++;
    if (!library_get()->cover(ui_album_index())) s_spin_miss++;
    if (ui_album_flying()) s_spin_flew++;
    if (ui_album_turn_rate() > s_spin_peak) s_spin_peak = ui_album_turn_rate();
    if (s_spin_done < s_spin_total) return;
    ESP_LOGI(TAG, "SPIN %s %d detents asked %lu ms apart, really %lu-%lu (%lu avg): %d with no cover (%d%%), "
                  "%d%% shown as letters, fastest %d.%d/sec, ended on %d",
             s_spin_dir > 0 ? "forward" : "back", s_spin_done, (unsigned long)s_spin_gap,
             (unsigned long)s_spin_best, (unsigned long)s_spin_worst,
             (unsigned long)(lv_tick_diff(now, s_spin_from) / (uint32_t)s_spin_done), s_spin_miss,
             100 * s_spin_miss / s_spin_done, 100 * s_spin_flew / s_spin_done, (int)s_spin_peak, ((int)(s_spin_peak * 10)) % 10, ui_album_index());
    s_spin_timer = NULL;
    lv_timer_delete(t);
}

/* flick: a finger moving 240px in 60ms (4 px/ms), like a quick swipe; `flick 3`
 * does three, a quarter of a second apart, like a rapid run */
static int s_flick_step, s_flick_dir, s_flick_left;
static uint32_t s_flick_t0;
static void flick_step(lv_timer_t *t)
{
    int x0 = s_flick_dir < 0 ? 300 : 60;
    int k = s_flick_step % 25;  /* 10 ms steps: 8 for the flick, then a pause */
    /* stamp each sample with its intended time: the drawing loop can't run this
     * timer every 10 ms, and the UI times gestures by their samples */
    ui_touch_time(s_flick_t0 + (uint32_t)s_flick_step * 10);
    if (k == 0) ui_touch_press(x0, 180);
    else if (k <= 6) ui_touch_move(x0 + s_flick_dir * 40 * k, 180);
    else if (k == 7) {
        ui_touch_release(x0 + s_flick_dir * 240, 180);
        if (--s_flick_left <= 0) { lv_timer_delete(t); return; }
    }
    s_flick_step++;
}

static void print_line(const char *line) { printf("%s\n", line); }

/* ---- Audit: open every album and check its tracks come back ------------------
 * "audit" walks the whole library asking for each album's track list, the same
 * way opening it on screen does, and reports every one that comes back empty or
 * never comes back at all. Darren asked for the exhaustive version: 1238
 * albums, not the handful anyone would open by hand (29 Oct 2026). */
static int s_audit_at = -1, s_audit_empty, s_audit_slow, s_audit_tracks;
static uint32_t s_audit_since, s_audit_from;

static void audit_step(void)
{
    if (s_audit_at < 0) return;
    const ui_tracks_t *t = library_get()->tracks(s_audit_at);  /* asking starts the load */
    if (!t) {
        if (lv_tick_diff(lv_tick_get(), s_audit_since) < 10000) return;
        ESP_LOGW(TAG, "AUDIT album %d (%s): no answer in 10 s", s_audit_at, library_key(s_audit_at));
        s_audit_slow++;
    }
    else if (t->count == 0) {
        ESP_LOGW(TAG, "AUDIT album %d (%s): no tracks", s_audit_at, library_key(s_audit_at));
        s_audit_empty++;
    }
    else s_audit_tracks += t->count;
    if (++s_audit_at >= library_loaded()) {
        ESP_LOGI(TAG, "AUDIT done: %d albums, %d tracks, %d empty, %d timed out, %lu s",
                 s_audit_at, s_audit_tracks, s_audit_empty, s_audit_slow,
                 (unsigned long)(lv_tick_diff(lv_tick_get(), s_audit_from) / 1000));
        s_audit_at = -1;
        return;
    }
    s_audit_since = lv_tick_get();
}

static void run_command(const char *line)
{
    int ms, n, dx, dy;
    char name[24];
    if (s_bench_running) return;
    if (!strcmp(line, "bench")) { s_bench_running = true; ui_bench_start(frames, bench_done); }
    else if (!strcmp(line, "shots")) { s_bench_running = true; ui_test_wake(); shots_start(); }
    else if (!strcmp(line, "np")) { ui_touch_press(180, 180); ui_touch_release(180, 180); }
    /* Open a screen straight off, so a scripted test doesn't have to tap its
     * way there from whatever was last left on the knob (31 Oct 2026). */
    else if (!strncmp(line, "view ", 5)) {
        static const struct { const char *name; ui_view_t view; } VIEWS[] = {
            {"menu", UI_VIEW_MENU},         {"albums", UI_VIEW_ALBUMS},
            {"np", UI_VIEW_NOWPLAYING},     {"eq", UI_VIEW_EQ},
            {"search", UI_VIEW_SEARCH},     {"playlists", UI_VIEW_PLAYLISTS},
            {"recent", UI_VIEW_RECENT},     {"settings", UI_VIEW_SETTINGS},
        };
        for (size_t i = 0; i < sizeof VIEWS / sizeof *VIEWS; i++)
            if (!strcmp(line + 5, VIEWS[i].name)) { ui_test_wake(); ui_open(VIEWS[i].view); break; }
        ESP_LOGI(TAG, "VIEW %d", (int)ui_view());
    }
    else if (sscanf(line, "slide %d", &n) == 1) { ui_set_slide_ms((uint32_t)n); ESP_LOGI(TAG, "slide = %lu ms", (unsigned long)ui_slide_ms()); }
    else if (!strcmp(line, "audit")) {
        s_audit_at = 0;
        s_audit_empty = s_audit_slow = s_audit_tracks = 0;
        s_audit_since = s_audit_from = lv_tick_get();
        ESP_LOGI(TAG, "AUDIT starting: %d albums", library_loaded());
    }
    else if (!strcmp(line, "audit stop")) s_audit_at = -1;
    else if (sscanf(line, "spin %d %d", &n, &ms) == 2 && n) {
        ui_show(UI_VIEW_ALBUMS);
        if (s_spin_timer) { lv_timer_delete(s_spin_timer); s_spin_timer = NULL; }
        s_spin_dir = n > 0 ? 1 : -1;
        s_spin_total = n > 0 ? n : -n;
        s_spin_done = s_spin_miss = s_spin_flew = 0;
        s_spin_peak = 0.0f;
        s_spin_best = s_spin_worst = 0;
        s_spin_from = lv_tick_get();
        s_spin_gap = (uint32_t)(ms < 1 ? 1 : ms);
        s_spin_timer = lv_timer_create(spin_step, s_spin_gap, NULL);
    }
    else if (sscanf(line, "knob %d", &n) == 1) ui_knob(n);
    /* Is a question up? The screenshot can't always be trusted to have been
     * redrawn yet, and a scripted test needs the answer itself (31 Oct 2026). */
    else if (!strcmp(line, "asking")) ESP_LOGI(TAG, "ASKING %d", ui_asking() ? 1 : 0);
    /* Is the scrubber still up? The close is timed against the sound arriving,
     * which is the one thing a screenshot cannot show (2 Nov 2026). */
    else if (!strcmp(line, "scrub")) ESP_LOGI(TAG, "SCRUB %s", ui_scrubbing() ? "open" : "closed");
    else if (!strcmp(line, "scrub open")) { ui_test_wake(); ui_test_scrub_open(); }
    else if (sscanf(line, "tap %d %d", &n, &ms) == 2) { ui_touch_press(n, ms); ui_touch_release(n, ms); }
    else if (sscanf(line, "drag %d %d %d %d", &n, &ms, &dx, &dy) == 4) {
        ui_touch_press(n, ms);
        for (int i = 1; i <= 8; i++) ui_touch_move(n + dx * i / 8, ms + dy * i / 8);
        ui_touch_release(n + dx, ms + dy);
    }
    else if (sscanf(line, "frame %23s", name) == 1) {
        ESP_LOGI(TAG, "arc fraction %.4f", ui_overlay_arc_fraction());
        if (shadow_on()) {
            /* Just switched on, so it holds nothing yet: dump it once the
             * screen has been drawn into it in full. */
            snprintf(s_frame_name, sizeof s_frame_name, "%s", name);
            lv_timer_t *t = lv_timer_create(frame_later, SHADOW_FILL_MS, NULL);
            lv_timer_set_repeat_count(t, 1);
        } else if (board_display_shadow()) screenshot_dump_raw(name, board_display_shadow(), BOARD_LCD_W, BOARD_LCD_H);
        else ESP_LOGW(TAG, "no shadow frame");
    }
    else if (!strcmp(line, "flicks")) ui_dump_flicks(print_line);
    else if (!strncmp(line, "fling ", 6) && strchr(line + 6, ' ') == NULL) ui_test_fling(strtof(line + 6, NULL));
    else if (sscanf(line, "album %d", &n) == 1) { ui_test_wake(); ui_goto_album(n); }
    else if (sscanf(line, "playlist %d", &n) == 1) { ui_test_wake(); ui_goto_playlist(n); }
    else if (!strncmp(line, "flick", 5) && (line[5] == 0 || line[5] == ' ')) {
        s_flick_dir = strstr(line, "back") ? 1 : -1;
        s_flick_left = 1;
        sscanf(line + 5, "%d", &s_flick_left);
        if (s_flick_left < 1) s_flick_left = 1;
        s_flick_step = 0;
        s_flick_t0 = lv_tick_get();
        lv_timer_create(flick_step, 10, NULL);
    }
    else if (!strncmp(line, "fling", 5)) {
        float min, letter;
        uint32_t tau;
        ui_get_fling(&tau, &min, &letter);
        if (sscanf(line, "fling %d %f %f", &n, &min, &letter) >= 2) ui_set_fling((uint32_t)n, min, letter);
        ui_get_fling(&tau, &min, &letter);
        ESP_LOGI(TAG, "fling: glide %lu ms, flick above %.2f px/ms, letters above %.2f px/ms", (unsigned long)tau, min,
                 letter);
    }
    else if (!strcmp(line, "sleep")) ui_test_sleep();  /* now, instead of CORE_SLEEP_MS from now */
    else if (!strcmp(line, "deep")) ui_test_deep_sleep();
    else if (!strcmp(line, "power on")) { s_power_on_at = 0; power_on(); }
    else if (!strcmp(line, "power off")) power_off();
    /* For long scripted runs on the bench (tools/mem_soak.py): the amps go off
     * and stay off, however much the script turns the knob. Every turn asks
     * for them otherwise, which is right on the wall and wrong for a fifteen
     * hour test with nobody in the bathroom. Until the next restart only. */
    else if (!strcmp(line, "amps quiet")) { power_off(); s_amps_quiet = true; ESP_LOGI(TAG, "AMPS quiet until restart"); }
    else if (!strcmp(line, "pool")) library_dump();
    else if (!strcmp(line, "haptic demo")) ui_haptic_demo();
    else if (!strcmp(line, "haptic off") || !strcmp(line, "haptic on")) {
        bool on = line[7] == 'o' && line[8] == 'n';
        ui_haptic_enable(on);
        haptics_save(on);
        ESP_LOGI(TAG, "HAPTICS %s, remembered on the knob", on ? "on" : "off");
    }
    else if (sscanf(line, "haptic %d", &n) == 1) { ui_haptic_set((uint8_t)n); board_haptic((uint8_t)n); }
    else if (sscanf(line, "pick %d", &n) == 1) { ui_haptic_set(ui_haptic_candidate(n)); board_haptic(ui_haptic_effect()); }
    else if (sscanf(line, "list %d %d", &n, &ms) == 2) library_list(n, ms);
    else if (!strncmp(line, "find ", 5)) library_find(line + 5);
    else if (!strcmp(line, "loader off")) library_pause(true);
    else if (!strcmp(line, "loader on")) library_pause(false);
    else if (sscanf(line, "inv %d", &n) == 1) board_display_log_invalidations(n);
    else if (!strcmp(line, "scan")) s_scan_requested = true;
    /* The network lines again, without power-cycling the dial: "Connecting",
     * then "Connected" four seconds later. Driven through net_check so what is
     * tested is the path that really runs. */
    else if (!strcmp(line, "net")) net_say_again();
    /* The connected screen, on the glass, for two minutes: in use it is gone
     * in under two seconds and cannot be judged by eye (Darren, 4 Nov 2026).
     * Wakes the screen first, the way `shots` does, or there is nothing to
     * look at. */
    /* The power-on logo again, and the finished design held still. */
    else if (!strcmp(line, "logo")) { ui_test_wake(); ui_logo_show(); }
    else if (!strcmp(line, "logo hold")) { ui_test_wake(); ui_test_logo_hold(); }
    else if (sscanf(line, "logo at %d", &n) == 1) { ui_test_wake(); ui_test_logo_at((uint32_t)n); }
    else if (!strcmp(line, "net hold")) { ui_test_wake(); ui_test_connected(false); }
    else if (!strcmp(line, "amp done")) { ui_test_wake(); ui_test_amp_connected(); }
    else if (!strcmp(line, "amp note")) { ui_test_wake(); ui_open(UI_VIEW_NOWPLAYING); ui_test_amp_note(true); }
    /* The two failure screens, without waiting three minutes for a dead amp. */
    else if (!strcmp(line, "amp fail")) { ui_test_wake(); ui_net_amp_problem(true); }
    else if (!strcmp(line, "amp fail2")) { ui_test_wake(); ui_net_amp_problem(false); }
    /* ...and as it looks once the amp has answered too, without waiting for it. */
    else if (!strcmp(line, "net hold done")) { ui_test_wake(); ui_test_connected(true); }
    else if (!strncmp(line, "heard ", 6)) voice_test_heard(line + 6);
    else if (!strcmp(line, "voice")) voice_log();
    else if (!strcmp(line, "stt")) voice_test_stt();
    else if (!strcmp(line, "voice clear")) voice_clear();
    else if (!strcmp(line, "batt")) ESP_LOGI(TAG, "BATT rail %d mV", board_battery_mv());
    else if (!strcmp(line, "device")) device_read();  /* what Settings would show */
    else if (!strcmp(line, "idle")) {  /* why a screen is or is not coming back to the music */
        bool playing, screen, amp;
        uint32_t still;
        ui_idle_state(&playing, &screen, &amp, &still);
        ESP_LOGI(TAG, "IDLE view %d  playing %d  screen busy %d  amp connecting %d  still %lu ms", (int)ui_view(),
                 playing, screen, amp, (unsigned long)still);
    }
    else if (!strcmp(line, "eq"))  /* the EQ as the amp would get it, and where the arc is */
        ESP_LOGI(TAG, "EQ view %d  treble %+d dB  bass %+d dB  subs %+d dB  arc %.4f", (int)ui_view(),
                 ui_eq_value(0), ui_eq_value(1), ui_eq_value(2), ui_overlay_arc_fraction());
    else if (!strcmp(line, "mem"))
        ESP_LOGI(TAG, "MEM internal free %u (largest %u, lowest ever %u), PSRAM free %u (lowest ever %u, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    else if (!strcmp(line, "memmap")) mem_report();
    else if (!strcmp(line, "stats")) {
        library_stats_t st = library_stats();
        ESP_LOGI(TAG, "STATS %lu covers (fetch avg %lu ms, decode avg %lu ms), %lu failed, %lu abandoned, "
                      "idle: %lu nothing to load, %lu no free buffer",
                 (unsigned long)st.fulls, (unsigned long)(st.fulls ? st.fetch_ms / st.fulls : 0),
                 (unsigned long)(st.fulls ? st.decode_ms / st.fulls : 0), (unsigned long)st.failures,
                 (unsigned long)st.abandoned, (unsigned long)st.idle_nothing, (unsigned long)st.idle_nobuf);
    }
    else ESP_LOGW(TAG, "unknown command '%s'", line);
}

static void input_poll(lv_timer_t *t)
{
    (void)t;
    library_poll();
    playback_poll();
    eq_seed_apply();
    volume_follow_apply();
    ota_apply();
    net_apply();
    status_apply();
    amp_apply();
    music_follow();
    battery_report();
    audit_step();
    voice_poll();
    char cmd[CMD_LEN];
    while (!s_bench_running && xQueueReceive(s_cmds, cmd, 0) == pdTRUE) run_command(cmd);
    /* Roaming to another access point drops the network for a few seconds, so
     * it is only allowed once the knob has been left alone for a while - and
     * never while firmware is being downloaded. An update runs precisely when
     * the knob has been left alone, so without that second test the roam and
     * the download were almost guaranteed to collide: the first real update
     * over Wi-Fi died at 71% that way (2 Nov 2026).
     *
     * s_used, rather than the gap alone: both the tick and s_last_use start at
     * zero, so "ten seconds since he last touched it" was false for the first
     * ten seconds of every boot however untouched the knob was - and the one
     * roam that matters most happens at about six seconds, straight after the
     * first address, when the chip has just rejoined whatever access point it
     * had written down last time without looking at what else is there.
     *
     * Darren, 5 Nov 2026: a dial sitting a metre from an access point joined
     * one at -80 dBm and stayed on it for eighty seconds, which is how long it
     * took the once-a-minute check to come round. Nothing has been touched at
     * boot by definition, so the boot roam is allowed; everything that follows
     * it then happens on the right access point. */
    static uint32_t s_last_use;
    static bool s_used;
    net_allow_roam(!ota_busy() && (!s_used || lv_tick_diff(lv_tick_get(), s_last_use) > 10000));
    int steps = board_knob_take();
    if (steps) {
        s_last_use = lv_tick_get();
        s_used = true;
        static uint32_t knob_last;  /* the gap says what kind of turn it is */
        uint32_t knob_now = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "INPUT knob %+d  %lu ms since the last", steps,
                 (unsigned long)(knob_last ? knob_now - knob_last : 0));
        knob_last = knob_now;
        ui_knob(steps);
    }
    board_touch_t ev;
    static int x0, y0;
    while (board_touch_next(&ev)) {
        if (s_bench_running) continue;
        s_last_use = lv_tick_get();
        s_used = true;
        ui_touch_time(ev.t_ms);  /* the sample's own time, for accurate flick speed */
        if (ev.down && !s_touched) { ui_touch_press(ev.x, ev.y); x0 = ev.x; y0 = ev.y; }
        else if (ev.down) ui_touch_move(ev.x, ev.y);
        else if (s_touched) {
            ui_touch_release(ev.x, ev.y);
            /* the dwell and the verdict too: the thresholds that tell a finger
              * coming over the bezel from one put down on the glass are set from
              * these (CORE_EDGE_PX, CORE_EDGE_INWARD, CORE_EDGE_DWELL_MS) */
            ESP_LOGI(TAG, "INPUT touch %d,%d -> %d,%d dwell %ums %s", x0, y0, ev.x, ev.y,
                     (unsigned)ui_touch_dwell_ms(),
                     ui_touch_from_edge()      ? "TOP"
                     : ui_touch_from_left_edge() ? "LEFT"
                                                 : "glass");
            ESP_LOGI(TAG, "INPUT  saw %d samples, reached %d px",
                     ui_touch_samples(), ui_touch_reach_px());
        }
        s_touched = ev.down;
    }
}

/* Serial console: type a command and Enter. */
static void console_task(void *arg)
{
    (void)arg;
    char line[CMD_LEN];
    int n = 0;
    for (;;) {
        if (s_scan_requested) { s_scan_requested = false; net_scan_log(); }
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (c == '\n' || c == '\r') {
            line[n] = 0;
            if (n) xQueueSend(s_cmds, line, 0);
            n = 0;
        } else if (n < (int)sizeof line - 1) {
            line[n++] = (char)c;
        }
    }
}

/* ---- Frame-rate log: once a second while anything is being drawn ------------ */
static void stats(lv_timer_t *t)
{
    (void)t;
    static uint32_t last_frames;
    static uint64_t last_render, last_flush, last_px, last_post;
    uint32_t f = board_display_frames();
    uint64_t r, fl;
    board_display_times(&r, &fl);
    uint32_t n = f - last_frames;
    uint64_t px = board_display_flushed_px();
    if (n > 1) {
        ESP_LOGI(TAG, "fps=%lu  render=%.1fms (rim %.1fms)  send=%.1fms  redrawn=%.0f%% of the screen per frame",
                 (unsigned long)n, (r - last_render) / 1000.0 / n, (board_display_post_us() - last_post) / 1000.0 / n,
                 (fl - last_flush) / 1000.0 / n, (px - last_px) * 100.0 / n / (BOARD_LCD_W * BOARD_LCD_H));
    }
    last_px = px;
    last_post = board_display_post_us();
    last_frames = f;
    last_render = r;
    last_flush = fl;
}

static uint32_t frames(void) { return board_display_frames(); }

/* After the bench: one screenshot of each screen, for checking on the Mac. */
static void shots(lv_timer_t *t)
{
    static int step;
    switch (step++) {
    case 0: ui_show(UI_VIEW_MENU); break;
    case 3: screenshot_dump("menu"); break;
    case 4: ui_goto_album(0); break;
    case 7: screenshot_dump("albums_short_title"); break;
    case 8: ui_goto_album(5); break;
    case 11: screenshot_dump("albums_long_title_start"); break;
    case 40: screenshot_dump("albums_long_title_scrolling"); break;
    case 41: ui_touch_press(180, 180); ui_touch_release(180, 180); break;
    case 42: ui_knob(5); break;
    case 44: screenshot_dump("nowplaying_volume_70"); break;
    case 45: ui_show(UI_VIEW_MENU); s_bench_running = false; step = 0; lv_timer_delete(t); break;
    default: break;
    }
}

static void shots_start(void)
{
    shadow_on();  /* the first capture is three steps (600 ms) away: drawn in full by then */
    lv_timer_create(shots, 200, NULL);
}

static void bench_done(void)
{
    s_bench_running = false;
    ESP_LOGI(TAG, "BENCH done. Display %s, QSPI %lu MHz", board_display_mode_name(),
             (unsigned long)board_display_pclk_mhz());
}

/* ---- Settings: the knob's own details ----------------------------------------
 * Read as Settings opens, on the LVGL thread it is called from. Nothing here
 * runs otherwise: browsing and playing music get the processor to themselves.
 *
 * The charge: the divider reads the rail, which is the cell when it's running
 * on its own. Plugged in, the charger holds the rail above anything the cell
 * reaches, which is how "on charge" is told apart; the cell's own level can't
 * be seen through it, so the figure reads full while it charges, as a plugged
 * in device does, and the bolt says why. Unplug it to see the real level. */
#define BATT_EMPTY_MV    3300  /* a 3.7 V cell, flat */
#define BATT_FULL_MV     4200  /* the same cell, full */
#define BATT_CHARGER_MV  4350  /* above the cell's own range: the charger is holding the rail up */
/* The last figure reported, for the Settings screen to show. It used to take
 * its own ADC read, which with the screen necessarily on was the loaded rail
 * every time and disagreed with Home Assistant by several points. */
static int s_batt_said = -1;

static void device_read(void)
{
    char mac[24] = "", ip[24] = "";
    uint8_t m[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, m) == ESP_OK)
        snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr)
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&info.ip));
    ui_device_init(mac, ip, esp_app_get_description()->version);
    ESP_LOGI(TAG, "DEVICE %s, %s, firmware %s, haptics %s", mac, ip, esp_app_get_description()->version,
             ui_haptic_enabled() ? "on" : "off");

    /* Settings is open, so the screen is on, so a reading taken here and now is
     * the loaded rail - several points below the resting one, and it used to be
     * what this screen showed, disagreeing with Home Assistant every time. The
     * figure shown is the last resting one instead (6 Nov 2026); the fresh read
     * is only for telling the charger apart, which it is high enough to do
     * through any load. */
    int mv = board_battery_mv();
    bool charging = mv >= BATT_CHARGER_MV;
    int percent = charging ? 100 : s_batt_said;
    ESP_LOGI(TAG, "DEVICE battery %d mV now, %d%% resting%s", mv, percent, charging ? ", on charge" : "");
    ui_device_battery(percent, charging);
}

/* ---- Amp power (31 Oct 2026) ---------------------------------------------------
 * Darren took the bathroom's Music toggle off the dashboard: the amps come on
 * because the knob is being used and go off because it isn't, so picking an
 * album never waits for the Denon to come up.
 *
 * The on is sent again every POWER_ON_REPEAT_MS while the knob is in use rather
 * than remembered, because the helper can be turned off by something else -
 * Music Auto Off when the room reads empty, or by hand - and a remembered
 * "it's on" would be wrong with nothing to correct it. Turning on a boolean
 * that is already on changes no state, so Music Fade Off never sees the repeat
 * and nothing is fetched to find out. */
#define POWER_ON_REPEAT_MS 30000

/* Whether the music is meant to be on, which is the only time the amp being
 * deaf is worth saying. Music off powers the amp down on purpose, so from then
 * on it stops answering because Darren asked it to - and the probe, which
 * cannot tell that from a cold boot or a lock-up, called it "Connecting to the
 * amp" over the menu he had just used (Darren, 4 Nov 2026).
 *
 * True at boot, as amp.c's own flag is and for the same reason: the dial comes
 * up with its screen on, nothing calls this for a state that is already so,
 * and a false here would leave a freshly booted dial never probing - which
 * holds every volume write (4 Nov 2026). The first Music off settles it. */
static bool s_music_on = true;

/* ...and only an "on" that was actually seen to reach Home Assistant starts
 * that window. From a cold start the screen is live several seconds before
 * Wi-Fi is, so the first touch's ask can go nowhere; counting it as sent left
 * the amps off through half a minute of browsing and they came on only when
 * Darren opened an album, thirty seconds later (4 Nov 2026). */
static void power_on(void)
{
    if (s_amps_quiet) return;  /* the console's "amps quiet": a bench run, nobody listening */
    uint32_t now = lv_tick_get();
    if (s_power_on_at && ha_music_power_landed() &&
        lv_tick_diff(now, s_power_on_at) < POWER_ON_REPEAT_MS)
        return;
    s_power_on_at = now;
    ha_music_power(true);
    s_music_on = true;
    amp_follow();
}

static void power_off(void)
{
    s_power_on_at = 0;  /* the next touch powers it straight back up */
    ha_music_power(false);
    s_music_on = false;
    amp_follow();
}

static const ui_power_t s_power = {.on = power_on, .off = power_off};

/* ---- The player (31 Oct 2026) --------------------------------------------------
 * The screens ask for a collection and a track; the uris are looked up here,
 * on the LVGL thread where the track cache lives, and the playing is done on
 * playback.c's own task. The volume and the tone controls are not Music
 * Assistant's business at all: they are the Denon's, which Home Assistant
 * drives through Darren's helpers. */

/* The knob counts volume 0-100; the Denon's own scale runs to 98, and 80% of
 * the dial is Denon 78, which is where the knob's limit sits (PLANNING.md). */
#define DENON_MAX 98
static int denon_mv(int percent) { return (percent * DENON_MAX + 50) / 100; }
/* ...and back again, for a volume the knob is being told about rather than
 * setting. Rounded the same way, so a value the knob sent and then reads back
 * returns to the number it started from. */
static int denon_percent(int mv) { return (mv * 100 + DENON_MAX / 2) / DENON_MAX; }

static void player_play(int coll, int track)
{
    char uri[160];
    if (!library_collection_uri(coll, uri, sizeof uri)) {
        ESP_LOGW(TAG, "nothing to play: collection %d has no uri yet", coll);
        return;
    }
    playback_play(uri, library_track_uri(coll, track));
}

static void player_volume(int percent) { ha_set_number(HA_VOLUME, denon_mv(percent)); }

static void player_eq(int param, int db)
{
    static const ha_number_t AS[3] = {HA_TREBLE, HA_BASS, HA_SUBWOOFER};
    if (param >= 0 && param < 3) ha_set_number(AS[param], db);
}

static const ui_player_t s_player = {
    .play = player_play,
    .pause = playback_pause,
    .stop = playback_stop_after,
    .seek = playback_seek,
    .volume = player_volume,
    .eq = player_eq,
};

/* The amp's tone controls as they already are, read once, so the knob's EQ
 * opens where the sound actually is rather than at a flat 0 dB that the first
 * click would then drag the amp to.
 *
 * The reading blocks on three HTTP requests, so it has a task of its own; the
 * values are handed to the screens on the LVGL thread, which is the only one
 * allowed to touch them. */
static _Atomic int s_eq_seed[3];
static _Atomic bool s_eq_seeded;

static void eq_seed(void *arg)
{
    (void)arg;
    /* While we are on a task that may block: tell Home Assistant which
     * firmware is running, so what is on the wall can be read off a phone
     * rather than by opening Settings on the knob (2 Nov 2026). */
    ha_report_firmware();
    ha_report_restart();
    static const ha_number_t AS[3] = {HA_TREBLE, HA_BASS, HA_SUBWOOFER};
    for (int i = 0; i < 3; i++) {
        int db = 0;
        if (!ha_read_number(AS[i], &db)) { vTaskDeleteWithCaps(NULL); return; }
        atomic_store(&s_eq_seed[i], db);
    }
    atomic_store(&s_eq_seeded, true);
    /* WithCaps, because it was created WithCaps: plain vTaskDelete leaves the
     * stack and the control block allocated for ever, since the kernel did not
     * allocate them and so does not free them (idf_additions.c). */
    vTaskDeleteWithCaps(NULL);
}

/* The volume, whenever something other than the knob moved it: at start-up,
 * where this is what puts the arc on the amp's real level rather than the 30
 * the knob assumes, and afterwards every time the bathroom dashboard's slider
 * or the Denon's remote moves it (Darren, 1-2 Nov 2026 - the arc used to sit
 * at 30 over an amp at 71, and the slider moved the amp without the arc
 * following). LVGL thread: the screens are only touched here. */
static void volume_follow_apply(void)
{
    int mv;
    if (!ha_volume_moved(&mv)) return;
    int percent = denon_percent(mv);
    ESP_LOGI(TAG, "volume is %d (Denon %d), not the knob's doing", percent, mv);
    ui_volume_set(percent);
}

/* LVGL thread. */
static void eq_seed_apply(void)
{
    if (!atomic_exchange(&s_eq_seeded, false)) return;
    for (int i = 0; i < 3; i++) {
        int db = atomic_load(&s_eq_seed[i]);
        ui_eq_set(i, db);
        ESP_LOGI(TAG, "EQ %d starts at %d dB", i, db);
    }
}

/* The charge, handed to ha.c once a minute (6 Nov 2026: rewritten again, see
 * below; 4 Nov 2026: the median; 3 Nov 2026: one read a minute).
 *
 * The figure used to be one ADC read a minute, reported as it came. On the
 * wall it jumped by ten points between consecutive minutes - 32, 41, 34, 42 -
 * which is not a battery discharging, it is the measurement being wrong, and
 * it made every drain figure quoted that day worthless.
 *
 * The cause is the scale. Empty to full is 900 mV, so ONE PER CENT IS 9 mV,
 * and the radio pulls the rail down by far more than that while it transmits.
 * board_battery_mv already averages eight ADC samples, but it takes them
 * microseconds apart, so all eight land inside the same transmit burst and
 * averaging them buys nothing at all. So the spread has to be in time, not in
 * the ADC: a reading is taken every BATT_SAMPLE_MS into a ring, and the ring
 * is what gets reported, once a minute.
 *
 * WHICH READING OF THE RING, AND WHY NOT THE MEDIAN (Darren, 6 Nov 2026).
 *
 * The ring used to report its median, with a latch forbidding the figure to
 * climb, on the grounds that no real discharge ever goes up. Both of those
 * were wrong, and together they cost 7 points of a 13-hour overnight run.
 *
 * The screen and the radio hold the rail down for as long as they are on, so
 * every sample taken while the dial is awake is a loaded sample - it is not a
 * spike the median can step over, it is the whole minute. Picking the middle
 * of a loaded minute reports the loaded voltage. Then the latch made that
 * permanent: the rail came back up the moment the screen went off, and the
 * figure was not allowed to follow it. Four minutes in the hand on the morning
 * of 6 Nov took the reading from 71% to 64% and it could never return. As
 * Darren put it: accelerate to 70 and slow back to 60, and it only ever tells
 * you 70.
 *
 * The cell's resting voltage is the CEILING of the distribution, not the
 * middle of it. Load only ever pulls the rail down; nothing pulls it up. So
 * the reading to report is near the top of the sorted ring - not the very top,
 * because ADC noise is symmetric and the single highest sample rides it, so
 * the top n/6 are dropped as noise and the next one down is taken.
 *
 * The ceiling deals with the radio, which bursts. It cannot deal with the
 * backlight, which does not: the screen being on holds the whole ring down for
 * as long as it is on, so there is no ceiling in it to find. So no sample is
 * taken at all while the screen is on. The charge is a resting measurement and
 * it is made at rest - the screen goes off two minutes after the last touch,
 * and sampling picks up there. While somebody is using the dial the figure
 * stands still, which is the truth: nothing is known about the resting voltage
 * during that time. Home Assistant keeps hearing the last one every five
 * minutes (see ha.c), so the sensor never goes quiet.
 *
 * With every sample in the ring now a resting sample, the latch that forbade
 * the figure to climb has nothing left to protect against and is gone. A slew
 * limit of two points per report stays as a backstop on the way up, so that no
 * single odd ring can walk the figure anywhere the next report will not undo.
 *
 * ACROSS DEEP SLEEP. A deep-sleep wake is a restart, so the ring came back
 * empty and the first report of the check-in was computed from one or two
 * samples taken while Wi-Fi was coming up - the single worst moment there is.
 * The last figure reported is kept in RTC memory instead, which survives deep
 * sleep, and it is what gets posted the instant the dial is up, so Home
 * Assistant is never blank and never sees a wake as a cliff. A computed figure
 * only replaces it once the ring holds BATT_MIN_SAMPLES, which at a sample
 * every two seconds is inside the thirty-second check-in window.
 *
 * What this does not fix: 3300-4200 mV read as a straight line is a crude
 * model of a lithium cell, which sits near 3.7 V for most of its life and then
 * falls off a cliff. That makes the number optimistic in the middle and sudden
 * at the end. The millivolts now go to Home Assistant alongside the
 * percentage, at nine times the resolution and with no scaling of any kind, so
 * the real curve can be drawn from a run rather than guessed at. */
#define BATT_SAMPLE_MS   2000
#define BATT_SAMPLES     12   /* 24 s: a full ring inside the check-in window */
#define BATT_MIN_SAMPLES 10   /* enough of a distribution to have a top to take */
#define BATT_REPORT_MS   60000
#define BATT_CLIMB_MAX   2    /* points per report, on the way up only */
#define BATT_RTC_MAGIC   0x0B5A7701

/* Kept across deep sleep, which is a restart but not a power cycle. The magic
 * is the guard: on a cold boot this memory is whatever was in the silicon. */
static RTC_DATA_ATTR uint32_t s_batt_rtc_magic;
static RTC_DATA_ATTR int s_batt_rtc_said;


static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void battery_report(void)
{
    static int ring[BATT_SAMPLES];
    static int n;                 /* how many of the ring are filled */
    static int next;              /* where the next sample goes */
    static int said = -1;         /* the last percentage reported */
    static bool carried;          /* the figure from before the sleep has been posted */
    static uint32_t sampled, reported;

    if (sampled && lv_tick_elaps(sampled) < BATT_SAMPLE_MS) return;
    sampled = lv_tick_get();
    int mv = board_battery_mv();
    if (mv <= 0) return;

    bool charging = mv >= BATT_CHARGER_MV;

    /* The screen is on, so the rail is loaded and there is nothing here worth
     * keeping. The charger check above still runs: a dial plugged in while
     * somebody is holding it should say so. */
    if (!charging && !ui_asleep()) return;

    /* On the charger the rail is the charger's, not the cell's, and those
     * samples tell nothing about the charge. They are kept out of the ring
     * entirely, so unplugging does not spend a minute reading 100%. */
    if (charging) {
        n = next = 0;
        said = -1;                /* and the figure is free to climb again */
        s_batt_rtc_magic = 0;     /* nothing from before the charge is worth carrying */
        ha_battery(100, mv, true);
        return;
    }

    ring[next] = mv;
    next = (next + 1) % BATT_SAMPLES;
    if (n < BATT_SAMPLES) n++;

    /* The figure from before the sleep, posted at once rather than a ring-fill
     * later: a dial that has just woken should not be blank in Home Assistant,
     * and on a check-in wake it is only up for thirty seconds. */
    if (!carried) {
        carried = true;
        if (s_batt_rtc_magic == BATT_RTC_MAGIC && s_batt_rtc_said >= 0) {
            said = s_batt_rtc_said;
            ESP_LOGI(TAG, "DEVICE charge %d%% carried over the sleep", said);
            ha_battery(said, mv, false);
            reported = lv_tick_get();
        }
    }

    /* Too little of a distribution to find the top of: wait. */
    if (n < BATT_MIN_SAMPLES) return;
    if (reported && lv_tick_elaps(reported) < BATT_REPORT_MS) return;
    reported = lv_tick_get();

    int sorted[BATT_SAMPLES];
    memcpy(sorted, ring, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, cmp_int);
    /* The resting rail is the ceiling, less the top n/6 as noise. */
    int resting = sorted[n - 1 - n / 6];

    int percent = (resting - BATT_EMPTY_MV) * 100 / (BATT_FULL_MV - BATT_EMPTY_MV);
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (said >= 0 && percent > said + BATT_CLIMB_MAX) percent = said + BATT_CLIMB_MAX;
    said = percent;
    s_batt_said = percent;
    s_batt_rtc_said = percent;
    s_batt_rtc_magic = BATT_RTC_MAGIC;
    ESP_LOGI(TAG, "DEVICE charge %d%% (resting %d mV of %d samples, span %d mV)", percent, resting, n,
             sorted[n - 1] - sorted[0]);
    ha_battery(percent, resting, false);
}

/* Whether the amp can hear, onto the screen. amp.c asks the amp itself on its
 * own task; this is the LVGL thread picking the answer up, and it is called on
 * every poll because the label has to be kept alive while the wait lasts. */
/* The amp never answered (Darren, 4 Nov 2026).
 *
 * Three minutes is well past the two the bathroom Denon takes from cold, so
 * at that point something is actually wrong - it is off at the wall, or it has
 * hung. The dial used to respond by letting the clock through and going to
 * sleep, which is the worst of both: the fault is not fixed and the person
 * waiting is shown a blank screen. "If after 3 minutes it's not connected,
 * there's clearly a problem. That should surface to the screen saying
 * something along the lines of problem connecting to the amplifier,
 * restarting. And then I need you to effectively do a cold boot of the amps
 * and a reboot of the dial. Don't just kick me out."
 *
 * So it says so, switches the amps off, and restarts - and on the way back up
 * it asks for them again, which is a genuine power cycle of the amp rather
 * than another round of asking a hung one the same question.
 *
 * Once. The counter lives in RTC memory, which survives a restart but not a
 * power-on, so a dial that comes up next to an amp that is switched off at the
 * wall tries this exactly once and then says so and stops - rather than
 * restarting every three minutes all night with the screen lit. */
#define AMP_RECOVER_SAY_MS  4000   /* the message, read before anything happens */
#define AMP_RECOVER_OFF_MS  2500   /* ...and Home Assistant given time to act on it */
/* RTC_NOINIT, not RTC_DATA: RTC_DATA is re-initialised on an ordinary restart,
 * which is precisely the restart this needs to count. NOINIT survives a
 * restart and a deep sleep and is garbage at power-on, so a magic word says
 * whether it means anything. Reset deliberately below for a power-on or a
 * deep-sleep wake - those are new episodes and deserve a fresh attempt. */
#define AMP_RECOVER_MAGIC 0xA37C0DE5u
RTC_NOINIT_ATTR static uint32_t s_amp_recover_magic;
RTC_NOINIT_ATTR static int s_amp_recover_tries;

static void amp_recover_counter_init(esp_sleep_wakeup_cause_t woke)
{
    esp_reset_reason_t why = esp_reset_reason();
    bool fresh = s_amp_recover_magic != AMP_RECOVER_MAGIC || why == ESP_RST_POWERON ||
                 woke == ESP_SLEEP_WAKEUP_EXT1 || woke == ESP_SLEEP_WAKEUP_TIMER;
    s_amp_recover_magic = AMP_RECOVER_MAGIC;
    if (fresh) s_amp_recover_tries = 0;
    else ESP_LOGW(TAG, "this boot follows an amp recovery restart (%d so far)", s_amp_recover_tries);
}

static void amp_recover_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(AMP_RECOVER_SAY_MS));
    ESP_LOGW(TAG, "amp never answered: powering the amps down and restarting");
    power_off();
    vTaskDelay(pdMS_TO_TICKS(AMP_RECOVER_OFF_MS));
    esp_restart();
}

static void amp_gave_up(void)
{
    if (s_amp_recover_tries >= 1) {
        /* Tried that already. Say so and leave it: the ordinary idle rules
         * come back, so the dial can sleep instead of burning the battery
         * restarting into the same fault for the rest of the night. */
        ESP_LOGE(TAG, "amp still not answering after a restart; leaving it");
        ui_net_amp_problem(false);
        amp_awake(false);  /* stop asking, so the clock and sleep are allowed again */
        s_music_on = false;
        return;
    }
    s_amp_recover_tries++;
    ui_net_amp_problem(true);
    xTaskCreatePinnedToCoreWithCaps(amp_recover_task, "amprec", 4096, NULL, 4, NULL, 0,
                                    MALLOC_CAP_SPIRAM);
}

/* Home Assistant turned the music off: the dial joins in rather than going on
 * believing it is playing and asking a dead amp whether it is there. */
static void music_follow(void)
{
    if (!ha_music_turned_off()) return;
    if (!s_music_on) return;  /* the dial turned it off itself; it already knows */
    ESP_LOGI(TAG, "music off came from Home Assistant: letting the amps go");
    s_music_on = false;
    amp_follow();
    ui_music_off_elsewhere();
}

static void amp_apply(void)
{
    /* Only while there is something to wait for: amp_connecting_secs is zero
     * unless the amp is actually being asked. */
    static bool given_up;
    int secs = amp_connecting_secs();
    if (!secs) given_up = false;
    /* Any wait, not only the first of a boot. I had limited this to the first
     * connection to avoid rebooting the dial under Darren's hand mid-evening,
     * and he is right that it is the wrong instinct: "if there is a
     * reconnection issue that's a fault, then it should have a power cycle
     * like it does on the cold start. If it can't reconnect, there's no point
     * in it going to sleep. It needs to restart" (4 Nov 2026).
     *
     * Three minutes without the amp answering means the dial cannot play
     * anything, cannot set a volume and cannot be used for what it is for. A
     * restart under his hand is a second of inconvenience; a dial sitting
     * there politely useless is the fault left in place. */
    else if (!given_up && amp_get_state() == AMP_CONNECTING &&
             secs * 1000 >= CORE_AMP_GIVE_UP_MS) {
        given_up = true;
        amp_gave_up();
        return;
    }

    static const ui_amp_t AS[] = {
        [AMP_UNKNOWN] = UI_AMP_UNKNOWN,
        [AMP_CONNECTING] = UI_AMP_CONNECTING,
        [AMP_READY] = UI_AMP_READY,
    };
    ui_amp_state(AS[amp_get_state()], amp_connecting_secs());
}

/* Whether the dial is on the network, onto the screen. net_check.c joins Wi-Fi
 * on its own task and keeps the answer in an atomic; this is the LVGL thread
 * picking it up, which is the only thread allowed to touch a screen. Called on
 * every poll because the line has to be kept alive while the wait lasts. */
static void net_apply(void)
{
    static const ui_net_t AS[] = {
        [NET_WIFI] = UI_NET_WIFI,
        [NET_MUSIC_ASSISTANT] = UI_NET_MUSIC_ASSISTANT,
        [NET_HOME_ASSISTANT] = UI_NET_HOME_ASSISTANT,
        [NET_SPEAKER] = UI_NET_SPEAKER,
        [NET_READY] = UI_NET_READY,
        [NET_FAILED] = UI_NET_FAILED,
    };
    ui_net_show(AS[net_state()]);
    /* And whether the speaker it plays through is actually there, which is the
     * stage that was missing on 16 Nov 2026: three services up, nothing able
     * to make a sound, and the dial saying Connected. Only once it has been
     * asked - before that the screen keeps the benefit of the doubt. */
    net_status_t st = net_status();
    if (st.speaker >= 0) ui_net_speaker(st.speaker == 1);
}

/* The Connections screen is open and wants a fresh look. net_check.c does the
 * asking on its own task - four requests, a second or two - and the answers are
 * picked up on the next poll by status_apply below. */
static void status_read(void)
{
    net_status_refresh();
    net_status_t st = net_status();
    ui_status_set(st.wifi, st.home_assistant, st.music_assistant, st.speaker, st.checking);
}

/* ...and picked up here, on the LVGL thread, the way net_apply picks up the
 * network's own state. Only while that screen is the one on the glass. */
static void status_apply(void)
{
    if (ui_view() != UI_VIEW_STATUS) return;
    net_status_t st = net_status();
    ui_status_set(st.wifi, st.home_assistant, st.music_assistant, st.speaker, st.checking);
}

/* An update over Wi-Fi, on the screen. ota.c does the work on its own task and
 * keeps its progress in atomics; this is the LVGL thread picking it up, which
 * is the only thread allowed to touch a screen. The states are named the same
 * on both sides, so this is a translation and nothing more. */
static void ota_apply(void)
{
    int percent;
    ota_fail_t why;
    ota_state_t st = ota_state(&percent, &why);
    static const ui_update_t AS[] = {
        [OTA_IDLE] = UI_UPDATE_NONE,          [OTA_CONNECTING] = UI_UPDATE_CONNECTING,
        [OTA_DOWNLOADING] = UI_UPDATE_DOWNLOADING, [OTA_INSTALLING] = UI_UPDATE_INSTALLING,
        [OTA_FAILED] = UI_UPDATE_FAILED,
    };
    ui_update_show(AS[st], percent, ota_fail_text(why));
}

/* ---- Low power: two minutes behind the clock (30 Oct 2026, rebuilt 4 Nov) -----
 * The screen goes off and everything that was only running to keep it fed
 * stops: the cover loaders, the fps log, the knob's 3 ms sampling timer (the
 * lines raise interrupts instead), and the touch panel drops from 200 reads a
 * second to 16. The processor is released from its top clock and Wi-Fi is let
 * sleep between beacons, so the knob stays on the network and HA can still
 * reach it - music starting anywhere wakes it.
 *
 * And, since 4 Nov 2026, the processor actually stops: light sleep between
 * those polls, and the panel itself put to sleep rather than merely unlit. What
 * was here before looked like a low-power state and was not one - measured on
 * the wall, a dial asleep like this still lost 19% of its battery an hour and
 * was flat in five and a half hours. Deep sleep is a tier further down again
 * (see deep_sleep below), two hours after the last touch. */
#define INPUT_POLL_MS       10
#define INPUT_POLL_SLEEP_MS 100
#define LVGL_WAIT_MAX_MS       10
#define LVGL_WAIT_MAX_SLEEP_MS 100

static lv_timer_t *s_input_timer, *s_stats_timer;
static esp_pm_lock_handle_t s_cpu_lock;
static volatile bool s_asleep;

/* The amp is only asked while there is both music to hear and somebody awake
 * to be told about it. Either one missing and the question is not asked and
 * what was known is forgotten (amp.h), so the screen says nothing. */
static void amp_follow(void)
{
    amp_awake(s_music_on && !s_asleep);
}

static void set_asleep(bool asleep)
{
    s_asleep = asleep;
    board_display_sleep(asleep);  /* fades down; coming back, the first frame raises it */
    board_touch_sleep(asleep);
    board_knob_sleep(asleep);
    board_haptic_sleep(asleep);   /* it draws milliamps out of standby, buzzing or not */
    library_pause(asleep);        /* no covers fetched for a screen nobody can see */
    ha_volume_follow(!asleep);    /* nor the volume read back for an arc nobody can see */
    amp_follow();                 /* nor the amp asked whether it can hear, with nobody to tell */
    lv_timer_set_period(s_input_timer, asleep ? INPUT_POLL_SLEEP_MS : INPUT_POLL_MS);
    if (asleep) lv_timer_pause(s_stats_timer);
    else lv_timer_resume(s_stats_timer);
    esp_wifi_set_ps(asleep ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (s_cpu_lock) {
        if (asleep) esp_pm_lock_release(s_cpu_lock);
        else esp_pm_lock_acquire(s_cpu_lock);
    }
    ESP_LOGI(TAG, "%s", asleep ? "SLEEP screen off, loaders stopped, knob on interrupts, Wi-Fi dozing"
                               : "WAKE screen on, everything back");
}

/* ---- Deep sleep (4 Nov 2026) --------------------------------------------------
 * Light sleep above gets the dial from five hours to days. This is what gets it
 * from days to weeks, and it is only worth having because of how little the
 * dial is actually used: three spells of about a quarter of an hour, so better
 * than 23 hours out of 24 with nobody near it.
 *
 * What it costs is honest and worth writing down. A turn of the knob wakes it;
 * a tap cannot, because the glass is read over I2C by a task and no task is
 * running. Coming back is a restart, so it takes several seconds to have Wi-Fi,
 * Music Assistant and the covers again. And while it is under, Home Assistant
 * cannot reach it at all - which is why it wakes itself on a timer to report in
 * and ask whether a firmware is waiting. An update asked for while it is deep
 * asleep lands at the next check-in, not at once (tools/fw.sh says so when it
 * gives up waiting). */
static bool s_checkin_boot;  /* this boot is the timer check-in, not somebody's hand */

static void deep_sleep(void)
{
    /* Nothing half-written goes under. */
    if (ota_busy()) return;
    /* And nothing goes under while it is on the charger. There is no battery to
     * save, and dropping off the network for six hours at a time while plugged
     * in is actively wrong: Home Assistant stops being able to reach it, and
     * the charge reading freezes, so there is no way to see it fill up. The
     * rail is read here and now rather than taken from the once-a-minute
     * report, because this decision is worth a fresh ADC read (4 Nov 2026).
     *
     * This is also how deep sleep is tested from the bench: unplug it. The
     * console's `deep` comes through here too, on purpose - a single path, so
     * what is tested is what runs. */
    if (board_battery_mv() >= BATT_CHARGER_MV) {
        ESP_LOGI(TAG, "deep sleep held off: on charge, so there is nothing to save");
        return;
    }
    /* A line resting low would wake it again the instant it went under, so wait
     * for the knob to be at rest; the idle check comes back in two seconds. */
    if (!board_knob_at_rest()) {
        ESP_LOGW(TAG, "deep sleep held off: a knob line is low");
        return;
    }
    /* The touch chip has to be told, because board_touch_init deliberately
     * turned its own auto-sleep off, and left running it would draw more than
     * the sleeping processor does. */
    board_touch_off(true);
    board_haptic_sleep(true);   /* the console's `deep` can get here with the screen on */
    board_backlight(0);
    esp_wifi_stop();

    /* Both knob lines idle high and a detent pulls one low, so any-low is a
     * turn. These are GPIO 7 and 8, which are RTC pins on the S3 - the reason
     * a turn can wake it from here at all. */
    esp_sleep_enable_ext1_wakeup_io(board_knob_pin_mask(), ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_enable_timer_wakeup((uint64_t)CORE_DEEP_SLEEP_CHECK_MS * 1000ULL);
    ESP_LOGI(TAG, "DEEP SLEEP: a turn of the knob, or %d hours, brings it back",
             (int)(CORE_DEEP_SLEEP_CHECK_MS / 3600000));
    esp_deep_sleep_start();
}

/* Woken by the timer rather than by a hand: the screen stays off, Home
 * Assistant gets told the charge, the update check gets its one look, and then
 * the dial goes straight back down. If a turn came in while that was happening
 * the UI is awake and this leaves it alone. */
#define CHECKIN_WINDOW_MS 30000

static void checkin_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CHECKIN_WINDOW_MS));
    if (ui_asleep() && !ota_busy()) {
        ESP_LOGI(TAG, "checked in; going back down");
        deep_sleep();
    } else {
        ESP_LOGI(TAG, "checked in and staying up: %s", ota_busy() ? "an update is landing" : "somebody is here");
    }
    vTaskDeleteWithCaps(NULL);  /* created WithCaps: see eq_seed */
}

/* The processor runs at its top clock whenever the knob is awake - the lock is
 * taken at start-up and only let go when the screen goes off - so nothing about
 * browsing or drawing changes. Asleep it falls to the minimum. */
static void power_init(void)
{
    /* A detent is what has to bring the processor out of light sleep, and a
     * level on a GPIO is the only thing that can: the GPIO peripheral has no
     * clock while the processor is stopped, so an edge is never seen. The pins
     * themselves are armed in board_knob_sleep; this is the one global switch
     * that lets GPIO be a wake source at all. */
    esp_sleep_enable_gpio_wakeup();
    esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        /* On since 4 Nov 2026. Off, the processor never actually stopped - it
         * only dropped to 80 MHz - and the dial asleep on the wall still lost
         * 19% of its battery an hour. It was off because light sleep takes the
         * USB console with it, which IDF now handles by holding light sleep
         * away only while a USB host is really plugged in (see
         * USJ_NO_AUTO_LS_ON_CONNECTION in sdkconfig.defaults), so the bench
         * keeps its console and the wall gets its sleep from one build. */
        .light_sleep_enable = CONFIG_KNOB_LIGHT_SLEEP ? true : false,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) { ESP_LOGW(TAG, "no power management: %s", esp_err_to_name(err)); return; }
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "awake", &s_cpu_lock) != ESP_OK) { s_cpu_lock = NULL; return; }
    esp_pm_lock_acquire(s_cpu_lock);
}

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lvgl_task(void *arg)
{
    /* Display buffers need internal DMA memory, so they're taken before Wi-Fi starts. */
    board_display_init();
    mem_mark("display");
    board_display_set_post_render(ui_overlay_apply);
    /* No shadow copy of the screen at start-up any more (6 Nov 2026): it is
     * 259 KB of PSRAM and a copy of every pixel sent, for the `frame` and
     * `shots` console commands alone. They switch it on when they are first
     * used (shadow_on below), so the bench keeps its screenshots and the wall
     * keeps the memory. */
    library_init();                 /* cover pool in PSRAM, before Wi-Fi takes memory */
    mem_mark("cover pool");
    voice_mic_init();               /* the microphone's DMA memory, likewise */
    mem_mark("microphone");
    xTaskNotifyGive((TaskHandle_t)arg);
    board_touch_init();
    board_haptic_init();
    ui_haptic_init(board_haptic);
    ui_haptic_enable(haptics_saved());
    ESP_LOGI(TAG, "HAPTICS %s at start-up", ui_haptic_enabled() ? "on" : "off");
    board_knob_init();
    board_battery_init();
    /* The charge the dial last worked out, back from RTC memory (16 Nov 2026).
     * battery_report only computes one while the dial is asleep - awake, the
     * screen loads the rail and the reading is worthless - so a dial that has
     * restarted and stayed awake has no figure at all, and Settings showed no
     * battery and "--" where the charge goes. Four updates in half an hour was
     * all it took to find. The figure survives a software reset in RTC memory
     * and is already trusted enough to be posted to Home Assistant on the way
     * back up, so it is good enough for the screen too. A cold power-on clears
     * it, and then there is genuinely nothing to show until the dial sleeps. */
    if (s_batt_rtc_magic == BATT_RTC_MAGIC && s_batt_rtc_said >= 0) {
        s_batt_said = s_batt_rtc_said;
        ESP_LOGI(TAG, "DEVICE charge %d%% carried over the restart", s_batt_said);
    }
    ui_init(library_get());
    ui_voice_init(voice_init());
    mem_mark("screens");
    s_input_timer = lv_timer_create(input_poll, INPUT_POLL_MS, NULL);
    s_stats_timer = lv_timer_create(stats, 1000, NULL);
    ui_sleep_init(set_asleep);
#if CONFIG_KNOB_DEEP_SLEEP
    ui_deep_sleep_init(deep_sleep);
#endif
    ui_power_init(&s_power);
    ui_player_init(&s_player);
    ui_device_source(device_read);  /* Settings reads them as it opens */
    ui_status_source(status_read);   /* ...and Connections asks the network as it opens */
#if CONFIG_KNOB_BENCH_ON_BOOT
    s_bench_running = true;
    ui_bench_start(frames, bench_done);
#endif
    /* The timer check-in must not light the bathroom up at four in the morning,
     * so the UI is put into its asleep state before a single frame is drawn.
     * The backlight is still at zero at this point and board_display_sleep
     * knows not to fade from a brightness that was never there. */
    if (s_checkin_boot) {
        ui_test_sleep();
        /* ...and no opening behind the dark screen. It would have run in full
         * - logo, connecting, ticks - with nobody in the room and the
         * backlight off (4 Nov 2026). */
        ui_logo_hide();
        ui_net_skip();
    }
    for (;;) {
        uint32_t wait = lv_timer_handler();
        uint32_t cap = s_asleep ? LVGL_WAIT_MAX_SLEEP_MS : LVGL_WAIT_MAX_MS;
        vTaskDelay(pdMS_TO_TICKS(wait < 1 ? 1 : wait > cap ? cap : wait));
    }
}

/* Wi-Fi is up: load the library, and let the Home Assistant calls go out. */
static void net_up(void)
{
    /* Wi-Fi is up, which is the test a freshly updated firmware has to pass:
     * it is exactly the ability to be updated again, so a build that gets here
     * is a build worth keeping and the bootloader stops holding the old one in
     * reserve (ota.c). */
    mem_mark("wifi up");
    ota_validate();
    ha_start();
    amp_start();
    playback_start();
    library_start();
    /* The amp's own tone settings, read once off the LVGL thread. */
    xTaskCreatePinnedToCoreWithCaps(eq_seed, "eqseed", 6144, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
    /* And the amps themselves, now rather than on the first touch (Darren, 4
     * Nov 2026). The amp takes up to two minutes to answer and nothing the
     * dial does can hurry it, so the only thing left is to start it sooner:
     * asking here instead of waiting for a finger buys most of the time he
     * spends watching the logo and the connecting sequence.
     *
     * Not on a check-in boot. That one wakes itself in the middle of the night
     * with nobody in the room, says what its charge is and goes back down -
     * and switching the bathroom amps on at four in the morning because the
     * dial fancied a word with Home Assistant would be its own fault report. */
    if (!s_checkin_boot) power_on();
    /* Woken by the timer and not by a hand: give Home Assistant its half minute
     * and then go back down. */
    mem_mark("services");
    if (s_checkin_boot) {
        xTaskCreatePinnedToCoreWithCaps(checkin_task, "checkin", 4096, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
    }
}

void app_main(void)
{
    /* The backlight pin, held low before anything else. Not the cause of the
     * flash at switch-on - it is already low when we get here - but the right
     * thing to do with it regardless (board_display.c). */
    board_backlight_off_early();

    /* First, before anything that could hang: if this firmware arrived over
     * Wi-Fi and has not yet proved itself, start the clock that puts the
     * previous one back (ota.c). Nothing else in here is allowed to be the
     * reason a dial on a wall needs a cable. */
    ota_boot_check();
    /* Before the screen: a touch in the seconds before Wi-Fi comes up must
     * still be able to ask for the amps (ha_hold_asks). */
    ha_hold_asks();
    power_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    /* blocking reads from the USB serial port, for the console */
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usb_cfg);
    usb_serial_jtag_vfs_use_driver();
    /* Why it is running: the knob restarted by itself at 16:29 on 1 Nov 2026
     * and the only trace of it was the clock showing its "time not set yet"
     * line, so there was nothing to say whether it had crashed, run out of
     * memory or simply been unplugged. One line at every boot answers that
     * next time: "power on" is the plug, "software" is a reset from the
     * console or a flash, and panic, a watchdog or a brownout is the knob
     * falling over. The free heap goes with it, since a slow leak shows up as
     * a smaller number each boot. */
    /* ...and when it was deep sleep that ended, what ended it: a hand on the
     * knob, or the check-in timer. The two are treated completely differently -
     * one lights the screen, the other must not - so the log says which. */
    esp_sleep_wakeup_cause_t woke = esp_sleep_get_wakeup_cause();
    s_checkin_boot = (woke == ESP_SLEEP_WAKEUP_TIMER);
    amp_recover_counter_init(woke);
    ESP_LOGI(TAG, "BOOT after %s%s, internal RAM free %u",
             crash_reset_name(),
             woke == ESP_SLEEP_WAKEUP_TIMER ? " (check-in timer, screen stays off)"
             : woke == ESP_SLEEP_WAKEUP_EXT1 ? " (the knob was turned)"
                                             : "",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    mem_mark("start");
    lv_init();
    lv_tick_set_cb(tick_ms);
    /* Drawing gets core 1 to itself; Wi-Fi and the checks run on core 0. */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16 * 1024, xTaskGetCurrentTaskHandle(), 5, NULL, 1);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    net_check_start(net_up);
    mem_mark("wifi started");
    s_cmds = xQueueCreate(16, CMD_LEN);
    xTaskCreatePinnedToCoreWithCaps(console_task, "console", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM);
}
