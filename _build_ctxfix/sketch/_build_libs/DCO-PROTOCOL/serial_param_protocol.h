#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/DCO-PROTOCOL/serial_param_protocol.h"
#ifndef SERIAL_PARAM_PROTOCOL_H
#define SERIAL_PARAM_PROTOCOL_H

#include <stdint.h>
#include "serial_input_protocol.h"  // also supplies the INPUT_ALWAYS_INLINE fallback

// Decode / encode helpers for parameter payloads (inner frame, after framing).
//
//   'p' : [id:u8][value:i16 LE]
//   'w' : [id:u8][value:u8]          — Screen UI only (Input→Screen)
//   'x' : [id:u8][value:u32 LE]

struct ParamFrame {
  uint8_t id;
  int32_t value;
};

static inline INPUT_ALWAYS_INLINE uint16_t decode_u16_le(const uint8_t* b) {
  return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline INPUT_ALWAYS_INLINE int16_t decode_i16_le(const uint8_t* b) {
  return (int16_t)decode_u16_le(b);
}

static inline INPUT_ALWAYS_INLINE uint32_t decode_u32_le(const uint8_t* b) {
  return (uint32_t)b[0]
       | ((uint32_t)b[1] << 8)
       | ((uint32_t)b[2] << 16)
       | ((uint32_t)b[3] << 24);
}

static inline INPUT_ALWAYS_INLINE void encode_u16_le(uint8_t* dst, uint16_t v) {
  dst[0] = (uint8_t)(v & 0xFF);
  dst[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline INPUT_ALWAYS_INLINE void encode_u32_le(uint8_t* dst, uint32_t v) {
  dst[0] = (uint8_t)(v & 0xFF);
  dst[1] = (uint8_t)((v >> 8) & 0xFF);
  dst[2] = (uint8_t)((v >> 16) & 0xFF);
  dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline INPUT_ALWAYS_INLINE void decode_param_p(const uint8_t* payload, ParamFrame& out) {
  out.id = payload[0];
  out.value = decode_i16_le(payload + 1);
}

static inline INPUT_ALWAYS_INLINE uint8_t encode_param_p(uint8_t* dst, uint8_t id, int16_t value) {
  dst[0] = id;
  encode_u16_le(dst + 1, (uint16_t)value);
  return INPUT_SERIAL_LEN_PARAM_16;
}

static constexpr uint8_t SERIAL_LEN_PARAM_8 = 2;  // Screen 'w': [id][u8]

static inline INPUT_ALWAYS_INLINE void decode_param_w(const uint8_t* payload, ParamFrame& out) {
  out.id = payload[0];
  out.value = (int8_t)payload[1];
}

static inline INPUT_ALWAYS_INLINE uint8_t encode_param_w(uint8_t* dst, uint8_t id, uint8_t value) {
  dst[0] = id;
  dst[1] = value;
  return SERIAL_LEN_PARAM_8;
}

static inline INPUT_ALWAYS_INLINE void decode_param_x(const uint8_t* payload, ParamFrame& out) {
  out.id = payload[0];
  out.value = (int32_t)decode_u32_le(payload + 1);
}

static inline INPUT_ALWAYS_INLINE uint8_t encode_param32(uint8_t* dst, uint8_t id, uint32_t value) {
  dst[0] = id;
  encode_u32_le(dst + 1, value);
  return INPUT_SERIAL_LEN_PARAM_32;
}

#endif  // SERIAL_PARAM_PROTOCOL_H
