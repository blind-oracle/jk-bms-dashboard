// JK BMS "GPS port" protocol: request builders, receive assembler, read-all
// TLV parser and encoder, decoding rules and alarm names.
//
// Pure C (see jk_proto.h): no ESP-IDF, sdkconfig or logging, so the same file
// is unit-tested on the host (pio test -e native).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "jk_proto.h"

#define JK_START_0      0x4E
#define JK_START_1      0x57
#define JK_END_MARK     0x68
#define JK_ID_CELLS     0x79

// Bytes before the info section: start, LEN, terminal, command, source, type
#define JK_HEADER_LEN   11
// Bytes after the info section: record number (4), end mark, 00 00, sum16
#define JK_TRAILER_LEN  9

_Static_assert(JK_HEADER_LEN + JK_TRAILER_LEN == JK_FRAME_MIN, "frame layout");

// Fixed field lengths of the 0xB4 / 0xB7 / 0xBA strings
#define JK_LEN_DEVICE_ID    8
#define JK_LEN_SW_VERSION   15
#define JK_LEN_MANUFACTURER 24

_Static_assert(sizeof(((jk_data_t *)0)->device_id) == JK_LEN_DEVICE_ID + 1, "device_id size");
_Static_assert(sizeof(((jk_data_t *)0)->sw_version) == JK_LEN_SW_VERSION + 1, "sw_version size");
_Static_assert(sizeof(((jk_data_t *)0)->manufacturer) == JK_LEN_MANUFACTURER + 1, "manufacturer size");

// ---- Byte helpers (all multi-byte values are big-endian) ----

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

// ---- Request builders ----

uint16_t jk_sum16(const uint8_t *buf, size_t len)
{
    uint16_t sum = 0;
    if (buf == NULL) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        sum = (uint16_t)(sum + buf[i]);
    }
    return sum;
}

// Request frame with info_len info bytes: 1 (register) or 2 (register + value).
static size_t build_request(uint8_t *buf, size_t cap, uint8_t cmd, uint8_t source,
                            const uint8_t *info, size_t info_len)
{
    const size_t total = JK_FRAME_MIN + info_len;
    if (buf == NULL || cap < total) {
        return 0;
    }
    memset(buf, 0, total);  // terminal, record number and the unused CRC half are 0
    buf[0] = JK_START_0;
    buf[1] = JK_START_1;
    put_be16(&buf[2], (uint16_t)(total - 2));
    buf[8] = cmd;
    buf[9] = source;
    buf[10] = JK_TYPE_REQUEST;
    memcpy(&buf[JK_HEADER_LEN], info, info_len);
    buf[total - 5] = JK_END_MARK;
    put_be16(&buf[total - 2], jk_sum16(buf, total - 4));
    return total;
}

size_t jk_build_read_all(uint8_t *buf, size_t cap, uint8_t source)
{
    const uint8_t info[] = {0x00};
    return build_request(buf, cap, JK_CMD_READ_ALL, source, info, sizeof(info));
}

size_t jk_build_activate(uint8_t *buf, size_t cap, uint8_t source)
{
    const uint8_t info[] = {0x00};
    return build_request(buf, cap, JK_CMD_ACTIVATE, source, info, sizeof(info));
}

size_t jk_build_password(uint8_t *buf, size_t cap, uint8_t source)
{
    const uint8_t info[] = {0x00, 0x00};
    return build_request(buf, cap, JK_CMD_PASSWORD, source, info, sizeof(info));
}

size_t jk_build_write_u8(uint8_t *buf, size_t cap, uint8_t source, uint8_t reg, uint8_t value)
{
    const uint8_t info[] = {reg, value};
    return build_request(buf, cap, JK_CMD_WRITE, source, info, sizeof(info));
}

// ---- Validation ----

