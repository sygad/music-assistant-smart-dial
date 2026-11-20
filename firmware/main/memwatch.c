#include "memwatch.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mem";

void mem_report_jpeg(void);

mem_now_t mem_now(void)
{
    return (mem_now_t){
        .int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        .int_lowest = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        .ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .ps_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        .ps_lowest = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
    };
}

bool mem_below_floor(void)
{
    mem_now_t m = mem_now();
    return m.int_largest < MEM_FLOOR_INTERNAL_BLOCK || m.ps_largest < MEM_FLOOR_PSRAM_BLOCK;
}

/* ---- boot stages ---- */

#define MARKS 16
typedef struct {
    const char *stage;
    uint32_t ms;
    mem_now_t m;
} mark_t;
/* In PSRAM, like everything here: the instrument must not take what it is
 * measuring. */
EXT_RAM_BSS_ATTR static mark_t s_marks[MARKS];
static int s_nmarks;

static void print_mark(const mark_t *k)
{
    ESP_LOGI(TAG, "STAGE %-14s %6lu ms  internal free %6u largest %6u lowest %6u | PSRAM free %7u largest %7u",
             k->stage, (unsigned long)k->ms, (unsigned)k->m.int_free, (unsigned)k->m.int_largest,
             (unsigned)k->m.int_lowest, (unsigned)k->m.ps_free, (unsigned)k->m.ps_largest);
}

void mem_mark(const char *stage)
{
    mark_t k = {.stage = stage, .ms = (uint32_t)(esp_timer_get_time() / 1000), .m = mem_now()};
    if (s_nmarks < MARKS) s_marks[s_nmarks++] = k;
    print_mark(&k);
}

/* ---- tasks ---- */

static void report_tasks(void)
{
    EXT_RAM_BSS_ATTR static TaskStatus_t st[40];
    UBaseType_t n = uxTaskGetSystemState(st, sizeof st / sizeof st[0], NULL);
    ESP_LOGI(TAG, "TASKS %u (stack: I = internal RAM, P = PSRAM; headroom = least ever left, bytes)", (unsigned)n);
    for (UBaseType_t i = 0; i < n; i++) {
        const void *base = st[i].pxStackBase;
        char where = esp_ptr_external_ram(base) ? 'P' : esp_ptr_internal(base) ? 'I' : '?';
        ESP_LOGI(TAG, "TASK %-16s stack %c  headroom %5u", st[i].pcTaskName, where,
                 (unsigned)(st[i].usStackHighWaterMark * sizeof(StackType_t)));
    }
}

/* ---- heap walk ----
 *
 * The walker runs with the heap locked, so it may not allocate or log: it only
 * counts, into these. Used blocks are counted by size class; free blocks are
 * listed one by one (the biggest few), because the shape of the free space is
 * the fragmentation. */

#define CLASSES 8
static const size_t CLASS_TOP[CLASSES] = {32, 128, 512, 2048, 8192, 32768, 131072, SIZE_MAX};
typedef struct {
    unsigned used_n[CLASSES];
    size_t used_b[CLASSES];
    unsigned free_n;
    size_t free_b;
    size_t top[8];
} walk_t;

static bool walker(walker_heap_into_t heap, walker_block_info_t b, void *user)
{
    (void)heap;
    walk_t *w = user;
    if (b.used) {
        int c = 0;
        while (b.size > CLASS_TOP[c]) c++;
        w->used_n[c]++;
        w->used_b[c] += b.size;
    } else {
        w->free_n++;
        w->free_b += b.size;
        for (int i = 0; i < 8; i++) {
            if (b.size > w->top[i]) {
                memmove(&w->top[i + 1], &w->top[i], (7 - i) * sizeof w->top[0]);
                w->top[i] = b.size;
                break;
            }
        }
    }
    return true;
}

static void report_heap(const char *name, uint32_t caps)
{
    EXT_RAM_BSS_ATTR static walk_t w;
    memset(&w, 0, sizeof w);
    heap_caps_walk(caps, walker, &w);
    ESP_LOGI(TAG, "HEAP %s: %u free blocks, %u bytes; biggest %u %u %u %u %u %u %u %u", name, w.free_n,
             (unsigned)w.free_b, (unsigned)w.top[0], (unsigned)w.top[1], (unsigned)w.top[2], (unsigned)w.top[3],
             (unsigned)w.top[4], (unsigned)w.top[5], (unsigned)w.top[6], (unsigned)w.top[7]);
    static const char *LABEL[CLASSES] = {"<=32", "<=128", "<=512", "<=2K", "<=8K", "<=32K", "<=128K", ">128K"};
    for (int c = 0; c < CLASSES; c++) {
        if (!w.used_n[c]) continue;
        ESP_LOGI(TAG, "HEAP %s used %-6s %5u blocks %8u bytes", name, LABEL[c], w.used_n[c], (unsigned)w.used_b[c]);
    }
}

