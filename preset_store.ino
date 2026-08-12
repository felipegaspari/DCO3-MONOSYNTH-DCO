#include "include_all.h"

// MCU-side preset store: LittleFS chunk files (4 records each), host text dumps,
// bulk restore. See preset_store.h for the record layout and the protocol summary.
//
// Everything here runs on core 0 (serial task / MIDI / boot one-shot), the same
// context that applies live parameter changes. LittleFS writes briefly stall the
// other core (flash-safe), exactly like the existing calibration FS writers.

static const char PRESET_LAST_FILE[] = "pstLast";

// Shared scratch for one record (save / load / dump) and the bulk staging area.
static uint8_t presetRecordBuf[PRESET_RECORD_SIZE];
static uint8_t presetBulkStaging[PRESET_BULK_STAGING_SIZE];

// "pb00".."pb63" — one chunk file holds PRESET_RECORDS_PER_FILE records.
static void preset_chunk_filename(uint8_t chunkIndex, char* out, size_t cap) {
  snprintf(out, cap, "pb%02u", (unsigned)chunkIndex);
}

static inline uint8_t preset_chunk_index(uint8_t slot) {
  return (uint8_t)(slot >> 2);
}

static inline uint16_t preset_slot_offset(uint8_t slot) {
  return (uint16_t)(slot & 3u) * PRESET_RECORD_SIZE;
}

static const char* preset_bulk_target_name(uint8_t target) {
  switch (target) {
    case PRESET_BULK_PRESET:        return "preset";
    case PRESET_BULK_VOICE_TABLES:  return "voiceTables";
    case PRESET_BULK_PW_CENTER:     return "PWCenter";
    case PRESET_BULK_PW_HIGH_LIMIT: return "PWHighLimit";
    case PRESET_BULK_PW_LOW_LIMIT:  return "PWLowLimit";
    case PRESET_BULK_MANUAL_OFFSET: return "ManualOffset";
    case PRESET_BULK_AMP_COMP_440:  return "AmpComp440";
    case PRESET_BULK_AMP_COMP_DUTY: return "AmpCompDutyOffset";
    default:                        return "unknown";
  }
}

// --- dump text helpers -------------------------------------------------------

static void dump_print_begin(const char* target, int slot, uint32_t size) {
  if (slot >= 0) {
    Serial.printf("[dump] begin target=%s slot=%d size=%lu\n",
                  target, slot, (unsigned long)size);
  } else {
    Serial.printf("[dump] begin target=%s size=%lu\n",
                  target, (unsigned long)size);
  }
}

static void dump_print_data_line(uint16_t offset, const uint8_t* data, uint16_t len) {
  char line[96];
  int n = snprintf(line, sizeof(line), "[dump] d %04X ", (unsigned)offset);
  for (uint16_t i = 0; i < len && n + 2 < (int)sizeof(line); ++i) {
    n += snprintf(line + n, sizeof(line) - n, "%02X", data[i]);
  }
  Serial.println(line);
}

static void dump_print_end(const char* target, uint32_t crc) {
  Serial.printf("[dump] end target=%s crc=%08lX\n", target, (unsigned long)crc);
}

static void dump_print_err(const char* target, const char* reason) {
  Serial.printf("[dump] err target=%s reason=%s\n", target, reason);
}

// Hex-dump a RAM buffer with begin/data/end framing (used for preset records).
static void dump_buffer(const char* target, int slot, const uint8_t* data, uint16_t size) {
  dump_print_begin(target, slot, size);
  for (uint16_t off = 0; off < size; off += PRESET_BULK_CHUNK_DATA) {
    uint16_t n = size - off;
    if (n > PRESET_BULK_CHUNK_DATA) n = PRESET_BULK_CHUNK_DATA;
    dump_print_data_line(off, data + off, n);
  }
  dump_print_end(target, preset_crc32(data, size));
}