jk_err_t jk_validate_reply(const uint8_t *frame, size_t len)
{
    if (frame == NULL) {
        return JK_ERR_ARG;
    }
    if (len < JK_FRAME_MIN || len > JK_FRAME_MAX) {
        return JK_ERR_LENGTH;
    }
    if (frame[0] != JK_START_0 || frame[1] != JK_START_1) {
        return JK_ERR_HEADER;
    }
    if ((size_t)get_be16(&frame[2]) + 2 != len) {
        return JK_ERR_LENGTH;
    }
    if (frame[len - 5] != JK_END_MARK) {
        return JK_ERR_END_MARK;
    }
    if (jk_sum16(frame, len - 4) != get_be16(&frame[len - 2])) {
        return JK_ERR_CHECKSUM;
    }
    if (frame[9] != JK_SRC_BMS) {
        return JK_ERR_NOT_REPLY;
    }
    if (frame[10] == JK_TYPE_REPLY) {
        return JK_OK;
    }
    if (frame[10] == JK_TYPE_UPLOAD && frame[8] == JK_CMD_READ_ALL) {
        return JK_OK;
    }
    return JK_ERR_NOT_REPLY;
}

// ---- Receive assembler ----

static void rx_consume(jk_rx_t *rx, size_t n)
{
    if (n >= rx->len) {
        rx->len = 0;
        return;
    }
    memmove(rx->buf, &rx->buf[n], rx->len - n);
    rx->len -= n;
}

void jk_rx_reset(jk_rx_t *rx)
{
    if (rx != NULL) {
        rx->len = 0;
    }
}

void jk_rx_push(jk_rx_t *rx, const uint8_t *data, size_t n)
{
    if (rx == NULL || data == NULL || n == 0) {
        return;
    }
    const size_t cap = sizeof(rx->buf);
    if (rx->len > cap) {
        rx->len = 0;    // corrupted state: start over
    }
    if (n >= cap) {
        // Only the newest cap bytes survive
        memcpy(rx->buf, &data[n - cap], cap);
        rx->len = cap;
        return;
    }
    if (rx->len + n > cap) {
        rx_consume(rx, rx->len + n - cap);
    }
    memcpy(&rx->buf[rx->len], data, n);
    rx->len += n;
}

// Every 4E 57 position is a candidate, scanned from the start. A rejected
// candidate is skipped by one byte and an incomplete one does not stop the
// scan, so neither a corrupt frame nor a false header with a large LEN inside
// garbage can hide a complete frame behind it. A valid frame that does not fit
// out is consumed and reported with *out_len = 0.
bool jk_rx_pop_frame(jk_rx_t *rx, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (rx == NULL) {
        return false;
    }
    if (rx->len > sizeof(rx->buf)) {
        rx->len = 0;    // corrupted state: start over
    }

    size_t keep = rx->len;      // first byte to keep when no frame is returned
    bool pending = false;       // keep is the first incomplete candidate
    for (size_t i = 0; i + 1 < rx->len; i++) {
        if (rx->buf[i] != JK_START_0 || rx->buf[i + 1] != JK_START_1) {
            continue;
        }
        const size_t avail = rx->len - i;
        if (avail < 4) {
            // LEN not received yet; every later position has even fewer bytes
            if (!pending) {
                keep = i;
                pending = true;
            }
            break;
        }
        const size_t total = (size_t)get_be16(&rx->buf[i + 2]) + 2;
        if (total < JK_FRAME_MIN || total > JK_FRAME_MAX) {
            continue;
        }
        if (avail < total) {
            if (!pending) {
                keep = i;
                pending = true;
            }
            continue;
        }
        if (jk_validate_reply(&rx->buf[i], total) != JK_OK) {
            continue;
        }
        if (out != NULL && out_cap >= total) {
            memcpy(out, &rx->buf[i], total);
            if (out_len != NULL) {
                *out_len = total;
            }
        }
        rx_consume(rx, i + total);
        return true;
    }

    if (!pending && rx->len > 0 && rx->buf[rx->len - 1] == JK_START_0) {
        keep = rx->len - 1;     // may be the first start byte of the next frame
    }
    rx_consume(rx, keep);
    return false;
}

// ---- Reply helpers ----

bool jk_is_write_ack(const uint8_t *frame, size_t len, uint8_t reg)
{
    return frame != NULL && len == 21 && frame[8] == JK_CMD_WRITE && frame[11] == reg;
}

// ---- Decoding rules ----

int16_t jk_decode_temp(uint16_t raw)
{
    if (raw > 99) {
        int32_t t = 99 - (int32_t)raw;
        if (t < INT16_MIN) {
            t = INT16_MIN;
        }
        return (int16_t)t;
    }
    return (int16_t)raw;
}

