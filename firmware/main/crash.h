/* What the last restart was, and if the dial fell over, where.
 *
 * The dial is on the bathroom wall with no cable, so a crash prints its
 * backtrace to a console nobody is reading and the restart wipes it. Voice
 * search restarted the dial twice on 6 Nov 2026 and there was nothing at all to
 * say why. The panic handler now keeps the reason, the task and the call chain
 * in memory that survives the restart, and the next boot hands them to Home
 * Assistant (ha_report_restart). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* "power on", "panic", "brownout"... for the reset that started this boot */
const char *crash_reset_name(void);

/* True once per crash: what it was ("assert failed: ..."), the task, and the
 * call chain as "0x4200abcd 0x4037..." for addr2line against the build's ELF.
 * Consumed by the call, so a later soft restart does not report it again. */
bool crash_take(char *reason, size_t reason_n, char *task, size_t task_n, char *pcs, size_t pcs_n);
