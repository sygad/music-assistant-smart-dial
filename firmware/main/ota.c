/* Updating the knob over Wi-Fi: see ota.h for why this exists at all.
 *
 * The shape of it: Home Assistant holds a URL in a helper and a switch that
 * asks for an update. ha.c polls both, and hands the URL here. This reads the
 * file straight into the flash slot that isn't running, checks it really is
 * this knob's firmware before trusting a byte of it, points the bootloader at
 * it and restarts.
 *
 * One rule runs through all of this and is not obvious: every esp_ota_ call
 * has to happen on a task whose stack is in internal RAM. Reading which slot
 * the bootloader will start means memory-mapping flash, and writing a slot
 * means erasing it, and both freeze the processor's cache - at which point a
 * stack that lives in PSRAM cannot be reached and the firmware asserts and
 * restarts. Nearly every task in this build is deliberately given a PSRAM
 * stack to keep internal RAM for Wi-Fi, so the two places that touch the OTA
 * machinery get internal stacks of their own on purpose, and the download
 * buffer is internal for the same reason. Found the hard way on 2 Nov 2026:
 * the first flash crash-looped, asserting in esp_cache_freeze_caches on the
 * network task the moment Wi-Fi came up.
 *
 * What makes it safe to do to something screwed to a wall is the rollback. A
 * slot written by an update boots as "pending verify", and the bootloader puts
 * the previous firmware back on the next restart unless the new one says it is
 * well. What counts as well is deliberately strict: Wi-Fi up and Home
 * Assistant answering (ota_validate, called from app_main's net_up). That is
 * exactly the ability to be updated again, so a build that loses it is a build
 * that would cost a cable - and it rolls back instead. A build that crashes or
 * hangs before getting there is caught the same way, by the clock below. */
#include "ota.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "secrets_gen.h"
#include "ui.h"

static const char *TAG = "ota";

/* How long a freshly updated firmware has to get Wi-Fi up before the
 * bootloader puts the old one back. Five minutes, not thirty seconds: a router
 * still coming up after a power cut must not be mistaken for a bad build. The
 * cost of waiting is only that a genuinely broken build sits there a few
 * minutes before fixing itself. */
#define OTA_VALIDATE_MS 300000

/* The download. 4 KB a read is a good trade between HTTP round trips and the
 * flash write size. */
#define OTA_CHUNK 4096

/* The update task's stack. In ESP-IDF a stack depth is a byte count and
 * StackType_t is a byte, so this is 8 KB either way you read it. */
#define OTA_STACK_BYTES 8192
/* Long, because this is one file over a home network and a stall part way
 * through means a half-written slot. */
#define OTA_TIMEOUT_MS 20000
/* How many times to start the whole download again. The knob sits between two
 * access points and a roam costs three and a half seconds with no network at
 * all, which is enough to kill a transfer; roaming is held off while an update
 * runs (app_main), but a weak link can still drop on its own. Three goes with
 * a breath between them turns "unlucky" into "fine". */
#define OTA_TRIES    3
#define OTA_RETRY_MS 3000
/* Enough of the front of the image to hold the headers and the app
 * description, which is where the project name lives. */
#define OTA_DESC_BYTES (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static _Atomic int s_state = OTA_IDLE;
static _Atomic int s_percent = -1;
static _Atomic int s_fail = OTA_FAIL_NONE;
static _Atomic bool s_busy;
static _Atomic bool s_validated;
static esp_timer_handle_t s_rollback_timer;
static char s_url[256];  /* written before the task starts, read only by it */

const char *ota_fail_text(ota_fail_t why)
{
    switch (why) {
    case OTA_FAIL_NONE: return "";
    case OTA_FAIL_NO_URL: return "no firmware address set";
    case OTA_FAIL_HTTP: return "could not fetch the firmware";
    case OTA_FAIL_NOT_KNOB: return "that file is not the knob's firmware";
    case OTA_FAIL_SHORT: return "the download stopped early";
    case OTA_FAIL_FLASH: return "writing it failed";
    case OTA_FAIL_NO_RAM: return "no memory to download into";
    case OTA_FAIL_BEGIN: return "the spare slot would not open";
    case OTA_FAIL_BOOT: return "could not switch to the new firmware";
    case OTA_FAIL_NO_TASK: return "could not start the update";
    case OTA_FAIL_WEDGED: return "the last attempt never finished";
    }
    return "";
}