bool jk_decode_current(uint16_t raw, bool has_protocol, uint8_t protocol, bool clean_end, int32_t *out_10ma)
{
    int32_t current;
    bool valid = true;
    if (has_protocol && protocol == 1) {
        current = (int32_t)(raw & 0x7FFF);
        if ((raw & 0x8000) == 0) {
            current = -current;     // bit 15 clear: discharging
        }
    } else if ((has_protocol && protocol == 0) || (!has_protocol && clean_end)) {
        current = 10000 - (int32_t)raw;
    } else {
        // C0 missing after an early stop, or an unknown protocol version
        current = 0;
        valid = false;
    }
    if (out_10ma != NULL) {
        *out_10ma = current;
    }
    return valid;
}

int32_t jk_power_dw(uint16_t voltage_10mv, int32_t current_10ma)
{
    return (int32_t)(((int64_t)voltage_10mv * current_10ma) / 1000);
}

// ---- Read-all parser ----

// Value length of a fixed-size TLV; 0 = unknown ID (0x79 is handled by the caller)
static size_t tlv_len(uint8_t id)
{
    switch (id) {
    case 0x85: case 0x86: case 0x9D: case 0xA9: case 0xAB: case 0xAC:
    case 0xAE: case 0xAF: case 0xB1: case 0xB3: case 0xB8: case 0xC0:
        return 1;
    case 0x89: case 0xAA: case 0xB5: case 0xB6: case 0xB9:
        return 4;
    case 0xB4:
        return JK_LEN_DEVICE_ID;
    case 0xB2:
        return 10;
    case 0xB7:
        return JK_LEN_SW_VERSION;
    case 0xBA:
        return JK_LEN_MANUFACTURER;
    default:
        break;
    }
    if ((id >= 0x80 && id <= 0x84) || id == 0x87 || (id >= 0x8A && id <= 0x8C) ||
        (id >= 0x8E && id <= 0x9C) || (id >= 0x9E && id <= 0xA8) || id == 0xAD || id == 0xB0) {
        return 2;
    }
    return 0;
}

// Copy a fixed-length string field: stop at the first non-printable byte,
// trim trailing spaces, always NUL-terminate.
static void copy_str(char *dst, size_t dst_size, const uint8_t *src, size_t n)
{
    size_t k = 0;
    while (k < n && k + 1 < dst_size && src[k] >= 0x20 && src[k] <= 0x7E) {
        dst[k] = (char)src[k];
        k++;
    }
    while (k > 0 && dst[k - 1] == ' ') {
        k--;
    }
    dst[k] = '\0';
}

// 0x79 payload: n cells of {index, mV BE}, stored in frame order
static void decode_cells(jk_data_t *d, const uint8_t *p, size_t n)
{
    const size_t keep = n < JK_MAX_CELLS ? n : JK_MAX_CELLS;
    memset(d->cell_mv, 0, sizeof(d->cell_mv));
    for (size_t k = 0; k < keep; k++) {
        d->cell_mv[k] = get_be16(&p[3 * k + 1]);
    }
    d->cell_count = (uint8_t)keep;
    d->present |= JK_HAS_CELLS;
}

