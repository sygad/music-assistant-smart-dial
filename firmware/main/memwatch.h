#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Memory as something the dial watches, rather than something found out about
 * after it has run out (6 Nov 2026).
 *
 * The dial has two heaps that fail in different ways. Internal RAM is small
 * and is what Wi-Fi, sockets, the display's DMA buffers and every task's
 * control block must have; PSRAM is large and holds the covers and the
 * library. Every memory fault so far was one of them running out while the
 * other had plenty, and the free TOTAL hid it each time - so everything here
 * reports the largest single free block, which is what an allocation actually
 * has to fit in.
 *
 * The floors are the smallest internal block and the smallest PSRAM block the
 * dial must always be able to find, measured against what really asks for
 * them (docs/MEMORY.md). Below either one, something that worked yesterday
 * starts failing today and blames the network. */

/* What one boot stage left behind. Logged as it happens and kept, so the
 * `memmap` console command can print the whole boot again later. */
void mem_mark(const char *stage);

/* The full picture on the console: the boot stages, every task (where its
 * stack lives and how close it has come to the end of it), and both heaps
 * walked block by block so fragmentation is visible rather than inferred. */
void mem_report(void);

/* Both heaps, in the numbers that matter. */
typedef struct {
    size_t int_free, int_largest, int_lowest;
    size_t ps_free, ps_largest, ps_lowest;
} mem_now_t;
mem_now_t mem_now(void);

/* True when either heap's largest free block is under its floor right now. */
bool mem_below_floor(void);

/* The floors, and where each number comes from, are in docs/MEMORY.md. They
 * are checked every five minutes alongside the RAM sensor, and a breach is
 * logged and posted to Home Assistant rather than left to surface as some
 * other fault. */
#define MEM_FLOOR_INTERNAL_BLOCK 2048
#define MEM_FLOOR_PSRAM_BLOCK (128 * 1024)
