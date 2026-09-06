// Peripheral drivers for the IO Dock. Implements the operations behind the
// text and binary command parsers. All long-running / blocking bus operations
// (I2C, SPI, UART TX) run on core 0's command loop; time-critical work that
// must not block USB (sweeps, pulses, ADC streaming) is stepped by drivers_poll().

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "drivers.h"

// ============================= IO ==========================================

static uint8_t io_dir[IO_COUNT];      // 0=out, 1=in
static uint8_t io_pull_cfg[IO_COUNT]; // 0=float, 1=up, 2=down (for persistence)
static uint8_t io_evt_mode[IO_COUNT]; // 0=off, 1=rising, 2=falling, 3=change
static volatile uint32_t io_pending;  // bit per IO, set in ISR
static uint8_t io_pulse_active[IO_COUNT];
static uint8_t io_pulse_level[IO_COUNT];
static uint64_t io_pulse_end_us[IO_COUNT];

static void io_irq_cb(uint gpio, uint32_t events) {
    (void)events;
    for (uint8_t i = 0; i < IO_COUNT; i++) {
        if (IO_GPIO_BASE + i == gpio) {
            io_pending |= (1u << i);
            return;
        }
    }
}

int io_configure(uint8_t ch, uint8_t dir, uint8_t pull) {
    if (ch >= IO_COUNT || dir > 1 || pull > 2) return E_PARAM;
    io_pulse_active[ch] = 0;
    uint8_t g = IO_GPIO_BASE + ch;
    io_dir[ch] = dir;
    io_pull_cfg[ch] = pull;
    gpio_set_function(g, GPIO_FUNC_SIO); // without SIO, gpio_put has no effect
    if (pull == 1)      gpio_pull_up(g);
    else if (pull == 2) gpio_pull_down(g);
    else                gpio_disable_pulls(g);
    if (dir == 0) {
        gpio_put(g, 0);
        gpio_set_dir(g, GPIO_OUT);
    } else {
        gpio_set_dir(g, GPIO_IN);
    }
    return 0;
}

int io_write(uint8_t ch, uint8_t level) {
    if (ch >= IO_COUNT || level > 1) return E_PARAM;
    if (io_dir[ch] != 0) return E_CFG; // must be output
    io_pulse_active[ch] = 0;           // cancel any pending pulse restore
    gpio_put(IO_GPIO_BASE + ch, level);
    return 0;
}

int io_read(uint8_t ch, uint8_t *level) {
    if (ch >= IO_COUNT) return E_PARAM;
    *level = gpio_get(IO_GPIO_BASE + ch) ? 1 : 0;
    return 0;
}

void io_readall(uint8_t levels[IO_COUNT]) {
    for (uint8_t i = 0; i < IO_COUNT; i++) levels[i] = gpio_get(IO_GPIO_BASE + i) ? 1 : 0;
}

void io_cfg_get(uint8_t ch, uint8_t *dir, uint8_t *pull) {
    if (ch >= IO_COUNT) { if (dir) *dir = 1; if (pull) *pull = 0; return; }
    if (dir) *dir = io_dir[ch];
    if (pull) *pull = io_pull_cfg[ch];
}

int io_toggle(uint8_t ch) {
    if (ch >= IO_COUNT) return E_PARAM;
    if (io_dir[ch] != 0) return E_CFG;
    io_pulse_active[ch] = 0;
    gpio_put(IO_GPIO_BASE + ch, !gpio_get(IO_GPIO_BASE + ch));
    return 0;
}

int io_pulse(uint8_t ch, uint8_t level, uint16_t ms) {
    if (ch >= IO_COUNT || level > 1 || ms == 0) return E_PARAM;
    if (io_dir[ch] != 0) return E_CFG;
    gpio_put(IO_GPIO_BASE + ch, level);
    io_pulse_active[ch] = 1;
    io_pulse_level[ch] = level;
    io_pulse_end_us[ch] = time_us_64() + (uint64_t)ms * 1000u;
    return 0;
}

