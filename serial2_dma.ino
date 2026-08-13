#include "include_all.h"
#include "hardware/dma.h"
#include "hardware/uart.h"
#include "pico/mutex.h"
#include <string.h>

// Ping-pong so an ADSR/filter block or a 4-slot directory chunk can queue
// while the previous transfer drains. 256 B holds several stuffed frames.
// Serial2 is Arduino-Pico uart1 (GP20 TX / GP21 RX).
static constexpr uint16_t SERIAL2_DMA_BUF_SIZE = 256;

static mutex_t serial2_dma_mutex;
static int serial2_dma_chan = -1;
static dma_channel_config serial2_dma_cfg;
static uint8_t serial2_dma_buf[2][SERIAL2_DMA_BUF_SIZE];
static uint16_t serial2_dma_len[2];
static uint8_t serial2_dma_fill;
static bool serial2_dma_sending;

Serial2DmaTx Serial2Dma;

static void serial2_dma_poll_unlocked() {
  if (serial2_dma_chan < 0) {
    return;
  }
  if (serial2_dma_sending) {
    if (dma_channel_is_busy((uint)serial2_dma_chan)) {
      return;
    }
    serial2_dma_sending = false;
  }
  const uint16_t count = serial2_dma_len[serial2_dma_fill];
  if (count == 0) {
    return;
  }
  const uint8_t send = serial2_dma_fill;
  serial2_dma_len[send] = 0;
  serial2_dma_fill ^= 1u;
  serial2_dma_sending = true;
  dma_channel_set_write_addr((uint)serial2_dma_chan, &uart_get_hw(uart1)->dr, false);
  dma_channel_set_read_addr((uint)serial2_dma_chan, serial2_dma_buf[send], false);
  dma_channel_set_trans_count((uint)serial2_dma_chan, count, true);
}

void serial2_dma_init() {
  mutex_init(&serial2_dma_mutex);
  serial2_dma_chan = dma_claim_unused_channel(true);
  serial2_dma_cfg = dma_channel_get_default_config((uint)serial2_dma_chan);
  channel_config_set_transfer_data_size(&serial2_dma_cfg, DMA_SIZE_8);
  channel_config_set_read_increment(&serial2_dma_cfg, true);
  channel_config_set_write_increment(&serial2_dma_cfg, false);
  channel_config_set_dreq(&serial2_dma_cfg, uart_get_dreq(uart1, true));
  dma_channel_configure((uint)serial2_dma_chan, &serial2_dma_cfg,
                        &uart_get_hw(uart1)->dr, nullptr, 0, false);
}

void serial2_dma_poll() {
  mutex_enter_blocking(&serial2_dma_mutex);
  serial2_dma_poll_unlocked();
  mutex_exit(&serial2_dma_mutex);
}

bool serial2_dma_tx_ready() {
  if (serial2_dma_chan < 0) {
    return false;
  }
  mutex_enter_blocking(&serial2_dma_mutex);
  serial2_dma_poll_unlocked();
  const bool ok =
      (uint16_t)(serial2_dma_len[serial2_dma_fill] + SERIAL_STUFFED_MAX) <=
      SERIAL2_DMA_BUF_SIZE;
  mutex_exit(&serial2_dma_mutex);
  return ok;
}

size_t serial2_dma_write(const uint8_t *p, size_t n) {
  if (serial2_dma_chan < 0 || p == nullptr || n == 0) {
    return 0;
  }
  if (n > SERIAL2_DMA_BUF_SIZE) {
    return 0;
  }
  mutex_enter_blocking(&serial2_dma_mutex);
  serial2_dma_poll_unlocked();
  if ((size_t)serial2_dma_len[serial2_dma_fill] + n > SERIAL2_DMA_BUF_SIZE) {
    if (!serial2_dma_sending && serial2_dma_len[serial2_dma_fill] > 0) {
      serial2_dma_poll_unlocked();
    }
    if ((size_t)serial2_dma_len[serial2_dma_fill] + n > SERIAL2_DMA_BUF_SIZE) {
      mutex_exit(&serial2_dma_mutex);
      return 0;
    }
  }
  memcpy(serial2_dma_buf[serial2_dma_fill] + serial2_dma_len[serial2_dma_fill], p, n);
  serial2_dma_len[serial2_dma_fill] =
      (uint16_t)(serial2_dma_len[serial2_dma_fill] + n);
  serial2_dma_poll_unlocked();
  mutex_exit(&serial2_dma_mutex);
  return n;
}
