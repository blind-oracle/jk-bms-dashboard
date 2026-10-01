// Host unit tests for src/jk_proto.c:  pio test -e native
//
// Real captures. Each was fetched from its source and decoded independently
// (Python) to derive the expected values asserted below.
//   k_fake14        syssi/esphome-jk-bms tests/esp8266-fake-bms.yaml: 14S, +2.08 A, sum16 0x54D1
//   k_issue341      syssi/esphome-jk-bms issue #341: JK_B2A24S15P hw 11.XW sw 11.26, 16S, 0 A, 0x564C
//   k_mpp_solar     jblance/mpp-solar mppsolar/protocols/jkserial.py test_responses: 14S, sw 11.261,
//                   0x51D6. mpp-solar polls with command 0x03 (register 0x00), so this reply carries
//                   command 0x03, not 0x06.
//   k_pdf_old20     syssi docs/"RS485 Communication example.pdf" (official JK example): old 20S
//                   layout ending after 0xB7 (no B8/B9/BA/C0), 0x84 = 0x2710. The printed checksum
//                   0x4923 is wrong: the bytes sum to 0x485C, which is used here.
//   k_b2a24s_1007   syssi docs/pdus/JK-B2A24S20P-10.07-UART-TTL.txt: 13S, balancing, MOSFETs off, 0x544E
//   k_bd6a17s_710h  syssi docs/pdus/JK-BD6A17S6P-7.1.0H-UART-TTL.txt: 16S, -0.69 A, 0x4AB1. The text
//                   file drops the 0xA0 TLV ID ("0" instead of "A0"); with 0xA0 restored the bytes
//                   match the frame's own checksum.
//   k_ack_ac, k_ack_ab, k_read_ab_reply: write acks and a read-one reply captured in issue #341
//                   (the 0xAC/0xAB acks also appear in the PR #343 thread); k_ack_9d: PR #343.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "jk_proto.h"

// Every JK_HAS_* bit
#define ALL_FIELDS ((1u << 23) - 1u)
// The old 20S layout has no 0xB9, 0xBA or 0xC0
#define OLD_LAYOUT_FIELDS (ALL_FIELDS & ~(JK_HAS_ACTUAL_AH | JK_HAS_MANUFACTURER | JK_HAS_PROTOCOL))

// ---------------------------------------------------------------------------
// Real read-all replies
// ---------------------------------------------------------------------------

// syssi tests/esp8266-fake-bms.yaml (line 38)
static const uint8_t k_fake14[285] = {
    0x4E, 0x57, 0x01, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x79, 0x2A, 0x01, 0x0E, 0xED,
    0x02, 0x0E, 0xFA, 0x03, 0x0E, 0xF7, 0x04, 0x0E, 0xEC, 0x05, 0x0E, 0xF8, 0x06, 0x0E, 0xFA, 0x07,
    0x0E, 0xF1, 0x08, 0x0E, 0xF8, 0x09, 0x0E, 0xE3, 0x0A, 0x0E, 0xFA, 0x0B, 0x0E, 0xF1, 0x0C, 0x0E,
    0xFB, 0x0D, 0x0E, 0xFB, 0x0E, 0x0E, 0xF2, 0x80, 0x00, 0x1D, 0x81, 0x00, 0x1E, 0x82, 0x00, 0x1C,
    0x83, 0x14, 0xEF, 0x84, 0x80, 0xD0, 0x85, 0x0F, 0x86, 0x02, 0x87, 0x00, 0x04, 0x89, 0x00, 0x00,
    0x00, 0x00, 0x8A, 0x00, 0x0E, 0x8B, 0x00, 0x00, 0x8C, 0x00, 0x07, 0x8E, 0x16, 0x26, 0x8F, 0x10,
    0xAE, 0x90, 0x0F, 0xD2, 0x91, 0x0F, 0xA0, 0x92, 0x00, 0x05, 0x93, 0x0B, 0xEA, 0x94, 0x0C, 0x1C,
    0x95, 0x00, 0x05, 0x96, 0x01, 0x2C, 0x97, 0x00, 0x07, 0x98, 0x00, 0x03, 0x99, 0x00, 0x05, 0x9A,
    0x00, 0x05, 0x9B, 0x0C, 0xE4, 0x9C, 0x00, 0x08, 0x9D, 0x01, 0x9E, 0x00, 0x5A, 0x9F, 0x00, 0x46,
    0xA0, 0x00, 0x64, 0xA1, 0x00, 0x64, 0xA2, 0x00, 0x14, 0xA3, 0x00, 0x46, 0xA4, 0x00, 0x46, 0xA5,
    0xFF, 0xEC, 0xA6, 0xFF, 0xF6, 0xA7, 0xFF, 0xEC, 0xA8, 0xFF, 0xF6, 0xA9, 0x0E, 0xAA, 0x00, 0x00,
    0x00, 0x0E, 0xAB, 0x01, 0xAC, 0x01, 0xAD, 0x04, 0x11, 0xAE, 0x01, 0xAF, 0x01, 0xB0, 0x00, 0x0A,
    0xB1, 0x14, 0xB2, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00, 0x00, 0x00, 0xB3, 0x00, 0xB4,
    0x49, 0x6E, 0x70, 0x75, 0x74, 0x20, 0x55, 0x73, 0xB5, 0x32, 0x31, 0x30, 0x31, 0xB6, 0x00, 0x00,
    0xE2, 0x00, 0xB7, 0x48, 0x36, 0x2E, 0x58, 0x5F, 0x5F, 0x53, 0x36, 0x2E, 0x31, 0x2E, 0x33, 0x53,
    0x5F, 0x5F, 0xB8, 0x00, 0xB9, 0x00, 0x00, 0x00, 0x00, 0xBA, 0x42, 0x54, 0x33, 0x30, 0x37, 0x32,
    0x30, 0x32, 0x30, 0x31, 0x32, 0x30, 0x30, 0x30, 0x30, 0x32, 0x30, 0x30, 0x35, 0x32, 0x31, 0x30,
    0x30, 0x31, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x54, 0xD1,
};

// syssi issue #341 (uart_debug chunks joined)
static const uint8_t k_issue341[291] = {
    0x4E, 0x57, 0x01, 0x21, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x79, 0x30, 0x01, 0x0C, 0xCA,
    0x02, 0x0C, 0xCC, 0x03, 0x0C, 0xCC, 0x04, 0x0C, 0xCD, 0x05, 0x0C, 0xCC, 0x06, 0x0C, 0xCD, 0x07,
    0x0C, 0xCC, 0x08, 0x0C, 0xCD, 0x09, 0x0C, 0xCC, 0x0A, 0x0C, 0xCC, 0x0B, 0x0C, 0xCC, 0x0C, 0x0C,
    0xCC, 0x0D, 0x0C, 0xCD, 0x0E, 0x0C, 0xCD, 0x0F, 0x0C, 0xCC, 0x10, 0x0C, 0xCC, 0x80, 0x00, 0x16,
    0x81, 0x00, 0x12, 0x82, 0x00, 0x12, 0x83, 0x14, 0x79, 0x84, 0x00, 0x00, 0x85, 0x63, 0x86, 0x02,
    0x87, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00, 0x00, 0x8A, 0x00, 0x10, 0x8B, 0x00, 0x00, 0x8C, 0x00,
    0x00, 0x8E, 0x16, 0x80, 0x8F, 0x10, 0x40, 0x90, 0x0E, 0x10, 0x91, 0x0D, 0xDE, 0x92, 0x00, 0x05,
    0x93, 0x0A, 0x28, 0x94, 0x0A, 0x5A, 0x95, 0x00, 0x05, 0x96, 0x01, 0x2C, 0x97, 0x00, 0x96, 0x98,
    0x01, 0x2C, 0x99, 0x00, 0x19, 0x9A, 0x00, 0x1E, 0x9B, 0x0B, 0xB8, 0x9C, 0x00, 0x0A, 0x9D, 0x00,
    0x9E, 0x00, 0x64, 0x9F, 0x00, 0x50, 0xA0, 0x00, 0x64, 0xA1, 0x00, 0x64, 0xA2, 0x00, 0x14, 0xA3,
    0x00, 0x46, 0xA4, 0x00, 0x46, 0xA5, 0xFF, 0xEC, 0xA6, 0xFF, 0xF6, 0xA7, 0xFF, 0xEC, 0xA8, 0xFF,
    0xF6, 0xA9, 0x10, 0xAA, 0x00, 0x00, 0x00, 0xE6, 0xAB, 0x00, 0xAC, 0x00, 0xAD, 0x04, 0x1C, 0xAE,
    0x01, 0xAF, 0x00, 0xB0, 0x00, 0x0A, 0xB1, 0x14, 0xB2, 0x33, 0x30, 0x31, 0x36, 0x35, 0x33, 0x00,
    0x00, 0x00, 0x00, 0xB3, 0x00, 0xB4, 0x49, 0x6E, 0x70, 0x75, 0x74, 0x20, 0x55, 0x73, 0xB5, 0x32,
    0x33, 0x30, 0x36, 0xB6, 0x00, 0x00, 0x10, 0xCF, 0xB7, 0x31, 0x31, 0x2E, 0x58, 0x57, 0x5F, 0x53,
    0x31, 0x31, 0x2E, 0x32, 0x36, 0x5F, 0x5F, 0x5F, 0xB8, 0x00, 0xB9, 0x00, 0x00, 0x00, 0xE6, 0xBA,
    0x49, 0x6E, 0x70, 0x75, 0x74, 0x20, 0x55, 0x73, 0x65, 0x72, 0x64, 0x61, 0x31, 0x36, 0x53, 0x2D,
    0x32, 0x33, 0x30, 0x41, 0x48, 0x2D, 0x31, 0x00, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00,
    0x00, 0x56, 0x4C,
};

// mpp-solar jkserial.py test_responses[1] (command 0x03)
static const uint8_t k_mpp_solar[285] = {
    0x4E, 0x57, 0x01, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x79, 0x2A, 0x01, 0x0F, 0x90,
    0x02, 0x0F, 0x91, 0x03, 0x0F, 0x94, 0x04, 0x0F, 0x8E, 0x05, 0x0F, 0x92, 0x06, 0x0F, 0x91, 0x07,
    0x0F, 0x91, 0x08, 0x0F, 0x91, 0x09, 0x0F, 0x93, 0x0A, 0x0F, 0x8E, 0x0B, 0x0F, 0x91, 0x0C, 0x0F,
    0x90, 0x0D, 0x0F, 0x90, 0x0E, 0x0F, 0x8D, 0x80, 0x00, 0x21, 0x81, 0x00, 0x1C, 0x82, 0x00, 0x1E,
    0x83, 0x15, 0xCA, 0x84, 0x81, 0xC5, 0x85, 0x64, 0x86, 0x02, 0x87, 0x00, 0x19, 0x89, 0x00, 0x00,
    0x16, 0xDA, 0x8A, 0x00, 0x0E, 0x8B, 0x00, 0x00, 0x8C, 0x00, 0x03, 0x8E, 0x16, 0xB2, 0x8F, 0x10,
    0xF4, 0x90, 0x10, 0x36, 0x91, 0x10, 0x04, 0x92, 0x00, 0x05, 0x93, 0x0C, 0x1C, 0x94, 0x0C, 0x80,
    0x95, 0x00, 0x05, 0x96, 0x01, 0x2C, 0x97, 0x00, 0x6E, 0x98, 0x01, 0x2C, 0x99, 0x00, 0x55, 0x9A,
    0x00, 0x1E, 0x9B, 0x0B, 0xB8, 0x9C, 0x00, 0x32, 0x9D, 0x01, 0x9E, 0x00, 0x5A, 0x9F, 0x00, 0x46,
    0xA0, 0x00, 0x64, 0xA1, 0x00, 0x64, 0xA2, 0x00, 0x14, 0xA3, 0x00, 0x3C, 0xA4, 0x00, 0x3C, 0xA5,
    0x00, 0x01, 0xA6, 0x00, 0x03, 0xA7, 0xFF, 0xEC, 0xA8, 0xFF, 0xF6, 0xA9, 0x0E, 0xAA, 0x00, 0x00,
    0x00, 0xEA, 0xAB, 0x01, 0xAC, 0x01, 0xAD, 0x04, 0x37, 0xAE, 0x01, 0xAF, 0x01, 0xB0, 0x00, 0x0A,
    0xB1, 0x14, 0xB2, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00, 0x00, 0x00, 0xB3, 0x00, 0xB4,
    0x49, 0x6E, 0x70, 0x75, 0x74, 0x20, 0x55, 0x73, 0xB5, 0x32, 0x33, 0x30, 0x36, 0xB6, 0x00, 0x01,
    0x82, 0xE3, 0xB7, 0x31, 0x31, 0x2E, 0x58, 0x57, 0x5F, 0x53, 0x31, 0x31, 0x2E, 0x32, 0x36, 0x31,
    0x5F, 0x5F, 0xB8, 0x00, 0xB9, 0x00, 0x00, 0x00, 0xEA, 0xBA, 0x49, 0x6E, 0x70, 0x75, 0x74, 0x20,
    0x55, 0x73, 0x65, 0x72, 0x64, 0x61, 0x4A, 0x4B, 0x5F, 0x42, 0x31, 0x41, 0x32, 0x30, 0x53, 0x31,
    0x35, 0x50, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x51, 0xD6,
};