int io_event_set(uint8_t ch, uint8_t mode) {
    if (ch >= IO_COUNT || mode > 3) return E_PARAM;
    io_evt_mode[ch] = mode;
    uint32_t mask = 0;
    switch (mode) {
        case 1: mask = GPIO_IRQ_EDGE_RISE; break;
        case 2: mask = GPIO_IRQ_EDGE_FALL; break;
        case 3: mask = GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL; break;
        default: mask = 0; break;
    }
    gpio_set_irq_enabled(IO_GPIO_BASE + ch, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    gpio_acknowledge_irq(IO_GPIO_BASE + ch, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    if (mask) gpio_set_irq_enabled(IO_GPIO_BASE + ch, mask, true);
    // gpio_set_irq_enabled() does NOT enable the IO_IRQ_BANK0 vector (only
    // gpio_set_irq_enabled_with_callback does), so enable it explicitly here.
    if (mask != 0) irq_set_enabled(IO_IRQ_BANK0, true);
    return 0;
}

uint32_t io_events_pending(void) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t pending = io_pending;
    io_pending = 0;
    restore_interrupts(irq);
    return pending;
}

void io_events_clear(uint32_t mask) {
    uint32_t irq = save_and_disable_interrupts();
    io_pending &= ~mask;
    restore_interrupts(irq);
}

// ============================= PWM ==========================================

static uint32_t pwm_freqs[PWM_COUNT];
static uint16_t pwm_duties[PWM_COUNT]; // raw 0..65535
static uint8_t pwm_run[PWM_COUNT];
static uint8_t pwm_inv[PWM_COUNT];

typedef struct {
    uint8_t active;
    int32_t cur;
    int32_t target;
    int32_t from;
    uint16_t step_ms;
    uint16_t repeat;
    uint64_t next_us;
} sweep_t;
static sweep_t sweep[PWM_COUNT];
static bool pwm_tick_owns_slice(uint8_t slice);

static uint32_t pwm_compute(uint32_t freq, uint32_t *top, uint32_t *div_i, uint32_t *div_f) {
    if (freq == 0) return E_RANGE;
    uint64_t period = (uint64_t)clock_get_hz(clk_sys) / freq; // = div * (top+1)
    if (period < 2) return E_RANGE;
    // Reserve CC=TOP+1 for an exact 100% output (CC is only 16 bits).
    if (period <= 65535u) {
        *top = (uint32_t)period - 1;
        *div_i = 1;
        *div_f = 0;
    } else {
        uint32_t di = (uint32_t)((period + 65534u) / 65535u);
        if (di > 255u) return E_RANGE;
        *div_i = di;
        *top = (uint32_t)(period / di) - 1;
        *div_f = 0;
    }
    return 0;
}

static uint16_t pwm_duty_to_cc(uint32_t top, uint16_t duty) {
    uint32_t cc = ((uint32_t)duty * (top + 1u)) / 65535u;
    if (cc > 65535u) cc = 65535u;
    return (uint16_t)cc;
}

static void pwm_apply_polarity(uint8_t slice) {
    uint8_t a = 0, b = 0;
    for (uint8_t ch = 0; ch < PWM_COUNT; ch++) {
        if (PWM_MAP[ch].slice == slice) {
            if (PWM_MAP[ch].chan == PWM_CHAN_A) a = pwm_inv[ch];
            else b = pwm_inv[ch];
        }
    }
    pwm_set_output_polarity(slice, a, b);
}

static void pwm_apply_channel(uint8_t ch) {
    uint8_t slice = PWM_MAP[ch].slice;
    pwm_set_chan_level(slice, PWM_MAP[ch].chan,
        pwm_run[ch] ? pwm_duty_to_cc(pwm_hw->slice[slice].top, pwm_duties[ch]) : 0);
    // Force stopped pins low, even with inverted polarity or a stopped counter.
    gpio_set_outover(PWM_MAP[ch].gpio, pwm_run[ch] ? GPIO_OVERRIDE_NORMAL : GPIO_OVERRIDE_LOW);
}

// Re-apply frequency/divider/top to a slice and refresh all channels sharing it
// (PWM2/PWM3 share slice 4 -> they must run at the same frequency).
// Re-apply the frequency of `freq_ch` to its slice and refresh all channels
// sharing it. freq_ch is passed explicitly: two channels share slice 4
// (PWM2/PWM3), so blindly using "the first channel on the slice" would pick a
// channel that may not have been configured yet.
static void pwm_recalc_slice(uint8_t slice, uint8_t freq_ch) {
    uint32_t freq = pwm_freqs[freq_ch];
    if (freq == 0) return;
    uint32_t top, di, df;
    if (pwm_compute(freq, &top, &di, &df) != 0) return;
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac(&cfg, di, df);
    pwm_config_set_wrap(&cfg, top);
    bool was_on = false;
    pwm_init(slice, &cfg, false);
    for (uint8_t ch = 0; ch < PWM_COUNT; ch++) {
        if (PWM_MAP[ch].slice != slice) continue;
        pwm_freqs[ch] = freq;
        was_on |= pwm_run[ch] != 0;
        sweep[ch].active = 0; // freq changed, cancel sweep
        pwm_apply_channel(ch);
    }
    if (was_on) pwm_set_enabled(slice, true);
    pwm_apply_polarity(slice);
}

int pwm_cfg(uint8_t ch, uint32_t freq, uint16_t duty) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) return E_BUSY;
    uint32_t top, di, df;
    int rc = pwm_compute(freq, &top, &di, &df);
    if (rc) return rc;
    pwm_freqs[ch] = freq;
    pwm_duties[ch] = duty;
    pwm_recalc_slice(PWM_MAP[ch].slice, ch);
    return 0;
}

int pwm_freq(uint8_t ch, uint32_t freq) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) return E_BUSY;
    uint32_t top, di, df;
    int rc = pwm_compute(freq, &top, &di, &df);
    if (rc) return rc;
    // frequency is per-slice: update all channels on this slice
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        if (PWM_MAP[i].slice == PWM_MAP[ch].slice) pwm_freqs[i] = freq;
    }
    pwm_recalc_slice(PWM_MAP[ch].slice, ch);
    return 0;
}

int pwm_duty(uint8_t ch, uint16_t duty) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) return E_BUSY;
    sweep[ch].active = 0;
    pwm_duties[ch] = duty;
    pwm_apply_channel(ch);
    return 0;
}

int pwm_start(uint8_t ch) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (!pwm_freqs[ch]) return E_CFG;
    if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) return E_BUSY;
    pwm_run[ch] = 1;
    pwm_apply_channel(ch);
    pwm_set_enabled(PWM_MAP[ch].slice, true);
    return 0;
}

int pwm_stop(uint8_t ch) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) pwm_tick_off();
    sweep[ch].active = 0;
    pwm_run[ch] = 0;
    uint8_t slice = PWM_MAP[ch].slice;
    uint8_t sibling_run = 0;
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        if (i != ch && PWM_MAP[i].slice == slice && pwm_run[i]) sibling_run = 1;
    }
    pwm_apply_channel(ch);
    if (!sibling_run) pwm_set_enabled(slice, false);
    return 0;
}

int pwm_sweep(uint8_t ch, uint16_t from, uint16_t to, uint16_t step_ms, uint16_t repeat) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) return E_BUSY;
    if (!pwm_freqs[ch]) return E_CFG;
    if (from == to || step_ms == 0) return E_PARAM;
    sweep[ch].active = 1;
    sweep[ch].from = from;
    sweep[ch].cur = from;
    sweep[ch].target = to;
    sweep[ch].step_ms = step_ms;
    sweep[ch].repeat = repeat;
    sweep[ch].next_us = time_us_64() + (uint64_t)step_ms * 1000u;
    pwm_duties[ch] = from;
    pwm_apply_channel(ch);
    return 0;
}

int pwm_read(uint8_t ch, uint32_t *freq, uint16_t *duty, uint8_t *running, uint8_t *inv) {
    if (ch >= PWM_COUNT) return E_PARAM;
    *freq = pwm_freqs[ch];
    *duty = pwm_duties[ch];
    *running = pwm_run[ch];
    *inv = pwm_inv[ch];
    return 0;
}

int pwm_sync(const uint8_t *chs, uint8_t n) {
    if (!chs || n == 0 || n > PWM_COUNT) return E_PARAM;
    uint32_t mask = 0;
    uint32_t freq = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t ch = chs[i];
        if (ch >= PWM_COUNT) return E_PARAM;
        if (!pwm_freqs[ch]) return E_CFG;
        if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) return E_BUSY;
        if (freq == 0) freq = pwm_freqs[ch];
        else if (freq != pwm_freqs[ch]) return E_CFG; // must be same frequency
        mask |= (1u << PWM_MAP[ch].slice);
    }
    // zero all involved counters, then start them together. Set only the EN bits
    // for the involved slices (do NOT write the whole EN register via
    // pwm_set_mask_enabled, which would disable every other running slice — e.g.
    // the PWM TICK channel driving a 1 Hz output on its own slice).
    hw_clear_bits(&pwm_hw->en, mask);
    for (uint8_t i = 0; i < n; i++) {
        pwm_set_counter(PWM_MAP[chs[i]].slice, 0);
        pwm_run[chs[i]] = 1;
        pwm_apply_channel(chs[i]);
    }
    hw_set_bits(&pwm_hw->en, mask);
    for (uint8_t i = 0; i < n; i++) pwm_run[chs[i]] = 1;
    return 0;
}

