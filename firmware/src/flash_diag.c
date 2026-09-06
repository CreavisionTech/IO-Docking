// flash_diag.c — RAM-resident flash diagnostic.
// Identifies the on-board SPI NOR flash and reads status registers using pure
// bootrom helpers + direct SSI single-I/O commands (0x9F/0x05/0x35/0x03).
// Deliberately does NOT use pico's hardware_flash (flash_do_cmd_cs re-enters
// XIP via a boot2 copy read FROM XIP, which hangs/crashes when quad-XIP is
// broken). Built with pico_set_binary_type(copy_to_ram) so it runs from SRAM
// even if flash XIP is not usable. Reports over USB CDC (pico_stdio_usb) once
// per second, plus a heartbeat blink on the status LED (GPIO2, active-low).
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/regs/io_qspi.h"
#include "hardware/structs/io_qspi.h"
#include "hardware/regs/ssi.h"
#include "hardware/structs/ssi.h"

#define STATUS_LED_GPIO 2u // active low

// Send a single-I/O command to the flash: connect internal flash -> disable
// XIP -> CS low -> SSI shift count bytes (full duplex) -> CS high. No XIP
// reads anywhere, so it is safe even when the boot's quad-XIP config is broken.
static void diag_cmd(const uint8_t *txbuf, uint8_t *rxbuf, size_t count) {
    rom_connect_internal_flash_fn connect =
        (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn exit_xip =
        (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    if (connect) connect();
    if (exit_xip) exit_xip();

    hw_write_masked(&io_qspi_hw->io[1].ctrl,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_LOW
                        << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS);

    size_t tx_remaining = count;
    size_t rx_remaining = count;
    const size_t max_in_flight = 16 - 2; // don't overflow the 16-deep SSI FIFO
    while (tx_remaining || rx_remaining) {
        uint32_t flags = ssi_hw->sr;
        bool can_put = flags & SSI_SR_TFNF_BITS;
        bool can_get = flags & SSI_SR_RFNE_BITS;
        if (can_put && tx_remaining && rx_remaining - tx_remaining < max_in_flight) {
            ssi_hw->dr0 = *txbuf++;
            --tx_remaining;
        }
        if (can_get && rx_remaining) {
            *rxbuf++ = (uint8_t)ssi_hw->dr0;
            --rx_remaining;
        }
    }

    hw_write_masked(&io_qspi_hw->io[1].ctrl,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH
                        << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS);
}

static void read_jedec(uint8_t out[3]) {
    uint8_t tx[4] = {0x9F, 0, 0, 0};
    uint8_t rx[4];
    diag_cmd(tx, rx, 4);
    out[0] = rx[1];
    out[1] = rx[2];
    out[2] = rx[3];
}

static uint8_t read_sr(uint8_t cmd) {
    uint8_t tx[2] = {cmd, 0};
    uint8_t rx[2];
    diag_cmd(tx, rx, 2);
    return rx[1];
}

static void read_flash(uint32_t addr, uint8_t *out, uint8_t n) {
    uint8_t tx[1 + 3 + 16] = {0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t rx[1 + 3 + 16];
    tx[1] = (uint8_t)((addr >> 16) & 0xff);
    tx[2] = (uint8_t)((addr >> 8) & 0xff);
    tx[3] = (uint8_t)(addr & 0xff);
    diag_cmd(tx, rx, 1 + 3 + 16);
    memcpy(out, rx + 4, n); // data bytes start after cmd + 3 addr bytes
}

int main(void) {
    stdio_init_all();
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT);
    gpio_put(STATUS_LED_GPIO, 1); // LED off (active low)
    sleep_ms(800);                // let USB enumerate

    // Phase 1 (no flash access at all): prove the app boots + USB enumerates.
    printf("\n=== FLASH_DIAG ALIVE 1 ===\n");
    fflush(stdout);
    for (int i = 0; i < 4; i++) {
        gpio_put(STATUS_LED_GPIO, 0);
        sleep_ms(500);
        printf("alive tick %d\n", i);
        fflush(stdout);
        gpio_put(STATUS_LED_GPIO, 1);
        sleep_ms(500);
    }

    // Phase 2: flash reads (direct SSI, no XIP).
    uint8_t jedec[3];
    uint8_t hdr[16];
    bool toggle = false;

    for (;;) {
        read_jedec(jedec);
        uint8_t sr1 = read_sr(0x05);
        uint8_t sr2 = read_sr(0x35);
        read_flash(0, hdr, 16);

        printf("\n===== FLASH DIAG =====\n");
        printf("JEDEC: %02X %02X %02X\n", jedec[0], jedec[1], jedec[2]);
        if (jedec[0] == 0xFF && jedec[1] == 0xFF && jedec[2] == 0xFF) {
            printf("  -> flash NOT responding (solder/wiring/power or dead part)\n");
        } else if (jedec[0] == 0xEF && jedec[1] == 0x40 && jedec[2] == 0x17) {
            printf("  -> Winbond W25Q64 (64Mbit / 8MB)\n");
        } else if (jedec[0] == 0xEF && jedec[1] == 0x40 && jedec[2] == 0x18) {
            printf("  -> Winbond W25Q128 (128Mbit / 16MB)\n");
        } else if (jedec[0] == 0xEF && jedec[1] == 0x40 && jedec[2] == 0x16) {
            printf("  -> Winbond W25Q32 (32Mbit / 4MB)\n");
        } else if (jedec[0] == 0xC8) {
            printf("  -> GigaDevice GD25Q, capacity byte %02X\n", jedec[2]);
        } else {
            printf("  -> unknown/clone part (mfr %02X dev %02X %02X)\n",
                   jedec[0], jedec[1], jedec[2]);
        }
        printf("SR1=%02X SR2=%02X (QE=%d)\n", sr1, sr2, (sr2 >> 1) & 1);
        printf("flash@0: ");
        for (int i = 0; i < 16; i++) printf("%02X ", hdr[i]);
        printf("\n");
        bool vec = hdr[0] == 0x00 && hdr[1] == 0x00 && hdr[2] == 0x00 && hdr[3] == 0x20;
        printf("boot2 vec = %02X%02X%02X%02X -> %s\n", hdr[0], hdr[1], hdr[2], hdr[3],
               vec ? "VALID firmware written" : "EMPTY / NOT written (or corrupt)");
        fflush(stdout);

        toggle = !toggle;
        gpio_put(STATUS_LED_GPIO, toggle ? 0 : 1); // heartbeat
        sleep_ms(1000);
    }
}
