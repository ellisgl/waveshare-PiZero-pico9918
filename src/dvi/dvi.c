/*
 * Project: pico9918 - dvi
 *
 * DVI (HDMI-compatible) 640x480@60Hz output for RP2040 boards with a directly
 * wired HDMI connector, such as the Waveshare RP2040-PiZero.
 *
 * This is a drop-in replacement for the vga driver: it implements the same API
 * (vga.h) so the rest of the firmware is unchanged. Core 1 renders 12-bit RGB
 * scanlines exactly as it does for VGA. Core 0 TMDS-encodes them from the DMA
 * interrupt and three PIO state machines bit-bang the TMDS lanes.
 *
 * Timing: the system clock must be 252 MHz. Each lane runs at one bit per
 * system clock, giving a 25.2 MHz pixel clock, 800x525 total, 60.0 Hz.
 *
 * This code is licensed under the MIT license
 */

#include "vga.h"
#include "tmds.h"
#include "dvi.pio.h"

#include "../display.h"

#include "pico/multicore.h"
#include "pico/binary_info.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/interp.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"

#include <string.h>

#ifndef PICO9918_DVI_D0_PIN
#error "Board header must define PICO9918_DVI_D0_PIN, D1, D2 and CLK pins for DVI output"
#endif

#ifndef PICO9918_DVI_INVERT_DIFFPAIRS
#define PICO9918_DVI_INVERT_DIFFPAIRS 0
#endif

#ifndef PICO9918_DVI_STRICT_TMDS
#define PICO9918_DVI_STRICT_TMDS 0
#endif

#ifndef PICO9918_DVI_DRIVE_STRENGTH
#define PICO9918_DVI_DRIVE_STRENGTH GPIO_DRIVE_STRENGTH_2MA
#endif

#ifndef PICO9918_DVI_SLEW_RATE
#define PICO9918_DVI_SLEW_RATE GPIO_SLEW_RATE_SLOW
#endif

#if (PICO9918_DVI_CLK_PIN & 1) != 0
#error "PICO9918_DVI_CLK_PIN must be an even GPIO (PWM channel A) with CLK- on the next GPIO"
#endif

bi_decl(bi_2pins_with_names(PICO9918_DVI_D0_PIN, "DVI D0+", PICO9918_DVI_D0_PIN + 1, "DVI D0-"));
bi_decl(bi_2pins_with_names(PICO9918_DVI_D1_PIN, "DVI D1+", PICO9918_DVI_D1_PIN + 1, "DVI D1-"));
bi_decl(bi_2pins_with_names(PICO9918_DVI_D2_PIN, "DVI D2+", PICO9918_DVI_D2_PIN + 1, "DVI D2-"));
bi_decl(bi_2pins_with_names(PICO9918_DVI_CLK_PIN, "DVI CLK+", PICO9918_DVI_CLK_PIN + 1, "DVI CLK-"));

#define DVI_PIO         pio0     // the tms bus and clocks use pio1
#define DVI_LANES       3

/* DMA channels. Others in use (none of them claimed, so dma_channel_claim()
 * can't catch a clash):
 *   2     main.c dma32 (memset)
 *   3, 4  vrEmuTms9918.c dma32 / dma8 (memset, used while rendering scanlines)
 *   5, 6  main.c PALCONV (reserved)
 * Sharing 3 or 4 breaks the DVI chain as soon as core 1 renders a scanline. */
static const uint dataDmaChan[DVI_LANES] = { 0, 1, 7 };
static const uint ctrlDmaChan[DVI_LANES] = { 8, 9, 10 };

/* messages to the render loop on core 1 (same values as the vga driver) */
#define FRONT_PORCH_MSG     0x20000000
#define END_OF_SCANLINE_MSG 0x40000000
#define END_OF_FRAME_MSG    0x80000000

/* fixed 640x480 timing (in pixels / lines) */
#define H_ACTIVE      640
#define H_FRONT       16
#define H_SYNC        96
#define H_BACK        48
#define H_TOTAL       (H_ACTIVE + H_FRONT + H_SYNC + H_BACK)

#define V_ACTIVE      480
#define V_FRONT       10
#define V_SYNC        2
#define V_BACK        33
#define V_TOTAL       (V_ACTIVE + V_FRONT + V_SYNC + V_BACK)

#define LINE_WORDS    (H_TOTAL / 2)   // two symbols per word
#define ACTIVE_WORDS  (H_ACTIVE / 2)

#define ENCODE_LATE_WORDS 48          // irq started > ~10% into the line = late

