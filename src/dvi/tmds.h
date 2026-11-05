/*
 * Project: pico9918 - dvi
 *
 * TMDS symbol tables and scanline encoder.
 *
 * This file has no Pico SDK dependencies so it can be unit tested on a host.
 *
 * This code is licensed under the MIT license
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* DVI control tokens, indexed by (c1 << 1) | c0 (c0 = HSYNC, c1 = VSYNC on lane 0).
 * Bit 0 is transmitted first. */
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

/*
 * pair table: index = (first pixel level) | (second pixel level << 4) with 4-bit
 * levels. value = two 10-bit symbols packed into bits 0-19, first symbol in bits 0-9.
 *
 * every symbol in the table is individually DC-balanced (5 ones, 5 zeros), so no
 * running disparity is required and each symbol pair can be looked up independently.
 */
extern uint32_t tmdsPairTable[256];

/*
 * build the symbol tables.
 *
 * strict = false: use any decodable, DC-balanced, non-control symbol. Gives exact
 *                 black (0) and white (255); levels without one use the nearest
 *                 value within a few LSBs.
 * strict = true:  only use symbols a DVI-spec encoder could emit. Black becomes 16
 *                 and white 239 - use this if a display misbehaves.
 */
void tmdsInitTables(bool strict);

/* the 10-bit symbol and 8-bit value chosen for a 4-bit level (after tmdsInitTables) */
uint16_t tmdsLevelSymbol(uint8_t level);
uint8_t tmdsLevelValue(uint8_t level);

/* encode an 8-bit value as a 10-bit symbol: xnor/invert select the q_m mode and q_out[9] */
uint16_t tmdsEncodeSymbol(uint8_t value, bool xnor, bool invert);

/* decode a 10-bit data symbol back to its 8-bit value (as a DVI receiver does) */
uint8_t tmdsDecodeSymbol(uint16_t symbol);

/*
 * encode a scanline of 12-bit pixels into three TMDS lanes.
 *
 * src:   pixel pairs, one uint32_t per two pixels (first pixel in bits 0-15).
 *        pixel format: bits 0-3 red, 4-7 green, 8-11 blue, 12-15 ignored.
 * lane0: blue  symbol pairs (DVI channel 0)
 * lane1: green symbol pairs (DVI channel 1)
 * lane2: red   symbol pairs (DVI channel 2)
 */
void tmdsEncodeLine(const uint32_t *src, uint32_t *lane0, uint32_t *lane1, uint32_t *lane2, uint32_t pairCount);

/* portable C implementation of tmdsEncodeLine() (the reference for the RP2040 assembly) */
void tmdsEncodeLineGeneric(const uint32_t *src, uint32_t *lane0, uint32_t *lane1, uint32_t *lane2, uint32_t pairCount);

/*
 * RP2040 only: configure THIS core's interp0/interp1 for the assembly encoder.
 * tmdsEncodeLine() must then only be called from this core.
 */
void tmdsEncoderInterpInit(void);