ota_state_t ota_state(int *percent, ota_fail_t *why)
{
    if (percent) *percent = atomic_load(&s_percent);
    if (why) *why = (ota_fail_t)atomic_load(&s_fail);
    return (ota_state_t)atomic_load(&s_state);
}

bool ota_busy(void) { return atomic_load(&s_busy); }

/* Every attempt numbered (6 Nov 2026).
 *
 * The morning of 6 Nov went on an update that failed instantly and reported
 * the same words as the attempt before it. Home Assistant records nothing when
 * a state is re-posted unchanged, so nothing new reached it, and tools/fw.sh -
 * which deliberately ignores a result identical to the one already there, so
 * as not to read a stale failure as this attempt's - sat for five minutes
 * learning nothing at all while the dial sat there with the answer on its
 * screen. Both ends were behaving correctly and the pair of them said nothing.
 *
 * So the attempt is counted and the count goes out with the reason. Two
 * identical failures in a row are now two different sensor states. */
static unsigned s_attempt;

static void fail(ota_fail_t why)
{
    atomic_store(&s_fail, why);
    atomic_store(&s_percent, -1);
    atomic_store(&s_state, OTA_FAILED);
    ESP_LOGE(TAG, "update attempt %u failed: %s (internal RAM free %u, largest block %u)", s_attempt,
             ota_fail_text(why), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

unsigned ota_attempt(void) { return s_attempt; }

/* ---- Rollback: proving a new firmware is worth keeping ------------------------ */

static bool pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

/* The new firmware never got as far as Wi-Fi. Restarting is what hands it
 * back: the bootloader sees a slot that was never marked good and starts the
 * previous one instead. */
static void rollback_now(void *arg)
{
    (void)arg;
    if (!pending_verify()) return;
    ESP_LOGE(TAG, "this firmware has not reached Wi-Fi in %d s: restarting into the previous one",
             OTA_VALIDATE_MS / 1000);
    esp_restart();
}

void ota_boot_check(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running from %s", running ? running->label : "?");
    if (!pending_verify()) return;
    /* An esp_timer rather than a task: it fires even if every task is wedged,
     * which is one of the ways a build can be bad. */
    const esp_timer_create_args_t args = {.callback = rollback_now, .name = "ota_rollback"};
    if (esp_timer_create(&args, &s_rollback_timer) != ESP_OK) return;
    esp_timer_start_once(s_rollback_timer, (uint64_t)OTA_VALIDATE_MS * 1000);
    ESP_LOGW(TAG, "this firmware is on trial: Wi-Fi within %d s or the previous one comes back",
             OTA_VALIDATE_MS / 1000);
}

static void validate_task(void *arg)
{
    (void)arg;
    if (s_rollback_timer) {
        esp_timer_stop(s_rollback_timer);
        esp_timer_delete(s_rollback_timer);
        s_rollback_timer = NULL;
    }
    if (pending_verify() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGI(TAG, "this firmware has Wi-Fi and is now the one that stays");
    vTaskDelete(NULL);
}

void ota_validate(void)
{
    if (atomic_exchange(&s_validated, true)) return;  /* net_up can run more than once */
    /* Not on the caller's task: this is called from the network task, whose
     * stack is in PSRAM, and reading otadata freezes the cache. A small task
     * with an ordinary internal stack, for the half-second it takes. */
    xTaskCreatePinnedToCore(validate_task, "ota_ok", 3072, NULL, 3, NULL, 0);
}

/* ---- The update itself -------------------------------------------------------- */

/* Is this really the knob's firmware? Checked before the first write is
 * trusted, because the one thing worse than a failed update is a successful
 * one that installs the wrong image: the magic byte every ESP app starts with,
 * and the project name the build puts in its description. */
static bool looks_like_our_firmware(const uint8_t *front, int len)
{
    if (len < (int)OTA_DESC_BYTES) return false;
    if (front[0] != ESP_IMAGE_HEADER_MAGIC) return false;
    const esp_app_desc_t *desc =
        (const esp_app_desc_t *)(front + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    const esp_app_desc_t *mine = esp_app_get_description();
    if (strncmp(desc->project_name, mine->project_name, sizeof desc->project_name) != 0) return false;
    ESP_LOGI(TAG, "incoming firmware is %s %.32s (running %s)", desc->project_name, desc->version, mine->version);
    return true;
}

/* One go at fetching and writing the firmware. Returns OTA_FAIL_NONE having
 * pointed the bootloader at the new slot, or the reason it could not.
 *
 * Split out from the task below so it can simply be tried again: a 2.9 MB
 * transfer takes about fifteen seconds and the knob is at the far end of the
 * house, between two access points, at -54 dBm on a good day and -84 on a bad
 * one. The first real update over Wi-Fi died at 71% when the link dropped for
 * a few seconds (2 Nov 2026), and a mechanism that cannot survive that is not
 * a mechanism Darren can rely on from the other side of a wall. */
static ota_fail_t download_once(const esp_partition_t *slot, uint8_t *buf)
{
    esp_ota_handle_t handle = 0;
    bool open = false, checked = false;
    int written = 0, total = 0;
    ota_fail_t why = OTA_FAIL_NONE;

    atomic_store(&s_state, OTA_CONNECTING);
    atomic_store(&s_percent, -1);

    esp_http_client_config_t cfg = {.url = s_url, .method = HTTP_METHOD_GET, .timeout_ms = OTA_TIMEOUT_MS,
                                    .buffer_size_tx = 2048};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return OTA_FAIL_HTTP;
    /* Home Assistant serves /local without a token, but sending one costs
     * nothing and means the file can be moved somewhere that wants it without
     * a new firmware to carry the header. */
    esp_http_client_set_header(c, "Authorization", "Bearer " SECRET_HA_TOKEN);
    if (esp_http_client_open(c, 0) != ESP_OK || esp_http_client_fetch_headers(c) < 0 ||
        esp_http_client_get_status_code(c) != 200) {
        ESP_LOGE(TAG, "HTTP %d for %s", esp_http_client_get_status_code(c), s_url);
        esp_http_client_cleanup(c);
        return OTA_FAIL_HTTP;
    }
    total = esp_http_client_get_content_length(c);
    ESP_LOGI(TAG, "firmware is %d bytes", total);

    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return OTA_FAIL_BEGIN;
    }
    open = true;
    atomic_store(&s_state, OTA_DOWNLOADING);
    atomic_store(&s_percent, 0);

    for (;;) {
        int n = esp_http_client_read(c, (char *)buf, OTA_CHUNK);
        if (n < 0) { why = OTA_FAIL_SHORT; goto out; }
        if (n == 0) break;
        /* The first read carries the headers, so it is the one that gets to
         * say whether any of this is worth writing. */
        if (!checked) {
            if (!looks_like_our_firmware(buf, n)) { why = OTA_FAIL_NOT_KNOB; goto out; }
            checked = true;
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write at %d bytes: %s", written, esp_err_to_name(err));
            why = OTA_FAIL_FLASH;
            goto out;
        }
        written += n;
        if (total > 0) atomic_store(&s_percent, written >= total ? 100 : (int)((int64_t)written * 100 / total));
    }

    /* A download that stopped early would otherwise be written as a short
     * image and fail its own verification at boot - which rollback would
     * recover from, but there is no reason to spend a reboot finding out. */
    if (!checked || written == 0 || (total > 0 && written != total)) {
        ESP_LOGE(TAG, "got %d of %d bytes", written, total);
        why = OTA_FAIL_SHORT;
        goto out;
    }

    atomic_store(&s_state, OTA_INSTALLING);
    atomic_store(&s_percent, 100);
    if (esp_ota_end(handle) != ESP_OK) {  /* this is where a corrupt image is caught */
        open = false;
        why = OTA_FAIL_NOT_KNOB;
        goto out;
    }
    open = false;
    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        why = OTA_FAIL_BOOT;
    }

out:
    if (open) esp_ota_abort(handle);
    esp_http_client_cleanup(c);
    return why;
}

/* Light sleep must not happen in the middle of this (4 Nov 2026).
 *
 * Light sleep went on this morning to stop the dial flattening in five hours,
 * and the very next update to the dial on the wall failed in the flash write.
 * It is the only thing that changed between an update that worked and one that
 * did not, and the mechanism is plain enough: the processor stopping itself
 * part way through erasing and writing a flash slot, on a task whose whole
 * reason for having an internal stack is that the cache is frozen while that
 * happens. The screen being on already holds off the *screen* sleep while an
 * update runs (ui_updating in ui.c); nothing held off this one, and on the
 * wall the dial is asleep, which is precisely when an unattended update lands.
 *
 * So an update takes the lock for as long as it runs, and gives it back
 * whatever the outcome. Also the CPU at full speed: this is 2.9 MB over Wi-Fi
 * and the sooner it is done the sooner the dial can go back to sleep. */
static esp_pm_lock_handle_t s_awake_lock, s_fast_lock;

#define OTA_WEDGED_MS 600000
static int64_t s_began_at;

static void stay_awake(bool yes)
{
    if (!s_awake_lock && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "otaawake", &s_awake_lock) != ESP_OK)
        s_awake_lock = NULL;
    if (!s_fast_lock && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "otafast", &s_fast_lock) != ESP_OK)
        s_fast_lock = NULL;
    if (yes) {
        if (s_awake_lock) esp_pm_lock_acquire(s_awake_lock);
        if (s_fast_lock) esp_pm_lock_acquire(s_fast_lock);
    } else {
        if (s_fast_lock) esp_pm_lock_release(s_fast_lock);
        if (s_awake_lock) esp_pm_lock_release(s_awake_lock);
    }
}

