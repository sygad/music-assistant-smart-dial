/*
 * The rim and the arc, painted into each finished block of pixels just before
 * it goes to the panel.
 *
 * In the design both always sit on top of everything else (prototype z-index
 * 90), so they can be applied after LVGL has drawn the frame. Drawn as LVGL
 * objects, the rim's circular mask was recomputed over the whole screen every
 * frame and cost about 40% of the frame rate while sliding covers. Here they
 * come from precomputed tables and touch only the ~14,000 pixels of the rim.
 *
 * Geometry (DESIGN-TOKENS.md): rim opaque black from radius 168 outwards, the
 * arc 8px wide on radius 170-178 with round caps, from 6 o'clock clockwise.
 */
#include <math.h>
#include <string.h>

#include "ui_internal.h"

#define C           (CORE_SCREEN / 2.0f)
#define RIM_R       168.0f
#define ARC_MID     174.0f
#define ARC_HALF    4.0f
#define ARC_TABLE_R0 168
#define ARC_TABLE_R1 180

/* Per row: rim pixels on the left are [0, solid) fully black, [solid, clear)
 * anti-aliased with the alphas in rim_aa; mirrored on the right. */
#define RIM_AA_MAX 64  /* near the top and bottom the inner edge runs almost along a row */

typedef struct {
    int16_t solid, clear;
    uint8_t aa[RIM_AA_MAX];
} rim_row_t;

/* Per pixel in the arc's annulus: angle from 6 o'clock clockwise (0..65535)
 * and radial coverage of the 8px stroke. */
typedef struct {
    int16_t x, y;
    uint16_t angle;
    uint8_t radial;
} arc_px_t;

/* The same pixels indexed by angle, in BUCKETS slices around the circle, so a
 * short arc (the rim marker, ~1% of a turn) only visits its own pixels. Going
 * row by row through all ~11,000 cost ~4.5 ms a frame for the marker. */
#define BUCKETS 256
static uint16_t *s_by_angle;               /* indices into s_arc_px */
static uint32_t s_bucket_start[BUCKETS + 1];

/* In PSRAM (6 Nov 2026): 24 KB, the largest single thing this firmware kept
 * in internal RAM. Each flushed row reads its own 68 bytes once, which the
 * cache serves; the measured cost is in docs/MEMORY.md. */
UI_IN_PSRAM static rim_row_t s_rim[CORE_SCREEN];
static arc_px_t *s_arc_px;
static uint32_t s_arc_row_start[CORE_SCREEN + 1];
static bool s_inited, s_rim_on;
static bool s_arc_on;
static float s_arc_from, s_arc_frac;          /* the arc runs from s_arc_from to s_arc_frac (fractions of a turn) */
static float s_painted_from, s_painted_to;    /* what was last actually painted on screen */
static lv_color_t s_arc_color;
/* Scales the arc's coverage as it is painted, so the same arc can be drawn
 * faint or solid. Only the spinner uses it; every other arc resets it. */
static uint8_t s_arc_bright = 255;
/* Two-tone arc: past s_arc_split (a fraction of a turn) the arc is drawn in
 * s_arc_color2 instead, which is how the volume goes red past the limit.
 * Negative: one colour all the way. */
static float s_arc_split = -1.0f;
static lv_color_t s_arc_color2;
static lv_obj_t *s_invalidator;

/* ---- The spinning arc: the rim's third meaning (Darren, 4 Nov 2026) ------------
 * A solid arc is a level - the volume, a tone control. A short arc is a
 * fraction - which album in the library, which track in the record. Neither
 * fits "working on it, and I cannot tell you how far along", which is what
 * joining a network or fetching an album actually is, so there is now a third
 * idiom: a short arc that goes round and round, and breathes longer and
 * shorter as it goes so it reads as motion rather than as a marker that has
 * got loose.
 *
 * It is here rather than in the screen that wants it because the rim is drawn
 * here, and because anything on the dial can now ask for it. */
#define SPIN_MS       33     /* ~30 a second: smooth enough on a rim this thin */
#define SPIN_TURN_MS  1100   /* one time round, at full speed */
#define SPIN_BREATH_MS 1700  /* and the length cycling, deliberately not in step */
#define SPIN_MIN      0.07f  /* shortest, as a fraction of a turn */
#define SPIN_MAX      0.34f
/* Armed, not owning. The spinner is what the rim shows when nothing else is
 * showing anything: a view that wants its own arc takes it and the spinner
 * steps aside, and when that arc goes the spinner comes back. Darren, 4 Nov
 * 2026: going through an album the blue circle "will temporarily be replaced
 * by the white segmented arc to show the track list", and come back after.
 * It owned the rim outright to begin with, which would have blanked the track
 * marker for the whole two minutes an amp takes to wake. */
