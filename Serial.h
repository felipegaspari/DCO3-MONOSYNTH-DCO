#ifndef __SERIAL_H__
#define __SERIAL_H__

#include "serial_param_protocol.h"
#include "serial_protocol.h"
#include "serial_input_protocol.h"
#include "serial_frame.h"
#include "serial_parser.h"

// Serial1 = DIN MIDI @ 31250; Serial2 = Input panel protocol + slim 'x'/'p' TX @ 2.5M.
// Screen has no DCO port: Input relays gap 154 to it on its own Screen port.

void init_serial();
void init_usb();
void serial_panel_task();

#ifdef ENABLE_USB_CONTROL
// USB CDC bench link: same inner frames, for control without the Input board.
void serial_usb_task();
#endif

void serialSendParam32(byte paramNumber, uint32_t paramValue);
void serialSendParam16(byte paramNumber, int16_t paramValue);
// Echo LittleFS-persistable 'p' to Input (USB/MIDI only; never Input→DCO loop).
void serial_echo_persistable_param16(uint8_t id, int16_t value);

#endif
