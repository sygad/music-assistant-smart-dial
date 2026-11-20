/* Music Assistant's HTTP API: POST /api {"command": ..., "args": {...}} with the
 * long-lived token. Read-only commands only, for now. */
#include "ma.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "secrets_gen.h"

static const char *TAG = "ma";

#define MA_API SECRET_MA_SERVER "/api"

/* cJSON makes many small allocations; internal RAM is too scarce for that. */
static void *psram_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void psram_free(void *p) { heap_caps_free(p); }

void ma_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = psram_free};
    cJSON_InitHooks(&hooks);
}

/* NULL when any part of it cannot be had. It used to hand back a connection
 * with a NULL buffer or a NULL client inside, and the first request wrote
 * through it: voice search asked for its 96 KB on a dial with 63 KB left in one
 * piece, and the dial restarted (StoreProhibited in esp_http_client_prepare,
 * caught by crash.c, 6 Nov 2026). Callers already treat NULL as "can't". */
ma_conn_t *ma_open(size_t max_response)
{
    ma_conn_t *c = heap_caps_calloc(1, sizeof *c, MALLOC_CAP_SPIRAM);
    if (!c) return NULL;
    c->buf = heap_caps_malloc(max_response + 1, MALLOC_CAP_SPIRAM);
    c->max = max_response;
    /* the token is ~400 characters: the default 512-byte header buffer is too small */
    esp_http_client_config_t cfg = {.url = MA_API, .timeout_ms = 8000, .keep_alive_enable = true,
                                    .buffer_size_tx = 2048};
    c->http = c->buf ? esp_http_client_init(&cfg) : NULL;
    if (!c->http) {
        ESP_LOGE(TAG, "no memory for a connection with a %u-byte reply (PSRAM free %u, largest %u)",
                 (unsigned)max_response, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        heap_caps_free(c->buf);
        heap_caps_free(c);
        return NULL;
    }
    return c;
}

/* Walk a JSON array as it arrives, one element at a time.
 *
 * Two limits used to decide how long a list could be. The reply had to fit
 * c->buf, so a playlist over half a megabyte was cut off mid-track and the
 * rest of it silently lost; and cJSON turns a document into a tree of small
 * nodes costing several times the text, so Anthony Chandler's album - 82
 * tracks, 205 KB - ran the PSRAM out and came back empty (Darren, 29 Oct
 * 2026). Reading it in chunks and handing back each element as it completes
 * removes both: the buffer only ever has to hold one element, and only one
 * element is ever a tree. The length of a list stops mattering.
 */
int ma_each(ma_conn_t *c, const char *command, cJSON *args, ma_item_fn fn, void *ctx)
{
    if (!c) { cJSON_Delete(args); return -1; }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "command", command);
    cJSON_AddItemToObject(req, "args", args ? args : cJSON_CreateObject());
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;
    esp_http_client_set_url(c->http, MA_API);
    esp_http_client_set_method(c->http, HTTP_METHOD_POST);
    esp_http_client_set_header(c->http, "Content-Type", "application/json");
    esp_http_client_set_header(c->http, "Authorization", "Bearer " SECRET_MA_TOKEN);
    int len = (int)strlen(body);
    bool open_ok = esp_http_client_open(c->http, len) == ESP_OK &&
                   esp_http_client_write(c->http, body, len) == len &&
                   esp_http_client_fetch_headers(c->http) >= 0 &&
                   esp_http_client_get_status_code(c->http) == 200;
    cJSON_free(body);
    if (!open_ok) {
        ESP_LOGW(TAG, "%s: request failed", command);
        esp_http_client_close(c->http);
        return -1;
    }

    int fill = 0, scanned = 0, start = -1, depth = 0, count = 0;
    bool str = false, esc = false, in_array = false, keep_going = true, overflow = false;
    for (;;) {
        int room = (int)c->max - fill;
        if (room <= 0) { overflow = true; break; }  /* one element longer than the whole buffer */
        int r = esp_http_client_read(c->http, c->buf + fill, room);
        if (r <= 0) break;
        fill += r;
        for (; scanned < fill && keep_going; scanned++) {
            char ch = c->buf[scanned];
            if (esc) { esc = false; continue; }
            if (str) { if (ch == '\\') esc = true; else if (ch == '"') str = false; continue; }
            if (ch == '"') { str = true; continue; }
            if (!in_array) { if (ch == '[') in_array = true; continue; }
            if (ch == '{' || ch == '[') { if (!depth++) start = scanned; continue; }
            if (ch != '}' && ch != ']') continue;
            if (!depth) { keep_going = false; break; }  /* the array's own closing bracket */
            if (--depth) continue;
            cJSON *item = cJSON_ParseWithLength(c->buf + start, (size_t)(scanned - start + 1));
            keep_going = item && fn(item, count, ctx);
            cJSON_Delete(item);
            count++;
            start = -1;
        }
        if (!keep_going) break;
        /* keep only the element being built; everything before it is spent */
        int from = start >= 0 ? start : scanned;
        if (from > 0) {
            memmove(c->buf, c->buf + from, (size_t)(fill - from));
            fill -= from;
            scanned -= from;
            if (start >= 0) start = 0;
        }
    }
    esp_http_client_close(c->http);
    if (overflow) ESP_LOGW(TAG, "%s: a single element did not fit %u bytes", command, (unsigned)c->max);
    return count;
}

