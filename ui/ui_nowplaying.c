/* Screens 04/05: Now Playing, for one album's tracks, or a playlist's
 * (UI_PLAYLIST(p), 27 Oct 2026: the same screens, each track with its own
 * picture).
 *
 * Paused (or not the album that's playing): play disc and title band; swipe or
 * turn the knob to pick a track; the rim marks which track of how many. Tap to
 * play.
 * Playing: transport bar (thirds: back 5 s, pause, forward 5 s), progress bar,
 * title band only peeking on track changes; swipe changes track; the knob is
 * the volume and the rim shows the volume arc. A swipe borrows the rim for the
 * track marker until shortly after it lands. Quick skips add up in the label.
 * Holding back or forward skips faster and faster: 30 s at a time, then a
 * minute, then 5 minutes (Darren, 27 Oct 2026; core_skip_hold_step).
 *
 * Playback is simulated on the knob for now (timing from Music Assistant's
 * track lengths); nothing is sent to Music Assistant or the amp yet. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_strip.h"

#define TRACK_MARK 0xFFFFFF
#define TRACK_GAP 0.004f        /* gap either side of the track marker, in turns */
#define TRACK_MIN 0.012f        /* ...and the shortest marker still worth drawing */
#define PROGRESS_FILL 0xFF00FB
#define PROGRESS_TRACK 0x919191
#define LOAD_W 120              /* loading bar, centred in the title band */
#define LOAD_SEG_W 36
#define LOAD_SWEEP_MS 700

static const ui_library_t *s_lib;
static lv_obj_t *s_root, *s_paused_ui, *s_transport, *s_fill, *s_loadbar, *s_loadseg, *s_amp_note;
/* Dim enough to read as "not yet", not so dim it looks broken or missing. */
/* How long the track marker keeps the rim after the last movement, before the
 * spinner has it back. Long enough to pick a track off a list and look at what
 * you have landed on; short enough that, having chosen, the rim goes back to
 * saying the dial is still working on the amp. */
#define MARKER_HOLD_MS 1600
static strip_t *s_strip;
static int s_album = -1;           /* the album (or UI_PLAYLIST) on screen */
static const ui_tracks_t *s_tracks;
static float s_press_pos;
static char s_want_track[96];      /* from search: land on this track once the list is in */
static ui_view_t s_back = UI_VIEW_ALBUMS;  /* where dragging down goes: wherever the album was opened from */

static struct {
    int album, track;
    int asked;                     /* the track the player was last told to play */
    bool playing;
    uint32_t elapsed_ms;
    int volume;                    /* 0-100, 2 per click (prototype); real range comes with the amp */
} s_player = {.album = -1, .volume = CORE_VOLUME_START};

static int32_t s_arc_shown = CORE_VOLUME_START * 10;  /* volume x 10 currently drawn */
static lv_timer_t *s_play_timer, *s_peek_timer, *s_unborrow_timer, *s_marker_timer;
static uint32_t s_last_tick;
static bool s_borrowed;            /* playing, but the rim shows the track marker while swiping */
static int s_skip_total;           /* seconds actually skipped in the current run of quick presses */
static int s_skip_dir;             /* that run's direction */
static uint32_t s_skip_at;

/* Scrubbing: the position is the knob's own while it moves (see "Scrubbing") */
static lv_obj_t *s_scrub_view, *s_scrub_time, *s_scrub_total, *s_scrub_fill, *s_scrub_handle;
static lv_timer_t *s_scrub_rest;   /* no scrubbing for a while: close the screen */
static lv_timer_t *s_scrub_settle; /* the knob has stopped turning: send the seek */
static bool s_scrubbing;
static bool s_scrub_moved;         /* the position was really changed, so a seek is owed */
static bool s_scrub_sent;          /* a seek has gone out: the music is where the handle is */
static bool s_scrub_touch;         /* a finger is on the glass */
static uint32_t s_scrub_ms;        /* where the scrubber is, in ms into the track */
static uint32_t s_seek_want_ms;    /* a seek sent and not yet carried out: see CORE_SEEK_WAIT_MS */
static uint32_t s_seek_at;
static bool s_seek_pending;
static uint32_t s_scrub_press;     /* ...and where it was when the finger landed */
static bool s_time_tap;            /* this touch started on the play time */
static char s_scrub_text[12], s_scrub_total_text[12];

/* ---- Recent as one list to play through (Darren, 27 Oct 2026) -----------------
 * Opened from Recent, Now Playing steps through the recent tracks themselves,
 * not the album or playlist the first came from: each slide is one recent
 * track, with its own picture (its album's cover, or its playlist picture),
 * and playback carries on down the list. A snapshot is taken on opening, so the
 * list doesn't reorder while it's being played. */
#define NP_RECENT 0x0FFFFF  /* s_album / s_player.album: the Recent list */
#define MIX_MAX 20

static struct {
    int coll[MIX_MAX], index[MIX_MAX];  /* where each lives; its track there, -1 until known */
    char title[MIX_MAX][96];
    const char *titles[MIX_MAX];
    uint16_t seconds[MIX_MAX];
    ui_tracks_t t;
} s_mix UI_IN_PSRAM;
static int s_start = -1;  /* open on this slide (Recent), else as start_track says */

static bool is_mix(int c) { return c == NP_RECENT; }
static const ui_tracks_t *tracks_of(int c) { return is_mix(c) ? &s_mix.t : s_lib->tracks(c); }

/* Find recent track i in its album or playlist, once that track list is in
 * (asking starts the load). */
static bool mix_resolve(int i)
{
    if (s_mix.index[i] >= 0) return true;
    const ui_tracks_t *t = s_lib->tracks(s_mix.coll[i]);
    if (!t) return false;
    for (int k = 0; k < t->count; k++)
        if (!strcmp(t->title[k], s_mix.title[i])) {
            s_mix.index[i] = k;
            s_mix.seconds[i] = t->seconds[k];
            return true;
        }
    s_mix.index[i] = 0x7FFF;  /* not there any more: no picture, a default length */
    return true;
}

static bool mix_has(int coll)
{
    for (int i = 0; i < s_mix.t.count; i++)
        if (s_mix.coll[i] == coll) return true;
    return false;
}

/* Load the picture of the recent track at `pos`: its album's cover, or its
 * playlist track's picture. */
static void mix_focus(float pos)
{
    int i = (int)lroundf(pos);
    if (i < 0 || i >= s_mix.t.count) return;
    int c = s_mix.coll[i];
    if (!UI_IS_PLAYLIST(c)) s_lib->focus((float)c, 0.0f);
    else if (mix_resolve(i) && s_mix.index[i] != 0x7FFF && s_lib->track_focus) s_lib->track_focus(c, (float)s_mix.index[i]);
}

static void out_play_settled(void);  /* the player: see "The player" below */
static bool can_play(void);          /* whether the amp could hear a track if one started */
static void rim(void);
static void out_stop_settle(void);
static void scrub_end(bool seek);    /* the scrubber: see "Scrubbing" below */
static void scrub_follow(void);
static void seek_asked(uint32_t to_ms);

bool np_playing(void) { return s_player.playing; }
int np_player_album(void) { return s_player.album; }
bool np_showing_player(void) { return ui_view() == UI_VIEW_NOWPLAYING && s_album == s_player.album; }
static bool playing_here(void) { return s_player.playing && s_player.album == s_album; }
static int ntracks(void) { return s_tracks ? s_tracks->count : 0; }
static int shown_track(void) { return strip_target(s_strip); }

static uint32_t track_ms(int album, int track)
{
    const ui_tracks_t *t = album == s_album ? s_tracks : tracks_of(album);
    uint32_t s = t && track < t->count ? t->seconds[track] : 0;
    return (s ? s : 180) * 1000u;
}

/* ---- Fades -------------------------------------------------------------------- */
/* Something faded right out is hidden, not just transparent: LVGL would
 * otherwise still build a blending layer for it every frame (that took the
 * knob from 64 to 25 fps while playing). */
