// Flash-resident parameter storage. Config lives in the last 4 KB sector of the
// 8 MB flash (offset 0x7FF000). A SAVE erases that sector and programs it from
// a RAM buffer; LOAD/restore reads it back via XIP and validates magic+CRC.
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "protocol.h"
#include "drivers.h"
#include "seq.h"

// 8 MB flash, last 4 KB sector: absolute 0x107FF000, flash offset 0x7FF000.
#define CONFIG_OFFSET      ((8u * 1024u * 1024u) - 4096u)
#define CONFIG_SECTOR_SIZE 4096u
#define CONFIG_MAGIC       0x494F4431u  // "IOD1"
#define CONFIG_VERSION     2u // Standard-board layout; reject custom/legacy layouts.

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc;                       // crc32 of the fields below
    // PWM (4)
    uint32_t pwm_freq[PWM_COUNT];
    uint16_t pwm_duty[PWM_COUNT];
    uint8_t  pwm_run[PWM_COUNT];
    uint8_t  pwm_inv[PWM_COUNT];
    // IO (6)
    uint8_t  io_dir[IO_COUNT];
    uint8_t  io_pull[IO_COUNT];
    uint8_t  io_level[IO_COUNT];
    // UART (2)
    uint32_t uart_baud[2];
    uint8_t  uart_dbits[2];
    uint8_t  uart_parity[2];
    uint8_t  uart_sbits[2];
    // I2C
    uint32_t i2c_khz;
    // SPI
    uint32_t spi_baud;
    uint8_t  spi_mode;
    uint8_t  spi_msb;
    uint8_t  spi_bits;
    // Sequence
    char     seq_name[16];
    char     seq_script[256];
} config_t;

static const config_t *cfg_ptr(void) {
    return (const config_t *)(XIP_BASE + CONFIG_OFFSET);
}

static uint32_t crc32(const uint8_t *d, uint32_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static uint32_t cfg_crc(const config_t *c) {
    return crc32((const uint8_t *)c + offsetof(config_t, pwm_freq),
                 sizeof(config_t) - offsetof(config_t, pwm_freq));
}

int config_valid(void) {
    const config_t *c = cfg_ptr();
    if (c->magic != CONFIG_MAGIC || c->version != CONFIG_VERSION) return 0;
    return cfg_crc(c) == c->crc;
}

// ---- snapshot current driver state into cfg ----

static void snapshot(config_t *c) {
    memset(c, 0, sizeof(*c));
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        pwm_read(i, &c->pwm_freq[i], &c->pwm_duty[i], &c->pwm_run[i], &c->pwm_inv[i]);
    }
    for (uint8_t i = 0; i < IO_COUNT; i++) {
        io_cfg_get(i, &c->io_dir[i], &c->io_pull[i]);
        uint8_t lv;
        io_read(i, &lv);
        c->io_level[i] = lv;
    }
    for (uint8_t i = 0; i < 2; i++) {
        uart_cfg_get(i, &c->uart_baud[i], &c->uart_dbits[i], &c->uart_parity[i], &c->uart_sbits[i]);
    }
    c->i2c_khz = i2c_get_rate();
    spi_cfg_get(&c->spi_baud, &c->spi_mode, &c->spi_msb, &c->spi_bits);
    char name[16];
    seq_list(name, sizeof(name));
    if (name[0]) {
        strncpy(c->seq_name, name, sizeof(c->seq_name) - 1);
        c->seq_name[sizeof(c->seq_name) - 1] = '\0';
        char script[256];
        if (seq_show(name, script, sizeof(script)) == 0) {
            strncpy(c->seq_script, script, sizeof(c->seq_script) - 1);
            c->seq_script[sizeof(c->seq_script) - 1] = '\0';
        }
    }
}

// ---- apply cfg to driver state ----

static void apply(const config_t *c) {
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        if (c->pwm_freq[i] == 0) continue; // never configured
        pwm_cfg(i, c->pwm_freq[i], c->pwm_duty[i]);
        pwm_pol(i, c->pwm_inv[i]);
        if (c->pwm_run[i]) pwm_start(i);
        else pwm_stop(i);
    }
    for (uint8_t i = 0; i < IO_COUNT; i++) {
        io_configure(i, c->io_dir[i], c->io_pull[i]);
        if (c->io_dir[i] == 0) io_write(i, c->io_level[i]); // only drive outputs
    }
    for (uint8_t i = 0; i < 2; i++) {
        if (c->uart_baud[i]) uart_cfg(i, c->uart_baud[i], c->uart_dbits[i], c->uart_parity[i], c->uart_sbits[i]);
    }
    if (c->i2c_khz == 100 || c->i2c_khz == 400 || c->i2c_khz == 1000) {
        i2c_set_rate(c->i2c_khz);
    }
    if (c->spi_baud) spi_cfg(c->spi_baud, c->spi_mode, c->spi_msb, c->spi_bits);
    if (c->seq_name[0]) seq_define(c->seq_name, c->seq_script);
}

// ---- flash sector write/erase helpers ----

static int config_busy(void) {
    uint8_t state;
    char name[16];
    uint32_t remaining, step;
    seq_stat(&state, name, sizeof(name), &remaining, &step);
    return state || adc_sample_active();
}

static void write_sector(void *param) {
    flash_range_erase(CONFIG_OFFSET, CONFIG_SECTOR_SIZE);
    if (param) flash_range_program(CONFIG_OFFSET, param, CONFIG_SECTOR_SIZE);
}

int config_save(void) {
    if (config_busy()) return E_BUSY;
    config_t c;
    snapshot(&c);
    c.magic = CONFIG_MAGIC;
    c.version = CONFIG_VERSION;
    c.crc = cfg_crc(&c);

    static uint8_t buf[CONFIG_SECTOR_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &c, sizeof(c));
    int rc = flash_safe_execute(write_sector, buf, 1000);
    if (rc != PICO_OK) return E_IO;
    return config_valid() ? 0 : E_IO;
}

int config_load(void) {
    if (config_busy()) return E_BUSY;
    if (!config_valid()) return E_NOTFOUND;
    apply(cfg_ptr());
    return 0;
}

int config_restore(void) {
    if (!config_valid()) return 0;
    apply(cfg_ptr());
    return 0;
}

int config_clear(void) {
    if (config_busy()) return E_BUSY;
    return flash_safe_execute(write_sector, NULL, 1000) == PICO_OK ? 0 : E_IO;
}
