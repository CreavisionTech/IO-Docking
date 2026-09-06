#ifndef BOARD_H
#define BOARD_H

// Board pin mapping. See Doc/RP2040_IO.md.
#include "hardware/pwm.h"

#define IO_GPIO_BASE  20u
#define IO_COUNT      6u   // IO1..IO6 = GPIO20..25

// PWM channel mapping: index 0..3 -> {gpio, slice, A/B channel}
typedef struct {
    uint8_t gpio;
    uint8_t slice;
    uint8_t chan;   // PWM_CHAN_A or PWM_CHAN_B
} pwm_map_t;

// PWM1=GPIO7 (slice3 B), PWM2=GPIO8 (slice4 A),
// PWM3=GPIO9 (slice4 B), PWM4=GPIO10 (slice5 A).
// NOTE: PWM2 and PWM3 share slice 4 -> same frequency, phase-locked.
static const pwm_map_t PWM_MAP[4] = {
    { 7,  3, PWM_CHAN_B },
    { 8,  4, PWM_CHAN_A },
    { 9,  4, PWM_CHAN_B },
    { 10, 5, PWM_CHAN_A },
};
#define PWM_COUNT 4u

#define USART0_TX_GPIO 0u
#define USART0_RX_GPIO 1u
#define USART1_TX_GPIO 4u
#define USART1_RX_GPIO 5u

#define I2C0_SDA_GPIO 12u
#define I2C0_SCL_GPIO 13u

#define SPI0_MOSI_GPIO 19u
#define SPI0_MISO_GPIO 16u
#define SPI0_SCK_GPIO  18u
#define SPI0_CS_GPIO   6u

#define ADC_GPIO_BASE 26u
#define ADC_COUNT     3u   // ADC0..ADC2 = GPIO26..28 (ADC ch0..2)

// Status LED (LED command): reuse IIC_TX_LED, a dedicated activity LED.
#define STATUS_LED_GPIO 2u

// UART RX ring size per channel.
#define UART_RX_RING 1024u

#endif
