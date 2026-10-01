// JK BMS "GPS port" UART protocol (frames start with 0x4E 0x57).
//
// Pure C: depends only on the C standard library, so it builds for the host
// test environment (pio test -e native) as well as for the firmware.
//
// Frame layout (total = LEN + 2 bytes, LEN big-endian):
//   [0..1]   4E 57           start
//   [2..3]   LEN             counts everything except the start bytes
//   [4..7]   00 00 00 00     BMS terminal number
//   [8]      command         01 activate, 02 write, 03 read one, 05 password, 06 read all
//   [9]      frame source    0 BMS, 1 Bluetooth, 2 GPS, 3 PC
//   [10]     transport type  0 request, 1 reply, 2 BMS active upload
//   [11..]   info            requests: ID (+ value); replies: ID/value TLVs
//   [t-9..t-6] record number (4 bytes)
//   [t-5]    68              end marker
//   [t-4..t-3] 00 00         (unused CRC half)
//   [t-2..t-1] sum16 BE      uint16 sum of bytes [0, t-4)
//
// Sources: syssi/esphome-jk-bms (components/jk_modbus, components/jk_bms),
// docs/protocol-design-uart-ttl-gps-port.md and the official JK protocol PDF.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JK_MAX_CELLS    24
#define JK_FRAME_MIN    20      // smallest well-formed frame (empty info section)
#define JK_FRAME_MAX    512

#define JK_SRC_BMS      0x00
#define JK_SRC_GPS      0x02
#define JK_SRC_PC       0x03

#define JK_CMD_ACTIVATE 0x01
#define JK_CMD_WRITE    0x02
#define JK_CMD_READ     0x03
#define JK_CMD_PASSWORD 0x05
#define JK_CMD_READ_ALL 0x06

#define JK_TYPE_REQUEST 0x00
#define JK_TYPE_REPLY   0x01
#define JK_TYPE_UPLOAD  0x02

#define JK_REG_BALANCER_SWITCH  0x9D
#define JK_REG_CHARGE_SWITCH    0xAB
#define JK_REG_DISCHARGE_SWITCH 0xAC

// 0x8C status bits (actual state). Bit 3 is not usable on v11 firmware.
#define JK_STATUS_CHG_MOS   (1u << 0)
#define JK_STATUS_DSG_MOS   (1u << 1)
#define JK_STATUS_BALANCING (1u << 2)

// jk_data_t.present bits: which fields were found in the frame
#define JK_HAS_CELLS        (1u << 0)   // 0x79
#define JK_HAS_TEMP_MOS     (1u << 1)   // 0x80
#define JK_HAS_TEMP_1       (1u << 2)   // 0x81
#define JK_HAS_TEMP_2       (1u << 3)   // 0x82
#define JK_HAS_VOLTAGE      (1u << 4)   // 0x83
#define JK_HAS_CURRENT_RAW  (1u << 5)   // 0x84
#define JK_HAS_SOC          (1u << 6)   // 0x85
#define JK_HAS_TEMP_COUNT   (1u << 7)   // 0x86
#define JK_HAS_CYCLES       (1u << 8)   // 0x87
#define JK_HAS_CYCLE_AH     (1u << 9)   // 0x89
#define JK_HAS_CELL_SETTING (1u << 10)  // 0x8A
#define JK_HAS_ALARMS       (1u << 11)  // 0x8B
#define JK_HAS_STATUS       (1u << 12)  // 0x8C
#define JK_HAS_BAL_SWITCH   (1u << 13)  // 0x9D
#define JK_HAS_CAPACITY     (1u << 14)  // 0xAA
#define JK_HAS_CHG_SWITCH   (1u << 15)  // 0xAB
#define JK_HAS_DSG_SWITCH   (1u << 16)  // 0xAC
#define JK_HAS_DEVICE_ID    (1u << 17)  // 0xB4
#define JK_HAS_UPTIME       (1u << 18)  // 0xB6
#define JK_HAS_SW_VERSION   (1u << 19)  // 0xB7
#define JK_HAS_ACTUAL_AH    (1u << 20)  // 0xB9
#define JK_HAS_MANUFACTURER (1u << 21)  // 0xBA
#define JK_HAS_PROTOCOL     (1u << 22)  // 0xC0

// A poll counts as successful only if all of these are present
#define JK_REQUIRED_FIELDS (JK_HAS_CELLS | JK_HAS_VOLTAGE | JK_HAS_CURRENT_RAW | JK_HAS_SOC | JK_HAS_STATUS)

