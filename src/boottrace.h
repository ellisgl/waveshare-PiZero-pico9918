/*
 * Bring-up trace (PICO9918_BOOT_TRACE=ON): lets a host see how far core 0 got
 * through start-up, over the TMS9918 bus alone.
 *
 * After the F18A unlock (write 0x1C to VR57 twice), selecting status register
 * 4 or 5 with VR15 and reading the status port returns:
 *   SR4  bootTraceStage: last start-up stage reached (BT_* below), with
 *        bit 7 (BT_HARDFAULT) set once any hard fault has happened. The
 *        existing handler (gpu.c, meant for F18A GPU MPU faults) returns, so
 *        a real fault repeats forever and the stage shows where it stuck.
 *   SR5  low byte of the DVI DMA interrupt count (changes ~31k times a second
 *        when DVI is running)
 * Both live outside the emulator's status registers, so a host RESET (which
 * clears those) doesn't wipe them.
 *
 * This code is licensed under the MIT license
 */
#pragma once

#include <stdint.h>

#if PICO9918_BOOT_TRACE

extern volatile uint8_t bootTraceStage;
extern volatile uint32_t bootTraceDviIrqs;
#define BOOT_TRACE(stage)  do { bootTraceStage = (stage); __asm volatile ("" ::: "memory"); } while (0)
#define BOOT_TRACE_DVI_IRQ() (++bootTraceDviIrqs)

#else

#define BOOT_TRACE(stage)  do { } while (0)
#define BOOT_TRACE_DVI_IRQ() do { } while (0)

#endif

#define BT_HARDFAULT        0x80

/* core 0 start-up stages (main.c) */
#define BT_MAIN_START       0x01
#define BT_EMU_INIT         0x02
#define BT_CORE1_LAUNCHED   0x03
#define BT_CONFIG_READ      0x04
#define BT_CLOCK_SET        0x05
#define BT_CLOCKS_OUT       0x06
#define BT_VGA_INIT_CALL    0x07
/* inside vgaInit (dvi.c) */
#define BT_DVI_TABLES       0x10
#define BT_DVI_INTERP       0x11
#define BT_DVI_BLANKING     0x12
#define BT_DVI_SERIALISERS  0x13
#define BT_DVI_CLOCK_LANE   0x14
#define BT_DVI_DMA          0x15
#define BT_DVI_DMA_STARTED  0x16
#define BT_DVI_FIFOS_FULL   0x17
#define BT_DVI_RUNNING      0x18
/* back in main.c */
#define BT_TEMPERATURE      0x20
#define BT_DIAGNOSTICS      0x21
#define BT_CORE1_SIGNALLED  0x22
#define BT_MAIN_LOOP        0x23
