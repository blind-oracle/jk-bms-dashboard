// Application event queue: the button module and the BMS task post, main.c receives.
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_events.h"

#define APP_EVENT_QUEUE_LEN 16

static const char *TAG = "events";

static QueueHandle_t s_queue;

void app_events_init(void)
{
    if (s_queue != NULL) {
        return;
    }
    s_queue = xQueueCreate(APP_EVENT_QUEUE_LEN, sizeof(app_event_t));
    if (s_queue == NULL) {
        // Nothing works without the queue. The reset counts towards the boot-loop guard.
        ESP_LOGE(TAG, "Cannot create the event queue");
        abort();
    }
}

bool app_post_event(const app_event_t *ev)
{
    if (s_queue == NULL || ev == NULL) {
        return false;
    }
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, event %d dropped", (int)ev->type);
        return false;
    }
    return true;
}

bool app_post_simple(app_event_type_t type)
{
    const app_event_t ev = { .type = type };
    return app_post_event(&ev);
}

bool app_wait_event(app_event_t *ev, uint32_t timeout_ms)
{
    if (s_queue == NULL || ev == NULL) {
        // Keep the caller's loop paced even if used before app_events_init()
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }
    return xQueueReceive(s_queue, ev, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