static bool s_spin_armed;
static bool s_spin_on;      /* ...and actually running at this moment */
static bool s_spin_held;    /* stopped where it is and drawn by its owner: see ui_overlay_spin_hold */
static bool s_in_spin;      /* the spinner's own call, which is allowed through */
static bool s_view_arc;     /* a view is drawing its own arc */
static lv_timer_t *s_spin_timer;
static uint32_t s_spin_color;
/* Where it is in its turn and in its breath, carried forward step by step
 * rather than worked out from a start time. Speed can then change without the
 * arc jumping to a different part of the circle (Darren, 4 Nov 2026: it ramps
 * up as the dial connects, and drops back once the menu has his attention). */
static float s_spin_head, s_spin_breath;
static float s_spin_speed = 1.0f;
static uint8_t s_spin_bright = 255;

static float cover(float d) { return d <= -0.5f ? 0.0f : d >= 0.5f ? 1.0f : d + 0.5f; }

static void build_tables(void)
{
    for (int y = 0; y < CORE_SCREEN; y++) {
        float dy = y + 0.5f - C;
        rim_row_t *r = &s_rim[y];
        r->solid = (int16_t)(C);
        r->clear = (int16_t)(C);
        int n = 0;
        for (int x = 0; x < CORE_SCREEN / 2; x++) {
            float dx = x + 0.5f - C;
            float a = cover(sqrtf(dx * dx + dy * dy) - RIM_R);
            if (a >= 1.0f) continue;
            if (r->solid == (int16_t)C) r->solid = (int16_t)x;
            if (a <= 0.0f) { r->clear = (int16_t)x; break; }
            if (n < RIM_AA_MAX) r->aa[n++] = (uint8_t)(a * 255.0f + 0.5f);
        }
        if (r->clear == (int16_t)C && r->solid < (int16_t)C) r->clear = (int16_t)(r->solid + n);
    }

    /* count, then fill, the arc's pixels row by row */
    uint32_t count = 0;
    for (int pass = 0; pass < 2; pass++) {
        count = 0;
        for (int y = 0; y < CORE_SCREEN; y++) {
            s_arc_row_start[y] = count;
            float dy = y + 0.5f - C;
            for (int x = 0; x < CORE_SCREEN; x++) {
                float dx = x + 0.5f - C;
                float r = sqrtf(dx * dx + dy * dy);
                if (r < ARC_TABLE_R0 || r > ARC_TABLE_R1) continue;
                if (pass == 1) {
                    /* 6 o'clock is +y on screen; clockwise on screen runs towards -x */
                    float ang = atan2f(-dx, dy);
                    if (ang < 0) ang += 6.2831853f;
                    s_arc_px[count] = (arc_px_t){
                        .x = (int16_t)x,
                        .y = (int16_t)y,
                        .angle = (uint16_t)(ang / 6.2831853f * 65535.0f),
                        .radial = (uint8_t)(cover(ARC_HALF - fabsf(r - ARC_MID)) * 255.0f + 0.5f),
                    };
                }
                count++;
            }
        }
        s_arc_row_start[CORE_SCREEN] = count;
        if (pass == 0) s_arc_px = lv_malloc(count * sizeof(arc_px_t));
    }
    /* counting sort by angle bucket */
    s_by_angle = lv_malloc(count * sizeof(uint16_t));
    uint32_t fill[BUCKETS] = {0};
    for (uint32_t i = 0; i < count; i++) s_bucket_start[(s_arc_px[i].angle >> 8) + 1]++;
    for (int b = 0; b < BUCKETS; b++) s_bucket_start[b + 1] += s_bucket_start[b];
    for (uint32_t i = 0; i < count; i++) {
        int b = s_arc_px[i].angle >> 8;
        s_by_angle[s_bucket_start[b] + fill[b]++] = (uint16_t)i;
    }
}