// JK "RS485 Communication example.pdf", checksum recomputed: 0x485C (printed: 0x4923)
static const uint8_t k_pdf_old20[269] = {
    0x4E, 0x57, 0x01, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x79, 0x3C, 0x01, 0x0E, 0xF9,
    0x02, 0x0E, 0xF8, 0x03, 0x0F, 0x01, 0x04, 0x0F, 0x03, 0x05, 0x0F, 0x02, 0x06, 0x0F, 0x05, 0x07,
    0x0F, 0x02, 0x08, 0x0F, 0x05, 0x09, 0x0E, 0xFB, 0x0A, 0x0E, 0xC8, 0x0B, 0x0E, 0xCB, 0x0C, 0x0E,
    0x9A, 0x0D, 0x0E, 0xC5, 0x0E, 0x0E, 0xC6, 0x0F, 0x0E, 0xCB, 0x10, 0x0E, 0xC1, 0x11, 0x0E, 0xCD,
    0x12, 0x0E, 0xCB, 0x13, 0x0E, 0xBC, 0x14, 0x0E, 0xC2, 0x80, 0x00, 0x1B, 0x81, 0x00, 0x1E, 0x82,
    0x00, 0x1E, 0x83, 0x1D, 0xBC, 0x84, 0x27, 0x10, 0x85, 0x47, 0x86, 0x02, 0x87, 0x00, 0x01, 0x89,
    0x00, 0x00, 0x00, 0x00, 0x8A, 0x00, 0x14, 0x8B, 0x00, 0x00, 0x8C, 0x00, 0x0B, 0x8E, 0x20, 0xD0,
    0x8F, 0x15, 0xE0, 0x90, 0x10, 0x68, 0x91, 0x10, 0x36, 0x92, 0x00, 0x04, 0x93, 0x0A, 0xF0, 0x94,
    0x0B, 0x54, 0x95, 0x00, 0x04, 0x96, 0x01, 0x2C, 0x97, 0x00, 0x28, 0x98, 0x00, 0x04, 0x99, 0x00,
    0x14, 0x9A, 0x00, 0x04, 0x9B, 0x10, 0x36, 0x9C, 0x00, 0x64, 0x9D, 0x00, 0x9E, 0x00, 0x64, 0x9F,
    0x00, 0x50, 0xA0, 0x00, 0x50, 0xA1, 0x00, 0x46, 0xA2, 0x00, 0x14, 0xA3, 0x00, 0x64, 0xA4, 0x00,
    0x64, 0xA5, 0xFF, 0xEC, 0xA6, 0xFF, 0xF6, 0xA7, 0xFF, 0xEC, 0xA8, 0xFF, 0xF6, 0xA9, 0x14, 0xAA,
    0x00, 0x00, 0x00, 0x28, 0xAB, 0x00, 0xAC, 0x00, 0xAD, 0x03, 0xE8, 0xAE, 0x01, 0xAF, 0x01, 0xB0,
    0x00, 0x0A, 0xB1, 0x14, 0xB2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB3,
    0x01, 0xB4, 0x36, 0x30, 0x33, 0x30, 0x30, 0x30, 0x30, 0x31, 0xB5, 0x32, 0x30, 0x30, 0x34, 0xB6,
    0x00, 0x00, 0x00, 0x01, 0xB7, 0x4E, 0x57, 0x5F, 0x31, 0x5F, 0x30, 0x5F, 0x30, 0x5F, 0x32, 0x30,
    0x30, 0x34, 0x32, 0x38, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x48, 0x5C,
};

// syssi docs/pdus/JK-B2A24S20P-10.07-UART-TTL.txt
static const uint8_t k_b2a24s_1007[282] = {
    0x4E, 0x57, 0x01, 0x18, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x79, 0x27, 0x01, 0x0F, 0xFC,
    0x02, 0x0F, 0xCF, 0x03, 0x0F, 0xFD, 0x04, 0x0F, 0xFC, 0x05, 0x0F, 0xFC, 0x06, 0x0F, 0xFA, 0x07,
    0x0F, 0xF7, 0x08, 0x0F, 0xFE, 0x09, 0x0F, 0xFE, 0x0A, 0x0F, 0xFC, 0x0B, 0x0F, 0xF7, 0x0C, 0x0F,
    0xF7, 0x0D, 0x0F, 0xFD, 0x80, 0x00, 0x16, 0x81, 0x00, 0x13, 0x82, 0x00, 0x13, 0x83, 0x14, 0xC1,
    0x84, 0x00, 0x00, 0x85, 0x5E, 0x86, 0x02, 0x87, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00, 0x00, 0x8A,
    0x00, 0x0D, 0x8B, 0x00, 0x00, 0x8C, 0x00, 0x04, 0x8E, 0x15, 0x54, 0x8F, 0x0E, 0xBA, 0x90, 0x10,
    0x68, 0x91, 0x10, 0x04, 0x92, 0x00, 0x05, 0x93, 0x0B, 0x54, 0x94, 0x0C, 0x80, 0x95, 0x00, 0x05,
    0x96, 0x01, 0x2C, 0x97, 0x00, 0x96, 0x98, 0x01, 0x2C, 0x99, 0x00, 0x19, 0x9A, 0x00, 0x1E, 0x9B,
    0x0C, 0xE4, 0x9C, 0x00, 0x0A, 0x9D, 0x01, 0x9E, 0x00, 0x5A, 0x9F, 0x00, 0x46, 0xA0, 0x00, 0x64,
    0xA1, 0x00, 0x64, 0xA2, 0x00, 0x14, 0xA3, 0x00, 0x46, 0xA4, 0x00, 0x46, 0xA5, 0xFF, 0xEC, 0xA6,
    0xFF, 0xF6, 0xA7, 0xFF, 0xEC, 0xA8, 0xFF, 0xF6, 0xA9, 0x0D, 0xAA, 0x00, 0x00, 0x00, 0x05, 0xAB,
    0x00, 0xAC, 0x00, 0xAD, 0x04, 0x2E, 0xAE, 0x01, 0xAF, 0x01, 0xB0, 0x00, 0x0A, 0xB1, 0x14, 0xB2,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00, 0x00, 0x00, 0xB3, 0x00, 0xB4, 0x49, 0x6E, 0x70,
    0x75, 0x74, 0x20, 0x55, 0x73, 0xB5, 0x32, 0x32, 0x30, 0x34, 0xB6, 0x00, 0x00, 0x00, 0x00, 0xB7,
    0x31, 0x30, 0x2E, 0x58, 0x57, 0x5F, 0x53, 0x31, 0x30, 0x2E, 0x30, 0x37, 0x5F, 0x5F, 0x5F, 0xB8,
    0x00, 0xB9, 0x00, 0x00, 0x00, 0x00, 0xBA, 0x49, 0x6E, 0x70, 0x75, 0x74, 0x20, 0x55, 0x73, 0x65,
    0x72, 0x64, 0x61, 0x4A, 0x4B, 0x2D, 0x42, 0x32, 0x41, 0x32, 0x34, 0x53, 0x31, 0x35, 0x50, 0xC0,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x54, 0x4E,
};

// syssi docs/pdus/JK-BD6A17S6P-7.1.0H-UART-TTL.txt (0xA0 ID restored)
static const uint8_t k_bd6a17s_710h[291] = {
    0x4E, 0x57, 0x01, 0x21, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x79, 0x30, 0x01, 0x0C, 0x81,
    0x02, 0x0C, 0x81, 0x03, 0x0C, 0x82, 0x04, 0x0C, 0x81, 0x05, 0x0C, 0x83, 0x06, 0x0C, 0x81, 0x07,
    0x0C, 0x71, 0x08, 0x0C, 0x81, 0x09, 0x0C, 0x7C, 0x0A, 0x0C, 0x83, 0x0B, 0x0C, 0x82, 0x0C, 0x0C,
    0x83, 0x0D, 0x0C, 0x83, 0x0E, 0x0C, 0x83, 0x0F, 0x0C, 0x83, 0x10, 0x0C, 0x82, 0x80, 0x00, 0x12,
    0x81, 0x00, 0x10, 0x82, 0x00, 0x10, 0x83, 0x14, 0x01, 0x84, 0x00, 0x45, 0x85, 0x0F, 0x86, 0x02,
    0x87, 0x00, 0x11, 0x89, 0x00, 0x00, 0x05, 0x00, 0x8A, 0x00, 0x10, 0x8B, 0x00, 0x00, 0x8C, 0x00,
    0x03, 0x8E, 0x16, 0xD0, 0x8F, 0x10, 0x90, 0x90, 0x0E, 0x42, 0x91, 0x0D, 0xDE, 0x92, 0x00, 0x05,
    0x93, 0x0A, 0x5A, 0x94, 0x0A, 0xBE, 0x95, 0x00, 0x05, 0x96, 0x01, 0x2C, 0x97, 0x00, 0x3C, 0x98,
    0x01, 0x2C, 0x99, 0x00, 0x1E, 0x9A, 0x00, 0x1E, 0x9B, 0x0D, 0x7A, 0x9C, 0x00, 0x0A, 0x9D, 0x01,
    0x9E, 0x00, 0x5A, 0x9F, 0x00, 0x46, 0xA0, 0x00, 0x64, 0xA1, 0x00, 0x64, 0xA2, 0x00, 0x14, 0xA3,
    0x00, 0x46, 0xA4, 0x00, 0x46, 0xA5, 0x00, 0x00, 0xA6, 0x00, 0x05, 0xA7, 0xFF, 0xEC, 0xA8, 0xFF,
    0xF6, 0xA9, 0x10, 0xAA, 0x00, 0x00, 0x00, 0x51, 0xAB, 0x01, 0xAC, 0x01, 0xAD, 0x02, 0xD5, 0xAE,
    0x01, 0xAF, 0x01, 0xB0, 0x00, 0x0A, 0xB1, 0x14, 0xB2, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00,
    0x00, 0x00, 0x00, 0xB3, 0x00, 0xB4, 0x49, 0x6E, 0x70, 0x75, 0x74, 0x20, 0x55, 0x73, 0xB5, 0x32,
    0x31, 0x30, 0x36, 0xB6, 0x00, 0x01, 0x64, 0x00, 0xB7, 0x48, 0x37, 0x2E, 0x58, 0x5F, 0x5F, 0x53,
    0x37, 0x2E, 0x31, 0x2E, 0x30, 0x48, 0x5F, 0x5F, 0xB8, 0x00, 0xB9, 0x00, 0x00, 0x00, 0x00, 0xBA,
    0x42, 0x54, 0x33, 0x30, 0x37, 0x32, 0x30, 0x32, 0x30, 0x31, 0x32, 0x30, 0x30, 0x30, 0x30, 0x32,
    0x30, 0x30, 0x35, 0x32, 0x31, 0x30, 0x30, 0x31, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00,
    0x00, 0x4A, 0xB1,
};

// ---------------------------------------------------------------------------
// Requests (source 0x02 = GPS unless noted) and acknowledgements
// ---------------------------------------------------------------------------

static const uint8_t k_req_read_all_gps[21] = {
    0x4E, 0x57, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x06, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0x28,
};
// Source 0x03 (PC): byte-identical to the requests in the JK PDF and in issue #341
static const uint8_t k_req_read_all_pc[21] = {
    0x4E, 0x57, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x06, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0x29,
};
static const uint8_t k_req_activate_gps[21] = {
    0x4E, 0x57, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0x23,
};
static const uint8_t k_req_password_gps[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x05, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0x28,
};
static const uint8_t k_req_dsg_on_gps[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00,
    0xAC, 0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xD2,
};
static const uint8_t k_req_dsg_off_gps[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00,
    0xAC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xD1,
};
// Captured in issue #341 ("Write 0x0AC = 1", source PC)
static const uint8_t k_req_dsg_on_pc[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00,
    0xAC, 0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xD3,
};
static const uint8_t k_req_dsg_off_pc[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00,
    0xAC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xD2,
};
static const uint8_t k_req_chg_on_gps[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00,
    0xAB, 0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xD1,
};
static const uint8_t k_req_chg_off_gps[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00,
    0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xD0,
};
// Captured in issue #341 ("Write 0x0AB = 1", source PC)
static const uint8_t k_req_chg_on_pc[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00,
    0xAB, 0x01, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xD2,
};

// Write acks captured in issue #341: 21 bytes, command 0x02, info = register
static const uint8_t k_ack_ac[21] = {
    0x4E, 0x57, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
    0xAC, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xCF,
};
static const uint8_t k_ack_ab[21] = {
    0x4E, 0x57, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
    0xAB, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xCE,
};
// Balancer-switch (0x9D) ack captured in the esphome PR #343 thread
static const uint8_t k_ack_9d[21] = {
    0x4E, 0x57, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
    0x9D, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xC0,
};
// Read-one (0x03) reply for 0xAB from issue #341: valid, but not a write ack
static const uint8_t k_read_ab_reply[22] = {
    0x4E, 0x57, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01,
    0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0xD0,
};

// Info-section pieces for synthetic frames
static const uint8_t k_tlv_cells2[] = {0x79, 0x06, 0x01, 0x0C, 0xE4, 0x02, 0x0C, 0xEE}; // 3300, 3310 mV
static const uint8_t k_tlv_volt[] = {0x83, 0x0A, 0x50};    // 26.40 V
static const uint8_t k_tlv_curr[] = {0x84, 0x80, 0x64};    // +1.00 A with C0 = 1
static const uint8_t k_tlv_soc[] = {0x85, 0x32};           // 50 %
static const uint8_t k_tlv_status[] = {0x8C, 0x00, 0x03};  // CHG + DSG MOSFET on
static const uint8_t k_tlv_proto[] = {0xC0, 0x01};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint8_t s_frame[JK_FRAME_MAX + 64];
static uint8_t s_out[JK_FRAME_MAX];
static uint8_t s_big[1600];
static jk_rx_t s_rx;

// Reference checksum, independent of the code under test
static uint16_t ref_sum16(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s += p[i];
    }
    return (uint16_t)(s & 0xFFFFu);
}

// Recompute the checksum after editing a frame in place
static void fix_sum(uint8_t *f, size_t len)
{
    const uint16_t s = ref_sum16(f, len - 4);
    f[len - 2] = (uint8_t)(s >> 8);
    f[len - 1] = (uint8_t)s;
}

// Recompute LEN and the checksum
static void fix_frame(uint8_t *f, size_t len)
{
    f[2] = (uint8_t)((len - 2) >> 8);
    f[3] = (uint8_t)(len - 2);
    fix_sum(f, len);
}

// Wrap info bytes into a reply frame (source BMS, type reply); returns its length
static size_t make_reply(uint8_t *f, size_t cap, uint8_t cmd, const uint8_t *info, size_t info_len)
{
    const size_t total = JK_FRAME_MIN + info_len;
    TEST_ASSERT_TRUE(total <= cap);
    memset(f, 0, total);
    f[0] = 0x4E;
    f[1] = 0x57;
    f[8] = cmd;
    f[9] = JK_SRC_BMS;
    f[10] = JK_TYPE_REPLY;
    if (info_len > 0) {
        memcpy(&f[11], info, info_len);
    }
    f[total - 5] = 0x68;
    fix_frame(f, total);
    return total;
}

