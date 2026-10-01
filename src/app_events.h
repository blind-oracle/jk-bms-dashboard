// Events posted to the application loop (main.c) by the button module and the BMS task.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "jk_proto.h"

typedef enum {
    APP_EV_BTN_DOWN = 0,     // debounced press started (activity)
    APP_EV_BTN_SHORT,        // released before HOLD_START: next page
    APP_EV_BTN_HOLD_START,   // held HOLD_START ms: show the output-toggle card
    APP_EV_BTN_CANCEL,       // released between HOLD_START and LONG, or held past the arm window
    APP_EV_BTN_ARMED,        // held LONG ms: releasing now executes the toggle
    APP_EV_BTN_LONG_RELEASE, // released inside the arm window: execute the toggle
    APP_EV_BTN_STUCK,        // the wake press has been held for APP_BTN_STUCK_MS
    APP_EV_BMS_DATA,         // the BMS snapshot changed (poll succeeded or failed)
    APP_EV_DSG_RESULT,       // result of jk_bms_set_discharge()
} app_event_type_t;

typedef enum {
    APP_DSG_RESULT_OK = 0,        // confirmed by a frame read after the write
    APP_DSG_RESULT_NO_REPLY,      // no write ack and no frame after the write
    APP_DSG_RESULT_NOT_CONFIRMED, // frames were read back but do not match the target
    APP_DSG_RESULT_UNKNOWN,       // the write was acked but nothing could be read back
} app_dsg_result_t;

typedef struct {
    app_event_type_t type;
    // APP_EV_DSG_RESULT only
    app_dsg_result_t dsg_result;
    bool             dsg_target;  // requested setting
    jk_dsg_state_t   dsg_state;   // state read back after the write (OK / NOT_CONFIRMED only)
} app_event_t;

// Create the queue (call once, before starting the producers)
void app_events_init(void);
// Post without blocking from task context (not from an ISR). Returns false if the queue is full.
bool app_post_event(const app_event_t *ev);
// Convenience for events without payload
bool app_post_simple(app_event_type_t type);
// Receive with a timeout in ms. Returns false on timeout.
bool app_wait_event(app_event_t *ev, uint32_t timeout_ms);
