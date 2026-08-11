# MCU Preset Store & Calibration Dump

LittleFS-backed **256-slot presets** on the DCO board, plus host dump/restore for
presets and the five calibration tables. Host UI:
[`tools/dco_control`](../tools/dco_control/README.md). Serial how-to:
[`README_serial_and_params.md`](README_serial_and_params.md).

**Source of truth:** [`preset_store.h`](../preset_store.h) / [`preset_store.ino`](../preset_store.ino).

---

## 1. What is stored

A **preset** is a snapshot of:

| Domain | How it is captured |
|--------|--------------------|
| Persistable `'p'` ParamIds | Shadowed in `presetParamShadow[]` + set-bitmap by `update_parameters()` → `preset_shadow_capture()` |
| EnvVCA / EnvVCF / EnvDCO times (`'a'`/`'b'`/`'c'`) | Read from ADSR globals at save time |
| Filter block (`'d'`) | `CUTOFF`, `RESONANCE`, `ADSR2toVCF`, `LFO2toVCF` |
| Name | Last `'q'` frame (`presetName[16]`), copied verbatim into the record |

**Not in a preset:** calibration tables, autotune / store-cal pulses, `PARAM_DEBUG_COMMAND`,
PIO pulse length, Character diagnostic jitters, or other DCO-local bench ids.

`preset_param_is_persistable()` (in `preset_store.h`) is the shared filter for shadow
capture and for `serial_echo_persistable_param16()` (USB/MIDI → Input mirror).

---

## 2. LittleFS layout (chunked)

Arduino-Pico LittleFS uses **4096-byte blocks**. One file per 598-byte record would
waste most of each block and would not fit 256 slots even in a 512 KB partition.
Instead, slots are packed **4 records per chunk file**:

| File | Size | Role |
|------|-----:|------|
| `pb00` … `pb63` | 2392 B each (= 4 × 598) | Chunk: slots `chunk*4 + 0..3` |
| `pstLast` | 1 B | Slot recalled at boot / last save-load |
| `voiceTables` | 528 B | Amp-comp bank (`FSBankSize`, 3 osc × 22 pairs) |
| `PWCenter` / `PWHighLimit` / `PWLowLimit` | 6 B each | `FSPWBankSize` |
| `ManualOffset` | 3 B | `FSManualOffsetBankSize` |

**Addressing:** `chunk = slot >> 2`, `offset = (slot & 3) * 598`.

**Why 4/file:** 4 × 598 = 2392 B fits in one 4096-byte block. An in-place save
(`"r+"` + `seek` + `write`) stays inside that one block, so LittleFS does not
copy a long file tail (mid-file writes otherwise rewrite the remainder of the CTZ
skip-list). Empty slots are all-zero records (fail magic validation = “unused”).

**Partition:** flash with `flash=4194304_524288` (4 MB flash, **512 KB** LittleFS =
128 blocks). Budget ~64 chunk data blocks + metadata + `voiceTables` (~1 block);
small cal files/`pstLast` stay under the 256-byte inline limit. See
[`BUILD_FLAGS.md`](BUILD_FLAGS.md).

**Create path:** `File::seek()` refuses past-EOF, so a missing/wrong-size chunk is
created with `"w"` in one pass writing all four records (target real, others zeroed).
Subsequent saves use `"r+"`. Write return values are checked; a full FS reports
`reason=nospace` / `reason=write`.

**Migration:** Changing the FS size moves `_FS_start`/`_FS_end` and reformats the
filesystem. Back up calibration + presets with `dco_control` before flashing, then
restore after. No on-flash migration from the old `pstNNN` layout is required.

---

## 3. Preset record (598 bytes, LE)

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 1 | Magic `0xA5` |
| 1 | 1 | Version `1` |
| 2 | 16 | Name (ASCII, zero-padded) |
| 18 | 32 | Param set-bitmap (bit *id* set ⇒ value at that ParamId was captured) |
| 50 | 512 | 256 × `int16` ParamId values |
| 562 | 32 | 16 × `uint16` block fields (order below) |
| 594 | 4 | CRC32 (IEEE / zlib) over bytes `[0 .. 594)` |