static void decode_tlv(jk_data_t *d, uint8_t id, const uint8_t *v)
{
    switch (id) {
    case 0x80:
        d->temp_mos_c = jk_decode_temp(get_be16(v));
        d->present |= JK_HAS_TEMP_MOS;
        break;
    case 0x81:
        d->temp_1_c = jk_decode_temp(get_be16(v));
        d->present |= JK_HAS_TEMP_1;
        break;
    case 0x82:
        d->temp_2_c = jk_decode_temp(get_be16(v));
        d->present |= JK_HAS_TEMP_2;
        break;
    case 0x83:
        d->voltage_10mv = get_be16(v);
        d->present |= JK_HAS_VOLTAGE;
        break;
    case 0x84:
        d->current_raw = get_be16(v);
        d->present |= JK_HAS_CURRENT_RAW;
        break;
    case 0x85:
        d->soc_pct = v[0];
        d->present |= JK_HAS_SOC;
        break;
    case 0x86:
        d->temp_sensor_count = v[0];
        d->present |= JK_HAS_TEMP_COUNT;
        break;
    case 0x87:
        d->cycles = get_be16(v);
        d->present |= JK_HAS_CYCLES;
        break;
    case 0x89:
        d->cycle_capacity_ah = get_be32(v);
        d->present |= JK_HAS_CYCLE_AH;
        break;
    case 0x8A:
        d->cell_count_setting = get_be16(v);
        d->present |= JK_HAS_CELL_SETTING;
        break;
    case 0x8B:
        d->alarms = get_be16(v);
        d->present |= JK_HAS_ALARMS;
        break;
    case 0x8C:
        d->status = get_be16(v);
        d->present |= JK_HAS_STATUS;
        break;
    case 0x9D:
        d->balancer_switch = v[0] != 0;
        d->present |= JK_HAS_BAL_SWITCH;
        break;
    case 0xAA:
        d->capacity_ah = get_be32(v);
        d->present |= JK_HAS_CAPACITY;
        break;
    case 0xAB:
        d->charge_switch = v[0] != 0;
        d->present |= JK_HAS_CHG_SWITCH;
        break;
    case 0xAC:
        d->discharge_switch = v[0] != 0;
        d->present |= JK_HAS_DSG_SWITCH;
        break;
    case 0xB4:
        copy_str(d->device_id, sizeof(d->device_id), v, JK_LEN_DEVICE_ID);
        d->present |= JK_HAS_DEVICE_ID;
        break;
    case 0xB6:
        d->uptime_min = get_be32(v);
        d->present |= JK_HAS_UPTIME;
        break;
    case 0xB7:
        copy_str(d->sw_version, sizeof(d->sw_version), v, JK_LEN_SW_VERSION);
        d->present |= JK_HAS_SW_VERSION;
        break;
    case 0xB9:
        d->actual_capacity_ah = get_be32(v);
        d->present |= JK_HAS_ACTUAL_AH;
        break;
    case 0xBA:
        copy_str(d->manufacturer, sizeof(d->manufacturer), v, JK_LEN_MANUFACTURER);
        d->present |= JK_HAS_MANUFACTURER;
        break;
    case 0xC0:
        d->protocol_version = v[0];
        d->present |= JK_HAS_PROTOCOL;
        break;
    default:
        break;  // known length, not used (settings)
    }
}

// Min/max (first occurrence), average and delta over the non-zero cells
static void cell_stats(jk_data_t *d)
{
    uint32_t sum = 0;
    uint8_t n = 0;
    d->cell_min_mv = 0;
    d->cell_max_mv = 0;
    d->cell_avg_mv = 0;
    d->cell_delta_mv = 0;
    d->cell_min_idx = 0xFF;
    d->cell_max_idx = 0xFF;
    for (uint8_t k = 0; k < d->cell_count && k < JK_MAX_CELLS; k++) {
        const uint16_t mv = d->cell_mv[k];
        if (mv == 0) {
            continue;
        }
        if (n == 0 || mv < d->cell_min_mv) {
            d->cell_min_mv = mv;
            d->cell_min_idx = k;
        }
        if (n == 0 || mv > d->cell_max_mv) {
            d->cell_max_mv = mv;
            d->cell_max_idx = k;
        }
        sum += mv;
        n++;
    }
    d->cells_valid = n;
    if (n > 0) {
        d->cell_avg_mv = (uint16_t)(sum / n);
        d->cell_delta_mv = (uint16_t)(d->cell_max_mv - d->cell_min_mv);
    }
}