static void set_opa(lv_obj_t *obj, lv_opa_t v)
{
    lv_obj_set_style_opa(obj, v, 0);
    if (v == LV_OPA_TRANSP) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
static void opa_exec(void *obj, int32_t v) { set_opa(obj, (lv_opa_t)v); }
static void fade(lv_obj_t *obj, lv_opa_t to, uint32_t ms)
{
    lv_opa_t from = lv_obj_get_style_opa(obj, 0);
    if (from == to) return;
    ui_anim(obj, opa_exec, from, to, ms, 0.25f, 0.1f, 0.25f, 1.0f);
}

static lv_opa_t s_band_opa = LV_OPA_COVER;
static void band_exec(void *var, int32_t v) { (void)var; s_band_opa = (lv_opa_t)v; strip_set_band_opa(s_strip, (lv_opa_t)v); }
static void band_fade(lv_opa_t to, uint32_t ms) { ui_anim(&s_band_opa, band_exec, s_band_opa, to, ms, 0.25f, 0.1f, 0.25f, 1.0f); }

/* Title peek while playing: fade in 250 ms, hold one scroll pass (2-6 s), fade out 500 ms */
static void peek_end(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (playing_here()) band_fade(LV_OPA_TRANSP, 500);
}

static void peek(void)
{
    if (!playing_here()) return;
    band_fade(LV_OPA_COVER, 250);
    uint32_t hold = strip_title_cycle_ms(s_strip, shown_track());
    hold = hold < 2000 ? 2000 : hold > 6000 ? 6000 : hold;
    lv_timer_set_period(s_peek_timer, hold);
    lv_timer_reset(s_peek_timer);
    lv_timer_resume(s_peek_timer);
}

/* ---- Rim: track marker when picking, volume arc when playing ------------------ */
/* Where the limit sits on the rim. The arc below it is the floor-to-limit
 * stretch and the arc above it the limit-to-100 stretch, so the colour change
 * and the value mapping are the same number and cannot drift apart. */
#define ARC_SPLIT (CORE_VOLUME_LIMIT / 100.0f)

/* The arc's zero is CORE_VOLUME_FLOOR, not 0 (Darren, 5 Nov 2026): the quiet
 * end of the dial was unusable, so showing it ate most of the rim for nothing.
 * Floor to limit now fills everything up to the colour change, and limit to
 * 100 the red part above it - the same shape as before, but the part the hand
 * actually uses is spread over five sixths of the rim instead of a quarter of
 * it. Takes volume in tenths of a percent, as s_arc_shown holds it. */
static float arc_frac(int32_t tenths)
{
    float v = tenths / 10.0f;
    if (v <= CORE_VOLUME_LIMIT) {
        /* Home Assistant can still report a volume under the floor, so this
         * clamps rather than trusting the range. */
        float f = (v - CORE_VOLUME_FLOOR) / (float)(CORE_VOLUME_LIMIT - CORE_VOLUME_FLOOR);
        return f < 0 ? 0 : f * ARC_SPLIT;
    }
    return ARC_SPLIT + (1.0f - ARC_SPLIT) * (v - CORE_VOLUME_LIMIT) / (float)(100 - CORE_VOLUME_LIMIT);
}

/* Yellow to the 80% limit, red past it (Darren, 30 Oct 2026): the loud part of
 * the turn looks loud, and the limit is a colour change rather than a surprise. */
static void volume_arc(float to)
{
    ui_overlay_arc2(true, UI_VOLUME, UI_VOLUME_LOUD, ARC_SPLIT, 0, to);
}

/* Is the volume arc the rim's job this moment? Now Playing draws it while the
 * track on screen is the one playing and the rim hasn't been lent to a swipe;
 * the EQ overview borrows it. Everywhere else the rim belongs to something else
 * or to nothing at all. */
static bool volume_arc_is_up(void)
{
    /* Not while the amp cannot hear: the arc would move and nothing would
     * happen, because send_numbers holds every write behind amp_ready. The rim
     * says what the dial is really doing instead, and the volume arc takes
     * over the moment the amp answers. */
    if (!can_play()) return false;
    if (ui_view() == UI_VIEW_NOWPLAYING) return playing_here() && !s_borrowed;
    if (ui_view() == UI_VIEW_EQ) return eq_on_overview();
    return false;
}

/* The arc is only ever drawn where it belongs (Darren, 2 Nov 2026: he turned
 * the volume down from the iPad with the menu up and the volume arc appeared
 * over the menu). Turning the knob can only reach this from the two screens
 * where the arc is the rim's, so the guard never mattered until the volume
 * started arriving from Home Assistant on its own; it also closes the window
 * where leaving Now Playing mid-animation drew an arc over whatever came next.
 * The value is still kept up to date, so the arc is right the moment the rim
 * is the volume's again. */
static void arc_value(void *var, int32_t v)
{
    (void)var;
    s_arc_shown = v;
    if (volume_arc_is_up()) volume_arc(arc_frac(v));
}

/* The track marker has had the rim long enough since it last moved: hand it
 * back, which with the amp still waking means the spinner takes it (the rim is
 * a fallback - see ui_overlay_spin). "When I've selected something I want to
 * play and it is not yet connected to the amplifier, it should then return to
 * the blue spinning circle" (Darren, 4 Nov 2026). */
static void marker_done(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (ui_view() == UI_VIEW_NOWPLAYING && !playing_here() && !can_play())
        ui_overlay_arc(false, TRACK_MARK, 0, 0);
}

static void scrub_paint(void);

static void rim(void)
{
    if (ui_view() != UI_VIEW_NOWPLAYING) return;
    /* While the handle is out, the rim is the scrubber's and nothing else
     * gets to put anything on it (Darren, 5 Nov 2026: turning through a track
     * made the purple position arc flash to the track marker, and once to the
     * yellow volume arc). rim() is called from everything that changes what is
     * on screen - the strip settling, the amp answering, the ordinary update -
     * and none of them knew a scrub was running, so each one drew over it for
     * a frame until the next scrub_paint put it back. Deciding it here means
     * every caller is covered rather than the ones I happened to find. */
    if (s_scrubbing) { scrub_paint(); return; }
    if (playing_here() && !s_borrowed && can_play()) {
        /* Through volume_arc, like every other draw of it. This drew the rim
         * itself - raw volume over 100, single colour, no limit split - and so
         * it was the one place the new mapping did not reach: the arc opened a
         * third full at the starting volume and then snapped back to empty on
         * the first click (Darren, 5 Nov 2026). It is also where the arc is
         * first drawn when a track starts, which is why that was the moment it
         * showed. Nothing here needs to know how a volume becomes an arc. */
        volume_arc(arc_frac(s_arc_shown));
    } else if (ntracks() > 0) {
        int n = ntracks();
        float k = strip_pos(s_strip);
        /* The gap either side separates one track from the next, and the
         * marker is held to a length that can still be seen. On a long album
         * the two gaps together are wider than a track's whole share of the
         * rim, which left nothing drawn at all - Radio 1's Essential Mix has
         * 150 tracks and showed no marker (Darren, 29 Oct 2026). Anything over
         * 125 tracks was in that state. */
        float a = k / n, b = (k + 1) / n;
        float gap = (b - a) < 4 * TRACK_GAP ? (b - a) / 4.0f : TRACK_GAP;
        float from = a + gap, to = b - gap;
        if (to - from < TRACK_MIN) {
            float mid = (a + b) / 2.0f;
            from = mid - TRACK_MIN / 2.0f;
            to = mid + TRACK_MIN / 2.0f;
            if (from < 0.0f) { to -= from; from = 0.0f; }
            if (to > 1.0f) { from -= to - 1.0f; to = 1.0f; }
        }
        ui_overlay_arc(true, TRACK_MARK, from, to);
        /* Choosing is movement; having chosen is stillness. While the amp is
         * still waking, stillness gives the rim back to the spinner. */
        if (!can_play()) {
            if (!s_marker_timer) {
                s_marker_timer = lv_timer_create(marker_done, MARKER_HOLD_MS, NULL);
                lv_timer_set_auto_delete(s_marker_timer, false);
            }
            lv_timer_set_repeat_count(s_marker_timer, 1);
            lv_timer_reset(s_marker_timer);
            lv_timer_resume(s_marker_timer);
        }
    } else {
        ui_overlay_arc(false, TRACK_MARK, 0, 0);
    }
}

static void borrow(void)
{
    s_borrowed = true;
    lv_timer_pause(s_unborrow_timer);
}

static void unborrow(lv_timer_t *t)
{
    (void)t;
    lv_timer_pause(s_unborrow_timer);
    s_borrowed = false;
    rim();
}

/* ---- The play time in the label slot (Darren, 31 Oct 2026) ----------------------
 * The progress bar shows how far through the track the music is; this says
 * where that is in minutes and seconds. It lives in the same slot as the
 * volume, which stands in front of it for a second and a half when the knob is
 * turned (ui.c's chip). */
static char s_time_text[12];
static int s_time_shown = -1;

const char *np_play_time(void)
{
    if (ui_view() != UI_VIEW_NOWPLAYING || !playing_here()) return NULL;
    if (s_scrubbing) return NULL;  /* the scrubber is showing the same time, far bigger */
    return s_time_text;
}

/* Only when the second has turned over, or when the slot has to be taken back
 * (opening the screen): setting the label every frame would redraw the top of
 * the screen a hundred times a second for nothing. */
static void play_time(bool force)
{
    int sec = playing_here() ? (int)(s_player.elapsed_ms / 1000u) : -1;
    if (sec == s_time_shown && !force) return;
    s_time_shown = sec;
    if (sec >= 0) core_time_label(sec, s_time_text, sizeof s_time_text);
    ui_chip_rest();
}

/* ---- Paused vs playing --------------------------------------------------------- */
static void progress(void)
{
    uint32_t len = track_ms(s_player.album, s_player.track);
    int w = playing_here() || s_player.album == s_album ? (int)(222ull * s_player.elapsed_ms / len) : 0;
    lv_obj_set_width(s_fill, w > 222 ? 222 : w);
}

/* Whether pressing play would actually play anything (Darren, 4 Nov 2026).
 *
 * "I don't need the play button because I can't actually play anything. So
 * what you're doing is you're telling me you're doing one thing and you're
 * giving me the option to do something that I actually can't do."
 *
 * While the amp is still waking, a press does nothing but lose the track he
 * picked, so the disc is not there at all and the press is ignored. Dimmed
 * first, which was still wrong: "I don't want it dimmed. It shouldn't be
 * present. Don't show me something that I'm physically incapable of doing"
 * (Darren, 4 Nov 2026). Only CONNECTING
 * blocks it: UNKNOWN means the amp has not been asked at all, and a play
 * button that could never be pressed because nothing ever asked would be a
 * far worse fault than the one this fixes. */
static bool can_play(void) { return ui_amp() != UI_AMP_CONNECTING; }

/* The amp answered, or stopped answering: the disc goes live or goes dim. */
void np_amp_changed(void)
{
    if (ui_view() == UI_VIEW_NOWPLAYING && !playing_here()) {
        fade(s_paused_ui, can_play() ? LV_OPA_COVER : LV_OPA_TRANSP, 280);
        fade(s_amp_note, can_play() ? LV_OPA_TRANSP : LV_OPA_COVER, 280);
    }
    rim();
}

static void update(void)
{
    bool p = playing_here();
    fade(s_paused_ui, p || !can_play() ? LV_OPA_TRANSP : LV_OPA_COVER, 280);
    fade(s_amp_note, !p && !can_play() ? LV_OPA_COVER : LV_OPA_TRANSP, 280);
    fade(s_transport, p ? LV_OPA_COVER : LV_OPA_TRANSP, 220);
    if (!p) band_fade(LV_OPA_COVER, 220);
    progress();
    play_time(true);
    rim();
}

/* ---- Strip ---------------------------------------------------------------------- */
/* Until the track list is in, the strip holds just the album's cover, with the
 * loading bar where the title goes, so opening an album never blanks the art
 * (Darren, 27 Oct 2026). */
static int src_count(void *ctx) { (void)ctx; return s_tracks ? s_tracks->count : s_album >= 0; }
static const char *src_title(void *ctx, int i) { (void)ctx; return s_tracks ? s_tracks->title[i] : ""; }
static const lv_image_dsc_t *src_cover(void *ctx, int i)
{
    (void)ctx;
    if (is_mix(s_album)) {
        if (i < 0 || i >= s_mix.t.count) return NULL;
        int c = s_mix.coll[i];
        if (!UI_IS_PLAYLIST(c)) return s_lib->cover(c);
        int k = s_mix.index[i];
        return k >= 0 && k != 0x7FFF && s_lib->track_cover ? s_lib->track_cover(c, k) : NULL;
    }
    if (!UI_IS_PLAYLIST(s_album)) return s_lib->cover(s_album);
    return s_lib->track_cover ? s_lib->track_cover(s_album, i) : NULL;
}

/* The picture behind the track on screen (the EQ shows it too). */
const lv_image_dsc_t *np_cover(void) { return s_album < 0 ? NULL : src_cover(NULL, strip_target(s_strip)); }
static void src_moved(void *ctx, float pos, float speed)
{
    (void)ctx;
    (void)speed;
    if (is_mix(s_album)) mix_focus(pos);
    else if (UI_IS_PLAYLIST(s_album) && s_lib->track_focus) s_lib->track_focus(s_album, pos);  /* load pictures around here */
    rim();
}

static void src_settled(void *ctx, int i)
{
    (void)ctx;
    if (!s_tracks) return;  /* the cover alone, while the tracks load */
    if (playing_here() && i != s_player.track) {
        s_player.track = i;
        s_player.elapsed_ms = 0;
        out_play_settled();  /* ...and play it once it has stayed there */
        progress();
        play_time(false);
        peek();
    }
    if (s_borrowed) {
        lv_timer_reset(s_unborrow_timer);
        lv_timer_resume(s_unborrow_timer);
    }
    rim();
}

/* ---- The player --------------------------------------------------------------------
 * The screens say what was asked for; ui.h's ui_player_t does it. Unset (the
 * simulator), everything below falls back to the simulated playback this was
 * built against, so the screens behave the same with no music in the room. */
static const ui_player_t *s_out;
static lv_timer_t *s_settle_timer;

/* Between asking the player to start and it saying it has (Darren, 2 Nov 2026:
 * picking a track off Recent counted up to four seconds over silence, then
 * snapped back to zero when the music came in). Music Assistant takes a moment
 * to open the stream and another to answer for the new queue, so the knob's own
 * clock stays put until the first real position arrives - but never for good,
 * in case the player stops answering while the music plays. */
#define WAIT_FOR_PLAYER_MS 8000
static bool s_awaiting;
static uint32_t s_asked_at;
/* This play is a resume of the track that was just paused, so there is nothing
 * to wait for. Set by pause, and only good for the track it was set on. */
static bool s_resuming;

bool np_loading(void)
{
    return s_awaiting && ui_view() == UI_VIEW_NOWPLAYING && playing_here() && !s_scrubbing;
}

/* While the wait is on, the label slot shows a sweeping bar in place of a time
 * standing still (ui.c draws it; Darren, 2 Nov 2026). */
static void wait_for_player(bool on)
{
    if (on) s_asked_at = lv_tick_get();
    if (s_awaiting == on) return;
    s_awaiting = on;
    play_time(true);  /* ...which hands the slot over, or takes it back */
}

void ui_player_init(const ui_player_t *player) { s_out = player; }

static bool wired(void) { return s_out && s_out->play; }

/* Which collection and track the player is being asked for. Picking from
 * Recent is passed on as the album or playlist that track lives in, so the
 * player never has to know what Recent is. */
static bool out_target(int *coll, int *track)
{
    int c = s_player.album, t = s_player.track;
    if (is_mix(c)) {
        if (t < 0 || t >= s_mix.t.count || !mix_resolve(t) || s_mix.index[t] == 0x7FFF) return false;
        c = s_mix.coll[t];
        t = s_mix.index[t];
    }
    if (c < 0) return false;
    *coll = c;
    *track = t;
    return true;
}

static void out_play(void)
{
    int coll, track;
    if (!wired() || !out_target(&coll, &track)) return;
    s_player.asked = s_player.track;
    /* The sweeping bar is for a wait that is actually happening: Music
     * Assistant has to resolve a stream and open it, which takes three or four
     * seconds, and a play time sitting at 0:00 through that reads as a frozen
     * screen (2 Nov 2026).
     *
     * Resuming the track that was just paused is not that wait. playback.c
     * already sends player_queues/play for it rather than loading the track
     * again - the queue is still open and the music comes back at once - but
     * this asked for the bar regardless, so a pause and an immediate play
     * showed a buffering animation over music that was already playing
     * (Darren, 4 Nov 2026: "almost like a half a second later, why is it still
     * buffering? I don't understand that"). It was the screen that was wrong,
     * not the player. */
    if (!s_resuming) wait_for_player(true);
    s_resuming = false;
    s_out->play(coll, track);
}

/* A track settled under the finger: only the one it comes to rest on is
 * played, CORE_TRACK_MARK_HOLD_MS after it lands, so swiping past three tracks
 * plays the third and not all three (PLANNING.md). */
static void settle_send(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (s_player.playing) out_play();
}

static void out_play_settled(void)
{
    if (!wired()) return;
    wait_for_player(true);  /* the track under the finger isn't playing yet either */
    if (!s_settle_timer) {
        s_settle_timer = lv_timer_create(settle_send, CORE_TRACK_MARK_HOLD_MS, NULL);
        lv_timer_set_auto_delete(s_settle_timer, false);
    }
    lv_timer_reset(s_settle_timer);
    lv_timer_resume(s_settle_timer);
}

static void out_stop_settle(void)
{
    if (s_settle_timer) lv_timer_pause(s_settle_timer);
}

/* ---- Playback --------------------------------------------------------------------- */
static void next_track_auto(void)
{
    const ui_tracks_t *t = s_player.album == s_album ? s_tracks : tracks_of(s_player.album);
    s_player.elapsed_ms = 0;
    if (t && s_player.track + 1 < t->count) {
        s_player.track++;
        out_play();  /* Recent steps itself; an album or playlist is the player's own queue */
        if (playing_here()) {
            strip_snap(s_strip, s_player.track, CORE_TRACK_SLIDE_MS);
            peek();
        }
    } else {
        s_player.playing = false;  /* end of the album: stop (Music Assistant's default) */
        if (wired() && s_out->stop) s_out->stop(0);
        update();
    }
}

static void play_tick(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get(), dt = lv_tick_diff(now, s_last_tick);
    s_last_tick = now;
    if (!s_player.playing) return;
    if (s_awaiting) {
        if (lv_tick_elaps(s_asked_at) < WAIT_FOR_PLAYER_MS) return;  /* nothing is playing yet */
        wait_for_player(false);
    }
    s_player.elapsed_ms += dt;
    uint32_t len = track_ms(s_player.album, s_player.track);
    if (s_player.elapsed_ms < len) {
        if (playing_here()) { progress(); play_time(false); scrub_follow(); }
        return;
    }
    /* Past the end. With a real player, only it knows whether the next track
     * has actually started, so the bar waits full rather than guessing; the
     * Recent list is the knob's own, so it steps that itself. */
    if (!wired() || is_mix(s_player.album)) next_track_auto();
    else {
        s_player.elapsed_ms = len;
        if (playing_here()) { progress(); play_time(false); }
    }
}

static void play(void)
{
    int track = shown_track();
    /* A different track is a real load, whatever was paused before it. */
    if (s_player.album != s_album || s_player.track != track) {
        s_player.elapsed_ms = 0;
        s_resuming = false;
    }
    s_player.album = s_album;
    s_player.track = track;
    s_player.playing = true;
    if (s_lib->played && s_tracks && track < s_tracks->count) {  /* to the top of Recent */
        if (is_mix(s_album)) s_lib->played(s_mix.coll[track], s_mix.title[track]);
        else s_lib->played(s_album, s_tracks->title[track]);
    }
    s_last_tick = lv_tick_get();
    out_stop_settle();
    out_play();
    band_fade(LV_OPA_TRANSP, 500);
    update();
}

static void pause_playback(void)
{
    scrub_end(false);  /* a paused track still has a position, but the slot is its own again */
    s_resuming = true;  /* ...and playing it again picks it straight back up */
    s_player.playing = false;
    out_stop_settle();
    if (wired() && s_out->pause) s_out->pause();
    lv_timer_pause(s_peek_timer);
    update();
}

/* Stop simulated playback entirely (tests use this so they don't leave the
 * knob "playing", which would trigger the return to Now Playing). */
void np_stop(void) { np_stop_after(0); }

void np_stop_after(int ms)
{
    scrub_end(false);
    bool was = s_player.playing;
    s_player.playing = false;
    s_awaiting = false;  /* the slot goes with the screen; no need to hand it back */
    s_resuming = false;  /* stopped is not paused: playing again is a real load */
    out_stop_settle();
    if (was && wired() && s_out->stop) s_out->stop(ms);
    s_player.album = -1;
    s_player.elapsed_ms = 0;
    lv_timer_pause(s_peek_timer);
    if (ui_view() == UI_VIEW_NOWPLAYING) update();
}

/* The label adds up what was actually skipped: back stops at the start of the
 * track and forward at its end (Darren, 27 Oct 2026: holding back from 3
 * minutes in showed -50:00). */
static void skip(int seconds)
{
    int dir = seconds > 0 ? 1 : -1;
    bool run = lv_tick_elaps(s_skip_at) < CORE_SKIP_RUN_MS && s_skip_dir == dir;
    s_skip_at = lv_tick_get();
    s_skip_dir = dir;
    int64_t from = s_player.elapsed_ms, len = track_ms(s_player.album, s_player.track);
    int64_t e = from + (int64_t)seconds * 1000;
    if (e < 0) e = 0;  /* skipping back stops at the start */
    bool past_end = e >= len;
    if (past_end) e = len;
    s_skip_total = (run ? s_skip_total : 0) + (int)((e - from + (e >= from ? 500 : -500)) / 1000);
    char buf[20];
    core_skip_label(s_skip_total, buf, sizeof buf);
    ui_chip(buf);
    if (past_end) next_track_auto();  /* past the end: next */
    else {
        s_player.elapsed_ms = (uint32_t)e;
        wait_for_player(false);
        if (wired() && s_out->seek) s_out->seek((int)(e / 1000));
        seek_asked((uint32_t)e);
        progress();
        play_time(false);
    }
}

/* ---- Scrubbing (Darren, 1 Nov 2026) ------------------------------------------------
 * Tap the play time and the track opens full screen: the time in the clock's
 * own thin figures, a slider under it. A detent moves CORE_SKIP_TAP_S, the
 * same as a tap on forward; a swipe drags the slider, the bar's width being
 * the whole track. The rim shows the position, because the rim always shows
 * what the knob is doing and here that is not the volume.
 *
 * Nothing reaches the player until the hand stops - see CORE_SCRUB_REST_MS for
 * why - and then the seek goes out and the screen closes together, so what is
 * left on Now Playing is the time he chose.
 *
 * The ways out that change nothing change nothing: a tap commits where it
 * already is, and a swipe down leaves without a seek. Opened and left alone it
 * closes by itself having sent nothing.
 */
#define SCRUB_BAR_W 222      /* the progress bar's own width: the two read as one thing */
#define SCRUB_BAR_H 6
#define SCRUB_BAR_X ((CORE_SCREEN - SCRUB_BAR_W) / 2)
#define SCRUB_BAR_Y 244
#define SCRUB_HANDLE 20
#define SCRUB_TIME_Y 112
#define SCRUB_TOTAL_Y 266

/* Wider than it looks, so a thumb finds it: the whole top of the screen above
 * the artwork is the play time as far as a touch is concerned, the same
 * bargain the letter badge makes in Albums. It stops short of the transport
 * band at y 138. */
#define SCRUB_HIT_W 170
#define SCRUB_HIT_H 120

static bool on_play_time(int x, int y)
{
    return np_play_time() && abs(x - CORE_SCREEN / 2) <= SCRUB_HIT_W / 2 && y <= SCRUB_HIT_H;
}

static void scrub_paint(void)
{
    uint32_t len = track_ms(s_player.album, s_player.track);
    core_minutes_label((int)(s_scrub_ms / 1000u), s_scrub_text, sizeof s_scrub_text);
    lv_label_set_text(s_scrub_time, s_scrub_text);
    float through = len ? (float)s_scrub_ms / (float)len : 0.0f;
    int w = (int)(SCRUB_BAR_W * through);
    if (w < 0) w = 0;
    if (w > SCRUB_BAR_W) w = SCRUB_BAR_W;
    lv_obj_set_width(s_scrub_fill, w < 1 ? 1 : w);
    /* The handle hangs over both ends of the bar, so it is the bar's sibling
     * and not its child: a child is clipped, and at the start of a track that
     * left nothing to see. */
    lv_obj_set_x(s_scrub_handle, SCRUB_BAR_X + w - SCRUB_HANDLE / 2);
    ui_overlay_arc(true, PROGRESS_FILL, 0, through);
}

static void scrub_move(int64_t to_ms)
{
    int64_t len = track_ms(s_player.album, s_player.track);
    if (to_ms < 0) to_ms = 0;
    if (to_ms > len) to_ms = len;
    if ((uint32_t)to_ms == s_scrub_ms) return;
    s_scrub_ms = (uint32_t)to_ms;
    s_scrub_moved = true;
    scrub_paint();
}

/* How long the screen stays up with nothing happening to it. */
/* A seek has gone out; the player has yet to answer from where it puts it. */
static void seek_asked(uint32_t to_ms)
{
    s_seek_want_ms = to_ms;
    s_seek_at = lv_tick_get();
    s_seek_pending = true;
}

/* Is this reported position the one from before that seek? Measured against
 * where the seek says the music should have got to by now, so it reads a move
 * backwards down a track the same way as a move forwards. */
static bool seek_stale(uint32_t elapsed_ms)
{
    if (!s_seek_pending) return false;
    uint32_t since = lv_tick_elaps(s_seek_at);
    if (since > CORE_SEEK_WAIT_MS) { s_seek_pending = false; return false; }
    int64_t off = (int64_t)elapsed_ms - ((int64_t)s_seek_want_ms + since);
    if (off < 0) off = -off;
    if (off <= CORE_SEEK_SLACK_MS) { s_seek_pending = false; return false; }  /* it has landed */
    return true;
}

static void scrub_wait(uint32_t ms)
{
    lv_timer_set_period(s_scrub_rest, ms);
    lv_timer_reset(s_scrub_rest);
    lv_timer_resume(s_scrub_rest);
}

/* The music goes to where the handle is, and the scrubber stays up: one throw
 * of the hunt, not the end of it. */
static void scrub_seek(void)
{
    lv_timer_pause(s_scrub_settle);
    if (!s_scrub_moved) return;
    s_scrub_moved = false;
    s_scrub_sent = true;
    s_player.elapsed_ms = s_scrub_ms;
    wait_for_player(false);  /* the knob's clock counts from here; no stream to wait on */
    s_last_tick = lv_tick_get();
    if (wired() && s_out->seek) s_out->seek((int)(s_scrub_ms / 1000u));
    seek_asked(s_scrub_ms);
    progress();
    /* The screen stays up over the gap and for the dwell after it, so the
     * listening he does to judge the throw is not spent watching it close. */
    scrub_wait(CORE_SEEK_SOUND_MS + CORE_SCRUB_REST_MS);
}

/* The knob has no release to commit on, so the seek follows the last detent by
 * CORE_SCRUB_SETTLE_MS. */
static void scrub_settled(lv_timer_t *t)
{
    (void)t;
    scrub_seek();
}

/* `seek` is false for the ways out that are only a way out - though by then
 * the music has usually already moved, which is the point. Either way the play
 * time comes back to the slot it was tapped in, and the rim goes back to being
 * the volume. */
static void scrub_end(bool seek)
{
    if (!s_scrubbing) return;
    lv_timer_pause(s_scrub_rest);
    lv_timer_pause(s_scrub_settle);
    if (seek) scrub_seek();
    s_scrubbing = false;
    s_scrub_touch = false;
    lv_obj_add_flag(s_scrub_view, LV_OBJ_FLAG_HIDDEN);
    /* Having seeked, the clock is the handle's and not the player's: it may
     * still be answering for where the track was before the hunt. */
    if (s_scrub_sent) {
        s_player.elapsed_ms = s_scrub_ms;
        s_last_tick = lv_tick_get();
        progress();
    }
    s_scrub_moved = false;
    s_scrub_sent = false;
    play_time(true);
    rim();
}

static void scrub_rest(lv_timer_t *t)
{
    (void)t;
    scrub_end(true);
}

/* Once the music is playing from where the handle was left, the handle follows
 * it, so the screen reads as the position and not as a stale choice. Not while
 * a finger is down: the drag is measured from where it landed. */
static void scrub_follow(void)
{
    if (!s_scrubbing || s_scrub_moved || s_scrub_touch) return;
    uint32_t len = track_ms(s_player.album, s_player.track);
    uint32_t to = s_player.elapsed_ms > len ? len : s_player.elapsed_ms;
    if (to == s_scrub_ms) return;
    s_scrub_ms = to;
    scrub_paint();
}

static void scrub_show(void)
{
    uint32_t len = track_ms(s_player.album, s_player.track);
    if (!len) return;
    s_scrubbing = true;
    s_scrub_moved = false;
    s_scrub_sent = false;
    s_scrub_touch = false;
    s_scrub_ms = s_player.elapsed_ms > len ? len : s_player.elapsed_ms;
    core_minutes_label((int)(len / 1000u), s_scrub_total_text, sizeof s_scrub_total_text);
    lv_label_set_text(s_scrub_total, s_scrub_total_text);
    scrub_paint();
    lv_obj_remove_flag(s_scrub_view, LV_OBJ_FLAG_HIDDEN);
    ui_chip_off();  /* whatever was in the slot, the big time has taken over from it */
    s_time_shown = -1;
    ui_tick();
    scrub_wait(CORE_SCRUB_OPEN_MS);
}

bool np_scrubbing(void) { return s_scrubbing; }

/* ---- Drawing helpers ---------------------------------------------------------------- */
typedef struct { float x0, y0, x1, y1, x2, y2; } tri_t;
static const tri_t TRI_PLAY[] = {{0, 0, 30, 23.45f, 0, 46.9f}};
static const tri_t TRI_PREV[] = {{20.6f, 0, 20.6f, 33.8f, 0, 16.9f}, {39.1f, 0, 39.1f, 33.8f, 17.5f, 16.9f}};
static const tri_t TRI_NEXT[] = {{0, 0, 0, 33.8f, 20.6f, 16.9f}, {18.5f, 0, 18.5f, 33.8f, 39.1f, 16.9f}};

static void draw_tris(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    const tri_t *t = lv_event_get_user_data(e);
    int n = t == TRI_PLAY ? 1 : 2;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    for (int i = 0; i < n; i++) {
        lv_draw_triangle_dsc_t d;
        lv_draw_triangle_dsc_init(&d);
        d.color = lv_color_hex(UI_WHITE);
        d.p[0].x = a.x1 + t[i].x0; d.p[0].y = a.y1 + t[i].y0;
        d.p[1].x = a.x1 + t[i].x1; d.p[1].y = a.y1 + t[i].y1;
        d.p[2].x = a.x1 + t[i].x2; d.p[2].y = a.y1 + t[i].y2;
        lv_draw_triangle(lv_event_get_layer(e), &d);
    }
}

static lv_obj_t *icon(lv_obj_t *parent, int x, int y, int w, int h, const tri_t *tris)
{
    lv_obj_t *o = ui_plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_add_event_cb(o, draw_tris, LV_EVENT_DRAW_MAIN, (void *)tris);
    return o;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, int r, uint32_t col, lv_opa_t opa)
{
    lv_obj_t *o = ui_plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    return o;
}

/* ---- View ------------------------------------------------------------------------- */
void np_create(lv_obj_t *parent, const ui_library_t *lib)
{
    s_lib = lib;
    s_root = ui_plain(parent);
    lv_obj_set_size(s_root, CORE_SCREEN, CORE_SCREEN);  /* no fill: the screen behind is black */

    strip_src_t src = {.count = src_count, .title = src_title, .cover = src_cover,
                       .moved = src_moved, .settled = src_settled};
    s_strip = strip_create(s_root, &src);

    /* Loading bar, in the title band while the track list loads (Darren, 27 Oct
     * 2026): a segment sweeping along a faint track. Music Assistant sends the
     * list in one go, so there's no real progress to show, only that it's coming. */
    s_loadbar = box(s_root, (CORE_SCREEN - LOAD_W) / 2, 292, LOAD_W, 4, 2, UI_WHITE, LV_OPA_20);
    s_loadseg = box(s_loadbar, 0, 0, LOAD_SEG_W, 4, 2, UI_WHITE, LV_OPA_COVER);
    lv_obj_add_flag(s_loadbar, LV_OBJ_FLAG_HIDDEN);

    /* Frame 04: play disc (126, 107.3) r60 black 80%, triangle (178, 142) 30 x 46.9 */
    s_paused_ui = ui_plain(s_root);
    lv_obj_set_size(s_paused_ui, CORE_SCREEN, CORE_SCREEN);
    box(s_paused_ui, 126, 107, 120, 120, 60, UI_BLACK, UI_OPA_SCRIM);
    icon(s_paused_ui, 178, 142, 31, 48, TRI_PLAY);

    /* Why there is no play disc, in the space the transport band occupies.
     *
     * It was a line in the chip slot at the top first and it clipped at both
     * ends; then two lines of plain white text in the middle, which Darren
     * could barely read over artwork: "white text on a background, that is
     * almost impossible to read... effectively, I'm going to want the single
     * line black background as we've got for the volume label in the pill
     * shape" (4 Nov 2026).
     *
     * So: the volume chip's own pill, in the transport band's place. One line
     * fits here where it would not up there - at y 64 the rim leaves about
     * 242 px across, and on the centre line it leaves 336 - which is the whole
     * reason this moved rather than being reworded. */
    const lv_font_t *af = &font_inter_regular_24;
    s_amp_note = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_amp_note, af, 0);
    lv_obj_set_style_text_color(s_amp_note, lv_color_hex(UI_WHITE), 0);
    lv_obj_set_style_bg_color(s_amp_note, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_amp_note, UI_OPA_SCRIM, 0);
    lv_obj_set_style_radius(s_amp_note, UI_CHIP_H / 2, 0);
    lv_obj_set_style_pad_hor(s_amp_note, UI_CHIP_H / 2, 0);
    lv_obj_set_style_pad_top(s_amp_note, (UI_CHIP_H - lv_font_get_line_height(af)) / 2, 0);
    lv_obj_set_height(s_amp_note, UI_CHIP_H);
    lv_label_set_text(s_amp_note, "Connecting to the amp");
    lv_obj_align(s_amp_note, LV_ALIGN_CENTER, 0, 0);
    set_opa(s_amp_note, LV_OPA_TRANSP);

    /* Frame 05: transport band (33, 138) 294 x 85 r42.5; previous, pause, next; progress */
    s_transport = ui_plain(s_root);
    lv_obj_set_size(s_transport, CORE_SCREEN, CORE_SCREEN);
    set_opa(s_transport, LV_OPA_TRANSP);
    box(s_transport, 33, 138, 294, 85, 42, UI_BLACK, UI_OPA_SCRIM);
    icon(s_transport, 69, 160, 40, 34, TRI_PREV);   /* Figma: 68.9, 159.6 */
    icon(s_transport, 252, 160, 40, 34, TRI_NEXT);
    box(s_transport, 160, 151, 13, 51, 0, UI_WHITE, LV_OPA_COVER);  /* pause bars: 160.4, 151, 13 x 50.8 */
    box(s_transport, 186, 151, 13, 51, 0, UI_WHITE, LV_OPA_COVER);
    lv_obj_t *bar = box(s_transport, 69, 214, 222, 3, 2, PROGRESS_TRACK, LV_OPA_COVER);  /* 69, 213.8, 222.1 x 3.4 */
    s_fill = box(bar, 0, 0, 0, 3, 2, PROGRESS_FILL, LV_OPA_COVER);

    /* The scrubber (not in the design): on black, the time in the clock's own
     * 96px extralight, the progress bar again at four times the height with a
     * handle on it so it reads as something you can drag, and the track's
     * length under its right-hand end. */
    s_scrub_view = ui_plain(s_root);
    lv_obj_set_size(s_scrub_view, CORE_SCREEN, CORE_SCREEN);
    lv_obj_set_style_bg_color(s_scrub_view, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(s_scrub_view, LV_OPA_COVER, 0);
    s_scrub_time = lv_label_create(s_scrub_view);
    lv_obj_set_style_text_font(s_scrub_time, &font_inter_extralight_96, 0);
    lv_obj_set_style_text_color(s_scrub_time, lv_color_hex(UI_WHITE), 0);
    lv_label_set_text(s_scrub_time, "00:00");
    lv_obj_align(s_scrub_time, LV_ALIGN_TOP_MID, 0, SCRUB_TIME_Y);
    lv_obj_t *sbar = box(s_scrub_view, SCRUB_BAR_X, SCRUB_BAR_Y, SCRUB_BAR_W, SCRUB_BAR_H,
                         SCRUB_BAR_H / 2, PROGRESS_TRACK, LV_OPA_COVER);
    s_scrub_fill = box(sbar, 0, 0, 1, SCRUB_BAR_H, SCRUB_BAR_H / 2, PROGRESS_FILL, LV_OPA_COVER);
    s_scrub_handle = box(s_scrub_view, SCRUB_BAR_X - SCRUB_HANDLE / 2,
                         SCRUB_BAR_Y + (SCRUB_BAR_H - SCRUB_HANDLE) / 2,
                         SCRUB_HANDLE, SCRUB_HANDLE, SCRUB_HANDLE / 2, UI_WHITE, LV_OPA_COVER);
    s_scrub_total = lv_label_create(s_scrub_view);
    lv_obj_set_style_text_font(s_scrub_total, &font_inter_regular_20, 0);
    lv_obj_set_style_text_color(s_scrub_total, lv_color_hex(UI_GREY), 0);
    lv_label_set_text(s_scrub_total, "00:00");
    lv_obj_align(s_scrub_total, LV_ALIGN_TOP_RIGHT, -SCRUB_BAR_X, SCRUB_TOTAL_Y);
    lv_obj_add_flag(s_scrub_view, LV_OBJ_FLAG_HIDDEN);
    s_scrub_rest = lv_timer_create(scrub_rest, CORE_SCRUB_REST_MS, NULL);
    lv_timer_set_auto_delete(s_scrub_rest, false);
    lv_timer_pause(s_scrub_rest);
    s_scrub_settle = lv_timer_create(scrub_settled, CORE_SCRUB_SETTLE_MS, NULL);
    lv_timer_set_auto_delete(s_scrub_settle, false);
    lv_timer_pause(s_scrub_settle);

    s_play_timer = lv_timer_create(play_tick, 100, NULL);
    s_peek_timer = lv_timer_create(peek_end, 2000, NULL);
    lv_timer_pause(s_peek_timer);
    s_unborrow_timer = lv_timer_create(unborrow, CORE_TRACK_MARK_HOLD_MS, NULL);
    lv_timer_pause(s_unborrow_timer);
}

lv_obj_t *np_root(void) { return s_root; }

/* The track to start on: the playing one, or one asked for by search. */
static float start_track(int album)
{
    if (s_start >= 0) {
        float k = (float)s_start;
        s_start = -1;
        return k;
    }
    if (s_want_track[0] && s_tracks) {
        for (int i = 0; i < s_tracks->count; i++)
            if (!strcmp(s_tracks->title[i], s_want_track)) {
                s_want_track[0] = 0;
                return (float)i;
            }
        s_want_track[0] = 0;
    }
    return s_player.album == album ? (float)s_player.track : 0.0f;
}

void np_open_track(int album, const char *track)
{
    snprintf(s_want_track, sizeof s_want_track, "%s", track ? track : "");
    np_open(album);
    s_back = UI_VIEW_SEARCH;
}

/* Recent: the list as it is now, opened on its item `at`. */
void np_open_recent(int at)
{
    int n = s_lib->recent ? s_lib->recent() : 0;
    if (n > MIX_MAX) n = MIX_MAX;
    /* playing from Recent already: remember which, to find it in the new list
     * (playing moves a track to the top) */
    bool mixing = is_mix(s_player.album) && s_player.track < s_mix.t.count;
    int was_coll = mixing ? s_mix.coll[s_player.track] : -1;
    char was[96] = "";
    if (mixing) snprintf(was, sizeof was, "%s", s_mix.title[s_player.track]);
    for (int i = 0; i < n; i++) {
        const char *track;
        s_mix.coll[i] = s_lib->recent_item(i, &track);
        snprintf(s_mix.title[i], sizeof s_mix.title[i], "%s", track ? track : "");
        s_mix.titles[i] = s_mix.title[i];
        s_mix.index[i] = -1;
        s_mix.seconds[i] = 0;
        if (mixing && s_mix.coll[i] == was_coll && !strcmp(s_mix.title[i], was)) s_player.track = i;
    }
    s_mix.t = (ui_tracks_t){.count = n, .title = s_mix.titles, .seconds = s_mix.seconds};
    for (int i = 0; i < n; i++) mix_resolve(i);
    s_start = at;
    np_open(NP_RECENT);
    s_back = UI_VIEW_RECENT;
}

void np_open_from_playlists(int playlist)
{
    np_open(UI_PLAYLIST(playlist));
    s_back = UI_VIEW_PLAYLISTS;
}

void np_open_from_albums(int album)
{
    np_open(album);
    s_back = UI_VIEW_ALBUMS;
}

static void seg_x(void *obj, int32_t v) { lv_obj_set_x(obj, v); }

/* The loading bar shows while the album's track list is on its way. */
static void loading_bar(void)
{
    bool show = !s_tracks && s_album >= 0;
    if (show != lv_obj_has_flag(s_loadbar, LV_OBJ_FLAG_HIDDEN)) return;  /* already so */
    if (!show) {
        lv_anim_delete(s_loadseg, seg_x);
        lv_obj_add_flag(s_loadbar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_loadbar, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_loadseg);
    lv_anim_set_exec_cb(&a, seg_x);
    lv_anim_set_values(&a, 0, LOAD_W - LOAD_SEG_W);
    lv_anim_set_duration(&a, LOAD_SWEEP_MS);
    lv_anim_set_reverse_duration(&a, LOAD_SWEEP_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void np_open(int album)
{
    s_album = album;
    s_tracks = tracks_of(album);  /* NULL until loaded */
    if (!UI_IS_PLAYLIST(album) && !is_mix(album)) s_lib->focus((float)album, 0.0f);  /* keep this album's cover loaded */
    loading_bar();
    strip_refresh(s_strip, -1);
    strip_jump(s_strip, start_track(album));
    if (is_mix(album)) mix_focus(strip_pos(s_strip));
    else if (UI_IS_PLAYLIST(album) && s_lib->track_focus) s_lib->track_focus(album, strip_pos(s_strip));
    /* the play disc fades in over the cover (prototype .np-disc: opacity .28s) */
    if (!playing_here()) set_opa(s_paused_ui, LV_OPA_TRANSP);
    ui_show(UI_VIEW_NOWPLAYING);
    band_exec(NULL, playing_here() ? LV_OPA_TRANSP : LV_OPA_COVER);
    update();
}

void ui_goto_playlist(int p) { np_open(UI_PLAYLIST(p)); }

/* The play disc, set without a fade: on the way into the screen there is
 * nothing to animate from. np_enter did not touch it at all, so the disc kept
 * whatever opacity the last update left - which meant arriving on Now Playing
 * while the amp was still waking showed a play button after all, the exact
 * thing being fixed (found on the bench, 4 Nov 2026). */
static void amp_gate(void)
{
    if (playing_here()) return;  /* update() owns it while something is playing */
    set_opa(s_paused_ui, can_play() ? LV_OPA_COVER : LV_OPA_TRANSP);
    set_opa(s_amp_note, can_play() ? LV_OPA_TRANSP : LV_OPA_COVER);
}

void np_enter(void) { scrub_end(false); rim(); play_time(true); amp_gate(); }

/* Bench: show the amp note without an amp that is actually down, so it can be
 * looked at on the glass rather than only when something is broken. */
void ui_test_amp_note(bool on) { set_opa(s_amp_note, on ? LV_OPA_COVER : LV_OPA_TRANSP); }

/* Bench: open the scrubber, which otherwise needs a finger on the play time.
 * Without this a scrub can only be reproduced by asking Darren to do it, and
 * the seek it sends is the one thing worth watching here. */
void ui_test_scrub_open(void) { if (playing_here()) scrub_show(); }

void ui_tracks_changed(int album)
{
    if (is_mix(s_album) && mix_has(album)) {  /* a recent track's album or playlist: now it can be found */
        for (int i = 0; i < s_mix.t.count; i++)
            if (s_mix.coll[i] == album) mix_resolve(i);
        strip_refresh(s_strip, -1);
        mix_focus(strip_pos(s_strip));
        return;
    }
    if (album != s_album || s_tracks) return;
    s_tracks = s_lib->tracks(album);
    loading_bar();
    strip_refresh(s_strip, -1);
    strip_jump(s_strip, start_track(album));
    rim();
}

void np_cover_changed(int album)
{
    if (album != s_album && !(is_mix(s_album) && mix_has(album))) return;
    loading_bar();
    strip_refresh(s_strip, -1);
}

void ui_track_cover_changed(int playlist, int track)
{
    if (is_mix(s_album)) {
        for (int i = 0; i < s_mix.t.count; i++)
            if (s_mix.coll[i] == playlist && s_mix.index[i] == track) strip_refresh(s_strip, i);
        eq_cover_changed(NP_RECENT);
        return;
    }
    if (playlist != s_album) return;
    strip_refresh(s_strip, track);
    eq_cover_changed(playlist);
}

/* ---- Hold to skip ------------------------------------------------------------------ */
static lv_timer_t *s_hold_timer;
static int s_hold_dir;      /* -1 back, +1 forward, 0 not holding a skip third */
static bool s_held;         /* the hold has started skipping: the release isn't a tap */

static void hold_stop(void)
{
    s_hold_dir = 0;
    if (s_hold_timer) lv_timer_pause(s_hold_timer);
}

static void hold_step(lv_timer_t *t)
{
    if (!s_hold_dir || !playing_here()) { hold_stop(); return; }
    lv_timer_set_period(t, CORE_SKIP_REPEAT_MS);
    bool run = lv_tick_elaps(s_skip_at) < CORE_SKIP_RUN_MS && s_skip_dir == s_hold_dir;
    int track = s_player.track;
    s_held = true;
    ui_tick();
    skip(s_hold_dir * core_skip_hold_step(run ? s_skip_total : 0));
    if (s_player.track != track) hold_stop();                      /* past the end: on the next track, stop there */
    else if (s_hold_dir < 0 && s_player.elapsed_ms == 0) hold_stop();  /* back at the start: nothing further */
}

/* Back or forward pressed while playing: a hold starts skipping after CORE_SKIP_HOLD_MS. */
static void hold_start(int x, int y)
{
    s_held = false;
    s_hold_dir = 0;
    if (!playing_here() || x < 33 || x > 327 || y < 138 || y > 223 || (x >= 131 && x <= 229)) return;
    s_hold_dir = x < 131 ? -1 : 1;
    if (!s_hold_timer) {
        s_hold_timer = lv_timer_create(hold_step, CORE_SKIP_HOLD_MS, NULL);
        lv_timer_set_auto_delete(s_hold_timer, false);
    }
    lv_timer_set_period(s_hold_timer, CORE_SKIP_HOLD_MS);
    lv_timer_set_repeat_count(s_hold_timer, -1);
    lv_timer_reset(s_hold_timer);
    lv_timer_resume(s_hold_timer);
}

void np_press(int x, int y)
{
    if (s_scrubbing) {
        s_scrub_press = s_scrub_ms;
        s_scrub_touch = true;
        /* A finger on the glass: nothing commits and nothing closes while it
         * is down. The release is the commit. */
        lv_timer_pause(s_scrub_rest);
        lv_timer_pause(s_scrub_settle);
        return;
    }
    /* Only note where the touch started. A drag that begins on the play time
     * is just a drag - a swipe down to leave the screen naturally starts up
     * there - and only a tap opens the scrubber (the lesson the letter badge
     * cost in Albums, 29 Oct 2026). */
    s_time_tap = on_play_time(x, y);
    strip_stop(s_strip);
    s_press_pos = strip_pos(s_strip);
    hold_start(x, y);
}

void np_drag(float dx, float dy)
{
    if (s_scrubbing) {
        /* A slider: the bar's width is the whole track, so the time follows
         * the finger at the rate it would if the finger were on the bar. */
        if (fabsf(dx) > fabsf(dy))
            scrub_move((int64_t)s_scrub_press +
                       (int64_t)(dx * (float)track_ms(s_player.album, s_player.track) / SCRUB_BAR_W));
        return;
    }
    if (s_hold_dir && !s_held && core_is_drag(dx, dy)) hold_stop();  /* a swipe, not a hold */
    if (s_held) return;  /* holding a skip: the finger isn't moving tracks */
    if (ui_touch_left_edge_pending()) return;  /* leaving the screen, not picking a track */
    /* ...and the same on the right, while music is playing: that swipe is the
     * Music off shortcut, so the tracks must not shuffle under it. */
    if (s_player.playing && ui_touch_right_edge_pending()) return;
    if (fabsf(dx) > fabsf(dy) && ntracks()) {
        if (playing_here()) borrow();
        strip_drag(s_strip, s_press_pos - dx / CORE_SCREEN);
        rim();
    }
}

void np_release(int x, int y, float dx, float dy, bool moved)
{
    if (s_scrubbing) {
        s_scrub_touch = false;
        /* Out, by either of the two ways out of every screen: the swipe down,
         * and the one in over the left bezel. Nothing further is sent - though
         * the throws already made have been. */
        if (ui_touch_back_gesture(dx, dy, moved) ||
            (moved && fabsf(dy) > fabsf(dx) && fabsf(dy) > CORE_SWIPE_V_PX)) {
            scrub_end(false);
            return;
        }
        if (!moved) { scrub_end(true); return; }  /* a tap: go there and have done */
        /* The hand let go: the music moves now, and the screen stays up for
         * the next throw - scrub_seek sets how long for, counting from when the
         * sound is due rather than from now. */
        if (s_scrub_moved) scrub_seek();
        else scrub_wait(CORE_SCRUB_REST_MS);
        return;
    }
    bool time_tap = s_time_tap;
    s_time_tap = false;
    bool held = s_held;
    hold_stop();
    s_held = false;
    if (held) return;  /* the hold did the skipping */
    /* The second way back, asked before anything horizontal, because the strip
     * has been dragging under the finger all the way across: see
     * ui_touch_back_gesture in ui.h. Put the strip back where it was first,
     * so leaving a screen never also changes the track. */
    if (ui_touch_back_gesture(dx, dy, moved)) {
        strip_snap(s_strip, (int)lroundf(s_press_pos), CORE_TRACK_SLIDE_MS);
        ui_show(s_back);
        return;
    }
    /* In over the right bezel, with music playing: straight to the Music off
     * question, instead of back out through the menu to find it (Darren,
     * 7 Nov 2026). Asked here, next to the way back, and with the strip put
     * back first for the same reason: leaving never also changes the track.
     * The question itself is the menu's, countdown and all, so nothing goes
     * off on the strength of one swipe. */
    if (s_player.playing && ui_touch_off_gesture(dx, dy, moved)) {
        strip_snap(s_strip, (int)lroundf(s_press_pos), CORE_TRACK_SLIDE_MS);
        ui_ask_music_off();
        return;
    }
    if (!moved) {
        if (time_tap) { scrub_show(); return; }  /* the play time: into the scrubber */
        strip_snap(s_strip, (int)lroundf(s_press_pos), CORE_TRACK_SLIDE_MS);
        if (!ntracks()) return;
        if (!playing_here()) { if (can_play()) play(); return; }
        /* while playing, the transport band (33, 138, 294 x 85) splits into thirds */
        if (x < 33 || x > 327 || y < 138 || y > 223) return;
        if (x < 131) skip(-CORE_SKIP_TAP_S);
        else if (x > 229) skip(CORE_SKIP_TAP_S);
        else pause_playback();
        return;
    }
    if (fabsf(dx) > fabsf(dy)) {
        int k = (int)lroundf(s_press_pos);
        if (fabsf(dx) > CORE_SWIPE_H_PX) k += dx < 0 ? 1 : -1;
        strip_snap(s_strip, k, CORE_TRACK_SLIDE_MS);
        return;
    }
    strip_snap(s_strip, (int)lroundf(s_press_pos), CORE_TRACK_SLIDE_MS);
    if (fabsf(dy) > CORE_SWIPE_V_PX && dy > 0) ui_show(s_back);  /* drag down: back where it came from */
    if (fabsf(dy) > CORE_SWIPE_V_PX && dy < 0) eq_open(s_album);         /* drag up: EQ */
}

void np_knob(int steps)
{
    if (s_scrubbing) {
        scrub_move((int64_t)s_scrub_ms + (int64_t)steps * CORE_SKIP_TAP_S * 1000);
        ui_tick();
        lv_timer_reset(s_scrub_settle);  /* ...and the seek once the turning stops */
        lv_timer_resume(s_scrub_settle);
        scrub_wait(CORE_SCRUB_REST_MS);
        return;
    }
    if (!playing_here()) {
        /* picking a track */
        if (ntracks()) strip_snap(s_strip, strip_target(s_strip) + steps, CORE_TRACK_SLIDE_MS);
        return;
    }
    np_volume_step(steps);
}

/* ---- Volume limit (PLANNING.md "Volume limit") ---------------------------------- */
static bool s_louder;  /* allowed past CORE_VOLUME_LIMIT until the music is next switched off */
static lv_timer_t *s_relock_timer;  /* ...or until it has sat back under the limit for a while */

/* The lift is lent, not given (Darren, 2 Nov 2026). Past the limit it stands:
 * the amp is already there and the next click up has nothing to ask about.
 * Back at the limit or below it is on a clock, and CORE_VOLUME_RELOCK_MS of
 * sitting there ends it, so a yes given once doesn't answer for the whole
 * evening. Every move of the volume comes through here, so easing off and
 * going back up inside the half minute is one movement with no question in
 * the middle of it. */
static void relock(lv_timer_t *t)
{
    lv_timer_pause(t);
    s_louder = false;
}

static void louder_watch(void)
{
    if (!s_louder || s_player.volume > CORE_VOLUME_LIMIT) {
        if (s_relock_timer) lv_timer_pause(s_relock_timer);
        return;
    }
    if (!s_relock_timer) {
        s_relock_timer = lv_timer_create(relock, CORE_VOLUME_RELOCK_MS, NULL);
        lv_timer_set_auto_delete(s_relock_timer, false);
    }
    lv_timer_reset(s_relock_timer);
    lv_timer_resume(s_relock_timer);
}

/* Saying yes only unlocks the limit: it never moves the volume itself. The
 * answer used to carry a click of its own, so a tap stepped to 82 - a jump the
 * speakers never asked for and the hand didn't make (Darren, 30 Oct 2026).
 * Turning the knob is the only thing that makes it louder; this just shows
 * where the volume still is. */
static void louder_yes(void)
{
    s_louder = true;
    np_volume_step(0);
}

/* One answer, no Cancel: the knob stays the volume (pinned at the limit) while
 * it's up, so a hand already turning can't answer it (Darren, 30 Oct 2026). */
static const ui_ask_t LOUDER = {.title = "Louder than 80%?",
                                .button = "Louder",
                                .hint = "Keep turning and it stays at 80",
                                .yes = louder_yes};

/* Music off: the limit comes back, and the volume goes back to where the music
 * starts, so the next listen starts quietly. */
void np_volume_relock(void)
{
    s_louder = false;
    if (s_relock_timer) lv_timer_pause(s_relock_timer);
    s_player.volume = CORE_VOLUME_START;
    s_arc_shown = CORE_VOLUME_START * 10;
    /* Nothing is sent to the amp: Darren's own fade walks it down and finishes
     * at this very level, and setting it here would drop the volume in one
     * step first - the opposite of a fade. */
}

void np_volume_step(int steps)
{
    if (s_borrowed) unborrow(NULL);  /* turning the knob takes the rim straight back */
    /* Turning back down answers "Louder than 80%?" by itself (Darren, 31 Oct
     * 2026): the hand has retreated, so the question has no subject left and
     * shouldn't sit over the volume it is now in the way of. */
    if (steps < 0 && ui_ask_is(&LOUDER)) ui_ask_cancel();
    int dir = steps < 0 ? -1 : 1, n = steps < 0 ? -steps : steps;
    int v = s_player.volume;
    bool asked = false;
    for (int i = 0; i < n; i++) {
        /* One per click, the whole way (Darren, 5 Nov 2026). It used to be two
         * below the limit and one above, to keep the long climb from quiet
         * quick. There is no climb from quiet any more - the dial starts at
         * CORE_VOLUME_FLOOR and cannot go under it - so the fifty clicks from
         * the floor to the limit are all usable level, and finer is simply
         * better. */
        int next = v + dir;
        if (!s_louder && next > CORE_VOLUME_LIMIT) {
            if (v >= CORE_VOLUME_LIMIT) {  /* at the limit and turning on up: ask */
                if (!ui_asking()) ui_ask_one(&LOUDER);
                asked = true;
                break;
            }
            next = CORE_VOLUME_LIMIT;  /* stop at the limit */
        }
        /* The floor is the bottom of the knob: turning down stops there rather
         * than walking into the inaudible. A volume already under it (Home
         * Assistant's slider can set one) is left where it is on the way down
         * and climbs out of it on the way up. */
        if (next < CORE_VOLUME_FLOOR && next < v) next = v < CORE_VOLUME_FLOOR ? v : CORE_VOLUME_FLOOR;
        v = next < 0 ? 0 : next > 100 ? 100 : next;
    }
    if (asked && v == s_player.volume) return;  /* the question is the whole answer to that click */
    s_player.volume = v;
    if (s_out && s_out->volume) s_out->volume(v);
    /* While the amp is deaf a number is a lie: the turn is real and is being
     * held, but nothing has reached the amp yet, so say that instead. */
    char buf[16];
    /* Short on purpose: this slot is a pill about 300 px wide and a sentence
     * does not fit in it - "Connecting to the amp" clipped at both ends there
     * (Darren, 4 Nov 2026). Now Playing says the long version in the middle of
     * the screen; this is only the answer to "why did the number not move?" */
    if (ui_amp() == UI_AMP_CONNECTING) {
        ui_chip("Amp not ready");
    } else {
        snprintf(buf, sizeof buf, "%d", s_player.volume);
        ui_chip_volume(buf);
    }
    /* animated in tenths of a percent so the 100 ms move is smooth */
    ui_anim(&s_arc_shown, arc_value, s_arc_shown, s_player.volume * 10, CORE_ARC_MS, 0, 0, 1, 1);
    louder_watch();
}

void np_volume_rim(void) { volume_arc(arc_frac(s_arc_shown)); }

/* Something other than the knob moved the volume (ha.c reads the helper back).
 * Two faults turned out to be the same missing read (Darren, 1-2 Nov 2026):
 * the knob opened at CORE_VOLUME_START over an amp sitting at 71, so what it
 * showed was wrong and the music was far louder than the arc said; and moving
 * the dashboard slider left the arc where it was, so the sync only ever ran
 * from the dial to the slider and never back.
 *
 * Nothing is sent to the amp - it is already there. The label is shown, the
 * same as a turn: the number is the point when the dial and the dashboard are
 * being read side by side. Past the 80% limit the limit counts as already
 * lifted, because the amp is past it and the next click up has nothing left to
 * ask about. */
void ui_volume_set(int percent)
{
    int v = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (v == s_player.volume) return;
    if (v > CORE_VOLUME_LIMIT) s_louder = true;
    s_player.volume = v;
    louder_watch();  /* the slider retreating under the limit starts the clock too */
    /* Off those two screens this is bookkeeping and nothing else: no arc, no
     * label, nothing that says the volume to a screen that isn't about it. */
    if (!volume_arc_is_up()) {
        lv_anim_delete(&s_arc_shown, arc_value);  /* an animation would overwrite it */
        s_arc_shown = v * 10;
        return;
    }
    char buf[16];
    /* Short on purpose: this slot is a pill about 300 px wide and a sentence
     * does not fit in it - "Connecting to the amp" clipped at both ends there
     * (Darren, 4 Nov 2026). Now Playing says the long version in the middle of
     * the screen; this is only the answer to "why did the number not move?" */
    if (ui_amp() == UI_AMP_CONNECTING) {
        ui_chip("Amp not ready");
    } else {
        snprintf(buf, sizeof buf, "%d", v);
        ui_chip_volume(buf);
    }
    ui_anim(&s_arc_shown, arc_value, s_arc_shown, v * 10, CORE_ARC_MS, 0, 0, 1, 1);
}

void np_eq_changed(int param, int db)
{
    if (s_out && s_out->eq) s_out->eq(param, db);
}

/* ---- What the player is really doing ---------------------------------------------
 * The knob keeps its own clock so the progress bar moves every frame; this is
 * what puts it right, and what notices the player moving on to the next track
 * at the end of one. Recent is the knob's own list and steps itself, so a move
 * reported against it is the album the track lives in, not the list, and is
 * left alone. */
void ui_player_state(bool playing, int advanced, uint32_t elapsed_ms)
{
    if (s_player.album < 0) return;
    /* A track swiped to is not playing yet, and the player is still answering
     * for the one before it: taking its word for the track or the position
     * would snap the strip back under the finger. */
    if (s_settle_timer && lv_timer_get_paused(s_settle_timer) == false) return;
    if (advanced > 0 && !is_mix(s_player.album)) {
        const ui_tracks_t *t = tracks_of(s_player.album);
        int want = s_player.asked + advanced;
        if (t && want >= t->count) want = t->count - 1;
        if (want >= 0 && want != s_player.track) {
            s_player.track = want;
            if (playing_here()) {
                strip_snap(s_strip, want, CORE_TRACK_SLIDE_MS);
                peek();
            }
        }
    }
    if (playing) wait_for_player(false);  /* it really is playing now: the clock is ours again */
    /* While the hunt is on the position is the handle's, and for a moment after
     * a seek it is the seek's: either way the player is still answering for
     * where the track was before it, and taking its word would drag the clock
     * back over a move that has already happened. */
    if (!s_scrubbing && !seek_stale(elapsed_ms)) {
        s_player.elapsed_ms = elapsed_ms;
        s_last_tick = lv_tick_get();
    }
    if (s_player.playing != playing) {
        s_player.playing = playing;
        if (!playing) lv_timer_pause(s_peek_timer);
        update();
    } else if (playing_here()) {
        progress();
        play_time(false);
    }
}
