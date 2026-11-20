/*
 * A list of two-line rows (a title, a detail line under it) that moves like the
 * menu: a teal pill in the middle, the rows behind it with the fisheye (smaller,
 * darker and closer together away from the pill), detents while dragged, a
 * snap to each item, and a long title or detail scrolling once its row has come
 * to rest in the pill. Every such list shares it (Darren, 27 Oct 2026): the
 * search results and Playlists, and Recent when it's built.
 */
#pragma once

#include "ui_internal.h"

typedef struct ui_list ui_list_t;

/* The pill and up to `max` rows, in `parent`. Anything drawn over the top of the
 * list (a heading band) is created after this. */
ui_list_t *list_create(lv_obj_t *parent, int max);

/* Fill the list: row i shows title[i] and detail[i] (copied; "" for no detail
 * line: the title is then a menu item, the menu's size, centred). Selects `sel`. */
void list_fill(ui_list_t *l, int n, const char *const *title, const char *const *detail, int sel);
void list_show(ui_list_t *l, bool on);    /* the pill and the rows */
int list_count(const ui_list_t *l);
int list_selected(const ui_list_t *l);

/* The way back off a list screen is a finger brought in over the bezel, and
 * nothing else: see ui_touch_from_edge in ui.h. A top band of the screen used
 * to be the way back too, because the list's own dragging swallowed a swipe
 * down once a long list had scrolled up to fill the screen (Darren, 29 Oct
 * 2026). It has gone: with the bezel swipe measured off his own fingers and
 * working on the knob, the band only got in the way, taking a swipe down that
 * started near the top of the glass and throwing him out of the screen
 * (Darren, 30 Oct 2026). So every touch that starts on the glass is the
 * list's, wherever on the glass it starts, and the list's dragging no longer
 * has to share the top of the screen with anything. */
#define LIST_HEADING_BAND 112  /* the heading's band; every screen draws it this deep */

/* Input, as offsets from where the touch began. list_release returns true if
 * it was a tap (open the selected row). */
void list_press(ui_list_t *l);
void list_drag(ui_list_t *l, float dy);
bool list_release(ui_list_t *l, float dy, bool moved);
void list_settle(ui_list_t *l);           /* a drag that wasn't for the list: back to the selected row */
void list_knob(ui_list_t *l, int steps);
/* The row a touch at screen y is on, -1 anywhere else on the screen: the
 * question screen (ui_ask) takes a tap only when it lands on the row in the
 * pill, so a tap anywhere else chooses nothing. */
int list_row_at(const ui_list_t *l, int y);
void list_detent(ui_list_t *l, float k);  /* how strongly it holds at each item while dragged */
/* Whether this gesture belongs to the list, decided on its first real movement
 * and kept for the rest of it. */
bool list_vertical(ui_list_t *l, float dx, float dy);

/* A heading over the top of the list (Darren, 27 Oct 2026: there to introduce
 * the list, not to stay): shown on the first item, fading out as the list
 * moves to the second and back in as it returns, so the list has the whole
 * screen once it's moving. list_heading_shown: more than half there. */
void list_heading(ui_list_t *l, lv_obj_t *band);
bool list_heading_shown(const ui_list_t *l);
