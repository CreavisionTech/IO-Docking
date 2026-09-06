// Binary-mode protocol: framing + command dispatch. See Host_Protocol.md section 11.
// Frame: [SOF 0xCB][TYPE][OP][FLAGS][LEN u16 LE][SEQ u16 LE][payload][CRC16-CCITT u16 LE]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "board.h"
#include "protocol.h"
#include "drivers.h"
#include "seq.h"
#include "bin_proto.h"
#include "version.h"

#define BIN_MAX_PAYLOAD 4096u

static uint8_t  frame[8 + BIN_MAX_PAYLOAD + 2];
static uint8_t  fstate;   // 0=sof, 1=hdr, 2=payload, 3=crc
static uint16_t fpos;
static uint32_t flen;
static uint16_t g_req_seq;
static uint64_t last_rx_us;

static uint8_t big[4100]; // scratch for tx data / event payloads

// ---------- CRC ----------

static uint16_t crc16_byte(uint16_t c, uint8_t b) {
    static const uint16_t table[16] = {
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
        0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF
    };
    c ^= (uint16_t)b << 8;
    c = (uint16_t)((c << 4) ^ table[c >> 12]);
    c = (uint16_t)((c << 4) ^ table[c >> 12]);
    return c;
}

static uint16_t crc16(const uint8_t *d, uint16_t len) {
    uint16_t c = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) c = crc16_byte(c, d[i]);
    return c;
}

// ---------- senders ----------

void bin_send(uint8_t type, uint8_t op, uint16_t seq, const uint8_t *payload, uint16_t len) {
    static uint8_t packet[8 + BIN_MAX_PAYLOAD + 2];
    if (len > BIN_MAX_PAYLOAD || (len && !payload)) return;
    packet[0] = BIN_SOF; packet[1] = type; packet[2] = op; packet[3] = 0;
    packet[4] = (uint8_t)len; packet[5] = (uint8_t)(len >> 8);
    packet[6] = (uint8_t)seq; packet[7] = (uint8_t)(seq >> 8);
    if (len) memcpy(packet + 8, payload, len);
    uint16_t c = crc16(packet, (uint16_t)(8 + len));
    packet[8 + len] = (uint8_t)c; packet[9 + len] = (uint8_t)(c >> 8);
    ring_put(&g_tx, packet, (uint16_t)(10 + len));
}

void bin_send_resp(uint8_t op, uint16_t seq, const uint8_t *payload, uint16_t len) {
    bin_send(BIN_TYPE_RESP, op, seq, payload, len);
}

void bin_send_err(uint8_t op, uint16_t seq, uint8_t code, const char *msg) {
    uint8_t p[260];
    size_t ml = msg ? strlen(msg) : 0;
    if (ml > 255) ml = 255;
    p[0] = code;
    p[1] = (uint8_t)ml;
    if (ml) memcpy(p + 2, msg, ml);
    bin_send(BIN_TYPE_ERR, op, seq, p, (uint16_t)(2 + ml));
}

static void err_resp(uint8_t op, uint16_t seq, int rc) {
    if (rc == 0) { // success (0 also equals E_BADCMD) -> send an OK response
        bin_send_resp(op, seq, NULL, 0);
        return;
    }
    if (rc < 0 || rc >= E_COUNT) rc = E_IO;
    const char *m = (rc >= 0 && rc < E_COUNT) ? err_str[rc] : NULL;
    bin_send_err(op, seq, (uint8_t)rc, m);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static void pack64(uint8_t *dst, uint64_t v) {
    for (int i = 0; i < 8; i++) dst[i] = (uint8_t)(v >> (i * 8));
}

// ---------- system ----------

static void bp_ping(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    uint8_t r[4] = { 0x4F, FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH }; // magic + firmware version
    bin_send_resp(op, seq, r, sizeof(r));
}

// 0x0C SYNC: one-way time sync. Payload [host_us:u64] -> resp [local:u64][offset:i64][synced:u64]
static void bp_sync(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 8) { err_resp(op, seq, E_PARAM); return; }
    if (rd64(p) > INT64_MAX) { err_resp(op, seq, E_RANGE); return; }
    int64_t host_us = (int64_t)rd64(p);
    int64_t local_us = (int64_t)time_us_64();
    int64_t offset = host_us - local_us;
    time_sync_set_offset(offset);
    uint8_t r[24];
    pack64(r, (uint64_t)local_us);
    pack64(r + 8, (uint64_t)offset);
    pack64(r + 16, (uint64_t)time_synced_us());
    bin_send_resp(op, seq, r, 24);
}