int pwm_phase(uint8_t ch, uint8_t ref, uint8_t unit, uint16_t val) {
    if (ch >= PWM_COUNT || ref >= PWM_COUNT) return E_PARAM;
    if (unit > 1 || (unit == 0 && val > 360)) return E_PARAM;
    if (ch == ref) return E_CFG;
    uint8_t s_ch = PWM_MAP[ch].slice, s_ref = PWM_MAP[ref].slice;
    if (s_ch == s_ref) return E_CFG; // same slice: phase locked, not adjustable
    if (!pwm_freqs[ch]) return E_CFG;
    if (pwm_tick_owns_slice(s_ch) || pwm_tick_owns_slice(s_ref)) return E_BUSY;
    if (pwm_freqs[ch] != pwm_freqs[ref]) return E_CFG;
    uint32_t top = pwm_hw->slice[s_ch].top;
    uint32_t ticks;
    if (unit == 0) ticks = ((uint32_t)val * (top + 1u)) / 360u;
    else ticks = val;
    if (ticks > top + 1u) return E_RANGE;
    ticks %= top + 1u;
    hw_clear_bits(&pwm_hw->en, (1u << s_ch) | (1u << s_ref));
    pwm_run[ch] = pwm_run[ref] = 1;
    pwm_apply_channel(ch);
    pwm_apply_channel(ref);
    pwm_set_counter(s_ch, ticks);
    pwm_set_counter(s_ref, 0);
    hw_set_bits(&pwm_hw->en, (1u << s_ch) | (1u << s_ref)); // enable, don't clobber other slices
    pwm_run[ch] = 1;
    pwm_run[ref] = 1;
    return 0;
}

int pwm_phadj(uint8_t ch, int16_t delta) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) return E_BUSY;
    if (!pwm_run[ch]) return E_CFG;
    uint8_t slice = PWM_MAP[ch].slice;
    if (!(pwm_hw->slice[slice].csr & PWM_CH0_CSR_EN_BITS)) return E_CFG; // must be running
    uint32_t div = pwm_hw->slice[slice].div;
    uint32_t div_int = (div & PWM_CH0_DIV_INT_BITS) >> PWM_CH0_DIV_INT_LSB;
    uint32_t div_frac = (div & PWM_CH0_DIV_FRAC_BITS) >> PWM_CH0_DIV_FRAC_LSB;
    int32_t rem = delta;
    while (rem != 0) {
        if (rem > 0) {
            if (div_int == 1 && div_frac == 0) return E_DENIED; // divider must be > 1 to advance
            // set the bit without clearing EN/other CSR fields (a plain '=' would disable the slice)
            hw_set_bits(&pwm_hw->slice[slice].csr, PWM_CH0_CSR_PH_ADV_BITS);
            uint64_t deadline = time_us_64() + 1000u;
            while (pwm_hw->slice[slice].csr & PWM_CH0_CSR_PH_ADV_BITS) {
                if (time_us_64() >= deadline) return E_TIMEOUT;
                tight_loop_contents();
            }
            rem--;
        } else {
            hw_set_bits(&pwm_hw->slice[slice].csr, PWM_CH0_CSR_PH_RET_BITS);
            uint64_t deadline = time_us_64() + 1000u;
            while (pwm_hw->slice[slice].csr & PWM_CH0_CSR_PH_RET_BITS) {
                if (time_us_64() >= deadline) return E_TIMEOUT;
                tight_loop_contents();
            }
            rem++;
        }
    }
    return 0;
}

int pwm_pol(uint8_t ch, uint8_t inv) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (pwm_tick_owns_slice(PWM_MAP[ch].slice)) return E_BUSY;
    pwm_inv[ch] = inv ? 1 : 0;
    pwm_apply_polarity(PWM_MAP[ch].slice);
    return 0;
}

// ============================= UART =========================================

static uart_inst_t *const uarts[2] = { uart0, uart1 };
static const uint8_t uart_tx_gpio[2] = { USART0_TX_GPIO, USART1_TX_GPIO };
static const uint8_t uart_rx_gpio[2] = { USART0_RX_GPIO, USART1_RX_GPIO };
static ring_t uart_rx[2];
static volatile uint32_t uart_errs[2];
static uint8_t uart_stream_on[2];
typedef struct { uint32_t baud; uint8_t dbits, parity, sbits; } uart_cfg_t;
static uart_cfg_t uart_cfg_state[2]; // for persistence

static void uart_drain(uart_inst_t *u, uint8_t idx) {
    while (uart_is_readable(u)) {
        uint32_t dr = uart_get_hw(u)->dr;
        uint8_t b = (uint8_t)dr;
        if (dr & UART_UARTDR_OE_BITS) uart_errs[idx]++;
        if (ring_full(&uart_rx[idx])) uart_errs[idx]++;
        else ring_put_byte(&uart_rx[idx], b);
    }
    uart_get_hw(u)->rsr = 0; // clear sticky hardware receive errors (ECR alias)
}

static void uart0_irq(void) { uart_drain(uart0, 0); }
static void uart1_irq(void) { uart_drain(uart1, 1); }

