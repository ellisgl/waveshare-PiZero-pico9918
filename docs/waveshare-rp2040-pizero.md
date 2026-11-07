# PICO9918 on the Waveshare RP2040-PiZero

This fork can run the PICO9918 firmware on an off-the-shelf
[Waveshare RP2040-PiZero](https://www.waveshare.com/wiki/RP2040-PiZero) instead of a
custom PICO9918 PCB. Video comes out of the board's own **mini HDMI** connector as
DVI (640x480 @ 60 Hz, accepted by HDMI monitors and TVs), so no VGA resistor DAC is
needed.

You still need a small **carrier board** between the retro computer's 40-pin VDP
socket and the PiZero's 40-pin header, for 5V to 3.3V level shifting and power
(see [Carrier board](#carrier-board)).

## Building

```bash
cmake -S . -B build-pizero -G Ninja -DPICO_BOARD=waveshare_rp2040_pizero \
      -DPICO_SDK_FETCH_FROM_GIT=ON -DPICO_SDK_FETCH_FROM_GIT_TAG=2.1.1
cmake --build build-pizero --target firmware
```

Output: `build-pizero/dist/pico9918-waveshare_rp2040_pizero-dvi-v<version>.uf2`.
Hold BOOT, plug in USB-C (with the board **not** fitted to the carrier/host),
and copy the `.uf2` to the `RPI-RP2` drive.

Selecting this board turns on `PICO9918_DISPLAY_DVI` and turns off SCART. Options
(set with `-D...` or in `pico9918_config.cmake`):

| Option | Default | Meaning |
|---|---|---|
| `PICO9918_DVI_STRICT_TMDS` | `ON` | Only send TMDS symbols a spec encoder would produce (black is level 16, white 239). With `OFF`, black and white use the symbols 0x255 and 0x155, which have 8–9 bit transitions. Receivers can take those for control tokens and lose sync, and a tested monitor turned its backlight off. |
| `PICO9918_NO_CLOCKS` | `OFF` | Don't drive GROMCLK/CPUCLK. |
| `PICO9918_GPIO_*` | see below | Move bus signals. CMake checks the constraints. |

## Pin mapping

The 40-pin header follows the Raspberry Pi layout, but the GPIO numbers are
RP2040 GPIOs, not BCM numbers. All levels at the header are **3.3V**.

| Signal | TMS9918A pin | RP2040 GPIO | PiZero header pin |
|---|---|---|---|
| CD7 | 24 | GPIO8  | 24 |
| CD6 | 23 | GPIO9  | 32 |
| CD5 | 22 | GPIO10 | 23 |
| CD4 | 21 | GPIO11 | 19 |
| CD3 | 20 | GPIO12 | 21 |
| CD2 | 19 | GPIO13 | 33 |
| CD1 | 18 | GPIO14 | 7  |
| CD0 | 17 | GPIO15 | 29 |
| MODE | 13 | GPIO17 | 11 |
| /CSR | 15 | GPIO18 | 12 |
| /CSW | 14 | GPIO19 | 35 |
| /INT | 16 | GPIO20 | 38 |
| RESET | 34 | GPIO21 | 40 |
| GROMCLK | 37 | GPIO4 | 8 |
| CPUCLK | 38 | GPIO5 | 10 |
| MODE1 (V9938, optional) | – | GPIO7 | 26 |
| +5V in | 33 | VBUS | 2, 4 (see power) |
| GND | 12 | GND | 6, 9, 14, 20, 25, 30, 34, 39 |

**Leave these header pins unconnected:**

| Header pins | GPIO | Why |
|---|---|---|
| 13, 15, 16, 18, 22, 37 | GPIO27, 22, 23, 24, 25, 26 | These carry the 252 Mbit/s TMDS signals to the mini HDMI. Any trace on the carrier is a stub that degrades the picture. |
| 3, 5, 36 | GPIO2, 3, 16 | Wired to the HDMI DDC (SDA/SCL) and CEC lines of the attached monitor. |
| 31 | GPIO6 | 2.2K pull-up for the PIO-USB port. Free, but avoid it. |

Notes from the board schematic:
- GPIO18–21 have 47K pull-ups for the TF card slot. That's harmless here (/CSR,
  /CSW, /INT and RESET all idle high). Don't insert a TF card.
- GPIO28/29 (DVI clock) don't reach the header.
- Constraints if you remap pins: CD7..CD0 must be 8 consecutive GPIOs, and MODE
  must be 8–15 GPIOs above CD7, because the write PIO samples 16 pins starting at
  CD7. No bus pin may be GPIO22–29.

## Carrier board

The RP2040 is **not 5V tolerant**. Copy the level-shifting front end of the
PICO9918 v1.3 (`pcb/v1.3/pico9918_v1_3_schematic.pdf`, sheet 1, "I/O (DATA)",
"INPUTS" and "OUTPUTS"), feeding its 3.3V side to the GPIOs above:

- **Data bus:** 74HC245 (5V) back-to-back with a 74LVC245 (3.3V). DIR = /CSR, so
  the bus drives toward the host only during a read. /OE is held off while the
  host is in RESET.
- **Inputs** (MODE, MODE1, /CSW, /CSR, RESET): 74LVC245 at 3.3V with DIR fixed
  (its inputs are 5V tolerant).
- **Outputs** (GROMCLK, CPUCLK, /INT): 74AHC126 at 5V.

**Power:** header pins 2/4 are the board's USB VBUS rail, wired directly to the
USB-C connector. If you feed host +5V there, put a Schottky or ideal diode
(v1.3 uses an LM66200) between host 5V and VBUS. Otherwise plugging in USB-C
while the host is on back-feeds the host and the PC into each other.

### Driving it from a 3.3V host

A 3.3V host such as an FPGA or a 3.3V microcontroller can wire straight to the header
pins in the pin mapping table, with no level shifting. Power the PiZero from its USB-C,
share GND, and leave the 5V pins (2, 4) and the HDMI pins above unconnected. A Mojo V3
(Spartan-6) host worked with this bus timing:

| | |
|---|---|
| MODE/CD setup before /CSW or /CSR falls | 60 ns |
| /CSW low | 300 ns |
| /CSR low (sample CD at the end) | 400 ns |
| Hold after the strobe rises | 60 ns |
| Gap between accesses | 2 µs |

Every access interrupts the RP2040, which has to update its read-ahead before the next
read. Keep the gap at about 2 µs, which is also what a real TMS9918A needs. With
unbuffered wiring, keep the RESET wire short and away from the data and strobe lines. The
firmware ignores RESET pulses shorter than 2 µs, because a false reset blanks the display
and turns off /INT.

## How the DVI output works

- `src/dvi/` replaces `src/vga/` and implements the same `vga.h` API, so the
  renderer is unchanged.
- Three PIO state machines on `pio0` serialise one TMDS lane each at one bit per
  system clock (252 MHz, so the pixel clock is 25.2 MHz). PWM slice 6 generates
  the differential pixel clock. DMA control channels chain a whole line buffer
  per lane from a 2-slot pointer ring.
- Core 1 renders scanlines exactly as for VGA. Core 0, which otherwise only runs
  the F18A GPU, encodes each line in the DMA interrupt using an assembly encoder
  and the core's interpolators.
- Every 4-bit colour level uses a DC-balanced TMDS symbol, so no running
  disparity is needed and the 80-column mode works at full resolution. Levels
  3, 6, 9 and 12 are off by 1–2/255.
- The system clock is fixed at 252 MHz. The configurator's clock presets are
  ignored on this build.

### Limitations

- **CRT scanline effect** is not implemented for DVI. The setting is ignored.
- **F18A GPU speed:** the encoder takes an estimated 35–70% of core 0 while video is
  active (depending on picture content), and the GPU gets the rest. Programs that
  depend on exact GPU timing may run slower than on a PICO9918.
- **480-row mode** (R0 bit 3, a PICO9918 extension) gives each output line its
  own row, which halves the encode budget. Busy lines may show stale content
  rather than lose sync.
- No HDMI audio, EDID or hot-plug handling. The output is always 640x480.
- **Not yet tested with a retro computer.** An FPGA host on direct 3.3V
  wiring has exercised the bus, /INT and the DVI output on a real monitor (see
  [Driving it from a 3.3V host](#driving-it-from-a-33v-host)). The tests below
  also cover the encoder and pipeline in emulation.

## Tests

```bash
# TMDS tables + C encoder (host)
cc -O2 -Wall -I src/dvi test/dvi/tmds_test.c src/dvi/tmds.c -o tmds_test && ./tmds_test

# RP2040 assembly encoder and full DVI line pipeline, running the real firmware
# ELF under the Unicorn emulator (pip install unicorn; needs arm-none-eabi-gcc)
python3 test/dvi/tmds_asm_test.py      build-pizero/src/pico9918-*.elf
python3 test/dvi/dvi_pipeline_test.py  build-pizero/src/pico9918-*.elf
```

The pipeline test checks, over several frames, the line count and vsync
placement, the blanking contents, that every active line carries the right
rendered row, that no buffer changes while it is being sent, and the render
request and interrupt message sequence.

On real hardware, check these first: that the monitor locks (keep
`PICO9918_DVI_STRICT_TMDS=ON`, the default), the colours, and the diagnostics
overlay's frame rate while running F18A GPU software.
