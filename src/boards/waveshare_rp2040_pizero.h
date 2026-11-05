/*
 * Waveshare RP2040-PiZero board definition for PICO9918
 *
 * https://www.waveshare.com/wiki/RP2040-PiZero
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// pico_cmake_set PICO_PLATFORM=rp2040

#pragma once

// For board detection
#define PICO9918_WAVESHARE_PIZERO

// This board has no PICO9918 v0.3 heritage. The v0.3 probe toggles pulls on
// GPIO_RESET and, if it misfires, moves the clock outputs onto the v0.3 pins.
// Skip it and always report v1.x hardware.
#define PICO9918_SKIP_HWVER_DETECT 1

// Board wiring (from the Waveshare RP2040-PiZero schematic)
//
//   GPIO22/23  DVI D2+/D2-  (mini HDMI, 200R series)   also on header pins 15/16
//   GPIO24/25  DVI D1+/D1-                             also on header pins 18/22
//   GPIO26/27  DVI D0+/D0-                             also on header pins 37/13
//   GPIO28/29  DVI CLK+/CLK- (not on the 40-pin header)
//   GPIO2/3    HDMI DDC SDA/SCL (2K2 series)           also on header pins 3/5
//   GPIO16     HDMI CEC         (2K2 + 200R series)    also on header pin 36
//   GPIO6/7    PIO-USB D+/D- (22R series, 2K2 pull-up on GPIO6)
//   GPIO18-21  TF card SPI (47K pull-ups to 3V3)
//   12 MHz crystal, W25Q128JV 16 MB flash, no user LED

// --- DVI ---
#ifndef PICO9918_DVI_D0_PIN
#define PICO9918_DVI_D0_PIN 26   // D0+ (blue, sync); D0- = D0+ + 1
#endif
#ifndef PICO9918_DVI_D1_PIN
#define PICO9918_DVI_D1_PIN 24   // D1+ (green)
#endif
#ifndef PICO9918_DVI_D2_PIN
#define PICO9918_DVI_D2_PIN 22   // D2+ (red)
#endif
#ifndef PICO9918_DVI_CLK_PIN
#define PICO9918_DVI_CLK_PIN 28  // CLK+ (PWM slice 6 A); CLK- = 29 (slice 6 B)
#endif
#ifndef PICO9918_DVI_INVERT_DIFFPAIRS
#define PICO9918_DVI_INVERT_DIFFPAIRS 0  // + is the lower GPIO of each pair
#endif

// --- UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
// power LED only - no PICO_DEFAULT_LED_PIN

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 32
#endif

#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 0
#endif
