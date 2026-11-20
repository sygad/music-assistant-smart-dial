/* See crash.h. The hook is esp_panic_handler wrapped at link time
 * (-Wl,--wrap, in CMakeLists.txt), which IDF supports for exactly this: it runs
 * first on every panic - an exception, an abort or failed assert, a stack
 * overflow, the interrupt watchdog - records what it can, then hands over to
 * IDF's own handler, which prints and restarts exactly as before.
 *
 * It runs inside the panic, so it only copies: no allocation, no printf, no
 * locks. A brownout or the task watchdog never reaches it (the first is a reset
 * of its own, the second only prints); those still show as the reset reason. */
#include "crash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa_context.h"

#define CRASH_MAGIC 0xC4A5D1A1u
#define CRASH_PCS 20

typedef struct {
    uint32_t magic;
    char reason[120];
    char task[16];
    uint32_t pc[CRASH_PCS];
    int npc;
    bool corrupted;
} crash_t;

/* NOINIT: kept through a panic restart, garbage at power-on, hence the magic */
RTC_NOINIT_ATTR static crash_t s_crash;

extern bool g_panic_abort;           /* esp_system/panic.c: an abort() or a failed assert */
extern char *g_panic_abort_details;  /* ...and its message */

static void copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    s_crash.magic = 0;  /* half-written must not read as written */
    copy(s_crash.reason, sizeof s_crash.reason,
         g_panic_abort && g_panic_abort_details ? g_panic_abort_details
         : info->reason                         ? info->reason
                                                : "unknown");
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(info->core);
    copy(s_crash.task, sizeof s_crash.task, t ? pcTaskGetName(t) : "(none)");
    s_crash.npc = 0;
    s_crash.corrupted = false;
    const XtExcFrame *f = info->frame;
    if (f) {
        /* as IDF's own backtrace print walks it (debug_helpers.c) */
        esp_backtrace_frame_t fr = {.pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f};
        s_crash.pc[s_crash.npc++] = esp_cpu_process_stack_pc(fr.pc);
        while (s_crash.npc < CRASH_PCS && fr.next_pc) {
            if (!esp_stack_ptr_is_sane(fr.sp) || !esp_backtrace_get_next_frame(&fr)) {
                s_crash.corrupted = true;
                break;
            }
            s_crash.pc[s_crash.npc++] = esp_cpu_process_stack_pc(fr.pc);
        }
    }
    s_crash.magic = CRASH_MAGIC;
    __real_esp_panic_handler(info);
}

const char *crash_reset_name(void)
{
    static const char *const WHY[] = {"unknown", "power on", "external", "software", "panic",
                                      "interrupt watchdog", "task watchdog", "other watchdog",
                                      "deep sleep", "brownout", "sdio", "usb", "jtag"};
    int why = (int)esp_reset_reason();
    return why >= 0 && why < (int)(sizeof WHY / sizeof *WHY) ? WHY[why] : "?";
}

bool crash_take(char *reason, size_t reason_n, char *task, size_t task_n, char *pcs, size_t pcs_n)
{
    if (s_crash.magic != CRASH_MAGIC) return false;
    s_crash.magic = 0;
    s_crash.reason[sizeof s_crash.reason - 1] = 0;
    s_crash.task[sizeof s_crash.task - 1] = 0;
    copy(reason, reason_n, s_crash.reason);
    copy(task, task_n, s_crash.task);
    size_t at = 0;
    pcs[0] = 0;
    for (int i = 0; i < s_crash.npc && i < CRASH_PCS && at + 12 < pcs_n; i++)
        at += snprintf(pcs + at, pcs_n - at, "%s0x%08lx", i ? " " : "", (unsigned long)s_crash.pc[i]);
    if (s_crash.corrupted && at + 12 < pcs_n) snprintf(pcs + at, pcs_n - at, " |corrupted");
    return true;
}