// Copy a frame with n bytes inserted at offset at (LEN and checksum fixed)
static size_t insert_bytes(uint8_t *dst, const uint8_t *src, size_t len, size_t at,
                           const uint8_t *ins, size_t n)
{
    memcpy(dst, src, at);
    memcpy(&dst[at], ins, n);
    memcpy(&dst[at + n], &src[at], len - at);
    fix_frame(dst, len + n);
    return len + n;
}

// Copy a frame with n bytes removed at offset at (LEN and checksum fixed)
static size_t remove_bytes(uint8_t *dst, const uint8_t *src, size_t len, size_t at, size_t n)
{
    memcpy(dst, src, at);
    memcpy(&dst[at], &src[at + n], len - at - n);
    fix_frame(dst, len - n);
    return len - n;
}

static size_t append(uint8_t *buf, size_t pos, const uint8_t *src, size_t n)
{
    memcpy(&buf[pos], src, n);
    return pos + n;
}

// Info section with the required fields plus C0; skip = index of a piece to leave out (-1: none)
static size_t make_required_info(uint8_t *info, int skip)
{
    const uint8_t *parts[] = {k_tlv_cells2, k_tlv_volt, k_tlv_curr, k_tlv_soc, k_tlv_status, k_tlv_proto};
    const size_t sizes[] = {sizeof(k_tlv_cells2), sizeof(k_tlv_volt), sizeof(k_tlv_curr),
                            sizeof(k_tlv_soc), sizeof(k_tlv_status), sizeof(k_tlv_proto)};
    size_t n = 0;
    for (int k = 0; k < 6; k++) {
        if (k != skip) {
            n = append(info, n, parts[k], sizes[k]);
        }
    }
    return n;
}

static void parse_ok(const uint8_t *f, size_t len, jk_data_t *d, jk_parse_info_t *info)
{
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(f, len));
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(f, len, d, info));
}

static void assert_same_data(const jk_data_t *a, const jk_data_t *b)
{
    TEST_ASSERT_EQUAL_HEX32(a->present, b->present);
    TEST_ASSERT_EQUAL_UINT8(a->cell_count, b->cell_count);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(a->cell_mv, b->cell_mv, JK_MAX_CELLS);
    TEST_ASSERT_EQUAL_UINT8(a->cells_valid, b->cells_valid);
    TEST_ASSERT_EQUAL_UINT16(a->cell_min_mv, b->cell_min_mv);
    TEST_ASSERT_EQUAL_UINT16(a->cell_max_mv, b->cell_max_mv);
    TEST_ASSERT_EQUAL_UINT16(a->cell_avg_mv, b->cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(a->cell_delta_mv, b->cell_delta_mv);
    TEST_ASSERT_EQUAL_UINT8(a->cell_min_idx, b->cell_min_idx);
    TEST_ASSERT_EQUAL_UINT8(a->cell_max_idx, b->cell_max_idx);
    TEST_ASSERT_EQUAL_INT16(a->temp_mos_c, b->temp_mos_c);
    TEST_ASSERT_EQUAL_INT16(a->temp_1_c, b->temp_1_c);
    TEST_ASSERT_EQUAL_INT16(a->temp_2_c, b->temp_2_c);
    TEST_ASSERT_EQUAL_UINT8(a->temp_sensor_count, b->temp_sensor_count);
    TEST_ASSERT_EQUAL_UINT16(a->voltage_10mv, b->voltage_10mv);
    TEST_ASSERT_EQUAL_HEX16(a->current_raw, b->current_raw);
    TEST_ASSERT_EQUAL(a->current_valid, b->current_valid);
    TEST_ASSERT_EQUAL_INT32(a->current_10ma, b->current_10ma);
    TEST_ASSERT_EQUAL_INT32(a->power_dw, b->power_dw);
    TEST_ASSERT_EQUAL_UINT8(a->soc_pct, b->soc_pct);
    TEST_ASSERT_EQUAL_UINT16(a->cycles, b->cycles);
    TEST_ASSERT_EQUAL_UINT32(a->cycle_capacity_ah, b->cycle_capacity_ah);
    TEST_ASSERT_EQUAL_UINT16(a->cell_count_setting, b->cell_count_setting);
    TEST_ASSERT_EQUAL_HEX16(a->alarms, b->alarms);
    TEST_ASSERT_EQUAL_HEX16(a->status, b->status);
    TEST_ASSERT_EQUAL(a->balancer_switch, b->balancer_switch);
    TEST_ASSERT_EQUAL_UINT32(a->capacity_ah, b->capacity_ah);
    TEST_ASSERT_EQUAL(a->charge_switch, b->charge_switch);
    TEST_ASSERT_EQUAL(a->discharge_switch, b->discharge_switch);
    TEST_ASSERT_EQUAL_UINT32(a->uptime_min, b->uptime_min);
    TEST_ASSERT_EQUAL_UINT32(a->actual_capacity_ah, b->actual_capacity_ah);
    TEST_ASSERT_EQUAL_UINT8(a->protocol_version, b->protocol_version);
    TEST_ASSERT_EQUAL_STRING(a->device_id, b->device_id);
    TEST_ASSERT_EQUAL_STRING(a->sw_version, b->sw_version);
    TEST_ASSERT_EQUAL_STRING(a->manufacturer, b->manufacturer);
}

// parse -> encode -> parse must reproduce every decoded field
static void assert_reencodes(const uint8_t *f, size_t len)
{
    jk_data_t a;
    jk_data_t b;
    jk_parse_info_t info;
    parse_ok(f, len, &a, &info);
    const size_t n = jk_encode_read_all_reply(&a, s_frame, sizeof(s_frame));
    TEST_ASSERT_TRUE(n > 0);
    parse_ok(s_frame, n, &b, &info);
    TEST_ASSERT_TRUE(info.clean_end);
    assert_same_data(&a, &b);
}

void setUp(void)
{
    jk_rx_reset(&s_rx);
}

void tearDown(void)
{
}

// ---------------------------------------------------------------------------
// 1. Builders and checksum
// ---------------------------------------------------------------------------

static void test_build_read_all(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(21, jk_build_read_all(buf, sizeof(buf), JK_SRC_GPS));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_read_all_gps, buf, 21);
    TEST_ASSERT_EQUAL_UINT(21, jk_build_read_all(buf, 21, JK_SRC_PC));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_read_all_pc, buf, 21);
}

static void test_build_activate(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(21, jk_build_activate(buf, sizeof(buf), JK_SRC_GPS));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_activate_gps, buf, 21);
    // Source 0x03: same frame, checksum one higher
    TEST_ASSERT_EQUAL_UINT(21, jk_build_activate(buf, sizeof(buf), JK_SRC_PC));
    TEST_ASSERT_EQUAL_HEX8(0x03, buf[9]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_activate_gps, buf, 9);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&k_req_activate_gps[10], &buf[10], 10);
    TEST_ASSERT_EQUAL_HEX8(0x24, buf[20]);
}

static void test_build_password(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(22, jk_build_password(buf, sizeof(buf), JK_SRC_GPS));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_password_gps, buf, 22);
    TEST_ASSERT_EQUAL_UINT(22, jk_build_password(buf, sizeof(buf), JK_SRC_PC));
    TEST_ASSERT_EQUAL_HEX8(0x03, buf[9]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[20]);
    TEST_ASSERT_EQUAL_HEX8(0x29, buf[21]);
}

static void test_build_write_dsg(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(22, jk_build_write_u8(buf, sizeof(buf), JK_SRC_GPS, JK_REG_DISCHARGE_SWITCH, 1));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_dsg_on_gps, buf, 22);
    TEST_ASSERT_EQUAL_UINT(22, jk_build_write_u8(buf, sizeof(buf), JK_SRC_GPS, JK_REG_DISCHARGE_SWITCH, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_dsg_off_gps, buf, 22);
    TEST_ASSERT_EQUAL_UINT(22, jk_build_write_u8(buf, 22, JK_SRC_PC, JK_REG_DISCHARGE_SWITCH, 1));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_dsg_on_pc, buf, 22);
    TEST_ASSERT_EQUAL_UINT(22, jk_build_write_u8(buf, 22, JK_SRC_PC, JK_REG_DISCHARGE_SWITCH, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_dsg_off_pc, buf, 22);
}

static void test_build_write_chg(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(22, jk_build_write_u8(buf, sizeof(buf), JK_SRC_GPS, JK_REG_CHARGE_SWITCH, 1));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_chg_on_gps, buf, 22);
    TEST_ASSERT_EQUAL_UINT(22, jk_build_write_u8(buf, sizeof(buf), JK_SRC_GPS, JK_REG_CHARGE_SWITCH, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_chg_off_gps, buf, 22);
    TEST_ASSERT_EQUAL_UINT(22, jk_build_write_u8(buf, sizeof(buf), JK_SRC_PC, JK_REG_CHARGE_SWITCH, 1));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_req_chg_on_pc, buf, 22);
}

static void test_build_cap_too_small(void)
{
    uint8_t buf[32];
    memset(buf, 0xA5, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(0, jk_build_read_all(buf, 20, JK_SRC_GPS));
    TEST_ASSERT_EQUAL_UINT(0, jk_build_activate(buf, 20, JK_SRC_GPS));
    TEST_ASSERT_EQUAL_UINT(0, jk_build_password(buf, 21, JK_SRC_GPS));
    TEST_ASSERT_EQUAL_UINT(0, jk_build_write_u8(buf, 21, JK_SRC_GPS, JK_REG_DISCHARGE_SWITCH, 1));
    TEST_ASSERT_EQUAL_UINT(0, jk_build_read_all(NULL, 32, JK_SRC_GPS));
    TEST_ASSERT_EQUAL_HEX8(0xA5, buf[0]);   // nothing written
}

static void test_requests_are_not_replies(void)
{
    // Our own requests (for example an echo on the RX line) never validate as replies
    TEST_ASSERT_EQUAL_INT(JK_ERR_NOT_REPLY, jk_validate_reply(k_req_read_all_gps, sizeof(k_req_read_all_gps)));
    TEST_ASSERT_EQUAL_INT(JK_ERR_NOT_REPLY, jk_validate_reply(k_req_password_gps, sizeof(k_req_password_gps)));
    TEST_ASSERT_EQUAL_INT(JK_ERR_NOT_REPLY, jk_validate_reply(k_req_dsg_on_pc, sizeof(k_req_dsg_on_pc)));
}

static void test_sum16(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x54D1, jk_sum16(k_fake14, sizeof(k_fake14) - 4));
    TEST_ASSERT_EQUAL_HEX16(0x564C, jk_sum16(k_issue341, sizeof(k_issue341) - 4));
    TEST_ASSERT_EQUAL_HEX16(0x51D6, jk_sum16(k_mpp_solar, sizeof(k_mpp_solar) - 4));
    TEST_ASSERT_EQUAL_HEX16(0x485C, jk_sum16(k_pdf_old20, sizeof(k_pdf_old20) - 4));
    TEST_ASSERT_EQUAL_HEX16(0x0128, jk_sum16(k_req_read_all_gps, 17));
    TEST_ASSERT_EQUAL_HEX16(0x01D2, jk_sum16(k_req_dsg_on_gps, 18));
    // Wraps modulo 2^16: 300 * 0xFF = 0x12AD4
    memset(s_big, 0xFF, 300);
    TEST_ASSERT_EQUAL_HEX16(0x2AD4, jk_sum16(s_big, 300));
    TEST_ASSERT_EQUAL_HEX16(0, jk_sum16(s_big, 0));
    TEST_ASSERT_EQUAL_HEX16(0, jk_sum16(NULL, 10));
}

// ---------------------------------------------------------------------------
// 2. Real captures
// ---------------------------------------------------------------------------