/* THE UPDATE PATH OWNS ITS MEMORY, AND OWNS IT AT LINK TIME (6 Nov 2026).
 *
 * This is why updates kept failing after the dial had been up for a while and
 * worked on a fresh boot - the thing recorded on 4 Nov as "a dial running for
 * hours may refuse an update a fresh boot takes", with no cause attached.
 *
 * The cause is that both pieces of memory the update needs were asked for at
 * the moment of the update, from internal RAM, and an 8 KB task stack has to
 * be ONE CONTIGUOUS BLOCK of it.
 *
 * It was first put down to fragmentation building up over a long run, because
 * 4 Nov recorded an update failing twice and then being taken by a dial that
 * had just been power cycled. That was wrong, and the dial itself said so the
 * moment it was asked over the cable (6 Nov 2026):
 *
 *     MEM internal free 4647 (largest 2560, lowest ever 228)
 *
 * There is no 8 KB. There is no 4 KB. The largest contiguous block of internal
 * RAM this build ever has is about 2.5 KB, and at its worst it has been down to
 * 228 bytes - and that reading is from a dial fifteen minutes into a fresh
 * boot. So the update task could not be created at ANY point in the dial's
 * life, on any boot, however clean the heap: the memory was not there to find.
 * Nothing accumulated and nothing fragmented. The build simply grew past what
 * it had, and 4 Nov's power cycle was a coincidence that cost two mornings.
 *
 * Which is also why reserving it is the only fix, rather than one of several:
 * there is no runtime moment at which asking would have worked. .bss is taken
 * before the heap exists, so this 12 KB is the one 12 KB that cannot be
 * competed for.
 *
 * And when xTaskCreate then failed, ota_begin reported OTA_FAIL_FLASH - the
 * write failed - for an update that had not been started, let alone written a
 * byte. Which is why the answer looked impossible: the dial said the flash
 * write failed, and the flash was never touched.
 *
 * So the stack and the download buffer are both reserved here, in .bss, which
 * is internal RAM. They cost 12 KB that the dial no longer has for anything
 * else, which is the right trade for a device screwed to a wall: it buys an
 * update path that cannot be starved by whatever the dial has been doing, and
 * it is the same 12 KB that was being asked for at the worst possible moment
 * anyway. Nothing can now fail here for want of memory, and the two reasons
 * that could are kept only so that a future change cannot report them wrong. */
