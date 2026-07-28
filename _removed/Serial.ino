// Removed unused code from Serial.ino

// --- from original:155-167 ---

void serial_send_generaldata(uint16_t data) {

  if (uart_is_writable(uart1) > 0) {

    uint8_t *b = (uint8_t *)&data;
    uart_putc(uart1, 'w');
    uart_putc_raw(uart1, b[0]);
    uart_putc_raw(uart1, b[1]);
    //uart_putc(uart1, 'z');
    return;
  }
}