// Stream a LittleFS file as a dump without loading it whole (calibration banks).
// expectedSize is the compile-time bank size (see FS.h) rather than the raw
// on-disk file size: older/larger cal files can linger on flash across board
// revisions (e.g. a NUM_OSCILLATORS change), but init_FS() only ever reads the
// leading expectedSize bytes at boot, so that's the data that is actually live.
static void dump_fs_file(const char* target, const char* filename, uint32_t expectedSize) {
  if (!LittleFS.exists(filename)) {
    dump_print_err(target, "missing");
    return;
  }
  File f = LittleFS.open(filename, "r");
  if (!f) {
    dump_print_err(target, "open");
    return;
  }
  const uint32_t fileSize = f.size();
  if (fileSize < expectedSize) {
    f.close();
    dump_print_err(target, "short");
    return;
  }
  const uint32_t size = expectedSize;  // ignore stale trailing bytes from an older, larger bank
  dump_print_begin(target, -1, size);
  uint8_t chunk[PRESET_BULK_CHUNK_DATA];
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t off = 0;
  while (off < size) {
    uint16_t n = (uint16_t)((size - off > PRESET_BULK_CHUNK_DATA)
                                ? PRESET_BULK_CHUNK_DATA
                                : (size - off));
    f.read(chunk, n);
    crc = preset_crc32_update(crc, chunk, n);
    dump_print_data_line((uint16_t)off, chunk, n);
    off += n;
  }
  f.close();
  dump_print_end(target, crc ^ 0xFFFFFFFFu);
}

// --- record build / validate / apply ------------------------------------------

// Snapshot the live patch (param shadow + block globals + presetName) into buf.
static void preset_record_build(uint8_t* buf) {
  memset(buf, 0, PRESET_RECORD_SIZE);
  buf[PRESET_OFF_MAGIC]   = PRESET_MAGIC;
  buf[PRESET_OFF_VERSION] = PRESET_VERSION;

  // Name: the 16 ASCII chars from the last 'q' frame.
  for (int i = 0; i < 16; ++i) {
    buf[PRESET_OFF_NAME + i] = presetName[i];
  }

  memcpy(buf + PRESET_OFF_BITMAP, presetParamSetBitmap, sizeof(presetParamSetBitmap));
  for (uint16_t id = 0; id < PRESET_PARAM_COUNT; ++id) {
    encode_u16_le(buf + PRESET_OFF_PARAMS + id * 2, (uint16_t)presetParamShadow[id]);
  }

  const uint16_t blocks[PRESET_BLOCK_FIELDS] = {
    ADSR_VCA_attack, ADSR_VCA_decay, ADSR_VCA_sustain, ADSR_VCA_release,
    ADSR_VCF_attack, ADSR_VCF_decay, ADSR_VCF_sustain, ADSR_VCF_release,
    ADSR1_attack,    ADSR1_decay,    ADSR1_sustain,    ADSR1_release,
    CUTOFF,          RESONANCE,      (uint16_t)ADSR2toVCF, LFO2toVCF,
  };
  for (uint8_t i = 0; i < PRESET_BLOCK_FIELDS; ++i) {
    encode_u16_le(buf + PRESET_OFF_BLOCKS + i * 2, blocks[i]);
  }

  encode_u32_le(buf + PRESET_OFF_CRC, preset_crc32(buf, PRESET_OFF_CRC));
}

static bool preset_record_validate(const uint8_t* buf) {
  if (buf[PRESET_OFF_MAGIC] != PRESET_MAGIC) return false;
  if (buf[PRESET_OFF_VERSION] != PRESET_VERSION) return false;
  return decode_u32_le(buf + PRESET_OFF_CRC) == preset_crc32(buf, PRESET_OFF_CRC);
}