// 0x0D TIME: resp [local:u64][offset:i64][synced:u64]
static void bp_time(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    uint8_t r[24];
    pack64(r, (uint64_t)time_us_64());
    pack64(r + 8, (uint64_t)time_sync_offset());
    pack64(r + 16, (uint64_t)time_synced_us());
    bin_send_resp(op, seq, r, 24);
}

static void add_tlv(uint8_t *buf, uint16_t *pos, const char *tag, const char *val) {
    uint8_t tl = (uint8_t)strlen(tag);
    uint8_t vl = (uint8_t)strlen(val);
    buf[(*pos)++] = tl;
    memcpy(buf + *pos, tag, tl); *pos += tl;
    buf[(*pos)++] = vl;
    memcpy(buf + *pos, val, vl); *pos += vl;
}

static void bp_info(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    char uid[17];
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    for (int i = 0; i < 8; i++) sprintf(&uid[i * 2], "%02x", id.id[i]);
    uid[16] = '\0';

    uint8_t tlv[256];
    uint16_t pos = 0;
    char num[24];
    add_tlv(tlv, &pos, "board", "IO-Dock");
    add_tlv(tlv, &pos, "mcu", "RP2040");
    add_tlv(tlv, &pos, "fw", FW_VERSION);
    add_tlv(tlv, &pos, "uid", uid);
    sprintf(num, "%lu", (unsigned long)clock_get_hz(clk_sys));
    add_tlv(tlv, &pos, "clk", num);
    sprintf(num, "%u", IO_COUNT); add_tlv(tlv, &pos, "io", num);
    sprintf(num, "%u", PWM_COUNT); add_tlv(tlv, &pos, "pwm", num);
    add_tlv(tlv, &pos, "uart", "2");
    add_tlv(tlv, &pos, "i2c", "1");
    add_tlv(tlv, &pos, "spi", "1");
    sprintf(num, "%u", ADC_COUNT); add_tlv(tlv, &pos, "adc", num);
    tlv[pos++] = 0; // terminator
    bin_send_resp(op, seq, tlv, pos);
}

static void bp_reset(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    bin_send_resp(op, seq, NULL, 0);
    watchdog_reboot(0, 0, 0);
}

static void bp_bootloader(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    bin_send_resp(op, seq, NULL, 0);
    reset_usb_boot(0, 0);
}

static void bp_led(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] > 1) { err_resp(op, seq, E_RANGE); return; }
    gpio_put(STATUS_LED_GPIO, p[0] ? 0 : 1);
    bin_send_resp(op, seq, NULL, 0);
}

static void bp_mode_exit(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    bin_send_resp(op, seq, NULL, 0);
    g_mode = MODE_TEXT;
}

// ---------- IO ----------

static void bp_io_cfg(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 3) { err_resp(op, seq, E_PARAM); return; }
    if (p[1] > 1 || p[2] > 2) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= IO_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, io_configure(p[0], p[1], p[2]));
}

static void bp_io_write(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 2) { err_resp(op, seq, E_PARAM); return; }
    if (p[1] > 1) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= IO_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, io_write(p[0], p[1]));
}

static void bp_io_read(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= IO_COUNT) { err_resp(op, seq, E_RANGE); return; }
    uint8_t lv;
    int rc = io_read(p[0], &lv);
    if (rc) { err_resp(op, seq, rc); return; }
    uint8_t r[2] = { p[0], lv };
    bin_send_resp(op, seq, r, 2);
}

static void bp_io_readall(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    uint8_t lv[IO_COUNT];
    io_readall(lv);
    uint8_t mask = 0;
    for (uint8_t i = 0; i < IO_COUNT; i++) mask |= (lv[i] ? 1 : 0) << i;
    bin_send_resp(op, seq, &mask, 1);
}

static void bp_io_toggle(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= IO_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, io_toggle(p[0]));
}

static void bp_io_pulse(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 4) { err_resp(op, seq, E_PARAM); return; }
    if (p[1] > 1) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= IO_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, io_pulse(p[0], p[1], rd16(p + 2)));
}

static void bp_io_event(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 2) { err_resp(op, seq, E_PARAM); return; }
    if (p[1] > 3) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= IO_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, io_event_set(p[0], p[1]));
}

