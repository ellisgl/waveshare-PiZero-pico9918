#!/usr/bin/env python3
"""
Emulator test for the DVI line pipeline (src/dvi/dvi.c) using the real firmware.

Runs vgaInit() from a built firmware ELF under Unicorn with a permissive
peripheral model, then plays the role of the hardware and of core 1:

  - DMA: for every output line, the control channels load the next buffer
    pointer from each lane's two-slot ring (alternating slots), exactly as
    configured, then the line's irq handler (dviDmaIrqHandler) is called.
  - core 1: render requests pushed through the SIO FIFO are "rendered" into
    rgbDataBuffer with a known pattern; END_OF_FRAME switches to 2x vertical
    scale like tmsEndOfFrame() does.

Checks, over several frames:
  - 525 lines per frame, vsync on lines 490-491 only, blanking/active buffers
    on the right lines, all three lanes coherent
  - every active line's TMDS buffer decodes to the right virtual line's pixels
    when the line starts, and is not modified while it is being sent
  - render requests 0..N-1 in order once per frame, then END_OF_FRAME, front
    porch and trigger-scanline messages once per frame

Usage:
  pip install unicorn
  python3 test/dvi/dvi_pipeline_test.py build-pizero/src/pico9918-*.elf
"""

import os
import re
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)

sys.path.insert(0, os.path.dirname(__file__))
from tmds_asm_test import read_elf, Interp, INTERP_OFFSETS  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))

RAM_BASE, RAM_SIZE = 0x20000000, 0x42000
FLASH_BASE, FLASH_SIZE = 0x10000000, 0x01000000
RET_ADDR = 0x08000000

LINE_WORDS, ACTIVE_WORDS = 400, 320
V_ACTIVE, V_TOTAL = 480, 525
VSYNC_LINES = (490, 491)
DATA_CHANS = (0, 1, 7)
END_OF_FRAME, END_OF_SCANLINE, FRONT_PORCH = 0x80000000, 0x40000000, 0x20000000
TRIGGER = 216

CTRL = {0: 0x354, 1: 0x0AB, 2: 0x154, 3: 0x2AB}


def struct_offsets():
    """sizes/offsets of VgaParams/VgaInitParams as laid out by the ARM compiler"""
    src = r'''
#include <stddef.h>
#include "vga.h"
const unsigned int offs[] = {
  sizeof(VgaParams), sizeof(VgaInitParams),
  offsetof(VgaInitParams, scanlines), offsetof(VgaInitParams, triggerScanline),
  offsetof(VgaParams, vPixelScale), offsetof(VgaParams, vVirtualPixels),
};
'''
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, 'o.c')
        open(c, 'w').write(src)
        asm = subprocess.check_output(['arm-none-eabi-gcc', '-mcpu=cortex-m0plus', '-mthumb', '-O0', '-S',
                                       '-I', os.path.join(ROOT, 'src', 'vga'), c, '-o', '-'], text=True)
    words = [int(x) for x in re.findall(r'\.word\s+(\d+)', asm)]
    keys = ['sizeParams', 'sizeInit', 'scanlines', 'trigger', 'vPixelScale', 'vVirtualPixels']
    return dict(zip(keys, words))