typedef struct {
    uint32_t present;               // JK_HAS_* bits

    // 0x79 cell voltages (clamped to JK_MAX_CELLS); statistics exclude 0 mV cells
    uint8_t  cell_count;
    uint16_t cell_mv[JK_MAX_CELLS];
    uint8_t  cells_valid;           // cells with a non-zero voltage
    uint16_t cell_min_mv;           // 0 if no valid cell
    uint16_t cell_max_mv;
    uint16_t cell_avg_mv;
    uint16_t cell_delta_mv;
    uint8_t  cell_min_idx;          // 0-based; 0xFF if no valid cell
    uint8_t  cell_max_idx;

    int16_t  temp_mos_c;            // 0x80
    int16_t  temp_1_c;              // 0x81
    int16_t  temp_2_c;              // 0x82
    uint8_t  temp_sensor_count;     // 0x86

    uint16_t voltage_10mv;          // 0x83, 10 mV units
    uint16_t current_raw;           // 0x84 as received
    bool     current_valid;         // false: current (and power) unknown
    int32_t  current_10ma;          // 10 mA units, + charging / - discharging
    int32_t  power_dw;              // 0.1 W units, same sign as current

    uint8_t  soc_pct;               // 0x85
    uint16_t cycles;                // 0x87
    uint32_t cycle_capacity_ah;     // 0x89
    uint16_t cell_count_setting;    // 0x8A
    uint16_t alarms;                // 0x8B bitmask, see jk_alarm_*()
    uint16_t status;                // 0x8C bitmask, JK_STATUS_*
    bool     balancer_switch;       // 0x9D
    uint32_t capacity_ah;           // 0xAA nominal capacity
    bool     charge_switch;         // 0xAB setting
    bool     discharge_switch;      // 0xAC setting
    uint32_t uptime_min;            // 0xB6
    uint32_t actual_capacity_ah;    // 0xB9
    uint8_t  protocol_version;      // 0xC0

    char     device_id[9];          // 0xB4, NUL-terminated
    char     sw_version[16];        // 0xB7, NUL-terminated
    char     manufacturer[25];      // 0xBA, NUL-terminated
} jk_data_t;

typedef enum {
    JK_OK = 0,
    JK_ERR_ARG,
    JK_ERR_LENGTH,      // too short, or LEN outside [JK_FRAME_MIN, JK_FRAME_MAX]
    JK_ERR_HEADER,      // does not start with 4E 57
    JK_ERR_END_MARK,    // no 0x68 at total-5
    JK_ERR_CHECKSUM,
    JK_ERR_NOT_REPLY,   // source/type are not a BMS reply (for example our own echo)
    JK_ERR_COMMAND,     // not the expected command
    JK_ERR_TLV_BOUNDS,  // a TLV overruns the info section
    JK_ERR_CELLS,       // 0x79 length is not a multiple of 3
    JK_ERR_MISSING,     // a JK_REQUIRED_FIELDS field is missing
} jk_err_t;

typedef struct {
    bool     clean_end;     // the TLV walk ended exactly at the record number
    uint16_t stop_offset;   // frame offset where the walk stopped
    uint8_t  stop_id;       // unknown ID that stopped the walk (0 when clean)
    uint8_t  cells_reported;// cells in 0x79 before clamping to JK_MAX_CELLS
} jk_parse_info_t;

// ---- Request builders: return the frame length, or 0 if cap is too small ----
size_t jk_build_read_all(uint8_t *buf, size_t cap, uint8_t source);            // 21 bytes
size_t jk_build_activate(uint8_t *buf, size_t cap, uint8_t source);            // 21 bytes
size_t jk_build_password(uint8_t *buf, size_t cap, uint8_t source);            // 22 bytes, reg 0x00 val 0x00
size_t jk_build_write_u8(uint8_t *buf, size_t cap, uint8_t source,
                         uint8_t reg, uint8_t value);                          // 22 bytes

// uint16 sum of len bytes
uint16_t jk_sum16(const uint8_t *buf, size_t len);

// Validate one complete frame: start bytes, LEN == len - 2, length bounds,
// end marker, checksum, and source == BMS with type reply (or active upload
// for command 0x06).
jk_err_t jk_validate_reply(const uint8_t *frame, size_t len);

// ---- Receive assembler ----
typedef struct {
    uint8_t buf[2 * JK_FRAME_MAX];
    size_t  len;
} jk_rx_t;

void jk_rx_reset(jk_rx_t *rx);
// Append received bytes. If the buffer is full, the oldest bytes are dropped.
void jk_rx_push(jk_rx_t *rx, const uint8_t *data, size_t n);
// Extract the next valid reply frame into out. Garbage and rejected candidates
// are skipped one byte at a time (rescan from offset 1). Returns false if more
// bytes are needed.
bool jk_rx_pop_frame(jk_rx_t *rx, uint8_t *out, size_t out_cap, size_t *out_len);