// The TLV walk covers [11, len - 9). An unknown ID stops it (newer firmware may
// add fields); a TLV that overruns the info section invalidates the frame.
// On TLV errors, info->stop_offset/stop_id name the offending TLV.
jk_err_t jk_parse_read_all(const uint8_t *frame, size_t len, jk_data_t *out, jk_parse_info_t *info)
{
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
    if (out == NULL) {
        return JK_ERR_ARG;
    }
    jk_err_t err = jk_validate_reply(frame, len);
    if (err != JK_OK) {
        return err;
    }
    if (frame[8] != JK_CMD_READ_ALL) {
        return JK_ERR_COMMAND;
    }
    memset(out, 0, sizeof(*out));

    const size_t end = len - JK_TRAILER_LEN;
    size_t i = JK_HEADER_LEN;
    uint8_t stop_id = 0;
    uint8_t cells_reported = 0;
    while (i < end) {
        const uint8_t id = frame[i];
        if (id == JK_ID_CELLS) {
            if (i + 2 > end) {
                err = JK_ERR_TLV_BOUNDS;
            } else if (frame[i + 1] % 3 != 0) {
                err = JK_ERR_CELLS;
            } else if (i + 2 + frame[i + 1] > end) {
                err = JK_ERR_TLV_BOUNDS;
            }
            if (err != JK_OK) {
                stop_id = id;
                break;
            }
            const size_t n = frame[i + 1];
            decode_cells(out, &frame[i + 2], n / 3);
            cells_reported = (uint8_t)(n / 3);
            i += 2 + n;
            continue;
        }
        const size_t l = tlv_len(id);
        if (l == 0) {
            stop_id = id;   // unknown ID: stop, keep what was decoded
            break;
        }
        if (i + 1 + l > end) {
            err = JK_ERR_TLV_BOUNDS;
            stop_id = id;
            break;
        }
        decode_tlv(out, id, &frame[i + 1]);
        i += 1 + l;
    }
    const bool clean_end = err == JK_OK && i == end;
    if (info != NULL) {
        info->clean_end = clean_end;
        info->stop_offset = (uint16_t)i;
        info->stop_id = stop_id;
        info->cells_reported = cells_reported;
    }
    if (err != JK_OK) {
        return err;
    }

    if (out->present & JK_HAS_CURRENT_RAW) {
        out->current_valid = jk_decode_current(out->current_raw, (out->present & JK_HAS_PROTOCOL) != 0,
                                               out->protocol_version, clean_end, &out->current_10ma);
        if (out->current_valid) {
            out->power_dw = jk_power_dw(out->voltage_10mv, out->current_10ma);
        }
    }
    cell_stats(out);

    if ((out->present & JK_REQUIRED_FIELDS) != JK_REQUIRED_FIELDS) {
        return JK_ERR_MISSING;
    }
    return JK_OK;
}

// ---- Read-all encoder ----

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     overflow;
} jk_writer_t;

static void w_u8(jk_writer_t *w, uint8_t v)
{
    if (w->pos < w->cap) {
        w->buf[w->pos] = v;
    } else {
        w->overflow = true;
    }
    w->pos++;
}

static void w_u16(jk_writer_t *w, uint16_t v)
{
    w_u8(w, (uint8_t)(v >> 8));
    w_u8(w, (uint8_t)v);
}

static void w_u32(jk_writer_t *w, uint32_t v)
{
    w_u16(w, (uint16_t)(v >> 16));
    w_u16(w, (uint16_t)v);
}

// String field, NUL-padded to its fixed length
static void w_str(jk_writer_t *w, const char *s, size_t field_len)
{
    size_t k = 0;
    for (; k < field_len && s[k] != '\0'; k++) {
        w_u8(w, (uint8_t)s[k]);
    }
    for (; k < field_len; k++) {
        w_u8(w, 0x00);
    }
}

// Inverse of jk_decode_temp
static uint16_t encode_temp(int16_t c)
{
    return c >= 0 ? (uint16_t)c : (uint16_t)(99 - (int32_t)c);
}

