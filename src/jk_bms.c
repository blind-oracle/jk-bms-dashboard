// JK BMS polling task: UART1 transport, fixed-rate read-all polls, the shared
// snapshot, the discharge-switch command and the demo-mode BMS simulator.
//
// Every transaction takes the same path: link_begin() drops stale input and
// resets the assembler, jk_tx() keeps >= 100 ms between packets in either
// direction, and rx_wait() feeds 20 ms read chunks to the assembler until the
// expected frame arrives or the deadline passes. In demo mode the link is a
// simulated BMS that answers read-all requests with frames built by
// jk_encode_read_all_reply(), so demo data goes through the same assembler and
// parser as real data.
#include "jk_bms.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "app_events.h"
#include "jk_proto.h"

static const char *TAG = "jk_bms";

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// ---- Link and protocol timing ----
#define JK_UART             UART_NUM_1
#define JK_BAUD             115200
#define JK_UART_RX_BUF      1024        // driver ring buffer; must exceed the 128-byte HW FIFO
#define JK_TX_DONE_MS       50          // a 22-byte request takes about 2 ms
#define JK_GAP_US           100000      // >= 100 ms between packets (JK protocol, 4.1)
#define JK_RX_CHUNK_MS      20
#define JK_ACK_TIMEOUT_MS   500
#define JK_CMD_ATTEMPTS     2           // password + write + confirmation polls, retried once
#define JK_CONFIRM_POLLS    2
#define JK_TRIP_RECHECK_MS  300         // enabling showed TRIP: let the MOSFET follow before re-polling
#define JK_REQ_MAX          32          // requests are 21-22 bytes

#define POLL_PERIOD_US      ((int64_t)APP_POLL_INTERVAL_MS * 1000)
#define RX_TIMEOUT_US       ((int64_t)APP_BMS_RX_TIMEOUT_MS * 1000)

// ---- Task ----
#define JK_TASK_STACK       4096        // bytes: IDF's StackType_t is uint8_t
#define JK_TASK_PRIO        5
#define JK_STOP_WAIT_MS     1000
#define JK_RESULT_TRIES     10          // app queue full: retry posting the command result
#define JK_RESULT_RETRY_MS  50

// ---- Shared with the API functions, guarded by s_lock ----
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static jk_snapshot_t     s_snap;
static bool              s_cmd_pending;     // queued or running
static bool              s_cmd_target;
static bool              s_stop_req;
static bool              s_parked;

static SemaphoreHandle_t s_parked_sem;      // given by the task when it parks
static StaticSemaphore_t s_parked_sem_buf;
static TaskHandle_t      s_task;
static StaticTask_t      s_task_buf;
static StackType_t       s_task_stack[JK_TASK_STACK];

// ---- Task-private ----
typedef enum {
    LINK_UNKNOWN = 0,
    LINK_ONLINE,
    LINK_OFFLINE,
} link_state_t;

static jk_rx_t      s_rx;
static uint8_t      s_frame[JK_FRAME_MAX];
static uint8_t      s_chunk[128];
static jk_data_t    s_parsed;
static int64_t      s_last_io_us;           // last byte sent or received
static size_t       s_rx_bytes;             // bytes received since link_begin()
static link_state_t s_link;
static bool         s_activation_armed = true;

// Parse problems already logged: each distinct one is logged once
typedef struct {
    jk_err_t err;
    bool     clean_end;
    uint8_t  stop_id;
    uint16_t stop_offset;
} parse_problem_t;

#define PROBLEM_SLOTS 4
static parse_problem_t s_problems[PROBLEM_SLOTS];
static unsigned        s_problem_count;
static unsigned        s_problem_next;
static bool            s_warned_cells_clamped;
static bool            s_warned_garbage;

// ---- Demo mode: a simulated BMS behind the link ----
#define DEMO_CONNECT_US     2000000LL   // first data after about 2 s ("Connecting...")
#define DEMO_SCENARIO_US    10000000LL  // each scenario lasts 10 s
#define DEMO_REPLY_US       30000       // simulated reply latency
#define DEMO_CMD_DELAY_MS   600         // simulated password + write + ack
#define DEMO_LINK_LOSS      3           // scenario d: the BMS does not answer
#define DEMO_SCENARIOS      4

typedef enum {
    DEMO_OUT_OK = 0,        // the discharge switch and MOSFET follow the target
    DEMO_OUT_NO_REPLY,      // nothing changes
    DEMO_OUT_BLOCKED,       // the switch follows the target, the MOSFET stays off
    DEMO_OUTCOMES,
} demo_outcome_t;