static void test_real_fake14(void)
{
    jk_data_t d;
    jk_parse_info_t info;
    TEST_ASSERT_EQUAL_UINT(285, sizeof(k_fake14));
    parse_ok(k_fake14, sizeof(k_fake14), &d, &info);
    TEST_ASSERT_TRUE(info.clean_end);
    TEST_ASSERT_EQUAL_UINT16(276, info.stop_offset);
    TEST_ASSERT_EQUAL_HEX8(0, info.stop_id);
    TEST_ASSERT_EQUAL_UINT8(14, info.cells_reported);
    TEST_ASSERT_EQUAL_HEX32(ALL_FIELDS, d.present);

    TEST_ASSERT_EQUAL_UINT8(14, d.cell_count);
    TEST_ASSERT_EQUAL_UINT16(3821, d.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3811, d.cell_mv[8]);
    TEST_ASSERT_EQUAL_UINT16(3826, d.cell_mv[13]);
    TEST_ASSERT_EQUAL_UINT16(0, d.cell_mv[14]);
    TEST_ASSERT_EQUAL_UINT8(14, d.cells_valid);
    TEST_ASSERT_EQUAL_UINT16(3811, d.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT8(8, d.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT16(3835, d.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT8(11, d.cell_max_idx);    // 3835 at 11 and 12: first occurrence
    TEST_ASSERT_EQUAL_UINT16(3828, d.cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(24, d.cell_delta_mv);

    TEST_ASSERT_EQUAL_INT16(29, d.temp_mos_c);
    TEST_ASSERT_EQUAL_INT16(30, d.temp_1_c);
    TEST_ASSERT_EQUAL_INT16(28, d.temp_2_c);
    TEST_ASSERT_EQUAL_UINT8(2, d.temp_sensor_count);

    TEST_ASSERT_EQUAL_UINT16(5359, d.voltage_10mv);
    TEST_ASSERT_EQUAL_HEX16(0x80D0, d.current_raw);
    TEST_ASSERT_TRUE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(208, d.current_10ma);       // +2.08 A: C0 = 1, bit 15 = charging
    TEST_ASSERT_EQUAL_INT32(1114, d.power_dw);          // 111.4 W

    TEST_ASSERT_EQUAL_UINT8(15, d.soc_pct);
    TEST_ASSERT_EQUAL_UINT16(4, d.cycles);
    TEST_ASSERT_EQUAL_UINT32(0, d.cycle_capacity_ah);
    TEST_ASSERT_EQUAL_UINT16(14, d.cell_count_setting);
    TEST_ASSERT_EQUAL_HEX16(0x0000, d.alarms);
    TEST_ASSERT_EQUAL_HEX16(JK_STATUS_CHG_MOS | JK_STATUS_DSG_MOS | JK_STATUS_BALANCING, d.status);
    TEST_ASSERT_TRUE(d.balancer_switch);
    TEST_ASSERT_EQUAL_UINT32(14, d.capacity_ah);
    TEST_ASSERT_TRUE(d.charge_switch);
    TEST_ASSERT_TRUE(d.discharge_switch);
    TEST_ASSERT_EQUAL_UINT32(57856, d.uptime_min);
    TEST_ASSERT_EQUAL_UINT32(0, d.actual_capacity_ah);
    TEST_ASSERT_EQUAL_UINT8(1, d.protocol_version);
    TEST_ASSERT_EQUAL_STRING("Input Us", d.device_id);
    TEST_ASSERT_EQUAL_STRING("H6.X__S6.1.3S__", d.sw_version);
    TEST_ASSERT_EQUAL_STRING("BT3072020120000200521001", d.manufacturer);

    TEST_ASSERT_TRUE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, jk_dsg_state(&d));
}

static void test_real_issue341_16s(void)
{
    jk_data_t d;
    jk_parse_info_t info;
    TEST_ASSERT_EQUAL_UINT(291, sizeof(k_issue341));
    parse_ok(k_issue341, sizeof(k_issue341), &d, &info);
    TEST_ASSERT_TRUE(info.clean_end);
    TEST_ASSERT_EQUAL_UINT16(282, info.stop_offset);
    TEST_ASSERT_EQUAL_UINT8(16, info.cells_reported);
    TEST_ASSERT_EQUAL_HEX32(ALL_FIELDS, d.present);

    TEST_ASSERT_EQUAL_UINT8(16, d.cell_count);
    TEST_ASSERT_EQUAL_UINT16(3274, d.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3277, d.cell_mv[3]);
    TEST_ASSERT_EQUAL_UINT16(3276, d.cell_mv[15]);
    TEST_ASSERT_EQUAL_UINT8(16, d.cells_valid);
    TEST_ASSERT_EQUAL_UINT16(3274, d.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT8(0, d.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT16(3277, d.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT8(3, d.cell_max_idx);     // 3277 at 3, 5, 7, 12, 13: first occurrence
    TEST_ASSERT_EQUAL_UINT16(3276, d.cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(3, d.cell_delta_mv);

    TEST_ASSERT_EQUAL_INT16(22, d.temp_mos_c);
    TEST_ASSERT_EQUAL_INT16(18, d.temp_1_c);
    TEST_ASSERT_EQUAL_INT16(18, d.temp_2_c);
    TEST_ASSERT_EQUAL_UINT8(2, d.temp_sensor_count);

    TEST_ASSERT_EQUAL_UINT16(5241, d.voltage_10mv);     // 52.41 V
    TEST_ASSERT_EQUAL_HEX16(0x0000, d.current_raw);
    TEST_ASSERT_TRUE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(0, d.current_10ma);
    TEST_ASSERT_EQUAL_INT32(0, d.power_dw);

    TEST_ASSERT_EQUAL_UINT8(99, d.soc_pct);
    TEST_ASSERT_EQUAL_UINT16(0, d.cycles);
    TEST_ASSERT_EQUAL_UINT16(16, d.cell_count_setting);
    TEST_ASSERT_EQUAL_HEX16(0x0000, d.alarms);
    TEST_ASSERT_EQUAL_HEX16(0x0000, d.status);
    TEST_ASSERT_FALSE(d.balancer_switch);
    TEST_ASSERT_EQUAL_UINT32(230, d.capacity_ah);
    TEST_ASSERT_FALSE(d.charge_switch);
    TEST_ASSERT_FALSE(d.discharge_switch);
    TEST_ASSERT_EQUAL_UINT32(4303, d.uptime_min);
    TEST_ASSERT_EQUAL_UINT32(230, d.actual_capacity_ah);
    TEST_ASSERT_EQUAL_UINT8(1, d.protocol_version);
    TEST_ASSERT_EQUAL_STRING("Input Us", d.device_id);
    TEST_ASSERT_EQUAL_STRING("11.XW_S11.26___", d.sw_version);
    TEST_ASSERT_EQUAL_STRING("Input Userda16S-230AH-1", d.manufacturer);    // NUL-padded field

    TEST_ASSERT_FALSE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_OFF, jk_dsg_state(&d));
}

static void test_real_mpp_solar(void)
{
    jk_data_t d;
    jk_parse_info_t info;
    TEST_ASSERT_EQUAL_UINT(285, sizeof(k_mpp_solar));
    // A valid reply, but to command 0x03 (read register 0x00), not read-all
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(k_mpp_solar, sizeof(k_mpp_solar)));
    TEST_ASSERT_EQUAL_HEX8(JK_CMD_READ, jk_frame_command(k_mpp_solar));
    TEST_ASSERT_EQUAL_INT(JK_ERR_COMMAND, jk_parse_read_all(k_mpp_solar, sizeof(k_mpp_solar), &d, &info));

    // The same payload as a 0x06 reply
    memcpy(s_frame, k_mpp_solar, sizeof(k_mpp_solar));
    s_frame[8] = JK_CMD_READ_ALL;
    fix_sum(s_frame, sizeof(k_mpp_solar));
    TEST_ASSERT_EQUAL_HEX8(0xD9, s_frame[sizeof(k_mpp_solar) - 1]);
    parse_ok(s_frame, sizeof(k_mpp_solar), &d, &info);
    TEST_ASSERT_TRUE(info.clean_end);
    TEST_ASSERT_EQUAL_UINT16(276, info.stop_offset);
    TEST_ASSERT_EQUAL_HEX32(ALL_FIELDS, d.present);

    TEST_ASSERT_EQUAL_UINT8(14, d.cell_count);
    TEST_ASSERT_EQUAL_UINT16(3984, d.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3988, d.cell_mv[2]);
    TEST_ASSERT_EQUAL_UINT16(3981, d.cell_mv[13]);
    TEST_ASSERT_EQUAL_UINT16(3981, d.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT8(13, d.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT16(3988, d.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT8(2, d.cell_max_idx);
    TEST_ASSERT_EQUAL_UINT16(3984, d.cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(7, d.cell_delta_mv);

    TEST_ASSERT_EQUAL_INT16(33, d.temp_mos_c);
    TEST_ASSERT_EQUAL_INT16(28, d.temp_1_c);
    TEST_ASSERT_EQUAL_INT16(30, d.temp_2_c);

    TEST_ASSERT_EQUAL_UINT16(5578, d.voltage_10mv);
    TEST_ASSERT_EQUAL_HEX16(0x81C5, d.current_raw);
    TEST_ASSERT_TRUE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(453, d.current_10ma);       // +4.53 A
    TEST_ASSERT_EQUAL_INT32(2526, d.power_dw);          // 252.6 W

    TEST_ASSERT_EQUAL_UINT8(100, d.soc_pct);
    TEST_ASSERT_EQUAL_UINT16(25, d.cycles);
    TEST_ASSERT_EQUAL_UINT32(5850, d.cycle_capacity_ah);
    TEST_ASSERT_EQUAL_HEX16(JK_STATUS_CHG_MOS | JK_STATUS_DSG_MOS, d.status);
    TEST_ASSERT_TRUE(d.charge_switch);
    TEST_ASSERT_TRUE(d.discharge_switch);
    TEST_ASSERT_EQUAL_UINT32(234, d.capacity_ah);
    TEST_ASSERT_EQUAL_UINT32(234, d.actual_capacity_ah);
    TEST_ASSERT_EQUAL_UINT32(99043, d.uptime_min);
    TEST_ASSERT_EQUAL_UINT8(1, d.protocol_version);
    TEST_ASSERT_EQUAL_STRING("11.XW_S11.261__", d.sw_version);
    TEST_ASSERT_EQUAL_STRING("Input UserdaJK_B1A20S15P", d.manufacturer);

    TEST_ASSERT_TRUE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, jk_dsg_state(&d));
}

static void test_real_pdf_old20(void)
{
    jk_data_t d;
    jk_parse_info_t info;
    TEST_ASSERT_EQUAL_UINT(269, sizeof(k_pdf_old20));
    TEST_ASSERT_EQUAL_HEX8(0x01, k_pdf_old20[2]);       // LEN 0x010B
    TEST_ASSERT_EQUAL_HEX8(0x0B, k_pdf_old20[3]);
    parse_ok(k_pdf_old20, sizeof(k_pdf_old20), &d, &info);
    TEST_ASSERT_TRUE(info.clean_end);                   // ends after 0xB7
    TEST_ASSERT_EQUAL_UINT16(260, info.stop_offset);
    TEST_ASSERT_EQUAL_UINT8(20, info.cells_reported);
    TEST_ASSERT_EQUAL_HEX32(OLD_LAYOUT_FIELDS, d.present);

    TEST_ASSERT_EQUAL_UINT8(20, d.cell_count);
    TEST_ASSERT_EQUAL_UINT16(3833, d.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3738, d.cell_mv[11]);
    TEST_ASSERT_EQUAL_UINT16(3778, d.cell_mv[19]);
    TEST_ASSERT_EQUAL_UINT8(20, d.cells_valid);
    TEST_ASSERT_EQUAL_UINT16(3738, d.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT8(11, d.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT16(3845, d.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT8(5, d.cell_max_idx);     // 3845 at 5 and 7
    TEST_ASSERT_EQUAL_UINT16(3806, d.cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(107, d.cell_delta_mv);

    TEST_ASSERT_EQUAL_INT16(27, d.temp_mos_c);
    TEST_ASSERT_EQUAL_INT16(30, d.temp_1_c);
    TEST_ASSERT_EQUAL_INT16(30, d.temp_2_c);

    TEST_ASSERT_EQUAL_UINT16(7612, d.voltage_10mv);     // 76.12 V
    TEST_ASSERT_EQUAL_HEX16(0x2710, d.current_raw);
    TEST_ASSERT_TRUE(d.current_valid);                  // no C0 but a clean end: 10000 - raw
    TEST_ASSERT_EQUAL_INT32(0, d.current_10ma);
    TEST_ASSERT_EQUAL_INT32(0, d.power_dw);

    TEST_ASSERT_EQUAL_UINT8(71, d.soc_pct);
    TEST_ASSERT_EQUAL_UINT16(1, d.cycles);
    TEST_ASSERT_EQUAL_UINT16(20, d.cell_count_setting);
    TEST_ASSERT_EQUAL_HEX16(0x0000, d.alarms);
    TEST_ASSERT_EQUAL_HEX16(0x000B, d.status);          // CHG + DSG MOSFET on, bit 3
    TEST_ASSERT_FALSE(d.charge_switch);                 // old firmware: AB/AC always read 00
    TEST_ASSERT_FALSE(d.discharge_switch);
    TEST_ASSERT_EQUAL_UINT32(40, d.capacity_ah);
    TEST_ASSERT_EQUAL_UINT32(1, d.uptime_min);
    TEST_ASSERT_EQUAL_STRING("60300001", d.device_id);
    TEST_ASSERT_EQUAL_STRING("NW_1_0_0_200428", d.sw_version);
    TEST_ASSERT_EQUAL_STRING("", d.manufacturer);

    // Old firmware: the output is on per the MOSFET bit although 0xAC reads 0
    TEST_ASSERT_TRUE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, jk_dsg_state(&d));
}

static void test_real_pdf_published_checksum_rejected(void)
{
    jk_data_t d;
    memcpy(s_frame, k_pdf_old20, sizeof(k_pdf_old20));
    s_frame[sizeof(k_pdf_old20) - 2] = 0x49;    // as printed in the PDF
    s_frame[sizeof(k_pdf_old20) - 1] = 0x23;
    TEST_ASSERT_EQUAL_INT(JK_ERR_CHECKSUM, jk_validate_reply(s_frame, sizeof(k_pdf_old20)));
    TEST_ASSERT_EQUAL_INT(JK_ERR_CHECKSUM, jk_parse_read_all(s_frame, sizeof(k_pdf_old20), &d, NULL));
    jk_rx_push(&s_rx, s_frame, sizeof(k_pdf_old20));
    TEST_ASSERT_FALSE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), NULL));
}

static void test_real_b2a24s_1007(void)
{
    jk_data_t d;
    jk_parse_info_t info;
    parse_ok(k_b2a24s_1007, sizeof(k_b2a24s_1007), &d, &info);
    TEST_ASSERT_TRUE(info.clean_end);
    TEST_ASSERT_EQUAL_HEX32(ALL_FIELDS, d.present);
    TEST_ASSERT_EQUAL_UINT8(13, d.cell_count);
    TEST_ASSERT_EQUAL_UINT16(4092, d.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(4047, d.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT8(1, d.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT16(4094, d.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT8(7, d.cell_max_idx);
    TEST_ASSERT_EQUAL_UINT16(4087, d.cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(47, d.cell_delta_mv);
    TEST_ASSERT_EQUAL_INT16(22, d.temp_mos_c);
    TEST_ASSERT_EQUAL_UINT16(5313, d.voltage_10mv);
    TEST_ASSERT_TRUE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(0, d.current_10ma);
    TEST_ASSERT_EQUAL_UINT8(94, d.soc_pct);
    TEST_ASSERT_EQUAL_HEX16(JK_STATUS_BALANCING, d.status);
    TEST_ASSERT_FALSE(d.charge_switch);
    TEST_ASSERT_FALSE(d.discharge_switch);
    TEST_ASSERT_EQUAL_UINT8(1, d.protocol_version);
    TEST_ASSERT_EQUAL_STRING("10.XW_S10.07___", d.sw_version);
    TEST_ASSERT_EQUAL_STRING("Input UserdaJK-B2A24S15P", d.manufacturer);
    TEST_ASSERT_FALSE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_OFF, jk_dsg_state(&d));
}

static void test_real_bd6a17s_710h_discharging(void)
{
    jk_data_t d;
    jk_parse_info_t info;
    parse_ok(k_bd6a17s_710h, sizeof(k_bd6a17s_710h), &d, &info);
    TEST_ASSERT_TRUE(info.clean_end);
    TEST_ASSERT_EQUAL_HEX32(ALL_FIELDS, d.present);
    TEST_ASSERT_EQUAL_UINT8(16, d.cell_count);
    TEST_ASSERT_EQUAL_UINT16(3201, d.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3185, d.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT8(6, d.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT16(3203, d.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT8(4, d.cell_max_idx);
    TEST_ASSERT_EQUAL_UINT16(3200, d.cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(18, d.cell_delta_mv);
    TEST_ASSERT_EQUAL_INT16(18, d.temp_mos_c);
    TEST_ASSERT_EQUAL_INT16(16, d.temp_1_c);
    TEST_ASSERT_EQUAL_INT16(16, d.temp_2_c);
    TEST_ASSERT_EQUAL_UINT16(5121, d.voltage_10mv);
    TEST_ASSERT_EQUAL_HEX16(0x0045, d.current_raw);
    TEST_ASSERT_TRUE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(-69, d.current_10ma);       // -0.69 A: bit 15 clear = discharging
    TEST_ASSERT_EQUAL_INT32(-353, d.power_dw);          // -35.3 W
    TEST_ASSERT_EQUAL_UINT8(15, d.soc_pct);
    TEST_ASSERT_EQUAL_UINT16(17, d.cycles);
    TEST_ASSERT_EQUAL_UINT32(1280, d.cycle_capacity_ah);
    TEST_ASSERT_EQUAL_HEX16(JK_STATUS_CHG_MOS | JK_STATUS_DSG_MOS, d.status);
    TEST_ASSERT_TRUE(d.charge_switch);
    TEST_ASSERT_TRUE(d.discharge_switch);
    TEST_ASSERT_EQUAL_UINT32(81, d.capacity_ah);
    TEST_ASSERT_EQUAL_UINT32(91136, d.uptime_min);
    TEST_ASSERT_EQUAL_UINT8(1, d.protocol_version);
    TEST_ASSERT_EQUAL_STRING("H7.X__S7.1.0H__", d.sw_version);
    TEST_ASSERT_TRUE(jk_dsg_on(&d));
}

// ---------------------------------------------------------------------------
// 3-5. Current, power, temperature
// ---------------------------------------------------------------------------

static void test_decode_current_rules(void)
{
    int32_t i = 12345;
    // C0 = 1: bit 15 = charging, magnitude = raw & 0x7FFF
    TEST_ASSERT_TRUE(jk_decode_current(0x80D0, true, 1, true, &i));
    TEST_ASSERT_EQUAL_INT32(208, i);
    TEST_ASSERT_TRUE(jk_decode_current(0x00D0, true, 1, true, &i));
    TEST_ASSERT_EQUAL_INT32(-208, i);
    TEST_ASSERT_TRUE(jk_decode_current(0xFFFF, true, 1, false, &i));  // C0 known: clean_end irrelevant
    TEST_ASSERT_EQUAL_INT32(32767, i);
    TEST_ASSERT_TRUE(jk_decode_current(0x7FFF, true, 1, false, &i));
    TEST_ASSERT_EQUAL_INT32(-32767, i);
    TEST_ASSERT_TRUE(jk_decode_current(0x8000, true, 1, true, &i));
    TEST_ASSERT_EQUAL_INT32(0, i);
    TEST_ASSERT_TRUE(jk_decode_current(0x0000, true, 1, true, &i));
    TEST_ASSERT_EQUAL_INT32(0, i);
    // C0 = 0: 10000 - raw
    TEST_ASSERT_TRUE(jk_decode_current(10000, true, 0, false, &i));
    TEST_ASSERT_EQUAL_INT32(0, i);
    TEST_ASSERT_TRUE(jk_decode_current(9000, true, 0, true, &i));
    TEST_ASSERT_EQUAL_INT32(1000, i);
    TEST_ASSERT_TRUE(jk_decode_current(11234, true, 0, true, &i));
    TEST_ASSERT_EQUAL_INT32(-1234, i);
    // C0 absent with a clean end (old layout): 10000 - raw
    TEST_ASSERT_TRUE(jk_decode_current(0x2710, false, 0, true, &i));
    TEST_ASSERT_EQUAL_INT32(0, i);
    TEST_ASSERT_TRUE(jk_decode_current(8766, false, 0, true, &i));
    TEST_ASSERT_EQUAL_INT32(1234, i);
    // C0 absent after an early stop: unknown, never -229.76 A
    i = 12345;
    TEST_ASSERT_FALSE(jk_decode_current(0x80D0, false, 0, false, &i));
    TEST_ASSERT_EQUAL_INT32(0, i);
    TEST_ASSERT_FALSE(jk_decode_current(0x80D0, false, 1, false, &i));
    // Unknown protocol versions
    TEST_ASSERT_FALSE(jk_decode_current(0x80D0, true, 2, true, &i));
    TEST_ASSERT_FALSE(jk_decode_current(0x80D0, true, 3, false, &i));
    TEST_ASSERT_FALSE(jk_decode_current(0x80D0, true, 255, true, &i));
    TEST_ASSERT_TRUE(jk_decode_current(0x80D0, true, 1, true, NULL));
}

// fake14 has C0 = 1 at offset 274 and current raw 0x80D0 (+2.08 A)
static void test_parse_current_unknown_id_before_c0(void)
{
    static const uint8_t unknown[] = {0xBB, 0x12, 0x34};
    jk_data_t d;
    jk_parse_info_t info;
    TEST_ASSERT_EQUAL_HEX8(0xC0, k_fake14[274]);
    const size_t len = insert_bytes(s_frame, k_fake14, sizeof(k_fake14), 274, unknown, sizeof(unknown));
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_FALSE(info.clean_end);
    TEST_ASSERT_EQUAL_UINT16(274, info.stop_offset);
    TEST_ASSERT_EQUAL_HEX8(0xBB, info.stop_id);
    TEST_ASSERT_EQUAL_HEX32(ALL_FIELDS & ~JK_HAS_PROTOCOL, d.present);
    TEST_ASSERT_EQUAL_HEX16(0x80D0, d.current_raw);
    // The legacy rule would give 10000 - 0x80D0 = -229.76 A
    TEST_ASSERT_FALSE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(0, d.current_10ma);
    TEST_ASSERT_EQUAL_INT32(0, d.power_dw);
    // Everything before the stop is still decoded
    TEST_ASSERT_EQUAL_UINT16(5359, d.voltage_10mv);
    TEST_ASSERT_EQUAL_STRING("BT3072020120000200521001", d.manufacturer);
}

static void test_parse_current_unknown_id_after_c0(void)
{
    static const uint8_t unknown[] = {0xBB, 0x12, 0x34};
    jk_data_t d;
    jk_parse_info_t info;
    const size_t len = insert_bytes(s_frame, k_fake14, sizeof(k_fake14), 276, unknown, sizeof(unknown));
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_FALSE(info.clean_end);
    TEST_ASSERT_EQUAL_UINT16(276, info.stop_offset);
    TEST_ASSERT_EQUAL_HEX8(0xBB, info.stop_id);
    TEST_ASSERT_EQUAL_HEX32(ALL_FIELDS, d.present);
    TEST_ASSERT_TRUE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(208, d.current_10ma);
}

static void test_parse_current_protocol_versions(void)
{
    jk_data_t d;
    memcpy(s_frame, k_fake14, sizeof(k_fake14));
    // C0 = 0: 10000 - raw
    s_frame[275] = 0x00;
    fix_sum(s_frame, sizeof(k_fake14));
    parse_ok(s_frame, sizeof(k_fake14), &d, NULL);
    TEST_ASSERT_TRUE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(10000 - 0x80D0, d.current_10ma);
    // C0 = 2: unknown
    s_frame[275] = 0x02;
    fix_sum(s_frame, sizeof(k_fake14));
    parse_ok(s_frame, sizeof(k_fake14), &d, NULL);
    TEST_ASSERT_FALSE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(0, d.power_dw);
    TEST_ASSERT_EQUAL_UINT8(2, d.protocol_version);
}

static void test_power(void)
{
    TEST_ASSERT_EQUAL_INT32(175200, jk_power_dw(8760, 20000));      // 87.60 V x 200 A
    TEST_ASSERT_EQUAL_INT32(-175200, jk_power_dw(8760, -20000));
    TEST_ASSERT_EQUAL_INT32(21488, jk_power_dw(5241, 4100));        // 52.41 V x 41 A
    TEST_ASSERT_EQUAL_INT32(0, jk_power_dw(5241, 0));
    // v * i overflows int32 here
    TEST_ASSERT_EQUAL_INT32(-3639486, jk_power_dw(65535, -55535));
    TEST_ASSERT_EQUAL_INT32(2147385, jk_power_dw(65535, 32767));
    // Truncates toward zero
    TEST_ASSERT_EQUAL_INT32(-1, jk_power_dw(1000, -1));
    TEST_ASSERT_EQUAL_INT32(0, jk_power_dw(999, -1));
}

static void test_temps(void)
{
    TEST_ASSERT_EQUAL_INT16(0, jk_decode_temp(0));
    TEST_ASSERT_EQUAL_INT16(25, jk_decode_temp(25));
    TEST_ASSERT_EQUAL_INT16(99, jk_decode_temp(99));
    TEST_ASSERT_EQUAL_INT16(-1, jk_decode_temp(100));
    TEST_ASSERT_EQUAL_INT16(-6, jk_decode_temp(105));
    TEST_ASSERT_EQUAL_INT16(-20, jk_decode_temp(119));
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, jk_decode_temp(32867));
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, jk_decode_temp(0xFFFF));     // clamped, no wrap-around
}

// ---------------------------------------------------------------------------
// 6. Receive assembler
// ---------------------------------------------------------------------------

static void test_rx_split_across_pushes(void)
{
    size_t n = 0;
    for (size_t off = 0; off < sizeof(k_issue341); off += 7) {
        const size_t left = sizeof(k_issue341) - off;
        const size_t chunk = left < 7 ? left : 7;
        jk_rx_push(&s_rx, &k_issue341[off], chunk);
        const bool got = jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n);
        if (off + chunk < sizeof(k_issue341)) {
            TEST_ASSERT_FALSE(got);
            TEST_ASSERT_EQUAL_UINT(0, n);
            TEST_ASSERT_EQUAL_UINT(off + chunk, s_rx.len);  // the partial frame is kept
        } else {
            TEST_ASSERT_TRUE(got);
        }
    }
    TEST_ASSERT_EQUAL_UINT(sizeof(k_issue341), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_issue341, s_out, sizeof(k_issue341));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);
    TEST_ASSERT_FALSE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
}

static void test_rx_byte_by_byte(void)
{
    size_t n = 0;
    for (size_t k = 0; k < sizeof(k_fake14); k++) {
        jk_rx_push(&s_rx, &k_fake14[k], 1);
        const bool got = jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n);
        TEST_ASSERT_EQUAL(k + 1 == sizeof(k_fake14), got);
        if (!got) {
            TEST_ASSERT_EQUAL_UINT(k + 1, s_rx.len);
        }
    }
    TEST_ASSERT_EQUAL_UINT(sizeof(k_fake14), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_fake14, s_out, sizeof(k_fake14));
}

static void test_rx_garbage_before(void)
{
    static const uint8_t garbage[] = {0x00, 0xFF, 0x4E, 0x00, 0x57, 0x68, 0x4E, 0x4E, 0x13, 0x37};
    size_t n = 0;
    jk_rx_push(&s_rx, garbage, sizeof(garbage));
    TEST_ASSERT_FALSE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);    // no candidate: everything dropped

    jk_rx_push(&s_rx, garbage, sizeof(garbage));
    jk_rx_push(&s_rx, k_fake14, sizeof(k_fake14));
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_fake14), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_fake14, s_out, sizeof(k_fake14));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);
}

static void test_rx_trailing_start_byte_kept(void)
{
    static const uint8_t garbage[] = {0x11, 0x22, 0x4E};
    size_t n = 0;
    jk_rx_push(&s_rx, garbage, sizeof(garbage));
    TEST_ASSERT_FALSE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(1, s_rx.len);
    TEST_ASSERT_EQUAL_HEX8(0x4E, s_rx.buf[0]);
    // The rest of the frame arrives in the next chunk
    jk_rx_push(&s_rx, &k_fake14[1], sizeof(k_fake14) - 1);
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_fake14, s_out, sizeof(k_fake14));
}

static void test_rx_false_header_in_garbage(void)
{
    // 4E 57 01 F0 claims a 498-byte frame that never completes; 4E 57 FF FF is out of range
    static const uint8_t garbage[] = {0x12, 0x4E, 0x57, 0x01, 0xF0, 0x00, 0x99, 0x4E, 0x57, 0xFF, 0xFF, 0x42};
    size_t n = 0;
    jk_rx_push(&s_rx, garbage, sizeof(garbage));
    TEST_ASSERT_FALSE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(sizeof(garbage) - 1, s_rx.len);     // kept from the incomplete candidate

    jk_rx_push(&s_rx, k_issue341, sizeof(k_issue341));
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_issue341), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_issue341, s_out, sizeof(k_issue341));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);    // the false header went with the frame
}

static void test_rx_rejected_candidates_keep_real_frame(void)
{
    // a) A complete frame with a bad checksum right before the real one
    size_t n = 0;
    memcpy(s_big, k_ack_ac, sizeof(k_ack_ac));
    s_big[20] ^= 0x01;
    memcpy(&s_big[21], k_fake14, sizeof(k_fake14));
    jk_rx_push(&s_rx, s_big, 21 + sizeof(k_fake14));
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_fake14, s_out, sizeof(k_fake14));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);

    // b) A false header whose claimed 34 bytes end inside the real frame
    static const uint8_t junk[] = {0x4E, 0x57, 0x00, 0x20, 0x01, 0x02};
    jk_rx_push(&s_rx, junk, sizeof(junk));
    jk_rx_push(&s_rx, k_fake14, sizeof(k_fake14));
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_fake14, s_out, sizeof(k_fake14));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);

    // c) A truncated frame (BMS restarted mid-reply), then a frame arriving in two parts
    jk_rx_push(&s_rx, k_fake14, 100);
    jk_rx_push(&s_rx, k_issue341, 50);
    TEST_ASSERT_FALSE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(150, s_rx.len);
    jk_rx_push(&s_rx, &k_issue341[50], sizeof(k_issue341) - 50);
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_issue341), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_issue341, s_out, sizeof(k_issue341));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);
}