int uart_cfg(uint8_t ch, uint32_t baud, uint8_t databits, uint8_t parity, uint8_t stopbits) {
    if (ch >= 2) return E_PARAM;
    if (baud < 1200 || baud > 7800000u) return E_RANGE;
    if (databits < 5 || databits > 8) return E_PARAM;
    if (parity > 2 || stopbits < 1 || stopbits > 2) return E_PARAM;
    // Host protocol parity codes (Host_Protocol.md §5): 0=NONE, 1=ODD, 2=EVEN.
    // The SDK enum is NONE=0, EVEN=1, ODD=2, so remap to avoid ODD/EVEN being
    // silently swapped on the wire.
    static const uart_parity_t parity_sdk[3] = {
        UART_PARITY_NONE, UART_PARITY_ODD, UART_PARITY_EVEN
    };
    uart_inst_t *u = uarts[ch];
    uart_init(u, baud);
    gpio_set_function(uart_tx_gpio[ch], GPIO_FUNC_UART);
    gpio_set_function(uart_rx_gpio[ch], GPIO_FUNC_UART);
    uart_set_format(u, databits, stopbits, parity_sdk[parity]);
    uart_set_irq_enables(u, true, false);
    uart_cfg_state[ch].baud = baud;
    uart_cfg_state[ch].dbits = databits;
    uart_cfg_state[ch].parity = parity;
    uart_cfg_state[ch].sbits = stopbits;
    return 0;
}

int uart_cfg_get(uint8_t ch, uint32_t *baud, uint8_t *dbits, uint8_t *parity, uint8_t *sbits) {
    if (ch >= 2) return E_PARAM;
    if (baud)  *baud  = uart_cfg_state[ch].baud;
    if (dbits) *dbits = uart_cfg_state[ch].dbits;
    if (parity)*parity = uart_cfg_state[ch].parity;
    if (sbits) *sbits = uart_cfg_state[ch].sbits;
    return 0;
}

int uart_tx(uint8_t ch, const uint8_t *data, uint16_t len) {
    if (ch >= 2) return E_PARAM;
    uart_write_blocking(uarts[ch], data, len);
    return 0;
}

uint16_t uart_rx_avail(uint8_t ch) {
    if (ch >= 2) return 0;
    return ring_avail(&uart_rx[ch]);
}

uint16_t uart_rx_read(uint8_t ch, uint8_t *dst, uint16_t max) {
    if (ch >= 2) return 0;
    uint16_t a = ring_avail(&uart_rx[ch]);
    if (a > max) a = max;
    ring_get(&uart_rx[ch], dst, a);
    return a;
}

void uart_rx_flush(uint8_t ch) {
    if (ch >= 2) return;
    ring_drop(&uart_rx[ch], ring_avail(&uart_rx[ch]));
}

int uart_stream(uint8_t ch, uint8_t on) {
    if (ch >= 2) return E_PARAM;
    uart_stream_on[ch] = on ? 1 : 0;
    return 0;
}

uint8_t uart_stream_enabled(uint8_t ch) {
    return ch < 2 ? uart_stream_on[ch] : 0;
}

int uart_stat(uint8_t ch, uint16_t *rx, uint16_t *tx, uint32_t *errs) {
    if (ch >= 2) return E_PARAM;
    *rx = ring_avail(&uart_rx[ch]);
    *tx = 0; // TX is fire-and-forget through the hardware FIFO
    *errs = uart_errs[ch];
    return 0;
}

// ============================= I2C ==========================================

static uint32_t g_i2c_khz = 400;

int i2c_set_rate(uint32_t khz) {
    if (khz != 100 && khz != 400 && khz != 1000) return E_RANGE;
    g_i2c_khz = khz;
    i2c_set_baudrate(i2c0, khz * 1000u);
    return 0;
}

uint32_t i2c_get_rate(void) {
    return g_i2c_khz;
}

int i2c_scan(uint8_t *addrs, uint8_t *count) {
    *count = 0;
    uint8_t probe = 0;
    // The SDK rejects len==0 writes, so probe with a single byte: a device at
    // this address ACKs the address byte -> exactly 1 byte transferred.
    for (uint8_t a = 8; a <= 119; a++) {
        if (i2c_write_blocking_until(i2c0, a, &probe, 1, false, make_timeout_time_ms(10)) == 1) {
            addrs[(*count)++] = a;
        }
    }
    return 0;
}

static int i2c_last_rc(int rc) {
    if (rc == PICO_ERROR_TIMEOUT) return E_TIMEOUT;
    return E_NACK;
}

int i2c_write_reg(uint8_t addr, uint8_t reg, const uint8_t *data, uint16_t n) {
    uint8_t buf[260];
    if (n > sizeof(buf) - 1) return E_PARAM;
    buf[0] = reg;
    memcpy(buf + 1, data, n);
    int rc = i2c_write_blocking_until(i2c0, addr, buf, n + 1, false, make_timeout_time_ms(100));
    return rc == (int)(n + 1) ? 0 : i2c_last_rc(rc);
}

int i2c_write_only(uint8_t addr, const uint8_t *data, uint16_t n) {
    if (n == 0 || n > 255) return E_PARAM;
    int rc = i2c_write_blocking_until(i2c0, addr, data, n, false, make_timeout_time_ms(100));
    return rc == (int)n ? 0 : i2c_last_rc(rc);
}

int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *dst, uint16_t n) {
    if (n == 0 || n > 255) return E_PARAM;
    int rc = i2c_write_blocking_until(i2c0, addr, &reg, 1, true, make_timeout_time_ms(100)); // nostop -> restart
    if (rc != 1) return i2c_last_rc(rc);
    rc = i2c_read_blocking_until(i2c0, addr, dst, n, false, make_timeout_time_ms(100));
    return rc == (int)n ? 0 : i2c_last_rc(rc);
}

int i2c_read_only(uint8_t addr, uint8_t *dst, uint16_t n) {
    if (n == 0 || n > 255) return E_PARAM;
    int rc = i2c_read_blocking_until(i2c0, addr, dst, n, false, make_timeout_time_ms(100));
    return rc == (int)n ? 0 : i2c_last_rc(rc);
}

// ============================= SPI ==========================================

static uint8_t spi_cs_mode = 0; // 0=auto, 1=low, 2=high
static uint8_t spi_data_bits = 8;
static uint32_t spi_baud_cfg = 0; // for persistence
static uint8_t  spi_mode_cfg = 0, spi_msb_cfg = 1;

static void spi_cs_begin(void) {
    if (spi_cs_mode == 0) gpio_put(SPI0_CS_GPIO, 0);
}
static void spi_cs_end(void) {
    if (spi_cs_mode == 0) gpio_put(SPI0_CS_GPIO, 1);
}