// ---------- PWM ----------

static void bp_pwm_cfg(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 7) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, pwm_cfg(p[0], rd32(p + 1), rd16(p + 5)));
}

static void bp_pwm_freq(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 5) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, pwm_freq(p[0], rd32(p + 1)));
}

static void bp_pwm_duty(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 3) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, pwm_duty(p[0], rd16(p + 1)));
}

static void bp_pwm_start(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, pwm_start(p[0]));
}

static void bp_pwm_stop(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, pwm_stop(p[0]));
}

static void bp_pwm_sweep(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 9) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, pwm_sweep(p[0], rd16(p + 1), rd16(p + 3), rd16(p + 5), rd16(p + 7)));
}

static void bp_pwm_read(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    uint32_t f; uint16_t d; uint8_t run, inv;
    int rc = pwm_read(p[0], &f, &d, &run, &inv);
    if (rc) { err_resp(op, seq, rc); return; }
    uint8_t r[10];
    r[0] = p[0];
    r[1] = (uint8_t)(f & 0xFF); r[2] = (uint8_t)((f >> 8) & 0xFF); r[3] = (uint8_t)((f >> 16) & 0xFF); r[4] = (uint8_t)((f >> 24) & 0xFF);
    r[5] = (uint8_t)(d & 0xFF); r[6] = (uint8_t)((d >> 8) & 0xFF);
    r[7] = run; r[8] = inv; r[9] = 0;
    bin_send_resp(op, seq, r, 10);
}

static void bp_pwm_sync(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 1 || len - 1 != p[0]) { err_resp(op, seq, E_PARAM); return; }
    if (!p[0] || p[0] > PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    for (unsigned i = 1; i <= p[0]; i++) {
        if (p[i] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
        for (unsigned j = 1; j < i; j++) if (p[i] == p[j]) { err_resp(op, seq, E_PARAM); return; }
    }
    err_resp(op, seq, pwm_sync(p + 1, p[0]));
}

static void bp_pwm_phase(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 5) { err_resp(op, seq, E_PARAM); return; }
    if (p[1] >= PWM_COUNT || p[2] > 1 || (p[2] == 0 && rd16(p + 3) > 360)) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, pwm_phase(p[0], p[1], p[2], rd16(p + 3)));
}

static void bp_pwm_phadj(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 3) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    int16_t d = (int16_t)rd16(p + 1);
    err_resp(op, seq, pwm_phadj(p[0], d));
}

static void bp_pwm_pol(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 2) { err_resp(op, seq, E_PARAM); return; }
    if (p[1] > 1) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, pwm_pol(p[0], p[1]));
}

// 0x2B PWM_TICK: [ch][freq:u32] (freq=0 -> off)
static void bp_pwm_tick(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 5) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= PWM_COUNT) { err_resp(op, seq, E_RANGE); return; }
    uint32_t freq = rd32(p + 1);
    if (freq == 0) err_resp(op, seq, pwm_tick_off());
    else err_resp(op, seq, pwm_tick_set(p[0], freq));
}

// ---------- UART ----------

static void bp_uart_cfg(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 8) { err_resp(op, seq, E_PARAM); return; }
    if (rd32(p + 1) < 1200 || rd32(p + 1) > 7800000 || p[5] < 5 || p[5] > 8 || p[6] > 2 || p[7] < 1 || p[7] > 2) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= 2) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, uart_cfg(p[0], rd32(p + 1), p[5], p[6], p[7]));
}

static void bp_uart_tx(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 3) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= 2) { err_resp(op, seq, E_RANGE); return; }
    uint16_t n = rd16(p + 1);
    if (3 + n != len) { err_resp(op, seq, E_PARAM); return; }
    int rc = uart_tx(p[0], p + 3, n);
    if (rc) { err_resp(op, seq, rc); return; }
    uint8_t r[2] = { (uint8_t)(n & 0xFF), (uint8_t)((n >> 8) & 0xFF) };
    bin_send_resp(op, seq, r, 2);
}

static void bp_uart_rx(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 3) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= 2) { err_resp(op, seq, E_RANGE); return; }
    uint16_t max = rd16(p + 1);
    if (max > 2048) { err_resp(op, seq, E_RANGE); return; }
    uint16_t n = uart_rx_read(p[0], big + 3, max);
    big[0] = p[0];
    big[1] = (uint8_t)(n & 0xFF);
    big[2] = (uint8_t)((n >> 8) & 0xFF);
    bin_send_resp(op, seq, big, (uint16_t)(3 + n));
}