static const char *const DEMO_OUTCOME_NAMES[DEMO_OUTCOMES] = {
    [DEMO_OUT_OK]       = "ok",
    [DEMO_OUT_NO_REPLY] = "no reply",
    [DEMO_OUT_BLOCKED]  = "blocked by a protection",
};

typedef struct {
    const char     *name;
    const uint16_t *cell_mv;
    uint8_t         cells;
    uint16_t        voltage_10mv;
    int32_t         current_10ma;   // + charging
    uint8_t         soc_pct;
    int16_t         temp_mos_c;
    int16_t         temp_1_c;
    int16_t         temp_2_c;
    uint16_t        cycles;
    uint32_t        cycle_ah;
    uint32_t        capacity_ah;
    uint32_t        actual_ah;
    uint16_t        alarms;
    uint16_t        status;
} demo_scenario_t;

static const uint16_t DEMO_CELLS_A[] = {
    3301, 3298, 3304, 3296, 3300, 3281, 3302, 3299,
    3305, 3297, 3303, 3312, 3300, 3298, 3302, 3301,
};
static const uint16_t DEMO_CELLS_B[] = {
    4152, 4160, 4148, 4171, 4155, 4163, 4140, 4158,
    4166, 4150, 4175, 4157, 4161, 4149, 4168, 4153,
    4159, 0,    4162, 4146, 4170, 4154, 4164, 4151,
};
static const uint16_t DEMO_CELLS_C[] = {
    3452, 3461, 3448, 3652, 3455, 3449, 3458, 3450,
    3446, 3457, 3453, 3460, 3447, 3454, 3451, 3459,
};
static_assert(ARRAY_LEN(DEMO_CELLS_B) <= JK_MAX_CELLS, "too many demo cells");

static const demo_scenario_t DEMO_DATA[] = {
    {   // a: 16S LiFePO4 discharging
        .name = "16S discharging",
        .cell_mv = DEMO_CELLS_A, .cells = ARRAY_LEN(DEMO_CELLS_A),
        .voltage_10mv = 5280, .current_10ma = -1234, .soc_pct = 76,
        .temp_mos_c = 25, .temp_1_c = 24, .temp_2_c = 23,
        .cycles = 123, .cycle_ah = 33825, .capacity_ah = 280, .actual_ah = 275,
        .alarms = 0,
        .status = JK_STATUS_CHG_MOS | JK_STATUS_DSG_MOS,
    },
    {   // b: 24S charging at 175 A, balancing, cold, one cell reads 0 mV
        .name = "24S charging, balancing",
        .cell_mv = DEMO_CELLS_B, .cells = ARRAY_LEN(DEMO_CELLS_B),
        .voltage_10mv = 9960, .current_10ma = 17500, .soc_pct = 98,
        .temp_mos_c = -20, .temp_1_c = -18, .temp_2_c = -19,
        .cycles = 57, .cycle_ah = 17328, .capacity_ah = 304, .actual_ah = 301,
        .alarms = 0,
        .status = JK_STATUS_CHG_MOS | JK_STATUS_DSG_MOS | JK_STATUS_BALANCING,
    },
    {   // c: low capacity, MOS overtemp, charge overcurrent, box overtemp,
        //    cell overvoltage; both MOSFETs off with the switches on (tripped)
        .name = "alarms, discharge tripped",
        .cell_mv = DEMO_CELLS_C, .cells = ARRAY_LEN(DEMO_CELLS_C),
        .voltage_10mv = 5545, .current_10ma = 0, .soc_pct = 9,
        .temp_mos_c = 88, .temp_1_c = 61, .temp_2_c = 57,
        .cycles = 412, .cycle_ah = 113300, .capacity_ah = 280, .actual_ah = 268,
        .alarms = (1u << 0) | (1u << 1) | (1u << 5) | (1u << 8) | (1u << 10),
        .status = 0,
    },
};
static_assert(ARRAY_LEN(DEMO_DATA) == DEMO_LINK_LOSS, "one data set per scenario before the link loss");

// Discharge switch state changed by a demo command, per scenario
typedef struct {
    bool set;
    bool setting;   // 0xAC
    bool mos;       // 0x8C bit 1
} demo_dsg_t;

static struct {
    int64_t        t0_us;
    int            scenario;            // -1 while "connecting"
    bool           frozen;              // no scenario change while a command runs
    demo_outcome_t next_outcome;
    demo_dsg_t     dsg[DEMO_LINK_LOSS];
    jk_data_t      data;
    uint8_t        wire[JK_FRAME_MAX];  // reply "on the wire"
    size_t         wire_len;
    size_t         wire_pos;
    int64_t        reply_at_us;
} s_demo;