// ---- Reply helpers (frame must be validated) ----
static inline uint8_t jk_frame_command(const uint8_t *frame) { return frame[8]; }
// A write acknowledgement: 21 bytes, command 0x02, info = the written register
bool jk_is_write_ack(const uint8_t *frame, size_t len, uint8_t reg);

// Parse a validated read-all reply (command 0x06). On JK_OK all fields named
// in out->present are valid. info may be NULL.
jk_err_t jk_parse_read_all(const uint8_t *frame, size_t len, jk_data_t *out, jk_parse_info_t *info);

// Encode data as a read-all reply frame (used by demo mode and tests).
// Fields whose JK_HAS_* bit is clear are omitted. Returns the frame length or 0.
size_t jk_encode_read_all_reply(const jk_data_t *d, uint8_t *buf, size_t cap);

// ---- Decoding rules (exposed for tests) ----
// raw > 99 -> 99 - raw (esphome: 100 = -1 degC)
int16_t jk_decode_temp(uint16_t raw);
// protocol == 1: bit 15 = charging, magnitude raw & 0x7FFF.
// protocol == 0, or protocol absent with clean_end: (10000 - raw).
// Otherwise unknown -> returns false. Units: 10 mA, + charging.
bool jk_decode_current(uint16_t raw, bool has_protocol, uint8_t protocol, bool clean_end, int32_t *out_10ma);
// 0.1 W units: (int64) v_10mV * i_10mA / 1000
int32_t jk_power_dw(uint16_t voltage_10mv, int32_t current_10ma);

// ---- Discharge output model ----
typedef enum {
    JK_DSG_OFF = 0,     // setting off and MOSFET off
    JK_DSG_ON,          // MOSFET on
    JK_DSG_TRIP,        // setting on but MOSFET off: a protection is active
} jk_dsg_state_t;

// (0xAC present && 0xAC) || (0x8C bit 1). One rule for the toggle target,
// the hold label and the confirmation.
bool jk_dsg_on(const jk_data_t *d);
jk_dsg_state_t jk_dsg_state(const jk_data_t *d);

// ---- Discharge-switch command confirmation ----
// Tracks what was learned after a write to 0xAC. Start from {0}.
typedef struct {
    bool           acked;       // a write acknowledgement arrived (advisory only)
    bool           readback;    // at least one valid frame was read after the write
    bool           confirmed;   // the last read-back frame satisfies jk_dsg_on() == target
    bool           settled;     // confirmed and final (see jk_dsg_cmd_readback)
    uint8_t        trip_frames; // consecutive JK_DSG_TRIP frames while enabling
    jk_dsg_state_t state;       // state in the last read-back frame (valid if readback)
} jk_dsg_cmd_t;

typedef enum {
    JK_DSG_CMD_OK = 0,          // confirmed by a frame read after the write
    JK_DSG_CMD_NO_REPLY,        // no ack and no frame after the write
    JK_DSG_CMD_NOT_CONFIRMED,   // frames were read back, but they do not match the target
    JK_DSG_CMD_UNKNOWN,         // acked, but no frame could be read back
} jk_dsg_cmd_result_t;

// Number of consecutive TRIP frames needed to settle an enable as tripped
#define JK_DSG_TRIP_FRAMES 2

// Account one valid frame read back after the write. Returns true once the
// command is settled. A frame that confirms an enable but shows JK_DSG_TRIP
// (setting on, MOSFET off) settles only after JK_DSG_TRIP_FRAMES in a row,
// because the MOSFET may lag the setting by a poll.
bool jk_dsg_cmd_readback(jk_dsg_cmd_t *c, bool target, const jk_data_t *d);
jk_dsg_cmd_result_t jk_dsg_cmd_result(const jk_dsg_cmd_t *c);

// ---- Alarms (0x8B) ----
#define JK_ALARM_BITS 14
// Long name for bit 0..13, NULL for unused bits
const char *jk_alarm_name(unsigned bit);
// Short name (fits the main-screen banner), NULL for unused bits
const char *jk_alarm_short_name(unsigned bit);
// Bit 0 (low capacity) is only a warning
bool jk_alarm_is_warning(unsigned bit);
// Highest-priority active bit: the lowest protection bit 1..13, then bit 0. -1 if none.
int jk_alarm_top(uint16_t alarms);
unsigned jk_alarm_count(uint16_t alarms);