static void bp_uart_flush(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= 2) { err_resp(op, seq, E_RANGE); return; }
    uart_rx_flush(p[0]);
    bin_send_resp(op, seq, NULL, 0);
}

static void bp_uart_stream(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 2) { err_resp(op, seq, E_PARAM); return; }
    if (p[1] > 1) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= 2) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, uart_stream(p[0], p[1]));
}

static void bp_uart_stat(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= 2) { err_resp(op, seq, E_RANGE); return; }
    uint16_t rx, tx; uint32_t errs;
    int rc = uart_stat(p[0], &rx, &tx, &errs);
    if (rc) { err_resp(op, seq, rc); return; }
    uint8_t r[8];
    r[0] = (uint8_t)(rx & 0xFF); r[1] = (uint8_t)((rx >> 8) & 0xFF);
    r[2] = (uint8_t)(tx & 0xFF); r[3] = (uint8_t)((tx >> 8) & 0xFF);
    r[4] = (uint8_t)(errs & 0xFF); r[5] = (uint8_t)((errs >> 8) & 0xFF);
    r[6] = (uint8_t)((errs >> 16) & 0xFF); r[7] = (uint8_t)((errs >> 24) & 0xFF);
    bin_send_resp(op, seq, r, 8);
}

// ---------- I2C ----------

static void bp_i2c_cfg(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 4) { err_resp(op, seq, E_PARAM); return; }
    err_resp(op, seq, i2c_set_rate(rd32(p)));
}

static void bp_i2c_scan(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    uint8_t addrs[112];
    uint8_t n = 0;
    int rc = i2c_scan(addrs, &n);
    if (rc) { err_resp(op, seq, rc); return; }
    big[0] = n;
    memcpy(big + 1, addrs, n);
    bin_send_resp(op, seq, big, (uint16_t)(1 + n));
}

static void bp_i2c_write(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 4) { err_resp(op, seq, E_PARAM); return; }
    uint16_t n = rd16(p + 2);
    if (4 + n != len) { err_resp(op, seq, E_PARAM); return; }
    err_resp(op, seq, i2c_write_reg(p[0], p[1], p + 4, n));
}

static void bp_i2c_wronly(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 3) { err_resp(op, seq, E_PARAM); return; }
    uint16_t n = rd16(p + 1);
    if (3 + n != len) { err_resp(op, seq, E_PARAM); return; }
    err_resp(op, seq, i2c_write_only(p[0], p + 3, n));
}

static void bp_i2c_read(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 4) { err_resp(op, seq, E_PARAM); return; }
    uint16_t n = rd16(p + 2);
    if (!n || n > 255 || p[0] > 127) { err_resp(op, seq, E_RANGE); return; }
    int rc = i2c_read_reg(p[0], p[1], big + 2, n);
    if (rc) { err_resp(op, seq, rc); return; }
    big[0] = (uint8_t)(n & 0xFF); big[1] = (uint8_t)((n >> 8) & 0xFF);
    bin_send_resp(op, seq, big, (uint16_t)(2 + n));
}

static void bp_i2c_ronly(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 3) { err_resp(op, seq, E_PARAM); return; }
    uint16_t n = rd16(p + 1);
    if (!n || n > 255 || p[0] > 127) { err_resp(op, seq, E_RANGE); return; }
    int rc = i2c_read_only(p[0], big + 2, n);
    if (rc) { err_resp(op, seq, rc); return; }
    big[0] = (uint8_t)(n & 0xFF); big[1] = (uint8_t)((n >> 8) & 0xFF);
    bin_send_resp(op, seq, big, (uint16_t)(2 + n));
}

static void bp_i2c_wr(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 4) { err_resp(op, seq, E_PARAM); return; }
    uint16_t n = rd16(p + 2);
    if (!n || n > 255 || p[0] > 127) { err_resp(op, seq, E_RANGE); return; }
    int rc = i2c_read_reg(p[0], p[1], big + 2, n);
    if (rc) { err_resp(op, seq, rc); return; }
    big[0] = (uint8_t)(n & 0xFF); big[1] = (uint8_t)((n >> 8) & 0xFF);
    bin_send_resp(op, seq, big, (uint16_t)(2 + n));
}

// ---------- SPI ----------