// Replay a validated record into the live synth state (params via the normal
// router, blocks straight into their globals like the 'a'-'d' handlers do).
// Persistable params and all four blocks are also mirrored to the Input board.
static void preset_record_apply(const uint8_t* buf) {
  for (uint16_t id = 0; id < PRESET_PARAM_COUNT; ++id) {
    if (!(buf[PRESET_OFF_BITMAP + (id >> 3)] & (1u << (id & 7u)))) continue;
    if (!preset_param_is_persistable((uint8_t)id)) continue;
    const int16_t value = (int16_t)decode_u16_le(buf + PRESET_OFF_PARAMS + id * 2);
    update_parameters(id, value);
    serial_echo_persistable_param16((uint8_t)id, value);
  }

  const uint8_t* b = buf + PRESET_OFF_BLOCKS;
  ADSR_VCA_attack  = decode_u16_le(b + 0);
  ADSR_VCA_decay   = decode_u16_le(b + 2);
  ADSR_VCA_sustain = decode_u16_le(b + 4);
  ADSR_VCA_release = decode_u16_le(b + 6);
  ADSR_VCF_attack  = decode_u16_le(b + 8);
  ADSR_VCF_decay   = decode_u16_le(b + 10);
  ADSR_VCF_sustain = decode_u16_le(b + 12);
  ADSR_VCF_release = decode_u16_le(b + 14);
  ADSR1_attack     = decode_u16_le(b + 16);
  ADSR1_decay      = decode_u16_le(b + 18);
  ADSR1_sustain    = decode_u16_le(b + 20);
  ADSR1_release    = decode_u16_le(b + 22);
  CUTOFF           = decode_u16_le(b + 24);
  RESONANCE        = decode_u16_le(b + 26);
  ADSR2toVCF       = (int16_t)decode_u16_le(b + 28);
  LFO2toVCF        = decode_u16_le(b + 30);
  mark_adsr_params_dirty(ADSR_DIRTY_VCA_ALL | ADSR_DIRTY_VCF_ALL | ADSR_DIRTY_DCO_ALL);
  cv_bake_adsr2_to_vcf_scale();
  cv_bake_lfo2_to_vcf_scale();

  serial_send_adsr_vca_block_to_mb();
  serial_send_adsr_vcf_block_to_mb();
  serial_send_adsr_dco_block_to_mb();
  serial_send_filter_block_to_mb();

  for (int i = 0; i < 16; ++i) {
    presetName[i] = buf[PRESET_OFF_NAME + i];
  }
}

// --- last-slot persistence (boot recall) --------------------------------------

static void preset_store_write_last(uint8_t slot) {
  File f = LittleFS.open(PRESET_LAST_FILE, "w");
  if (!f) return;
  f.write(&slot, 1);
  f.close();
}

// --- chunked record I/O -------------------------------------------------------

// Write one validated 598-byte record into its chunk file (create full-size if
// missing / wrong size, otherwise in-place "r+" seek). Returns false on I/O
// failure; prints a [preset]/[bulk] err line via the caller's context.
static bool preset_chunk_write_record(uint8_t slot, const uint8_t* record,
                                      const char* errTag) {
  const uint8_t chunk = preset_chunk_index(slot);
  const uint16_t offset = preset_slot_offset(slot);
  char fname[8];
  preset_chunk_filename(chunk, fname, sizeof(fname));

  const bool exists = LittleFS.exists(fname);
  bool needCreate = !exists;
  if (exists) {
    File check = LittleFS.open(fname, "r");
    if (!check || check.size() != PRESET_CHUNK_SIZE) {
      needCreate = true;
    }
    if (check) check.close();
  }

  if (needCreate) {
    // Create a full-size chunk: write four records in one pass. The target
    // slot gets `record`; the other three get zeros (empty = invalid magic).
    // File::seek refuses past-EOF, so the file must be born at full size.
    File f = LittleFS.open(fname, "w");
    if (!f) {
      FSInfo info;
      if (LittleFS.info(info) && info.usedBytes + PRESET_CHUNK_SIZE > info.totalBytes) {
        Serial.printf("[%s] err slot=%u reason=nospace\n", errTag, (unsigned)slot);
      } else {
        Serial.printf("[%s] err slot=%u reason=open\n", errTag, (unsigned)slot);
      }
      return false;
    }
    static uint8_t emptyRecord[PRESET_RECORD_SIZE];  // zeroed BSS; empty = no magic
    for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
      const uint8_t* src =
          ((slot & 3u) == i) ? record : emptyRecord;
      if (f.write(src, PRESET_RECORD_SIZE) != PRESET_RECORD_SIZE) {
        f.close();
        Serial.printf("[%s] err slot=%u reason=write\n", errTag, (unsigned)slot);
        return false;
      }
    }
    f.close();
    return true;
  }

  File f = LittleFS.open(fname, "r+");
  if (!f) {
    Serial.printf("[%s] err slot=%u reason=open\n", errTag, (unsigned)slot);
    return false;
  }
  if (!f.seek(offset)) {
    f.close();
    Serial.printf("[%s] err slot=%u reason=seek\n", errTag, (unsigned)slot);
    return false;
  }
  if (f.write(record, PRESET_RECORD_SIZE) != PRESET_RECORD_SIZE) {
    f.close();
    Serial.printf("[%s] err slot=%u reason=write\n", errTag, (unsigned)slot);
    return false;
  }
  f.close();
  return true;
}