// ---------------------------------------------------------------------------
// Helpers

// Ticks to block so that esp_timer time t_us has certainly passed (0 if it has)
static TickType_t ticks_until(int64_t t_us)
{
    int64_t left_us = t_us - esp_timer_get_time();
    if (left_us <= 0) {
        return 0;
    }
    if (left_us > 60000000LL) {
        left_us = 60000000LL;   // callers re-check the time after waking
    }
    // Round up, plus one tick because the current tick period is partly over
    return pdMS_TO_TICKS((uint32_t)((left_us + 999) / 1000)) + 1;
}

static void sleep_until(int64_t t_us)
{
    for (TickType_t t = ticks_until(t_us); t > 0; t = ticks_until(t_us)) {
        vTaskDelay(t);
    }
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *err_str(jk_err_t err)
{
    switch (err) {
    case JK_OK:             return "ok";
    case JK_ERR_ARG:        return "bad argument";
    case JK_ERR_LENGTH:     return "bad length";
    case JK_ERR_HEADER:     return "bad start bytes";
    case JK_ERR_END_MARK:   return "no end marker";
    case JK_ERR_CHECKSUM:   return "checksum mismatch";
    case JK_ERR_NOT_REPLY:  return "not a BMS reply";
    case JK_ERR_COMMAND:    return "unexpected command";
    case JK_ERR_TLV_BOUNDS: return "a TLV overruns the info section";
    case JK_ERR_CELLS:      return "bad 0x79 cell list length";
    case JK_ERR_MISSING:    return "required fields missing";
    }
    return "unknown error";
}

static const char *dsg_state_str(jk_dsg_state_t state)
{
    switch (state) {
    case JK_DSG_OFF:  return "OFF";
    case JK_DSG_ON:   return "ON";
    case JK_DSG_TRIP: return "tripped (protection active)";
    }
    return "?";
}

static const char *dsg_result_str(app_dsg_result_t result)
{
    switch (result) {
    case APP_DSG_RESULT_OK:            return "confirmed";
    case APP_DSG_RESULT_NO_REPLY:      return "no reply";
    case APP_DSG_RESULT_NOT_CONFIRMED: return "not confirmed";
    case APP_DSG_RESULT_UNKNOWN:       return "acked, state unknown";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Demo BMS

// Current scenario, -1 while "connecting"
static int demo_sync(void)
{
    if (!s_demo.frozen) {
        int64_t up_us = esp_timer_get_time() - s_demo.t0_us - DEMO_CONNECT_US;
        int sc = up_us < 0 ? -1 : (int)((up_us / DEMO_SCENARIO_US) % DEMO_SCENARIOS);
        if (sc != s_demo.scenario) {
            s_demo.scenario = sc;
            if (sc == DEMO_LINK_LOSS) {
                ESP_LOGI(TAG, "demo: scenario d, link loss");
            } else if (sc >= 0) {
                ESP_LOGI(TAG, "demo: scenario %c, %s", 'a' + sc, DEMO_DATA[sc].name);
            }
        }
    }
    return s_demo.scenario;
}

static bool demo_link_up(int sc)
{
    return sc >= 0 && sc != DEMO_LINK_LOSS;
}

// Protocol 1 (0xC0 = 1) current: bit 15 = charging, magnitude in 10 mA
static void demo_set_current(jk_data_t *d, int32_t current_10ma)
{
    uint32_t mag = (uint32_t)(current_10ma < 0 ? -current_10ma : current_10ma) & 0x7FFFu;
    d->current_raw = (uint16_t)((current_10ma > 0 ? 0x8000u : 0u) | mag);
    d->current_10ma = current_10ma;
    d->current_valid = true;
}

static void demo_fill(int sc, jk_data_t *d)
{
    const demo_scenario_t *s = &DEMO_DATA[sc];

    memset(d, 0, sizeof(*d));
    d->present = JK_HAS_CELLS | JK_HAS_TEMP_MOS | JK_HAS_TEMP_1 | JK_HAS_TEMP_2 |
                 JK_HAS_VOLTAGE | JK_HAS_CURRENT_RAW | JK_HAS_SOC | JK_HAS_TEMP_COUNT |
                 JK_HAS_CYCLES | JK_HAS_CYCLE_AH | JK_HAS_CELL_SETTING | JK_HAS_ALARMS |
                 JK_HAS_STATUS | JK_HAS_BAL_SWITCH | JK_HAS_CAPACITY | JK_HAS_CHG_SWITCH |
                 JK_HAS_DSG_SWITCH | JK_HAS_DEVICE_ID | JK_HAS_UPTIME | JK_HAS_SW_VERSION |
                 JK_HAS_ACTUAL_AH | JK_HAS_MANUFACTURER | JK_HAS_PROTOCOL;
    d->cell_count = s->cells;
    memcpy(d->cell_mv, s->cell_mv, (size_t)s->cells * sizeof(d->cell_mv[0]));
    d->temp_mos_c = s->temp_mos_c;
    d->temp_1_c = s->temp_1_c;
    d->temp_2_c = s->temp_2_c;
    d->temp_sensor_count = 2;
    d->voltage_10mv = s->voltage_10mv;
    demo_set_current(d, s->current_10ma);
    d->soc_pct = s->soc_pct;
    d->cycles = s->cycles;
    d->cycle_capacity_ah = s->cycle_ah;
    d->cell_count_setting = s->cells;
    d->alarms = s->alarms;
    d->status = s->status;
    d->balancer_switch = true;
    d->capacity_ah = s->capacity_ah;
    d->charge_switch = true;
    d->discharge_switch = true;
    d->uptime_min = 18000;
    d->actual_capacity_ah = s->actual_ah;
    d->protocol_version = 1;
    copy_str(d->device_id, sizeof(d->device_id), "Input Us");
    copy_str(d->sw_version, sizeof(d->sw_version), "11.XW_S11.261");
    copy_str(d->manufacturer, sizeof(d->manufacturer), "Input UserdaJK_B2A24S15P");

    const demo_dsg_t *o = &s_demo.dsg[sc];
    if (o->set) {
        d->discharge_switch = o->setting;
        if (o->mos) {
            d->status = (uint16_t)(d->status | JK_STATUS_DSG_MOS);
        } else {
            d->status = (uint16_t)(d->status & ~JK_STATUS_DSG_MOS);
        }
    }
    if (!(d->status & JK_STATUS_DSG_MOS) && d->current_10ma < 0) {
        demo_set_current(d, 0);     // no discharge current with the MOSFET off
    }
}

// A request reached the simulated BMS. Only read-all is answered.
static void demo_request(const uint8_t *frame, size_t len)
{
    s_demo.wire_len = 0;
    s_demo.wire_pos = 0;
    int sc = demo_sync();
    if (len < JK_FRAME_MIN || jk_frame_command(frame) != JK_CMD_READ_ALL || !demo_link_up(sc)) {
        return;
    }
    demo_fill(sc, &s_demo.data);
    s_demo.wire_len = jk_encode_read_all_reply(&s_demo.data, s_demo.wire, sizeof(s_demo.wire));
    s_demo.reply_at_us = esp_timer_get_time() + DEMO_REPLY_US;
    if (s_demo.wire_len == 0) {
        ESP_LOGE(TAG, "demo: encoding the reply failed");
    }
}

static int demo_read(uint8_t *buf, size_t cap, TickType_t ticks)
{
    if (s_demo.wire_pos < s_demo.wire_len && esp_timer_get_time() >= s_demo.reply_at_us) {
        size_t n = s_demo.wire_len - s_demo.wire_pos;
        if (n > cap) {
            n = cap;
        }
        memcpy(buf, &s_demo.wire[s_demo.wire_pos], n);
        s_demo.wire_pos += n;
        return (int)n;
    }
    vTaskDelay(ticks);  // nothing on the wire (yet), like a UART read timing out
    return 0;
}

// ---------------------------------------------------------------------------
// Link

// Drop unread UART input (stale here); bytes that were waiting count as just received
static void drop_input(void)
{
    size_t buffered = 0;
    if (uart_get_buffered_data_len(JK_UART, &buffered) == ESP_OK && buffered > 0) {
        s_last_io_us = esp_timer_get_time();
    }
    uart_flush_input(JK_UART);
}

// Start of a transaction: drop stale input and reset the assembler
static void link_begin(void)
{
    if (APP_DEMO_MODE) {
        s_demo.wire_len = 0;
        s_demo.wire_pos = 0;
    } else {
        drop_input();
    }
    jk_rx_reset(&s_rx);
    s_rx_bytes = 0;
}

static int link_read(uint8_t *buf, size_t cap, TickType_t ticks)
{
    if (APP_DEMO_MODE) {
        return demo_read(buf, cap, ticks);
    }
    return uart_read_bytes(JK_UART, buf, (uint32_t)cap, ticks);
}

// Every request goes out here: >= 100 ms after the last byte sent or received
static void jk_tx(const uint8_t *frame, size_t len)
{
    if (len == 0) {
        ESP_LOGE(TAG, "building a request frame failed");
        return;
    }
    sleep_until(s_last_io_us + JK_GAP_US);
    if (!APP_DEMO_MODE) {
        drop_input();                           // bytes that arrived during the gap restart it
        sleep_until(s_last_io_us + JK_GAP_US);  // no wait if none did
    }
    ESP_LOGD(TAG, "tx %u bytes", (unsigned)len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, (uint16_t)len, ESP_LOG_DEBUG);
    if (APP_DEMO_MODE) {
        demo_request(frame, len);
    } else {
        int written = uart_write_bytes(JK_UART, frame, len);
        if (written != (int)len) {
            ESP_LOGW(TAG, "UART write: %d of %u bytes", written, (unsigned)len);
        }
        esp_err_t err = uart_wait_tx_done(JK_UART, pdMS_TO_TICKS(JK_TX_DONE_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UART TX not done: %s", esp_err_to_name(err));
        }
    }
    s_last_io_us = esp_timer_get_time();
}

// Feed received bytes to the assembler until a frame for cmd arrives (for
// JK_CMD_WRITE: the ack for reg) or deadline_us passes. Other valid frames
// are skipped. Returns the frame length (frame in s_frame), or 0.
static size_t rx_wait(uint8_t cmd, uint8_t reg, int64_t deadline_us)
{
    for (;;) {
        size_t len = 0;
        while (jk_rx_pop_frame(&s_rx, s_frame, sizeof(s_frame), &len)) {
            bool match = (cmd == JK_CMD_WRITE) ? jk_is_write_ack(s_frame, len, reg)
                                               : (jk_frame_command(s_frame) == cmd);
            if (match) {
                return len;
            }
            ESP_LOGD(TAG, "skipping a frame: command 0x%02x, %u bytes",
                     (unsigned)jk_frame_command(s_frame), (unsigned)len);
        }

        int64_t left_us = deadline_us - esp_timer_get_time();
        if (left_us <= 0) {
            return 0;
        }
        int64_t chunk_ms = (left_us + 999) / 1000;
        if (chunk_ms > JK_RX_CHUNK_MS) {
            chunk_ms = JK_RX_CHUNK_MS;
        }
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)chunk_ms);
        if (ticks == 0) {
            ticks = 1;
        }
        int n = link_read(s_chunk, sizeof(s_chunk), ticks);
        if (n > 0) {
            s_last_io_us = esp_timer_get_time();
            s_rx_bytes += (size_t)n;
            jk_rx_push(&s_rx, s_chunk, (size_t)n);
        } else if (n < 0) {
            vTaskDelay(ticks);  // driver error: do not spin
        }
    }
}

// ---------------------------------------------------------------------------
// Diagnostics

static bool problem_is_new(const parse_problem_t *p)
{
    for (unsigned i = 0; i < s_problem_count; i++) {
        const parse_problem_t *q = &s_problems[i];
        if (q->err == p->err && q->clean_end == p->clean_end &&
            q->stop_id == p->stop_id && q->stop_offset == p->stop_offset) {
            return false;
        }
    }
    s_problems[s_problem_next] = *p;
    s_problem_next = (s_problem_next + 1) % PROBLEM_SLOTS;
    if (s_problem_count < PROBLEM_SLOTS) {
        s_problem_count++;
    }
    return true;
}

static void report_parse(jk_err_t err, const jk_parse_info_t *info, size_t len)
{
    if (info->cells_reported > JK_MAX_CELLS && !s_warned_cells_clamped) {
        s_warned_cells_clamped = true;
        ESP_LOGW(TAG, "the BMS reports %u cells, only the first %d are used",
                 (unsigned)info->cells_reported, JK_MAX_CELLS);
    }
    if (err == JK_OK && info->clean_end) {
        return;
    }
    const parse_problem_t p = {
        .err = err,
        .clean_end = info->clean_end,
        .stop_id = info->clean_end ? 0 : info->stop_id,
        .stop_offset = info->clean_end ? 0 : info->stop_offset,
    };
    if (!problem_is_new(&p)) {
        return;
    }
    if (err == JK_OK) {
        ESP_LOGW(TAG, "unknown ID 0x%02x at offset %u ended the TLV walk early, later fields are ignored%s",
                 (unsigned)p.stop_id, (unsigned)p.stop_offset,
                 s_parsed.current_valid ? "" : " (current unknown)");
    } else if (!info->clean_end) {
        ESP_LOGW(TAG, "read-all reply rejected: %s (TLV walk stopped at offset %u, ID 0x%02x)",
                 err_str(err), (unsigned)p.stop_offset, (unsigned)p.stop_id);
    } else {
        ESP_LOGW(TAG, "read-all reply rejected: %s", err_str(err));
    }
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_frame, (uint16_t)len, ESP_LOG_WARN);
}

static void report_no_frame(void)
{
    ESP_LOGD(TAG, "no reply (%u bytes received)", (unsigned)s_rx_bytes);
    if (s_rx_bytes > 0 && !s_warned_garbage) {
        s_warned_garbage = true;
        ESP_LOGW(TAG, "no complete read-all reply within %d ms, but %u bytes arrived "
                 "(slow BMS, noise, wrong baud rate or TX looped back to RX?)",
                 APP_BMS_RX_TIMEOUT_MS, (unsigned)s_rx_bytes);
    }
}

// ---------------------------------------------------------------------------
// Snapshot and polling

// d == NULL: the poll failed
static void snapshot_update(const jk_data_t *d)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (d != NULL) {
        s_snap.has_data = true;
        s_snap.offline = false;
        s_snap.fail_count = 0;
        s_snap.last_ok_us = esp_timer_get_time();
        s_snap.seq++;
        s_snap.data = *d;
    } else {
        if (s_snap.fail_count < UINT32_MAX) {
            s_snap.fail_count++;
        }
        s_snap.offline = s_snap.fail_count >= (uint32_t)APP_BMS_OFFLINE_POLLS;
    }
    const bool     offline = s_snap.offline;
    const bool     has_data = s_snap.has_data;
    const uint32_t fails = s_snap.fail_count;
    xSemaphoreGive(s_lock);

    if (d != NULL && s_link != LINK_ONLINE) {
        s_link = LINK_ONLINE;
        s_activation_armed = true;
        ESP_LOGI(TAG, "BMS online: %u cells, %u.%02u V, SOC %u%%, firmware \"%s\"",
                 (unsigned)d->cell_count, (unsigned)(d->voltage_10mv / 100),
                 (unsigned)(d->voltage_10mv % 100), (unsigned)d->soc_pct,
                 d->sw_version[0] != '\0' ? d->sw_version : "?");
    } else if (d == NULL && offline && s_link != LINK_OFFLINE) {
        s_link = LINK_OFFLINE;
        ESP_LOGI(TAG, "BMS offline: %" PRIu32 " polls without a valid reply", fails);
        if (!has_data && !APP_DEMO_MODE) {
            ESP_LOGW(TAG, "no reply since start: check BMS TX -> GPIO%d, BMS RX -> GPIO%d, GND, "
                     "or try the PC frame source in menuconfig", APP_PIN_BMS_RX, APP_PIN_BMS_TX);
        }
    }
}

