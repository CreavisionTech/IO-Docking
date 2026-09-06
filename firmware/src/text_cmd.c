// Text-mode command parser. Implements the Host_Protocol.md text command set.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"
#include "board.h"
#include "protocol.h"
#include "drivers.h"
#include "seq.h"
#include "config.h"
#include "text_cmd.h"

#include "version.h"
#include "parse_utils.h"
#define READ_NUM(type, name, token, base, low, high) \
    int64_t name##_value; \
    do { int rc_ = parse_integer(token, base, low, high, &name##_value); \
        if (rc_) { resp_err(cmd, rc_, "invalid " #name); return; } } while (0); \
    type name = (type)name##_value

void tprintf(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(b)) n = (int)sizeof(b) - 1;
    ring_put(&g_tx, (const uint8_t *)b, (uint16_t)n);
}

void tputs(const char *s) {
    ring_put(&g_tx, (const uint8_t *)s, (uint16_t)strlen(s));
}

// ---------- helpers ----------

static int parse_io(const char *s) {
    return parse_channel(s, "IO", 1, 6);
}
static int parse_pwm(const char *s) {
    return parse_channel(s, "PWM", 1, 4);
}
static int parse_uart(const char *s) {
    return parse_channel(s, "USART", 1, 2);
}
static int parse_adc(const char *s) {
    return parse_channel(s, "ADC", 0, 3);
}
static int parse_level(const char *s) {
    if (!s) return -1;
    if (!strcasecmp(s, "HIGH") || !strcmp(s, "1")) return 1;
    if (!strcasecmp(s, "LOW") || !strcmp(s, "0")) return 0;
    return -1;
}
static int parse_onoff(const char *s) {
    if (!s) return -1;
    if (!strcasecmp(s, "ON") || !strcmp(s, "1")) return 1;
    if (!strcasecmp(s, "OFF") || !strcmp(s, "0")) return 0;
    return -1;
}
// duty: 0-100 = percent, #N or >100 = raw 16-bit count
static int parse_duty(const char *s, uint16_t *raw) {
    if (!s || !*s) return E_PARAM;
    int raw_mode = *s == '#';
    if (raw_mode) s++;
    size_t len = strlen(s);
    int percent = len && s[len - 1] == '%';
    if (raw_mode && percent) return E_PARAM;
    if (percent) len--;
    char num[24];
    if (!len || len >= sizeof(num)) return E_PARAM;
    memcpy(num, s, len); num[len] = 0;
    int64_t v;
    int rc = parse_integer(num, 10, 0, percent ? 100 : 65535, &v);
    if (rc) return rc;
    *raw = !raw_mode && (percent || v <= 100) ? (uint16_t)(v * 65535 / 100) : (uint16_t)v;
    return 0;
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int parse_hex(const char *s, uint8_t *out, uint16_t max, uint16_t *n) {
    if (!s) return E_PARAM;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    size_t len = strlen(s);
    if (!len || len % 2) return E_PARAM;
    if (len / 2 > max) return E_RANGE;
    *n = (uint16_t)(len / 2);
    if (*n > max) return E_RANGE;
    for (uint16_t i = 0; i < *n; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return E_PARAM;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void resp_ok(const char *cmd) {
    tprintf("OK %s\n", cmd);
}
static void resp_err(const char *cmd, int code, const char *msg) {
    if (code == 0) {
        // Success: return value 0 also collides with E_BADCMD, so emit OK here.
        tprintf("OK %s\n", cmd);
        return;
    }
    if (code >= 0 && code < E_COUNT) {
        tprintf("ERR %s %s %s\n", cmd, err_str[code], msg ? msg : "");
    } else {
        tprintf("ERR %s E_IO %s\n", cmd, msg ? msg : "");
    }
}

// ---------- system commands ----------

static void get_uid_hex(char out[17]) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    for (int i = 0; i < 8; i++) sprintf(&out[i * 2], "%02x", id.id[i]);
    out[16] = '\0';
}

static void cmd_help(const char *cmd, char **rest) {
    (void)cmd; (void)rest;
    tprintf("OK HELP\n");
    tprintf(" HELP INFO PING RESET BOOTLOADER LED\n"
            " SAVE|LOAD|CFG CLEAR   (参数持久化:保存/恢复/清除)\n"
            " IO CFG|WRITE|READ|READALL|TOGGLE|PULSE|EVENT\n"
            " PWM CFG|FREQ|DUTY|START|STOP|SWEEP|READ|SYNC|PHASE|PHADJ|POL|TICK\n"
            " UART CFG|TX|RX|FLUSH|STREAM|STAT\n"
            " I2C CFG|SCAN|WRITE|WRONLY|READ|RONLY|WR\n"
            " SPI CFG|XFR|WRITE|READ|CS\n"
            " ADC READ|READALL|SAMPLE|TEMP|THRESH\n"
            " SEQ DEF|LIST|SHOW|RUN|STOP|STAT|DEL\n"
            " BIN ENTER\nEND\n");
}

static void cmd_info(const char *cmd, char **rest) {
    (void)cmd; (void)rest;
    char uid[17];
    get_uid_hex(uid);
    tprintf("OK INFO\n");
    tprintf(" board=IO-Dock\n mcu=RP2040\n fw=%s\n", FW_VERSION);
    tprintf(" uid=%s\n clk=%u\n", uid, clock_get_hz(clk_sys));
    tprintf(" io=%u pwm=%u uart=2 i2c=1 spi=1 adc=%u\nEND\n",
            IO_COUNT, PWM_COUNT, ADC_COUNT);
}

static void cmd_ping(const char *cmd, char **rest) {
    (void)rest;
    tprintf("OK %s PONG %s\n", cmd, FW_VERSION);
}

static void cmd_reset(const char *cmd, char **rest) {
    (void)rest;
    tprintf("OK %s\n", cmd);
    watchdog_reboot(0, 0, 0);
}

static void cmd_bootloader(const char *cmd, char **rest) {
    (void)rest;
    tprintf("OK %s\n", cmd);
    reset_usb_boot(0, 0);
}

static void cmd_led(const char *cmd, char **rest) {
    char *arg = strtok_r(NULL, " ", rest);
    if (!arg || parse_onoff(arg) < 0) { resp_err(cmd, E_PARAM, "ON|OFF"); return; }
    int on = parse_onoff(arg);
    // status LED is active-low
    gpio_put(STATUS_LED_GPIO, on ? 0 : 1);
    resp_ok(cmd);
}

// SYNC <host_us>: one-way time sync. Board sets offset = host_time - local_time.
static void cmd_sync(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    if (!a) { resp_err(cmd, E_PARAM, "host_us"); return; }
    READ_NUM(int64_t, host_us, a, 10, 0, INT64_MAX);
    int64_t local_us = (int64_t)time_us_64();
    int64_t offset = host_us - local_us;
    time_sync_set_offset(offset);
    tprintf("OK %s offset=%lld local=%lld synced=%lld\n", cmd,
            (long long)offset, (long long)local_us, (long long)time_synced_us());
}

// TIME: report local / offset / synced time.
static void cmd_time(const char *cmd, char **rest) {
    (void)rest;
    tprintf("OK %s local=%lld offset=%lld synced=%lld\n", cmd,
            (long long)time_us_64(), (long long)time_sync_offset(),
            (long long)time_synced_us());
}

// ---------- IO ----------

static void cmd_io_cfg(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    int ch = parse_io(a);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "IOx IN|OUT [PULLUP|PULLDOWN|FLOAT]"); return; }
    int dir = !strcasecmp(b, "IN") ? 1 : !strcasecmp(b, "OUT") ? 0 : -1;
    if (dir < 0) { resp_err(cmd, E_PARAM, "IN|OUT"); return; }
    int pull = 0;
    if (c) {
        if (!strcasecmp(c, "PULLUP")) pull = 1;
        else if (!strcasecmp(c, "PULLDOWN")) pull = 2;
        else if (!strcasecmp(c, "FLOAT")) pull = 0;
        else { resp_err(cmd, E_PARAM, "PULLUP|PULLDOWN|FLOAT"); return; }
    }
    resp_err(cmd, io_configure(ch, dir, pull), NULL);
}

static void cmd_io_write(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    int ch = parse_io(a);
    int lv = parse_level(b);
    if (ch < 0 || lv < 0) { resp_err(cmd, E_PARAM, "IOx HIGH|LOW"); return; }
    resp_err(cmd, io_write(ch, lv), NULL);
}

static void cmd_io_read(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_io(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "IOx"); return; }
    uint8_t lv;
    int rc = io_read(ch, &lv);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s IO%u %s\n", cmd, ch + 1, lv ? "HIGH" : "LOW");
}

static void cmd_io_readall(const char *cmd, char **rest) {
    (void)rest;
    uint8_t lv[IO_COUNT];
    io_readall(lv);
    tprintf("OK %s\n", cmd);
    for (uint8_t i = 0; i < IO_COUNT; i++) {
        tprintf(" IO%u %s", i + 1, lv[i] ? "HIGH" : "LOW");
    }
    tprintf("\nEND\n");
}

static void cmd_io_toggle(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_io(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "IOx"); return; }
    resp_err(cmd, io_toggle(ch), NULL);
}

static void cmd_io_pulse(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    int ch = parse_io(a);
    int lv = parse_level(b);
    if (ch < 0 || lv < 0 || !c) { resp_err(cmd, E_PARAM, "IOx HIGH|LOW ms"); return; }
    READ_NUM(long, ms, c, 10, 0, INT32_MAX);
    if (ms < 0 || ms > 65535) { resp_err(cmd, E_RANGE, "ms 0..65535"); return; }
    resp_err(cmd, io_pulse(ch, lv, (uint16_t)ms), NULL);
}

static void cmd_io_event(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    int ch = parse_io(a);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "IOx ON|OFF [RISING|FALLING|CHANGE]"); return; }
    if (!strcasecmp(b, "ON")) {
        int mode = 3;
        if (c) {
            if (!strcasecmp(c, "RISING")) mode = 1;
            else if (!strcasecmp(c, "FALLING")) mode = 2;
            else if (!strcasecmp(c, "CHANGE")) mode = 3;
            else { resp_err(cmd, E_PARAM, "RISING|FALLING|CHANGE"); return; }
        }
        resp_err(cmd, io_event_set(ch, mode), NULL);
    } else if (!strcasecmp(b, "OFF")) {
        if (c) { resp_err(cmd, E_PARAM, "extra argument"); return; }
        resp_err(cmd, io_event_set(ch, 0), NULL);
    } else {
        resp_err(cmd, E_PARAM, "ON|OFF");
    }
}