// Read one 598-byte record from its chunk. Returns false if the chunk is
// missing, short, or the seek/read fails (caller treats that as empty/corrupt).
static bool preset_chunk_read_record(uint8_t slot, uint8_t* out) {
  const uint8_t chunk = preset_chunk_index(slot);
  const uint16_t offset = preset_slot_offset(slot);
  char fname[8];
  preset_chunk_filename(chunk, fname, sizeof(fname));
  if (!LittleFS.exists(fname)) return false;
  File f = LittleFS.open(fname, "r");
  if (!f || f.size() != PRESET_CHUNK_SIZE) {
    if (f) f.close();
    return false;
  }
  if (!f.seek(offset) || f.read(out, PRESET_RECORD_SIZE) != (int)PRESET_RECORD_SIZE) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

// --- public API ----------------------------------------------------------------

// PARAM_PRESET_SAVE: snapshot live state into slot N and mark it as boot-recall.
void preset_store_save(uint8_t slot) {
  preset_record_build(presetRecordBuf);
  if (!preset_chunk_write_record(slot, presetRecordBuf, "preset")) return;
  preset_store_write_last(slot);
  Serial.printf("[preset] saved slot=%u name=\"%.16s\"\n",
                (unsigned)slot, (const char*)presetName);
}

// PARAM_PRESET_LOAD / MIDI program change / boot recall.
bool preset_store_load(uint8_t slot) {
  if (!preset_chunk_read_record(slot, presetRecordBuf)) {
    Serial.printf("[preset] err slot=%u reason=empty\n", (unsigned)slot);
    return false;
  }

  if (!preset_record_validate(presetRecordBuf)) {
    Serial.printf("[preset] err slot=%u reason=corrupt\n", (unsigned)slot);
    return false;
  }

  preset_record_apply(presetRecordBuf);
  preset_store_write_last(slot);

  Serial.printf("[preset] loaded slot=%u name=\"%.16s\"\n",
                (unsigned)slot, (const char*)presetName);
  serial_send_preset_loaded_to_mb(slot);
  return true;
}

// 'N' handler: push the whole 256-slot directory to the Input board as 'O'
// frames. Opens each chunk once and seeks for the 4 name heads — 64 opens for
// 256 slots. Blank (all-zero) name = unused slot.
void preset_store_send_directory_to_mb() {
  if (Serial2.availableForWrite() < 1) return;  // no Input board on this link

  char fname[8];
  uint8_t entry[1 + PRESET_NAME_LEN];
  uint8_t head[PRESET_OFF_BITMAP];

  for (uint8_t chunk = 0; chunk < PRESET_CHUNK_COUNT; ++chunk) {
    preset_chunk_filename(chunk, fname, sizeof(fname));
    File f;
    bool openOk = false;
    if (LittleFS.exists(fname)) {
      f = LittleFS.open(fname, "r");
      openOk = f && f.size() == PRESET_CHUNK_SIZE;
      if (f && !openOk) f.close();
    }

    for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
      const uint8_t slot = (uint8_t)((chunk << 2) | i);
      memset(entry, 0, sizeof(entry));
      entry[0] = slot;

      if (openOk) {
        if (f.seek((uint32_t)i * PRESET_RECORD_SIZE) &&
            f.read(head, sizeof(head)) == (int)sizeof(head) &&
            head[PRESET_OFF_MAGIC] == PRESET_MAGIC) {
          memcpy(entry + 1, head + PRESET_OFF_NAME, PRESET_NAME_LEN);
        }
      }

      serial_frame_write(Serial2, INPUT_CMD_PRESET_DIR_ENTRY, entry, sizeof(entry));
    }
    if (openOk) f.close();
  }
}

