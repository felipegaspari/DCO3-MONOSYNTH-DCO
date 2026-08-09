// Serial1 = MIDI DIN @ 31250; Serial2 = Input hub @ 2.5M (inner panel protocol).
// Screen has no DCO port: gap 'x' rides the Input link and Input relays it.

static void input_handle_adsr1(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_ADSR_BLOCK) return;

  uint16_t dirty = 0;
  uint16_t v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR_VCA_attack)  { ADSR_VCA_attack  = v; dirty |= ADSR_DIRTY_VCA_A; }

  v = decode_u16_le(payload + 2);
  if (v != ADSR_VCA_decay)   { ADSR_VCA_decay   = v; dirty |= ADSR_DIRTY_VCA_D; }

  v = decode_u16_le(payload + 4);
  if (v != ADSR_VCA_sustain) { ADSR_VCA_sustain = v; dirty |= ADSR_DIRTY_VCA_S; }

  v = decode_u16_le(payload + 6);
  if (v != ADSR_VCA_release) { ADSR_VCA_release = v; dirty |= ADSR_DIRTY_VCA_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
}

static void input_handle_adsr2(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_ADSR_BLOCK) return;

  uint16_t dirty = 0;
  uint16_t v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR_VCF_attack)  { ADSR_VCF_attack  = v; dirty |= ADSR_DIRTY_VCF_A; }

  v = decode_u16_le(payload + 2);
  if (v != ADSR_VCF_decay)   { ADSR_VCF_decay   = v; dirty |= ADSR_DIRTY_VCF_D; }

  v = decode_u16_le(payload + 4);
  if (v != ADSR_VCF_sustain) { ADSR_VCF_sustain = v; dirty |= ADSR_DIRTY_VCF_S; }

  v = decode_u16_le(payload + 6);
  if (v != ADSR_VCF_release) { ADSR_VCF_release = v; dirty |= ADSR_DIRTY_VCF_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
}

// EnvDCO times ('c') → existing ADSR1_* engine (pitch/PW)
static void input_handle_adsr3(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_ADSR_BLOCK) return;

  uint16_t dirty = 0;
  uint16_t v;

  v = decode_u16_le(payload + 0);
  if (v != ADSR1_attack)  { ADSR1_attack  = v; dirty |= ADSR_DIRTY_DCO_A; }

  v = decode_u16_le(payload + 2);
  if (v != ADSR1_decay)   { ADSR1_decay   = v; dirty |= ADSR_DIRTY_DCO_D; }

  v = decode_u16_le(payload + 4);
  if (v != ADSR1_sustain) { ADSR1_sustain = v; dirty |= ADSR_DIRTY_DCO_S; }

  v = decode_u16_le(payload + 6);
  if (v != ADSR1_release) { ADSR1_release = v; dirty |= ADSR_DIRTY_DCO_R; }

  if (dirty) mark_adsr_params_dirty(dirty);
}

static void input_handle_filter_block(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_FILTER_BLOCK) return;
  CUTOFF     = decode_u16_le(payload + 0);
  RESONANCE  = decode_u16_le(payload + 2);
  ADSR2toVCF = decode_i16_le(payload + 4);
  LFO2toVCF  = decode_u16_le(payload + 6);
  cv_bake_adsr2_to_vcf_scale();
  cv_bake_lfo2_to_vcf_scale();
}

// USB and panel share one LUT. Tag the drain so USB 'p' can mirror to Input
// without echoing the panel's own stream (loop).
enum ParamIngress : uint8_t {
  PARAM_SRC_INPUT = 0,
  PARAM_SRC_USB   = 1,
};
static ParamIngress g_param_ingress = PARAM_SRC_INPUT;

static bool param_is_persistable(uint8_t id) {
  if (id >= (uint8_t)PARAM_MOD_SLOT0_SOURCE && id <= (uint8_t)PARAM_MOD_SLOT7_DEPTH) {
    return true;
  }
  if (id >= (uint8_t)PARAM_LFO1_TO_OSC1 && id <= (uint8_t)PARAM_ADSR3_PITCH_MODE) {
    return true;
  }
  switch (id) {
    case PARAM_OSC1_SAW_ENABLE:
    case PARAM_OSC1_PULSE_ENABLE:
    case PARAM_OSC1_TRI_ENABLE:
    case PARAM_RESONANCE_COMPENSATION:
    case PARAM_VCA_ADSR_RESTART:
    case PARAM_VCF_ADSR_RESTART:
    case PARAM_ADSR3_TO_OSC_SELECT:
    case PARAM_LFO1_WAVEFORM:
    case PARAM_LFO2_WAVEFORM:
    case PARAM_OSC1_INTERVAL:
    case PARAM_OSC2_INTERVAL:
    case PARAM_OSC2_DETUNE_VAL:
    case PARAM_LFO2_TO_OSC2:
    case PARAM_OSC_SYNC_MODE:
    case PARAM_PORTAMENTO_TIME:
    case PARAM_VCF_KEYTRACK:
    case PARAM_VELOCITY_TO_VCF:
    case PARAM_VELOCITY_TO_VCA:
    case PARAM_OSC1_LEVEL:
    case PARAM_OSC2_LEVEL:
    case PARAM_SUB_LEVEL:
    case PARAM_OSC3_LEVEL:
    case PARAM_VOICE_MODE:
    case PARAM_UNISON_DETUNE:
    case PARAM_ANALOG_DRIFT_AMOUNT:
    case PARAM_ANALOG_DRIFT_SPEED:
    case PARAM_ANALOG_DRIFT_SPREAD:
    case PARAM_SYNC_MODE:
    case PARAM_PORTAMENTO_MODE:
    case PARAM_OSC3_INTERVAL:
    case PARAM_OSC3_DETUNE_VAL:
    case PARAM_LFO2_TO_OSC3:
    case PARAM_SOFT_SYNC:
    case PARAM_SUBOSC_DIVIDE:
    case PARAM_LFO1_TO_DCO:
    case PARAM_LFO1_SPEED:
    case PARAM_LFO2_SPEED:
    case PARAM_VCA_LEVEL:
    case PARAM_LFO1_TO_VCA:
    case PARAM_LFO2_TO_PW:
    case PARAM_ADSR3_TO_PWM:
    case PARAM_ADSR3_TO_DETUNE1:
    case PARAM_ADSR1_ATTACK_CURVE:
    case PARAM_ADSR1_DECAY_CURVE:
    case PARAM_ADSR2_ATTACK_CURVE:
    case PARAM_ADSR2_DECAY_CURVE:
    case PARAM_DIST_DRIVE:
    case PARAM_DIST_MIX:
    case PARAM_FILTER_MODE:
    case PARAM_OSC2_SAW_ENABLE:
    case PARAM_OSC2_PULSE_ENABLE:
    case PARAM_OSC2_TRI_ENABLE:
    case PARAM_OSC3_SAW_ENABLE:
    case PARAM_OSC3_PULSE_ENABLE:
    case PARAM_OSC3_TRI_ENABLE:
    case PARAM_ADSR3_ENABLED:
    case PARAM_PW_VALUE:
      return true;
    default:
      return false;
  }
}