// One read-all transaction: updates the snapshot and posts APP_EV_BMS_DATA.
// Returns the parsed data, or NULL if the poll failed.
static const jk_data_t *bms_poll(void)
{
    uint8_t req[JK_REQ_MAX];

    if (APP_JK_SEND_ACTIVATION && s_link == LINK_OFFLINE && s_activation_armed) {
        s_activation_armed = false;     // once per offline period
        ESP_LOGI(TAG, "sending the activation frame");
        link_begin();
        jk_tx(req, jk_build_activate(req, sizeof(req), APP_JK_FRAME_SOURCE));
    }

    link_begin();
    jk_tx(req, jk_build_read_all(req, sizeof(req), APP_JK_FRAME_SOURCE));
    size_t len = rx_wait(JK_CMD_READ_ALL, 0, esp_timer_get_time() + RX_TIMEOUT_US);

    bool ok = false;
    if (len > 0) {
        jk_parse_info_t info = {0};
        memset(&s_parsed, 0, sizeof(s_parsed));
        ESP_LOGD(TAG, "rx %u bytes", (unsigned)len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_frame, (uint16_t)len, ESP_LOG_DEBUG);
        jk_err_t err = jk_parse_read_all(s_frame, len, &s_parsed, &info);
        ok = (err == JK_OK);
        report_parse(err, &info, len);
    } else {
        report_no_frame();
    }

    snapshot_update(ok ? &s_parsed : NULL);
    app_post_simple(APP_EV_BMS_DATA);
    return ok ? &s_parsed : NULL;
}