// ---------- PWM ----------

static void cmd_pwm_cfg(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0 || !b || !c) { resp_err(cmd, E_PARAM, "PWMx freq_Hz duty"); return; }
    READ_NUM(long, f, b, 10, 0, INT32_MAX);
    uint16_t duty;
    int rc = parse_duty(c, &duty);
    if (rc) { resp_err(cmd, rc, "duty"); return; }
    if (f <= 0) { resp_err(cmd, E_RANGE, "freq"); return; }
    rc = pwm_cfg(ch, (uint32_t)f, duty);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s PWM%u %ldHz %u%%\n", cmd, ch + 1, f, (unsigned)((duty * 100u + 32767u) / 65535u));
}

static void cmd_pwm_freq(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "PWMx freq_Hz"); return; }
    READ_NUM(long, f, b, 10, 0, INT32_MAX);
    if (f <= 0) { resp_err(cmd, E_RANGE, "freq"); return; }
    resp_err(cmd, pwm_freq(ch, (uint32_t)f), NULL);
}

static void cmd_pwm_duty(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "PWMx duty"); return; }
    uint16_t duty;
    int rc = parse_duty(b, &duty);
    if (rc) { resp_err(cmd, rc, "duty"); return; }
    resp_err(cmd, pwm_duty(ch, duty), NULL);
}

