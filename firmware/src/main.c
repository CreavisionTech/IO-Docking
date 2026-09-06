// IO Dock RP2040 firmware main loop.
//  - USB CDC (TinyUSB) is the control channel.
//  - Text protocol by default; BIN ENTER switches to binary mode.
//  - Core 1 runs the sequence engine; core 0 serves USB + drivers.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "tusb.h"
#include "board.h"
#include "protocol.h"
#include "drivers.h"
#include "seq.h"
#include "config.h"
#include "text_cmd.h"
#include "bin_proto.h"

ring_t g_tx = { 0, 0, { 0 } };
volatile uint8_t g_mode = MODE_TEXT;

void board_serial_init(void);

static char line[512];
static uint16_t line_len;
static bool line_overflow;
static uint64_t gpio_last_evt[IO_COUNT];
static uint32_t uart_last_errs[2];

// ---------- USB TX ----------

static void flush_tx(void) {
    if (!tud_cdc_connected()) return;
    // Batch: push as much as TinyUSB's CDC buffer can take, flushing only once
    // the TX ring is drained (or the buffer is full). This yields larger USB
    // IN bursts than flushing after every small write, which matters for the
    // continuous ADC data stream.
    while (ring_avail(&g_tx)) {
        uint32_t room = tud_cdc_write_available();
        if (room == 0) return; // buffer full; tud_task() will transmit it
        uint16_t n = ring_avail(&g_tx);
        if (n > room) n = (uint16_t)room;
        uint16_t to_end = (uint16_t)(TX_RING_SIZE - g_tx.tail);
        if (n > to_end) n = to_end;
        if (n == 0) return;
        uint32_t written = tud_cdc_write(&g_tx.buf[g_tx.tail], n);
        if (written == 0) return;
        ring_drop(&g_tx, (uint16_t)written);
    }
    tud_cdc_write_flush();
}

// Used by the protocol writer for bounded backpressure without reentering RX.
void protocol_tx_pump(void) {
    tud_task();
    flush_tx();
}

// ---------- USB RX ----------

static void usb_input(void) {
    unsigned budget = 512;
    while (budget-- && tud_cdc_available()) {
        // Process at most one command per main-loop pass, preserving reply room.
        uint8_t b;
        if (tud_cdc_read(&b, 1) != 1) break;
        if (g_mode == MODE_TEXT) {
            if (b == '\n') {
                line[line_len] = '\0';
                if (line_overflow) tputs("ERR LINE E_LONG command too long\n");
                else process_line(line);
                line_len = 0;
                line_overflow = false;
                break;
            } else if (b != '\r') {
                if (line_len < sizeof(line) - 1) line[line_len++] = (char)b;
                else line_overflow = true;
            }
        } else {
            process_bin_byte(b);
        }
    }
}

// ---------- async events ----------

static void handle_gpio_events(void) {
    uint32_t pend = io_events_pending();
    if (!pend) return;
    uint32_t mask = 0;
    uint64_t now = time_us_64();
    for (uint8_t i = 0; i < IO_COUNT; i++) {
        if (pend & (1u << i)) {
            if (now - gpio_last_evt[i] >= 50000) { // 50 ms debounce
                gpio_last_evt[i] = now;
                uint8_t lv = gpio_get(IO_GPIO_BASE + i) ? 1 : 0;
                if (g_mode == MODE_TEXT) evt_gpio(i, lv);
                else bin_evt_gpio(i, lv);
            }
            mask |= (1u << i);
        }
    }
    (void)mask; // pending events were atomically taken before processing.
}

static void handle_uart_stream(void) {
    static uint8_t buf[512];
    for (uint8_t ch = 0; ch < 2; ch++) {
        if (!uart_stream_enabled(ch)) continue;
        uint16_t n = uart_rx_avail(ch);
        if (n == 0) continue;
        if (n > sizeof(buf)) n = sizeof(buf);
        n = uart_rx_read(ch, buf, n);
        if (n) {
            if (g_mode == MODE_TEXT) evt_uart_rx(ch, buf, n);
            else bin_evt_uart_rx(ch, buf, n);
        }
    }
    for (uint8_t ch = 0; ch < 2; ch++) {
        uint16_t rx, tx;
        uint32_t errs;
        if (uart_stat(ch, &rx, &tx, &errs) != 0) continue;
        if (errs != uart_last_errs[ch]) {
            uint32_t dropped = errs - uart_last_errs[ch];
            uart_last_errs[ch] = errs;
            if (g_mode == MODE_TEXT) evt_uart_overrun(ch, dropped);
            else bin_evt_uart_overrun(ch, dropped);
        }
    }
}

static uint16_t adc_data_buf[128];
static uint8_t adc_data_payload[2 + 128 * 2];
static uint8_t adc_was_active;
static uint8_t adc_last_ch;

// Fast uint16 -> decimal (sprintf is far too slow per-sample for streaming).
static char *u16toa(char *p, uint16_t v) {
    char tmp[6];
    int i = 0;
    do { tmp[i++] = (char)('0' + v % 10); v = (uint16_t)(v / 10); } while (v);
    while (i) *p++ = tmp[--i];
    return p;
}