cJSON *ma_call(ma_conn_t *c, const char *command, cJSON *args)
{
    if (!c) { cJSON_Delete(args); return NULL; }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "command", command);
    cJSON_AddItemToObject(req, "args", args ? args : cJSON_CreateObject());
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return NULL;

    esp_http_client_set_url(c->http, MA_API);
    esp_http_client_set_method(c->http, HTTP_METHOD_POST);
    esp_http_client_set_header(c->http, "Content-Type", "application/json");
    esp_http_client_set_header(c->http, "Authorization", "Bearer " SECRET_MA_TOKEN);
    int len = (int)strlen(body);
    cJSON *out = NULL;
    if (esp_http_client_open(c->http, len) == ESP_OK && esp_http_client_write(c->http, body, len) == len &&
        esp_http_client_fetch_headers(c->http) >= 0) {
        int status = esp_http_client_get_status_code(c->http);
        int n = 0, r;
        while (n < (int)c->max && (r = esp_http_client_read(c->http, c->buf + n, (int)c->max - n)) > 0) n += r;
        c->buf[n] = 0;
        if (status == 200) out = cJSON_ParseWithLength(c->buf, n);
        else ESP_LOGW(TAG, "%s: HTTP %d", command, status);
        if (n >= (int)c->max) ESP_LOGW(TAG, "%s: response truncated at %u bytes", command, (unsigned)c->max);
    } else {
        ESP_LOGW(TAG, "%s: request failed", command);
    }
    esp_http_client_close(c->http);
    cJSON_free(body);
    return out;
}

int ma_get(ma_conn_t *c, const char *url, uint8_t *dst, int max, bool (*keep_going)(void *), void *arg)
{
    if (!c || !dst) return -1;
    esp_http_client_set_url(c->http, url);
    esp_http_client_set_method(c->http, HTTP_METHOD_GET);
    esp_http_client_delete_header(c->http, "Content-Type");
    esp_http_client_delete_header(c->http, "Authorization");  /* covers don't need the token */
    /* -3 says the network is at fault and the same request is worth making
     * again; -1 says the server answered and the answer was no. */
    if (esp_http_client_open(c->http, 0) != ESP_OK) {
        esp_http_client_close(c->http);  /* start the next one from a clean socket */
        return -3;
    }
    if (esp_http_client_fetch_headers(c->http) < 0) {
        esp_http_client_close(c->http);
        return -3;
    }
    if (esp_http_client_get_status_code(c->http) != 200) {
        esp_http_client_close(c->http);
        return -1;
    }
    int n = 0, r = 0;
    while (n < max && (r = esp_http_client_read(c->http, (char *)dst + n, (max - n) > 4096 ? 4096 : max - n)) > 0) {
        n += r;
        if (keep_going && !keep_going(arg)) {
            esp_http_client_close(c->http);  /* half-read: nothing worth keeping */
            return -2;
        }
    }
    /* Hold the connection open between covers. Closing each one saves no
     * measurable time per cover - that was measured twice - but every close
     * leaves a TCP control block behind, and two loaders fetching ten covers a
     * second get through them fast. Reuse is only safe once the body has been
     * read to the end, so a short read or a full buffer still closes. */
    if (r < 0 || n >= max) esp_http_client_close(c->http);
    return n;
}
