// Board battery voltage: ADC1 oneshot at 12 dB with curve-fitting calibration
// (falls back to an uncalibrated linear conversion if the eFuse has no data).
#include "battery.h"

#include <stdbool.h>
#include <stdint.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"
#include "app_config.h"

static const char *TAG = "battery";

#define ADC_ATTEN           ADC_ATTEN_DB_12
#define UNCAL_FULL_SCALE_MV 3300
#define UNCAL_MAX_RAW       4095

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;        // NULL: uncalibrated fallback
static adc_channel_t s_chan;

esp_err_t battery_init(void)
{
    if (s_adc != NULL) {
        return ESP_OK;
    }

    adc_unit_t unit;
    adc_channel_t chan;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(APP_PIN_VBAT_ADC, &unit, &chan), TAG,
                        "GPIO%d is not an ADC pin", APP_PIN_VBAT_ADC);

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_unit_handle_t adc = NULL;
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &adc), TAG, "ADC unit init failed");

    // Also switches the pad to analog (input and pulls off): the divider must not be loaded
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(adc, chan, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(adc);
        return err;
    }

    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = chan,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (err != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration (%s), using raw * %d / %d", esp_err_to_name(err),
                 UNCAL_FULL_SCALE_MV, UNCAL_MAX_RAW);
    }

    s_chan = chan;
    s_adc = adc;
    ESP_LOGI(TAG, "GPIO%d = ADC%d channel %d, %s", APP_PIN_VBAT_ADC, (int)unit + 1, (int)chan,
             s_cali != NULL ? "calibrated" : "uncalibrated");
    return ESP_OK;
}

int battery_read_mv(void)
{
    if (s_adc == NULL) {
        return -1;
    }

    // Short burst; samples that fail to convert are skipped
    int32_t sum_mv = 0;
    int32_t n = 0;
    for (int i = 0; i < APP_VBAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (s_cali != NULL) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
                continue;
            }
        } else {
            mv = raw * UNCAL_FULL_SCALE_MV / UNCAL_MAX_RAW;
        }
        sum_mv += mv;
        n++;
    }
    if (n == 0) {
        ESP_LOGW(TAG, "ADC read failed");
        return -1;
    }

    const int32_t pin_mv = (sum_mv + n / 2) / n;
    const int32_t vbat_mv = (pin_mv * APP_VBAT_DIVIDER_X1000 + 500) / 1000 + APP_VBAT_OFFSET_MV;
    return vbat_mv > 0 ? (int)vbat_mv : 0;
}