**Block field order** (wire / exp domain for A/D/R):

0–3 EnvVCA A D S R · 4–7 EnvVCF · 8–11 EnvDCO (`ADSR1_*`) · 12–15 filter
`CUTOFF`, `RESONANCE`, `ADSR2toVCF`, `LFO2toVCF`.

On load, only bitmap-set persistable params are replayed through `update_parameters()`;
blocks write globals + dirty flags and are mirrored to Input via
`serial_send_adsr_*_block_to_mb()` / `serial_send_filter_block_to_mb()`.

---

## 4. Host ↔ board protocol

### Commands (host → DCO, slim inner frames)

| Path | Meaning |
|------|---------|
| `'p'` `PARAM_PRESET_SAVE` (170) = slot | Snapshot live state → chunk file, update `pstLast` |
| `'p'` `PARAM_PRESET_LOAD` (171) = slot | Recall slot (also MIDI PC + Bank Select; see §5) |
| `'p'` `PARAM_PRESET_DUMP` (172) = −1 | Directory listing (`[pdir]` lines) |
| `'p'` `PARAM_PRESET_DUMP` (172) = 0..255 | Hex dump of that slot record |
| `'p'` `PARAM_CAL_DUMP` (173) | 0/−1 = all five cal files; 1..5 = one (`CAL_DUMP_*`) |
| `'q'` | 16-char name before SAVE (board stores `presetName[16]`) |
| `'B'` | Bulk chunk: `[target][slot][offset:u16 LE][32 data]` → staging RAM |
| `'C'` | Bulk commit: `[target][slot][size:u16 LE][crc32 LE]` → verify + LittleFS write |

`SERIAL_INNER_MAX_PAYLOAD` is **36** on the DCO (`Serial.h`) so `'B'` fits.
Bulk preset commits still write one **598-byte record** (host protocol unchanged);
the board maps `slot` into the correct chunk offset.

### Input-only directory sync (`'N'`/`'O'`/`'L'`)

The Input board has no LittleFS preset storage of its own — the DCO's 256 slots are
the single source of truth system-wide. Input keeps a RAM-only `presetDir[256][16]`
name cache, refreshed by asking the DCO for the whole directory:

| Path | Direction | Payload | Meaning |
|------|-----------|---------|---------|
| `'N'` | Input → DCO | 1 unused/padding byte | "Send me the whole directory" |
| `'O'` | DCO → Input | `[slot:u8][name:16]` | One directory entry (256 sent per `'N'`, blank name = unused slot) |
| `'L'` | DCO → Input | `[slot:u8]` | "I just finished loading this slot" (boot recall, MIDI PC, USB/`dco_control`, or Input itself) |

`'N'`'s payload is 1 byte, not 0, even though the byte itself carries no
information: `serial_parser_dispatch()` / `serial_parser_process_byte()` treat
`payload_len == 0` as "unregistered command" in both RAW and COBS framing, so a
true zero-length frame can never dispatch. `preset_store_send_directory_to_mb()`
opens each of the 64 chunk files once and emits four `'O'` frames per chunk
(blocking `Serial2` writes — once per Input boot / browse-mode-enter, never per
encoder tick). `serial_send_preset_loaded_to_mb()` fires `'L'` once at the end of
every successful `preset_store_load()`. `dco_control` (USB host) uses `'p'`
`PARAM_PRESET_DUMP` / `[pdir]` text instead of `'N'`/`'O'`/`'L'`.

**Bulk targets** (`PresetBulkTarget`): `0` preset record, `1` voiceTables, `2` PWCenter,
`3` PWHighLimit, `4` PWLowLimit, `5` ManualOffset. Calibration commits call
`write_fs_bank()` then `init_FS()` (and amp-comp precompute for voiceTables).

