#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/DCO-PROTOCOL/serial_parser.h"
#ifndef SERIAL_PARSER_H
#define SERIAL_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "serial_frame.h"  // reaches serial_input_protocol.h for INPUT_ALWAYS_INLINE

// -----------------------------------------------------------------------------
// Inner-frame parser. Framing is separate (RAW vs COBS in serial_frame.h).
// Handlers always see cmd + payload; do not put framing logic in them.
//
// O(1) command lookup: SerialCommandTable is a 256-entry LUT.
// payload_len[cmd]==0 means unknown / ignore.
// 0x00 (SERIAL_FRAME_DELIMITER) is never a valid command.
// -----------------------------------------------------------------------------

// Partial-frame abort when RX is idle mid-frame (microseconds, not ms).
static const uint32_t SERIAL_FRAME_TIMEOUT_US = 500;

// Max bytes drained per loop() call on each serial link. 64 covers one 1 ms
// Input burst (a+b+c+d, COBS ~11 B each, plus a couple of 'p' frames).
static const uint8_t SERIAL_DRAIN_BYTE_BUDGET = 64;

// Max MIDI bytes parsed per loop() call on each MIDI port (one read() ≈ one byte).
static const uint8_t MIDI_DRAIN_BYTE_BUDGET = 32;

enum SerialParserState : uint8_t {
  SERIAL_WAIT_FOR_CMD = 0,
  SERIAL_READ_PAYLOAD = 1
};

struct SerialCommandDef {
  uint8_t  cmd;
  uint8_t  payload_len;
  void   (*on_frame)(char cmd, const uint8_t* payload, uint8_t len);
};

struct SerialCommandTable {
  uint8_t payload_len[256];
  void  (*on_frame[256])(char cmd, const uint8_t* payload, uint8_t len);
};

struct SerialParserContext {
  SerialParserState state;
  uint8_t           command;
  uint8_t           payload[SERIAL_INNER_MAX_PAYLOAD];
  uint8_t           expected_len;
  uint8_t           received_len;
  uint8_t           rx[SERIAL_STUFFED_MAX];  // COBS stuffed bytes (no delimiter)
  uint8_t           rx_len;
  uint32_t          last_byte_time_us;
};

static inline INPUT_ALWAYS_INLINE void serial_parser_reset(SerialParserContext& ctx) {
  ctx.state             = SERIAL_WAIT_FOR_CMD;
  ctx.command           = 0;
  ctx.expected_len      = 0;
  ctx.received_len      = 0;
  ctx.rx_len            = 0;
  ctx.last_byte_time_us = 0;
}

static inline INPUT_ALWAYS_INLINE bool serial_parser_in_frame(const SerialParserContext& ctx) {
#ifdef SERIAL_FRAMING_COBS
  return ctx.rx_len > 0;
#else
  return ctx.state == SERIAL_READ_PAYLOAD;
#endif
}

static inline INPUT_ALWAYS_INLINE void serial_command_table_init(
    SerialCommandTable& lut,
    const SerialCommandDef* commands,
    size_t numCommands)
{
  memset(lut.payload_len, 0, sizeof(lut.payload_len));
  memset(lut.on_frame, 0, sizeof(lut.on_frame));
  for (size_t i = 0; i < numCommands; ++i) {
    uint8_t c = commands[i].cmd;
    if (c == SERIAL_FRAME_DELIMITER) continue;
    lut.payload_len[c] = commands[i].payload_len;
    lut.on_frame[c]    = commands[i].on_frame;
  }
}

static inline INPUT_ALWAYS_INLINE void serial_parser_check_timeout(SerialParserContext& ctx,
                                               uint32_t now_us)
{
  if (serial_parser_in_frame(ctx) && ctx.last_byte_time_us != 0) {
    if ((uint32_t)(now_us - ctx.last_byte_time_us) > SERIAL_FRAME_TIMEOUT_US) {
      serial_parser_reset(ctx);
    }
  }
}

// Mainboard RX tracing. Off unless MB_UART_RX_LOG is defined, and it needs
// serial_param_protocol.h (ParamFrame/decode_param_p) included first, which the
// Mainboard's Serial.h already does.
#ifdef MB_UART_RX_LOG
extern const char* g_mb_uart_rx_port;

static inline void mb_uart_rx_log_cmd(uint8_t cmd) {
#ifdef ENABLE_SERIAL
  Serial.print("mb ");
  Serial.print(g_mb_uart_rx_port ? g_mb_uart_rx_port : "?");
  Serial.print(" rx '");
  Serial.print((char)cmd);
  Serial.println('\'');
#endif
}