static void test_rx_echo_skipped(void)
{
    uint8_t req[32];
    size_t n = 0;
    const size_t rl = jk_build_read_all(req, sizeof(req), JK_SRC_GPS);
    TEST_ASSERT_EQUAL_INT(JK_ERR_NOT_REPLY, jk_validate_reply(req, rl));
    jk_rx_push(&s_rx, req, rl);
    TEST_ASSERT_FALSE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);

    // Echo immediately followed by the reply
    jk_rx_push(&s_rx, req, rl);
    jk_rx_push(&s_rx, k_fake14, sizeof(k_fake14));
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_fake14), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_fake14, s_out, sizeof(k_fake14));

    // Echoed password + write requests, then the ack
    const size_t pl = jk_build_password(req, sizeof(req), JK_SRC_GPS);
    jk_rx_push(&s_rx, req, pl);
    const size_t wl = jk_build_write_u8(req, sizeof(req), JK_SRC_GPS, JK_REG_DISCHARGE_SWITCH, 0);
    jk_rx_push(&s_rx, req, wl);
    jk_rx_push(&s_rx, k_ack_ac, sizeof(k_ack_ac));
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_TRUE(jk_is_write_ack(s_out, n, JK_REG_DISCHARGE_SWITCH));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);
}

static void test_rx_back_to_back(void)
{
    size_t n = 0;
    size_t len = 0;
    len = append(s_big, len, k_ack_ac, sizeof(k_ack_ac));
    len = append(s_big, len, k_fake14, sizeof(k_fake14));
    len = append(s_big, len, k_issue341, sizeof(k_issue341));
    jk_rx_push(&s_rx, s_big, len);
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_ack_ac), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_ack_ac, s_out, sizeof(k_ack_ac));
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_fake14), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_fake14, s_out, sizeof(k_fake14));
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_issue341), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_issue341, s_out, sizeof(k_issue341));
    TEST_ASSERT_FALSE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);
}