// PARAM_PRESET_DUMP: -1 = directory listing ([pdir] lines), 0..255 = slot record.
void preset_store_dump(int16_t sel) {
  if (sel < 0) {
    Serial.println("[pdir] begin");
    uint16_t count = 0;
    char fname[8];
    uint8_t head[PRESET_OFF_BITMAP];

    for (uint8_t chunk = 0; chunk < PRESET_CHUNK_COUNT; ++chunk) {
      preset_chunk_filename(chunk, fname, sizeof(fname));
      if (!LittleFS.exists(fname)) continue;
      File f = LittleFS.open(fname, "r");
      if (!f || f.size() != PRESET_CHUNK_SIZE) {
        if (f) f.close();
        continue;
      }
      for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
        if (!f.seek((uint32_t)i * PRESET_RECORD_SIZE)) continue;
        if (f.read(head, sizeof(head)) != (int)sizeof(head)) continue;
        if (head[PRESET_OFF_MAGIC] != PRESET_MAGIC) continue;
        char name[PRESET_NAME_LEN + 1];
        memcpy(name, head + PRESET_OFF_NAME, PRESET_NAME_LEN);
        name[PRESET_NAME_LEN] = 0;
        const uint8_t slot = (uint8_t)((chunk << 2) | i);
        Serial.printf("[pdir] slot=%03u name=\"%s\"\n", (unsigned)slot, name);
        ++count;
      }
      f.close();
    }
    Serial.printf("[pdir] end count=%u\n", (unsigned)count);
    return;
  }

  if (sel >= (int16_t)PRESET_NUM_SLOTS) return;
  if (!preset_chunk_read_record((uint8_t)sel, presetRecordBuf)) {
    dump_print_err("preset", "empty");
    return;
  }
  dump_buffer("preset", sel, presetRecordBuf, PRESET_RECORD_SIZE);
}

// PARAM_CAL_DUMP: dump calibration LittleFS files as hex (0 / -1 = all six).
void preset_store_cal_dump(int16_t sel) {
  const bool all = (sel <= CAL_DUMP_ALL);
  if (all || sel == CAL_DUMP_VOICE_TABLES)  dump_fs_file("voiceTables", "voiceTables", FSBankSize);
  if (all || sel == CAL_DUMP_PW_CENTER)     dump_fs_file("PWCenter", "PWCenter", FSPWBankSize);
  if (all || sel == CAL_DUMP_PW_HIGH_LIMIT) dump_fs_file("PWHighLimit", "PWHighLimit", FSPWBankSize);
  if (all || sel == CAL_DUMP_PW_LOW_LIMIT)  dump_fs_file("PWLowLimit", "PWLowLimit", FSPWBankSize);
  if (all || sel == CAL_DUMP_MANUAL_OFFSET) dump_fs_file("ManualOffset", "ManualOffset", FSManualOffsetBankSize);
  if (all || sel == CAL_DUMP_AMP_COMP_440)  dump_fs_file("AmpComp440", "AmpComp440", FSAmpComp440BankSize);
  if (all || sel == CAL_DUMP_AMP_COMP_DUTY) dump_fs_file("AmpCompDutyOffset", "AmpCompDutyOffset", FSAmpCompDutyOffsetBankSize);
}

// --- bulk restore ('B' chunks + 'C' commit) ------------------------------------

