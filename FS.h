#ifndef __FS_H__
#define __FS_H__

static constexpr uint16_t FSVoiceDataSize = 22 * 2 * 4;

static constexpr uint16_t FSPWDataSize = 2;

static constexpr uint16_t FSBankSize = FSVoiceDataSize * NUM_OSCILLATORS;
static constexpr uint16_t FSPWBankSize = FSPWDataSize * NUM_OSCILLATORS;

// One signed byte per oscillator to store manualCalibrationOffset[].
static constexpr uint16_t FSManualOffsetDataSize = 1;
static constexpr uint16_t FSManualOffsetBankSize = FSManualOffsetDataSize * NUM_OSCILLATORS;

static constexpr uint16_t chanLevelVoiceDataSize = FSVoiceDataSize / 4;

// Calibration buffers (FS-local)
uint8_t voiceTablesCalibrationBuffer[FSVoiceDataSize];
uint8_t PWCenterCalibrationBuffer[FSPWDataSize];
uint8_t PWHighLimitCalibrationBuffer[FSPWDataSize];
uint8_t PWLowLimitCalibrationBuffer[FSPWDataSize];
uint8_t ManualOffsetCalibrationBuffer[FSManualOffsetDataSize];

uint8_t voiceTablesBankBuffer[FSBankSize];
uint8_t PWCenterBankBuffer[FSPWBankSize];
uint8_t PWHighLimitBankBuffer[FSPWBankSize];
uint8_t PWLowLimitBankBuffer[FSPWBankSize];
uint8_t ManualOffsetBankBuffer[FSManualOffsetBankSize];

File fileVoiceTablesFS;
File filePWCenterFS;
File filePWHighLimitFS;
File filePWLowLimitFS;
File fileManualOffsetFS;

// Build one oscillator's 22 [freq_x100, RANGE PWM] pairs into out[chanLevelVoiceDataSize].
void generate_fake_calibration_data(uint8_t osc, uint32_t* out);
// Seed LittleFS with fake amp-comp + PW defaults.
// force=false: only if voiceTables file is missing. force=true: overwrite + precompute (cmd 30).
void seed_fake_calibration_tables(bool force = false);
// Truncate/create a LittleFS file and write a full bank in one shot.
// Shared with the bulk-restore path in preset_store.ino.
void write_fs_bank(const char* name, const uint8_t* data, size_t size);

#endif