**Calibration dump sizes are clamped, not raw file sizes.** `dump_fs_file()` always
sends the compile-time bank size (`FSBankSize` / `FSPWBankSize` /
`FSManualOffsetBankSize`, per target), the same leading bytes `init_FS()` reads at
boot — never the LittleFS file's actual on-disk size. A cal file can be larger than
that on flash (e.g. a leftover from a firmware build with a different
`NUM_OSCILLATORS`); the extra trailing bytes are unused and are not sent. If the
on-disk file is *smaller* than expected, the dump aborts with `reason=short`.
`fileformats.decode_cal_table()` on the host truncates an over-length payload with
a warning and raises on under-length.

### Answers (DCO → host, plain USB CDC text)

```
[pdir] begin
[pdir] slot=005 name="BassPluck"
[pdir] end count=1

[dump] begin target=preset slot=5 size=598
[dump] d 0000 A501...
[dump] end target=preset crc=XXXXXXXX

[dump] begin target=voiceTables size=528
...
[dump] err target=ManualOffset reason=missing|open|short

[preset] saved slot=3 name="MyName          "
[preset] loaded slot=3 name="MyName          "
[preset] err slot=4 reason=empty|corrupt|open|write|seek|nospace

[bulk] ok target=preset slot=9
[bulk] ok target=PWCenter
[bulk] err target=preset reason=crc|size|record|open|write|seek|nospace|target
```

CRC on dump/commit is zlib-compatible IEEE CRC32.

---

## 5. Recall paths

| Trigger | Code path |
|---------|-----------|
| MIDI Bank Select + Program Change | CC 0 or 32 latches `midiPresetBank` (0/1); PC → `preset_store_load(bank*128 + program)` |
| `'p'` 171 | `apply_param_preset_load` → `preset_store_load` |
| Boot | `preset_store_boot_task()` from `loop()` after ~1.5 s, once, if `pstLast` exists and `!calibrationFlag` |

MIDI Program Change alone addresses slots 0..127 (bank 0). Nonzero CC 0 or CC 32
selects bank 1 (slots 128..255). Either bank CC is accepted for controller
compatibility (two banks only).

No `pstLast` → firmware defaults stay. Boot recall requires a successful 1-byte
read of `pstLast` (slot 255 is valid; there is no `0xFF` “empty” sentinel).

Everything runs on **Core 0** (serial / MIDI / boot one-shot). Flash writes briefly stall
the other core, same as existing calibration FS writers.

---

## 6. Host file formats (`tools/dco_control/fileformats.py`)

Tagged JSON so patch / bank / cal files cannot be mixed up:

| `"format"` | Contents |
|------------|----------|
| `dco3-patch` | One slot: `name`, `params`, `blocks` (linear ADSR fader domain) |
| `dco3-bank` | Full 256-slot bank (`version`, `current`, `slots`) |
| `dco3-cal` | Decoded tables: `amp_comp`, `pw_center`, `pw_high_limit`, `pw_low_limit`, `manual_offset` |

Host ↔ MCU record codec converts ADSR A/D/R between UI linear 0..4095 and the exp
wire domain 0..25000 (`lin_to_exp` / `exp_to_lin`).

---

## 7. Related files

| File | Role |
|------|------|
| `preset_store.h` / `.ino` | Record layout, CRC, chunked save/load/dump, bulk, boot recall |
| `FS.h` / `.ino` | Cal banks; `write_fs_bank()` shared with bulk restore |
| `params_def.h` | ParamIds 170–173 |
| `params.ino` | `apply_param_preset_*` / `apply_param_cal_dump`; shadow in `update_parameters` |
| `Serial.h` / `.ino` | `'B'`/`'C'` handlers; block-echo helpers after load; `'N'` handler; `serial_send_preset_loaded_to_mb()` |
| `midi.ino` / `globals.h` | Bank Select CC 0/32 + Program Change → load |
| `INPUT-CONTROLLER/presetStorage.ino` | Input's RAM-only `presetDir[256]` cache; `'N'`/`'O'`/`'L'`; `preset_save_to_board()` / `preset_load_from_board()` |
| `tools/dco_control/mcu_link.py` | Queued dump/bulk ops over CDC text |
| `tools/dco_control/fileformats.py` | JSON + binary codecs |