void ui_overlay_init(lv_obj_t *parent)
{
    if (!s_inited) build_tables();
    s_inited = true;
    /* An empty full-screen object, used only to tell LVGL which areas to redraw. */
    s_invalidator = ui_plain(parent);
    lv_obj_set_size(s_invalidator, CORE_SCREEN, CORE_SCREEN);
}

/* ---- Pixel helpers for the panel's byte-swapped RGB565 ------------------------ */
static inline uint16_t unswap(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static inline uint16_t mix565(uint16_t fg, uint16_t bg, uint8_t a)
{
    uint32_t r = ((fg >> 11) * a + (bg >> 11) * (255 - a)) / 255;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255 - a)) / 255;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline void blend(uint16_t *p, uint16_t fg, uint8_t a)
{
    if (a == 255) *p = unswap(fg);
    else if (a) *p = unswap(mix565(fg, unswap(*p), a));
}

/* Coverage of the arc at a pixel, including the round caps at both ends.
 *
 * The span may run the other way round the circle - start past end - which is
 * what a spinning arc does once a turn as it crosses 6 o'clock. Taken at face
 * value such a span matched no pixel at all and only the two caps appeared
 * (4 Nov 2026). One extra comparison, and the ordinary case pays nothing it
 * can measure. */
static bool s_arc_full;  /* the arc is a whole turn: a closed ring, no ends */

static uint8_t arc_alpha(const arc_px_t *p, int y, uint16_t start_angle, uint16_t end_angle, float sx, float sy,
                         float ex, float ey)
{
    if (s_arc_full) return p->radial;
    bool inside = start_angle <= end_angle ? (p->angle >= start_angle && p->angle <= end_angle)
                                           : (p->angle >= start_angle || p->angle <= end_angle);
    uint8_t a = inside ? p->radial : 0;
    if (a == 255) return a;
    float px = p->x + 0.5f, py = y + 0.5f;
    /* caps centred on the stroke's centre line at each end */
    float q0 = (px - sx) * (px - sx) + (py - sy) * (py - sy);
    float q1 = (px - ex) * (px - ex) + (py - ey) * (py - ey);
    float q = q0 < q1 ? q0 : q1;
    const float reach = (ARC_HALF + 0.5f) * (ARC_HALF + 0.5f);
    if (q >= reach) return a;
    float cap = cover(ARC_HALF - sqrtf(q));
    uint8_t c = (uint8_t)(cap * 255.0f + 0.5f);
    return c > a ? c : a;
}

/* Which side of the two-tone split a pixel falls on. The round caps reach
 * pixels just outside the arc's own span, and at the start that angle wraps
 * to near a whole turn: taken at face value it counted as past the split and
 * put a sliver of the second colour at the very start of the arc. A cap pixel
 * takes the colour of the end it belongs to. */
static inline bool past_split(uint16_t angle, uint16_t split_angle, uint16_t end_angle, int32_t margin)
{
    if (angle >= split_angle && angle <= end_angle) return true;
    if (end_angle < split_angle) return false;          /* the arc stops short of the split */
    int32_t after = (int32_t)angle - (int32_t)end_angle;
    if (after < 0) after += 65536;
    return after <= margin;                             /* the end cap, just past the end */
}