static inline void mb_uart_rx_log_p(uint8_t id, int16_t value) {
#ifdef ENABLE_SERIAL
  Serial.print("mb ");
  Serial.print(g_mb_uart_rx_port ? g_mb_uart_rx_port : "?");
  Serial.print(" 'p' ");
  Serial.print((unsigned)id);
  Serial.print(' ');
  Serial.println((int)value);
#endif
}
#endif

static inline INPUT_ALWAYS_INLINE void serial_parser_dispatch(
    const SerialCommandTable& lut,
    uint8_t cmd,
    const uint8_t* payload,
    uint8_t payload_len)
{
  if (cmd == SERIAL_FRAME_DELIMITER) return;
  if (lut.payload_len[cmd] == 0 || lut.payload_len[cmd] != payload_len) return;
#ifdef MB_UART_RX_LOG
  if (cmd == 'p' && payload_len >= 3) {
    ParamFrame frame;
    decode_param_p(payload, frame);
    mb_uart_rx_log_p(frame.id, (int16_t)frame.value);
  }
#endif
  void (*fn)(char, const uint8_t*, uint8_t) = lut.on_frame[cmd];
  if (fn) fn((char)cmd, payload, payload_len);
}

#ifdef SERIAL_FRAMING_COBS
static inline INPUT_ALWAYS_INLINE void serial_parser_process_byte_cobs(
    SerialParserContext& ctx,
    const SerialCommandTable& lut,
    uint8_t b)
{
  if (b == SERIAL_FRAME_DELIMITER) {
    if (ctx.rx_len == 0) return;
    uint8_t cmd = 0;
    uint8_t payload_len = 0;
    if (serial_frame_unstuff(ctx.rx, ctx.rx_len, cmd, ctx.payload, payload_len)) {
      serial_parser_dispatch(lut, cmd, ctx.payload, payload_len);
    }
    serial_parser_reset(ctx);
    return;
  }
  if (ctx.rx_len >= (uint8_t)(SERIAL_STUFFED_MAX - 1u)) {
    serial_parser_reset(ctx);
    return;
  }
  ctx.rx[ctx.rx_len++] = b;
  ctx.state = SERIAL_READ_PAYLOAD;
}
#endif

// Feed one on-wire byte.
static inline INPUT_ALWAYS_INLINE void serial_parser_process_byte(
    SerialParserContext& ctx,
    const SerialCommandTable& lut,
    uint8_t b)
{
#ifdef SERIAL_FRAMING_COBS
  serial_parser_process_byte_cobs(ctx, lut, b);
#else
  if (ctx.state == SERIAL_WAIT_FOR_CMD) {
    if (b == SERIAL_FRAME_DELIMITER) return;
#ifdef MB_UART_RX_LOG
    mb_uart_rx_log_cmd(b);
#endif
    uint8_t len = lut.payload_len[b];
    if (len == 0) return;
    ctx.command      = b;
    ctx.expected_len = len;
    ctx.received_len = 0;
    ctx.state        = SERIAL_READ_PAYLOAD;
    return;
  }

  if (ctx.received_len < SERIAL_INNER_MAX_PAYLOAD) {
    ctx.payload[ctx.received_len++] = b;
  }

  if (ctx.received_len >= ctx.expected_len) {
    serial_parser_dispatch(lut, ctx.command, ctx.payload, ctx.received_len);
    serial_parser_reset(ctx);
  }
#endif
}

// Drain up to byte_budget bytes. One available() snapshot, then read n.
// Timeout only when mid-frame and the stream is idle (no micros() per byte).
template<typename StreamT>
static inline INPUT_ALWAYS_INLINE void serial_parser_drain(
    SerialParserContext& ctx,
    const SerialCommandTable& lut,
    StreamT& stream,
    uint8_t byte_budget)
{
  int avail = stream.available();
  if (avail <= 0) {
    if (serial_parser_in_frame(ctx)) {
      serial_parser_check_timeout(ctx, micros());
    }
    return;
  }

  uint8_t n = (avail > byte_budget) ? byte_budget : (uint8_t)avail;
  uint32_t now = 0;
  bool stamped = false;
  while (n > 0) {
    uint8_t b = stream.read();
    serial_parser_process_byte(ctx, lut, b);
    if (serial_parser_in_frame(ctx)) {
      if (!stamped) {
        now = micros();
        stamped = true;
      }
      ctx.last_byte_time_us = now;
    }
    n--;
  }
}

#endif // SERIAL_PARSER_H
