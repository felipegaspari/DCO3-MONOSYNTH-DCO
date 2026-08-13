#ifndef __SERIAL_H__
#define __SERIAL_H__

// DCO accepts the 36-byte 'B' bulk-restore chunk (preset_store.h), so the inner
// payload cap must be raised before serial_frame.h locks its default of 8.
#ifndef SERIAL_INNER_MAX_PAYLOAD
#define SERIAL_INNER_MAX_PAYLOAD 36
#endif

#include "serial_param_protocol.h"
#include "serial_protocol.h"
#include "serial_input_protocol.h"
#include "serial_frame.h"
#include "serial_parser.h"
#include "serial2_dma.h"

// Serial1 = DIN MIDI @ 31250; Serial2 = Input panel protocol + slim 'x'/'p' TX @ 2.5M.
// Screen has no DCO port: Input relays gap 154 to it on its own Screen port.

void init_serial();
void init_usb();
void serial_panel_task();

#ifdef ENABLE_USB_CONTROL
// USB CDC bench link: same inner frames, for control without the Input board.
void serial_usb_task();
#endif

void serialSendParam32(byte paramNumber, uint32_t paramValue, bool force = false);
void serialSendParam16(byte paramNumber, int16_t paramValue, bool force = false);
// Echo LittleFS-persistable 'p' to Input (USB/MIDI only; never Input→DCO loop).
void serial_echo_persistable_param16(uint8_t id, int16_t value);

// Mirror current block globals to the Input hub (used after a preset load).
void serial_send_adsr_vca_block_to_mb();
void serial_send_adsr_vcf_block_to_mb();
void serial_send_adsr_dco_block_to_mb();
void serial_send_filter_block_to_mb();

// 'L' to Input: fired once at the end of every successful preset_store_load()
// (boot recall, MIDI PC, USB/dco_control, Input-triggered), so Input's Screen
// display reflects the DCO's actual current slot even for loads it didn't
// itself trigger.
void serial_send_preset_loaded_to_mb(uint8_t slot);

#endif