// ---------------------------------------------------------------------------
// Discharge switch command

// Confirmation polls after a write: only frames read after it count
static void dsg_confirm(bool target, jk_dsg_cmd_t *c)
{
    for (int i = 0; i < JK_CONFIRM_POLLS && !c->settled; i++) {
        if (c->trip_frames > 0) {
            sleep_until(s_last_io_us + (int64_t)JK_TRIP_RECHECK_MS * 1000);
        }
        const jk_data_t *d = bms_poll();
        if (d != NULL) {
            jk_dsg_cmd_readback(c, target, d);
        }
    }
}

static void dsg_command_uart(bool target, jk_dsg_cmd_t *c)
{
    uint8_t req[JK_REQ_MAX];

    // Retry the write only if nothing confirmed it (a confirmed but not yet
    // settled TRIP needs no second write)
    for (int attempt = 0; attempt < JK_CMD_ATTEMPTS && !c->confirmed; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "discharge switch not confirmed, retrying");
        }
        // The password frame is never answered
        link_begin();
        jk_tx(req, jk_build_password(req, sizeof(req), APP_JK_FRAME_SOURCE));
        sleep_until(s_last_io_us + (int64_t)APP_JK_WRITE_DELAY_MS * 1000);

        link_begin();
        jk_tx(req, jk_build_write_u8(req, sizeof(req), APP_JK_FRAME_SOURCE,
                                     JK_REG_DISCHARGE_SWITCH, target ? 1 : 0));
        // The ack is advisory: only the confirmation poll decides
        if (rx_wait(JK_CMD_WRITE, JK_REG_DISCHARGE_SWITCH,
                    esp_timer_get_time() + (int64_t)JK_ACK_TIMEOUT_MS * 1000) > 0) {
            c->acked = true;
        }
        dsg_confirm(target, c);
    }
}

