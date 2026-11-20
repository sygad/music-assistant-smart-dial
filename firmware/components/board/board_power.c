/* The battery reading.
 *
 * Waveshare's schematic (sheet 4) puts a 10K/10K divider across the 5 V rail
 * into GPIO1, which is ADC1 channel 0, so what the pin sees is half the rail.
 * The rail is the battery when it's running on its own and the charger when
 * it's plugged in, so the reading tells both apart by how high it is.
 */
#include "board.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "power";

#define BATT_CHANNEL ADC_CHANNEL_0  /* GPIO1 */
#define BATT_ATTEN   ADC_ATTEN_DB_12
#define SAMPLES      8  /* the rail wanders a few mV; the mean of a handful is steady */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

void board_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &s_adc));
    adc_oneshot_chan_cfg_t chan = {.atten = BATT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BATT_CHANNEL, &chan));

    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1, .chan = BATT_CHANNEL, .atten = BATT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        s_cali = NULL;  /* no calibration in efuse: the raw scale is close enough to show */
        ESP_LOGW(TAG, "no ADC calibration; the charge is an estimate");
    }
}

int board_battery_mv(void)
{
    if (!s_adc) return -1;
    int total = 0;
    for (int i = 0; i < SAMPLES; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, BATT_CHANNEL, &raw) != ESP_OK) return -1;
        if (s_cali) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
        } else {
            mv = raw * 3100 / 4095;  /* 12 dB attenuation reaches about 3.1 V */
        }
        total += mv;
    }
    return total / SAMPLES * 2;  /* the divider halves the rail */
}