static void cmd_pwm_start(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "PWMx"); return; }
    resp_err(cmd, pwm_start(ch), NULL);
}

static void cmd_pwm_stop(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "PWMx"); return; }
    resp_err(cmd, pwm_stop(ch), NULL);
}

static void cmd_pwm_sweep(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    char *d = strtok_r(NULL, " ", rest);
    char *e = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0 || !b || !c || !d) { resp_err(cmd, E_PARAM, "PWMx from_duty to_duty step_ms [repeat]"); return; }
    uint16_t from, to;
    if (parse_duty(b, &from) || parse_duty(c, &to)) { resp_err(cmd, E_PARAM, "duty"); return; }
    READ_NUM(long, ms, d, 10, 0, INT32_MAX);
    READ_NUM(long, rep, (e ? e : "0"), 10, 0, 65535);
    if (ms < 0 || ms > 65535 || rep < 0 || rep > 65535) { resp_err(cmd, E_RANGE, NULL); return; }
    resp_err(cmd, pwm_sweep(ch, from, to, (uint16_t)ms, (uint16_t)rep), NULL);
}

static void cmd_pwm_read(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "PWMx"); return; }
    uint32_t f; uint16_t d; uint8_t run, inv;
    int rc = pwm_read(ch, &f, &d, &run, &inv);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s PWM%u %luHz %u%% %s pol=%s\n", cmd, ch + 1,
            (unsigned long)f, (unsigned)((d * 100u + 32767u) / 65535u),
            run ? "running" : "stopped", inv ? "invert" : "normal");
}

static void cmd_pwm_sync(const char *cmd, char **rest) {
    char *list = strtok_r(NULL, " ", rest);
    if (!list) { resp_err(cmd, E_PARAM, "PWM1,PWM2,..."); return; }
    if (!*list || list[0] == ',' || list[strlen(list)-1] == ',' || strstr(list, ",,")) { resp_err(cmd, E_PARAM, "channel list"); return; }
    uint8_t chs[PWM_COUNT];
    uint8_t n = 0;
    char *s2 = NULL;
    char *tok = strtok_r(list, ",", &s2);
    while (tok && n < PWM_COUNT) {
        int ch = parse_pwm(tok);
        if (ch < 0) { resp_err(cmd, E_PARAM, NULL); return; }
        for (uint8_t i = 0; i < n; i++) if (chs[i] == ch) { resp_err(cmd, E_PARAM, "duplicate channel"); return; }
        chs[n++] = (uint8_t)ch;
        tok = strtok_r(NULL, ",", &s2);
    }
    if (n == 0 || tok) { resp_err(cmd, E_PARAM, NULL); return; }
    resp_err(cmd, pwm_sync(chs, n), NULL);
}

static void cmd_pwm_phase(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    int ref = parse_pwm(b);
    if (ch < 0 || ref < 0 || !c) { resp_err(cmd, E_PARAM, "PWMx ref_deg"); return; }
    READ_NUM(long, deg, c, 10, 0, INT32_MAX);
    if (deg < 0 || deg > 360) { resp_err(cmd, E_RANGE, "0..360"); return; }
    int rc = pwm_phase(ch, ref, 0, (uint16_t)deg);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s PWM%u %lddeg (ref PWM%u)\n", cmd, ch + 1, deg, ref + 1);
}

static void cmd_pwm_phadj(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "PWMx delta_ticks"); return; }
    READ_NUM(long, d, b, 10, -32768, 32767);
    if (d < -32768 || d > 32767) { resp_err(cmd, E_RANGE, NULL); return; }
    resp_err(cmd, pwm_phadj(ch, (int16_t)d), NULL);
}

static void cmd_pwm_pol(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    int ch = parse_pwm(a);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "PWMx NORMAL|INVERT"); return; }
    int inv = !strcasecmp(b, "INVERT") ? 1 : !strcasecmp(b, "NORMAL") ? 0 : -1;
    if (inv < 0) { resp_err(cmd, E_PARAM, "NORMAL|INVERT"); return; }
    resp_err(cmd, pwm_pol(ch, inv), NULL);
}

// PWM TICK <PWMx> <freq_Hz> | PWM TICK OFF
// Fire EVT PWM_TICK every `freq` period and drive the channel's pin as a real
// `freq`-Hz square wave (ISR toggles it when freq is below the PWM floor).
static void cmd_pwm_tick(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    if (!a) { resp_err(cmd, E_PARAM, "PWMx freq | OFF"); return; }
    if (!strcasecmp(a, "OFF")) { if (strtok_r(NULL, " ", rest)) { resp_err(cmd, E_PARAM, "extra argument"); return; } resp_err(cmd, pwm_tick_off(), NULL); return; }
    int ch = parse_pwm(a);
    char *b = strtok_r(NULL, " ", rest);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "PWMx freq"); return; }
    READ_NUM(long, f, b, 10, 0, INT32_MAX);
    resp_err(cmd, pwm_tick_set(ch, (uint32_t)f), NULL);
}