size_t jk_encode_read_all_reply(const jk_data_t *d, uint8_t *buf, size_t cap)
{
    if (d == NULL || buf == NULL) {
        return 0;
    }
    jk_writer_t w = {.buf = buf, .cap = cap, .pos = 0, .overflow = false};
    const uint32_t p = d->present;

    w_u8(&w, JK_START_0);
    w_u8(&w, JK_START_1);
    w_u16(&w, 0);           // LEN, patched below
    w_u32(&w, 0);           // terminal number
    w_u8(&w, JK_CMD_READ_ALL);
    w_u8(&w, JK_SRC_BMS);
    w_u8(&w, JK_TYPE_REPLY);

    if (p & JK_HAS_CELLS) {
        const uint8_t n = d->cell_count < JK_MAX_CELLS ? d->cell_count : JK_MAX_CELLS;
        w_u8(&w, JK_ID_CELLS);
        w_u8(&w, (uint8_t)(3 * n));
        for (uint8_t k = 0; k < n; k++) {
            w_u8(&w, (uint8_t)(k + 1));
            w_u16(&w, d->cell_mv[k]);
        }
    }
    if (p & JK_HAS_TEMP_MOS) {
        w_u8(&w, 0x80);
        w_u16(&w, encode_temp(d->temp_mos_c));
    }
    if (p & JK_HAS_TEMP_1) {
        w_u8(&w, 0x81);
        w_u16(&w, encode_temp(d->temp_1_c));
    }
    if (p & JK_HAS_TEMP_2) {
        w_u8(&w, 0x82);
        w_u16(&w, encode_temp(d->temp_2_c));
    }
    if (p & JK_HAS_VOLTAGE) {
        w_u8(&w, 0x83);
        w_u16(&w, d->voltage_10mv);
    }
    if (p & JK_HAS_CURRENT_RAW) {
        w_u8(&w, 0x84);
        w_u16(&w, d->current_raw);
    }
    if (p & JK_HAS_SOC) {
        w_u8(&w, 0x85);
        w_u8(&w, d->soc_pct);
    }
    if (p & JK_HAS_TEMP_COUNT) {
        w_u8(&w, 0x86);
        w_u8(&w, d->temp_sensor_count);
    }
    if (p & JK_HAS_CYCLES) {
        w_u8(&w, 0x87);
        w_u16(&w, d->cycles);
    }
    if (p & JK_HAS_CYCLE_AH) {
        w_u8(&w, 0x89);
        w_u32(&w, d->cycle_capacity_ah);
    }
    if (p & JK_HAS_CELL_SETTING) {
        w_u8(&w, 0x8A);
        w_u16(&w, d->cell_count_setting);
    }
    if (p & JK_HAS_ALARMS) {
        w_u8(&w, 0x8B);
        w_u16(&w, d->alarms);
    }
    if (p & JK_HAS_STATUS) {
        w_u8(&w, 0x8C);
        w_u16(&w, d->status);
    }
    if (p & JK_HAS_BAL_SWITCH) {
        w_u8(&w, 0x9D);
        w_u8(&w, d->balancer_switch ? 1 : 0);
    }
    if (p & JK_HAS_CAPACITY) {
        w_u8(&w, 0xAA);
        w_u32(&w, d->capacity_ah);
    }
    if (p & JK_HAS_CHG_SWITCH) {
        w_u8(&w, 0xAB);
        w_u8(&w, d->charge_switch ? 1 : 0);
    }
    if (p & JK_HAS_DSG_SWITCH) {
        w_u8(&w, 0xAC);
        w_u8(&w, d->discharge_switch ? 1 : 0);
    }
    if (p & JK_HAS_DEVICE_ID) {
        w_u8(&w, 0xB4);
        w_str(&w, d->device_id, JK_LEN_DEVICE_ID);
    }
    if (p & JK_HAS_UPTIME) {
        w_u8(&w, 0xB6);
        w_u32(&w, d->uptime_min);
    }
    if (p & JK_HAS_SW_VERSION) {
        w_u8(&w, 0xB7);
        w_str(&w, d->sw_version, JK_LEN_SW_VERSION);
    }
    if (p & JK_HAS_ACTUAL_AH) {
        w_u8(&w, 0xB9);
        w_u32(&w, d->actual_capacity_ah);
    }
    if (p & JK_HAS_MANUFACTURER) {
        w_u8(&w, 0xBA);
        w_str(&w, d->manufacturer, JK_LEN_MANUFACTURER);
    }
    if (p & JK_HAS_PROTOCOL) {
        w_u8(&w, 0xC0);
        w_u8(&w, d->protocol_version);
    }

    w_u32(&w, 0);           // record number
    w_u8(&w, JK_END_MARK);
    w_u16(&w, 0);           // unused CRC half
    w_u16(&w, 0);           // sum16, patched below
    if (w.overflow || w.pos > JK_FRAME_MAX) {
        return 0;
    }
    const size_t total = w.pos;
    put_be16(&buf[2], (uint16_t)(total - 2));
    put_be16(&buf[total - 2], jk_sum16(buf, total - 4));
    return total;
}

// ---- Discharge output model ----

