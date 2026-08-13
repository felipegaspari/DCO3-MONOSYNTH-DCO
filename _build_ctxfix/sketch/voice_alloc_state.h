#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/voice_alloc_state.h"
#ifndef __VOICE_ALLOC_STATE_H__
#define __VOICE_ALLOC_STATE_H__

// Voice allocation policy and mono note priority, both driven by
// PARAM_VOICE_ALLOC_MODE. The implementation is shared with DCO4-REBORN
// (DCO-SHARED-LIBRARIES/voice_alloc.h); this file only picks the build flags
// and declares the instances.

// Same reasoning as ADSR_BEZIER_SRAM_HOT / MO_LFO_SRAM_HOT: keep the note path
// out of flash so a note-on cannot stall on the XIP cache while Core1 is
// mid-frame.
#ifndef VOICE_ALLOC_SRAM_HOT
#define VOICE_ALLOC_SRAM_HOT 1
#endif

#include "_shared/voice_alloc.h"

// Para/poly allocation state: voice state, trigger/release stamps, LRU order.
// The gate flags (VOICES[]) and the pitch table (VOICE_NOTES[]) stay in
// globals.h where voice_task and autotune read them; voice_mark_on() and
// voice_mark_off() in voices.ino keep the two in step.
VoiceAllocator<NUM_VOICES_TOTAL> voiceAlloc;

// Mono held-key stack (Core0 MIDI path only). Which entry sounds depends on the
// same allocation mode. Voice_task porta still restarts only on note_on_flag →
// note_on_flag_flag from VOICE_NOTES (no pitch queue on Core1).
MonoNoteStack<8> monoStack;

#endif