// ---------- UART ----------

static void cmd_uart_cfg(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    char *d = strtok_r(NULL, " ", rest);
    char *e = strtok_r(NULL, " ", rest);
    int ch = parse_uart(a);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "USARTx baud [databits] [parity] [stopbits]"); return; }
    READ_NUM(long, baud, b, 10, 0, INT32_MAX);
    READ_NUM(int, dbits, (c ? c : "8"), 10, 5, 8);
    int parity = 0;
    if (d) {
        if (!strcasecmp(d, "N") || !strcmp(d, "0")) parity = 0;
        else if (!strcasecmp(d, "O") || !strcmp(d, "1")) parity = 1;
        else if (!strcasecmp(d, "E") || !strcmp(d, "2")) parity = 2;
        else { resp_err(cmd, E_PARAM, "parity N|O|E"); return; }
    }
    READ_NUM(int, sbits, (e ? e : "1"), 10, 1, 2);
    resp_err(cmd, uart_cfg(ch, (uint32_t)baud, (uint8_t)dbits, (uint8_t)parity, (uint8_t)sbits), NULL);
}

static void cmd_uart_tx(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_uart(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "USARTx TXT:..|HEX:.."); return; }
    const char *data = *rest ? *rest : "";
    uint8_t buf[512];
    uint16_t n;
    int rc;
    if (strncasecmp(data, "HEX:", 4) == 0) {
        rc = parse_hex(data + 4, buf, sizeof(buf), &n);
    } else {
        const char *txt = strncasecmp(data, "TXT:", 4) == 0 ? data + 4 : data;
        n = (uint16_t)strlen(txt);
        if (strlen(txt) > sizeof(buf)) { resp_err(cmd, E_LONG, "data"); return; }
        memcpy(buf, txt, n);
        rc = 0;
    }
    if (rc) { resp_err(cmd, rc, "data"); return; }
    rc = uart_tx(ch, buf, n);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s USART%u %u\n", cmd, ch + 1, n);
}

static void cmd_uart_rx(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_uart(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "USARTx [n]"); return; }
    uint16_t max = 1024;
    char *b = strtok_r(NULL, " ", rest);
    if (b) {
        READ_NUM(long, v, b, 10, 0, INT32_MAX);
        if (v < 0 || v > 2048) { resp_err(cmd, E_RANGE, "n"); return; }
        max = (uint16_t)v;
    }
    static uint8_t buf[2048];
    uint16_t n = uart_rx_read(ch, buf, max);
    tprintf("OK %s USART%u HEX:", cmd, ch + 1);
    for (uint16_t i = 0; i < n; i++) tprintf("%02X", buf[i]);
    tprintf("\n");
}

static void cmd_uart_flush(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_uart(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "USARTx"); return; }
    uart_rx_flush(ch);
    resp_ok(cmd);
}

static void cmd_uart_stream(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    int ch = parse_uart(a);
    if (ch < 0 || !b) { resp_err(cmd, E_PARAM, "USARTx ON|OFF"); return; }
    int on = parse_onoff(b);
    if (on < 0) { resp_err(cmd, E_PARAM, "ON|OFF"); return; }
    resp_err(cmd, uart_stream(ch, on), NULL);
}

static void cmd_uart_stat(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_uart(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "USARTx"); return; }
    uint16_t rx, tx; uint32_t errs;
    int rc = uart_stat(ch, &rx, &tx, &errs);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s USART%u rx=%u tx=%u errs=%lu\n", cmd, ch + 1, rx, tx, (unsigned long)errs);
}

// ---------- I2C ----------

static void cmd_i2c_cfg(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    if (!a) { resp_err(cmd, E_PARAM, "rate_kHz (100|400|1000)"); return; }
    READ_NUM(long, khz, a, 10, 0, INT32_MAX);
    resp_err(cmd, i2c_set_rate((uint32_t)khz), NULL);
}

static void cmd_i2c_scan(const char *cmd, char **rest) {
    (void)rest;
    uint8_t addrs[112];
    uint8_t n = 0;
    int rc = i2c_scan(addrs, &n);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s\n", cmd);
    for (uint8_t i = 0; i < n; i++) tprintf(" 0x%02X", addrs[i]);
    tprintf("\nEND\n");
}

static void cmd_i2c_write(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    if (!a || !b) { resp_err(cmd, E_PARAM, "addr reg HEXdata"); return; }
    READ_NUM(uint8_t, addr, a, 0, 0, 127);
    READ_NUM(uint8_t, reg, b, 0, 0, 255);
    uint8_t buf[255];
    uint16_t n;
    int rc = parse_hex(*rest ? *rest : "", buf, sizeof(buf), &n);
    if (rc) { resp_err(cmd, rc, "data"); return; }
    resp_err(cmd, i2c_write_reg(addr, reg, buf, n), NULL);
}

static void cmd_i2c_wronly(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    if (!a) { resp_err(cmd, E_PARAM, "addr HEXdata"); return; }
    READ_NUM(uint8_t, addr, a, 0, 0, 127);
    uint8_t buf[255];
    uint16_t n;
    int rc = parse_hex(*rest ? *rest : "", buf, sizeof(buf), &n);
    if (rc) { resp_err(cmd, rc, "data"); return; }
    resp_err(cmd, i2c_write_only(addr, buf, n), NULL);
}