static void handle_adc_stream(void) {
    uint8_t active = (uint8_t)adc_sample_active();
    uint8_t ch = adc_stream_channel();
    if (active) {
        // Leave samples in the ADC ring when USB is backpressured.
        if (ring_free(&g_tx) < (g_mode == MODE_TEXT ? 900u : 268u)) return;
        uint16_t n = adc_stream_poll(adc_data_buf, 128);
        if (n) {
            if (g_mode == MODE_TEXT) {
                static char lbuf[900];
                char *p = lbuf;
                *p++ = 'D'; *p++ = 'A'; *p++ = 'T'; *p++ = 'A'; *p++ = ' ';
                *p++ = 'A'; *p++ = 'D'; *p++ = 'C'; *p++ = (char)('0' + ch);
                *p++ = ' ';
                for (uint16_t i = 0; i < n; i++) {
                    if (i) *p++ = ',';
                    p = u16toa(p, adc_data_buf[i]);
                }
                *p++ = '\n';
                *p = '\0';
                tputs(lbuf);
            } else {
                adc_data_payload[0] = (uint8_t)(n & 0xFF);
                adc_data_payload[1] = (uint8_t)((n >> 8) & 0xFF);
                for (uint16_t i = 0; i < n; i++) {
                    adc_data_payload[2 + i * 2] = (uint8_t)(adc_data_buf[i] & 0xFF);
                    adc_data_payload[3 + i * 2] = (uint8_t)((adc_data_buf[i] >> 8) & 0xFF);
                }
                bin_send(BIN_TYPE_DATA, 0x62, 0, adc_data_payload, (uint16_t)(2 + n * 2));
            }
        }
        adc_was_active = 1;
        adc_last_ch = ch;
    } else if (adc_was_active || adc_stream_end_pending()) {
        adc_last_ch = ch;
        uint32_t lost = adc_stream_dropped();
        if (lost) {
            if (g_mode == MODE_TEXT) tprintf("EVT ADC_OVERRUN ADC%u %lu\n", adc_last_ch, (unsigned long)lost);
            else {
                uint8_t p[5] = {adc_last_ch, lost, lost >> 8, lost >> 16, lost >> 24};
                bin_send(BIN_TYPE_EVT, 0x66, 0, p, sizeof(p));
            }
        }
        if (g_mode == MODE_TEXT) {
            tprintf("DATA_END ADC%u\n", adc_last_ch);
        } else {
            uint32_t sent = adc_stream_sent();
            uint8_t p[4] = {sent, sent >> 8, sent >> 16, sent >> 24};
            bin_send(BIN_TYPE_DATA_END, 0x62, 0, p, 4);
        }
        adc_was_active = 0;
        adc_stream_ack_end();
    }
}

static void handle_adc_thresh(void) {
    if (!adc_thresh_event) return;
    adc_thresh_event = 0;
    if (g_mode == MODE_TEXT) evt_adc_thresh(adc_thresh_ev_ch, adc_thresh_ev_dir, adc_thresh_ev_mv);
    else bin_evt_adc_thresh(adc_thresh_ev_ch, adc_thresh_ev_dir, adc_thresh_ev_mv);
}

static void handle_seq_done(void) {
    char name[16];
    if (!seq_take_done(name, sizeof(name))) return;
    int error = seq_last_error();
    if (error) {
        if (g_mode == MODE_TEXT) tprintf("EVT SEQ_ERROR %s %s\n", name, err_str[error]);
        else {
            uint8_t p[18];
            size_t n = strlen(name);
            p[0] = (uint8_t)error;
            p[1] = (uint8_t)n;
            memcpy(p + 2, name, n);
            bin_send(BIN_TYPE_EVT, 0x78, 0, p, (uint16_t)(n + 2));
        }
        return;
    }
    if (g_mode == MODE_TEXT) evt_seq_done(name);
    else bin_evt_seq_done(name);
}

// 1 Hz "callback": fire EVT PWM_TICK whenever the PWM tick counter advances.
static uint32_t last_tick = 0;
static void handle_pwm_tick(void) {
    uint32_t t = pwm_tick_count();
    if (t == last_tick) return;
    last_tick = t;
    if (g_mode == MODE_TEXT) {
        tprintf("EVT PWM_TICK %lu\n", (unsigned long)t);
    } else {
        uint8_t p[4];
        p[0] = (uint8_t)(t & 0xFF);
        p[1] = (uint8_t)((t >> 8) & 0xFF);
        p[2] = (uint8_t)((t >> 16) & 0xFF);
        p[3] = (uint8_t)((t >> 24) & 0xFF);
        bin_send(BIN_TYPE_EVT, 0x2B, 0, p, 4);
    }
}

static void handle_tx_overrun(void) {
    static uint32_t reported;
    uint32_t count = g_tx_dropped;
    if (count == reported || ring_free(&g_tx) < 128) return;
    uint32_t lost = count - reported;
    reported = count;
    if (g_mode == MODE_TEXT) tprintf("EVT TX_OVERRUN %lu\n", (unsigned long)lost);
    else {
        uint8_t p[4] = {lost, lost >> 8, lost >> 16, lost >> 24};
        bin_send(BIN_TYPE_EVT, 0x07, 0, p, sizeof(p));
    }
}

// ---------- main ----------

int main(void) {
    board_serial_init();
    drivers_init();
    seq_init();
    config_restore(); // auto-apply saved parameters if present
    multicore_launch_core1(seq_core1_entry);
    uint64_t core_deadline = time_us_64() + 1000000u;
    while (!seq_core1_ready() && time_us_64() < core_deadline) tight_loop_contents();
    tusb_init();

    for (;;) {
        tud_task();
        if (tud_cdc_connected()) usb_input();
        seq_poll_core0();
        drivers_poll();
        handle_gpio_events();
        handle_uart_stream();
        handle_adc_stream();
        handle_adc_thresh();
        handle_seq_done();
        handle_pwm_tick();
        handle_tx_overrun();
        // Pump the TX ring to the USB as fast as possible: alternate flush +
        // tud_task so a continuous stream (e.g. ADC) isn't throttled to one
        // USB IN transaction per main-loop iteration.
        if (tud_cdc_connected() && ring_avail(&g_tx)) {
            uint32_t spins = 0;
            while (ring_avail(&g_tx) && spins < 16) {
                flush_tx();
                tud_task();
                spins++;
            }
        }
    }
}