void ui_overlay_apply(void *buf, const lv_area_t *area, int stride_px)
{
    if (!s_inited || (!s_rim_on && !s_arc_on)) return;
    /* Nothing on the rim while the logo has the screen. The rim and arc are
     * painted on top of every finished frame, so they go over an opaque
     * full-screen panel as happily as over anything else - and the amp is now
     * asked for the moment the network is up, which armed the spinner and drew
     * a blue arc across the logo (4 Nov 2026). The logo is a picture, not a
     * screen with a rim. */
    if (ui_logo_showing()) return;
    /* Nor over the clock. The guard in arc_set stops anything new being drawn
     * there, but this is the one that cannot be got round by a stale state:
     * the clock is a face, and nothing is painted on it. */
    if (clock_showing() && !ui_net_showing()) return;
    uint16_t *px = buf;
    const uint16_t black = 0;
    uint16_t arc_col = lv_color_to_u16(s_arc_color);
    uint16_t arc_col2 = lv_color_to_u16(s_arc_color2);
    uint16_t start_angle = (uint16_t)(s_arc_from * 65535.0f), end_angle = (uint16_t)(s_arc_frac * 65535.0f);
    bool two_tone = s_arc_split >= 0.0f;
    uint16_t split_angle = (uint16_t)((two_tone ? s_arc_split : 1.0f) * 65535.0f);
    float a0 = s_arc_from * 6.2831853f, a1 = s_arc_frac * 6.2831853f;
    float sx = C - sinf(a0) * ARC_MID, sy = C + cosf(a0) * ARC_MID;
    float ex = C - sinf(a1) * ARC_MID, ey = C + cosf(a1) * ARC_MID;
    /* the arc's angular reach, caps included, as a range that may wrap past 6 o'clock */
    const int32_t margin = (int32_t)((ARC_HALF + 1.5f) / ARC_MID / 6.2831853f * 65536.0f);
    int32_t lo = (int32_t)start_angle - margin, hi = (int32_t)end_angle + margin;
    int32_t lo_wrap = lo < 0 ? lo + 65536 : 65536;   /* angles >= this also count (wrapped below 0) */
    int32_t hi_wrap = hi > 65535 ? hi - 65536 : -1;  /* angles <= this also count (wrapped above 1) */
    /* A span that runs the long way round (the spinner crossing 6 o'clock):
     * everything from lo up, and everything up to hi, is in it. */
    if (lo > hi) { lo_wrap = lo; hi_wrap = hi; }
    /* A whole turn, which is how the spinner ends when the dial has connected
     * (ui_net.c). Its two ends land on the same angle, and taken as a span that
     * is a sliver, not a ring: every pixel round the rim is in it instead. */
    s_arc_full = s_arc_frac - s_arc_from >= 0.999f;
    if (s_arc_full) { lo = 0; hi = 65535; lo_wrap = 65536; hi_wrap = -1; }

    for (int y = area->y1; y <= area->y2; y++) {
        if (y < 0 || y >= CORE_SCREEN) continue;
        uint16_t *row = px + (size_t)(y - area->y1) * stride_px - area->x1;
        if (s_rim_on) {
            const rim_row_t *r = &s_rim[y];
            int solid = r->solid, clear = r->clear;
            for (int x = area->x1 < 0 ? 0 : area->x1; x < solid && x <= area->x2; x++) row[x] = black;
            for (int x = CORE_SCREEN - solid; x <= area->x2 && x < CORE_SCREEN; x++)
                if (x >= area->x1) row[x] = black;
            for (int i = 0; i < clear - solid && i < RIM_AA_MAX; i++) {
                int xl = solid + i, xr = CORE_SCREEN - 1 - solid - i;
                uint8_t a = r->aa[i];
                if (xl >= area->x1 && xl <= area->x2) blend(&row[xl], black, a);
                if (xr >= area->x1 && xr <= area->x2) blend(&row[xr], black, a);
            }
        }
    }
    if (!s_arc_on || s_arc_frac <= s_arc_from) return;
    s_painted_from = s_arc_from;
    s_painted_to = s_arc_frac;
    /* a short arc: visit only its angle buckets (two ranges if it crosses
     * 6 o'clock, as the marker does at the start of the library); a long one:
     * row by row */
    /* Short or long is decided from the arc's own length, not from lo and hi.
     * Those are worked out from the end's angle, which wraps past 6 o'clock:
     * an arc one percent short of a whole turn has its two ends a few units
     * apart, so lo and hi said "short" and only the pixels round the gap were
     * painted. The rest of the ring in the redrawn area came out black, and
     * stayed black until something redrew it - the missing piece Darren saw
     * as the ring closed (5 Nov 2026). */
    if (s_arc_frac - s_arc_from < 0.25f) {
        int32_t ranges[2][2] = {{lo, hi}, {1, 0}};
        if (lo < 0) { ranges[0][0] = 0; ranges[1][0] = lo + 65536; ranges[1][1] = 65535; }
        if (hi > 65535) { ranges[0][1] = 65535; ranges[1][0] = 0; ranges[1][1] = hi - 65536; }
        if (lo > hi) { ranges[0][1] = 65535; ranges[1][0] = 0; ranges[1][1] = hi; }
        for (int r = 0; r < 2; r++) {
            if (ranges[r][0] > ranges[r][1]) continue;
            int b0 = ranges[r][0] >> 8, b1 = ranges[r][1] >> 8;
            for (uint32_t k = s_bucket_start[b0]; k < s_bucket_start[b1 + 1]; k++) {
                const arc_px_t *p = &s_arc_px[s_by_angle[k]];
                if (p->y < area->y1 || p->y > area->y2 || p->x < area->x1 || p->x > area->x2) continue;
                uint16_t *row = px + (size_t)(p->y - area->y1) * stride_px - area->x1;
                uint16_t col = two_tone && past_split(p->angle, split_angle, end_angle, margin) ? arc_col2 : arc_col;
                uint8_t a = arc_alpha(p, p->y, start_angle, end_angle, sx, sy, ex, ey);
                if (s_arc_bright != 255) a = (uint8_t)((uint32_t)a * s_arc_bright / 255);
                blend(&row[p->x], col, a);
            }
        }
        return;
    }
    for (int y = area->y1 < 0 ? 0 : area->y1; y <= area->y2 && y < CORE_SCREEN; y++) {
        uint16_t *row = px + (size_t)(y - area->y1) * stride_px - area->x1;
        for (uint32_t i = s_arc_row_start[y]; i < s_arc_row_start[y + 1]; i++) {
            const arc_px_t *p = &s_arc_px[i];
            if (p->x < area->x1 || p->x > area->x2) continue;
            int32_t ang = p->angle;
            if (ang < lo && ang > hi_wrap && ang > hi && ang < lo_wrap) continue;
            uint16_t col = two_tone && past_split(p->angle, split_angle, end_angle, margin) ? arc_col2 : arc_col;
            uint8_t a = arc_alpha(p, y, start_angle, end_angle, sx, sy, ex, ey);
            if (s_arc_bright != 255) a = (uint8_t)((uint32_t)a * s_arc_bright / 255);
            blend(&row[p->x], col, a);
        }
    }
}

