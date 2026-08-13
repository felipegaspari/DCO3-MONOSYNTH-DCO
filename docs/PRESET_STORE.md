# Preset store — DCO3-MONOSYNTH specifics

The record format, the chunked LittleFS layout, the host protocol and the recall
paths are a shared contract, documented once for both projects:

- [`../_shared/docs/PRESET_STORAGE.md`](../_shared/docs/PRESET_STORAGE.md) — the
  598-byte record, chunk addressing, `'p'` 170-173, `'B'`/`'C'`, `'N'`/`'O'`/`'L'`,
  host answer lines, recall paths, host JSON formats.
- [`../_shared/docs/FILESYSTEM.md`](../_shared/docs/FILESYSTEM.md) — the 512 KB
  partition, the full file inventory and the space budget.
- [`../_shared/docs/CALIBRATION_STORAGE.md`](../_shared/docs/CALIBRATION_STORAGE.md)
  — the seven calibration banks and their sizes on this board.

**This page is only what is different here.** Source of truth:
[`preset_store.h`](../preset_store.h) / [`preset_store.ino`](../preset_store.ino).
Serial how-to: [`README_serial_and_params.md`](README_serial_and_params.md). Host UI:
[`DCO-CONTROL-PANEL`](../../DCO-CONTROL-PANEL/README.md).

---

## Staging buffer

`PRESET_BULK_STAGING_SIZE` is **640**. It has to cover the largest bulk target, and
on this board that is the 598-byte preset record itself — the `voiceTables` bank is
only 528 B with three oscillators. (On DCO4-REBORN it is the other way round: 1408 B
of `voiceTables` sets a 1440-byte buffer.)

## Persistable parameters: the sub-oscillator range

`preset_param_is_persistable()` here accepts one range DCO4 does not — **90-99**,
both sub-oscillators' divide / master / phase / width plus the logic combiner
(`PARAM_SUB1_DIVIDE` … `PARAM_SUB_LOGIC_OP`). Id 98 is reserved and simply never
arrives; 100 is reserved too and is left outside the range.

Everything else in that function is common to both boards.

## Topology: Input is the direct peer

The Input board sits directly on Serial2 — there is no Mainboard in this
instrument — so the preset frames are point to point:

```
Input  <--- Serial2 --->  DCO  <--- USB CDC --->  DCO-CONTROL-PANEL
```

**One command table serves both links.** `inputSerialLut` is used by
`serial_panel_task()` (Serial2) and by `serial_usb_task()` (USB CDC), so every
preset command — including `'B'`, `'C'` and `'N'` — is accepted from either. On
DCO4 those are split across two LUTs and `'B'`/`'C'` are USB-only.

**Block mirroring on load.** All four blocks go to Input:
`serial_send_adsr_*_block_to_mb()` and `serial_send_filter_block_to_mb()`. Nothing
stays DCO-local, because Input owns the whole panel display here.

**The directory push blasts.** `preset_store_send_directory_to_mb()` sends all 256
`'O'` frames in one call, busy-waiting on `serial2_dma_tx_ready()` between frames.
That is fine on a direct link with Input as the only consumer; DCO4 has to pace the
same push from `loop()` because its Mainboard cannot relay the burst.

Host answers for calibration dumps carry this board's bank sizes — `voiceTables` is
528 B, the PW banks 6 B each. The full table is in
[`../_shared/docs/CALIBRATION_STORAGE.md`](../_shared/docs/CALIBRATION_STORAGE.md).

## Host file format tags

`dco3-patch`, `dco3-bank`, `dco3-cal`. Patch and bank files interchange with
DCO4-REBORN; cal files do not, because the table sizes differ.

## Migration

No on-flash migration from the old `pstNNN` layout (one file per slot) is required —
those files are simply ignored. Presets saved before the chunked layout are not
readable and must be re-pulled from a host backup.

---

## Related files

| File | Role |
|---|---|
| `preset_store.h` / `.ino` | Record layout, CRC, chunked save/load/dump, bulk, boot recall, `'O'` directory push |
| `FS.h` / `.ino` | Shims → `_shared/FS.h` + `_shared/FS_impl.h`: cal banks, `write_fs_bank()` shared with bulk restore |
| `params_def.h` | ParamIds 170-173 |
| `params.ino` | `apply_param_preset_*` / `apply_param_cal_dump`; shadow capture in `update_parameters` |
| `Serial.h` / `.ino` | `'B'`/`'C'` handlers, block-echo helpers after load, `'N'` handler, `serial_send_preset_loaded_to_mb()` |
| `midi.ino` / `globals.h` | Bank Select CC 0/32 + Program Change → load |
| [`../../INPUT-CONTROLLER/presetStorage.ino`](../../INPUT-CONTROLLER/presetStorage.ino) | Input's RAM-only `presetDir[256]` cache; `'N'`/`'O'`/`'L'` client side |
| [`../../DCO-CONTROL-PANEL/mcu_link.py`](../../DCO-CONTROL-PANEL/mcu_link.py) | Queued dump / bulk ops over CDC text |
| [`../../DCO-CONTROL-PANEL/fileformats.py`](../../DCO-CONTROL-PANEL/fileformats.py) | JSON + binary codecs |
