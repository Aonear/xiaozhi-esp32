#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ESP32-C3 Super Mini + mic INMP441. Suara keluar lewat UART ke CYD (speaker CYD).
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000   // harus sama dengan sample rate yang dikirim ke CYD

// INMP441 (pin L/R disambung ke GND = kanal kiri)
#define MIC_I2S_GPIO_SCK  GPIO_NUM_5
#define MIC_I2S_GPIO_WS   GPIO_NUM_4
#define MIC_I2S_GPIO_DIN  GPIO_NUM_6

// UART ke CYD: C3 TX(GPIO21) -> CYD IO27, C3 RX(GPIO20) <- CYD IO22, GND bersama
#define CYD_UART_TX_GPIO  GPIO_NUM_21
#define CYD_UART_RX_GPIO  GPIO_NUM_20
#define CYD_UART_BAUD     921600

#define BOOT_BUTTON_GPIO  GPIO_NUM_9

#endif // _BOARD_CONFIG_H_
