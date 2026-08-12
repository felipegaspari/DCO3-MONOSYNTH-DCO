// Mono last-note-priority held stack (Core0 MIDI path only).
// Top = sounding pitch. Voice_task porta still restarts only on note_on_flag →
// note_on_flag_flag from VOICE_NOTES (no pitch queue on Core1).
static constexpr uint8_t MONO_NOTE_STACK_DEPTH = 8;
static uint8_t mono_note_stack[MONO_NOTE_STACK_DEPTH];
static uint8_t mono_note_stack_count = 0;

// Empty held stack when entering mono so notes do not leak across voice modes.
void mono_note_stack_clear() {
  mono_note_stack_count = 0;
}

// Remove first match; no-op if note is not held.
static void mono_note_stack_remove(uint8_t note) {
  for (uint8_t i = 0; i < mono_note_stack_count; i++) {
    if (mono_note_stack[i] != note) continue;
    for (uint8_t j = i; j + 1u < mono_note_stack_count; j++) {
      mono_note_stack[j] = mono_note_stack[j + 1u];
    }
    mono_note_stack_count--;
    return;
  }
}

// Re-strike moves to top; if full, drop oldest so the newest note still wins.
static void mono_note_stack_push(uint8_t note) {
  mono_note_stack_remove(note);
  if (mono_note_stack_count >= MONO_NOTE_STACK_DEPTH) {
    for (uint8_t i = 0; i + 1u < MONO_NOTE_STACK_DEPTH; i++) {
      mono_note_stack[i] = mono_note_stack[i + 1u];
    }
    mono_note_stack_count = MONO_NOTE_STACK_DEPTH - 1u;
  }
  mono_note_stack[mono_note_stack_count++] = note;
}

static inline uint8_t mono_note_stack_top() {
  return mono_note_stack[mono_note_stack_count - 1u];
}

// Register note/CC/program/pitch-bend handlers on USB + DIN MIDI. USB begin is in init_usb().
void init_midi() {
  MIDI_USB.setHandleNoteOn(handleNoteOn);
  MIDI_USB.setHandleNoteOff(handleNoteOff);
  MIDI_USB.setHandleControlChange(handleControlChange);
  MIDI_USB.setHandleProgramChange(handleProgramChange);
  MIDI_USB.setHandlePitchBend(handlePitchBend);
  MIDI_USB.setHandleAfterTouchChannel(handleAfterTouchChannel);

  MIDI_SERIAL.begin(MIDI_CHANNEL_OMNI);
  MIDI_SERIAL.setHandleNoteOn(handleNoteOn);
  MIDI_SERIAL.setHandleNoteOff(handleNoteOff);
  MIDI_SERIAL.setHandleControlChange(handleControlChange);
  MIDI_SERIAL.setHandleProgramChange(handleProgramChange);
  MIDI_SERIAL.setHandlePitchBend(handlePitchBend);
  MIDI_SERIAL.setHandleAfterTouchChannel(handleAfterTouchChannel);

  MIDI_USB.turnThruOff();
  MIDI_SERIAL.turnThruOff();
}


// MIDI library callback → note_on(). Invoked from loop via MIDI_*.read().
void handleNoteOn(byte channel, byte pitch, byte velocity) {
  note_on(pitch, velocity);
}
// MIDI library callback → note_off().
void handleNoteOff(byte channel, byte pitch, byte velocity) {
  note_off(pitch);
}

// MIDI CC handler: CC 0/32 latch preset bank, CC 42 sets pitch-bend range,
// everything else goes through the generated map in midi_cc_map.h.
void handleControlChange(byte channel, byte number, byte value) {
  (void)channel;
  // CC 1 (mod wheel) → mod matrix source 11.
  if (number == 1) {
    mod_matrix_set_mod_wheel(value);
    return;
  }
  // CC 0 (Bank Select MSB) / CC 32 (LSB): latch the upper/lower 128-slot bank
  // for the next Program Change. Nonzero = bank 1 (slots 128..255).
  if (number == 0 || number == 32) {
    midiPresetBank = (value != 0) ? 1 : 0;
    return;
  }
  // CC #42 is used to set the pitch bend range in semitones.
  if (number == MIDI_CC_PITCH_BEND_RANGE) {
    pitchBendRange = value;
    // Optimized: Use fast fixed-point multiplication instead of float division.
    pitchBendMultiplier_q24 = (int32_t)(((int64_t)pitchBendRange * RECIP_TWELVE_Q24));
    pitchBendMultiplier = (float)pitchBendMultiplier_q24 / (float)(1 << 24);
    return;
  }
  midi_cc_handle(number, value);
}