#define PRIME_LINE    (V_TOTAL - 4)   // request virtual lines 0 and 1 from core 1
#define ENCODE0_LINE  (V_TOTAL - 1)   // encode virtual line 0

/*
 * line buffers. each holds a whole line of symbols (active, front porch, sync,
 * back porch) for one lane. active lines ping-pong between two sets; the
 * blanking portion of those is filled once at init.
 */
static uint32_t __aligned(4) tmdsActive[2][DVI_LANES][LINE_WORDS];
static uint32_t __aligned(4) tmdsBlankD0[LINE_WORDS];       // lane 0, vsync inactive
static uint32_t __aligned(4) tmdsBlankD0Vsync[LINE_WORDS];  // lane 0, vsync active
static uint32_t __aligned(4) tmdsBlankD12[LINE_WORDS];      // lanes 1 and 2 (no sync bits)

/* rendered rgb scanlines (two, odd and even), written by core 1 */
__attribute__((section(".scratch_y.lookup"))) uint16_t __aligned(4) rgbDataBuffer[2][RGB_PIXELS_X] = { 0 };

/*
 * read by the control DMA channels: the buffer each lane sends next. each lane's
 * control channel walks a two-entry ring, so the irq always writes the slot no
 * lane is about to read, even if one lane's DMA runs a few cycles behind another.
 */
static volatile uint32_t __aligned(8) nextLinePtr[DVI_LANES][2];
static uint32_t writeSlot = 0;     // ring slot the irq fills next

static VgaInitParams vgaParams = { 0 };

static uint32_t currentLine = 0;   // output line the DMA is currently sending
static uint32_t frameScale = 1;    // vertical scale, latched once per frame
static uint32_t frameVLines = V_ACTIVE;  // virtual lines, latched once per frame

static uint32_t ctrlIrqMask = 0;


/*
 * control symbol pair for lane 0, given the logical (active = true) sync states
 */
static uint32_t ctrlPair(bool hsyncActive, bool vsyncActive)
{
  static const uint32_t ctrl[4] = { TMDS_CTRL_00, TMDS_CTRL_01, TMDS_CTRL_10, TMDS_CTRL_11 };
  const bool h = hsyncActive == vgaParams.params.hSyncParams.syncHigh;
  const bool v = vsyncActive == vgaParams.params.vSyncParams.syncHigh;
  const uint32_t s = ctrl[(v << 1) | h];
  return s | (s << 10);
}

static void fillWords(uint32_t *dst, uint32_t value, uint32_t count)
{
  while (count--) *dst++ = value;
}

/*
 * fill words [from, LINE_WORDS) of a lane 0 line with blanking + hsync
 */
static void fillHBlankD0(uint32_t *line, uint32_t from, bool vsync)
{
  const uint32_t idle = ctrlPair(false, vsync);
  const uint32_t sync = ctrlPair(true, vsync);

  fillWords(line + from, idle, ACTIVE_WORDS + H_FRONT / 2 - from);
  fillWords(line + ACTIVE_WORDS + H_FRONT / 2, sync, H_SYNC / 2);
  fillWords(line + ACTIVE_WORDS + (H_FRONT + H_SYNC) / 2, idle, H_BACK / 2);
}

static void buildBlankingBuffers()
{
  fillHBlankD0(tmdsBlankD0, 0, false);
  fillHBlankD0(tmdsBlankD0Vsync, 0, true);
  fillWords(tmdsBlankD12, TMDS_CTRL_00 | (TMDS_CTRL_00 << 10), LINE_WORDS);

  // active line buffers: zeroed (black-ish) pixels, then blanking tails
  for (int b = 0; b < 2; ++b)
  {
    for (int lane = 0; lane < DVI_LANES; ++lane)
    {
      fillWords(tmdsActive[b][lane], tmdsPairTable[0], ACTIVE_WORDS);
    }
    fillHBlankD0(tmdsActive[b][0], ACTIVE_WORDS, false);
    fillWords(tmdsActive[b][1] + ACTIVE_WORDS, TMDS_CTRL_00 | (TMDS_CTRL_00 << 10), LINE_WORDS - ACTIVE_WORDS);
    fillWords(tmdsActive[b][2] + ACTIVE_WORDS, TMDS_CTRL_00 | (TMDS_CTRL_00 << 10), LINE_WORDS - ACTIVE_WORDS);
  }
}

/*
 * point the control channels at the buffers for output line 'line'
 */