void serialSendParam16(byte paramNumber, int16_t paramValue) {
  if (Serial2.availableForWrite() < 1) {
    return;
  }
  uint8_t payload[INPUT_SERIAL_LEN_PARAM_16];
  encode_param_p(payload, (uint8_t)paramNumber, paramValue);
  serial_frame_write(Serial2, INPUT_CMD_PARAM_16, payload, INPUT_SERIAL_LEN_PARAM_16);
}

void serial_echo_persistable_param16(uint8_t id, int16_t value) {
  if (!param_is_persistable(id)) {
    return;
  }
  serialSendParam16(id, value);
}

static void input_handle_param16(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_PARAM_16) return;
  ParamFrame frame;
  decode_param_p(payload, frame);
  update_parameters(frame.id, (int16_t)frame.value);
  if (g_param_ingress != PARAM_SRC_INPUT) {
    serial_echo_persistable_param16(frame.id, (int16_t)frame.value);
  }
}

static void input_handle_preset_name(char, const uint8_t* payload, uint8_t len) {
  if (len != INPUT_SERIAL_LEN_PRESET_NAME) return;
  for (int i = 0; i < 8; ++i) {
    presetName[i] = payload[i];
  }
}

static const SerialCommandDef inputSerialCommands[] = {
  { INPUT_CMD_ADSR1_BLOCK,   INPUT_SERIAL_LEN_ADSR_BLOCK,   input_handle_adsr1        },
  { INPUT_CMD_ADSR2_BLOCK,   INPUT_SERIAL_LEN_ADSR_BLOCK,   input_handle_adsr2        },
  { INPUT_CMD_ADSR3_BLOCK,   INPUT_SERIAL_LEN_ADSR_BLOCK,   input_handle_adsr3        },
  { INPUT_CMD_FILTER_BLOCK,  INPUT_SERIAL_LEN_FILTER_BLOCK, input_handle_filter_block },
  { INPUT_CMD_PARAM_16,      INPUT_SERIAL_LEN_PARAM_16,     input_handle_param16      },
  { INPUT_CMD_PRESET_NAME,   INPUT_SERIAL_LEN_PRESET_NAME,  input_handle_preset_name  },
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

  serial_command_table_init(
    inputSerialLut,
    inputSerialCommands,
    sizeof(inputSerialCommands) / sizeof(inputSerialCommands[0])
  );
}

// USB composite: CDC serial + MIDI. Descriptors first, then detach/attach so the
// host re-enumerates after flash/soft reset (avoids missing /dev/ttyACM* on Linux).
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
  g_param_ingress = PARAM_SRC_INPUT;
  serial_parser_drain(
    inputSerialParser,
    inputSerialLut,
    Serial2,
    SERIAL_DRAIN_BYTE_BUDGET
  );
}

// USB CDC bench link: same inner frames as Serial2. Only host → DCO is framed;
// DCO → host stays plain debug text.
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

#endif  // ENABLE_USB_CONTROL

// TX slim 'x' to Input: gap (154) and cal offsets (155). Input relays 154 on to Screen.
// Drop the frame if Serial2 TX is not ready (USB-only bench with no Input board).
// Persistable USB/MIDI 'p' echo is serialSendParam16 / serial_echo_persistable_param16.
void serialSendParam32(byte paramNumber, uint32_t paramValue) {
  if (Serial2.availableForWrite() < 1) {
    return;
  }
  uint8_t payload[INPUT_SERIAL_LEN_PARAM_32];
  encode_param32(payload, (uint8_t)paramNumber, paramValue);
  serial_frame_write(Serial2, INPUT_CMD_PARAM_32, payload, INPUT_SERIAL_LEN_PARAM_32);
}