int spi_cfg(uint32_t baud, uint8_t mode, uint8_t msb_first, uint8_t bits) {
    if (mode > 3) return E_RANGE;
    if (bits < 4 || bits > 16) return E_RANGE;
    if (msb_first > 1) return E_PARAM;
    uint32_t peri = clock_get_hz(clk_peri);
    if (baud < (peri + 65023u) / 65024u || baud > peri / 2u) return E_RANGE;
    spi_data_bits = bits;
    spi_baud_cfg = baud; spi_mode_cfg = mode; spi_msb_cfg = msb_first;
    spi_deinit(spi0);
    spi_init(spi0, baud);
    gpio_set_function(SPI0_MOSI_GPIO, GPIO_FUNC_SPI);
    gpio_set_function(SPI0_MISO_GPIO, GPIO_FUNC_SPI);
    gpio_set_function(SPI0_SCK_GPIO, GPIO_FUNC_SPI);
    gpio_set_function(SPI0_CS_GPIO, GPIO_FUNC_SIO);
    gpio_set_dir(SPI0_CS_GPIO, GPIO_OUT);
    gpio_put(SPI0_CS_GPIO, 1);
    spi_cpol_t cpol = (mode & 2) ? SPI_CPOL_1 : SPI_CPOL_0;
    spi_cpha_t cpha = (mode & 1) ? SPI_CPHA_1 : SPI_CPHA_0;
    // PL022 is MSB-only. Implement LSB order by reversing each software word.
    spi_set_format(spi0, bits, cpol, cpha, SPI_MSB_FIRST);
    spi_cs_mode = 0;
    return 0;
}

int spi_cfg_get(uint32_t *baud, uint8_t *mode, uint8_t *msb, uint8_t *bits) {
    if (baud) *baud = spi_baud_cfg;
    if (mode) *mode = spi_mode_cfg;
    if (msb)  *msb  = spi_msb_cfg;
    if (bits) *bits = spi_data_bits;
    return 0;
}

// 16-bit frame buffers (avoid unaligned access on Cortex-M0+)
static uint16_t spi_buf16_a[2048];
static uint16_t spi_buf16_b[2048];

static uint16_t spi_reverse(uint16_t v) {
    uint16_t r = 0;
    for (uint8_t i = 0; i < spi_data_bits; i++) { r = (uint16_t)((r << 1) | (v & 1)); v >>= 1; }
    return r;
}

int spi_xfr(const uint8_t *tx, uint8_t *rx, uint16_t n) {
    if (n == 0 || n > 4096 || (spi_data_bits > 8 && (n & 1))) return E_PARAM;
    spi_cs_begin();
    if (spi_data_bits > 8) {
        memcpy(spi_buf16_a, tx, n);
        uint16_t frames = n / 2;
        if (!spi_msb_cfg) for (uint16_t i = 0; i < frames; i++) spi_buf16_a[i] = spi_reverse(spi_buf16_a[i]);
        spi_write16_read16_blocking(spi0, spi_buf16_a, spi_buf16_b, frames);
        if (!spi_msb_cfg) for (uint16_t i = 0; i < frames; i++) spi_buf16_b[i] = spi_reverse(spi_buf16_b[i]);
        memcpy(rx, spi_buf16_b, frames * 2);
    } else {
        const uint8_t *src = tx;
        if (!spi_msb_cfg) {
            uint8_t *tmp = (uint8_t *)spi_buf16_a;
            for (uint16_t i = 0; i < n; i++) tmp[i] = (uint8_t)spi_reverse(tx[i]);
            src = tmp;
        }
        spi_write_read_blocking(spi0, src, rx, n);
        if (!spi_msb_cfg) for (uint16_t i = 0; i < n; i++) rx[i] = (uint8_t)spi_reverse(rx[i]);
    }
    spi_cs_end();
    return 0;
}

int spi_write(const uint8_t *tx, uint16_t n) {
    if (n == 0 || n > 4096 || (spi_data_bits > 8 && (n & 1))) return E_PARAM;
    spi_cs_begin();
    if (spi_data_bits > 8) {
        memcpy(spi_buf16_a, tx, n);
        if (!spi_msb_cfg) for (uint16_t i = 0; i < n / 2; i++) spi_buf16_a[i] = spi_reverse(spi_buf16_a[i]);
        spi_write16_blocking(spi0, spi_buf16_a, n / 2);
    } else {
        const uint8_t *src = tx;
        if (!spi_msb_cfg) {
            uint8_t *tmp = (uint8_t *)spi_buf16_a;
            for (uint16_t i = 0; i < n; i++) tmp[i] = (uint8_t)spi_reverse(tx[i]);
            src = tmp;
        }
        spi_write_blocking(spi0, src, n);
    }
    spi_cs_end();
    return 0;
}

int spi_read(uint8_t *rx, uint16_t n) {
    if (n == 0 || n > 4096 || (spi_data_bits > 8 && (n & 1))) return E_PARAM;
    spi_cs_begin();
    if (spi_data_bits > 8) {
        spi_read16_blocking(spi0, 0, spi_buf16_b, n / 2);
        if (!spi_msb_cfg) for (uint16_t i = 0; i < n / 2; i++) spi_buf16_b[i] = spi_reverse(spi_buf16_b[i]);
        memcpy(rx, spi_buf16_b, (n / 2) * 2);
    } else {
        spi_read_blocking(spi0, 0, rx, n);
        if (!spi_msb_cfg) for (uint16_t i = 0; i < n; i++) rx[i] = (uint8_t)spi_reverse(rx[i]);
    }
    spi_cs_end();
    return 0;
}

int spi_cs(uint8_t mode) {
    if (mode > 2) return E_PARAM;
    spi_cs_mode = mode;
    if (mode == 1) gpio_put(SPI0_CS_GPIO, 0);
    else gpio_put(SPI0_CS_GPIO, 1);
    return 0;
}

// ============================= ADC ==========================================

static volatile uint8_t adc_streaming;
static uint8_t adc_end_pending;
static uint8_t adc_stream_ch;
static uint32_t adc_expected;
static uint32_t adc_sent;
static volatile uint32_t adc_captured;
static volatile uint32_t adc_dropped;
static volatile uint8_t adc_capture_done;
static volatile uint8_t adc_deadline_reached;
static volatile uint32_t adc_fifo_overflows;
static alarm_id_t adc_stop_alarm;
static uint64_t adc_last_packet_us;
// Software ring filled by the ADC FIFO IRQ (no DMA, no ring-alignment issues).
#define ADC_SW_RING 2048u
static volatile uint16_t adc_ring[ADC_SW_RING];
static volatile uint16_t adc_ring_head;
static volatile uint16_t adc_ring_tail;

