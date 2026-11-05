# PICO9918 for the Waveshare RP2040-PiZero

A TMS9918A video chip replacement built on an off-the-shelf
[Waveshare RP2040-PiZero](https://www.waveshare.com/wiki/RP2040-PiZero), with HDMI output
straight from the board's own mini HDMI port.

This is a fork of Troy Schrapel's [PICO9918](https://github.com/visrealm/pico9918), which
does the same job on a custom PCB. All of the TMS9918A and F18A emulation comes from that
project and its [vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918) library. This fork
adds a board definition, a DVI video driver and a pin layout that suit the PiZero.

> [!WARNING]
> **Not yet tested on real hardware.** The firmware builds. The video encoder and the full
> scanline pipeline have been verified by running the real firmware in an emulator (see
> [Tests](#tests)), but nothing has been run on a PiZero plugged into a retro computer yet.

## Contents

* [What you need](#what-you-need)
* [Video output](#video-output)
* [Pinout](#pinout)
* [Building](#building)
* [Flashing](#flashing)
* [Configuration](#configuration)
* [Limitations](#limitations)
* [Tests](#tests)
* [Repository layout](#repository-layout)
* [Credits](#credits)
* [Licensing](#licensing)

## What you need

- A **Waveshare RP2040-PiZero** (RP2040, 16 MB flash, mini HDMI, Pi Zero form factor).
- A **carrier board** that plugs into the computer's 40-pin VDP socket and the PiZero's
  40-pin header. It has to provide:
  - **5V to 3.3V level shifting.** The RP2040 is not 5V tolerant.
  - **Power** from the socket's +5V, through a diode so USB-C and the computer can't
    back-feed each other.

  The carrier can copy the level-shifting front end of the original PICO9918 v1.3
  ([schematic](pcb/v1.3/pico9918_v1_3_schematic.pdf), sheet 1). See
  [docs/waveshare-rp2040-pizero.md](docs/waveshare-rp2040-pizero.md#carrier-board) for details.
- An HDMI or DVI display that accepts 640x480 @ 60 Hz, and a mini HDMI cable or adapter.

## Video output

| | |
|---|---|
| Connector | The PiZero's mini HDMI port |
| Signal | DVI, 640x480 @ 60 Hz progressive. HDMI monitors and TVs accept it. |
| Audio | None |
| VGA / SCART | Not supported in this fork |

The firmware drives the HDMI lines directly from the RP2040: three PIO state machines
serialise the TMDS data at 252 Mbit/s per lane, and PWM generates the pixel clock. How it
works is described in the [board guide](docs/waveshare-rp2040-pizero.md#how-the-dvi-output-works).

## Pinout

All header signals are **3.3V**. GPIO numbers are RP2040 GPIOs, not Raspberry Pi BCM numbers.

| Signal | TMS9918A pin | GPIO | Header pin |
|---|---|---|---|
| CD7 – CD0 | 24 – 17 | GPIO8 – GPIO15 | 24, 32, 23, 19, 21, 33, 7, 29 |
| MODE | 13 | GPIO17 | 11 |
| /CSR | 15 | GPIO18 | 12 |
| /CSW | 14 | GPIO19 | 35 |
| /INT | 16 | GPIO20 | 38 |
| RESET | 34 | GPIO21 | 40 |
| GROMCLK | 37 | GPIO4 | 8 |
| CPUCLK | 38 | GPIO5 | 10 |
| MODE1 (optional) | – | GPIO7 | 26 |
| +5V | 33 | VBUS | 2, 4 |
| GND | 12 | GND | 6, 9, 14, 20, 25, 30, 34, 39 |

> [!IMPORTANT]
> The carrier must leave these header pins unconnected:
> - **13, 15, 16, 18, 22, 37** carry the HDMI signals.
> - **3, 5, 36** connect to the monitor's DDC and CEC lines.
>
> Header pins 2 and 4 are wired straight to the USB-C power rail, so feed them through a diode.

The full per-pin table, the reasons behind the layout, and the rules for moving pins are in
[docs/waveshare-rp2040-pizero.md](docs/waveshare-rp2040-pizero.md#pin-mapping).

## Building

You need CMake, Python 3 with Pillow, an ARM GNU toolchain and **Raspberry Pi Pico SDK
2.1.1** (newer SDKs may cause problems).

```bash
git clone --recurse-submodules https://github.com/ellisgl/waveshare-PiZero-pico9918.git
cd waveshare-PiZero-pico9918

cmake -S . -B build-pizero -DPICO_BOARD=waveshare_rp2040_pizero \
      -DPICO_SDK_FETCH_FROM_GIT=ON -DPICO_SDK_FETCH_FROM_GIT_TAG=2.1.1
cmake --build build-pizero --target firmware
```

The firmware is written to `build-pizero/dist/pico9918-waveshare_rp2040_pizero-dvi-v<version>.uf2`.

Build options can be passed as `-D` flags or put in a git-ignored `pico9918_config.cmake`
(start from [`pico9918_config.cmake.template`](pico9918_config.cmake.template)). Options
specific to this board:

| Option | Default | Effect |
|---|---|---|
| `PICO9918_DVI_STRICT_TMDS` | `OFF` | Use only TMDS symbols a standard encoder would produce. Try this if a display shows sparkles or won't lock. Black becomes slightly grey (16/255) and white 239/255. |
| `PICO9918_NO_CLOCKS` | `OFF` | Don't drive the GROMCLK/CPUCLK outputs. |
| `PICO9918_GPIO_*` | see [Pinout](#pinout) | Move bus signals. The build checks for clashes and invalid layouts. |

The upstream boards still build from this tree (`-DPICO_BOARD=pico9918` or `pico9918pro`)
and produce the same code as upstream. See [BUILDING.md](BUILDING.md) for toolchain setup
on each platform.

## Flashing

1. Take the PiZero **off the carrier**, so the computer and USB aren't both powering it.
2. Hold **BOOT** and plug in USB-C. A drive called `RPI-RP2` appears.
3. Copy the `.uf2` file onto it. The board reboots when the copy finishes.

> [!CAUTION]
> Don't install firmware from the upstream PICO9918 releases, or use the upstream
> configurator's firmware-update feature. Those images are built for the PICO9918 PCBs, use
> different pins, and output VGA.

## Configuration

Settings are stored in flash in the same place and format as upstream, so the PICO9918
configurator (running on the retro computer) and the
[web-based configurator](https://visrealm.github.io/pico9918/config/index.html) should work
for settings such as the palette, sprite limit and diagnostic overlays. This hasn't been
tested on this board yet.

Settings this build ignores:

- **Clock rate.** It is fixed at 252 MHz because the HDMI timing depends on it.
- **CRT scanline effect.** Not implemented for DVI.
- **Display output (VGA/SCART).** DVI is the only output.

The [upstream wiki](https://github.com/visrealm/pico9918/wiki) documents the configurator,
the diagnostic overlays, the
[F18A registers](https://github.com/visrealm/pico9918/wiki/F18A-Programmers-Reference) and
per-machine notes.

## Limitations

- **Untested on hardware** (see the warning at the top).
- **F18A GPU speed.** Core 0 encodes the video and also runs the F18A GPU. Encoding takes
  an estimated 35–70% of that core, depending on the picture, so GPU-heavy F18A software
  may run slower than on a PICO9918.
- **480-row mode** (R0 bit 3, a PICO9918 extension) gives the encoder half the time per line.
  Busy lines may show stale content, but the picture shouldn't lose sync.
- No audio, EDID or hot-plug detection. The output is always 640x480.
- Three of the 16 colour levels per channel are off by 1–2/255. You won't see the difference.

**Machine compatibility.** Upstream PICO9918 works on TI-99/4A, MSX, ColecoVision,
SG-1000, NABU, Memotech and many more machines (see the
[upstream list](https://github.com/visrealm/pico9918/wiki/Supported-Devices)). This fork
uses the same emulation, but bus timing on a carrier board is a separate question, and no
machine has been tested with it yet.

## Tests

```bash
# TMDS symbol tables and encoder (runs on the PC)
cc -O2 -Wall -I src/dvi test/dvi/tmds_test.c src/dvi/tmds.c -o tmds_test && ./tmds_test

# These run the real firmware under an ARM emulator. They need `pip install unicorn`
# and arm-none-eabi-gcc on the PATH.
python3 test/dvi/tmds_asm_test.py     build-pizero/src/pico9918-*.elf
python3 test/dvi/dvi_pipeline_test.py build-pizero/src/pico9918-*.elf
```

- **tmds_test.c** checks the symbol tables and the C encoder.
- **tmds_asm_test.py** runs the RP2040 assembly encoder, configured by the firmware's own
  setup code, and compares it with a reference.
- **dvi_pipeline_test.py** runs the real driver start-up and interrupt handler for several
  frames. It checks the sync timing, that each line carries the right row, and the messages
  to the rendering core.

## Repository layout

| Path | Contents |
|---|---|
| `src/dvi/` | DVI driver, TMDS encoder (C and RP2040 assembly), PIO serialiser |
| `src/boards/waveshare_rp2040_pizero.h` | Board definition |
| `docs/waveshare-rp2040-pizero.md` | Board guide: pinout, carrier board, internals |
| `test/dvi/` | Encoder and pipeline tests |
| `src/`, `submodules/`, `configtool/` | Upstream firmware, emulator library and configurator |
| `src/vga/`, `pcb/`, `nocut/`, `img/` | Upstream VGA/SCART driver, PCB designs, case mods and photos. These are kept for reference and aren't used on the PiZero. |

## Credits

- [Troy Schrapel (visrealm)](https://github.com/visrealm) for
  [PICO9918](https://github.com/visrealm/pico9918), the
  [vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918) emulator and the configurator.
- [JasonACT (AtariAge)](https://forums.atariage.com/profile/82586-jasonact/) for the F18A
  mode's TMS9900 "GPU" in ARM assembly.
- Luke Wren's [PicoDVI](https://github.com/Wren6991/PicoDVI), whose PIO serialiser technique
  this DVI driver uses.

For upstream news and discussion, see the
[PICO9918 AtariAge thread](https://forums.atariage.com/topic/367656-introducing-the-pico9918-a-tms9918a-drop-in-replacement-powered-by-a-pi-pico/).
For questions about this fork, open an issue here rather than upstream.

## Licensing

- **Firmware:** MIT License. See [LICENSE_FIRMWARE.md](LICENSE_FIRMWARE.md).
- **Hardware design files** (the upstream PCBs in `pcb/`): CERN-OHL-S. See
  [LICENSE_HARDWARE.md](LICENSE_HARDWARE.md).