static void cmd_i2c_read(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    if (!a || !b || !c) { resp_err(cmd, E_PARAM, "addr reg n"); return; }
    READ_NUM(uint8_t, addr, a, 0, 0, 127);
    READ_NUM(uint8_t, reg, b, 0, 0, 255);
    READ_NUM(long, n, c, 10, 0, INT32_MAX);
    if (n < 1 || n > 255) { resp_err(cmd, E_RANGE, "n 1..255"); return; }
    uint8_t buf[255];
    int rc = i2c_read_reg(addr, reg, buf, (uint16_t)n);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s HEX:", cmd);
    for (long i = 0; i < n; i++) tprintf("%02X", buf[i]);
    tprintf("\n");
}

static void cmd_i2c_ronly(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    if (!a || !b) { resp_err(cmd, E_PARAM, "addr n"); return; }
    READ_NUM(uint8_t, addr, a, 0, 0, 127);
    READ_NUM(long, n, b, 10, 0, INT32_MAX);
    if (n < 1 || n > 255) { resp_err(cmd, E_RANGE, "n 1..255"); return; }
    uint8_t buf[255];
    int rc = i2c_read_only(addr, buf, (uint16_t)n);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s HEX:", cmd);
    for (long i = 0; i < n; i++) tprintf("%02X", buf[i]);
    tprintf("\n");
}

static void cmd_i2c_wr(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    if (!a || !b || !c) { resp_err(cmd, E_PARAM, "addr reg n"); return; }
    READ_NUM(uint8_t, addr, a, 0, 0, 127);
    READ_NUM(uint8_t, reg, b, 0, 0, 255);
    READ_NUM(long, n, c, 10, 0, INT32_MAX);
    if (n < 1 || n > 255) { resp_err(cmd, E_RANGE, "n 1..255"); return; }
    uint8_t buf[255];
    int rc = i2c_read_reg(addr, reg, buf, (uint16_t)n);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s HEX:", cmd);
    for (long i = 0; i < n; i++) tprintf("%02X", buf[i]);
    tprintf("\n");
}

// ---------- SPI ----------

static void cmd_spi_cfg(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    char *d = strtok_r(NULL, " ", rest);
    if (!a) { resp_err(cmd, E_PARAM, "baud [mode] [bitorder] [bits]"); return; }
    READ_NUM(long, baud, a, 10, 0, INT32_MAX);
    READ_NUM(int, mode, (b ? b : "0"), 10, 0, 3);
    int msb = 1;
    if (c && !strcasecmp(c, "LSB")) msb = 0;
    else if (c && strcasecmp(c, "MSB")) { resp_err(cmd, E_PARAM, "MSB|LSB"); return; }
    READ_NUM(int, bits, (d ? d : "8"), 10, 4, 16);
    resp_err(cmd, spi_cfg((uint32_t)baud, (uint8_t)mode, (uint8_t)msb, (uint8_t)bits), NULL);
}

static void cmd_spi_xfr(const char *cmd, char **rest) {
    const char *data = *rest ? *rest : "";
    if (strncasecmp(data, "HEX:", 4) == 0) data += 4;
    static uint8_t tx[4096];
    static uint8_t rx[4096];
    uint16_t n;
    int rc = parse_hex(data, tx, sizeof(tx), &n);
    if (rc) { resp_err(cmd, rc, "HEXdata"); return; }
    rc = spi_xfr(tx, rx, n);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s HEX:", cmd);
    for (uint16_t i = 0; i < n; i++) tprintf("%02X", rx[i]);
    tprintf("\n");
}

static void cmd_spi_write(const char *cmd, char **rest) {
    const char *data = *rest ? *rest : "";
    if (strncasecmp(data, "HEX:", 4) == 0) data += 4;
    static uint8_t tx[4096];
    uint16_t n;
    int rc = parse_hex(data, tx, sizeof(tx), &n);
    if (rc) { resp_err(cmd, rc, "HEXdata"); return; }
    resp_err(cmd, spi_write(tx, n), NULL);
}

static void cmd_spi_read(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    if (!a) { resp_err(cmd, E_PARAM, "n"); return; }
    READ_NUM(long, n, a, 10, 0, INT32_MAX);
    if (n < 1 || n > 4096) { resp_err(cmd, E_RANGE, "n 1..4096"); return; }
    static uint8_t rx[4096];
    int rc = spi_read(rx, (uint16_t)n);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s HEX:", cmd);
    for (long i = 0; i < n; i++) tprintf("%02X", rx[i]);
    tprintf("\n");
}

static void cmd_spi_cs(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    if (!a) { resp_err(cmd, E_PARAM, "AUTO|HIGH|LOW"); return; }
    int mode = !strcasecmp(a, "AUTO") ? 0 : !strcasecmp(a, "LOW") ? 1 : !strcasecmp(a, "HIGH") ? 2 : -1;
    if (mode < 0) { resp_err(cmd, E_PARAM, "AUTO|HIGH|LOW"); return; }
    resp_err(cmd, spi_cs(mode), NULL);
}

// ---------- ADC ----------

static void cmd_adc_read_ch(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_adc(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "ADCx"); return; }
    uint16_t raw, mv;
    int rc = adc_read_ch(ch, &raw, &mv);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s ADC%u %u %umV\n", cmd, ch, raw, mv);
}

static void cmd_adc_readall(const char *cmd, char **rest) {
    (void)rest;
    uint16_t raw[ADC_COUNT], mv[ADC_COUNT];
    int rc = adc_readall(raw, mv);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s\n", cmd);
    for (uint8_t i = 0; i < ADC_COUNT; i++) tprintf(" ADC%u %u %umV\n", i, raw[i], mv[i]);
    tprintf("END\n");
}