static void bp_spi_cfg(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 7) { err_resp(op, seq, E_PARAM); return; }
    if (!rd32(p) || p[4] > 3 || p[5] > 1 || p[6] < 4 || p[6] > 16) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, spi_cfg(rd32(p), p[4], p[5], p[6]));
}

static void bp_spi_xfr(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 2) { err_resp(op, seq, E_PARAM); return; }
    uint16_t n = rd16(p);
    if (2u + n != len || 2u + n > BIN_MAX_PAYLOAD) { err_resp(op, seq, E_PARAM); return; }
    int rc = spi_xfr(p + 2, big + 2, n);
    if (rc) { err_resp(op, seq, rc); return; }
    big[0] = (uint8_t)(n & 0xFF); big[1] = (uint8_t)((n >> 8) & 0xFF);
    bin_send_resp(op, seq, big, (uint16_t)(2 + n));
}

static void bp_spi_write(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 2) { err_resp(op, seq, E_PARAM); return; }
    uint16_t n = rd16(p);
    if (2 + n != len) { err_resp(op, seq, E_PARAM); return; }
    err_resp(op, seq, spi_write(p + 2, n));
}

static void bp_spi_read(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 2) { err_resp(op, seq, E_PARAM); return; }
    uint16_t n = rd16(p);
    if (2u + n > BIN_MAX_PAYLOAD) { err_resp(op, seq, E_PARAM); return; }
    int rc = spi_read(big + 2, n);
    if (rc) { err_resp(op, seq, rc); return; }
    big[0] = (uint8_t)(n & 0xFF); big[1] = (uint8_t)((n >> 8) & 0xFF);
    bin_send_resp(op, seq, big, (uint16_t)(2 + n));
}

static void bp_spi_cs(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] > 2) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, spi_cs(p[0]));
}

// ---------- ADC ----------

static void bp_adc_read_ch(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= ADC_COUNT) { err_resp(op, seq, E_RANGE); return; }
    uint16_t raw, mv;
    int rc = adc_read_ch(p[0], &raw, &mv);
    if (rc) { err_resp(op, seq, rc); return; }
    uint8_t r[5];
    r[0] = p[0];
    r[1] = (uint8_t)(raw & 0xFF); r[2] = (uint8_t)((raw >> 8) & 0xFF);
    r[3] = (uint8_t)(mv & 0xFF); r[4] = (uint8_t)((mv >> 8) & 0xFF);
    bin_send_resp(op, seq, r, 5);
}

static void bp_adc_readall(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    uint16_t raw[ADC_COUNT], mv[ADC_COUNT];
    int rc = adc_readall(raw, mv);
    if (rc) { err_resp(op, seq, rc); return; }
    for (uint8_t i = 0; i < ADC_COUNT; i++) {
        big[i * 4 + 0] = (uint8_t)(raw[i] & 0xFF);
        big[i * 4 + 1] = (uint8_t)((raw[i] >> 8) & 0xFF);
        big[i * 4 + 2] = (uint8_t)(mv[i] & 0xFF);
        big[i * 4 + 3] = (uint8_t)((mv[i] >> 8) & 0xFF);
    }
    bin_send_resp(op, seq, big, ADC_COUNT * 4);
}

static void bp_adc_sample_start(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 9) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= ADC_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, adc_sample_start(p[0], rd32(p + 1), rd32(p + 5)));
}

static void bp_adc_sample_stop(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 1) { err_resp(op, seq, E_PARAM); return; }
    if (p[0] >= ADC_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, adc_sample_stop(p[0]));
}

static void bp_adc_temp(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    int32_t m;
    int rc = adc_temp(&m);
    if (rc) { err_resp(op, seq, rc); return; }
    uint8_t r[4];
    r[0] = (uint8_t)(m & 0xFF); r[1] = (uint8_t)((m >> 8) & 0xFF);
    r[2] = (uint8_t)((m >> 16) & 0xFF); r[3] = (uint8_t)((m >> 24) & 0xFF);
    bin_send_resp(op, seq, r, 4);
}

static void bp_adc_thresh(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len != 6) { err_resp(op, seq, E_PARAM); return; }
    if (p[5] > 1 || rd16(p + 1) > 3300 || (rd16(p + 3) && rd16(p + 1) >= rd16(p + 3)) || rd16(p + 3) > 3300) { err_resp(op, seq, E_RANGE); return; }
    if (p[0] >= ADC_COUNT) { err_resp(op, seq, E_RANGE); return; }
    err_resp(op, seq, adc_thresh(p[0], rd16(p + 1), rd16(p + 3), p[5]));
}