static inline void __time_critical_func(setNextLine)(uint32_t line, uint32_t slot)
{
  if (line < V_ACTIVE)
  {
    const uint32_t v = (frameScale == 2) ? (line >> 1) : line;
    uint32_t (*buf)[LINE_WORDS] = tmdsActive[v & 1];
    nextLinePtr[0][slot] = (uint32_t)buf[0];
    nextLinePtr[1][slot] = (uint32_t)buf[1];
    nextLinePtr[2][slot] = (uint32_t)buf[2];
  }
  else
  {
    const bool vsync = line >= (V_ACTIVE + V_FRONT) && line < (V_ACTIVE + V_FRONT + V_SYNC);
    nextLinePtr[0][slot] = (uint32_t)(vsync ? tmdsBlankD0Vsync : tmdsBlankD0);
    nextLinePtr[1][slot] = (uint32_t)tmdsBlankD12;
    nextLinePtr[2][slot] = (uint32_t)tmdsBlankD12;
  }
}

static inline void __time_critical_func(encodeVirtualLine)(uint32_t v)
{
  uint32_t (*buf)[LINE_WORDS] = tmdsActive[v & 1];
  tmdsEncodeLine((const uint32_t *)rgbDataBuffer[v & 1], buf[0], buf[1], buf[2], ACTIVE_WORDS);
}

/*
 * dma interrupt: fires as each output line starts (lane 0 control channel done).
 *
 * pipeline, for virtual line v while it is on screen:
 *   - core 1 renders v + 2 into rgb buffer (v & 1), which has already been encoded
 *   - core 0 (here) encodes v + 1 from rgb buffer ((v + 1) & 1)
 */
static void __isr __time_critical_func(dviDmaIrqHandler)(void)
{
  dma_hw->ints0 = ctrlIrqMask;

  if (++currentLine >= V_TOTAL) currentLine = 0;
  const uint32_t line = currentLine;

  // first: queue the buffers for the line after this one
  setNextLine((line + 1 >= V_TOTAL) ? 0 : line + 1, writeSlot);
  writeSlot ^= 1;

  if (line < V_ACTIVE)
  {
    const uint32_t v = (frameScale == 2) ? (line >> 1) : line;
    const uint32_t rpt = line & (frameScale - 1);

    if (rpt == 0)
    {
      const uint32_t request = v + 2;
      if (request < frameVLines)
      {
        multicore_fifo_push_timeout_us(request, 0);
        if (request == frameVLines - 1)
        {
          multicore_fifo_push_timeout_us(END_OF_FRAME_MSG, 0);
        }
      }

      // with 2 output lines per virtual line, the encode (worst ~11.5k cycles)
      // has two lines (16k cycles) of budget. with 1 (480-row mode) it has one
      // line and may overrun: if this irq started late because of that, skip
      // the encode (the line shows stale content) rather than fall further
      // behind and miss line pointer updates.
      if (v + 1 < frameVLines &&
          (frameScale == 2 || dma_hw->ch[dataDmaChan[0]].transfer_count > LINE_WORDS - ENCODE_LATE_WORDS))
      {
        encodeVirtualLine(v + 1);
      }
    }

    if (rpt == frameScale - 1 && v == vgaParams.triggerScanline)
    {
      multicore_fifo_push_timeout_us(END_OF_SCANLINE_MSG | v, 0);
    }
  }
  else if (line == V_ACTIVE + 2)
  {
    multicore_fifo_push_timeout_us(FRONT_PORCH_MSG, 0);
  }
  else if (line == PRIME_LINE)
  {
    // latch the vertical layout for the coming frame (core 1 changes it at end of frame)
    frameScale = (vgaParams.params.vPixelScale == 2) ? 2 : 1;
    frameVLines = vgaParams.params.vVirtualPixels;
    const uint32_t maxVLines = (frameScale == 2) ? (V_ACTIVE >> 1) : V_ACTIVE;  // no divide in an irq
    if (frameVLines > maxVLines) frameVLines = maxVLines;

    multicore_fifo_push_timeout_us(0, 0);
    multicore_fifo_push_timeout_us(1, 0);
  }
  else if (line == ENCODE0_LINE)
  {
    encodeVirtualLine(0);
  }
}

/*
 * the assembly encoder writes t = pair << 2 to ACCUM0 of both interpolators and
 * reads PEEK_FULL = BASE2 + lane0 + lane1 = &tmdsPairTable[c0 | c1 << 4]:
 *   lane0 = (t >> shift0) & 0x03c  -> c0 * 4   (first pixel's nibble)
 *   lane1 = (t >> shift1) & 0x3c0  -> c1 * 64  (second pixel's nibble, via cross input)
 */
