/* Screenshots over USB: prints a screen as base64 between
 *   SNAP <name> <width> <height>   and   SNAPEND
 * tools/snap_decode.py turns a captured log into PNGs.
 *
 * Both commands now send the same thing: the shadow copy of what the panel
 * was last given. "shots" used to take its own picture with lv_snapshot_take,
 * which allocates a buffer of its own - 360x360 at two bytes a pixel is
 * 253 KB, and neither internal RAM nor the PSRAM left over from the cover
 * pool has that much in one piece, so every "shots" since the screen went
 * full size failed with nothing in the log but "snapshot failed" (found by
 * another session, 1 Nov 2026). The shadow is already there and costs
 * nothing, it is what the panel actually received rather than a second
 * rendering of it, and it already carries the rim and the arc that the
 * snapshot had to paint on afterwards. */
#include <stdio.h>

#include "esp_attr.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "ui.h"

static const char *TAG = "snap";

/* What was actually sent to the panel (needs board_display_shadow_enable). */
void screenshot_dump_raw(const char *name, const uint8_t *frame, int w, int h)
{
    /* Say what was on the glass. The clock is a screensaver over everything,
     * so a capture taken under it is a picture of the time whatever screen the
     * UI thinks is open - which is what made four captures look like a lie
     * (2 Nov 2026). Both commands come through here, so both say it. */
    ESP_LOGI(TAG, "SNAPSTATE %s view=%d clock=%s asleep=%s scrubber=%s", name, (int)ui_view(),
             ui_clock_up() ? "up" : "down", ui_asleep() ? "yes" : "no", ui_scrubbing() ? "open" : "closed");
    printf("SNAP %s %d %d\n", name, w, h);
    EXT_RAM_BSS_ATTR static unsigned char out[1100];  /* a console command: not internal RAM */
    for (int y = 0; y < h; y++) {
        size_t olen = 0;
        mbedtls_base64_encode(out, sizeof out, &olen, frame + (size_t)y * w * 2, (size_t)w * 2);
        out[olen] = 0;
        printf("%s\n", out);
        if (y % 30 == 29) vTaskDelay(1);  /* let the idle task run: this takes seconds */
    }
    printf("SNAPEND\n");
}

void screenshot_dump(const char *name)
{
    const uint8_t *frame = board_display_shadow();
    if (!frame) {
        ESP_LOGE(TAG, "no shadow frame: board_display_shadow_enable was never called");
        return;
    }
    screenshot_dump_raw(name, frame, BOARD_LCD_W, BOARD_LCD_H);
}
