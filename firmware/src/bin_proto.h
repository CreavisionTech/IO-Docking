#ifndef BIN_PROTO_H
#define BIN_PROTO_H

#include <stdint.h>
#include "protocol.h"

#define BIN_SOF      0xCB
#define BIN_TYPE_CMD 0x01
#define BIN_TYPE_RESP 0x02
#define BIN_TYPE_ERR 0x03
#define BIN_TYPE_EVT 0x04
#define BIN_TYPE_DATA 0x05
#define BIN_TYPE_DATA_END 0x06

// Binary-mode response helpers
void bin_send(uint8_t type, uint8_t op, uint16_t seq, const uint8_t *payload, uint16_t len);
void bin_send_resp(uint8_t op, uint16_t seq, const uint8_t *payload, uint16_t len);
void bin_send_err(uint8_t op, uint16_t seq, uint8_t code, const char *msg);

// Binary-mode event emitters (called from the main loop)
void bin_evt_gpio(uint8_t ch, uint8_t level);
void bin_evt_uart_rx(uint8_t ch, const uint8_t *data, uint16_t n);
void bin_evt_uart_overrun(uint8_t ch, uint32_t dropped);
void bin_evt_adc_thresh(uint8_t ch, uint8_t dir, uint16_t mv);
void bin_evt_seq_done(const char *name);

#endif
