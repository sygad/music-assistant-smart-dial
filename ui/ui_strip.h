/*
 * A horizontal strip of full-screen slides, one per item (album or track), each
 * a cover with the title band. Three slides are recycled around the current
 * position, so the strip can hold any number of items.
 *
 * The strip owns its motion: drag it directly, snap to an item with the
 * design's slide curve, or glide (a fling) and settle.
 */
#pragma once

#include "ui_internal.h"

typedef struct {
    int (*count)(void *ctx);
    const char *(*title)(void *ctx, int i);
    const char *(*sub)(void *ctx, int i);  /* optional second line under the title, smaller */
    const lv_image_dsc_t *(*cover)(void *ctx, int i);
    void (*moved)(void *ctx, float pos, float speed);  /* every frame while moving; speed in items/s */
    void (*settled)(void *ctx, int i);
    void *ctx;
} strip_src_t;

typedef struct strip strip_t;

strip_t *strip_create(lv_obj_t *parent, const strip_src_t *src);
lv_obj_t *strip_obj(strip_t *s);

float strip_pos(const strip_t *s);
int strip_target(const strip_t *s);       /* where it's heading (or settled) */
bool strip_moving(const strip_t *s);
float strip_speed(const strip_t *s);      /* items per ms, signed */

void strip_jump(strip_t *s, float pos);   /* no animation; settles if pos is whole */
void strip_drag(strip_t *s, float pos);   /* follow a finger: no settling */
void strip_snap(strip_t *s, int target, uint32_t ms);
void strip_glide(strip_t *s, int target, uint32_t tau_ms);
void strip_stop(strip_t *s);              /* halt where it is (a touch) */

void strip_refresh(strip_t *s, int i);    /* item i's cover or title changed; -1 = all */
void strip_set_band_opa(strip_t *s, lv_opa_t opa);
void strip_set_visible(strip_t *s, bool visible);
uint32_t strip_title_cycle_ms(strip_t *s, int i);  /* one scroll pass of item i's title, 0 if it fits */
