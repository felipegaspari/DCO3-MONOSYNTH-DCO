#ifndef SERIAL2_DMA_H
#define SERIAL2_DMA_H

#include <stddef.h>
#include <stdint.h>

// Serial2 (uart1) TX via DMA. Arduino-Pico keeps the UART1 RX IRQ and software
// FIFO; this channel is the only writer to UART1 TX. Do not Serial2.write().
// Serial1 (MIDI) and USB CDC are unchanged.

void serial2_dma_init();
void serial2_dma_poll();
bool serial2_dma_tx_ready();
size_t serial2_dma_write(const uint8_t *p, size_t n);

struct Serial2DmaTx {
  size_t write(const uint8_t *p, size_t n) {
    return serial2_dma_write(p, n);
  }
};

extern Serial2DmaTx Serial2Dma;

#endif
