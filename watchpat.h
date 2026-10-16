// WatchPAT ONE protocol over the Nordic UART Service, ported from the WatchPAT ONE BLE client project (watchpat_ble.py,
// WatchPatProtocol.java). Packet = 24-byte header + payload, chunked into 20-byte BLE writes.
//   0  u16 BE 0xBBBB signature     2  u16 BE opcode        4  u64 LE timestamp (0)   12 u32 LE packet id
//   16 u16 LE total length         18 u16 LE opcode-dep    20 u16 reserved           22 u16 LE CRC-16 (over packet with CRC=0)
// DATA_PACKET payload = records: AAAA sync, u8 id, u8 type, u16 LE len, u16 LE rate, u32 LE flags, then len bytes.
#pragma once
#include <stdint.h>
#include <string.h>

#define WP_HDR 24
#define WP_MAX 2048                     // largest packet accepted (the device sends about 600 bytes)
#define WP_NUS_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define WP_NUS_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // host -> device (write without response)
#define WP_NUS_TX  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // device -> host (notify)

enum {
  WP_OP_ACK = 0x0000, WP_OP_SESSION_START = 0x0100, WP_OP_SESSION_CONFIRM = 0x0200, WP_OP_CONFIG = 0x0500,
  WP_OP_START_ACQ = 0x0600, WP_OP_STOP_ACQ = 0x0700, WP_OP_DATA = 0x0800, WP_OP_END_OF_TEST = 0x0900, WP_OP_ERROR = 0x0A00,
  WP_OP_BIT_REQ = 0x1200, WP_OP_BIT_RESP = 0x1300, WP_OP_TECH_REQ = 0x1500, WP_OP_TECH = 0x1600,
  WP_OP_FINGER_REQ = 0x2500, WP_OP_FINGER = 0x2600, WP_OP_IS_PAIRED = 0x2A00, WP_OP_IS_PAIRED_RESP = 0x2B00
};

static uint16_t wp_crc16(const uint8_t *d, size_t n) {   // CCITT, poly 0x1021, init 0xFFFF, bit by bit
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++)
    for (uint8_t mask = 0x80; mask; mask >>= 1) {
      bool carry = crc & 0x8000;
      if (d[i] & mask) carry = !carry;
      crc <<= 1;
      if (carry) crc ^= 0x1021;
    }
  return crc;
}
static bool wp_crc_ok(const uint8_t *p, size_t n) {   // CRC over the packet with bytes 22-23 taken as zero, no copy
  if (n < WP_HDR || n > WP_MAX) return false;
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    uint8_t d = (i == 22 || i == 23) ? 0 : p[i];
    for (uint8_t mask = 0x80; mask; mask >>= 1) { bool carry = crc & 0x8000; if (d & mask) carry = !carry; crc <<= 1; if (carry) crc ^= 0x1021; }
  }
  uint16_t c = crc, stored = p[22] | (p[23] << 8);
  return stored == c || stored == (uint16_t)((c >> 8) | (c << 8));   // the device stores it byte-swapped sometimes
}
static inline uint16_t wp_opcode(const uint8_t *p) { return (p[2] << 8) | p[3]; }
static inline uint32_t wp_pid(const uint8_t *p) { return p[12] | (p[13] << 8) | (p[14] << 16) | ((uint32_t)p[15] << 24); }
static inline uint16_t wp_len(const uint8_t *p) { return p[16] | (p[17] << 8); }

// build a packet into out (>= 24 + plen bytes), returns its length
static size_t wp_build(uint8_t *out, uint16_t opcode, uint32_t pid, const uint8_t *payload, size_t plen) {
  size_t total = WP_HDR + plen;
  memset(out, 0, WP_HDR);
  out[0] = out[1] = 0xBB; out[2] = opcode >> 8; out[3] = opcode & 0xFF;
  out[12] = pid; out[13] = pid >> 8; out[14] = pid >> 16; out[15] = pid >> 24;
  out[16] = total; out[17] = total >> 8;
  if (plen) memcpy(out + WP_HDR, payload, plen);
  uint16_t c = wp_crc16(out, total);
  out[22] = c; out[23] = c >> 8;
  return total;
}
// SESSION_START payload: u32 BE mobile id 0, u8 mode (1 normal, 4 resume), 14-byte OS version "14", NUL
static size_t wp_build_session_start(uint8_t *out, uint32_t pid, uint8_t mode = 1) {
  uint8_t p[20] = {0}; p[4] = mode; p[5] = '1'; p[6] = '4';
  return wp_build(out, WP_OP_SESSION_START, pid, p, sizeof p);
}
// ACK: packet id = id of the packet being acknowledged; payload u16 BE opcode, u8 status, 2 pad
static size_t wp_build_ack(uint8_t *out, uint16_t opcode, uint32_t pid, uint8_t status = 0) {
  uint8_t p[5] = {(uint8_t)(opcode >> 8), (uint8_t)opcode, status, 0, 0};
  return wp_build(out, WP_OP_ACK, pid, p, sizeof p);
}

// reassembler: feed BLE chunks, pull complete packets
struct WpReasm { uint8_t buf[WP_MAX + 64]; size_t n = 0; };
static void wp_feed(WpReasm &r, const uint8_t *d, size_t n) {
  if (r.n + n > sizeof r.buf) r.n = 0;            // overflow: resync
  memcpy(r.buf + r.n, d, n); r.n += n;
}
// returns the length of the next complete packet copied to out (>= 4096 bytes), or 0
static size_t wp_next(WpReasm &r, uint8_t *out) {
  size_t pos = 0;
  while (r.n - pos >= 18) {
    if (r.buf[pos] != 0xBB || r.buf[pos + 1] != 0xBB) { pos++; continue; }
    size_t total = wp_len(r.buf + pos);
    if (total < WP_HDR || total > WP_MAX) { pos++; continue; }
    if (r.n - pos < total) break;
    memcpy(out, r.buf + pos, total);
    pos += total;
    memmove(r.buf, r.buf + pos, r.n - pos); r.n -= pos;
    return total;
  }
  if (pos) { memmove(r.buf, r.buf + pos, r.n - pos); r.n -= pos; }
  return 0;
}

// walk the records of a DATA_PACKET payload; counts per record id (1 oxiA, 2 oxiB, 3 PAT, 4 chest, 5 metric, 6 motion, 0x0C/0x0D events)
struct WpCounts { uint32_t oxia, oxib, pat, chest, metric, motion, event, other, bad; };
static void wp_count_records(const uint8_t *payload, size_t n, WpCounts &c) {
  size_t pos = 3;                                  // u8 record count + u16 sub-header precede the records
  while (pos + 12 <= n) {
    if (payload[pos] != 0xAA || payload[pos + 1] != 0xAA) { c.bad++; return; }
    uint8_t id = payload[pos + 2]; uint16_t len = payload[pos + 4] | (payload[pos + 5] << 8);
    switch (id) { case 1: c.oxia++; break; case 2: c.oxib++; break; case 3: c.pat++; break; case 4: c.chest++; break;
                  case 5: c.metric++; break; case 6: c.motion++; break; case 0x0C: case 0x0D: c.event++; break; default: c.other++; }
    pos += 12 + len;
  }
}