static void test_rx_overflow_drops_oldest(void)
{
    size_t n = 0;
    memset(s_big, 0xA5, sizeof(s_big));
    jk_rx_push(&s_rx, s_big, 1500);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_rx.buf), s_rx.len);
    jk_rx_push(&s_rx, k_fake14, sizeof(k_fake14));
    TEST_ASSERT_EQUAL_UINT(sizeof(s_rx.buf), s_rx.len);
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_fake14, s_out, sizeof(k_fake14));

    // One push larger than the buffer keeps only its newest bytes
    jk_rx_reset(&s_rx);
    memset(s_big, 0xA5, sizeof(s_big));
    memcpy(&s_big[1400 - sizeof(k_issue341)], k_issue341, sizeof(k_issue341));
    jk_rx_push(&s_rx, s_big, 1400);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_rx.buf), s_rx.len);
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(s_out), &n));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_issue341, s_out, sizeof(k_issue341));
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);
}

static void test_rx_out_too_small(void)
{
    size_t n = 99;
    jk_rx_push(&s_rx, k_fake14, sizeof(k_fake14));
    // A frame that does not fit out is consumed and reported with length 0
    TEST_ASSERT_TRUE(jk_rx_pop_frame(&s_rx, s_out, sizeof(k_fake14) - 1, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);
    TEST_ASSERT_FALSE(jk_rx_pop_frame(NULL, s_out, sizeof(s_out), &n));
    jk_rx_push(&s_rx, NULL, 10);
    TEST_ASSERT_EQUAL_UINT(0, s_rx.len);
}

// ---------------------------------------------------------------------------
// 7. Validation
// ---------------------------------------------------------------------------

static void test_validate_errors(void)
{
    const size_t len = sizeof(k_fake14);
    memcpy(s_frame, k_fake14, len);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(s_frame, len));
    TEST_ASSERT_EQUAL_INT(JK_ERR_ARG, jk_validate_reply(NULL, len));

    // Bad start
    s_frame[0] = 0x4F;
    fix_sum(s_frame, len);
    TEST_ASSERT_EQUAL_INT(JK_ERR_HEADER, jk_validate_reply(s_frame, len));
    s_frame[0] = 0x4E;
    s_frame[1] = 0x56;
    fix_sum(s_frame, len);
    TEST_ASSERT_EQUAL_INT(JK_ERR_HEADER, jk_validate_reply(s_frame, len));
    memcpy(s_frame, k_fake14, len);

    // Bad checksum: one payload bit, or the checksum itself
    s_frame[100] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(JK_ERR_CHECKSUM, jk_validate_reply(s_frame, len));
    memcpy(s_frame, k_fake14, len);
    s_frame[len - 1] ^= 0x80;
    TEST_ASSERT_EQUAL_INT(JK_ERR_CHECKSUM, jk_validate_reply(s_frame, len));
    memcpy(s_frame, k_fake14, len);

    // Missing 0x68 end marker (checksum fixed, so only the marker is wrong)
    s_frame[len - 5] = 0x00;
    fix_sum(s_frame, len);
    TEST_ASSERT_EQUAL_INT(JK_ERR_END_MARK, jk_validate_reply(s_frame, len));
    memcpy(s_frame, k_fake14, len);

    // LEN mismatch: frame truncated, or LEN off by one
    TEST_ASSERT_EQUAL_INT(JK_ERR_LENGTH, jk_validate_reply(s_frame, len - 1));
    s_frame[3]++;
    fix_sum(s_frame, len);
    TEST_ASSERT_EQUAL_INT(JK_ERR_LENGTH, jk_validate_reply(s_frame, len));
    memcpy(s_frame, k_fake14, len);

    // Not a reply: source or type
    s_frame[9] = JK_SRC_GPS;
    fix_sum(s_frame, len);
    TEST_ASSERT_EQUAL_INT(JK_ERR_NOT_REPLY, jk_validate_reply(s_frame, len));
    s_frame[9] = JK_SRC_BMS;
    s_frame[10] = JK_TYPE_REQUEST;
    fix_sum(s_frame, len);
    TEST_ASSERT_EQUAL_INT(JK_ERR_NOT_REPLY, jk_validate_reply(s_frame, len));
    // Active upload is accepted for read-all only
    s_frame[10] = JK_TYPE_UPLOAD;
    fix_sum(s_frame, len);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(s_frame, len));
    jk_data_t d;
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, NULL));
    memcpy(s_frame, k_ack_ac, sizeof(k_ack_ac));
    s_frame[10] = JK_TYPE_UPLOAD;
    fix_sum(s_frame, sizeof(k_ack_ac));
    TEST_ASSERT_EQUAL_INT(JK_ERR_NOT_REPLY, jk_validate_reply(s_frame, sizeof(k_ack_ac)));
}

static void test_validate_length_bounds(void)
{
    static uint8_t info[JK_FRAME_MAX];
    memset(info, 0, sizeof(info));

    // 20 and 512 bytes are the limits
    size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info, 0);
    TEST_ASSERT_EQUAL_UINT(JK_FRAME_MIN, len);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(s_frame, len));
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info, JK_FRAME_MAX - JK_FRAME_MIN);
    TEST_ASSERT_EQUAL_UINT(JK_FRAME_MAX, len);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(s_frame, len));

    // 513 bytes with a consistent LEN
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info, JK_FRAME_MAX - JK_FRAME_MIN + 1);
    TEST_ASSERT_EQUAL_UINT(513, len);
    TEST_ASSERT_EQUAL_INT(JK_ERR_LENGTH, jk_validate_reply(s_frame, len));

    // 19 bytes with a consistent LEN and end marker
    memset(s_frame, 0, 19);
    s_frame[0] = 0x4E;
    s_frame[1] = 0x57;
    s_frame[9] = JK_SRC_BMS;
    s_frame[10] = JK_TYPE_REPLY;
    s_frame[19 - 5] = 0x68;
    fix_frame(s_frame, 19);
    TEST_ASSERT_EQUAL_INT(JK_ERR_LENGTH, jk_validate_reply(s_frame, 19));
    TEST_ASSERT_EQUAL_INT(JK_ERR_LENGTH, jk_validate_reply(s_frame, 3));
    TEST_ASSERT_EQUAL_INT(JK_ERR_LENGTH, jk_validate_reply(s_frame, 0));
}

// ---------------------------------------------------------------------------
// 8. Parser errors and edge cases
// ---------------------------------------------------------------------------

static void test_parse_cells_not_multiple_of_3(void)
{
    static const uint8_t info_bytes[] = {0x79, 0x04, 0x01, 0x0C, 0xE4, 0x02, 0x83, 0x0A, 0x50};
    jk_data_t d;
    jk_parse_info_t info;
    const size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, sizeof(info_bytes));
    TEST_ASSERT_EQUAL_INT(JK_ERR_CELLS, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_FALSE(info.clean_end);
    TEST_ASSERT_EQUAL_UINT16(11, info.stop_offset);
    TEST_ASSERT_EQUAL_HEX8(0x79, info.stop_id);
}

static void test_parse_tlv_overrun(void)
{
    jk_data_t d;
    jk_parse_info_t info;
    // 0x83 needs 2 bytes, only 1 is left before the record number
    static const uint8_t a[] = {0x85, 0x32, 0x83, 0x0A};
    size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, a, sizeof(a));
    TEST_ASSERT_EQUAL_INT(JK_ERR_TLV_BOUNDS, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_EQUAL_UINT16(13, info.stop_offset);
    TEST_ASSERT_EQUAL_HEX8(0x83, info.stop_id);
    TEST_ASSERT_FALSE(info.clean_end);
    // 0x79 claims 6 bytes, 3 are present
    static const uint8_t b[] = {0x79, 0x06, 0x01, 0x0C, 0xE4};
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, b, sizeof(b));
    TEST_ASSERT_EQUAL_INT(JK_ERR_TLV_BOUNDS, jk_parse_read_all(s_frame, len, &d, &info));
    // 0x79 without its length byte
    static const uint8_t c[] = {0x85, 0x32, 0x79};
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, c, sizeof(c));
    TEST_ASSERT_EQUAL_INT(JK_ERR_TLV_BOUNDS, jk_parse_read_all(s_frame, len, &d, &info));
    // A 15-byte string field cut short
    static const uint8_t e[] = {0xB7, 0x31, 0x31, 0x2E, 0x58};
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, e, sizeof(e));
    TEST_ASSERT_EQUAL_INT(JK_ERR_TLV_BOUNDS, jk_parse_read_all(s_frame, len, &d, &info));
    // Real frame with the C0 value byte removed: 0xC0 would read the record number
    len = remove_bytes(s_frame, k_fake14, sizeof(k_fake14), 275, 1);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(s_frame, len));
    TEST_ASSERT_EQUAL_INT(JK_ERR_TLV_BOUNDS, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_EQUAL_UINT16(274, info.stop_offset);
    TEST_ASSERT_EQUAL_HEX8(0xC0, info.stop_id);
}

static void test_parse_missing_required(void)
{
    uint8_t info_bytes[64];
    jk_data_t d;
    jk_parse_info_t info;
    // All present
    size_t n = make_required_info(info_bytes, -1);
    size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_TRUE(info.clean_end);
    TEST_ASSERT_EQUAL_INT32(100, d.current_10ma);
    TEST_ASSERT_EQUAL_INT32(264, d.power_dw);
    // Each required field left out in turn: 0x79, 0x83, 0x84, 0x85, 0x8C
    for (int skip = 0; skip < 5; skip++) {
        n = make_required_info(info_bytes, skip);
        len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
        TEST_ASSERT_EQUAL_INT_MESSAGE(JK_ERR_MISSING, jk_parse_read_all(s_frame, len, &d, &info), "skip");
        TEST_ASSERT_TRUE(info.clean_end);
        TEST_ASSERT_EQUAL_UINT8(1, d.protocol_version);     // out stays filled
    }
    // Without 0x84 the current is unknown, not 0 A
    n = make_required_info(info_bytes, 2);
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
    TEST_ASSERT_EQUAL_INT(JK_ERR_MISSING, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_FALSE(d.current_valid);
    TEST_ASSERT_EQUAL_UINT16(2640, d.voltage_10mv);
    // C0 is optional; with a clean end the old rule applies
    n = make_required_info(info_bytes, 5);
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_TRUE(d.current_valid);
    TEST_ASSERT_EQUAL_INT32(10000 - 0x8064, d.current_10ma);
    // Empty info section
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, 0);
    TEST_ASSERT_EQUAL_INT(JK_ERR_MISSING, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_TRUE(info.clean_end);
    TEST_ASSERT_EQUAL_HEX32(0, d.present);
    TEST_ASSERT_EQUAL_UINT8(0xFF, d.cell_min_idx);
}

static void test_parse_more_than_24_cells(void)
{
    uint8_t info_bytes[300];
    jk_data_t d;
    jk_parse_info_t info;
    for (unsigned cells = 25; cells <= 85; cells += 60) {
        size_t n = 0;
        info_bytes[n++] = 0x79;
        info_bytes[n++] = (uint8_t)(3 * cells);
        for (unsigned k = 0; k < cells; k++) {
            const unsigned mv = (k < 24) ? 3000 + k : 2000;  // cells past 24 must not count
            info_bytes[n++] = (uint8_t)(k + 1);
            info_bytes[n++] = (uint8_t)(mv >> 8);
            info_bytes[n++] = (uint8_t)mv;
        }
        n = append(info_bytes, n, k_tlv_volt, sizeof(k_tlv_volt));
        n = append(info_bytes, n, k_tlv_curr, sizeof(k_tlv_curr));
        n = append(info_bytes, n, k_tlv_soc, sizeof(k_tlv_soc));
        n = append(info_bytes, n, k_tlv_status, sizeof(k_tlv_status));
        const size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
        TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, &info));
        TEST_ASSERT_TRUE(info.clean_end);
        TEST_ASSERT_EQUAL_UINT8(cells, info.cells_reported);
        TEST_ASSERT_EQUAL_UINT8(JK_MAX_CELLS, d.cell_count);
        TEST_ASSERT_EQUAL_UINT16(3000, d.cell_mv[0]);
        TEST_ASSERT_EQUAL_UINT16(3023, d.cell_mv[23]);
        TEST_ASSERT_EQUAL_UINT8(24, d.cells_valid);
        TEST_ASSERT_EQUAL_UINT16(3000, d.cell_min_mv);
        TEST_ASSERT_EQUAL_UINT8(0, d.cell_min_idx);
        TEST_ASSERT_EQUAL_UINT16(3023, d.cell_max_mv);
        TEST_ASSERT_EQUAL_UINT8(23, d.cell_max_idx);
        TEST_ASSERT_EQUAL_UINT16(3011, d.cell_avg_mv);
        TEST_ASSERT_EQUAL_UINT16(23, d.cell_delta_mv);
        TEST_ASSERT_EQUAL_UINT16(2640, d.voltage_10mv);     // the walk continued after 0x79
    }
}