/* ---- State changes, each telling LVGL which part of the screen to redraw ------ */
static void invalidate_rim(void) { lv_obj_invalidate(s_invalidator); }

static void invalidate_arc_span(float f0, float f1)
{
    if (f0 > f1) { float t = f0; f0 = f1; f1 = t; }
    lv_area_t a = {CORE_SCREEN, CORE_SCREEN, -1, -1};
    /* include a cap's width either side of the changed span */
    float pad = (ARC_HALF + 2.0f) / ARC_MID / 6.2831853f;
    f0 -= pad;
    f1 += pad;
    /* sample every 1% of a turn and always the far end itself (missing the end
     * left stale pieces of arc behind when the volume went down) */
    int steps = (int)ceilf((f1 - f0) / 0.01f);
    for (int i = 0; i <= steps; i++) {
        float f = i == steps ? f1 : f0 + i * 0.01f;
        float ang = f * 6.2831853f;
        for (int k = 0; k < 2; k++) {
            float r = k ? ARC_TABLE_R1 : ARC_TABLE_R0;
            int x = (int)(C - sinf(ang) * r), y = (int)(C + cosf(ang) * r);
            if (x - 2 < a.x1) a.x1 = x - 2;
            if (y - 2 < a.y1) a.y1 = y - 2;
            if (x + 2 > a.x2) a.x2 = x + 2;
            if (y + 2 > a.y2) a.y2 = y + 2;
        }
    }
    lv_obj_invalidate_area(s_invalidator, &a);
}

/* A long span as a run of small areas rather than one box: the box round a
 * whole ring is the whole screen. */
static void invalidate_ring(float f0, float f1)
{
    const int pieces = 16;
    for (int i = 0; i < pieces; i++) {
        float a = f0 + (f1 - f0) * i / pieces, b = f0 + (f1 - f0) * (i + 1) / pieces;
        invalidate_arc_span(a, b);
    }
}

float ui_overlay_arc_fraction(void) { return s_arc_on ? s_arc_frac : -1.0f; }

void ui_overlay_rim(bool on)
{
    if (on == s_rim_on) return;
    s_rim_on = on;
    invalidate_rim();
}

static void span_union(float *lo, float *hi, float a, float b)
{
    if (a < *lo) *lo = a;
    if (b > *hi) *hi = b;
}

static void set_split(float split, uint32_t above_hex)
{
    lv_color_t col = lv_color_hex(above_hex);
    if (split == s_arc_split && lv_color_eq(col, s_arc_color2)) return;
    s_arc_split = split;
    s_arc_color2 = col;
    invalidate_rim();
}

static void arc_set(bool on, uint32_t color_hex, float from, float to);
static void spin_run(bool run);

