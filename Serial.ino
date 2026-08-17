#include "include_all.h"

// Ingress origin tagging to prevent echo loops
enum ParamIngress : uint8_t {
  PARAM_SRC_INPUT = 0,
  PARAM_SRC_USB   = 1,
};
static ParamIngress g_param_ingress = PARAM_SRC_INPUT;

static void serial_forward_input_block_to_mb(char cmd, const uint8_t* payload, uint8_t len) {
  if (g_param_ingress != PARAM_SRC_USB) return;
  if (!serial2_dma_tx_ready()) return;
  serial_frame_write(Serial2Dma, (uint8_t)cmd, payload, len);
}

static void serial_send_adsr_block_to_mb(uint8_t cmd, uint16_t a, uint16_t d, uint16_t s, uint16_t r) {
  if (!serial2_dma_tx_ready()) return;
  uint8_t payload[SERIAL_LEN_ADSR_BLOCK];
  encode_u16_le(payload + 0, a);
  encode_u16_le(payload + 2, d);
  encode_u16_le(payload + 4, s);
  encode_u16_le(payload + 6, r);
  serial_frame_write(Serial2Dma, cmd, payload, SERIAL_LEN_ADSR_BLOCK);
}

void serial_send_adsr_vca_block_to_mb() {
  serial_send_adsr_block_to_mb(
    CMD_ADSR1_BLOCK,
    ADSR_VCA_attack, ADSR_VCA_decay, ADSR_VCA_sustain, ADSR_VCA_release);
}

void serial_send_adsr_vcf_block_to_mb() {
  serial_send_adsr_block_to_mb(
    CMD_ADSR2_BLOCK,
    ADSR_VCF_attack, ADSR_VCF_decay, ADSR_VCF_sustain, ADSR_VCF_release);
}

void serial_send_adsr_dco_block_to_mb() {
  serial_send_adsr_block_to_mb(
    CMD_ADSR3_BLOCK,
    ADSR1_attack, ADSR1_decay, ADSR1_sustain, ADSR1_release);
}

void serial_send_filter_block_to_mb() {
  if (!serial2_dma_tx_ready()) return;
  uint8_t payload[SERIAL_LEN_FILTER_BLOCK];
  encode_u16_le(payload + 0, CUTOFF);
  encode_u16_le(payload + 2, RESONANCE);
  encode_u16_le(payload + 4, (uint16_t)ADSR2toVCF);
  encode_u16_le(payload + 6, LFO2toVCF);
  serial_frame_write(Serial2Dma, CMD_FILTER_BLOCK, payload, SERIAL_LEN_FILTER_BLOCK);
}

void serial_send_preset_loaded_to_mb(uint8_t slot) {
  if (!serial2_dma_tx_ready()) return;
  uint8_t payload[SERIAL_LEN_PRESET_LOADED] = { slot };
  serial_frame_write(Serial2Dma, CMD_PRESET_LOADED, payload, SERIAL_LEN_PRESET_LOADED);
}

static void input_handle_adsr1(char, const uint8_t* payload, uint8_t len) {
  if (len != SERIAL_LEN_ADSR_BLOCK) return;
  uint16_t dirty = 0, v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR_VCA_attack)  { ADSR_VCA_attack  = v; dirty |= ADSR_DIRTY_VCA_A; }
  v = decode_u16_le(payload + 2);
  if (v != ADSR_VCA_decay)   { ADSR_VCA_decay   = v; dirty |= ADSR_DIRTY_VCA_D; }
  v = decode_u16_le(payload + 4);
  if (v != ADSR_VCA_sustain) { ADSR_VCA_sustain = v; dirty |= ADSR_DIRTY_VCA_S; }
  v = decode_u16_le(payload + 6);
  if (v != ADSR_VCA_release) { ADSR_VCA_release = v; dirty |= ADSR_DIRTY_VCA_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
  serial_forward_input_block_to_mb(CMD_ADSR1_BLOCK, payload, len);
}