static void configureEncoderInterp(interp_hw_t *interp, uint shift0, uint shift1)
{
  interp_config c = interp_default_config();
  interp_config_set_shift(&c, shift0);
  interp_config_set_mask(&c, 2, 5);
  interp_set_config(interp, 0, &c);

  c = interp_default_config();
  interp_config_set_cross_input(&c, true);
  interp_config_set_shift(&c, shift1);
  interp_config_set_mask(&c, 6, 9);
  interp_set_config(interp, 1, &c);

  interp->base[0] = 0;
  interp->base[1] = 0;
  interp->base[2] = (uintptr_t)tmdsPairTable;
}

void __attribute__((noinline)) tmdsEncoderInterpInit(void)
{
  configureEncoderInterp(interp0, 0, 12);   // red:   pixel bits 3:0  and 19:16
  configureEncoderInterp(interp1, 4, 16);   // green: pixel bits 7:4  and 23:20
}

static void configurePad(uint gpio, bool invert)
{
  gpio_set_drive_strength(gpio, PICO9918_DVI_DRIVE_STRENGTH);
  gpio_set_slew_rate(gpio, PICO9918_DVI_SLEW_RATE);
  gpio_set_input_enabled(gpio, false);
  gpio_set_outover(gpio, invert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
}

static void initSerialisers()
{
  static const uint lanePins[DVI_LANES] = { PICO9918_DVI_D0_PIN, PICO9918_DVI_D1_PIN, PICO9918_DVI_D2_PIN };

  pio_add_program_at_offset(DVI_PIO, &dvi_serialiser_program, 0);

  for (uint lane = 0; lane < DVI_LANES; ++lane)
  {
    const uint sm = lane;
    const uint pin = lanePins[lane];

    pio_sm_config c = dvi_serialiser_program_get_default_config(0);
    sm_config_set_sideset_pins(&c, pin);
    sm_config_set_out_shift(&c, true, true, 20);   // R shift, autopull @ 20 bits (2 symbols)
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac(&c, 1, 0);

    pio_sm_set_pins_with_mask(DVI_PIO, sm, 2u << pin, 3u << pin);  // start as TMDS "0"
    pio_sm_set_consecutive_pindirs(DVI_PIO, sm, pin, 2, true);
    pio_gpio_init(DVI_PIO, pin);
    pio_gpio_init(DVI_PIO, pin + 1);
    configurePad(pin, PICO9918_DVI_INVERT_DIFFPAIRS);
    configurePad(pin + 1, PICO9918_DVI_INVERT_DIFFPAIRS);

    pio_sm_init(DVI_PIO, sm, 0, &c);
  }
}

static void initClockLane()
{
  // pixel clock = sys / 10, differential via PWM A and B on one slice.
  // Same as PicoDVI's dvi_serialiser_init (used by Waveshare's own demo for
  // this board): A (CLK+) inverted, B not, and the pair-inversion override
  // applied to the clock pads too. This sets the clock's phase against the
  // data lanes, so keep it identical to the known-working configuration.
  const uint slice = pwm_gpio_to_slice_num(PICO9918_DVI_CLK_PIN);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_output_polarity(&cfg, true, false);
  pwm_config_set_wrap(&cfg, 9);
  pwm_init(slice, &cfg, false);
  pwm_set_both_levels(slice, 5, 5);

  gpio_set_function(PICO9918_DVI_CLK_PIN, GPIO_FUNC_PWM);
  gpio_set_function(PICO9918_DVI_CLK_PIN + 1, GPIO_FUNC_PWM);
  configurePad(PICO9918_DVI_CLK_PIN, PICO9918_DVI_INVERT_DIFFPAIRS);
  configurePad(PICO9918_DVI_CLK_PIN + 1, PICO9918_DVI_INVERT_DIFFPAIRS);
}

static void initDma()
{
  for (uint lane = 0; lane < DVI_LANES; ++lane)
  {
    const uint data = dataDmaChan[lane];
    const uint ctrl = ctrlDmaChan[lane];
    dma_channel_claim(data);
    dma_channel_claim(ctrl);

    // data: one line of words into the lane's PIO FIFO, then hand over to ctrl
    dma_channel_config dc = dma_channel_get_default_config(data);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(DVI_PIO, lane, true));
    channel_config_set_chain_to(&dc, ctrl);
    channel_config_set_high_priority(&dc, true);
    dma_channel_configure(data, &dc, &DVI_PIO->txf[lane], NULL, LINE_WORDS, false);

    // ctrl: load the next line's buffer address into data and retrigger it
    dma_channel_config cc = dma_channel_get_default_config(ctrl);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, true);
    channel_config_set_ring(&cc, false, 3);          // wrap reads within the 8-byte ring
    channel_config_set_write_increment(&cc, false);
    channel_config_set_high_priority(&cc, true);
    dma_channel_configure(ctrl, &cc, &dma_hw->ch[data].al3_read_addr_trig, nextLinePtr[lane], 1, false);
  }

  ctrlIrqMask = 1u << ctrlDmaChan[0];
  dma_channel_set_irq0_enabled(ctrlDmaChan[0], true);
  irq_set_exclusive_handler(DMA_IRQ_0, dviDmaIrqHandler);
  irq_set_enabled(DMA_IRQ_0, true);
}

