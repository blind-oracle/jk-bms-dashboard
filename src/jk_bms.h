// JK BMS polling task: UART1 transport, snapshot, discharge-switch command.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "jk_proto.h"

typedef struct {
    bool      has_data;     // at least one valid frame since boot/wake
    bool      offline;      // >= APP_BMS_OFFLINE_POLLS consecutive failed polls
    uint32_t  fail_count;   // consecutive failed polls
    int64_t   last_ok_us;   // esp_timer_get_time() of the last valid frame, 0 = never
    uint32_t  seq;          // incremented on every valid frame
    jk_data_t data;         // last valid frame (meaningful only if has_data)
} jk_snapshot_t;

// Configure UART1 (not in demo mode) and start the polling task.
// Posts APP_EV_BMS_DATA after every poll and APP_EV_DSG_RESULT after commands.
esp_err_t jk_bms_start(void);

// Copy the latest snapshot (thread-safe).
void jk_bms_get_snapshot(jk_snapshot_t *out);

// True while a discharge command is queued or running.
bool jk_bms_command_pending(void);

// Queue an absolute discharge-switch setting (idempotent). Returns false if a
// command is already pending or the task is stopped.
bool jk_bms_set_discharge(bool on);

// Stop the task: waits up to 1 s for an in-flight transaction, then parks it.
// The UART pins are left to power.c. Point of no return for deep sleep.
void jk_bms_stop(void);
