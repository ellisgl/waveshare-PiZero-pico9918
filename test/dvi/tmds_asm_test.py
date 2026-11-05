#!/usr/bin/env python3
"""
Emulator test for the RP2040 assembly TMDS encoder (src/dvi/tmds_encode_rp2040.S).

Runs the real machine code from a built firmware ELF under Unicorn, with the
RP2040 SIO interpolators modelled in Python:

  1. runs tmdsEncoderInterpInit() from the ELF and captures its interpolator
     register writes (so the C configuration is tested too)
  2. fills tmdsPairTable with random values
  3. runs tmdsEncodeLine() on random scanlines (pixel-doubled, full resolution,
     solid runs) and compares every lane word with a Python reference
  4. reports instructions executed per pixel pair

Usage:
  pip install unicorn
  python3 test/dvi/tmds_asm_test.py build-pizero/src/pico9918-*.elf
"""

import random
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R4, UC_ARM_REG_R8,
                               UC_ARM_REG_R11)

RAM_BASE, RAM_SIZE = 0x20000000, 0x42000
FLASH_BASE, FLASH_SIZE = 0x10000000, 0x01000000
SIO_BASE = 0xD0000000
RET_ADDR = 0x08000000            # 'until' address for calls
INTERP_OFFSETS = {0: 0x080, 1: 0x0C0}