static void cmd_adc_sample(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    int ch = parse_adc(a);
    if (ch < 0) { resp_err(cmd, E_PARAM, "ADCx rate_Hz ms | STOP"); return; }
    char *b = strtok_r(NULL, " ", rest);
    if (b && !strcasecmp(b, "STOP")) {
        if (strtok_r(NULL, " ", rest)) { resp_err(cmd, E_PARAM, "extra argument"); return; }
        resp_err(cmd, adc_sample_stop(ch), NULL);
        return;
    }
    char *c = strtok_r(NULL, " ", rest);
    if (!b || !c) { resp_err(cmd, E_PARAM, "rate_Hz ms"); return; }
    READ_NUM(long, rate, b, 10, 0, INT32_MAX);
    READ_NUM(long, ms, c, 10, 0, INT32_MAX);
    if (rate <= 0 || rate > 500000 || ms <= 0 || ms > 1000000) { resp_err(cmd, E_RANGE, NULL); return; }
    resp_err(cmd, adc_sample_start(ch, (uint32_t)rate, (uint32_t)ms), NULL);
}

static void cmd_adc_temp(const char *cmd, char **rest) {
    (void)rest;
    int32_t m;
    int rc = adc_temp(&m);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    int32_t ip = m / 1000;
    int32_t fp = m % 1000;
    if (fp < 0) fp = -fp;
    tprintf("OK %s %ld.%03ldC\n", cmd, (long)ip, (long)fp);
}

static void cmd_adc_dbg(const char *cmd, char **rest) {
    (void)rest;
    uint32_t sent, expected;
    uint8_t streaming;
    uint16_t rh, rt;
    adc_stream_dbg(&sent, &expected, &streaming, &rh, &rt);
    tprintf("OK %s cs=0x%08x result=0x%04x stream=%u sent=%lu exp=%lu ring=%u/%u\n", cmd,
            (unsigned)adc_hw->cs, (unsigned)adc_hw->result, streaming,
            (unsigned long)sent, (unsigned long)expected, rh, rt);
}

static void cmd_adc_thresh(const char *cmd, char **rest) {
    char *a = strtok_r(NULL, " ", rest);
    char *b = strtok_r(NULL, " ", rest);
    char *c = strtok_r(NULL, " ", rest);
    char *d = strtok_r(NULL, " ", rest);
    int ch = parse_adc(a);
    if (ch < 0 || !b || !c || !d) { resp_err(cmd, E_PARAM, "ADCx low_mv high_mv ON|OFF"); return; }
    READ_NUM(long, lo, b, 10, 0, 3300);
    READ_NUM(long, hi, c, 10, 0, 3300);
    int on = parse_onoff(d);
    if (on < 0) { resp_err(cmd, E_PARAM, "ON|OFF"); return; }
    resp_err(cmd, adc_thresh(ch, (uint16_t)lo, (uint16_t)hi, on), NULL);
}

// ---------- SEQ ----------

static void cmd_seq_def(const char *cmd, char **rest) {
    char *name = strtok_r(NULL, " ", rest);
    if (!name) { resp_err(cmd, E_PARAM, "name script"); return; }
    const char *script = *rest ? *rest : "";
    size_t sl = strlen(script);
    char tmp[256];
    if (*script == '"') {
        if (sl < 2 || script[sl - 1] != '"') { resp_err(cmd, E_PARAM, "unclosed script"); return; }
        script++; sl -= 2;
    } else if (strchr(script, '"')) { resp_err(cmd, E_PARAM, "script quotes"); return; }
    if (!sl || sl >= sizeof(tmp) || strlen(name) > 15) { resp_err(cmd, E_LONG, "name/script"); return; }
    memcpy(tmp, script, sl); tmp[sl] = 0; script = tmp;
    resp_err(cmd, seq_define(name, script), NULL);
}

static void cmd_seq_list(const char *cmd, char **rest) {
    (void)rest;
    char out[256];
    int rc = seq_list(out, sizeof(out));
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s %s\n", cmd, out);
}

static void cmd_seq_show(const char *cmd, char **rest) {
    char *name = strtok_r(NULL, " ", rest);
    if (!name) { resp_err(cmd, E_PARAM, "name"); return; }
    char out[256];
    if (seq_show(name, out, sizeof(out)) != 0) { resp_err(cmd, E_NOTFOUND, "name"); return; }
    tprintf("OK %s %s\n", cmd, out);
}

static void cmd_seq_run(const char *cmd, char **rest) {
    char *name = strtok_r(NULL, " ", rest);
    if (!name) { resp_err(cmd, E_PARAM, "name [repeat]"); return; }
    char *r = strtok_r(NULL, " ", rest);
    READ_NUM(long, rep, (r ? r : "1"), 10, 0, 65535);
    if (rep < 0 || rep > 65535) { resp_err(cmd, E_RANGE, "repeat"); return; }
    resp_err(cmd, seq_run(name, (uint16_t)rep), NULL);
}

static void cmd_seq_stop(const char *cmd, char **rest) {
    (void)rest;
    resp_err(cmd, seq_stop(), NULL);
}

static void cmd_seq_stat(const char *cmd, char **rest) {
    (void)rest;
    uint8_t state; char name[16]; uint32_t remaining, step;
    int rc = seq_stat(&state, name, sizeof(name), &remaining, &step);
    if (rc) { resp_err(cmd, rc, NULL); return; }
    tprintf("OK %s state=%s name=%s remaining=%lu step=%lu\n", cmd,
            state ? "running" : "idle", name, (unsigned long)remaining, (unsigned long)step);
}

