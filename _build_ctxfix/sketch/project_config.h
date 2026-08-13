#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/project_config.h"
#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

// -----------------------------------------------------------------------------
// Which instrument this superproject is.
//
// INPUT-CONTROLLER, SCREEN-CONTROLLER and DCO-CONTROL-PANEL are one repo each,
// checked out into both DCO3-MONOSYNTH and DCO4-REBORN. They read this file
// through a symlink that resolves to whichever project they are sitting in, so
// their sources stay byte-identical in both trees and nothing has to be chosen
// at build time: no flag, no build script, no per-checkout edit.
//
//   3 = DCO3-MONOSYNTH, 1 voice, 3 oscillators + sub
//   4 = DCO4-REBORN, 4 voices of 2 oscillators
//
// The guard leaves -DPROJECT_INSTRUMENT=4 working, to compile-check the other
// instrument from this tree without touching a file.
// -----------------------------------------------------------------------------

#ifndef PROJECT_INSTRUMENT
#define PROJECT_INSTRUMENT 3
#endif

// -----------------------------------------------------------------------------
// Which MCU module the DCO voice board is soldered to.
//
// WeAct RP2040 and official Pico are both RP2040; Pico 2 is RP2350 with the
// same 40-pin GPIO header as Pico 1. Analog RESET/RANGE wiring cannot be
// inferred from PICO_RP2040 / PICO_RP2350. WeAct breaks out GPIO 29; official
// Pico / Pico 2 use that pad as the VSYS ADC, so osc 0/1 move to GP28/26 and
// GP27/22. Osc 2–7 (DCO3's live OSC1–3 slice) are identical on all three.
//
//   DCO_MCU_WEACT_RP2040  WeAct Studio RP2040 (GPIO 29 on header; GP23/24 fix)
//   DCO_MCU_PICO          Raspberry Pi Pico
//   DCO_MCU_PICO2         Raspberry Pi Pico 2 (same header map as Pico)
//
// -DDCO_MCU_BOARD=… wins, to compile-check another module without editing this
// file. DCO/globals.h #error's any other value.
// -----------------------------------------------------------------------------

#define DCO_MCU_WEACT_RP2040  1
#define DCO_MCU_PICO          2
#define DCO_MCU_PICO2         3

#ifndef DCO_MCU_BOARD
#define DCO_MCU_BOARD DCO_MCU_PICO2
#endif

#endif  // PROJECT_CONFIG_H
