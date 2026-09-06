#ifndef DRIVERS_H
#define DRIVERS_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "protocol.h"

// ---- Initialization / background tasks ----
void drivers_init(void);
void drivers_poll(void);   // drive sweeps, pulses, ADC stream; call from main loop

// ---- IO ----
int io_configure(uint8_t ch, uint8_t dir, uint8_t pull); // dir:0=out,1=in; pull:0=float,1=up,2=down
int io_write(uint8_t ch, uint8_t level);                 // 0=LOW,1=HIGH
int io_read(uint8_t ch, uint8_t *level);
void io_readall(uint8_t levels[IO_COUNT]);
void io_cfg_get(uint8_t ch, uint8_t *dir, uint8_t *pull); // for persistence
int io_toggle(uint8_t ch);
int io_pulse(uint8_t ch, uint8_t level, uint16_t ms);
int io_event_set(uint8_t ch, uint8_t mode);              // 0=off,1=rising,2=falling,3=change
uint32_t io_events_pending(void);                        // bit per IO
void io_events_clear(uint32_t mask);

// ---- PWM ----
int pwm_cfg(uint8_t ch, uint32_t freq, uint16_t duty);   // duty raw 0..65535
int pwm_freq(uint8_t ch, uint32_t freq);
int pwm_duty(uint8_t ch, uint16_t duty);
int pwm_start(uint8_t ch);
int pwm_stop(uint8_t ch);
int pwm_sweep(uint8_t ch, uint16_t from, uint16_t to, uint16_t step_ms, uint16_t repeat); // duty raw
int pwm_read(uint8_t ch, uint32_t *freq, uint16_t *duty, uint8_t *running, uint8_t *inv);
int pwm_sync(const uint8_t *chs, uint8_t n);
int pwm_phase(uint8_t ch, uint8_t ref, uint8_t unit, uint16_t val); // unit 0=deg,1=ticks
int pwm_phadj(uint8_t ch, int16_t delta);
int pwm_pol(uint8_t ch, uint8_t inv);
int pwm_tick_set(uint8_t ch, uint32_t freq); // fire EVT PWM_TICK each period; pin outputs a real freq-Hz square wave
int pwm_tick_off(void);
uint32_t pwm_tick_count(void);

// ---- UART ----
int uart_cfg(uint8_t ch, uint32_t baud, uint8_t databits, uint8_t parity, uint8_t stopbits);
int uart_cfg_get(uint8_t ch, uint32_t *baud, uint8_t *dbits, uint8_t *parity, uint8_t *sbits); // for persistence
int uart_tx(uint8_t ch, const uint8_t *data, uint16_t len);
uint16_t uart_rx_avail(uint8_t ch);
uint16_t uart_rx_read(uint8_t ch, uint8_t *dst, uint16_t max);
void uart_rx_flush(uint8_t ch);
int uart_stream(uint8_t ch, uint8_t on);
uint8_t uart_stream_enabled(uint8_t ch);
int uart_stat(uint8_t ch, uint16_t *rx, uint16_t *tx, uint32_t *errs);

// ---- I2C ----
int i2c_set_rate(uint32_t khz);
uint32_t i2c_get_rate(void);
int i2c_scan(uint8_t *addrs, uint8_t *count);
int i2c_write_reg(uint8_t addr, uint8_t reg, const uint8_t *data, uint16_t n);
int i2c_write_only(uint8_t addr, const uint8_t *data, uint16_t n);
int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *dst, uint16_t n);
int i2c_read_only(uint8_t addr, uint8_t *dst, uint16_t n);

// ---- SPI ----
int spi_cfg(uint32_t baud, uint8_t mode, uint8_t msb_first, uint8_t bits);
int spi_cfg_get(uint32_t *baud, uint8_t *mode, uint8_t *msb, uint8_t *bits); // for persistence
int spi_xfr(const uint8_t *tx, uint8_t *rx, uint16_t n);
int spi_write(const uint8_t *tx, uint16_t n);
int spi_read(uint8_t *rx, uint16_t n);
int spi_cs(uint8_t mode); // 0=auto,1=low,2=high

// ---- ADC ----
int adc_read_ch(uint8_t ch, uint16_t *raw, uint16_t *mv);
int adc_readall(uint16_t raw[ADC_COUNT], uint16_t mv[ADC_COUNT]);
uint32_t adc_stream_dropped(void);
uint32_t adc_stream_sent(void);
int adc_stream_end_pending(void);
void adc_stream_ack_end(void);
int adc_temp(int32_t *mdegc);
int adc_sample_start(uint8_t ch, uint32_t rate, uint32_t ms);
int adc_sample_stop(uint8_t ch);
int adc_sample_active(void);
uint8_t adc_stream_channel(void);
uint16_t adc_stream_poll(uint16_t *out, uint16_t max); // 0 if inactive; else new-sample count
void adc_stream_dbg(uint32_t *sent, uint32_t *expected, uint8_t *streaming, uint16_t *ring_head, uint16_t *ring_tail);
int adc_thresh(uint8_t ch, uint16_t low_mv, uint16_t high_mv, uint8_t on);

// ADC threshold crossing event (set by drivers_poll, cleared by main loop)
extern volatile uint8_t adc_thresh_event;
extern uint8_t adc_thresh_ev_ch;
extern uint8_t adc_thresh_ev_dir; // 0=BELOW, 1=ABOVE
extern uint16_t adc_thresh_ev_mv;

// ---- NTP-style time sync ----
// synced_us = time_us_64() + offset_us. The host periodically sends SYNC,
// computes the round-trip offset and applies it via time_sync_set_offset().
void time_sync_set_offset(int64_t offset_us);
int64_t time_sync_offset(void);
int64_t time_synced_us(void);

#endif