bool jk_dsg_on(const jk_data_t *d)
{
    if (d == NULL) {
        return false;
    }
    const bool setting_on = (d->present & JK_HAS_DSG_SWITCH) != 0 && d->discharge_switch;
    return setting_on || (d->status & JK_STATUS_DSG_MOS) != 0;
}

jk_dsg_state_t jk_dsg_state(const jk_data_t *d)
{
    if (d == NULL) {
        return JK_DSG_OFF;
    }
    if (d->status & JK_STATUS_DSG_MOS) {
        return JK_DSG_ON;
    }
    if ((d->present & JK_HAS_DSG_SWITCH) != 0 && d->discharge_switch) {
        return JK_DSG_TRIP;
    }
    return JK_DSG_OFF;
}

// ---- Discharge-switch command confirmation ----

bool jk_dsg_cmd_readback(jk_dsg_cmd_t *c, bool target, const jk_data_t *d)
{
    if (c == NULL || d == NULL) {
        return false;
    }
    c->readback = true;
    c->state = jk_dsg_state(d);
    c->confirmed = (jk_dsg_on(d) == target);
    if (!c->confirmed) {
        c->trip_frames = 0;
        c->settled = false;
        return false;
    }
    if (target && c->state == JK_DSG_TRIP) {
        // The setting took effect but the MOSFET is still off: a protection,
        // or the MOSFET has not followed yet. Settle only if it persists.
        if (c->trip_frames < JK_DSG_TRIP_FRAMES) {
            c->trip_frames++;
        }
        c->settled = (c->trip_frames >= JK_DSG_TRIP_FRAMES);
        return c->settled;
    }
    c->trip_frames = 0;
    c->settled = true;
    return true;
}

jk_dsg_cmd_result_t jk_dsg_cmd_result(const jk_dsg_cmd_t *c)
{
    if (c == NULL || !c->readback) {
        return (c != NULL && c->acked) ? JK_DSG_CMD_UNKNOWN : JK_DSG_CMD_NO_REPLY;
    }
    return c->confirmed ? JK_DSG_CMD_OK : JK_DSG_CMD_NOT_CONFIRMED;
}

// ---- Alarms (0x8B) ----

static const char *const s_alarm_names[JK_ALARM_BITS] = {
    [0]  = "Low capacity",
    [1]  = "MOSFET overtemperature",
    [2]  = "Charge overvoltage",
    [3]  = "Discharge undervoltage",
    [4]  = "Battery overtemperature",
    [5]  = "Charge overcurrent",
    [6]  = "Discharge overcurrent",
    [7]  = "Cell voltage difference",
    [8]  = "Battery-box overtemperature",
    [9]  = "Battery undertemperature",
    [10] = "Cell overvoltage",
    [11] = "Cell undervoltage",
    [12] = "309_A protection",
    [13] = "309_B protection",
};

static const char *const s_alarm_short_names[JK_ALARM_BITS] = {
    [0]  = "Low capacity",
    [1]  = "MOS overtemp",
    [2]  = "Chg overvolt",
    [3]  = "Dsg undervolt",
    [4]  = "Batt overtemp",
    [5]  = "Chg overcurr",
    [6]  = "Dsg overcurr",
    [7]  = "Cell diff",
    [8]  = "Box overtemp",
    [9]  = "Batt undertemp",
    [10] = "Cell overvolt",
    [11] = "Cell undervolt",
    [12] = "309_A prot.",
    [13] = "309_B prot.",
};

const char *jk_alarm_name(unsigned bit)
{
    return bit < JK_ALARM_BITS ? s_alarm_names[bit] : NULL;
}

const char *jk_alarm_short_name(unsigned bit)
{
    return bit < JK_ALARM_BITS ? s_alarm_short_names[bit] : NULL;
}

bool jk_alarm_is_warning(unsigned bit)
{
    return bit == 0;
}

int jk_alarm_top(uint16_t alarms)
{
    for (unsigned bit = 1; bit < JK_ALARM_BITS; bit++) {
        if (alarms & (1u << bit)) {
            return (int)bit;
        }
    }
    return (alarms & 1u) ? 0 : -1;
}

unsigned jk_alarm_count(uint16_t alarms)
{
    unsigned n = 0;
    for (unsigned bit = 0; bit < JK_ALARM_BITS; bit++) {
        if (alarms & (1u << bit)) {
            n++;
        }
    }
    return n;
}