static void input_handle_adsr2(char, const uint8_t* payload, uint8_t len) {
  if (len != SERIAL_LEN_ADSR_BLOCK) return;
  uint16_t dirty = 0, v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR_VCF_attack)  { ADSR_VCF_attack  = v; dirty |= ADSR_DIRTY_VCF_A; }
  v = decode_u16_le(payload + 2);
  if (v != ADSR_VCF_decay)   { ADSR_VCF_decay   = v; dirty |= ADSR_DIRTY_VCF_D; }
  v = decode_u16_le(payload + 4);
  if (v != ADSR_VCF_sustain) { ADSR_VCF_sustain = v; dirty |= ADSR_DIRTY_VCF_S; }
  v = decode_u16_le(payload + 6);
  if (v != ADSR_VCF_release) { ADSR_VCF_release = v; dirty |= ADSR_DIRTY_VCF_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
  serial_forward_input_block_to_mb(CMD_ADSR2_BLOCK, payload, len);
}

static void input_handle_adsr3(char, const uint8_t* payload, uint8_t len) {
  if (len != SERIAL_LEN_ADSR_BLOCK) return;
  uint16_t dirty = 0, v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR1_attack)  { ADSR1_attack  = v; dirty |= ADSR_DIRTY_DCO_A; }
  v = decode_u16_le(payload + 2);
  if (v != ADSR1_decay)   { ADSR1_decay   = v; dirty |= ADSR_DIRTY_DCO_D; }
  v = decode_u16_le(payload + 4);
  if (v != ADSR1_sustain) { ADSR1_sustain = v; dirty |= ADSR_DIRTY_DCO_S; }
  v = decode_u16_le(payload + 6);
  if (v != ADSR1_release) { ADSR1_release = v; dirty |= ADSR_DIRTY_DCO_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
  serial_forward_input_block_to_mb(CMD_ADSR3_BLOCK, payload, len);
}

static void input_handle_filter_block(char, const uint8_t* payload, uint8_t len) {
  if (len != SERIAL_LEN_FILTER_BLOCK) return;
  CUTOFF     = decode_u16_le(payload + 0);
  RESONANCE  = decode_u16_le(payload + 2);
  ADSR2toVCF = decode_i16_le(payload + 4);
  LFO2toVCF  = decode_u16_le(payload + 6);
  cv_bake_adsr2_to_vcf_scale();
  cv_bake_lfo2_to_vcf_scale();
  serial_forward_input_block_to_mb(CMD_FILTER_BLOCK, payload, len);
}

void serialSendParam16(byte paramNumber, int16_t paramValue, bool force) {
  if (!force && !serial2_dma_tx_ready()) return;
  while (force && !serial2_dma_tx_ready()) {
    tight_loop_contents();
  }
  uint8_t payload[SERIAL_LEN_PARAM_16];
  encode_param_p(payload, (uint8_t)paramNumber, paramValue);
  serial_frame_write(Serial2Dma, CMD_PARAM_16, payload, SERIAL_LEN_PARAM_16);
}

void serial_echo_persistable_param16(uint8_t id, int16_t value) {
  if (!preset_param_is_persistable(id)) return;
  serialSendParam16(id, value);
}

static void input_handle_param16(char, const uint8_t* payload, uint8_t len) {
  if (len != SERIAL_LEN_PARAM_16) return;
  ParamFrame frame;
  decode_param_p(payload, frame);
  update_parameters(frame.id, (int16_t)frame.value);
  if (g_param_ingress != PARAM_SRC_INPUT) {
    serial_echo_persistable_param16(frame.id, (int16_t)frame.value);
  }
}

static void input_handle_preset_name(char, const uint8_t* payload, uint8_t len) {
  if (len != SERIAL_LEN_PRESET_NAME) return;
  for (int i = 0; i < 16; ++i) {
    presetName[i] = payload[i];
  }
}