void ui_overlay_arc(bool on, uint32_t color_hex, float from, float to)
{
    set_split(-1.0f, 0);
    arc_set(on, color_hex, from, to);
}

void ui_overlay_arc2(bool on, uint32_t color_hex, uint32_t above_hex, float split, float from, float to)
{
    set_split(split, above_hex);
    arc_set(on, color_hex, from, to);
}

static void spin_step(lv_timer_t *t)
{
    (void)t;
    s_spin_head += (float)SPIN_MS / SPIN_TURN_MS * s_spin_speed;
    s_spin_breath += (float)SPIN_MS / SPIN_BREATH_MS * s_spin_speed;
    if (s_spin_head >= 1.0f) s_spin_head -= 1.0f;
    if (s_spin_breath >= 1.0f) s_spin_breath -= 1.0f;
    /* 0 at both ends of the cycle, 1 in the middle: the arc draws itself out
     * and gathers back in once every breath. */
    float len = SPIN_MIN + (SPIN_MAX - SPIN_MIN) * (0.5f - 0.5f * cosf(s_spin_breath * 6.2831853f));
    s_in_spin = true;
    set_split(-1.0f, 0);
    s_arc_bright = s_spin_bright;
    arc_set(true, s_spin_color, s_spin_head, s_spin_head + len);
    s_in_spin = false;
}

/* How bright and how fast. 255 and 1.0 is full tilt; the menu gets about half
 * of each, because by then the amp is background work and a bright fast arc
 * "smashing its way around" over a list is only distracting. */
void ui_overlay_spin_style(uint8_t bright, float speed)
{
    s_spin_bright = bright;
    s_spin_speed = speed < 0.05f ? 0.05f : speed;
}

/* Run or stop the spinner itself. Arming is a separate question: see
 * ui_overlay_spin. */
static void spin_run(bool run)
{
    if (run == s_spin_on) return;
    s_spin_on = run;
    if (run) {
        if (!s_spin_timer) {
            s_spin_timer = lv_timer_create(spin_step, SPIN_MS, NULL);
            lv_timer_set_auto_delete(s_spin_timer, false);
        }
        lv_timer_resume(s_spin_timer);
        spin_step(NULL);  /* on screen now, not a frame from now */
        return;
    }
    if (s_spin_timer) lv_timer_pause(s_spin_timer);
    s_spin_held = false;
    s_in_spin = true;
    arc_set(false, s_spin_color, 0, 0);  /* leave nothing of it painted */
    s_in_spin = false;
}

/* The spinner, stopped where it is and handed to whoever is finishing the job
 * it stood for. Darren, 5 Nov 2026: the word Connected arriving was so quiet it
 * read as a glitch, and the rim is what the whole dial uses to say something -
 * so when the dial has connected, the arc that said "working on it" sweeps on
 * round into a closed green ring that says "done", and then fades. The arc is
 * drawn from here on with what the caller gives it, at the spinner's own
 * layer, until ui_overlay_spin(false) takes it away or ui_overlay_spin_release
 * sets it going again. */
void ui_overlay_spin_hold(float from, float to, uint32_t color_hex, uint8_t bright)
{
    if (!s_spin_on) return;  /* a view's own arc has the rim; leave it be */
    if (!s_spin_held && s_spin_timer) lv_timer_pause(s_spin_timer);
    s_spin_held = true;
    if (from >= 1.0f) { from -= 1.0f; to -= 1.0f; }
    from = from < 0 ? 0 : from;
    to = to < from ? from : to > from + 1.0f ? from + 1.0f : to;
    s_in_spin = true;
    set_split(-1.0f, 0);
    lv_color_t col = lv_color_hex(color_hex);
    /* Only the leading end moving, nothing else: arc_set redraws just that end.
     * Except the frame the ring closes, which repaints all of it once, so the
     * finished ring is whole whatever any earlier frame left. */
    bool closes = to - from >= 0.999f && s_arc_frac - s_arc_from < 0.999f;
    if (!s_arc_on || (!closes && from == s_arc_from && bright == s_arc_bright && lv_color_eq(col, s_arc_color))) {
        s_arc_bright = bright;
        arc_set(true, color_hex, from, to);
        s_in_spin = false;
        return;
    }
    s_in_spin = false;
    /* The colour or the brightness is changing, or both ends are moving, which
     * means most of the arc has to be painted again - and it changes every
     * frame here, for two seconds. arc_set answers a colour change by redrawing the whole screen,
     * and on the bench that took the dial down to 7 frames a second (5 Nov
     * 2026). A ring's bounding box is the whole screen, so it is asked for in
     * sixteen pieces instead, each only as big as its own stretch of rim. The
     * whole circle, not the old span and the new one: that would be up to
     * thirty-two pieces, and past thirty-two LVGL gives up and redraws the
     * whole screen anyway. The same sixteen every frame cost nothing extra. */
    s_arc_color = col;
    s_arc_bright = bright;
    invalidate_ring(0.0f, 1.0f);
    s_arc_from = from;
    s_arc_frac = to;
}