// ---------- SEQ ----------

static void bp_seq_def(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 3) { err_resp(op, seq, E_PARAM); return; }
    uint8_t nl = p[0];
    if (!nl || nl >= 16 || 3u + nl > len) { err_resp(op, seq, E_PARAM); return; }
    uint16_t sl = rd16(p + 1 + nl);
    if (!sl || sl >= 256 || 3u + nl + sl != len || memchr(p + 1, 0, nl) || memchr(p + 3 + nl, 0, sl)) { err_resp(op, seq, E_PARAM); return; }
    char name[16], script[256];
    memcpy(name, p + 1, nl); name[nl] = 0;
    memcpy(script, p + 3 + nl, sl); script[sl] = 0;
    err_resp(op, seq, seq_define(name, script));
}

static void bp_seq_list(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    char out[256];
    int rc = seq_list(out, sizeof(out));
    if (rc) { err_resp(op, seq, rc); return; }
    uint16_t o = 0;
    char *tok = strtok(out, " ");
    while (tok) {
        uint8_t l = (uint8_t)strlen(tok);
        big[o++] = l;
        memcpy(big + o, tok, l);
        o += l;
        tok = strtok(NULL, " ");
    }
    bin_send_resp(op, seq, big, o);
}

static void bp_seq_show(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 1) { err_resp(op, seq, E_PARAM); return; }
    char name[16];
    uint8_t nl = p[0];
    if (1 + nl != len || !nl || nl >= sizeof(name) || memchr(p + 1, 0, nl)) { err_resp(op, seq, E_PARAM); return; }
    memcpy(name, p + 1, nl);
    name[nl] = '\0';
    char script[256];
    int rc = seq_show(name, script, sizeof(script));
    if (rc) { err_resp(op, seq, rc); return; }
    uint16_t sl = (uint16_t)strlen(script);
    big[0] = (uint8_t)(sl & 0xFF); big[1] = (uint8_t)((sl >> 8) & 0xFF);
    memcpy(big + 2, script, sl);
    bin_send_resp(op, seq, big, (uint16_t)(2 + sl));
}

static void bp_seq_run(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 3) { err_resp(op, seq, E_PARAM); return; }
    char name[16];
    uint8_t nl = p[0];
    if (1 + nl + 2 != len || !nl || nl >= sizeof(name) || memchr(p + 1, 0, nl)) { err_resp(op, seq, E_PARAM); return; }
    memcpy(name, p + 1, nl);
    name[nl] = '\0';
    err_resp(op, seq, seq_run(name, rd16(p + 1 + nl)));
}

static void bp_seq_stop(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    err_resp(op, seq, seq_stop());
}

static void bp_seq_stat(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    (void)p;
    if (len != 0) { err_resp(op, seq, E_PARAM); return; }
    uint8_t state; char name[16]; uint32_t remaining, step;
    int rc = seq_stat(&state, name, sizeof(name), &remaining, &step);
    if (rc) { err_resp(op, seq, rc); return; }
    uint8_t nl = (uint8_t)strlen(name);
    uint16_t o = 0;
    big[o++] = state;
    big[o++] = nl;
    memcpy(big + o, name, nl); o += nl;
    big[o++] = (uint8_t)(remaining & 0xFF);
    big[o++] = (uint8_t)((remaining >> 8) & 0xFF);
    big[o++] = (uint8_t)((remaining >> 16) & 0xFF);
    big[o++] = (uint8_t)((remaining >> 24) & 0xFF);
    big[o++] = (uint8_t)(step & 0xFF);
    big[o++] = (uint8_t)((step >> 8) & 0xFF);
    big[o++] = (uint8_t)((step >> 16) & 0xFF);
    big[o++] = (uint8_t)((step >> 24) & 0xFF);
    bin_send_resp(op, seq, big, o);
}

static void bp_seq_del(uint8_t op, uint16_t seq, const uint8_t *p, uint16_t len) {
    if (len < 1) { err_resp(op, seq, E_PARAM); return; }
    char name[16];
    uint8_t nl = p[0];
    if (1 + nl != len || !nl || nl >= sizeof(name) || memchr(p + 1, 0, nl)) { err_resp(op, seq, E_PARAM); return; }
    memcpy(name, p + 1, nl);
    name[nl] = '\0';
    err_resp(op, seq, seq_del(name));
}

