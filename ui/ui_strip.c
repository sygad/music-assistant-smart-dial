#include "ui_strip.h"

#include <math.h>
#include <string.h>

#define SLIDES 3
#define PLACEHOLDER 0x1C1C1C  /* not in the design: shown until a cover has loaded */
#define BAND_X (-2)
#define BAND_W 364

typedef struct {
    lv_obj_t *slide, *img, *band, *label, *sub;
    int item;
    int text_w;
} slide_t;

/* The band carries the album and, where the source offers one, the artist
 * underneath it in a smaller size. Album browse is ordered by artist, so the
 * album name alone left Darren with no idea where in the library he was
 * (29 Oct 2026). Now Playing passes no second line and keeps the one-line
 * band it was designed with. */
#define BAND_ONE 52
#define TITLE_FONT font_inter_regular_24  /* the album; the same size as the chip (Darren, 19 Nov 2026) */
#define SUB_FONT font_inter_regular_20    /* the artist above it, smaller but not by much */
#define SUB_COLOUR 0xB4B4B4               /* brighter than UI_GREY: the artist is what he is tracking */
#define BAND_LIFT 20                      /* ...and the whole band sits higher, still below centre */

typedef enum { MOTION_NONE, MOTION_SNAP, MOTION_GLIDE } motion_t;

struct strip {
    strip_src_t src;
    lv_obj_t *root;
    slide_t slides[SLIDES];
    float pos;
    int target;
    motion_t motion;
    float from, dist;
    uint32_t t0, ms;  /* ms: snap duration or glide time constant */
    lv_timer_t *timer;
    lv_opa_t band_opa;
    int last_settled;
};

static strip_t s_strips[2];
static int s_nstrips;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int last(const strip_t *s) { int n = s->src.count(s->src.ctx); return n > 0 ? n - 1 : 0; }

/* ---- Title band ------------------------------------------------------------
 * Band (-2, 267.7) 364x52, black 80%, Inter 24. A title that fits is centred.
 * One that doesn't is left-aligned at 70 and scrolls, once settled. */
static void marquee_exec(void *label, int32_t t)
{
    slide_t *sl = lv_obj_get_user_data(label);
    int over = sl->text_w - CORE_TITLE_SPACE_PX;
    lv_obj_set_x(label, CORE_TITLE_LEFT_PX + (int32_t)lroundf(core_marquee_offset(over, (uint32_t)t)));
}

static void label_rest(slide_t *sl)
{
    lv_anim_delete(sl->label, marquee_exec);
    lv_obj_set_x(sl->label, (BAND_W - sl->text_w) / 2);
    ui_reading_done(sl->label);
}

static void label_scroll(slide_t *sl)
{
    int over = sl->text_w - CORE_TITLE_SPACE_PX;
    if (over <= 0) return;
    lv_obj_set_x(sl->label, CORE_TITLE_LEFT_PX);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sl->label);
    lv_anim_set_exec_cb(&a, marquee_exec);
    uint32_t cycle = core_marquee_cycle_ms(over);
    lv_anim_set_values(&a, 0, (int32_t)cycle);
    lv_anim_set_duration(&a, cycle);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    ui_reading(sl->label, CORE_CLOCK_READ_PASSES * cycle);
}