static void test_parse_zero_mv_cells_excluded(void)
{
    static const uint8_t cells[] = {
        0x79, 0x12,
        0x01, 0x0C, 0xE4,   // 3300
        0x02, 0x00, 0x00,   // 0 mV: not connected
        0x03, 0x0C, 0xEE,   // 3310
        0x04, 0x00, 0x00,
        0x05, 0x0C, 0xDA,   // 3290
        0x06, 0x0C, 0xDA,   // 3290
    };
    uint8_t info_bytes[64];
    jk_data_t d;
    size_t n = append(info_bytes, 0, cells, sizeof(cells));
    n = append(info_bytes, n, k_tlv_volt, sizeof(k_tlv_volt));
    n = append(info_bytes, n, k_tlv_curr, sizeof(k_tlv_curr));
    n = append(info_bytes, n, k_tlv_soc, sizeof(k_tlv_soc));
    n = append(info_bytes, n, k_tlv_status, sizeof(k_tlv_status));
    size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, NULL));
    TEST_ASSERT_EQUAL_UINT8(6, d.cell_count);
    TEST_ASSERT_EQUAL_UINT16(0, d.cell_mv[1]);
    TEST_ASSERT_EQUAL_UINT8(4, d.cells_valid);
    TEST_ASSERT_EQUAL_UINT16(3290, d.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT8(4, d.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT16(3310, d.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT8(2, d.cell_max_idx);
    TEST_ASSERT_EQUAL_UINT16(3297, d.cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(20, d.cell_delta_mv);

    // All cells at 0 mV
    static const uint8_t zero[] = {0x79, 0x06, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00};
    n = append(info_bytes, 0, zero, sizeof(zero));
    n = append(info_bytes, n, k_tlv_volt, sizeof(k_tlv_volt));
    n = append(info_bytes, n, k_tlv_curr, sizeof(k_tlv_curr));
    n = append(info_bytes, n, k_tlv_soc, sizeof(k_tlv_soc));
    n = append(info_bytes, n, k_tlv_status, sizeof(k_tlv_status));
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, NULL));
    TEST_ASSERT_EQUAL_UINT8(2, d.cell_count);
    TEST_ASSERT_EQUAL_UINT8(0, d.cells_valid);
    TEST_ASSERT_EQUAL_UINT16(0, d.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT16(0, d.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT16(0, d.cell_avg_mv);
    TEST_ASSERT_EQUAL_UINT16(0, d.cell_delta_mv);
    TEST_ASSERT_EQUAL_UINT8(0xFF, d.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT8(0xFF, d.cell_max_idx);
}

static void test_parse_unknown_id_stops_walk(void)
{
    static const uint8_t tail[] = {0xC7, 0x12, 0x34, 0x85, 0x63};  // 0x85 after the stop is ignored
    uint8_t info_bytes[64];
    jk_data_t d;
    jk_parse_info_t info;
    size_t n = make_required_info(info_bytes, 5);
    const size_t stop = 11 + n;
    n = append(info_bytes, n, tail, sizeof(tail));
    const size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, &info));
    TEST_ASSERT_FALSE(info.clean_end);
    TEST_ASSERT_EQUAL_UINT16(stop, info.stop_offset);
    TEST_ASSERT_EQUAL_HEX8(0xC7, info.stop_id);
    TEST_ASSERT_EQUAL_UINT8(50, d.soc_pct);
    TEST_ASSERT_FALSE(d.current_valid);     // no C0 and no clean end
}

static void test_parse_strings(void)
{
    static const uint8_t strs[] = {
        // 0xB4: stops at the 0x01, trailing spaces trimmed -> "AB C"
        0xB4, 'A', 'B', ' ', 'C', ' ', ' ', 0x01, 'X',
        // 0xB7: only spaces -> ""
        0xB7, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        // 0xBA: NUL-padded, 0x7F stops the copy -> "Hello"
        0xBA, 'H', 'e', 'l', 'l', 'o', 0x7F, 'Z', 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t info_bytes[96];
    jk_data_t d;
    size_t n = make_required_info(info_bytes, -1);
    n = append(info_bytes, n, strs, sizeof(strs));
    const size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_parse_read_all(s_frame, len, &d, NULL));
    TEST_ASSERT_EQUAL_STRING("AB C", d.device_id);
    TEST_ASSERT_EQUAL_STRING("", d.sw_version);
    TEST_ASSERT_EQUAL_STRING("Hello", d.manufacturer);
    TEST_ASSERT_TRUE((d.present & (JK_HAS_DEVICE_ID | JK_HAS_SW_VERSION | JK_HAS_MANUFACTURER)) ==
                     (JK_HAS_DEVICE_ID | JK_HAS_SW_VERSION | JK_HAS_MANUFACTURER));
}

static void test_parse_wrong_command_and_args(void)
{
    uint8_t info_bytes[64];
    jk_data_t d;
    const size_t n = make_required_info(info_bytes, -1);
    size_t len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ, info_bytes, n);
    TEST_ASSERT_EQUAL_INT(JK_ERR_COMMAND, jk_parse_read_all(s_frame, len, &d, NULL));
    TEST_ASSERT_EQUAL_INT(JK_ERR_COMMAND, jk_parse_read_all(k_ack_ac, sizeof(k_ack_ac), &d, NULL));
    TEST_ASSERT_EQUAL_INT(JK_ERR_ARG, jk_parse_read_all(k_fake14, sizeof(k_fake14), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(JK_ERR_ARG, jk_parse_read_all(NULL, 100, &d, NULL));
    TEST_ASSERT_EQUAL_INT(JK_ERR_NOT_REPLY,
                          jk_parse_read_all(k_req_read_all_gps, sizeof(k_req_read_all_gps), &d, NULL));
    len = make_reply(s_frame, sizeof(s_frame), JK_CMD_READ_ALL, info_bytes, n);
    s_frame[len - 1] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(JK_ERR_CHECKSUM, jk_parse_read_all(s_frame, len, &d, NULL));
}

// ---------------------------------------------------------------------------
// 9. Encoder
// ---------------------------------------------------------------------------

static void fill_sample(jk_data_t *d)
{
    memset(d, 0, sizeof(*d));
    d->present = ALL_FIELDS;
    d->cell_count = JK_MAX_CELLS;
    for (unsigned k = 0; k < JK_MAX_CELLS; k++) {
        d->cell_mv[k] = (uint16_t)(3300 + 3 * k);
    }
    d->cell_mv[5] = 0;                  // one disconnected cell
    d->temp_mos_c = 45;
    d->temp_1_c = -20;
    d->temp_2_c = 0;
    d->temp_sensor_count = 2;
    d->voltage_10mv = 8760;
    d->current_raw = 0x8000 | 15000;    // +150 A with C0 = 1
    d->soc_pct = 87;
    d->cycles = 1234;
    d->cycle_capacity_ah = 98765;
    d->cell_count_setting = 24;
    d->alarms = 0x0103;
    d->status = JK_STATUS_CHG_MOS | JK_STATUS_BALANCING;
    d->balancer_switch = true;
    d->capacity_ah = 280;
    d->charge_switch = true;
    d->discharge_switch = true;
    d->uptime_min = 0x01020304;
    d->actual_capacity_ah = 275;
    d->protocol_version = 1;
    memcpy(d->device_id, "JK-12345", 8);
    d->device_id[8] = '\0';
    memcpy(d->sw_version, "11.XW_S11.26", 12);
    d->sw_version[12] = '\0';
    memcpy(d->manufacturer, "Demo 24S-280AH", 14);
    d->manufacturer[14] = '\0';
}

static void test_encode_round_trip(void)
{
    jk_data_t in;
    jk_data_t out;
    jk_parse_info_t info;
    fill_sample(&in);
    const size_t n = jk_encode_read_all_reply(&in, s_frame, sizeof(s_frame));
    TEST_ASSERT_EQUAL_UINT(203, n);     // every field with 24 cells
    TEST_ASSERT_EQUAL_HEX8(JK_SRC_BMS, s_frame[9]);
    TEST_ASSERT_EQUAL_HEX8(JK_TYPE_REPLY, s_frame[10]);
    parse_ok(s_frame, n, &out, &info);
    TEST_ASSERT_TRUE(info.clean_end);
    TEST_ASSERT_EQUAL_UINT8(24, info.cells_reported);

    TEST_ASSERT_EQUAL_HEX32(in.present, out.present);
    TEST_ASSERT_EQUAL_UINT8(in.cell_count, out.cell_count);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(in.cell_mv, out.cell_mv, JK_MAX_CELLS);
    TEST_ASSERT_EQUAL_INT16(45, out.temp_mos_c);
    TEST_ASSERT_EQUAL_INT16(-20, out.temp_1_c);
    TEST_ASSERT_EQUAL_INT16(0, out.temp_2_c);
    TEST_ASSERT_EQUAL_UINT8(2, out.temp_sensor_count);
    TEST_ASSERT_EQUAL_UINT16(8760, out.voltage_10mv);
    TEST_ASSERT_EQUAL_HEX16(in.current_raw, out.current_raw);
    TEST_ASSERT_EQUAL_UINT8(87, out.soc_pct);
    TEST_ASSERT_EQUAL_UINT16(1234, out.cycles);
    TEST_ASSERT_EQUAL_UINT32(98765, out.cycle_capacity_ah);
    TEST_ASSERT_EQUAL_UINT16(24, out.cell_count_setting);
    TEST_ASSERT_EQUAL_HEX16(0x0103, out.alarms);
    TEST_ASSERT_EQUAL_HEX16(in.status, out.status);
    TEST_ASSERT_TRUE(out.balancer_switch);
    TEST_ASSERT_EQUAL_UINT32(280, out.capacity_ah);
    TEST_ASSERT_TRUE(out.charge_switch);
    TEST_ASSERT_TRUE(out.discharge_switch);
    TEST_ASSERT_EQUAL_UINT32(0x01020304, out.uptime_min);
    TEST_ASSERT_EQUAL_UINT32(275, out.actual_capacity_ah);
    TEST_ASSERT_EQUAL_UINT8(1, out.protocol_version);
    TEST_ASSERT_EQUAL_STRING("JK-12345", out.device_id);
    TEST_ASSERT_EQUAL_STRING("11.XW_S11.26", out.sw_version);
    TEST_ASSERT_EQUAL_STRING("Demo 24S-280AH", out.manufacturer);
    // Derived by the parser
    TEST_ASSERT_TRUE(out.current_valid);
    TEST_ASSERT_EQUAL_INT32(15000, out.current_10ma);
    TEST_ASSERT_EQUAL_INT32(131400, out.power_dw);
    TEST_ASSERT_EQUAL_UINT8(23, out.cells_valid);
    TEST_ASSERT_EQUAL_UINT16(3300, out.cell_min_mv);
    TEST_ASSERT_EQUAL_UINT8(0, out.cell_min_idx);
    TEST_ASSERT_EQUAL_UINT16(3369, out.cell_max_mv);
    TEST_ASSERT_EQUAL_UINT8(23, out.cell_max_idx);
    // TRIP: setting on, MOSFET off
    TEST_ASSERT_TRUE(jk_dsg_on(&out));
    TEST_ASSERT_EQUAL_INT(JK_DSG_TRIP, jk_dsg_state(&out));

    // Too small a buffer
    for (size_t cap = 0; cap < n; cap++) {
        TEST_ASSERT_EQUAL_UINT(0, jk_encode_read_all_reply(&in, s_frame, cap));
    }
    TEST_ASSERT_EQUAL_UINT(n, jk_encode_read_all_reply(&in, s_frame, n));
    TEST_ASSERT_EQUAL_UINT(0, jk_encode_read_all_reply(NULL, s_frame, sizeof(s_frame)));
    TEST_ASSERT_EQUAL_UINT(0, jk_encode_read_all_reply(&in, NULL, sizeof(s_frame)));
}

static void test_encode_layout(void)
{
    // Expected bytes computed independently (Python)
    static const uint8_t expected[34] = {
        0x4E, 0x57, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01,
        0x79, 0x06, 0x01, 0x0C, 0xE4, 0x02, 0x0C, 0xEE,
        0x81, 0x00, 0x77,
        0x83, 0x0A, 0x50,
        0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x05, 0x75,
    };
    jk_data_t d;
    memset(&d, 0, sizeof(d));
    d.present = JK_HAS_CELLS | JK_HAS_TEMP_1 | JK_HAS_VOLTAGE;
    d.cell_count = 2;
    d.cell_mv[0] = 3300;
    d.cell_mv[1] = 3310;
    d.temp_1_c = -20;       // -> 99 + 20 = 119
    d.temp_mos_c = 77;      // not present: not encoded
    d.voltage_10mv = 0x0A50;
    const size_t n = jk_encode_read_all_reply(&d, s_frame, sizeof(s_frame));
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_frame, sizeof(expected));
}

static void test_encode_real_frames_round_trip(void)
{
    assert_reencodes(k_fake14, sizeof(k_fake14));
    assert_reencodes(k_issue341, sizeof(k_issue341));
    assert_reencodes(k_pdf_old20, sizeof(k_pdf_old20));
    assert_reencodes(k_b2a24s_1007, sizeof(k_b2a24s_1007));
    assert_reencodes(k_bd6a17s_710h, sizeof(k_bd6a17s_710h));
}

// ---------------------------------------------------------------------------
// 10. Discharge output model
// ---------------------------------------------------------------------------

static void set_dsg(jk_data_t *d, bool ac_present, bool ac, bool mos)
{
    memset(d, 0, sizeof(*d));
    d->present = JK_HAS_STATUS | (ac_present ? JK_HAS_DSG_SWITCH : 0u);
    d->discharge_switch = ac;
    d->status = (uint16_t)(JK_STATUS_CHG_MOS | (mos ? JK_STATUS_DSG_MOS : 0u));
}

static void test_dsg_model(void)
{
    jk_data_t d;
    // v11, output on: AC = 1, MOSFET on
    set_dsg(&d, true, true, true);
    TEST_ASSERT_TRUE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, jk_dsg_state(&d));
    // Off: AC = 0, MOSFET off
    set_dsg(&d, true, false, false);
    TEST_ASSERT_FALSE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_OFF, jk_dsg_state(&d));
    // Tripped: AC = 1, MOSFET off -> counts as on, so the toggle target is off
    set_dsg(&d, true, true, false);
    TEST_ASSERT_TRUE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_TRIP, jk_dsg_state(&d));
    // Old firmware: AC always 0, MOSFET on
    set_dsg(&d, true, false, true);
    TEST_ASSERT_TRUE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, jk_dsg_state(&d));
    // AC absent: only the MOSFET bit counts, a stale setting is ignored
    set_dsg(&d, false, true, false);
    TEST_ASSERT_FALSE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_OFF, jk_dsg_state(&d));
    set_dsg(&d, false, false, true);
    TEST_ASSERT_TRUE(jk_dsg_on(&d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, jk_dsg_state(&d));
    TEST_ASSERT_FALSE(jk_dsg_on(NULL));
    TEST_ASSERT_EQUAL_INT(JK_DSG_OFF, jk_dsg_state(NULL));
}

