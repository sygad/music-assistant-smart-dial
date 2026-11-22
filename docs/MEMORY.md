# The dial's memory budget

Memory is a budget this firmware keeps on purpose, not a thing found out about when
it runs out. Every change that keeps memory (a buffer, a task, a cache, a screen)
updates the tables here, with figures read off the dial rather than worked out.

First written 6 Nov 2026, after three faults in one day that were all memory: OTA
could never start, voice search restarted the dial, and a failed update kept it awake.

## Two heaps, and they fail differently

| | Size | What must use it | How it fails |
|---|---|---|---|
| **Internal RAM** | 341 KB in all, 205 KB of it free when `app_main` starts | Wi-Fi's buffers, the display's DMA strips, every task's control block, any task that writes flash | Something network-shaped fails ("couldn't reach Home Assistant", a socket that won't open), which looks like a network fault |
| **PSRAM** | 8 MB | Everything else: covers, the library, screens, reply buffers | The next big allocation fails wherever it happens to be, often as a restart |

**The number that matters is the largest free block, not the free total.** An
allocation has to fit in one piece. The free total hid both of the 6 Nov faults.

## Where internal RAM goes (measured at boot, 6 Nov 2026)

From the `STAGE` lines `memmap` prints. "Took" is the drop in free internal RAM over
that stage.

| Stage | Took | What it is |
|---|---|---|
| Before `app_main` | (fixed) | IDF's own tasks, 126 KB of code that must run from internal RAM, the 64 KB pool held back for DMA |
| Display | 77 KB | Two 40-line draw strips, 28.8 KB each, which the panel's DMA reads, plus the LVGL task's 16 KB stack |
| Cover pool, microphone | 4 KB | Queues, locks, the microphone's DMA |
| Wi-Fi | 67 KB | Wi-Fi's fixed receive and transmit buffers and its tasks. Dips to **7.8 KB free** while it starts, the lowest point of any boot |
| Services | 10 KB | Control blocks for the network tasks, queues, one-off start-up tasks |
| **Left, settled** | **52 KB free, largest block 39–43 KB** | Before 6 Nov: 4.6 KB free, largest 2.3 KB, lowest ever 156 bytes |

### What was moved out on 6 Nov, and what it cost

| Moved | Internal RAM given back | Cost, measured on the dial |
|---|---|---|
| The rim's per-row table (`s_rim`, 24 KB) to PSRAM | 24 KB | 0.2 ms more per frame on the rim (1.6 → 1.8 ms). Album drag 25–30 fps before and after |
| Recent lists, the mix, gesture records, Recent's on-screen copy, the Wi-Fi scan table, the screenshot line buffer, to PSRAM | 13 KB | None: read by the screens only |
| The cover decoder's working memory to PSRAM | Up to **20 KB at a time**, more than 3,000 allocations a minute while browsing | Decode 34 → 35 ms a cover |

The decoder was the one that did the damage. esp_new_jpeg asks for its working memory
"internal first", twice per cover. Browsing took internal RAM down to 208 bytes free
on the old layout. That's the state in which Wi-Fi can't get a buffer.

## Where PSRAM goes

| What | Size | Notes |
|---|---|---|
| Cover pool, 14 + 3 playlist pictures | 4.4 MB | Fixed at boot, never freed |
| Two download buffers + decode scratch | 0.77 MB | Fixed |
| Voice: recording buffer, results, reply buffer | ≈ 0.35 MB | Fixed |
| Reply buffers: library 64 KB, playback 16 KB | 80 KB | Fixed, sized from measured replies |
| Album list, 1,238 albums | ≈ 245 KB | 6,190 strings, 180 KB of text, plus heap overhead and the 30 KB album table. Loaded once a boot, never churned |
| Screens | built on first open, then kept | Nothing in the UI is ever deleted, so nothing churns |
| Screen shadow (screenshots) | 259 KB, **only once `frame` or `shots` is used** | It used to be allocated at every boot, and every flushed pixel copied into it |

## Floors

`mem_below_floor()` (memwatch.h) checks these every time the RAM sensor reports. A
breach is logged and posted as `below_floor: true` on `sensor.bathroom_dial_ram`.

| Heap | Floor (largest block) | Why |
|---|---|---|
| Internal | 2 KB | One Wi-Fi frame buffer is about 1.6 KB (IDF's Kconfig), and it has to come in one piece |
| PSRAM | 128 KB | A margin, not a derived figure: twice the largest reply buffer. Five hours of use never went below 510 KB |

## Five hours of use (6 Nov 2026)

`tools/mem_soak.py` on the bench, one boot, 300 minutes: album jumps, spins, every
screen, playlists, voice searches, rests long enough for the screen to sleep. About
740,000 decoder allocations in all, none of them internal. No restart, no complaint.

| | 10 min after boot | 5 hours | Lowest at any moment |
|---|---|---|---|
| Internal free / largest block | 52.7 KB / 40,960 | 52.6 KB / 40,960 | 7.8 KB, during Wi-Fi start only |
| Internal free pieces | 11 | 11 | |
| PSRAM free / largest block | 667 KB / 524 KB | 667 KB / 524 KB | 510 KB |
| PSRAM free pieces | 109 | 102 | |

Both heaps settle within minutes of boot and then hold flat. The PSRAM floor of
128 KB stays: the lowest point was 510 KB. On the rough figure of 200 bytes per album,
twice today's library fits with room left; three times would bring that low point
close to the floor.

No quiet restart was added as a backstop: nothing builds up, so there is nothing for
it to clear.

## Rules for anything new

1. **A static table goes in PSRAM** (`EXT_RAM_BSS_ATTR`, or `UI_IN_PSRAM` in `ui/`),
   unless DMA reads it, an interrupt touches it, or it's used while flash is being
   written.
2. **A task's stack goes in PSRAM** (`xTaskCreatePinnedToCoreWithCaps`), unless the
   task writes flash (NVS, OTA). That one needs internal RAM, or it asserts.
   A WithCaps task ends with `vTaskDeleteWithCaps`. Plain `vTaskDelete` leaves its
   stack and control block allocated for ever.
3. **Find out where a library allocates** before trusting it. The JPEG decoder
   quietly preferred internal RAM. `nm -u` on the library and a look at the caps it
   passes settles it in minutes.
4. **Size reply buffers from a measured reply**, and stream lists (`ma_each`) rather
   than parsing them whole.
5. **Run `memmap` before and after**, and update the tables above.

## How to look

- `memmap` on the USB console: the boot stages, every task (where its stack lives
  and the least headroom it has had), both heaps walked block by block, and the
  decoder's counters.
- `sensor.bathroom_dial_ram` in Home Assistant: internal largest block as the state.
  The attributes hold the internal free total and lowest ever, PSRAM largest block,
  free total and lowest ever, and `below_floor`.
- `tools/mem_soak.py`: hours of scripted use, one `memmap` row every ten minutes to a
  CSV. It never plays music, and it starts with `amps quiet` so the bathroom amps
  stay off however much it turns the knob.
