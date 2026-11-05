/*
 * Project: pico9918 - dvi
 *
 * RP2040 (Cortex-M0+) TMDS scanline encoder. Same contract as the C version
 * in tmds.c:
 *
 *   void tmdsEncodeLine(const uint32_t *src, uint32_t *laneB, uint32_t *laneG,
 *                       uint32_t *laneR, uint32_t pairCount);
 *
 * src holds pixel pairs (first pixel in bits 0-15, each pixel 0x?BGR). For each
 * pair, each lane gets tmdsPairTable[c0 | c1 << 4] where c0/c1 are that lane's
 * 4-bit levels.
 *
 * Index arithmetic for red and green is done by the core's two interpolators,
 * blue by the ALU. A pair identical to the previous one reuses the cached lane
 * words (borders and solid areas). Approximate cost per pair on M0+:
 *   new pair: 36 cycles, repeated pair: 17 cycles
 *
 * REQUIRES tmdsEncoderInterpInit() to have been called ON THE CALLING CORE:
 *   t = pair << 2 is written to both ACCUM0s, and
 *   interp0 FULL = &tmdsPairTable[R0 | R1 << 4]
 *   interp1 FULL = &tmdsPairTable[G0 | G1 << 4]
 *
 * This code is licensed under the MIT license
 */

#include "hardware/regs/addressmap.h"
#include "hardware/regs/sio.h"

.syntax unified
.cpu cortex-m0plus
.thumb

#define INTERP_BASE     (SIO_BASE + SIO_INTERP0_ACCUM0_OFFSET)
#define I0_ACCUM0       (SIO_INTERP0_ACCUM0_OFFSET    - SIO_INTERP0_ACCUM0_OFFSET)
#define I0_PEEK_FULL    (SIO_INTERP0_PEEK_FULL_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)
#define I1_ACCUM0       (SIO_INTERP1_ACCUM0_OFFSET    - SIO_INTERP0_ACCUM0_OFFSET)
#define I1_PEEK_FULL    (SIO_INTERP1_PEEK_FULL_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)

.section .time_critical.tmdsEncodeLine, "ax"
.global tmdsEncodeLine
.type tmdsEncodeLine, %function
.thumb_func
tmdsEncodeLine:
    // r0 = src, r1 = laneB, r2 = laneG, r3 = laneR, [sp] = pairCount
    push    {r4-r7, lr}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    push    {r4-r7}

    ldr     r4, [sp, #36]           // pairCount (9 registers pushed)
    cmp     r4, #0
    beq     .Ldone
    lsls    r4, r4, #2
    adds    r4, r0
    mov     ip, r4                  // ip  = end of src
    ldr     r4, =tmdsPairTable
    mov     r8, r4                  // r8  = pair table (blue lookup)
    ldr     r4, =INTERP_BASE        // r4  = interpolator registers
    ldr     r5, [r0]
    mvns    r5, r5
    mov     lr, r5                  // lr  = previous pair (forced mismatch)
                                    // r9/r10/r11 = cached blue/green/red words

.Lloop:
    ldmia   r0!, {r5}               // r5 = pixel pair
    cmp     r5, lr
    beq     .Lrepeat
    mov     lr, r5

    lsls    r6, r5, #2              // r6 = t = pair << 2 (word-scaled nibbles)
    str     r6, [r4, #I0_ACCUM0]
    str     r6, [r4, #I1_ACCUM0]

    ldr     r7, [r4, #I0_PEEK_FULL] // red
    ldr     r7, [r7]
    mov     r11, r7
    stmia   r3!, {r7}

    ldr     r7, [r4, #I1_PEEK_FULL] // green
    ldr     r7, [r7]
    mov     r10, r7
    stmia   r2!, {r7}

    lsrs    r5, r6, #8              // blue: B0 = t[13:10] -> bits 5:2
    movs    r7, #0x3c
    ands    r5, r7
    lsrs    r6, r6, #20             //       B1 = t[29:26] -> bits 9:6
    lsls    r7, r7, #4
    ands    r6, r7
    orrs    r5, r6
    add     r5, r8
    ldr     r5, [r5]
    mov     r9, r5
    stmia   r1!, {r5}

    cmp     r0, ip
    bne     .Lloop
    b       .Ldone

.Lrepeat:
    mov     r7, r11
    stmia   r3!, {r7}
    mov     r7, r10
    stmia   r2!, {r7}
    mov     r7, r9
    stmia   r1!, {r7}
    cmp     r0, ip
    bne     .Lloop

.Ldone:
    pop     {r4-r7}
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    pop     {r4-r7, pc}

.ltorg
.size tmdsEncodeLine, . - tmdsEncodeLine