static void dsg_command_demo(bool target, jk_dsg_cmd_t *c)
{
    int sc = demo_sync();
    s_demo.frozen = true;           // the command applies to the scenario on screen
    vTaskDelay(pdMS_TO_TICKS(DEMO_CMD_DELAY_MS));

    demo_outcome_t outcome = DEMO_OUT_NO_REPLY;
    if (demo_link_up(sc)) {
        outcome = s_demo.next_outcome;
        s_demo.next_outcome = (demo_outcome_t)((outcome + 1) % DEMO_OUTCOMES);
    }
    ESP_LOGI(TAG, "demo: simulated outcome: %s", DEMO_OUTCOME_NAMES[outcome]);
    if (outcome != DEMO_OUT_NO_REPLY) {
        s_demo.dsg[sc] = (demo_dsg_t){
            .set = true,
            .setting = target,
            .mos = target && outcome == DEMO_OUT_OK,
        };
        c->acked = true;
        dsg_confirm(target, c);
    }
    s_demo.frozen = false;
}

static void post_result(const app_event_t *ev)
{
    for (int i = 0; i < JK_RESULT_TRIES; i++) {
        if (app_post_event(ev)) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(JK_RESULT_RETRY_MS));
    }
    ESP_LOGW(TAG, "app queue full, discharge result dropped");
}