// Scale a controller into its parameter's native range and apply it. Unmapped CCs are
// ignored. Linear search over ~70 entries, which at MIDI's 3125 bytes/s is free.
void midi_cc_handle(uint8_t number, uint8_t value) {
  for (size_t i = 0; i < midiCcMapSize; ++i) {
    const MidiCcEntry& entry = midiCcMap[i];
    if (entry.cc != number) continue;

    int16_t scaled = entry.lo + (int16_t)(((int32_t)(entry.hi - entry.lo) * value + 63) / 127);
    if (entry.curve == MIDI_CC_EXP_TIME) {
      scaled = (int16_t)linearToExponential((uint16_t)scaled, MIDI_CC_EXP_BASE, MIDI_CC_EXP_MAX);
    }
    midi_cc_apply(entry.target, scaled);
    return;
  }
}

// Route a scaled value to its target. Table parameters take the normal router; ADSR/filter
// block values have no ParamId (1 ms packed 'a'–'d' frames), so they are written here
// exactly as input_handle_*() in Serial.ino writes them. PW and EnvVCA→VCA are ParamIds.
void midi_cc_apply(uint8_t target, int16_t value) {
  switch (target) {
    case CC_LOCAL_ADSR_VCA_ATTACK:      ADSR_VCA_attack  = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCA_A); break;
    case CC_LOCAL_ADSR_VCA_DECAY:       ADSR_VCA_decay   = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCA_D); break;
    case CC_LOCAL_ADSR_VCA_SUSTAIN:     ADSR_VCA_sustain = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCA_S); break;
    case CC_LOCAL_ADSR_VCA_RELEASE:     ADSR_VCA_release = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCA_R); break;

    case CC_LOCAL_ADSR_VCF_ATTACK:      ADSR_VCF_attack  = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCF_A); break;
    case CC_LOCAL_ADSR_VCF_DECAY:       ADSR_VCF_decay   = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCF_D); break;
    case CC_LOCAL_ADSR_VCF_SUSTAIN:     ADSR_VCF_sustain = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCF_S); break;
    case CC_LOCAL_ADSR_VCF_RELEASE:     ADSR_VCF_release = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_VCF_R); break;

    case CC_LOCAL_ADSR_DCO_ATTACK:      ADSR1_attack  = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_DCO_A); break;
    case CC_LOCAL_ADSR_DCO_DECAY:       ADSR1_decay   = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_DCO_D); break;
    case CC_LOCAL_ADSR_DCO_SUSTAIN:     ADSR1_sustain = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_DCO_S); break;
    case CC_LOCAL_ADSR_DCO_RELEASE:     ADSR1_release = (uint16_t)value; mark_adsr_params_dirty(ADSR_DIRTY_DCO_R); break;

    // CUTOFF/RESONANCE are used live in update_CV_outs; only mod depths need scale bake.
    // Input 'd' bakes ADSR2+LFO2 once for the filter block (depths in the same payload).
    case CC_LOCAL_FILTER_CUTOFF:        CUTOFF     = (uint16_t)value; break;
    case CC_LOCAL_FILTER_RESONANCE:     RESONANCE  = (uint16_t)value; break;
    case CC_LOCAL_FILTER_ADSR2_TO_VCF:  ADSR2toVCF = value;           cv_bake_adsr2_to_vcf_scale(); break;
    case CC_LOCAL_FILTER_LFO2_TO_VCF:   LFO2toVCF  = (uint16_t)value; cv_bake_lfo2_to_vcf_scale(); break;

    default:
      update_parameters((uint16_t)target, value);
      serial_echo_persistable_param16(target, value);
      return;
  }

  // A block CC has no ParamId to echo, so mirror the whole block the way a
  // preset recall does; otherwise the panel and the Screen keep showing the
  // values they last sent. The CC_LOCAL_* codes are grouped per block.
  if (target <= CC_LOCAL_ADSR_VCA_RELEASE) {
    serial_send_adsr_vca_block_to_mb();
  } else if (target <= CC_LOCAL_ADSR_VCF_RELEASE) {
    serial_send_adsr_vcf_block_to_mb();
  } else if (target <= CC_LOCAL_ADSR_DCO_RELEASE) {
    serial_send_adsr_dco_block_to_mb();
  } else {
    serial_send_filter_block_to_mb();
  }
}

// MIDI program-change callback → recall preset slot
// (midiPresetBank * 128 + program), covering 0..255 (preset_store.ino).
void handleProgramChange(byte channel, byte program) {
  (void)channel;
  const uint16_t slot = (uint16_t)midiPresetBank * 128u + (uint16_t)program;
  if (slot < PRESET_NUM_SLOTS) {
    preset_store_load((uint8_t)slot);
  }
}