# ----------------------------------------------------------------------------
# minimal ELF32 reader: allocated sections + symbols
# ----------------------------------------------------------------------------
def read_elf(path):
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF' and data[4] == 1, 'not an ELF32 file'
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    sections = []
    for i in range(shnum):
        sections.append(struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize))
    shstr = sections[shstrndx]

    def name_at(table, off):
        start = table[4] + off
        return data[start:data.index(b'\0', start)].decode()

    loads, symbols = [], {}
    for s in sections:
        name, stype, flags, addr, off, size = s[0], s[1], s[2], s[3], s[4], s[5]
        if (flags & 2) and stype == 1 and size:          # SHF_ALLOC, PROGBITS
            loads.append((addr, data[off:off + size]))
        if stype == 2:                                    # SYMTAB
            strtab = sections[s[6]]
            for j in range(size // 16):
                st_name, st_value, st_size, st_info, _, _ = struct.unpack_from('<IIIBBH', data, off + j * 16)
                if st_name:
                    symbols[name_at(strtab, st_name)] = (st_value, st_size)
    return loads, symbols


# ----------------------------------------------------------------------------
# RP2040 interpolator model (the subset the encoder uses)
# ----------------------------------------------------------------------------
class Interp:
    def __init__(self):
        self.accum = [0, 0]
        self.base = [0, 0, 0]
        self.ctrl = [0, 0]

    def lane(self, i):
        ctrl = self.ctrl[i]
        shift = ctrl & 0x1F
        lsb = (ctrl >> 5) & 0x1F
        msb = (ctrl >> 10) & 0x1F
        cross = (ctrl >> 16) & 1
        assert not (ctrl >> 15) & 1, 'SIGNED not modelled'
        assert not (ctrl >> 18) & 1, 'ADD_RAW not modelled'
        src = self.accum[i ^ cross]
        mask = ((1 << (msb - lsb + 1)) - 1) << lsb
        return (src >> shift) & mask

    def full(self):
        return (self.base[2] + self.lane(0) + self.lane(1)) & 0xFFFFFFFF


interps = {0: Interp(), 1: Interp()}


def sio_read(uc, offset, size, user):
    for n, base in INTERP_OFFSETS.items():
        rel = offset - base
        if rel == 0x28:                                   # PEEK_FULL
            return interps[n].full()
        if 0 <= rel < 0x40:
            raise AssertionError(f'unexpected interp{n} read at +0x{rel:02x}')
    return 0


def sio_write(uc, offset, size, value, user):
    for n, base in INTERP_OFFSETS.items():
        rel = offset - base
        it = interps[n]
        if rel in (0x00, 0x04):
            it.accum[rel // 4] = value
        elif rel in (0x08, 0x0C, 0x10):
            it.base[(rel - 0x08) // 4] = value
        elif rel in (0x2C, 0x30):
            it.ctrl[(rel - 0x2C) // 4] = value
        elif 0 <= rel < 0x40:
            raise AssertionError(f'unexpected interp{n} write at +0x{rel:02x}')


# ----------------------------------------------------------------------------
def main():
    elf = sys.argv[1] if len(sys.argv) > 1 else None
    if not elf:
        print(__doc__)
        return 2

    loads, symbols = read_elf(elf)
    for name in ('tmdsEncodeLine', 'tmdsEncoderInterpInit', 'tmdsPairTable'):
        assert name in symbols, f'{name} not found in {elf}'

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(RAM_BASE, RAM_SIZE)
    uc.mem_map(FLASH_BASE, FLASH_SIZE)
    uc.mem_map(RET_ADDR, 0x1000)
    uc.mmio_map(SIO_BASE, 0x1000, sio_read, None, sio_write, None)
    for addr, blob in loads:
        if RAM_BASE <= addr < RAM_BASE + RAM_SIZE or FLASH_BASE <= addr < FLASH_BASE + FLASH_SIZE:
            uc.mem_write(addr, blob)

    stack_top = RAM_BASE + RAM_SIZE - 0x100

    def call(fn, args, count=None):
        uc.reg_write(UC_ARM_REG_SP, stack_top - 16)
        if len(args) > 4:
            uc.mem_write(stack_top - 16, struct.pack('<I', args[4]))
        for reg, val in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(reg, val)
        uc.reg_write(UC_ARM_REG_LR, RET_ADDR | 1)
        uc.emu_start(symbols[fn][0] | 1, RET_ADDR, count=count or 0)

    # 1. interpolator configuration from the real C code
    call('tmdsEncoderInterpInit', [])
    table = symbols['tmdsPairTable'][0]
    for n in (0, 1):
        assert interps[n].base[2] == table, f'interp{n} BASE2 isn\'t tmdsPairTable'

    # 2. random, distinct table entries (20-bit, like real symbol pairs)
    rng = random.Random(9918)
    entries = rng.sample(range(1 << 20), 256)
    uc.mem_write(table, struct.pack('<256I', *entries))

    # callee-saved registers must survive
    callee = {UC_ARM_REG_R4: 0x11111111, UC_ARM_REG_R8: 0x88888888, UC_ARM_REG_R11: 0xBBBBBBBB}

    work = 0x20030000
    src_addr, b_addr, g_addr, r_addr = work, work + 0x800, work + 0x1000, work + 0x1800

    def reference(pairs):
        out = ([], [], [])
        for w in pairs:
            out[0].append(entries[((w >> 8) & 0x0F) | ((w >> 20) & 0xF0)])
            out[1].append(entries[((w >> 4) & 0x0F) | ((w >> 16) & 0xF0)])
            out[2].append(entries[(w & 0x0F) | ((w >> 12) & 0xF0)])
        return out

    executed = [0]
    uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, u: executed.__setitem__(0, executed[0] + 1))

    def run_line(pairs):
        n = len(pairs)
        uc.mem_write(src_addr, struct.pack(f'<{n}I', *pairs))
        sentinel = struct.pack('<I', 0xDEADBEEF)
        for a in (b_addr, g_addr, r_addr):
            uc.mem_write(a + 4 * n, sentinel)
        for reg, val in callee.items():
            uc.reg_write(reg, val)
        executed[0] = 0
        call('tmdsEncodeLine', [src_addr, b_addr, g_addr, r_addr, n])
        for reg, val in callee.items():
            assert uc.reg_read(reg) == val, 'callee-saved register clobbered'
        got = [list(struct.unpack(f'<{n}I', uc.mem_read(a, 4 * n))) for a in (b_addr, g_addr, r_addr)]
        for a in (b_addr, g_addr, r_addr):
            assert uc.mem_read(a + 4 * n, 4) == sentinel, 'wrote past the end of a lane'
        return got, executed[0]

    def px(r, g, b):
        return r | (g << 4) | (b << 8) | (g << 12)   # renderer duplicates green in bits 15:12

    failures = 0
    tests = 0
    for case in range(60):
        kind = case % 3
        pairs = []
        for i in range(320):
            p0 = px(rng.randrange(16), rng.randrange(16), rng.randrange(16))
            p1 = p0 if kind == 0 else px(rng.randrange(16), rng.randrange(16), rng.randrange(16))
            if kind == 2 and pairs and rng.random() < 0.7:
                pairs.append(pairs[-1])      # long runs
            else:
                pairs.append(p0 | (p1 << 16))
        got, _ = run_line(pairs)
        want = reference(pairs)
        tests += 1
        if [list(x) for x in want] != got:
            failures += 1
            for lane in range(3):
                for i, (g, w) in enumerate(zip(got[lane], want[lane])):
                    if g != w:
                        print(f'FAIL case {case} lane {lane} pair {i}: got {g:#x} want {w:#x} (src {pairs[i]:#010x})')
                        break

    # edge cases: zero length, one pair
    got, _ = run_line([])
    got, _ = run_line([px(1, 2, 3) | (px(4, 5, 6) << 16)])
    want = reference([px(1, 2, 3) | (px(4, 5, 6) << 16)])
    tests += 1
    if [list(x) for x in want] != got:
        failures += 1
        print('FAIL single pair')

    # instruction counts: all-distinct pairs vs one solid colour
    distinct = [px(i & 15, (i >> 4) & 15, (i * 7) & 15) | (px((i + 1) & 15, 3, 9) << 16) for i in range(320)]
    _, n_distinct = run_line(distinct)
    _, n_solid = run_line([px(4, 4, 4) * 0x10001] * 320)
    print(f'instructions per pair: new pair {n_distinct / 320:.1f}, repeated pair {n_solid / 320:.1f}')
    print(f'instructions per 640-pixel line: worst {n_distinct}, solid {n_solid}')

    print(f'{tests - failures}/{tests} encoder tests passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