static void adc_fifo_irq(void) {
    if (adc_hw->fcs & ADC_FCS_OVER_BITS) {
        adc_fifo_overflows = 1; // Sticky indication: the exact lost count is unknowable.
        adc_hw->fcs = adc_hw->fcs | ADC_FCS_OVER_BITS; // direct W1C, not atomic SET alias
    }
    while (!adc_fifo_is_empty()) {
        uint16_t s = adc_fifo_get();
        if (adc_capture_done) continue;
        uint16_t next = (uint16_t)((adc_ring_head + 1u) & (ADC_SW_RING - 1u));
        if (next != adc_ring_tail) { // drop when full
            adc_ring[adc_ring_head] = s;
            adc_ring_head = next;
        } else adc_dropped++;
        if (++adc_captured >= adc_expected) {
            adc_run(false);
            adc_irq_set_enabled(false);
            adc_capture_done = 1;
        }
    }
    // Draining the FIFO below the threshold clears the FIFO interrupt.
    if (adc_deadline_reached) {
        adc_capture_done = 1;
        adc_irq_set_enabled(false);
    }
}

static int64_t adc_deadline_alarm(alarm_id_t id, void *user_data) {
    (void)id; (void)user_data;
    adc_stop_alarm = 0;
    adc_run(false);
    // Never touch the ring here: this timer may preempt its ADC producer.
    adc_deadline_reached = 1;
    irq_set_pending(ADC_IRQ_FIFO); // lower-priority ADC IRQ drains the final FIFO
    return 0;
}

typedef struct {
    uint8_t on;
    uint8_t ch;
    uint16_t low_mv, high_mv;
    uint8_t side; // 0=in band, 1=above, 2=below; 0xFF = uninitialized
} thresh_t;
static thresh_t thresh;

int adc_read_ch(uint8_t ch, uint16_t *raw, uint16_t *mv) {
    if (ch >= ADC_COUNT) return E_PARAM;
    if (adc_streaming) return E_BUSY;
    adc_select_input(ch);
    uint16_t v = (uint16_t)adc_read();
    *raw = v;
    *mv = (uint16_t)(((uint32_t)v * 3300u) / 4095u);
    return 0;
}

int adc_readall(uint16_t raw[ADC_COUNT], uint16_t mv[ADC_COUNT]) {
    if (adc_streaming) return E_BUSY;
    for (uint8_t i = 0; i < ADC_COUNT; i++) {
        adc_read_ch(i, &raw[i], &mv[i]);
    }
    return 0;
}

int adc_temp(int32_t *mdegc) {
    if (adc_streaming) return E_BUSY;
    // Ensure the ADC is in one-shot mode and the temp-sensor bias is on.
    adc_fifo_setup(false, false, 1, false, false);
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);
    sleep_us(1000); // let the sensor bias settle
    // Average several reads to reject noise / first-sample garbage.
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += adc_read();
    }
    uint16_t v = (uint16_t)(sum / 8);
    // The sensor outputs ~0.48-0.99 V over its full -55..+125 C range (raw
    // ~600..1230). A reading well outside that means the sensor isn't driving
    // the pin (unconnected/absent) - report that instead of an absurd value.
    if (v > 1300 || v < 400) return E_IO;
    uint32_t mv = ((uint32_t)v * 3300u) / 4095u;
    // T = 27 - (V - 0.706) / 0.001721, V in volts. Result in millidegrees.
    // Compute (mv - 706) as signed int64 to avoid uint32 underflow when mv < 706.
    int64_t dv = (int64_t)mv - 706;
    *mdegc = (int32_t)(27000 - (dv * 1000000) / 1721);
    return 0;
}

// ============================= time sync (one-way) ===========================
// The host periodically sends its time via `SYNC <host_us>`. The board records
// its local receive time and sets offset = host_time - local_time, so that
// synced_us = time_us_64() + offset tracks the host clock (interpolated between
// sync packets by the free-running microsecond counter).

static volatile int64_t g_time_offset_us;

void time_sync_set_offset(int64_t offset_us) {
    g_time_offset_us = offset_us;
}

int64_t time_sync_offset(void) {
    return g_time_offset_us;
}

int64_t time_synced_us(void) {
    return (int64_t)time_us_64() + g_time_offset_us;
}

// ============================= PWM tick interrupt ============================
// A "1 Hz callback": when a PWM slice's counter wraps (each period), fire an
// event (EVT PWM_TICK) so the host can react (e.g. send one UART message).
// For freq below the PWM's ~7.45 Hz floor the selected channel's pin is driven
// by the ISR as a true `freq`-Hz square wave (see g_tick_out_*).

static volatile uint8_t  g_tick_on;
static volatile uint8_t  g_tick_slice;
static volatile uint16_t g_tick_divide; // ISR fires an event every this many wraps
static volatile uint16_t g_tick_wraps;
static volatile uint32_t g_tick_count;
// Real-output pin: when divide>=2 (freq below the PWM's ~7.45 Hz floor) the ISR
// drives the selected channel's pin as a true `freq`-Hz square wave (SIO toggle
// at each half-period boundary), instead of leaving it at the base frequency.
static volatile uint8_t  g_tick_out_gpio;
static volatile uint8_t  g_tick_out_valid;

static bool pwm_tick_owns_slice(uint8_t slice) {
    return g_tick_on && g_tick_slice == slice;
}

static void pwm_tick_irq(void) {
    uint32_t m = pwm_hw->intr;
    // Clear EVERY pending wrap flag (W1C), regardless of g_tick_on. PWM_IRQ_WRAP
    // is shared by all 8 slices; a stale flag left pending re-triggers the IRQ
    // forever and hard-hangs the firmware (saw this: PWM TICK after PWM START).
    pwm_hw->intr = m;
    if (g_tick_on && (m & (1u << g_tick_slice))) {
        g_tick_wraps++;
        if (g_tick_wraps >= g_tick_divide) { // software prescaler
            g_tick_wraps = 0;
            g_tick_count++;
        }
        // Real output: toggle the selected pin at each half-period boundary so
        // it emits a true `freq`-Hz 50% square wave (period start = low, half
        // period = high). divide/2 wraps = half period at the base rate.
        if (g_tick_out_valid) {
            if (g_tick_wraps == (g_tick_divide >> 1)) {
                gpio_put(g_tick_out_gpio, 1);
            } else if (g_tick_wraps == 0) {
                gpio_put(g_tick_out_gpio, 0);
            }
        }
    }
}

