#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/DCO-PROTOCOL/serial_frame.h"
#ifndef SERIAL_FRAME_H
#define SERIAL_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "serial_input_protocol.h"  // also supplies the INPUT_ALWAYS_INLINE fallback

// -----------------------------------------------------------------------------
// Framing layer: inner protocol vs on-wire bytes.
//
// Inner frame (handlers / ParamId — never changes with framing):
//   [cmd:1][payload:N]   N from serial_input_payload_len(cmd)
//
// On-wire RAW (default): identical to inner.
// On-wire COBS (#define SERIAL_FRAMING_COBS): COBS(inner) + 0x00.
//
// Codec is buffer-in / buffer-out (no Stream type) so UART today and SPI later
// share the same helpers. 0x00 is reserved and is never a command byte.
// -----------------------------------------------------------------------------

#if defined(SERIAL_FRAMING_COBS) && defined(SERIAL_FRAMING_RAW)
#error "Define only one of SERIAL_FRAMING_COBS or SERIAL_FRAMING_RAW"
#endif
#if !defined(SERIAL_FRAMING_COBS) && !defined(SERIAL_FRAMING_RAW)
#define SERIAL_FRAMING_RAW 1
#endif

static constexpr uint8_t SERIAL_FRAME_DELIMITER = 0x00;

// Largest inner payload. DCO default 8; Input/Screen set 17 before include (Screen 'q').
#ifndef SERIAL_INNER_MAX_PAYLOAD
#define SERIAL_INNER_MAX_PAYLOAD 8
#endif

// Stuffed-frame buffer: inner (1+payload) + COBS worst-case (+1 per 254) + delimiter.
static constexpr uint8_t SERIAL_STUFFED_MAX =
    (uint8_t)(1u + SERIAL_INNER_MAX_PAYLOAD + 1u + 1u);

// Pack inner [cmd][payload] into dst. Returns inner length (1 + payload_len).
static inline INPUT_ALWAYS_INLINE uint8_t serial_inner_pack(uint8_t* dst, uint8_t cmd,
                                        const uint8_t* payload, uint8_t payload_len)
{
  dst[0] = cmd;
  for (uint8_t i = 0; i < payload_len; ++i) {
    dst[1 + i] = payload[i];
  }
  return (uint8_t)(1u + payload_len);
}

// Split a decoded inner blob into cmd + payload. Returns false if empty.
static inline INPUT_ALWAYS_INLINE bool serial_inner_unpack(const uint8_t* inner, uint8_t inner_len,
                                       uint8_t& cmd, const uint8_t*& payload,
                                       uint8_t& payload_len)
{
  if (inner_len < 1) return false;
  cmd = inner[0];
  payload = inner + 1;
  payload_len = (uint8_t)(inner_len - 1u);
  return true;
}

// Consistent Overhead Byte Stuffing. dst never contains 0x00.
// Returns bytes written, or -1 if dst_cap is too small / src invalid.
static inline INPUT_ALWAYS_INLINE int serial_cobs_encode(const uint8_t* src, uint8_t src_len,
                                     uint8_t* dst, uint8_t dst_cap)
{
  if (dst_cap < 1) return -1;
  uint8_t dst_len = 0;
  uint8_t code_idx = 0;
  dst[dst_len++] = 0;
  uint8_t code = 1;
  for (uint8_t i = 0; i < src_len; ++i) {
    if (src[i] == 0) {
      dst[code_idx] = code;
      if (dst_len >= dst_cap) return -1;
      code_idx = dst_len;
      dst[dst_len++] = 0;
      code = 1;
    } else {
      if (dst_len >= dst_cap) return -1;
      dst[dst_len++] = src[i];
      code++;
      if (code == 0xFF) {
        dst[code_idx] = code;
        if (i + 1u < src_len) {
          if (dst_len >= dst_cap) return -1;
          code_idx = dst_len;
          dst[dst_len++] = 0;
          code = 1;
        }
      }
    }
  }
  dst[code_idx] = code;
  return (int)dst_len;
}

