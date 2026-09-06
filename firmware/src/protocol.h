#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Error codes, ordered to match Host_Protocol.md section 5 (0-based).
enum {
    E_BADCMD = 0, E_PARAM = 1, E_NOTFOUND = 2, E_RANGE = 3, E_CFG = 4,
    E_BUSY = 5, E_TIMEOUT = 6, E_NACK = 7, E_OVERRUN = 8, E_IO = 9,
    E_LONG = 10, E_DENIED = 11,
    E_COUNT
};
extern const char *const err_str[E_COUNT];

// Protocol modes
enum { MODE_TEXT = 0, MODE_BIN = 1 };

// Byte ring buffer, single producer / single consumer. Size must be a power of two.
#define TX_RING_SIZE 8192u
typedef struct {
    volatile uint16_t head;   // next write
    volatile uint16_t tail;   // next read
    uint8_t buf[TX_RING_SIZE];
} ring_t;

void ring_put(ring_t *r, const uint8_t *data, uint16_t len);
void ring_put_byte(ring_t *r, uint8_t b);
uint16_t ring_avail(ring_t *r);
bool ring_full(ring_t *r);
uint16_t ring_free(ring_t *r);
void protocol_tx_pump(void);
extern volatile uint32_t g_tx_dropped;
void ring_get(ring_t *r, uint8_t *dst, uint16_t len);
void ring_drop(ring_t *r, uint16_t len);

// Global USB TX buffer and protocol mode. Defined in main.c.
extern ring_t g_tx;
extern volatile uint8_t g_mode;

// Input entry points (defined in text_cmd.c / bin_proto.c)
void process_line(const char *line);   // text mode
void process_bin_byte(uint8_t b);      // binary mode

#endif