int pwm_tick_set(uint8_t ch, uint32_t freq) {
    if (ch >= PWM_COUNT) return E_PARAM;
    if (freq == 0 || freq > 1000) return E_RANGE; // events must stay manageable
    // The RP2040 PWM cannot run below ~7.5 Hz, so run the hardware at the
    // smallest multiple of `freq` that is >= 10 Hz and divide in the ISR.
    uint32_t divide = 1;
    uint32_t base = freq;
    while (base < 10) { base += freq; divide++; }
    // Software output needs two equal half-periods, including freq=2 or 4 Hz.
    if (divide > 1 && (divide & 1u)) { divide++; base += freq; }
    uint32_t top, di, df;
    int rc = pwm_compute(base, &top, &di, &df);
    if (rc) return rc;
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        if (i != ch && PWM_MAP[i].slice == PWM_MAP[ch].slice && pwm_run[i]) return E_BUSY;
    }
    uint32_t saved = save_and_disable_interrupts();
    pwm_tick_off();
    pwm_cfg(ch, base, 32768); // validated above, tick ownership released
    pwm_set_enabled(PWM_MAP[ch].slice, false);
    pwm_set_counter(PWM_MAP[ch].slice, 0);
    pwm_inv[ch] = 0;
    pwm_apply_polarity(PWM_MAP[ch].slice);
    pwm_run[ch] = 1;
    pwm_apply_channel(ch);
    g_tick_slice = PWM_MAP[ch].slice;
    g_tick_divide = (uint16_t)divide;
    g_tick_wraps = 0;
    g_tick_count = 0;
    // Real freq-Hz output: only needed when freq is below the PWM's ~7.45 Hz
    // floor (divide>=2). For freq>=10 the hardware PWM outputs it directly, so
    // leave the pin on the PWM function.
    if (divide >= 2) {
        gpio_put(PWM_MAP[ch].gpio, 0); // period starts low
        gpio_set_dir(PWM_MAP[ch].gpio, GPIO_OUT);
        gpio_set_function(PWM_MAP[ch].gpio, GPIO_FUNC_SIO);
        g_tick_out_gpio = PWM_MAP[ch].gpio;
        g_tick_out_valid = 1;
    } else {
        g_tick_out_valid = 0;
    }
    pwm_hw->intr = (1u << g_tick_slice); // clear any stale wrap flag BEFORE arming the IRQ
    g_tick_on = 1;
    hw_set_bits(&pwm_hw->inte, (1u << g_tick_slice));
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_set_enabled(g_tick_slice, true);
    restore_interrupts(saved);
    return 0;
}

int pwm_tick_off(void) {
    uint32_t saved = save_and_disable_interrupts();
    if (g_tick_on) {
        hw_clear_bits(&pwm_hw->inte, (1u << g_tick_slice));
        g_tick_on = 0;
        if (g_tick_out_valid) {
            gpio_set_function(g_tick_out_gpio, GPIO_FUNC_PWM);
            g_tick_out_valid = 0;
        }
    }
    restore_interrupts(saved);
    return 0;
}

uint32_t pwm_tick_count(void) {
    return g_tick_count;
}

int adc_sample_start(uint8_t ch, uint32_t rate, uint32_t ms) {
    if (ch >= ADC_COUNT) return E_PARAM;
    if (adc_streaming || adc_end_pending) return E_BUSY;
    if (rate < 1000 || rate > 500000) return E_RANGE;
    if (ms == 0 || ms > 1000000u) return E_RANGE;
    uint64_t expected = ((uint64_t)rate * ms) / 1000u;
    if (expected == 0 || expected > UINT32_MAX) return E_RANGE;
    adc_run(false);
    adc_irq_set_enabled(false);
    irq_set_enabled(ADC_IRQ_FIFO, false);
    adc_fifo_drain();
    adc_set_clkdiv(48000000.0f / (float)rate - 1.0f);
    adc_ring_head = adc_ring_tail = 0;
    adc_expected = (uint32_t)expected;
    adc_sent = adc_captured = adc_dropped = adc_fifo_overflows = 0;
    adc_capture_done = 0;
    adc_deadline_reached = 0;
    adc_last_packet_us = time_us_64();
    adc_stream_ch = ch;
    adc_streaming = 1;
    adc_fifo_setup(true, false, rate >= 50000 ? 4 : 1, false, false);
    adc_hw->fcs = adc_hw->fcs | ADC_FCS_OVER_BITS | ADC_FCS_UNDER_BITS;
    adc_select_input(ch);
    adc_irq_set_enabled(true);
    irq_set_enabled(ADC_IRQ_FIFO, true);
    adc_stop_alarm = add_alarm_in_ms(ms + 1, adc_deadline_alarm, NULL, true);
    if (adc_stop_alarm < 0) {
        adc_irq_set_enabled(false);
        adc_streaming = 0;
        return E_BUSY;
    }
    adc_run(true);
    return 0;
}

static void adc_sample_stop_impl(void) {
    adc_run(false);
    irq_set_enabled(ADC_IRQ_FIFO, false);
    adc_irq_set_enabled(false);
    adc_fifo_setup(false, false, 1, false, false);
    adc_streaming = 0;
    adc_end_pending = 1;
    if (adc_stop_alarm > 0) cancel_alarm(adc_stop_alarm);
    adc_stop_alarm = 0;
}

int adc_sample_stop(uint8_t ch) {
    if (ch >= ADC_COUNT) return E_PARAM;
    if (!adc_streaming) return E_CFG;
    if (ch != adc_stream_ch) return E_CFG;
    adc_sample_stop_impl();
    return 0;
}

int adc_sample_active(void) {
    return adc_streaming;
}

uint16_t adc_stream_poll(uint16_t *out, uint16_t max) {
    if (!adc_streaming) return 0;
    uint16_t available = (uint16_t)((adc_ring_head - adc_ring_tail) & (ADC_SW_RING - 1u));
    uint64_t now = time_us_64();
    if (!adc_capture_done && available < max && now - adc_last_packet_us < 1000u) return 0;
    adc_last_packet_us = now;
    uint16_t n = 0;
    while (adc_ring_tail != adc_ring_head && n < max) {
        out[n++] = adc_ring[adc_ring_tail];
        adc_ring_tail = (uint16_t)((adc_ring_tail + 1u) & (ADC_SW_RING - 1u));
    }
    adc_sent += n;
    if (adc_capture_done && adc_ring_tail == adc_ring_head) adc_sample_stop_impl();
    return n;
}

