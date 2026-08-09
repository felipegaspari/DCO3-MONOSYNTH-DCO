## Serial & Parameter Protocol – Usage Guide

Shared **inner** serial + parameter infrastructure for DCO4-family MCUs. On this board
(DCO3-MONOSYNTH) the only peer UART is Serial2 ↔ Input; USB CDC (`ENABLE_USB_CONTROL`)
speaks the same inner frames for [`tools/dco_control`](../tools/dco_control/README.md).

---

## 1. Headers

| Header | Role |
|--------|------|
| `params_def.h` | Canonical `enum ParamId : uint16_t` (wire id is still `uint8`) |
| `param_router.h` | `ParamDescriptorT` table + O(1) jump dispatch |
| `serial_input_protocol.h` | Command bytes + payload sizes (`'a'`–`'d'`, `'p'`, `'q'`, `'x'`) |
| `serial_param_protocol.h` | LE encode/decode for `'p'` / `'w'` / `'x'` |
| `serial_frame.h` | Buffer COBS encode/decode + `serial_frame_stuff` / `unstuff` / `write()`. Default RAW; `#define SERIAL_FRAMING_COBS` wraps `COBS(inner)+0x00` |
| `serial_parser.h` | Non-blocking parser: 256-entry cmd LUT, idle timeout (`SERIAL_FRAME_TIMEOUT_US` = 500 µs), drain budget 64. RAW fixed-length or COBS accumulate-until-`0x00` |
| `serial_protocol.h` | Compatibility stub → `serial_input_protocol.h` (Mainboard `'n'`/`'o'`/`'s'` retired) |

**USB bench link:** `serial_usb_task()` uses a second `SerialParserContext` and the same LUT as Serial2. Only host → DCO is framed; DCO → host is plain debug text. Panel Serial2 and USB CDC drain on Core 0 `timer1msFlag` (~1 ms). USB/DIN MIDI still runs every `loop()`. CDC drain is skipped when the host has not opened `Serial`.

**MIDI CC:** `midi_cc_apply()` writes ADSR/filter block globals directly (`CC_LOCAL_*`). Everything else, including `PARAM_PW_VALUE` and `PARAM_ADSR1_TO_VCA`, goes through `update_parameters()`. Persistable ParamId CCs also `serial_echo_persistable_param16()` so Input LittleFS save matches what you hear. `'a'`–`'d'` domains are not mirrored.

---

## 2. Inner frames (handlers always see this)

Little-endian multi-byte fields. No finish byte. `0x00` is reserved (COBS delimiter) and is never a command.

| Cmd | Payload | Meaning |
|-----|---------|---------|
| `'a'`/`'b'`/`'c'` | 8 | ADSR A,D,S,R as `uint16`. A/D/R exp-mapped 0..25000; S linear. Direct globals + dirty flags — **not** `update_parameters` |
| `'d'` | 8 | `CUTOFF`, `RESONANCE`, `ADSR2toVCF`, `LFO2toVCF` + scale bake |
| `'p'` | 3 | `[id:u8][value:i16]` → `update_parameters`. Also DCO→Input persistable mirror (USB/MIDI only; never panel ingress) |
| `'q'` | 8 | Preset name, 8 ASCII chars |
| `'x'` | 5 | DCO→Input `[id:u8][value:u32 LE]` (gap 154 / cal 155); `serial_frame_write` |

On-wire **default** = inner (RAW). **`#define SERIAL_FRAMING_COBS`:** `COBS(cmd+payload) + 0x00`. Handlers still see inner only. Host A/B: `dco_control --cobs` or `DCO_SERIAL_COBS=1` (must match firmware or controls look ignored). Codec is buffer-in/buffer-out so a later SPI link can reuse `serial_frame_unstuff` after reading until `0x00`.

**Input and Screen speak the same slim inner protocol.** Flash all three boards together. `SERIAL_INNER_MAX_PAYLOAD` defaults to 8 (DCO); Input/Screen override to 17 for Screen `'q'` scroll. Screen-only cmds (`'w'`/`'y'`/`'s'`/`'c'`, 17-byte `'q'`) never reach DCO. Former `'e'`/`'f'` are `'p'` ids 222 / 210. `SERIAL_FRAMING_COBS` must match on all three (commented = RAW).

---

## 3. Adding a parameter (`ParamId`)

1. Append a new id in `params_def.h` (do not renumber; stay ≤ 255 on the wire).
2. Implement `apply_param_*` and add one row to `paramTable[]` in `params.ino`.
3. Call `init_param_router()` at boot (already in `setup()`).
4. Send `'p'` + id + i16 LE. Host: one `Param` row in `tools/dco_control/params.py`.

No new serial command unless it is a new **1 ms analog block**.

---

## 4. Adding a serial command (rare)

1. Add the cmd + payload length to `serial_input_protocol.h` (`serial_input_payload_len`).
2. Implement `on_frame` and add a `SerialCommandDef` row in `Serial.ino`.
3. Keep `0x00` unused. Prefer LE. Send via `serial_frame_write()`, not ad-hoc UART bytes.

---

## 5. Parser notes

- O(1) lookup: `serial_command_table_init()` fills `payload_len[256]` / `on_frame[256]`.
- Timeout (`SERIAL_FRAME_TIMEOUT_US` = 500 µs) runs only when mid-frame **and** the stream is idle — no `micros()` per byte. COBS: mid-frame means stuffed bytes received, delimiter not yet seen.
- RAW: cmd LUT + fixed payload. COBS: accumulate until `0x00`, decode, unpack, then LUT length check.
- Drain snapshots `available()` once, then `read()`s up to `SERIAL_DRAIN_BYTE_BUDGET` (64) so one 1 ms Input burst (incl. COBS ~11 B/block) finishes in a single tick.

---

## 6. New MCU checklist

1. Copy `params_def.h`, `param_router.h`, `serial_input_protocol.h`, `serial_param_protocol.h`, `serial_frame.h`, `serial_parser.h`.
2. `paramTable[]` + `init_param_router()` + `update_parameters(uint16_t, int16_t)`.
3. Per UART: `SerialCommandDef[]` → LUT, `serial_parser_drain()`, `serial_frame_write()` for TX.
4. Keep ParamIds and inner layouts identical across MCUs.
