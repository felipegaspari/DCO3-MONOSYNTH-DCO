## Serial & Parameter Protocol – DCO (DCO3-MONOSYNTH)

The wire format, the command table, the parser and the `ParamId` enum are shared
by every board and documented once in
[`DCO-PROTOCOL/README.md`](../../DCO-PROTOCOL/README.md). The headers come from
that library through `_build_libs/DCO-PROTOCOL`; there is no copy in this sketch
folder any more.

This page covers only what is specific to the DCO.

---

## Links

The only peer UART is Serial2 ↔ Input. USB CDC (`ENABLE_USB_CONTROL`) speaks the
same inner frames for [`DCO-CONTROL-PANEL`](../../DCO-CONTROL-PANEL/README.md).

`serial_usb_task()` uses a second `SerialParserContext` with the same LUT as
Serial2. Only host → DCO is framed; DCO → host is plain debug text, including the
structured `[dump]` / `[pdir]` / `[preset]` / `[bulk]` lines the preset store
emits. Panel Serial2 and USB CDC drain on Core 0 `timer1msFlag` (~1 ms), while
USB/DIN MIDI runs every `loop()`. The CDC drain is skipped when the host has not
opened `Serial`.

`SERIAL_INNER_MAX_PAYLOAD` is **36** here (set in `Serial.h`), sized for the `'B'`
bulk chunk; Input and Screen use 17. Screen-only commands (`'w'`, `'y'`, `'s'`
and the 17-byte `'q'` scroll) never reach this board. Uppercase `'B'`/`'C'` are
the bulk restore pair and are distinct from lowercase `'c'`, the EnvDCO ADSR
block. The former `'e'`/`'f'` commands are now `'p'` ids 222 and 210.

## Board-specific frame handling

- `'a'`/`'b'`/`'c'`/`'d'` write the ADSR and filter block globals directly and
  set dirty flags — they do **not** go through `update_parameters()`. A block
  that arrived over USB is mirrored back out to Input
  (`serial_forward_input_block_to_mb`, gated on `g_param_ingress`) so the panel
  faders and pots and the Screen follow a host edit; a panel-origin block is not,
  or it would echo to its own sender.
- `'p'` is both panel ingress and the DCO→Input persistable mirror (the mirror is
  sent for USB/MIDI edits only, never for panel ingress).
- `'q'` also stages the name used by `PARAM_PRESET_SAVE`.

## MIDI CC

`midi_cc_apply()` writes the ADSR/filter block globals directly (`CC_LOCAL_*`).
Everything else, including `PARAM_PW_VALUE` and `PARAM_ADSR1_TO_VCA`, goes
through `update_parameters()`. Persistable ParamId CCs also call
`serial_echo_persistable_param16()` so a saved preset matches what you hear. A
block CC has no ParamId to echo, so `midi_cc_apply()` instead sends the whole
block it touched (`serial_send_adsr_*_block_to_mb` / `serial_send_filter_block_to_mb`)
the way a preset recall does. MIDI Program Change recalls a preset slot
(`midiPresetBank * 128 + program`; CC 0/32 select the bank).

## Preset / calibration ParamIds (DCO-local)

| Id | Name | Value |
|----|------|-------|
| 170 | `PARAM_PRESET_SAVE` | slot 0..255 — save live state to LittleFS |
| 171 | `PARAM_PRESET_LOAD` | slot 0..255 — recall (same as MIDI PC + bank) |
| 172 | `PARAM_PRESET_DUMP` | −1 = `[pdir]` listing; 0..255 = slot record hex dump |
| 173 | `PARAM_CAL_DUMP` | 0/−1 = all cal tables; 1..7 = one table |

These are deliberately off the MIDI CC map (filesystem access and long dumps).
See [`PRESET_STORE.md`](PRESET_STORE.md) and the host tool at
[`DCO-CONTROL-PANEL`](../../DCO-CONTROL-PANEL/README.md).

`PARAM_CALIBRATION_FLAG` (**150**) is shared with Input and Screen, but the DCO
reads more out of its value than their menus send: **1/2/3** run the amp-comp,
PW or full stage at normal precision, **5/6/7** run the same three at fine
precision (much more careful measurements, and the amp stage re-measures the
stored table instead of rebuilding it — see
[`CALIBRATION_PROCEDURE.md`](CALIBRATION_PROCEDURE.md)), and **0** cancels a
running pass. The Input and Screen menus always send 1/2/3; fine mode is
reachable from the panel and over USB.

## Adding a parameter here

The generic steps are in the [shared
guide](../../DCO-PROTOCOL/README.md#adding-a-parameter). On this board, after
adding the id you implement `apply_param_*`, add a row to `paramTable[]` in
`params.ino`, and rely on `init_param_router()` already being called from
`setup()`.