static void cmd_seq_del(const char *cmd, char **rest) {
    char *name = strtok_r(NULL, " ", rest);
    if (!name) { resp_err(cmd, E_PARAM, "name"); return; }
    resp_err(cmd, seq_del(name), NULL);
}

static void cmd_bin_enter(const char *cmd, char **rest) {
    (void)cmd; (void)rest;
    tprintf("OK BIN\n");
    g_mode = MODE_BIN;
}

// ---------- parameter persistence ----------

static void cmd_config_save(const char *cmd, char **rest) {
    (void)rest;
    int rc = config_save();
    if (rc) { resp_err(cmd, rc, "flash write failed"); return; }
    tprintf("OK %s PWM/IO/UART/I2C/SPI/SEQ saved\n", cmd);
}

static void cmd_config_load(const char *cmd, char **rest) {
    (void)rest;
    int rc = config_load();
    if (rc) { resp_err(cmd, rc, "no valid saved config"); return; }
    tprintf("OK %s config loaded\n", cmd);
}

static void cmd_config_clear(const char *cmd, char **rest) {
    (void)rest;
    resp_err(cmd, config_clear(), NULL);
}

// ---------- dispatch ----------

typedef void (*cmd_fn)(const char *cmd, char **rest);
typedef struct { const char *name; cmd_fn fn; } cmd_entry_t;

static const cmd_entry_t cmds[] = {
    { "HELP",       cmd_help },
    { "INFO",       cmd_info },
    { "PING",       cmd_ping },
    { "SYNC",       cmd_sync },
    { "TIME",       cmd_time },
    { "RESET",      cmd_reset },
    { "BOOTLOADER", cmd_bootloader },
    { "LED",        cmd_led },
    { "SAVE",       cmd_config_save },
    { "LOAD",       cmd_config_load },
    { "CFG CLEAR",  cmd_config_clear },

    { "IO CFG",     cmd_io_cfg },
    { "IO WRITE",   cmd_io_write },
    { "IO READ",    cmd_io_read },
    { "IO READALL", cmd_io_readall },
    { "IO TOGGLE",  cmd_io_toggle },
    { "IO PULSE",   cmd_io_pulse },
    { "IO EVENT",   cmd_io_event },

    { "PWM CFG",    cmd_pwm_cfg },
    { "PWM FREQ",   cmd_pwm_freq },
    { "PWM DUTY",   cmd_pwm_duty },
    { "PWM START",  cmd_pwm_start },
    { "PWM STOP",   cmd_pwm_stop },
    { "PWM SWEEP",  cmd_pwm_sweep },
    { "PWM READ",   cmd_pwm_read },
    { "PWM SYNC",   cmd_pwm_sync },
    { "PWM PHASE",  cmd_pwm_phase },
    { "PWM PHADJ",  cmd_pwm_phadj },
    { "PWM POL",    cmd_pwm_pol },
    { "PWM TICK",   cmd_pwm_tick },

    { "UART CFG",   cmd_uart_cfg },
    { "UART TX",    cmd_uart_tx },
    { "UART RX",    cmd_uart_rx },
    { "UART FLUSH", cmd_uart_flush },
    { "UART STREAM",cmd_uart_stream },
    { "UART STAT",  cmd_uart_stat },

    { "I2C CFG",    cmd_i2c_cfg },
    { "I2C SCAN",   cmd_i2c_scan },
    { "I2C WRITE",  cmd_i2c_write },
    { "I2C WRONLY", cmd_i2c_wronly },
    { "I2C READ",   cmd_i2c_read },
    { "I2C RONLY",  cmd_i2c_ronly },
    { "I2C WR",     cmd_i2c_wr },

    { "SPI CFG",    cmd_spi_cfg },
    { "SPI XFR",    cmd_spi_xfr },
    { "SPI WRITE",  cmd_spi_write },
    { "SPI READ",   cmd_spi_read },
    { "SPI CS",     cmd_spi_cs },

    { "ADC READ",   cmd_adc_read_ch },
    { "ADC DBG",    cmd_adc_dbg },
    { "ADC READALL",cmd_adc_readall },
    { "ADC SAMPLE", cmd_adc_sample },
    { "ADC TEMP",   cmd_adc_temp },
    { "ADC THRESH", cmd_adc_thresh },

    { "SEQ DEF",    cmd_seq_def },
    { "SEQ LIST",   cmd_seq_list },
    { "SEQ SHOW",   cmd_seq_show },
    { "SEQ RUN",    cmd_seq_run },
    { "SEQ STOP",   cmd_seq_stop },
    { "SEQ STAT",   cmd_seq_stat },
    { "SEQ DEL",    cmd_seq_del },

    { "BIN ENTER",  cmd_bin_enter },
};