// 'B': [target:u8][slot:u8][offset:u16 LE][32 data] → stage. Target/slot ride
// along for symmetry only; the commit frame is authoritative (a mixed-up
// transfer fails its CRC there anyway).
void preset_bulk_chunk(const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_BULK_CHUNK) return;
  const uint16_t offset = decode_u16_le(payload + 2);
  if ((uint32_t)offset + PRESET_BULK_CHUNK_DATA > PRESET_BULK_STAGING_SIZE) return;
  memcpy(presetBulkStaging + offset, payload + 4, PRESET_BULK_CHUNK_DATA);
}

// 'C': [target:u8][slot:u8][size:u16 LE][crc32 LE] → verify staging and persist.
void preset_bulk_commit(const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_BULK_COMMIT) return;
  const uint8_t  target = payload[0];
  const uint8_t  slot   = payload[1];
  const uint16_t size   = decode_u16_le(payload + 2);
  const uint32_t crc    = decode_u32_le(payload + 4);
  const char* tname = preset_bulk_target_name(target);

  if (size == 0 || size > PRESET_BULK_STAGING_SIZE) {
    Serial.printf("[bulk] err target=%s reason=size\n", tname);
    return;
  }
  if (preset_crc32(presetBulkStaging, size) != crc) {
    Serial.printf("[bulk] err target=%s reason=crc\n", tname);
    return;
  }

  uint16_t want = 0;
  const char* calFile = nullptr;
  switch (target) {
    case PRESET_BULK_PRESET:        want = PRESET_RECORD_SIZE; break;
    case PRESET_BULK_VOICE_TABLES:  want = FSBankSize;             calFile = "voiceTables"; break;
    case PRESET_BULK_PW_CENTER:     want = FSPWBankSize;           calFile = "PWCenter"; break;
    case PRESET_BULK_PW_HIGH_LIMIT: want = FSPWBankSize;           calFile = "PWHighLimit"; break;
    case PRESET_BULK_PW_LOW_LIMIT:  want = FSPWBankSize;           calFile = "PWLowLimit"; break;
    case PRESET_BULK_MANUAL_OFFSET: want = FSManualOffsetBankSize; calFile = "ManualOffset"; break;
    case PRESET_BULK_AMP_COMP_440:  want = FSAmpComp440BankSize;   calFile = "AmpComp440"; break;
    case PRESET_BULK_AMP_COMP_DUTY: want = FSAmpCompDutyOffsetBankSize; calFile = "AmpCompDutyOffset"; break;
    default:
      Serial.printf("[bulk] err target=%s reason=target\n", tname);
      return;
  }
  if (size != want) {
    Serial.printf("[bulk] err target=%s reason=size\n", tname);
    return;
  }

  if (target == PRESET_BULK_PRESET) {
    if (!preset_record_validate(presetBulkStaging)) {
      Serial.printf("[bulk] err target=%s reason=record\n", tname);
      return;
    }
    if (!preset_chunk_write_record(slot, presetBulkStaging, "bulk")) return;
    Serial.printf("[bulk] ok target=%s slot=%u\n", tname, (unsigned)slot);
    return;
  }

  // Calibration targets: rewrite the file, then reload the runtime tables the
  // same way debug command 30 (seed fakes) does.
  write_fs_bank(calFile, presetBulkStaging, size);
  init_FS();
  if (target == PRESET_BULK_VOICE_TABLES) {
    precompute_amp_comp_for_engine();
  }
  Serial.printf("[bulk] ok target=%s\n", tname);
}

// --- boot recall -----------------------------------------------------------------

// Recall the last saved/loaded slot ~1.5 s after boot (both cores up, FS mounted).
// No pstLast file (fresh board / never used) = keep firmware defaults.
// Gate on a successful 1-byte read (not a 0xFF sentinel — slot 255 is valid).
void preset_store_boot_recall() {
  if (calibrationFlag) return;
  if (!LittleFS.exists(PRESET_LAST_FILE)) return;
  File f = LittleFS.open(PRESET_LAST_FILE, "r");
  if (!f) return;
  uint8_t slot = 0;
  const bool ok = (f.read(&slot, 1) == 1);
  f.close();
  if (!ok) return;
  preset_store_load(slot);
}