// MIDI pitch-bend callback → midi_pitch_bend (offset to 0..16383 style).
void handlePitchBend(byte channel, int pitchBend) {
  midi_pitch_bend = pitchBend + 8192;
}

// Channel aftertouch → mod matrix source.
void handleAfterTouchChannel(byte channel, byte pressure) {
  (void)channel;
  mod_matrix_set_aftertouch(pressure);
}

// Allocate voice(s) from MIDI note-on per voiceMode/polyMode; set ADSR flags (EnvDCO/VCA/VCF are local).
// Mono: last-note stack push, then VOICE_NOTES/gate/note_on_flag + noteStart (porta + ADSR).
void note_on(uint8_t note, uint8_t velocity) {
  mod_matrix_on_note_on();

  switch (voiceMode) {
    case 0:
      // Last-note priority: stack top becomes the sounding pitch.
      mono_note_stack_push(note);
      VOICE_NOTES[0] = note;
      midi_velocity[0] = velocity;
      VOICES[0] = millis();
      note_on_flag[0] = 1;
      noteStart[0] = 1;
      noteEnd[0] = 0;
      return;

      break;

    case 1:

      if (polyMode == 0) {
        if (STACK_VOICES < 2) {
          for (int i = 0; i < NUM_VOICES; i++)  // REVISAR!!
          {
            if (VOICE_NOTES[i] == note) {
              VOICES[i] = millis();
              midi_velocity[i] = velocity;
              note_on_flag[i] = 1;
              noteStart[i] = 1;
              return;  // note already playing
            }
          }
        }

        for (int i = 0; i < STACK_VOICES; i++) {  // REVISAR!! Quizas debiera ser NUM_VOICES y no STACK
          uint8_t voice_num = get_free_voice();
          VOICES[voice_num] = millis();
          VOICE_NOTES[voice_num] = note;
          midi_velocity[voice_num] = velocity;
          note_on_flag[voice_num] = 1;
          noteStart[voice_num] = 1;
        }
      }

      if (polyMode == 1) {
        if (STACK_VOICES < 2) {
          for (int i = 0; i < NUM_VOICES; i++)  // REVISAR!!
          {
            if (VOICE_NOTES[i] == note) {
              VOICES[i] = 1;
              midi_velocity[i] = velocity;
              note_on_flag[i] = 1;
              noteStart[i] = 1;
              noteEnd[i] = 0;
              return;  // note already playing
            }
          }
        }

        uint8_t voice_num = get_free_voice_sequential();
        VOICES[voice_num] = 1;
        VOICE_NOTES[voice_num] = note;
        midi_velocity[voice_num] = velocity;
        note_on_flag[voice_num] = 1;
        noteStart[voice_num] = 1;
        noteEnd[voice_num] = 0;
      }
      break;

    case 2:
      // Mode 2 stub: fill all capacity slots (DCO4 stack). Engine still only runs VOICE_TASK.
      for (int i = 0; i < NUM_VOICES_TOTAL; i++)
      {
        VOICES[i] = 1;
        VOICE_NOTES[i] = note;
        midi_velocity[i] = velocity;
        note_on_flag[i] = 1;
        noteStart[i] = 1;
      }
      break;
    default:
      return;
      break;
  }
  last_midi_pitch_bend = 0;
}

// Release matching voice(s) on MIDI note-off; set noteEnd flags for the local envelopes.
// Mono: stack remove; empty → gate off; else fall back to top + note_on_flag (porta, no ADSR retrigger).
void note_off(uint8_t note) {
  if (voiceMode == 0) {
    const uint8_t prev_count = mono_note_stack_count;
    mono_note_stack_remove(note);
    if (mono_note_stack_count == prev_count) {
      return;  // note was not held
    }
    if (mono_note_stack_count == 0) {
      // Keep VOICE_NOTES so voice_task holds last pitch through release.
      VOICES[0] = 0;
      noteEnd[0] = 1;
      noteStart[0] = 0;
      return;
    }
    // Still holding other keys: sound new top; porta via note_on_flag only.
    VOICE_NOTES[0] = mono_note_stack_top();
    VOICES[0] = millis();
    note_on_flag[0] = 1;
    noteEnd[0] = 0;
    return;
  }

  // Para/poly: scan full capacity so a release is not missed if NUM_VOICES shrank mid-note.
  for (int i = 0; i < NUM_VOICES_TOTAL; i++)
  {
    if (VOICE_NOTES[i] == note) {
      // Keep VOICE_NOTES so voice_task holds last pitch through release
      // (portaTime==0 snaps portamento_cur_freq from VOICE_NOTES each frame).
      // Free-slot / steal uses VOICES[] only.
      VOICES[i] = 0;
      noteEnd[i] = 1;
      noteStart[i] = 0;
    }
  }
}