class Machine:
    def __init__(self, elf):
        self.loads, self.sym = read_elf(elf)
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(RAM_BASE, RAM_SIZE)
        uc.mem_map(FLASH_BASE, FLASH_SIZE)
        uc.mem_map(RET_ADDR, 0x1000)
        for addr, blob in self.loads:
            if RAM_BASE <= addr < RAM_BASE + RAM_SIZE or FLASH_BASE <= addr < FLASH_BASE + FLASH_SIZE:
                uc.mem_write(addr, blob)

        self.interps = {0: Interp(), 1: Interp()}
        self.fifo = []
        self.dma_read_addr = {}
        self.transfer_count = LINE_WORDS - 9      # "irq on time"

        uc.mmio_map(0x40000000, 0x100000, lambda *a: 0, None, lambda *a: None, None)    # APB peripherals
        # (0x4000: register block plus its atomic XOR/SET/CLR aliases)
        uc.mmio_map(0x50000000, 0x4000, self.dma_read, None, self.dma_write, None)     # DMA
        uc.mmio_map(0x50200000, 0x4000, self.pio_read, None, lambda *a: None, None)    # PIO0
        uc.mmio_map(0x50300000, 0x4000, self.pio_read, None, lambda *a: None, None)    # PIO1
        uc.mmio_map(0xD0000000, 0x1000, self.sio_read, None, self.sio_write, None)     # SIO
        uc.mmio_map(0xE000E000, 0x1000, self.ppb_read, None, lambda *a: None, None)    # NVIC/SCB

        # runtime init normally copies the vector table to RAM (irq_set_exclusive_handler
        # checks the slot still holds the default handler)
        if 'ram_vector_table' in self.sym and '__vectors' in self.sym:
            uc.mem_write(self.sym['ram_vector_table'][0], bytes(uc.mem_read(self.sym['__vectors'][0], 48 * 4)))

        # the SDK routes memcpy/memset to boot ROM routines (not loaded here): emulate them
        self.stubs = {}
        def ret(f):
            return lambda r0, r1, r2: self.uc.reg_write(UC_ARM_REG_R0, f(r0) & 0xFFFFFFFF)
        ctz = ret(lambda x: (x & -x).bit_length() - 1 if x else 32)
        clz = ret(lambda x: 32 - x.bit_length())
        popcount = ret(lambda x: bin(x).count('1'))
        for name, fn in (('__wrap_memcpy', self.stub_memcpy), ('__wrap_memset', self.stub_memset),
                         ('__aeabi_memcpy', self.stub_memcpy), ('__aeabi_memcpy4', self.stub_memcpy),
                         ('__wrap___ctzsi2', ctz), ('__wrap___clzsi2', clz), ('__wrap___popcountsi2', popcount)):
            if name in self.sym:
                self.stubs[self.sym[name][0] & ~1] = fn
        if self.stubs:
            uc.hook_add(UC_HOOK_CODE, self.stub_hook)

    def stub_hook(self, uc, address, size, user):
        fn = self.stubs.get(address)
        if fn:
            r0, r1, r2 = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
            fn(r0, r1, r2)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR) | 1)

    def stub_memcpy(self, dst, src, n):
        if n:
            self.uc.mem_write(dst, bytes(self.uc.mem_read(src, n)))

    def stub_memset(self, dst, value, n):
        if n:
            self.uc.mem_write(dst, bytes([value & 0xFF]) * n)

    # --- peripheral models ---------------------------------------------------
    def dma_read(self, uc, off, size, user):
        if off < 12 * 0x40 and off % 0x40 == 0x08:
            return self.transfer_count
        return 0

    def dma_write(self, uc, off, size, value, user):
        if off >= 0x1000:
            return
        ch, reg = divmod(off, 0x40)
        if ch < 12 and reg in (0x00, 0x3C):
            self.dma_read_addr[ch] = value

    def pio_read(self, uc, off, size, user):
        return 0x000F0F00 if off == 0x004 else 0   # FSTAT: tx full, rx empty

    def ppb_read(self, uc, off, size, user):
        return self.sym['ram_vector_table'][0] if off == 0xD08 else 0   # VTOR

    def sio_read(self, uc, off, size, user):
        for n, base in INTERP_OFFSETS.items():
            if off - base == 0x28:
                return self.interps[n].full()
        if off == 0x050:
            return 0x2                               # FIFO_ST: ready to write
        if 0x100 <= off < 0x180:
            return 1                                 # spinlock acquired
        return 0

    def sio_write(self, uc, off, size, value, user):
        for n, base in INTERP_OFFSETS.items():
            rel, it = off - base, self.interps[n]
            if rel in (0x00, 0x04):
                it.accum[rel // 4] = value
            elif rel in (0x08, 0x0C, 0x10):
                it.base[(rel - 0x08) // 4] = value
            elif rel in (0x2C, 0x30):
                it.ctrl[(rel - 0x2C) // 4] = value
        if off == 0x054:
            self.fifo.append(value)

    # --- helpers ---------------------------------------------------------------
    def addr(self, name):
        return self.sym[name][0]

    def call(self, name, regs=(), stack=b''):
        uc = self.uc
        sp = RAM_BASE + RAM_SIZE - 0x400
        uc.mem_write(sp, stack or b'\0')
        uc.reg_write(UC_ARM_REG_SP, sp)
        for reg, val in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), regs):
            uc.reg_write(reg, val)
        uc.reg_write(UC_ARM_REG_LR, RET_ADDR | 1)
        uc.emu_start(self.addr(name) | 1, RET_ADDR)

    def u32(self, addr):
        return struct.unpack('<I', self.uc.mem_read(addr, 4))[0]

    def words(self, addr, n):
        return list(struct.unpack(f'<{n}I', self.uc.mem_read(addr, 4 * n)))


def decode(sym):
    data = sym & 0xFF
    if sym & 0x200:
        data ^= 0xFF
    d = data & 1
    for i in range(1, 8):
        bit = ((data >> i) ^ (data >> (i - 1))) & 1
        if not sym & 0x100:
            bit ^= 1
        d |= bit << i
    return d


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    m = Machine(sys.argv[1])
    offs = struct_offsets()
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)
            if len(fails) < 25:
                print('FAIL:', msg)

    # --- vgaInit() ---------------------------------------------------------------
    # (vgaGetParams() needs the boot ROM float routines, so build the params here.
    #  the dvi driver only reads the sync polarities - 0 = active low, as for
    #  640x480 - and the vertical scale, which vgaGetParams() sets to 1x / 480.)
    init = bytearray(offs['sizeInit'])                   # function pointers stay NULL
    struct.pack_into('<B', init, offs['vPixelScale'], 1)
    struct.pack_into('<H', init, offs['vVirtualPixels'], 480)
    struct.pack_into('<I', init, offs['trigger'], TRIGGER)
    regs = struct.unpack_from('<4I', init, 0)
    m.call('vgaInit', regs, bytes(init[16:]))

    vga = m.addr('vgaParams')
    rgb = m.addr('rgbDataBuffer')
    rgb_stride = 642 * 2
    active = m.addr('tmdsActive')
    blank_d0, blank_d0v, blank_d12 = m.addr('tmdsBlankD0'), m.addr('tmdsBlankD0Vsync'), m.addr('tmdsBlankD12')
    ring = m.addr('nextLinePtr')
    table = m.words(m.addr('tmdsPairTable'), 256)
    level_value = [decode(table[i] & 0x3FF) for i in range(16)]

    # blanking buffer contents (polarity: 640x480 sync is active low)
    def ctrl_pair(h_active, v_active):
        s = CTRL[((0 if v_active else 1) << 1) | (0 if h_active else 1)]
        return s | (s << 10)
    for buf, vs in ((blank_d0, False), (blank_d0v, True)):
        w = m.words(buf, LINE_WORDS)
        check(w[:328] == [ctrl_pair(False, vs)] * 328, f'blank D0 (vsync={vs}) idle region wrong')
        check(w[328:376] == [ctrl_pair(True, vs)] * 48, f'blank D0 (vsync={vs}) hsync region wrong')
        check(w[376:] == [ctrl_pair(False, vs)] * 24, f'blank D0 (vsync={vs}) back porch wrong')
    check(m.words(blank_d12, LINE_WORDS) == [0x354 | (0x354 << 10)] * LINE_WORDS, 'blank D1/D2 wrong')

    def active_buf(b, lane):
        return active + (b * 3 + lane) * LINE_WORDS * 4

    for b in range(2):
        w = m.words(active_buf(b, 0), LINE_WORDS)
        check(w[320:328] == [ctrl_pair(False, False)] * 8 and w[328:376] == [ctrl_pair(True, False)] * 48,
              f'active buffer {b} lane 0 blanking tail wrong')

    # --- hardware + core 1 simulation ------------------------------------------
    first = [m.dma_read_addr.get(ch) for ch in DATA_CHANS]
    check(first == [blank_d0, blank_d12, blank_d12], 'data channels not started on a blanking line')

    rendered = {}                 # virtual line -> pixel pairs rendered for it (this frame)
    frame = 0
    msgs = []

    def pattern(y, x, f):
        r, g, b = (y * 3 + x + f) & 15, (y + x * 5) & 15, (x * 7 + f) & 15
        return r | (g << 4) | (b << 8) | (g << 12)

    def core1():
        nonlocal frame
        while m.fifo:
            msg = m.fifo.pop(0)
            msgs.append(msg)
            if msg & 0xF0000000:
                if msg == END_OF_FRAME:
                    # like tmsEndOfFrame(): 2x vertical scale, 240 virtual lines
                    m.uc.mem_write(vga + offs['vPixelScale'], b'\x02')
                    m.uc.mem_write(vga + offs['vVirtualPixels'], struct.pack('<H', 240))
                continue
            y = msg & 0xFFF
            full_res = (y % 3) == 0          # some lines not pixel-doubled (80 column)
            pairs = []
            for x in range(ACTIVE_WORDS):
                p0 = pattern(y, 2 * x, frame)
                p1 = pattern(y, 2 * x + 1, frame) if full_res else p0
                pairs.append(p0 | (p1 << 16))
            m.uc.mem_write(rgb + (y & 1) * rgb_stride, struct.pack(f'<{ACTIVE_WORDS}I', *pairs))
            rendered[y] = pairs

    def expected_lanes(pairs):
        lanes = ([], [], [])
        for w in pairs:
            lanes[0].append(table[((w >> 8) & 0xF) | ((w >> 20) & 0xF0)])
            lanes[1].append(table[((w >> 4) & 0xF) | ((w >> 16) & 0xF0)])
            lanes[2].append(table[(w & 0xF) | ((w >> 12) & 0xF0)])
        return lanes

    hw_line = V_ACTIVE            # line the DMA is sending (started by vgaInit)
    load = 0                      # chained control loads so far (ring slot = load & 1)
    frames_to_run = 4
    scale_this_frame = 1
    vsync_seen = []
    lines_in_frame = 0

    for step in range(V_TOTAL * frames_to_run + 40):
        # end of hw_line: control channels load the next pointers, irq fires
        slot = load & 1
        ptrs = [m.u32(ring + (lane * 2 + slot) * 4) for lane in range(3)]
        load += 1
        hw_line = (hw_line + 1) % V_TOTAL
        if hw_line == 0:
            frame += 1 if step > 0 else 0
            lines_in_frame = 0
        lines_in_frame += 1
        L = hw_line

        # what is about to be sent must be right
        if L < V_ACTIVE:
            v = L >> 1 if scale_this_frame == 2 else L
            b = v & 1
            check(ptrs == [active_buf(b, 0), active_buf(b, 1), active_buf(b, 2)],
                  f'frame {frame} line {L}: wrong active buffers')
            if v in rendered:
                want = expected_lanes(rendered[v])
                got = [m.words(active_buf(b, lane), ACTIVE_WORDS) for lane in range(3)]
                check(got == [list(x) for x in want], f'frame {frame} line {L}: buffer doesn\'t hold virtual line {v}')
            else:
                check(False, f'frame {frame} line {L}: virtual line {v} never rendered')
            snapshot = [m.words(p, LINE_WORDS) for p in ptrs]
        else:
            vs = L in VSYNC_LINES
            check(ptrs == [blank_d0v if vs else blank_d0, blank_d12, blank_d12],
                  f'frame {frame} line {L}: wrong blanking buffers')
            if vs:
                vsync_seen.append(step)
            snapshot = None

        if L == V_TOTAL - 4:   # PRIME_LINE: a new frame's renders start here
            rendered.clear()

        m.call('dviDmaIrqHandler')
        core1()

        if snapshot is not None:
            check([m.words(p, LINE_WORDS) for p in ptrs] == snapshot,
                  f'frame {frame} line {L}: buffer modified while being sent')

        if L == V_TOTAL - 4:   # PRIME_LINE: the irq latched the coming frame's scale
            scale_this_frame = 2 if struct.unpack('<B', m.uc.mem_read(vga + offs['vPixelScale'], 1))[0] == 2 else 1

    # vsync cadence: pairs of lines, 525 apart
    starts = vsync_seen[::2]
    check(all(b - a == V_TOTAL for a, b in zip(starts, starts[1:])), f'vsync not every {V_TOTAL} lines: {starts}')
    check(all(b - a == 1 for a, b in zip(vsync_seen[::2], vsync_seen[1::2])), 'vsync not 2 lines long')

    # message stream per frame (after the first, 2x-scaled frames have 240 lines)
    frames, cur = [], []
    for msg in msgs:
        cur.append(msg)
        if msg == FRONT_PORCH:
            frames.append(cur)
            cur = []
    for i, fm in enumerate(frames[1:], 1):
        reqs = [x for x in fm if not x & 0xF0000000]
        n = 240 if i > 1 else 480
        check(reqs == list(range(n)), f'frame {i}: render requests not 0..{n - 1} in order')
        check(fm.count(END_OF_FRAME) == 1 and fm.index(END_OF_FRAME) == fm.index(n - 1) + 1,
              f'frame {i}: END_OF_FRAME missing or misplaced')
        check(fm.count(END_OF_SCANLINE | TRIGGER) == 1, f'frame {i}: trigger scanline message count wrong')

    print(f'simulated {frames_to_run} frames ({load} lines), first frame 1x then 2x vertical scale')
    print(f'black={level_value[0]} white={level_value[15]}')
    print('PASS' if not fails else f'{len(fails)} FAILURES')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