uint8_t adc_stream_channel(void) {
    return adc_stream_ch;
}

uint32_t adc_stream_dropped(void) {
    uint32_t missing = adc_deadline_reached && adc_expected > adc_captured ? adc_expected - adc_captured : 0;
    return adc_dropped + (missing > adc_fifo_overflows ? missing : adc_fifo_overflows);
}
uint32_t adc_stream_sent(void) { return adc_sent; }
int adc_stream_end_pending(void) { return adc_end_pending; }
void adc_stream_ack_end(void) { adc_end_pending = 0; }

void adc_stream_dbg(uint32_t *sent, uint32_t *expected, uint8_t *streaming, uint16_t *ring_head, uint16_t *ring_tail) {
    *sent = adc_sent;
    *expected = adc_expected;
    *streaming = adc_streaming;
    *ring_head = adc_ring_head;
    *ring_tail = adc_ring_tail;
}

int adc_thresh(uint8_t ch, uint16_t low_mv, uint16_t high_mv, uint8_t on) {
    if (ch >= ADC_COUNT || on > 1) return E_PARAM;
    if (low_mv > 3300 || high_mv > 3300 || (high_mv && low_mv >= high_mv)) return E_RANGE;
    thresh.ch = ch;
    thresh.low_mv = low_mv;
    thresh.high_mv = high_mv;
    thresh.on = on ? 1 : 0;
    thresh.side = 0xFF; // first poll establishes baseline, no event
    return 0;
}

// Filled by drivers_poll when a threshold crossing occurs; consumed by main loop.
volatile uint8_t adc_thresh_event;
uint8_t adc_thresh_ev_dir;    // 0=BELOW, 1=ABOVE
uint16_t adc_thresh_ev_mv;
uint8_t adc_thresh_ev_ch;

// ============================= poll =========================================

static uint64_t thresh_last_us;

void drivers_init(void) {
    // IO: all input, no pull. Register the shared GPIO IRQ callback.
    gpio_set_irq_callback(io_irq_cb);
    for (uint8_t i = 0; i < IO_COUNT; i++) {
        io_configure(i, 1, 0);
        io_evt_mode[i] = 0;
    }

    // PWM: set function, default off. Must configure before use.
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        gpio_set_function(PWM_MAP[i].gpio, GPIO_FUNC_PWM);
        pwm_freqs[i] = 0;
        pwm_duties[i] = 0;
        pwm_run[i] = 0;
        pwm_inv[i] = 0;
        sweep[i].active = 0;
    }

    // UART: 115200 8N1, RX IRQ into ring buffers.
    for (uint8_t i = 0; i < 2; i++) {
        uart_rx[i].head = 0;
        uart_rx[i].tail = 0;
        uart_errs[i] = 0;
        uart_stream_on[i] = 0;
    }
    irq_set_exclusive_handler(UART0_IRQ, uart0_irq);
    irq_set_exclusive_handler(UART1_IRQ, uart1_irq);
    irq_set_enabled(UART0_IRQ, true);
    irq_set_enabled(UART1_IRQ, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_tick_irq);
    uart_cfg(0, 115200, 8, UART_PARITY_NONE, 1);
    uart_cfg(1, 115200, 8, UART_PARITY_NONE, 1);

    // I2C0: master, 400kHz.
    i2c_init(i2c0, 400000);
    gpio_set_function(I2C0_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SDA_GPIO);
    gpio_pull_up(I2C0_SCL_GPIO);
    i2c_set_slave_mode(i2c0, false, 0x55);

    // SPI0: default 1MHz, mode 0, MSB first, 8-bit.
    spi_cfg(1000000, 0, 1, 8);

    // ADC: GPIO26-28 + temp sensor.
    adc_init();
    adc_gpio_init(ADC_GPIO_BASE + 0);
    adc_gpio_init(ADC_GPIO_BASE + 1);
    adc_gpio_init(ADC_GPIO_BASE + 2);
    adc_set_temp_sensor_enabled(true);
    adc_fifo_setup(false, false, 1, false, false);
    irq_set_exclusive_handler(ADC_IRQ_FIFO, adc_fifo_irq);
    irq_set_priority(ADC_IRQ_FIFO, 0xC0); // allow the duration timer to stop overload capture

    // Status LED off (active low -> high turns it off).
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT);
    gpio_put(STATUS_LED_GPIO, 1);
}

void drivers_poll(void) {
    uint64_t now = time_us_64();

    // IO pulses
    for (uint8_t i = 0; i < IO_COUNT; i++) {
        if (io_pulse_active[i] && now >= io_pulse_end_us[i]) {
            io_pulse_active[i] = 0;
            gpio_put(IO_GPIO_BASE + i, !io_pulse_level[i]);
        }
    }

    // PWM sweeps
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        if (!sweep[i].active) continue;
        if (now < sweep[i].next_us) continue;
        if (sweep[i].cur < sweep[i].target) sweep[i].cur++;
        else if (sweep[i].cur > sweep[i].target) sweep[i].cur--;
        pwm_duties[i] = (uint16_t)sweep[i].cur;
        pwm_apply_channel(i);
        sweep[i].next_us += (uint64_t)sweep[i].step_ms * 1000u;
        if (sweep[i].cur == sweep[i].target) {
            if (sweep[i].repeat == 0 || --sweep[i].repeat > 0) {
                sweep[i].cur = sweep[i].from;
                pwm_duties[i] = (uint16_t)sweep[i].from;
                pwm_apply_channel(i);
            } else {
                sweep[i].active = 0;
            }
        }
    }

    // ADC threshold monitor (~every 10 ms)
    if (thresh.on && now - thresh_last_us >= 10000) {
        thresh_last_us = now;
        uint16_t raw, mv;
        if (adc_read_ch(thresh.ch, &raw, &mv) == 0) {
            uint8_t side;
            if (thresh.high_mv && mv > thresh.high_mv) side = 1;      // ABOVE
            else if (thresh.low_mv && mv < thresh.low_mv) side = 2;   // BELOW
            else side = 0;                                            // in band
            if (thresh.side != 0xFF && side != thresh.side) {
                if (side == 1 || side == 2) { // report only entering above/below
                    adc_thresh_ev_ch = thresh.ch;
                    adc_thresh_ev_dir = (uint8_t)(side == 1); // 0=BELOW,1=ABOVE
                    adc_thresh_ev_mv = mv;
                    adc_thresh_event = 1;
                }
            }
            thresh.side = side;
        }
    }
}