static DRAM_ATTR StackType_t s_ota_stack[OTA_STACK_BYTES];
static DRAM_ATTR StaticTask_t s_ota_tcb;
static DRAM_ATTR uint8_t s_ota_buf[OTA_CHUNK];

static void ota_task(void *arg)
{
    (void)arg;
    stay_awake(true);
    uint8_t *buf = s_ota_buf;
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    ota_fail_t why = OTA_FAIL_BEGIN;
    if (!slot) ESP_LOGE(TAG, "no spare slot to write into");

    if (slot) {
        ESP_LOGI(TAG, "updating from %s into %s", s_url, slot->label);
        for (int attempt = 1; attempt <= OTA_TRIES; attempt++) {
            why = download_once(slot, buf);
            if (why == OTA_FAIL_NONE) break;
            /* A file that is not our firmware will not become ours by asking
             * again; a dropped link might. */
            if (why == OTA_FAIL_NOT_KNOB) break;
            if (attempt < OTA_TRIES) {
                ESP_LOGW(TAG, "attempt %d failed (%s): starting again", attempt, ota_fail_text(why));
                atomic_store(&s_state, OTA_CONNECTING);
                vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_MS));
            }
        }
    }

    if (why != OTA_FAIL_NONE) {
        fail(why);
        stay_awake(false);  /* it can go back to sleep: there is nothing to protect */
        atomic_store(&s_busy, false);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "written and verified: restarting into %s", slot->label);
    /* The lock is not released here on purpose: nothing may stop the processor
     * between the last flash write and the restart, and the restart takes it
     * away anyway. */
    /* Long enough for "Restarting" to be read on the way out. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void ota_begin(const char *url)
{
    if (!url || !url[0]) {
        fail(OTA_FAIL_NO_URL);
        return;
    }
    /* "Already updating" has to have a time limit on it. An attempt that got
     * as far as taking the flag and then died - the task never created, a
     * crash inside it - would otherwise refuse every later attempt forever,
     * silently, with nothing to say why and no way in from the outside but a
     * cable. An update that has not finished in OTA_WEDGED_MS is not running:
     * 2.9 MB over the worst link this dial sees is a couple of minutes, so ten
     * is generous and still finite (6 Nov 2026). */
    bool was = atomic_exchange(&s_busy, true);
    if (was) {
        int64_t since = esp_timer_get_time() - s_began_at;
        if (since < OTA_WEDGED_MS * 1000LL) {
            ESP_LOGW(TAG, "already updating: ignoring %s", url);
            return;
        }
        ESP_LOGE(TAG, "an update has been busy for %lld s and is not coming back: starting again",
                 since / 1000000);
        s_attempt++;
        fail(OTA_FAIL_WEDGED);
        /* and on through: the flag is already ours. */
    }
    s_began_at = esp_timer_get_time();
    s_attempt++;
    snprintf(s_url, sizeof s_url, "%s", url);
    atomic_store(&s_fail, OTA_FAIL_NONE);
    atomic_store(&s_percent, -1);
    atomic_store(&s_state, OTA_CONNECTING);
    /* Its own task, on the core that does the network, so drawing the progress
     * is never held up by a slow read - and with an internal stack, because
     * writing flash freezes the cache (see the note at the top of the file).
     * It only exists while an update is running, so the internal RAM it costs
     * is not held against Wi-Fi for the rest of the time. */
    /* A static stack, so this cannot fail for want of a contiguous block and
     * cannot be refused by a heap that hours of cover art have broken up. The
     * check stays, with the reason it actually means. */
    if (!xTaskCreateStaticPinnedToCore(ota_task, "ota", OTA_STACK_BYTES, NULL, 4, s_ota_stack, &s_ota_tcb, 0)) {
        atomic_store(&s_busy, false);
        fail(OTA_FAIL_NO_TASK);
    }
}