// ---------- dispatch ----------

static void dispatch(uint8_t op, const uint8_t *p, uint16_t len) {
    switch (op) {
        case 0x00: bp_ping(op, g_req_seq, p, len); break;
        case 0x01: bp_info(op, g_req_seq, p, len); break;
        case 0x02: bp_reset(op, g_req_seq, p, len); break;
        case 0x03: bp_bootloader(op, g_req_seq, p, len); break;
        case 0x04: bp_led(op, g_req_seq, p, len); break;
        case 0x0C: bp_sync(op, g_req_seq, p, len); break;
        case 0x0D: bp_time(op, g_req_seq, p, len); break;
        case 0x0F: bp_mode_exit(op, g_req_seq, p, len); break;

        case 0x10: bp_io_cfg(op, g_req_seq, p, len); break;
        case 0x11: bp_io_write(op, g_req_seq, p, len); break;
        case 0x12: bp_io_read(op, g_req_seq, p, len); break;
        case 0x13: bp_io_readall(op, g_req_seq, p, len); break;
        case 0x14: bp_io_toggle(op, g_req_seq, p, len); break;
        case 0x15: bp_io_pulse(op, g_req_seq, p, len); break;
        case 0x16: bp_io_event(op, g_req_seq, p, len); break;

        case 0x20: bp_pwm_cfg(op, g_req_seq, p, len); break;
        case 0x21: bp_pwm_freq(op, g_req_seq, p, len); break;
        case 0x22: bp_pwm_duty(op, g_req_seq, p, len); break;
        case 0x23: bp_pwm_start(op, g_req_seq, p, len); break;
        case 0x24: bp_pwm_stop(op, g_req_seq, p, len); break;
        case 0x25: bp_pwm_sweep(op, g_req_seq, p, len); break;
        case 0x26: bp_pwm_read(op, g_req_seq, p, len); break;
        case 0x27: bp_pwm_sync(op, g_req_seq, p, len); break;
        case 0x28: bp_pwm_phase(op, g_req_seq, p, len); break;
        case 0x29: bp_pwm_phadj(op, g_req_seq, p, len); break;
        case 0x2A: bp_pwm_pol(op, g_req_seq, p, len); break;
        case 0x2B: bp_pwm_tick(op, g_req_seq, p, len); break;

        case 0x30: bp_uart_cfg(op, g_req_seq, p, len); break;
        case 0x31: bp_uart_tx(op, g_req_seq, p, len); break;
        case 0x32: bp_uart_rx(op, g_req_seq, p, len); break;
        case 0x33: bp_uart_flush(op, g_req_seq, p, len); break;
        case 0x34: bp_uart_stream(op, g_req_seq, p, len); break;
        case 0x35: bp_uart_stat(op, g_req_seq, p, len); break;

        case 0x40: bp_i2c_cfg(op, g_req_seq, p, len); break;
        case 0x41: bp_i2c_scan(op, g_req_seq, p, len); break;
        case 0x42: bp_i2c_write(op, g_req_seq, p, len); break;
        case 0x43: bp_i2c_wronly(op, g_req_seq, p, len); break;
        case 0x44: bp_i2c_read(op, g_req_seq, p, len); break;
        case 0x45: bp_i2c_ronly(op, g_req_seq, p, len); break;
        case 0x46: bp_i2c_wr(op, g_req_seq, p, len); break;

        case 0x50: bp_spi_cfg(op, g_req_seq, p, len); break;
        case 0x51: bp_spi_xfr(op, g_req_seq, p, len); break;
        case 0x52: bp_spi_write(op, g_req_seq, p, len); break;
        case 0x53: bp_spi_read(op, g_req_seq, p, len); break;
        case 0x54: bp_spi_cs(op, g_req_seq, p, len); break;

        case 0x60: bp_adc_read_ch(op, g_req_seq, p, len); break;
        case 0x61: bp_adc_readall(op, g_req_seq, p, len); break;
        case 0x62: bp_adc_sample_start(op, g_req_seq, p, len); break;
        case 0x63: bp_adc_sample_stop(op, g_req_seq, p, len); break;
        case 0x64: bp_adc_temp(op, g_req_seq, p, len); break;
        case 0x65: bp_adc_thresh(op, g_req_seq, p, len); break;

        case 0x70: bp_seq_def(op, g_req_seq, p, len); break;
        case 0x71: bp_seq_list(op, g_req_seq, p, len); break;
        case 0x72: bp_seq_show(op, g_req_seq, p, len); break;
        case 0x73: bp_seq_run(op, g_req_seq, p, len); break;
        case 0x74: bp_seq_stop(op, g_req_seq, p, len); break;
        case 0x75: bp_seq_stat(op, g_req_seq, p, len); break;
        case 0x76: bp_seq_del(op, g_req_seq, p, len); break;

        default:
            bin_send_err(op, g_req_seq, E_BADCMD, "unknown op");
            break;
    }
}