static int text_width(const char *t)
{
    lv_point_t size;
    lv_text_get_size(&size, t, &TITLE_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* ---- Slides ----------------------------------------------------------------- */
static void bind_cover(strip_t *s, slide_t *sl)
{
    const lv_image_dsc_t *img = s->src.cover(s->src.ctx, sl->item);
    /* placeholder fill only when there's no cover: LVGL draws it even when the
     * cover hides it, and that cost ~25% of the frame rate while sliding */
    if (img) {
        lv_image_set_src(sl->img, img);
        lv_obj_remove_flag(sl->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(sl->slide, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_add_flag(sl->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(sl->slide, LV_OPA_COVER, 0);
    }
}

static void bind(strip_t *s, slide_t *sl, int item)
{
    sl->item = item;
    bind_cover(s, sl);
    const char *title = s->src.title(s->src.ctx, item);
    lv_label_set_text(sl->label, title);
    sl->text_w = text_width(title);
    lv_obj_set_width(sl->label, sl->text_w);
    label_rest(sl);
    if (sl->sub) {
        /* Where Music Assistant has no artist of its own it repeats the album
         * name, and "Pop Party 10" twice over tells you nothing. */
        const char *who = s->src.sub(s->src.ctx, item);
        if (who && title && !strcmp(who, title)) who = NULL;
        lv_label_set_text(sl->sub, who ? who : "");
        lv_obj_set_width(sl->sub, BAND_W);  /* centred, and clipped if it is too long */
    }
}

static void layout(strip_t *s)
{
    int n = s->src.count(s->src.ctx);
    int first = (int)floorf(s->pos) - 1;
    for (int k = 0; k < SLIDES; k++) {
        int item = first + k;
        slide_t *sl = &s->slides[((item % SLIDES) + SLIDES) % SLIDES];
        if (item < 0 || item >= n) {
            lv_obj_add_flag(sl->slide, LV_OBJ_FLAG_HIDDEN);
            sl->item = -1;
            continue;
        }
        if (sl->item != item) bind(s, sl, item);
        lv_obj_remove_flag(sl->slide, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(sl->slide, (int32_t)lroundf((item - s->pos) * CORE_SCREEN));
    }
}

static slide_t *slide_for(strip_t *s, int item)
{
    for (int k = 0; k < SLIDES; k++)
        if (s->slides[k].item == item) return &s->slides[k];
    return NULL;
}

/* ---- Motion ----------------------------------------------------------------- */
float strip_speed(const strip_t *s)
{
    float t = (float)lv_tick_elaps(s->t0);
    if (s->motion == MOTION_GLIDE) return s->dist / s->ms * expf(-t / s->ms);
    if (s->motion == MOTION_SNAP && t < s->ms) {
        float dt = 4.0f, a = t / s->ms, b = (t + dt) / s->ms;
        b = b > 1 ? 1 : b;
        return s->dist * (core_bezier(0.2f, 0.9f, 0.3f, 1.0f, b) - core_bezier(0.2f, 0.9f, 0.3f, 1.0f, a)) / dt;
    }
    return 0.0f;
}

static void settle(strip_t *s)
{
    s->motion = MOTION_NONE;
    lv_timer_pause(s->timer);
    s->target = (int)lroundf(clampf(s->pos, 0, (float)last(s)));
    s->pos = (float)s->target;
    layout(s);
    slide_t *sl = slide_for(s, s->target);
    if (sl) label_scroll(sl);
    if (s->target != s->last_settled) ui_tick();  /* landed on a new album or track */
    s->last_settled = s->target;
    if (s->src.moved) s->src.moved(s->src.ctx, s->pos, 0.0f);
    if (s->src.settled) s->src.settled(s->src.ctx, s->target);
}

static void tick(lv_timer_t *t)
{
    strip_t *s = lv_timer_get_user_data(t);
    float el = (float)lv_tick_elaps(s->t0);
    if (s->motion == MOTION_SNAP) {
        float p = el >= s->ms ? 1.0f : core_bezier(0.2f, 0.9f, 0.3f, 1.0f, el / s->ms);
        s->pos = s->from + s->dist * p;
        if (p >= 1.0f) { settle(s); return; }
    } else if (s->motion == MOTION_GLIDE) {
        float p = 1.0f - expf(-el / s->ms);
        s->pos = s->from + s->dist * p;
        if (fabsf(s->dist) * (1.0f - p) * CORE_SCREEN < 0.5f) { settle(s); return; }
    }
    layout(s);
    if (s->src.moved) s->src.moved(s->src.ctx, s->pos, strip_speed(s) * 1000.0f);
}

static void start(strip_t *s, motion_t m, int target, uint32_t ms)
{
    target = target < 0 ? 0 : target > last(s) ? last(s) : target;
    s->motion = m;
    s->from = s->pos;
    s->dist = (float)target - s->pos;
    s->t0 = lv_tick_get();
    s->ms = ms < 1 ? 1 : ms;
    s->target = target;
    slide_t *sl = slide_for(s, (int)lroundf(s->from));
    if (sl) label_rest(sl);
    if (fabsf(s->dist) * CORE_SCREEN < 0.5f) { settle(s); return; }
    lv_timer_resume(s->timer);
}

void strip_snap(strip_t *s, int target, uint32_t ms) { start(s, MOTION_SNAP, target, ms); }
void strip_glide(strip_t *s, int target, uint32_t tau_ms) { start(s, MOTION_GLIDE, target, tau_ms); }

void strip_stop(strip_t *s)
{
    s->motion = MOTION_NONE;
    lv_timer_pause(s->timer);
    for (int k = 0; k < SLIDES; k++) label_rest(&s->slides[k]);
}

void strip_drag(strip_t *s, float pos)
{
    s->pos = clampf(pos, -0.4f, last(s) + 0.4f);
    layout(s);
}

void strip_jump(strip_t *s, float pos)
{
    s->motion = MOTION_NONE;
    lv_timer_pause(s->timer);
    s->pos = clampf(pos, 0, (float)last(s));
    settle(s);
}

float strip_pos(const strip_t *s) { return s->pos; }
int strip_target(const strip_t *s) { return s->target; }
bool strip_moving(const strip_t *s) { return s->motion != MOTION_NONE; }
lv_obj_t *strip_obj(strip_t *s) { return s->root; }

void strip_refresh(strip_t *s, int i)
{
    if (i < 0) {
        for (int k = 0; k < SLIDES; k++) s->slides[k].item = -1;
        layout(s);
        slide_t *sl = s->motion == MOTION_NONE ? slide_for(s, s->target) : NULL;
        if (sl) label_scroll(sl);
        return;
    }
    slide_t *sl = slide_for(s, i);
    if (sl) bind_cover(s, sl);
}

void strip_set_band_opa(strip_t *s, lv_opa_t opa)
{
    s->band_opa = opa;
    for (int k = 0; k < SLIDES; k++) {
        lv_obj_t *b = s->slides[k].band;
        lv_obj_set_style_opa(b, opa, 0);
        /* hidden, not just transparent, so LVGL doesn't build a layer for it */
        if (opa == LV_OPA_TRANSP) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
}

void strip_set_visible(strip_t *s, bool visible)
{
    if (visible) lv_obj_remove_flag(s->root, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s->root, LV_OBJ_FLAG_HIDDEN);
}

uint32_t strip_title_cycle_ms(strip_t *s, int i)
{
    if (i < 0 || i > last(s)) return 0;
    return core_marquee_cycle_ms(text_width(s->src.title(s->src.ctx, i)) - CORE_TITLE_SPACE_PX);
}

strip_t *strip_create(lv_obj_t *parent, const strip_src_t *src)
{
    strip_t *s = &s_strips[s_nstrips++];
    s->src = *src;
    s->band_opa = LV_OPA_COVER;
    s->root = ui_plain(parent);
    lv_obj_set_size(s->root, CORE_SCREEN, CORE_SCREEN);

    const lv_font_t *f = &TITLE_FONT;
    int title_h = lv_font_get_line_height(f), sub_h = lv_font_get_line_height(&SUB_FONT);
    /* One line sits in the band the design drew. Two lines put the artist on
     * top, because that is the order the library is sorted in and so the thing
     * being tracked while hunting; the album stays the larger of the two. The
     * band keeps the same breathing space, grows upwards and lifts clear of
     * the foot of the screen, while staying below the middle (Darren, 29 Oct
     * 2026, looking at Screamadelica). */
    int pad = (BAND_ONE - title_h) / 2;
    if (pad < 6) pad = 6;
    int band_h = s->src.sub ? pad + sub_h + title_h + pad : BAND_ONE;
    int band_y = 268 + BAND_ONE - band_h - (s->src.sub ? BAND_LIFT : 0);
    int top = s->src.sub ? pad + sub_h : (int)lroundf((BAND_ONE - title_h) / 2.0f);
    for (int i = 0; i < SLIDES; i++) {
        slide_t *sl = &s->slides[i];
        sl->item = -1;
        sl->slide = ui_plain(s->root);
        lv_obj_set_size(sl->slide, CORE_SCREEN, CORE_SCREEN);
        lv_obj_set_style_bg_color(sl->slide, lv_color_hex(PLACEHOLDER), 0);
        lv_obj_set_style_bg_opa(sl->slide, LV_OPA_COVER, 0);

        sl->img = lv_image_create(sl->slide);
        lv_obj_set_pos(sl->img, 0, 0);

        sl->band = ui_plain(sl->slide);
        lv_obj_set_pos(sl->band, BAND_X, band_y); /* Figma: 267.7 for one line */
        lv_obj_set_size(sl->band, BAND_W, band_h);
        lv_obj_set_style_bg_color(sl->band, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_bg_opa(sl->band, UI_OPA_SCRIM, 0);

        sl->label = lv_label_create(sl->band);
        lv_obj_remove_flag(sl->label, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(sl->label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_font(sl->label, f, 0);
        lv_obj_set_style_text_color(sl->label, lv_color_hex(UI_WHITE), 0);
        lv_obj_set_y(sl->label, top);
        lv_obj_set_user_data(sl->label, sl);

        if (s->src.sub) {
            sl->sub = lv_label_create(sl->band);
            lv_obj_remove_flag(sl->sub, LV_OBJ_FLAG_CLICKABLE);
            lv_label_set_long_mode(sl->sub, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_text_font(sl->sub, &SUB_FONT, 0);
            lv_obj_set_style_text_color(sl->sub, lv_color_hex(SUB_COLOUR), 0);
            lv_obj_set_style_text_align(sl->sub, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(sl->sub, 0, pad);
        }
    }
    s->timer = lv_timer_create(tick, 10, s);
    lv_timer_pause(s->timer);
    layout(s);
    return s;
}