void ui_overlay_spin_release(void)
{
    if (!s_spin_held) return;
    s_spin_held = false;
    if (!s_spin_on || !s_spin_timer) return;
    lv_timer_resume(s_spin_timer);
    spin_step(NULL);
}

/* Where the spinner's arc is this moment, so a hold can carry on from it
 * rather than jumping. */
void ui_overlay_spin_span(float *from, float *to)
{
    *from = s_arc_from;
    *to = s_arc_frac;
}

void ui_overlay_spin(bool on, uint32_t color_hex)
{
    s_spin_color = color_hex;
    if (on == s_spin_armed) return;
    s_spin_armed = on;
    spin_run(on && !s_view_arc);
}

bool ui_overlay_spinning(void) { return s_spin_on; }

static void arc_set(bool on, uint32_t color_hex, float from, float to)
{
    /* Nothing paints the rim while the clock is up. Seven places ask for an arc
     * - the album marker, the volume, the track marker, the EQ, the listening
     * level - and each of them only checked which screen was showing. The clock
     * is laid over the screen rather than replacing it, so every one of those
     * checks still passed and an arc appeared across the clock face (Darren,
     * 28 Oct 2026). Waking repaints the screen, the arc with it. */
    /* ...with one exception: the connecting overlay (ui_net.c) is laid over the
     * clock and the spinner on the rim is part of it, so the rim does paint
     * while that is up (4 Nov 2026).
     *
     * Only a request to DRAW is refused. Turning the arc off has to get
     * through, or an arc that was already up when the clock arrived stays up:
     * the spinner cannot redraw it, so it freezes mid-sweep and sits there as
     * a quarter segment over the clock face - which is exactly what Darren saw
     * when the amp answered while the clock was up and ui.c switched the
     * spinner off into a guard that threw the call away. */
    if (on && clock_showing() && !ui_net_showing()) return;
    if (!s_in_spin) {
        /* A view's own arc wins while it is up; when it goes, the spinner has
         * the rim again if it is still armed. A volume or a track marker is
         * never dimmed - only the spinner is. */
        s_arc_bright = 255;
        s_view_arc = on;
        if (on) spin_run(false);
        else if (s_spin_armed) { spin_run(true); return; }
    }
    from = from < 0 ? 0 : from > 1 ? 1 : from;
    /* A span may run past a whole turn, meaning it carries on round through
     * 6 o'clock; two turns is the most anything can ask for. */
    to = to < from ? from : to > 2 ? 2 : to;
    lv_color_t col = lv_color_hex(color_hex);
    if (on != s_arc_on || !lv_color_eq(col, s_arc_color)) {
        s_arc_on = on;
        s_arc_color = col;
        s_arc_from = from;
        s_arc_frac = to;
        invalidate_rim();
        return;
    }
    if (from == s_arc_from && to == s_arc_frac) return;
    /* Redraw from whatever is on screen now, not just the previous value: some
     * intermediate values are never painted, and a missed span left a stale
     * piece of arc behind. */
    if (from == s_arc_from) {
        /* growing or shrinking from a fixed start (volume): just the changed end */
        float lo = to, hi = to;
        span_union(&lo, &hi, s_arc_frac, s_arc_frac);
        span_union(&lo, &hi, s_painted_to, s_painted_to);
        invalidate_arc_span(lo, hi);
    } else {
        /* a marker moving: where it was, and where it's going */
        invalidate_arc_span(s_arc_from, s_arc_frac);
        if (s_painted_to > s_painted_from) invalidate_arc_span(s_painted_from, s_painted_to);
        invalidate_arc_span(from, to);
    }
    s_arc_from = from;
    s_arc_frac = to;
    if (to == from) s_painted_from = s_painted_to = from;
}