uint32_t vgaMinimumPioClockKHz(VgaParams* params)
{
  // one bit per clock, ten bits per pixel
  return params ? params->pixelClockKHz * 10 : 0;
}

/*
 * main render loop (core 1). identical to the vga driver's loop.
 */
void __time_critical_func(vgaLoop)()
{
  if (vgaParams.initFn)
  {
    vgaParams.initFn();
  }

  uint32_t frameNumber = 0;
  while (1)
  {
    uint32_t message = multicore_fifo_pop_blocking();

    if (message == FRONT_PORCH_MSG)
    {
      if (vgaParams.porchFn)
      {
        vgaParams.porchFn();
      }
    }
    else if (message == END_OF_FRAME_MSG)
    {
      if (vgaParams.endOfFrameFn)
      {
        vgaParams.endOfFrameFn(frameNumber);
        ++frameNumber;
      }
    }
    else if ((message & END_OF_SCANLINE_MSG) != 0)
    {
      if (vgaParams.endOfScanlineFn)
      {
        vgaParams.endOfScanlineFn(message & 0x0fff);
      }
    }
    else
    {
      bool doEof = false;
      if (message != 0)
      {
        while (multicore_fifo_rvalid()) // if we dropped a scanline, no point rendering it now
        {
          uint32_t nextMessage = sio_hw->fifo_rd;
          if (nextMessage == END_OF_FRAME_MSG)
          {
            doEof = true;
          }
          else
          {
            message = nextMessage;
          }
        }
      }

      vgaParams.scanlineFn(message & 0x0fff, &vgaParams.params, rgbDataBuffer[message & 0x01]);
      if (doEof && vgaParams.endOfFrameFn)
      {
        vgaParams.endOfFrameFn(frameNumber);
        ++frameNumber;
      }
    }
  }
}

/*
 * initialise dvi output. call from core 0 (the encoder runs in core 0's dma irq).
 */
void vgaInit(VgaInitParams params)
{
  vgaParams = params;

  // dvi is progressive only, and needs 252 MHz for a 25.2 MHz pixel clock
  vgaParams.params.interlaced = false;
  vgaParams.params.numFields = 1;

  tmdsInitTables(PICO9918_DVI_STRICT_TMDS);
  tmdsEncoderInterpInit();   // this core (0) runs the encoder from the dma irq
  buildBlankingBuffers();

  // start in the vertical front porch: the first frame gets primed normally
  currentLine = V_ACTIVE;
  setNextLine(V_ACTIVE, 1);  // slot 1 is free until the first irq; borrow it
  const uint32_t first[DVI_LANES] = { nextLinePtr[0][1], nextLinePtr[1][1], nextLinePtr[2][1] };
  setNextLine(V_ACTIVE + 1, 0);  // first chained load reads slot 0
  writeSlot = 1;

  initSerialisers();
  initClockLane();
  initDma();

  // data channels start on the first blanking line, then chain through nextLinePtr
  uint32_t dataMask = 0;
  for (uint lane = 0; lane < DVI_LANES; ++lane)
  {
    dma_channel_set_read_addr(dataDmaChan[lane], (const void *)first[lane], false);
    dataMask |= 1u << dataDmaChan[lane];
  }
  dma_start_channel_mask(dataMask);

  // let the FIFOs fill, then start all lanes on the same cycle, and the clock
  while (!pio_sm_is_tx_fifo_full(DVI_PIO, 0) || !pio_sm_is_tx_fifo_full(DVI_PIO, 1) || !pio_sm_is_tx_fifo_full(DVI_PIO, 2))
    tight_loop_contents();

  pio_enable_sm_mask_in_sync(DVI_PIO, 0x7);
  pwm_set_enabled(pwm_gpio_to_slice_num(PICO9918_DVI_CLK_PIN), true);
}

VgaInitParams *vgaCurrentParams()
{
  return &vgaParams;
}

void vgaSetTriggerScanline(uint32_t scanline)
{
  vgaParams.triggerScanline = scanline;
}