// ---------- incremental frame parser ----------

void process_bin_byte(uint8_t b) {
    uint64_t now = time_us_64();
    if (fstate && now - last_rx_us > 1000000u) {
        // A disconnected sender must not leave the next request trapped in a payload.
        fstate = 0;
        fpos = 0;
    }
    last_rx_us = now;
    switch (fstate) {
        case 0: // look for SOF
            if (b == BIN_SOF) { frame[0] = b; fstate = 1; fpos = 1; }
            break;
        case 1: // header (8 bytes total)
            frame[fpos++] = b;
            if (fpos == 8) {
                flen = (uint32_t)frame[4] | ((uint32_t)frame[5] << 8);
                if (flen > BIN_MAX_PAYLOAD) { fstate = 0; break; } // bogus -> resync
                fstate = (flen == 0) ? 3 : 2;
            }
            break;
        case 2: // payload
            frame[fpos++] = b;
            if (fpos == (uint16_t)(8 + flen)) fstate = 3;
            break;
        case 3: // CRC
            frame[fpos++] = b;
            if (fpos == (uint16_t)(8 + flen + 2)) {
                uint16_t got = (uint16_t)(frame[8 + flen] | (frame[8 + flen + 1] << 8));
                if (crc16(frame, (uint16_t)(8 + flen)) == got) {
                    if (frame[1] == BIN_TYPE_CMD) {
                        g_req_seq = (uint16_t)(frame[6] | (frame[7] << 8));
                        if (frame[3]) bin_send_err(frame[2], g_req_seq, E_PARAM, "unsupported flags");
                        else dispatch(frame[2], frame + 8, (uint16_t)flen);
                    }
                } else if (frame[1] == BIN_TYPE_CMD) {
                    bin_send_err(frame[2], rd16(frame + 6), E_PARAM, "CRC mismatch");
                }
                fstate = 0;
            }
            break;
    }
}

// ---------- binary event emitters ----------

void bin_evt_gpio(uint8_t ch, uint8_t level) {
    uint8_t p[2] = { ch, level };
    bin_send(BIN_TYPE_EVT, 0x16, 0, p, 2);
}

void bin_evt_uart_rx(uint8_t ch, const uint8_t *data, uint16_t n) {
    if (n > BIN_MAX_PAYLOAD - 3) n = BIN_MAX_PAYLOAD - 3;
    big[0] = ch;
    big[1] = (uint8_t)(n & 0xFF);
    big[2] = (uint8_t)((n >> 8) & 0xFF);
    memcpy(big + 3, data, n);
    bin_send(BIN_TYPE_EVT, 0x34, 0, big, (uint16_t)(3 + n));
}

void bin_evt_uart_overrun(uint8_t ch, uint32_t dropped) {
    uint8_t p[5];
    p[0] = ch;
    p[1] = (uint8_t)(dropped & 0xFF);
    p[2] = (uint8_t)((dropped >> 8) & 0xFF);
    p[3] = (uint8_t)((dropped >> 16) & 0xFF);
    p[4] = (uint8_t)((dropped >> 24) & 0xFF);
    bin_send(BIN_TYPE_EVT, 0x37, 0, p, 5);
}

void bin_evt_adc_thresh(uint8_t ch, uint8_t dir, uint16_t mv) {
    uint8_t p[4] = { ch, dir, (uint8_t)(mv & 0xFF), (uint8_t)((mv >> 8) & 0xFF) };
    bin_send(BIN_TYPE_EVT, 0x65, 0, p, 4);
}

void bin_evt_seq_done(const char *name) {
    uint8_t l = (uint8_t)strlen(name);
    if (l > 15) l = 15;
    big[0] = l;
    memcpy(big + 1, name, l);
    bin_send(BIN_TYPE_EVT, 0x77, 0, big, (uint16_t)(1 + l));
}