static void input_handle_bulk_chunk(char, const uint8_t* payload, uint8_t len) {
  preset_bulk_chunk(payload, len);
}

static void input_handle_bulk_commit(char, const uint8_t* payload, uint8_t len) {
  preset_bulk_commit(payload, len);
}

static void input_handle_preset_dir_request(char, const uint8_t*, uint8_t) {
  preset_store_send_directory_to_mb();
}

static const SerialCommandDef inputSerialCommands[] = {
  { CMD_ADSR1_BLOCK,        SERIAL_LEN_ADSR_BLOCK,        input_handle_adsr1             },
  { CMD_ADSR2_BLOCK,        SERIAL_LEN_ADSR_BLOCK,        input_handle_adsr2             },
  { CMD_ADSR3_BLOCK,        SERIAL_LEN_ADSR_BLOCK,        input_handle_adsr3             },
  { CMD_FILTER_BLOCK,       SERIAL_LEN_FILTER_BLOCK,      input_handle_filter_block      },
  { CMD_PARAM_16,           SERIAL_LEN_PARAM_16,          input_handle_param16           },
  { CMD_PRESET_NAME,        SERIAL_LEN_PRESET_NAME,       input_handle_preset_name       },
  { CMD_BULK_CHUNK,         SERIAL_LEN_BULK_CHUNK,        input_handle_bulk_chunk        },
  { CMD_BULK_COMMIT,        SERIAL_LEN_BULK_COMMIT,       input_handle_bulk_commit       },
  { CMD_PRESET_DIR_REQUEST, SERIAL_LEN_PRESET_DIR_REQUEST,input_handle_preset_dir_request},
};

static SerialCommandTable inputSerialLut;
static SerialParserContext inputSerialParser = {};

void init_serial() {
  Serial1.setFIFOSize(256);
  Serial1.setPollingMode(false);
  Serial1.setRX(1);
  Serial1.setTX(0);
  Serial1.begin(31250);

  Serial2.setFIFOSize(512);
  Serial2.setPollingMode(false);
  Serial2.setRX(21);
  Serial2.setTX(20);
  Serial2.begin(2500000);
  serial2_dma_init();

  serial_command_table_init(
    inputSerialLut,
    inputSerialCommands,
    sizeof(inputSerialCommands) / sizeof(inputSerialCommands[0])
  );
}

void init_usb() {
  USBDevice.setManufacturerDescriptor("FELA         ");
  USBDevice.setProductDescriptor("DCO3-MONO   ");

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  Serial.begin(2000000);
  usb_midi.setStringDescriptor("DCO3-MONO MIDI");
  MIDI_USB.begin(MIDI_CHANNEL_OMNI);

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

void __not_in_flash_func(serial_panel_task)() {
  serial2_dma_poll();
  g_param_ingress = PARAM_SRC_INPUT;
  serial_parser_drain(
    inputSerialParser,
    inputSerialLut,
    Serial2,
    SERIAL_DRAIN_BYTE_BUDGET
  );
}

#ifdef ENABLE_USB_CONTROL
static SerialParserContext usbSerialParser = {};

void __not_in_flash_func(serial_usb_task)() {
  if (!Serial) return;
  g_param_ingress = PARAM_SRC_USB;
  serial_parser_drain(
    usbSerialParser,
    inputSerialLut,
    Serial,
    SERIAL_DRAIN_BYTE_BUDGET
  );
}
#endif

void serialSendParam32(byte paramNumber, uint32_t paramValue, bool force) {
  if (!force && !serial2_dma_tx_ready()) return;
  while (force && !serial2_dma_tx_ready()) {
    tight_loop_contents();
  }
  uint8_t payload[SERIAL_LEN_PARAM_32];
  encode_param32(payload, (uint8_t)paramNumber, paramValue);
  serial_frame_write(Serial2Dma, CMD_PARAM_32, payload, SERIAL_LEN_PARAM_32);
}