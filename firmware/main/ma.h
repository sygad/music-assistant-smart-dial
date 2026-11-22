#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "secrets_gen.h"  /* the addresses */
#include "where.h"        /* MA_PLAYER below */

typedef struct {
    esp_http_client_handle_t http;
    char *buf;
    size_t max;
} ma_conn_t;

/* The one player the dial owns, in one place.
 *
 * It was a #define inside playback.c while playback was the only thing that
 * named it. The connecting sequence now asks Music Assistant whether this
 * player is available before it says the dial is connected (16 Nov 2026), and
 * two copies of a player id that must agree is exactly the half-fix that costs
 * an afternoon - so there is one copy and both files read it. */
/* Since 22 Nov 2026 it can come from Home Assistant as well as from
 * config/secrets.local, so it is read at run time (where.h). */
#define MA_PLAYER where_player()

void ma_init(void);                          /* once: cJSON allocates from PSRAM */
ma_conn_t *ma_open(size_t max_response);     /* one per task; keeps the connection open */
void ma_close(ma_conn_t *c);                 /* for a connection used once, e.g. at start-up */

/* Run an API command. Takes ownership of args. Returns the parsed result (free
 * with cJSON_Delete) or NULL. */
cJSON *ma_call(ma_conn_t *c, const char *command, cJSON *args);

/* Run a command whose result is an array, one element at a time, so a long
 * list never has to exist as one cJSON tree. Return false from fn to stop.
 * Takes ownership of args. Returns how many elements were seen, or -1. */
typedef bool (*ma_item_fn)(const cJSON *item, int index, void *ctx);
int ma_each(ma_conn_t *c, const char *command, cJSON *args, ma_item_fn fn, void *ctx);

/* GET a URL (cover images) into dst. keep_going is polled while reading; return
 * false to abandon (-2). Returns bytes read, -1 if the server refused, or -3 if
 * the network did (worth asking again once it is back). */
int ma_get(ma_conn_t *c, const char *url, uint8_t *dst, int max, bool (*keep_going)(void *), void *arg);
