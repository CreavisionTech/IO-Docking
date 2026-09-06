#include "protocol.h"
#include "pico/time.h"
#include <string.h>

volatile uint32_t g_tx_dropped;

uint16_t ring_free(ring_t *r) {
    return (uint16_t)(TX_RING_SIZE - 1u - ring_avail(r));
}

const char *const err_str[E_COUNT] = {
    "E_BADCMD", "E_PARAM", "E_NOTFOUND", "E_RANGE", "E_CFG", "E_BUSY",
    "E_TIMEOUT", "E_NACK", "E_OVERRUN", "E_IO", "E_LONG", "E_DENIED",
};

void ring_put_byte(ring_t *r, uint8_t b) {
    uint16_t next = (uint16_t)((r->head + 1) & (TX_RING_SIZE - 1));
    if (next == r->tail) return; // full: drop (caller should flush sooner)
    r->buf[r->head] = b;
    r->head = next;
}

void ring_put(ring_t *r, const uint8_t *data, uint16_t len) {
    if (r == &g_tx) {
        uint64_t deadline = time_us_64() + 100000u;
        while (len <= TX_RING_SIZE - 1u && ring_free(r) < len && time_us_64() < deadline)
            protocol_tx_pump();
        if (ring_free(r) < len) { g_tx_dropped++; return; }
    }
    if (ring_free(r) < len) return;
    uint16_t head = r->head;
    uint16_t first = (uint16_t)(TX_RING_SIZE - head);
    if (first > len) first = len;
    memcpy(r->buf + head, data, first);
    memcpy(r->buf, data + first, len - first);
    __compiler_memory_barrier();
    r->head = (uint16_t)((head + len) & (TX_RING_SIZE - 1u));
}

uint16_t ring_avail(ring_t *r) {
    return (uint16_t)((r->head - r->tail) & (TX_RING_SIZE - 1));
}

bool ring_full(ring_t *r) {
    return ((uint16_t)((r->head + 1) & (TX_RING_SIZE - 1))) == r->tail;
}

void ring_get(ring_t *r, uint8_t *dst, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = r->buf[r->tail];
        r->tail = (uint16_t)((r->tail + 1) & (TX_RING_SIZE - 1));
    }
}

void ring_drop(ring_t *r, uint16_t len) {
    r->tail = (uint16_t)((r->tail + len) & (TX_RING_SIZE - 1));
}