void mem_report(void)
{
    for (int i = 0; i < s_nmarks; i++) print_mark(&s_marks[i]);
    mark_t now = {.stage = "now", .ms = (uint32_t)(esp_timer_get_time() / 1000), .m = mem_now()};
    print_mark(&now);
    ESP_LOGI(TAG, "LOWEST EVER internal %u, PSRAM %u; floors: internal block %u, PSRAM block %u%s",
             (unsigned)now.m.int_lowest, (unsigned)now.m.ps_lowest, (unsigned)MEM_FLOOR_INTERNAL_BLOCK,
             (unsigned)MEM_FLOOR_PSRAM_BLOCK, mem_below_floor() ? "  ** BELOW FLOOR **" : "");
    report_tasks();
    report_heap("internal", MALLOC_CAP_INTERNAL);
    report_heap("PSRAM", MALLOC_CAP_SPIRAM);
    mem_report_jpeg();
}

/* ---- the cover decoder's own memory ----
 *
 * esp_new_jpeg asks for its working memory with "internal first, PSRAM if
 * not" (its jpeg_calloc_inner and jpeg_calloc_align_inner, read off the
 * disassembly on 6 Nov 2026: caps 0x804 tried before 0x404), and frees it
 * again when the decode closes - twice for every cover, since decode() opens
 * once for the header and once to decode. On a dial with a few KB of internal
 * RAM that is thousands of short-lived internal allocations a day, made in the
 * middle of whatever Wi-Fi is doing. These wrappers (linked in with --wrap in
 * CMakeLists.txt) count what it asks for and where it lands, so that is a
 * figure rather than a guess. */

#include <stdatomic.h>

void *__real_jpeg_calloc_inner(size_t size);
void *__real_jpeg_calloc_align_inner(size_t size, int aligned);
void __real_jpeg_free(void *p);
void __real_jpeg_free_align(void *p);

static _Atomic unsigned s_jpeg_calls, s_jpeg_internal_calls;
static _Atomic size_t s_jpeg_biggest, s_jpeg_live_internal, s_jpeg_peak_internal;

static void jpeg_took(void *p, size_t size)
{
    if (!p) return;
    atomic_fetch_add(&s_jpeg_calls, 1);
    size_t was = atomic_load(&s_jpeg_biggest);
    while (size > was && !atomic_compare_exchange_weak(&s_jpeg_biggest, &was, size)) {}
    if (!esp_ptr_internal(p)) return;
    atomic_fetch_add(&s_jpeg_internal_calls, 1);
    size_t held = heap_caps_get_allocated_size(p);  /* as the free side will see it */
    size_t live = atomic_fetch_add(&s_jpeg_live_internal, held) + held;
    size_t peak = atomic_load(&s_jpeg_peak_internal);
    while (live > peak && !atomic_compare_exchange_weak(&s_jpeg_peak_internal, &peak, live)) {}
}

static void jpeg_gave(void *p, size_t size)
{
    if (p && esp_ptr_internal(p)) atomic_fetch_sub(&s_jpeg_live_internal, size);
}

/* And where it lands: PSRAM, with internal only if PSRAM has nothing. Measured
 * 6 Nov 2026 on the dial, a cover browse took up to 20 KB of internal RAM at
 * once in these, over 1,200 allocations in a minute, on a heap whose largest
 * block had been 4 KB the day before. The cost of moving it is in
 * docs/MEMORY.md. Freed by the decoder's own jpeg_free / jpeg_free_align,
 * which are heap_caps_free underneath and so take either. */
void *__wrap_jpeg_calloc_inner(size_t size)
{
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = __real_jpeg_calloc_inner(size);
    jpeg_took(p, size);
    return p;
}

void *__wrap_jpeg_calloc_align_inner(size_t size, int aligned)
{
    void *p = heap_caps_aligned_calloc((size_t)aligned, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = __real_jpeg_calloc_align_inner(size, aligned);
    jpeg_took(p, size);
    return p;
}

/* The size is read back from the heap, since free is not told it; the taking
 * side counts the same way, so the two always cancel. */
void __wrap_jpeg_free(void *p)
{
    jpeg_gave(p, p ? heap_caps_get_allocated_size(p) : 0);
    __real_jpeg_free(p);
}

void __wrap_jpeg_free_align(void *p)
{
    jpeg_gave(p, p ? heap_caps_get_allocated_size(p) : 0);
    __real_jpeg_free_align(p);
}

void mem_report_jpeg(void)
{
    ESP_LOGI(TAG, "JPEG %u allocations, %u of them internal; biggest %u; internal held now %u, at most %u",
             atomic_load(&s_jpeg_calls), atomic_load(&s_jpeg_internal_calls), (unsigned)atomic_load(&s_jpeg_biggest),
             (unsigned)atomic_load(&s_jpeg_live_internal), (unsigned)atomic_load(&s_jpeg_peak_internal));
}
