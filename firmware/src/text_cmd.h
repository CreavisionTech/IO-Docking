#ifndef TEXT_CMD_H
#define TEXT_CMD_H

#include <stdint.h>
#include "protocol.h"

// Text-mode response helpers
void tprintf(const char *fmt, ...);
void tputs(const char *s);

// Text-mode event emitters (called from the main loop)
void evt_gpio(uint8_t ch, uint8_t level);
void evt_uart_rx(uint8_t ch, const uint8_t *data, uint16_t n);
void evt_uart_overrun(uint8_t ch, uint32_t dropped);
void evt_adc_thresh(uint8_t ch, uint8_t dir, uint16_t mv);
void evt_seq_done(const char *name);

#endif
