#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/DCO-PROTOCOL/README.md"
# DCO-PROTOCOL

The serial protocol and parameter definitions shared by every board in
**DCO3-MONOSYNTH** and **DCO4-REBORN**. These six headers used to be copy-pasted
into each sketch folder and hand-synced on every protocol change; they now live
here once, packaged as an Arduino library.

| Header | Role |
|--------|------|
| `params_def.h` | Canonical `enum ParamId : uint16_t` (the wire id is still `uint8`) |
| `param_router.h` | `ParamDescriptorT` table, 256-entry jump dispatch and linear-scan fallback |
| `serial_input_protocol.h` | Command bytes and payload sizes |
| `serial_param_protocol.h` | LE encode/decode for `'p'` / `'w'` / `'x'` |
| `serial_frame.h` | Inner pack/unpack, COBS codec, `serial_frame_write()` |
| `serial_parser.h` | Non-blocking parser: 256-entry command LUT, idle timeout, drain budget |

## How a board uses it

The library is checked out at each superproject root and symlinked into the
board's library folder, the same way `DCO_Noise` and `RoxMux_FELA` are:

```
<board>/_build_libs/DCO-PROTOCOL -> ../../DCO-PROTOCOL
SCREEN-CONTROLLER/libraries/DCO-PROTOCOL -> ../../DCO-PROTOCOL
```

The library is flat (headers at the repo root), so ordinary quoted includes keep
working: `#include "params_def.h"` needs no path.

**Configure before you include.** Each board sets its own knobs in its `Serial.h`
and then includes the protocol headers:

```c
#ifndef SERIAL_INNER_MAX_PAYLOAD
#define SERIAL_INNER_MAX_PAYLOAD 36   // largest inner payload this board sees
#endif
// #define SERIAL_FRAMING_COBS        // must match every board on the link

#include "serial_input_protocol.h"
#include "serial_frame.h"
#include "serial_parser.h"
#include "serial_param_protocol.h"
```

Two other hooks are read the same way:

- `INPUT_ALWAYS_INLINE` — a board that pins the serial hot path in SRAM defines
  it (see the Input board's `sram_hot.h`) before including. Everywhere else it
  falls back to empty, so only that board pays for the decoration.
- `MB_UART_RX_LOG` — enables the Mainboard's RX tracing in `serial_parser.h`.
  It needs `serial_param_protocol.h` included first, which the Mainboard already
  does.

## Inner frames

Every handler sees the inner frame, whatever the on-wire framing is:

```
[cmd:1][payload:N]      multi-byte fields little-endian, no finish byte
```

`0x00` is reserved as the COBS delimiter and is never a command.

| Cmd | Payload | Meaning |
|-----|---------|---------|
| `'a'`/`'b'`/`'c'` | 8 | ADSR A,D,S,R as `uint16`. A/D/R exp-mapped 0..25000, S linear |
| `'d'` | 8 | `CUTOFF`, `RESONANCE`, `ADSR2toVCF`, `LFO2toVCF` |
| `'p'` | 3 | `[id:u8][value:i16 LE]` — the general parameter frame |
| `'q'` | 16 | Preset name, 16 ASCII chars |
| `'x'` | 5 | `[id:u8][value:u32 LE]` — DCO→Input gap (154) / calibration (155) |
| `'B'` | 36 | Bulk restore chunk: `[target][slot][offset:u16 LE][32 data]` |
| `'C'` | 8 | Bulk commit: `[target][slot][size:u16 LE][crc32 LE]` |
| `'N'` | 1 (padding) | Input→DCO: send the whole preset directory |
| `'O'` | 17 | DCO→Input: `[slot:u8][name:16]` directory entry |
| `'L'` | 1 | DCO→Input: `[slot:u8]` just finished loading |

This table lists every command on every link. A board only acts on the ones it
registers in its own `SerialCommandDef[]`; anything else is dropped by the
parser. `serial_input_payload_len()` is reference material, not the dispatch
path.

On-wire framing is RAW by default (identical to the inner frame). With
`SERIAL_FRAMING_COBS` it becomes `COBS(inner) + 0x00`. Every board on a link must
agree, or controls silently look ignored. The host tool selects it with
`dco_control --cobs` or `DCO_SERIAL_COBS=1`. The codec is buffer-in/buffer-out,
so a later SPI link can reuse `serial_frame_unstuff()` after reading to `0x00`.

## Who mirrors what

A parameter can be changed from three places: the panel, the host tool over USB
(or a MIDI CC), and a preset recall. Only the panel's own edits start at the
panel, so the other two have to be mirrored back or the panel state and the
Screen go stale. The DCO tags its ingress (`g_param_ingress`) and mirrors only
what did **not** come from the panel, which is what keeps the link from echoing.

| Frame | DCO → panel side | Panel side → Screen |
|-------|------------------|---------------------|
| `'a'` EnvVCA | yes, exp-mapped | yes, as `'a'` in fader domain |
| `'b'` EnvVCF | yes, exp-mapped | yes, as `'b'` in fader domain |
| `'c'` EnvDCO | yes, exp-mapped | no — the Screen link's `'c'` is char-select |
| `'d'` filter | yes | as `'p'` 191..194, only the fields that changed |
| `'p'` | persistable ids only (`preset_param_is_persistable`) | yes, verbatim |
| `'x'` | gap 154, cal offsets 155 | 154 only |
| `'O'` / `'L'` | yes | preset name / slot display |

"Panel side" is the Input board on DCO3 and the Mainboard on DCO4; on DCO4 the
Mainboard owns the analog envelopes and filter, so it applies `'a'`/`'b'`/`'d'`
itself *and* relays them to the Input, while `'c'` is relay-only.

Two conversions matter. The Input sends the DCO exp-mapped times
(`linToExpLookup[]`) but sends the Screen raw fader values, so a mirrored ADSR
block is inverted back to a fader index (`exp_to_lin_index()`) before it is
stored or displayed. And the filter pots are analog with no ParamId of their
own, so the Screen learns them through the 191..194 UI ids rather than the `'d'`
block, which it does not display.

## Adding a parameter

1. Append a new id in `params_def.h`. Never renumber, and stay at or below 255,
   since the wire id is a `uint8`.
2. Implement `apply_param_*` and add one row to `paramTable[]` in the board's
   `params.ino`.
3. Send `'p'` with the id and an `int16` LE value.
4. Add the matching `Param` row in `DCO-CONTROL-PANEL/params.py`.

Because every board compiles this same enum, a new id is live everywhere as soon
as it is added here. Prefer a new `ParamId` over a new serial command; new
commands are for fixed-layout blocks or rare bulk transfers.

## Adding a serial command (rare)

1. Add the command byte and payload length to `serial_input_protocol.h`.
2. Implement `on_frame` and add a `SerialCommandDef` row in the board's
   `Serial.ino`.
3. Keep `0x00` unused, prefer little-endian, and send with
   `serial_frame_write()` rather than ad-hoc UART writes.
4. **The payload length must be at least 1.** `serial_parser_dispatch()` treats
   `payload_len == 0` as "unregistered command", so a genuinely empty command can
   never dispatch. Give it one padding byte instead, as `'N'` does.

## Parser notes

- O(1) lookup: `serial_command_table_init()` fills `payload_len[256]` and
  `on_frame[256]`.
- The 500 µs idle timeout (`SERIAL_FRAME_TIMEOUT_US`) only runs mid-frame while
  the stream is idle, so there is no `micros()` call per byte.
- RAW: command LUT plus fixed payload. COBS: accumulate until `0x00`, decode,
  unpack, then check the length against the LUT.
- `serial_parser_drain()` snapshots `available()` once and reads up to
  `SERIAL_DRAIN_BYTE_BUDGET` (64) bytes, enough for one 1 ms panel burst.

## Changing the protocol

There is one copy, so there is nothing to sync — but every board now moves
together. A change here affects both instruments, so rebuild and reflash the
boards on a link as a set, and bump the submodule in both superprojects.
`DCO-CONTROL-PANEL/gen_midi_map.py --check` validates the host tool against
`params_def.h` and the command bytes in `protocol.py` against
`serial_input_protocol.h`.
