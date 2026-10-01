// Board Li-ion voltage via the FireBeetle's 1M/1M divider on GPIO0 (ADC1_CH0).
#pragma once

#include "esp_err.h"

esp_err_t battery_init(void);
// Average APP_VBAT_SAMPLES conversions and return the battery voltage in mV
// (divider ratio and offset applied), or -1 on error. Blocks for a few ms.
int battery_read_mv(void);