// Decode stuffed bytes (no trailing 0x00). Returns decoded length, or -1 on error.
static inline INPUT_ALWAYS_INLINE int serial_cobs_decode(const uint8_t* src, uint8_t src_len,
                                     uint8_t* dst, uint8_t dst_cap)
{
  uint8_t dst_len = 0;
  uint8_t i = 0;
  while (i < src_len) {
    uint8_t code = src[i++];
    if (code == 0) return -1;
    uint8_t copy = (uint8_t)(code - 1u);
    if ((uint16_t)i + copy > src_len) return -1;
    if ((uint16_t)dst_len + copy > dst_cap) return -1;
    for (uint8_t j = 0; j < copy; ++j) {
      dst[dst_len++] = src[i++];
    }
    if (code != 0xFF && i < src_len) {
      if (dst_len >= dst_cap) return -1;
      dst[dst_len++] = 0;
    }
  }
  return (int)dst_len;
}

// Pack inner, then RAW copy or COBS+0x00 into dst. Returns on-wire length, or -1.
static inline INPUT_ALWAYS_INLINE int serial_frame_stuff(uint8_t cmd, const uint8_t* payload,
                                     uint8_t payload_len, uint8_t* dst,
                                     uint8_t dst_cap)
{
  if (payload_len > SERIAL_INNER_MAX_PAYLOAD) return -1;
  uint8_t inner[1 + SERIAL_INNER_MAX_PAYLOAD];
  uint8_t n = serial_inner_pack(inner, cmd, payload, payload_len);
#ifdef SERIAL_FRAMING_COBS
  if (dst_cap < 2) return -1;
  int enc = serial_cobs_encode(inner, n, dst, (uint8_t)(dst_cap - 1u));
  if (enc < 0) return -1;
  dst[enc] = SERIAL_FRAME_DELIMITER;
  return enc + 1;
#else
  if (n > dst_cap) return -1;
  for (uint8_t i = 0; i < n; ++i) dst[i] = inner[i];
  return (int)n;
#endif
}

// Inverse of serial_frame_stuff. COBS: trailing 0x00 optional (stripped if present).
// Copies payload into payload_out. Returns false on empty / corrupt / overflow.
static inline INPUT_ALWAYS_INLINE bool serial_frame_unstuff(const uint8_t* wire, uint8_t wire_len,
                                        uint8_t& cmd, uint8_t* payload_out,
                                        uint8_t& payload_len)
{
#ifdef SERIAL_FRAMING_COBS
  if (wire_len < 1) return false;
  uint8_t stuffed_len = wire_len;
  if (wire[wire_len - 1u] == SERIAL_FRAME_DELIMITER) stuffed_len--;
  if (stuffed_len == 0) return false;
  uint8_t inner[1 + SERIAL_INNER_MAX_PAYLOAD];
  int decoded = serial_cobs_decode(wire, stuffed_len, inner, sizeof(inner));
  if (decoded < 1) return false;
  const uint8_t* pay = nullptr;
  if (!serial_inner_unpack(inner, (uint8_t)decoded, cmd, pay, payload_len)) return false;
#else
  const uint8_t* pay = nullptr;
  if (!serial_inner_unpack(wire, wire_len, cmd, pay, payload_len)) return false;
#endif
  if (payload_len > SERIAL_INNER_MAX_PAYLOAD) return false;
  for (uint8_t i = 0; i < payload_len; ++i) payload_out[i] = pay[i];
  return true;
}

// Write one inner frame onto any stream with write(buf, n). UART today; SPI later.
template<typename StreamT>
static inline INPUT_ALWAYS_INLINE void serial_frame_write(StreamT& stream, uint8_t cmd,
                                      const uint8_t* payload, uint8_t payload_len)
{
  uint8_t buf[SERIAL_STUFFED_MAX];
  int n = serial_frame_stuff(cmd, payload, payload_len, buf, sizeof(buf));
  if (n > 0) stream.write(buf, (size_t)n);
}

#endif // SERIAL_FRAME_H