static app_dsg_result_t app_result(jk_dsg_cmd_result_t r)
{
    switch (r) {
    case JK_DSG_CMD_OK:            return APP_DSG_RESULT_OK;
    case JK_DSG_CMD_NO_REPLY:      return APP_DSG_RESULT_NO_REPLY;
    case JK_DSG_CMD_UNKNOWN:       return APP_DSG_RESULT_UNKNOWN;
    case JK_DSG_CMD_NOT_CONFIRMED: return APP_DSG_RESULT_NOT_CONFIRMED;
    }
    return APP_DSG_RESULT_NOT_CONFIRMED;
}

static void run_command(bool target)
{
    jk_dsg_cmd_t c = {0};

    ESP_LOGI(TAG, "discharge switch -> %s", target ? "ON" : "OFF");
    if (APP_DEMO_MODE) {
        dsg_command_demo(target, &c);
    } else {
        dsg_command_uart(target, &c);
    }

    const app_dsg_result_t result = app_result(jk_dsg_cmd_result(&c));
    // The state shown to the user must come from a frame read after the write
    jk_dsg_state_t state = c.state;
    if (!c.readback) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        state = s_snap.has_data ? jk_dsg_state(&s_snap.data) : JK_DSG_OFF;
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "discharge switch %s: %s (ack: %s, read back: %s), output %s", target ? "ON" : "OFF",
             dsg_result_str(result), c.acked ? "yes" : "no", c.readback ? "yes" : "no",
             c.readback ? dsg_state_str(state) : "unknown");

    const app_event_t ev = {
        .type = APP_EV_DSG_RESULT,
        .dsg_result = result,
        .dsg_target = target,
        .dsg_state = state,
    };
    post_result(&ev);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cmd_pending = false;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------------