// Discharge-switch command confirmation (jk_dsg_cmd_*)
static void test_dsg_cmd_results(void)
{
    jk_data_t d;
    jk_dsg_cmd_t c;

    // Nothing at all: no reply
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_NO_REPLY, jk_dsg_cmd_result(&c));
    // Acked but no frame read back: the state is unknown, never guessed
    c.acked = true;
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_UNKNOWN, jk_dsg_cmd_result(&c));
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_NO_REPLY, jk_dsg_cmd_result(NULL));

    // Disable confirmed by a frame without ack (the ack is advisory)
    memset(&c, 0, sizeof(c));
    set_dsg(&d, true, false, false);
    TEST_ASSERT_TRUE(jk_dsg_cmd_readback(&c, false, &d));
    TEST_ASSERT_TRUE(c.settled);
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_OK, jk_dsg_cmd_result(&c));
    TEST_ASSERT_EQUAL_INT(JK_DSG_OFF, c.state);

    // Disable ignored while tripped: setting still on -> not confirmed, state TRIP
    memset(&c, 0, sizeof(c));
    c.acked = true;
    set_dsg(&d, true, true, false);
    TEST_ASSERT_FALSE(jk_dsg_cmd_readback(&c, false, &d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_NOT_CONFIRMED, jk_dsg_cmd_result(&c));
    TEST_ASSERT_EQUAL_INT(JK_DSG_TRIP, c.state);

    // Enable confirmed with the MOSFET on: settled at once
    memset(&c, 0, sizeof(c));
    set_dsg(&d, true, true, true);
    TEST_ASSERT_TRUE(jk_dsg_cmd_readback(&c, true, &d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_OK, jk_dsg_cmd_result(&c));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, c.state);

    // Enable: TRIP first, the MOSFET follows on the next frame -> ON, no false TRIP
    memset(&c, 0, sizeof(c));
    set_dsg(&d, true, true, false);
    TEST_ASSERT_FALSE(jk_dsg_cmd_readback(&c, true, &d));
    TEST_ASSERT_TRUE(c.confirmed);
    TEST_ASSERT_FALSE(c.settled);
    set_dsg(&d, true, true, true);
    TEST_ASSERT_TRUE(jk_dsg_cmd_readback(&c, true, &d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, c.state);
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_OK, jk_dsg_cmd_result(&c));

    // Enable: TRIP twice in a row -> settled as tripped (protection active)
    memset(&c, 0, sizeof(c));
    set_dsg(&d, true, true, false);
    TEST_ASSERT_FALSE(jk_dsg_cmd_readback(&c, true, &d));
    TEST_ASSERT_TRUE(jk_dsg_cmd_readback(&c, true, &d));
    TEST_ASSERT_TRUE(c.settled);
    TEST_ASSERT_EQUAL_INT(JK_DSG_TRIP, c.state);
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_OK, jk_dsg_cmd_result(&c));

    // A single TRIP frame (no second poll) is confirmed but not settled: still OK + TRIP
    memset(&c, 0, sizeof(c));
    set_dsg(&d, true, true, false);
    jk_dsg_cmd_readback(&c, true, &d);
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_OK, jk_dsg_cmd_result(&c));
    TEST_ASSERT_EQUAL_INT(JK_DSG_TRIP, c.state);

    // Enable not taken: still off -> not confirmed with the read-back state
    memset(&c, 0, sizeof(c));
    c.acked = true;
    set_dsg(&d, true, false, false);
    TEST_ASSERT_FALSE(jk_dsg_cmd_readback(&c, true, &d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_CMD_NOT_CONFIRMED, jk_dsg_cmd_result(&c));
    TEST_ASSERT_EQUAL_INT(JK_DSG_OFF, c.state);

    // Mismatch after a confirmation resets the TRIP streak
    memset(&c, 0, sizeof(c));
    set_dsg(&d, true, true, false);
    jk_dsg_cmd_readback(&c, true, &d);
    set_dsg(&d, true, false, false);
    jk_dsg_cmd_readback(&c, true, &d);
    TEST_ASSERT_EQUAL_UINT8(0, c.trip_frames);
    TEST_ASSERT_FALSE(c.confirmed);

    // Old firmware (AB/AC always 00): enabling is confirmed through the MOSFET bit
    memset(&c, 0, sizeof(c));
    set_dsg(&d, true, false, true);
    TEST_ASSERT_TRUE(jk_dsg_cmd_readback(&c, true, &d));
    TEST_ASSERT_EQUAL_INT(JK_DSG_ON, c.state);

    TEST_ASSERT_FALSE(jk_dsg_cmd_readback(NULL, true, &d));
    TEST_ASSERT_FALSE(jk_dsg_cmd_readback(&c, true, NULL));
}

// ---------------------------------------------------------------------------
// 11. Write ack and alarms
// ---------------------------------------------------------------------------

static void test_write_ack(void)
{
    // Real 0xAC and 0xAB acks (issue #341) and the 0x9D ack (PR #343 thread)
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(k_ack_ac, sizeof(k_ack_ac)));
    TEST_ASSERT_EQUAL_HEX8(JK_CMD_WRITE, jk_frame_command(k_ack_ac));
    TEST_ASSERT_TRUE(jk_is_write_ack(k_ack_ac, sizeof(k_ack_ac), JK_REG_DISCHARGE_SWITCH));
    TEST_ASSERT_FALSE(jk_is_write_ack(k_ack_ac, sizeof(k_ack_ac), JK_REG_CHARGE_SWITCH));
    TEST_ASSERT_FALSE(jk_is_write_ack(k_ack_ac, 20, JK_REG_DISCHARGE_SWITCH));
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(k_ack_ab, sizeof(k_ack_ab)));
    TEST_ASSERT_TRUE(jk_is_write_ack(k_ack_ab, sizeof(k_ack_ab), JK_REG_CHARGE_SWITCH));
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(k_ack_9d, sizeof(k_ack_9d)));
    TEST_ASSERT_TRUE(jk_is_write_ack(k_ack_9d, sizeof(k_ack_9d), JK_REG_BALANCER_SWITCH));
    TEST_ASSERT_FALSE(jk_is_write_ack(k_ack_9d, sizeof(k_ack_9d), JK_REG_DISCHARGE_SWITCH));

    // The 0xAC ack with the 0x9D ack's checksum (01 C0) is corrupt
    memcpy(s_frame, k_ack_ac, sizeof(k_ack_ac));
    s_frame[20] = 0xC0;
    TEST_ASSERT_EQUAL_INT(JK_ERR_CHECKSUM, jk_validate_reply(s_frame, sizeof(k_ack_ac)));

    // A read-one reply (22 bytes, command 0x03) is not an ack
    TEST_ASSERT_EQUAL_INT(JK_OK, jk_validate_reply(k_read_ab_reply, sizeof(k_read_ab_reply)));
    TEST_ASSERT_FALSE(jk_is_write_ack(k_read_ab_reply, sizeof(k_read_ab_reply), JK_REG_CHARGE_SWITCH));
    // Nor is our own write request (22 bytes)
    TEST_ASSERT_FALSE(jk_is_write_ack(k_req_dsg_on_gps, sizeof(k_req_dsg_on_gps), JK_REG_DISCHARGE_SWITCH));
    TEST_ASSERT_FALSE(jk_is_write_ack(NULL, 21, JK_REG_DISCHARGE_SWITCH));
}

static void test_alarm_names(void)
{
    static const char *const long_names[JK_ALARM_BITS] = {
        "Low capacity", "MOSFET overtemperature", "Charge overvoltage", "Discharge undervoltage",
        "Battery overtemperature", "Charge overcurrent", "Discharge overcurrent",
        "Cell voltage difference", "Battery-box overtemperature", "Battery undertemperature",
        "Cell overvoltage", "Cell undervoltage", "309_A protection", "309_B protection",
    };
    static const char *const short_names[JK_ALARM_BITS] = {
        "Low capacity", "MOS overtemp", "Chg overvolt", "Dsg undervolt", "Batt overtemp",
        "Chg overcurr", "Dsg overcurr", "Cell diff", "Box overtemp", "Batt undertemp",
        "Cell overvolt", "Cell undervolt", "309_A prot.", "309_B prot.",
    };
    for (unsigned bit = 0; bit < JK_ALARM_BITS; bit++) {
        TEST_ASSERT_EQUAL_STRING(long_names[bit], jk_alarm_name(bit));
        TEST_ASSERT_EQUAL_STRING(short_names[bit], jk_alarm_short_name(bit));
        TEST_ASSERT_TRUE(strlen(jk_alarm_short_name(bit)) <= 14);
        TEST_ASSERT_EQUAL(bit == 0, jk_alarm_is_warning(bit));
    }
    TEST_ASSERT_NULL(jk_alarm_name(14));
    TEST_ASSERT_NULL(jk_alarm_name(15));
    TEST_ASSERT_NULL(jk_alarm_name(1000));
    TEST_ASSERT_NULL(jk_alarm_short_name(14));
    TEST_ASSERT_FALSE(jk_alarm_is_warning(14));
}

static void test_alarm_top_and_count(void)
{
    TEST_ASSERT_EQUAL_INT(-1, jk_alarm_top(0x0000));
    TEST_ASSERT_EQUAL_UINT(0, jk_alarm_count(0x0000));
    TEST_ASSERT_EQUAL_INT(0, jk_alarm_top(0x0001));     // low capacity: warning only
    TEST_ASSERT_EQUAL_UINT(1, jk_alarm_count(0x0001));
    TEST_ASSERT_EQUAL_INT(8, jk_alarm_top(0x0101));     // a protection beats the warning
    TEST_ASSERT_EQUAL_UINT(2, jk_alarm_count(0x0101));
    TEST_ASSERT_EQUAL_INT(1, jk_alarm_top(0x0006));
    TEST_ASSERT_EQUAL_INT(13, jk_alarm_top(0x2000));
    TEST_ASSERT_EQUAL_INT(1, jk_alarm_top(0x3FFF));
    TEST_ASSERT_EQUAL_UINT(14, jk_alarm_count(0x3FFF));
    // Bits 14 and 15 are unused
    TEST_ASSERT_EQUAL_INT(-1, jk_alarm_top(0xC000));
    TEST_ASSERT_EQUAL_UINT(0, jk_alarm_count(0xC000));
    TEST_ASSERT_EQUAL_INT(0, jk_alarm_top(0xC001));
    TEST_ASSERT_EQUAL_UINT(14, jk_alarm_count(0xFFFF));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_build_read_all);
    RUN_TEST(test_build_activate);
    RUN_TEST(test_build_password);
    RUN_TEST(test_build_write_dsg);
    RUN_TEST(test_build_write_chg);
    RUN_TEST(test_build_cap_too_small);
    RUN_TEST(test_requests_are_not_replies);
    RUN_TEST(test_sum16);

    RUN_TEST(test_real_fake14);
    RUN_TEST(test_real_issue341_16s);
    RUN_TEST(test_real_mpp_solar);
    RUN_TEST(test_real_pdf_old20);
    RUN_TEST(test_real_pdf_published_checksum_rejected);
    RUN_TEST(test_real_b2a24s_1007);
    RUN_TEST(test_real_bd6a17s_710h_discharging);

    RUN_TEST(test_decode_current_rules);
    RUN_TEST(test_parse_current_unknown_id_before_c0);
    RUN_TEST(test_parse_current_unknown_id_after_c0);
    RUN_TEST(test_parse_current_protocol_versions);
    RUN_TEST(test_power);
    RUN_TEST(test_temps);

    RUN_TEST(test_rx_split_across_pushes);
    RUN_TEST(test_rx_byte_by_byte);
    RUN_TEST(test_rx_garbage_before);
    RUN_TEST(test_rx_trailing_start_byte_kept);
    RUN_TEST(test_rx_false_header_in_garbage);
    RUN_TEST(test_rx_rejected_candidates_keep_real_frame);
    RUN_TEST(test_rx_echo_skipped);
    RUN_TEST(test_rx_back_to_back);
    RUN_TEST(test_rx_overflow_drops_oldest);
    RUN_TEST(test_rx_out_too_small);

    RUN_TEST(test_validate_errors);
    RUN_TEST(test_validate_length_bounds);

    RUN_TEST(test_parse_cells_not_multiple_of_3);
    RUN_TEST(test_parse_tlv_overrun);
    RUN_TEST(test_parse_missing_required);
    RUN_TEST(test_parse_more_than_24_cells);
    RUN_TEST(test_parse_zero_mv_cells_excluded);
    RUN_TEST(test_parse_unknown_id_stops_walk);
    RUN_TEST(test_parse_strings);
    RUN_TEST(test_parse_wrong_command_and_args);

    RUN_TEST(test_encode_round_trip);
    RUN_TEST(test_encode_layout);
    RUN_TEST(test_encode_real_frames_round_trip);

    RUN_TEST(test_dsg_model);
    RUN_TEST(test_dsg_cmd_results);

    RUN_TEST(test_write_ack);
    RUN_TEST(test_alarm_names);
    RUN_TEST(test_alarm_top_and_count);

    return UNITY_END();
}