void process_line(const char *line) {
    if (!line) return;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return;
    char buf[520];
    size_t len = strlen(line);
    if (len >= sizeof(buf)) { resp_err("LINE", E_LONG, "line too long"); return; }
    memcpy(buf, line, len + 1);
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\t') buf[i] = ' ';
        else if ((unsigned char)buf[i] < 32 || (unsigned char)buf[i] == 127) { resp_err("LINE", E_PARAM, "control character"); return; }
    }
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        size_t n = strlen(cmds[i].name);
        if (strncasecmp(buf, cmds[i].name, n) || (buf[n] && buf[n] != ' ')) continue;
        char *args = buf + n;
        while (*args == ' ') args++;
        unsigned argc = 0;
        for (const char *q = args; *q;) {
            argc++;
            while (*q && *q != ' ') q++;
            while (*q == ' ') q++;
        }
        int max_args = -1;
        if (cmds[i].fn == cmd_help) max_args = 0;
        if (cmds[i].fn == cmd_info) max_args = 0;
        if (cmds[i].fn == cmd_ping) max_args = 0;
        if (cmds[i].fn == cmd_reset) max_args = 0;
        if (cmds[i].fn == cmd_bootloader) max_args = 0;
        if (cmds[i].fn == cmd_led) max_args = 1;
        if (cmds[i].fn == cmd_sync) max_args = 1;
        if (cmds[i].fn == cmd_time) max_args = 0;
        if (cmds[i].fn == cmd_io_cfg) max_args = 3;
        if (cmds[i].fn == cmd_io_write) max_args = 2;
        if (cmds[i].fn == cmd_io_read) max_args = 1;
        if (cmds[i].fn == cmd_io_readall) max_args = 0;
        if (cmds[i].fn == cmd_io_toggle) max_args = 1;
        if (cmds[i].fn == cmd_io_pulse) max_args = 3;
        if (cmds[i].fn == cmd_io_event) max_args = 3;
        if (cmds[i].fn == cmd_pwm_cfg) max_args = 3;
        if (cmds[i].fn == cmd_pwm_freq) max_args = 2;
        if (cmds[i].fn == cmd_pwm_duty) max_args = 2;
        if (cmds[i].fn == cmd_pwm_start) max_args = 1;
        if (cmds[i].fn == cmd_pwm_stop) max_args = 1;
        if (cmds[i].fn == cmd_pwm_sweep) max_args = 5;
        if (cmds[i].fn == cmd_pwm_read) max_args = 1;
        if (cmds[i].fn == cmd_pwm_sync) max_args = 1;
        if (cmds[i].fn == cmd_pwm_phase) max_args = 3;
        if (cmds[i].fn == cmd_pwm_phadj) max_args = 2;
        if (cmds[i].fn == cmd_pwm_pol) max_args = 2;
        if (cmds[i].fn == cmd_pwm_tick) max_args = 2;
        if (cmds[i].fn == cmd_uart_cfg) max_args = 5;
        if (cmds[i].fn == cmd_uart_rx) max_args = 2;
        if (cmds[i].fn == cmd_uart_flush) max_args = 1;
        if (cmds[i].fn == cmd_uart_stream) max_args = 2;
        if (cmds[i].fn == cmd_uart_stat) max_args = 1;
        if (cmds[i].fn == cmd_i2c_cfg) max_args = 1;
        if (cmds[i].fn == cmd_i2c_scan) max_args = 0;
        if (cmds[i].fn == cmd_i2c_read) max_args = 3;
        if (cmds[i].fn == cmd_i2c_ronly) max_args = 2;
        if (cmds[i].fn == cmd_i2c_wr) max_args = 3;
        if (cmds[i].fn == cmd_spi_cfg) max_args = 4;
        if (cmds[i].fn == cmd_spi_read) max_args = 1;
        if (cmds[i].fn == cmd_spi_cs) max_args = 1;
        if (cmds[i].fn == cmd_adc_read_ch) max_args = 1;
        if (cmds[i].fn == cmd_adc_readall) max_args = 0;
        if (cmds[i].fn == cmd_adc_sample) max_args = 3;
        if (cmds[i].fn == cmd_adc_temp) max_args = 0;
        if (cmds[i].fn == cmd_adc_dbg) max_args = 0;
        if (cmds[i].fn == cmd_adc_thresh) max_args = 4;
        if (cmds[i].fn == cmd_seq_list) max_args = 0;
        if (cmds[i].fn == cmd_seq_show) max_args = 1;
        if (cmds[i].fn == cmd_seq_run) max_args = 2;
        if (cmds[i].fn == cmd_seq_stop) max_args = 0;
        if (cmds[i].fn == cmd_seq_stat) max_args = 0;
        if (cmds[i].fn == cmd_seq_del) max_args = 1;
        if (cmds[i].fn == cmd_bin_enter) max_args = 0;
        if (cmds[i].fn == cmd_config_save) max_args = 0;
        if (cmds[i].fn == cmd_config_load) max_args = 0;
        if (cmds[i].fn == cmd_config_clear) max_args = 0;
        if (max_args >= 0 && argc > (unsigned)max_args) { resp_err(cmds[i].name, E_PARAM, "extra arguments"); return; }
        cmds[i].fn(cmds[i].name, &args);
        return;
    }
    tprintf("ERR LINE E_BADCMD unknown command\n");
}

// ---------- text event emitters ----------

static char evt_buf[2300];

void evt_gpio(uint8_t ch, uint8_t level) {
    tprintf("EVT GPIO IO%u %s\n", ch + 1, level ? "HIGH" : "LOW");
}

void evt_uart_rx(uint8_t ch, const uint8_t *data, uint16_t n) {
    if (n * 2u + 40u > sizeof(evt_buf)) n = (uint16_t)((sizeof(evt_buf) - 40u) / 2u);
    char *p = evt_buf;
    p += sprintf(p, "EVT UART_RX USART%u HEX:", ch + 1);
    for (uint16_t i = 0; i < n; i++) p += sprintf(p, "%02X", data[i]);
    *p++ = '\n';
    *p = 0;
    tputs(evt_buf);
}

void evt_uart_overrun(uint8_t ch, uint32_t dropped) {
    tprintf("EVT UART_OVERRUN USART%u %lu\n", ch + 1, (unsigned long)dropped);
}

void evt_adc_thresh(uint8_t ch, uint8_t dir, uint16_t mv) {
    tprintf("EVT ADC_THRESH ADC%u %s %umV\n", ch, dir ? "ABOVE" : "BELOW", mv);
}

void evt_seq_done(const char *name) {
    tprintf("EVT SEQ_DONE %s\n", name);
}