// Task

static bool command_get(bool *target)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool pending = s_cmd_pending;
    *target = s_cmd_target;
    xSemaphoreGive(s_lock);
    return pending;
}

// Between transactions: park for good if jk_bms_stop() was called
static void checkpoint(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool stop = s_stop_req;
    const bool dropped = stop && s_cmd_pending;
    if (stop) {
        s_parked = true;
        s_cmd_pending = false;
    }
    xSemaphoreGive(s_lock);
    if (!stop) {
        return;
    }

    xSemaphoreGive(s_parked_sem);
    if (dropped) {
        ESP_LOGW(TAG, "stopped with a discharge command still queued, dropped");
    }
    ESP_LOGI(TAG, "stopped");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static void bms_task(void *arg)
{
    (void)arg;
    if (APP_DEMO_MODE) {
        s_demo.t0_us = esp_timer_get_time();
        s_demo.scenario = -1;
        ESP_LOGI(TAG, "demo: connecting, first data in about 2 s, scenarios change every 10 s");
    }

    int64_t next_poll_us = esp_timer_get_time();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, ticks_until(next_poll_us));
        checkpoint();

        bool target = false;
        if (command_get(&target)) {
            run_command(target);
            checkpoint();
        }

        const int64_t now = esp_timer_get_time();
        if (now >= next_poll_us) {
            bms_poll();
            next_poll_us += POLL_PERIOD_US;
            if (next_poll_us <= now) {
                next_poll_us = now + POLL_PERIOD_US;    // fell behind (a long command): restart from now
            }
        }
    }
}

static esp_err_t uart_setup(void)
{
    const uart_config_t cfg = {
        .baud_rate = JK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(JK_UART, JK_UART_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(JK_UART, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(JK_UART, APP_PIN_BMS_TX, APP_PIN_BMS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        // RX goes through the GPIO matrix without a pull: keep an unplugged line idle-high
        err = gpio_pullup_en((gpio_num_t)APP_PIN_BMS_RX);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART setup: %s", esp_err_to_name(err));
        uart_driver_delete(JK_UART);
        return err;
    }
    ESP_LOGI(TAG, "UART%d %d 8N1: TX GPIO%d -> BMS RX, RX GPIO%d <- BMS TX, frame source 0x%02x",
             (int)JK_UART, JK_BAUD, APP_PIN_BMS_TX, APP_PIN_BMS_RX, (unsigned)APP_JK_FRAME_SOURCE);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// API

esp_err_t jk_bms_start(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        s_parked_sem = xSemaphoreCreateBinaryStatic(&s_parked_sem_buf);
    }

    if (APP_DEMO_MODE) {
        ESP_LOGW(TAG, "DEMO MODE: simulated BMS data, the UART is not used");
    } else {
        esp_err_t err = uart_setup();
        if (err != ESP_OK) {
            return err;
        }
    }

    s_task = xTaskCreateStatic(bms_task, "jk_bms", JK_TASK_STACK, NULL, JK_TASK_PRIO,
                               s_task_stack, &s_task_buf);
    return s_task != NULL ? ESP_OK : ESP_FAIL;
}

void jk_bms_get_snapshot(jk_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
}

bool jk_bms_command_pending(void)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool pending = s_cmd_pending;
    xSemaphoreGive(s_lock);
    return pending;
}

bool jk_bms_set_discharge(bool on)
{
    if (s_task == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool accepted = !s_cmd_pending && !s_stop_req;
    if (accepted) {
        s_cmd_pending = true;
        s_cmd_target = on;
    }
    xSemaphoreGive(s_lock);
    if (accepted) {
        xTaskNotifyGive(s_task);
    }
    return accepted;
}

void jk_bms_stop(void)
{
    if (s_task == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool parked = s_parked;
    s_stop_req = true;
    xSemaphoreGive(s_lock);
    if (parked) {
        return;
    }
    xTaskNotifyGive(s_task);
    if (xSemaphoreTake(s_parked_sem, pdMS_TO_TICKS(JK_STOP_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "stop: a transaction is still running, not waiting any longer");
    }
}
