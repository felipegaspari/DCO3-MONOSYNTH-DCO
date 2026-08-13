#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/include_all.h"
#ifndef __INCLUDE_ALL__H_
#define __INCLUDE_ALL__H_

#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
//#include "tusb_config.h"

#include "pico/stdlib.h"
// #include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico-dco.pio.h"
#include "hardware/pwm.h"
// #include "hardware/spi.h"

#include "LittleFS.h"
//#include <SingleFileDrive.h>
// #include <EEPROM.h>

#include "params_def.h"
#include "param_router.h"

#include "globals.h"
#include "cv_state.h"
#include "cv_bezier.h"
#include "cv_out.h"

#include "FS.h"
#include "preset_store.h"

#include "noteList.h"
#include "amp_comp.h"

#include "Serial.h"
#include "midi.h"
#include "voices.h"
#include "state_machines.h"
#include "subosc.h"
#include "PWM.h"
#include "utils.h"
#include "noise.h"
#include "character_jitter.h"
#include "Timer_micros.h"
#include "mem_diag.h"

#include "LFO.h"
#include "adsr.h"
#include "voice_alloc_state.h"
#include "wave_mux.h"
#include "mod_matrix.h"

#include "autotune.h"

#endif