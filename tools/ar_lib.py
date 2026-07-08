"""ar_lib.py — shared primitives for the ActRaiser debug toolkit.

Used by dis65.py, romxref.py, wram.py, resolve_miss.py (and importable from
trace_slice.py). One canonical place for:
  - ROM loading + LoROM address mapping
  - gen_meta.json sidecar loading (static decode facts)
  - docs/ram-map.md symbol table parsing (WRAM names)
  - a full 65816 disassembler with m/x width tracking
  - the paired-return / construct-ret cfg-hazard guards (DEBUG.md §7.17 / §1)

Address conventions: pc24 as int 0xBBAAAA or string "BBAAAA"; user-facing
accepts "BB:AAAA", "BBAAAA", "$BB:AAAA", "AAAA" (bank required unless 24-bit).
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))

# ── loading ──────────────────────────────────────────────────────────────────

_rom_cache = [None]

def load_rom(path=None):
    """ROM bytes (cached). AR_ROM env overrides; default <repo>/ar.sfc."""
    if path:
        return open(path, 'rb').read()
    if _rom_cache[0] is None:
        for c in (os.environ.get('AR_ROM'), os.path.join(ROOT, 'ar.sfc'), 'ar.sfc'):
            if c and os.path.exists(c):
                _rom_cache[0] = open(c, 'rb').read()
                break
        else:
            raise SystemExit("ar_lib: ROM not found (set AR_ROM or place ar.sfc at repo root)")
    return _rom_cache[0]


def load_meta(path=None):
    """gen_meta.json (tools/gen_metadata.py sidecar) or None."""
    for c in ([path] if path else []) + [os.path.join(ROOT, 'saves', 'gen_meta.json'),
                                         'saves/gen_meta.json']:
        if c and os.path.exists(c):
            return json.load(open(c))
    return None


def parse_addr(s, default_bank=None):
    """'BB:AAAA' | 'BBAAAA' | '$..' | 'AAAA' (needs default_bank) -> pc24 int."""
    t = s.strip().lstrip('$').replace(':', '')
    v = int(t, 16)
    if len(t) <= 4:
        if default_bank is None:
            raise ValueError(f"'{s}': 16-bit address needs a bank (use BB:AAAA)")
        return ((default_bank & 0xFF) << 16) | v
    return v & 0xFFFFFF


def fmt24(pc24):
    return f"{(pc24 >> 16) & 0xFF:02X}:{pc24 & 0xFFFF:04X}"


def lorom_off(pc24):
    """pc24 -> ROM file offset (LoROM, headerless). None if not ROM-mapped."""
    bank, addr = (pc24 >> 16) & 0xFF, pc24 & 0xFFFF
    if addr < 0x8000:
        return None
    return (bank & 0x7F) * 0x8000 + (addr - 0x8000)


def rom_read(rom, pc24, n=1):
    off = lorom_off(pc24)
    if off is None or off + n > len(rom):
        return None
    return rom[off:off + n]

# ── ram-map.md symbol table ──────────────────────────────────────────────────

def load_symbols():
    """{wram_offset:int -> name:str} from docs/ram-map.md '| $7E:xxxx | ...' rows.
    Also returns reverse map name(lower)->offset for CLI lookups."""
    path = os.path.join(ROOT, 'docs', 'ram-map.md')
    by_addr, by_name = {}, {}
    if not os.path.exists(path):
        return by_addr, by_name
    for line in open(path, encoding='utf-8'):
        m = re.match(r'\|\s*`?\$7([EF]):([0-9A-Fa-f]{4})`?\s*\|(?:\s*(\d+)\s*\|)?\s*(.+?)\s*\|', line)
        if not m:
            continue
        bank7f, a, _size, desc = m.group(1), int(m.group(2), 16), m.group(3), m.group(4)
        off = a + (0x10000 if bank7f.upper() == 'F' else 0)
        desc = re.sub(r'\*\*|`', '', desc).strip()
        short = desc.split('—')[0].split('--')[0].split('(')[0].strip()[:40]
        if off not in by_addr:
            by_addr[off] = short
            key = re.sub(r'[^a-z0-9]+', '-', short.lower()).strip('-')
            if key and key not in by_name:
                by_name[key] = off
    return by_addr, by_name

# ── 65816 disassembler ───────────────────────────────────────────────────────
# mode -> length: int, or 'm'/'x' (2 + (flag==0))
_M = {
    'imp': 1, 'acc': 1, 'imm8': 2, 'immm': 'm', 'immx': 'x',
    'dp': 2, 'dpx': 2, 'dpy': 2, 'idp': 2, 'idpx': 2, 'idpy': 2,
    'idl': 2, 'idly': 2, 'sr': 2, 'isry': 2,
    'abs': 3, 'absx': 3, 'absy': 3, 'ind': 3, 'iax': 3, 'indl': 3,
    'absl': 4, 'abslx': 4, 'rel8': 2, 'rel16': 3, 'blk': 3, 'pea': 3, 'pei': 2,
}

_ALU = lambda base, name: {  # noqa: E731  column layout shared by ORA/AND/EOR/ADC/STA/LDA/CMP/SBC
    base + 0x01: (name, 'idpx'), base + 0x03: (name, 'sr'), base + 0x05: (name, 'dp'),
    base + 0x07: (name, 'idl'), base + 0x09: (name, 'immm'), base + 0x0D: (name, 'abs'),
    base + 0x0F: (name, 'absl'), base + 0x11: (name, 'idpy'), base + 0x12: (name, 'idp'),
    base + 0x13: (name, 'isry'), base + 0x15: (name, 'dpx'), base + 0x17: (name, 'idly'),
    base + 0x19: (name, 'absy'), base + 0x1D: (name, 'absx'), base + 0x1F: (name, 'abslx'),
}

OPCODES = {}
for b, n in ((0x00, 'ORA'), (0x20, 'AND'), (0x40, 'EOR'), (0x60, 'ADC'),
             (0x80, 'STA'), (0xA0, 'LDA'), (0xC0, 'CMP'), (0xE0, 'SBC')):
    OPCODES.update(_ALU(b, n))
OPCODES[0x89] = ('BIT', 'immm')  # 0x89 overrides STA-column immm slot
OPCODES.update({
    0x00: ('BRK', 'imm8'), 0x02: ('COP', 'imm8'), 0x42: ('WDM', 'imm8'),
    0x04: ('TSB', 'dp'), 0x0C: ('TSB', 'abs'), 0x14: ('TRB', 'dp'), 0x1C: ('TRB', 'abs'),
    0x06: ('ASL', 'dp'), 0x0A: ('ASL', 'acc'), 0x0E: ('ASL', 'abs'), 0x16: ('ASL', 'dpx'), 0x1E: ('ASL', 'absx'),
    0x26: ('ROL', 'dp'), 0x2A: ('ROL', 'acc'), 0x2E: ('ROL', 'abs'), 0x36: ('ROL', 'dpx'), 0x3E: ('ROL', 'absx'),
    0x46: ('LSR', 'dp'), 0x4A: ('LSR', 'acc'), 0x4E: ('LSR', 'abs'), 0x56: ('LSR', 'dpx'), 0x5E: ('LSR', 'absx'),
    0x66: ('ROR', 'dp'), 0x6A: ('ROR', 'acc'), 0x6E: ('ROR', 'abs'), 0x76: ('ROR', 'dpx'), 0x7E: ('ROR', 'absx'),
    0xC6: ('DEC', 'dp'), 0x3A: ('DEC', 'acc'), 0xCE: ('DEC', 'abs'), 0xD6: ('DEC', 'dpx'), 0xDE: ('DEC', 'absx'),
    0xE6: ('INC', 'dp'), 0x1A: ('INC', 'acc'), 0xEE: ('INC', 'abs'), 0xF6: ('INC', 'dpx'), 0xFE: ('INC', 'absx'),
    0x24: ('BIT', 'dp'), 0x2C: ('BIT', 'abs'), 0x34: ('BIT', 'dpx'), 0x3C: ('BIT', 'absx'),
    0x64: ('STZ', 'dp'), 0x74: ('STZ', 'dpx'), 0x9C: ('STZ', 'abs'), 0x9E: ('STZ', 'absx'),
    0x84: ('STY', 'dp'), 0x8C: ('STY', 'abs'), 0x94: ('STY', 'dpx'),
    0x86: ('STX', 'dp'), 0x8E: ('STX', 'abs'), 0x96: ('STX', 'dpy'),
    0xA0: ('LDY', 'immx'), 0xA4: ('LDY', 'dp'), 0xAC: ('LDY', 'abs'), 0xB4: ('LDY', 'dpx'), 0xBC: ('LDY', 'absx'),
    0xA2: ('LDX', 'immx'), 0xA6: ('LDX', 'dp'), 0xAE: ('LDX', 'abs'), 0xB6: ('LDX', 'dpy'), 0xBE: ('LDX', 'absy'),
    0xC0: ('CPY', 'immx'), 0xC4: ('CPY', 'dp'), 0xCC: ('CPY', 'abs'),
    0xE0: ('CPX', 'immx'), 0xE4: ('CPX', 'dp'), 0xEC: ('CPX', 'abs'),
    0x10: ('BPL', 'rel8'), 0x30: ('BMI', 'rel8'), 0x50: ('BVC', 'rel8'), 0x70: ('BVS', 'rel8'),
    0x80: ('BRA', 'rel8'), 0x90: ('BCC', 'rel8'), 0xB0: ('BCS', 'rel8'),
    0xD0: ('BNE', 'rel8'), 0xF0: ('BEQ', 'rel8'), 0x82: ('BRL', 'rel16'),
    0x4C: ('JMP', 'abs'), 0x5C: ('JML', 'absl'), 0x6C: ('JMP', 'ind'),
    0x7C: ('JMP', 'iax'), 0xDC: ('JML', 'indl'),
    0x20: ('JSR', 'abs'), 0x22: ('JSL', 'absl'), 0xFC: ('JSR', 'iax'),
    0x40: ('RTI', 'imp'), 0x60: ('RTS', 'imp'), 0x6B: ('RTL', 'imp'),
    0x08: ('PHP', 'imp'), 0x28: ('PLP', 'imp'), 0x48: ('PHA', 'imp'), 0x68: ('PLA', 'imp'),
    0x5A: ('PHY', 'imp'), 0x7A: ('PLY', 'imp'), 0xDA: ('PHX', 'imp'), 0xFA: ('PLX', 'imp'),
    0x8B: ('PHB', 'imp'), 0xAB: ('PLB', 'imp'), 0x0B: ('PHD', 'imp'), 0x2B: ('PLD', 'imp'),
    0x4B: ('PHK', 'imp'), 0x62: ('PER', 'rel16'), 0xF4: ('PEA', 'pea'), 0xD4: ('PEI', 'pei'),
    0x18: ('CLC', 'imp'), 0x38: ('SEC', 'imp'), 0x58: ('CLI', 'imp'), 0x78: ('SEI', 'imp'),
    0xB8: ('CLV', 'imp'), 0xD8: ('CLD', 'imp'), 0xF8: ('SED', 'imp'),
    0xC2: ('REP', 'imm8'), 0xE2: ('SEP', 'imm8'),
    0xAA: ('TAX', 'imp'), 0xA8: ('TAY', 'imp'), 0x8A: ('TXA', 'imp'), 0x98: ('TYA', 'imp'),
    0xBA: ('TSX', 'imp'), 0x9A: ('TXS', 'imp'), 0x9B: ('TXY', 'imp'), 0xBB: ('TYX', 'imp'),
    0x5B: ('TCD', 'imp'), 0x7B: ('TDC', 'imp'), 0x1B: ('TCS', 'imp'), 0x3B: ('TSC', 'imp'),
    0xEB: ('XBA', 'imp'), 0xFB: ('XCE', 'imp'),
    0xC8: ('INY', 'imp'), 0x88: ('DEY', 'imp'), 0xE8: ('INX', 'imp'), 0xCA: ('DEX', 'imp'),
    0xEA: ('NOP', 'imp'), 0xCB: ('WAI', 'imp'), 0xDB: ('STP', 'imp'),
    0x44: ('MVP', 'blk'), 0x54: ('MVN', 'blk'),
})
assert len(OPCODES) == 256, f"opcode table incomplete: {len(OPCODES)}"

FLOW_END = {'RTS', 'RTL', 'RTI', 'STP'}          # hard stops
FLOW_JUMP = {'JMP', 'JML', 'BRA', 'BRL'}          # unconditional transfers
CALLS = {'JSR', 'JSL'}


class Insn:
    __slots__ = ('pc24', 'raw', 'mnem', 'mode', 'operand', 'target', 'text', 'm', 'x')

    def __repr__(self):
        return f"{fmt24(self.pc24)}: {self.text}"


def _length(mode, m, x):
    L = _M[mode]
    if L == 'm':
        return 2 + (1 if m == 0 else 0)
    if L == 'x':
        return 2 + (1 if x == 0 else 0)
    return L


def decode_one(rom, pc24, m, x):
    """Decode a single instruction at pc24 with widths (m,x). Returns Insn or None."""
    raw = rom_read(rom, pc24, 4)
    if raw is None:
        return None
    op = raw[0]
    mnem, mode = OPCODES[op]
    n = _length(mode, m, x)
    raw = raw[:n]
    if rom_read(rom, pc24, n) is None or len(raw) < n:
        return None
    ins = Insn()
    ins.pc24, ins.raw, ins.mnem, ins.mode, ins.m, ins.x = pc24, raw, mnem, mode, m, x
    ins.operand = None
    ins.target = None
    bank, addr = (pc24 >> 16) & 0xFF, pc24 & 0xFFFF
    o = raw[1:]
    if mode in ('imp', 'acc'):
        ins.text = mnem + (' A' if mode == 'acc' else '')
    elif mode == 'imm8':
        ins.operand = o[0]
        ins.text = f"{mnem} #${o[0]:02X}"
    elif mode in ('immm', 'immx'):
        ins.operand = int.from_bytes(o, 'little')
        ins.text = f"{mnem} #${ins.operand:0{(n - 1) * 2}X}"
    elif mode in ('dp', 'pei'):
        ins.operand = o[0]
        ins.text = f"{mnem} ${o[0]:02X}"
    elif mode in ('dpx', 'dpy'):
        ins.operand = o[0]
        ins.text = f"{mnem} ${o[0]:02X},{mode[-1].upper()}"
    elif mode == 'idp':
        ins.text = f"{mnem} (${o[0]:02X})"
    elif mode == 'idpx':
        ins.text = f"{mnem} (${o[0]:02X},X)"
    elif mode == 'idpy':
        ins.text = f"{mnem} (${o[0]:02X}),Y"
    elif mode == 'idl':
        ins.text = f"{mnem} [${o[0]:02X}]"
    elif mode == 'idly':
        ins.text = f"{mnem} [${o[0]:02X}],Y"
    elif mode == 'sr':
        ins.text = f"{mnem} ${o[0]:02X},S"
    elif mode == 'isry':
        ins.text = f"{mnem} (${o[0]:02X},S),Y"
    elif mode in ('abs', 'pea'):
        ins.operand = o[0] | (o[1] << 8)
        ins.text = f"{mnem} ${ins.operand:04X}"
        if mnem in ('JSR', 'JMP'):
            ins.target = (bank << 16) | ins.operand
    elif mode in ('absx', 'absy'):
        ins.operand = o[0] | (o[1] << 8)
        ins.text = f"{mnem} ${ins.operand:04X},{mode[-1].upper()}"
    elif mode == 'absl':
        ins.operand = o[0] | (o[1] << 8) | (o[2] << 16)
        ins.text = f"{mnem} ${(ins.operand >> 16):02X}:{ins.operand & 0xFFFF:04X}"
        if mnem in ('JSL', 'JML'):
            ins.target = ins.operand
    elif mode == 'abslx':
        ins.operand = o[0] | (o[1] << 8) | (o[2] << 16)
        ins.text = f"{mnem} ${(ins.operand >> 16):02X}:{ins.operand & 0xFFFF:04X},X"
    elif mode == 'ind':
        ins.operand = o[0] | (o[1] << 8)
        ins.text = f"{mnem} (${ins.operand:04X})"
    elif mode == 'iax':
        ins.operand = o[0] | (o[1] << 8)
        ins.text = f"{mnem} (${ins.operand:04X},X)"
    elif mode == 'indl':
        ins.operand = o[0] | (o[1] << 8)
        ins.text = f"{mnem} [${ins.operand:04X}]"
    elif mode == 'rel8':
        d = o[0] - 256 if o[0] >= 128 else o[0]
        ins.target = (bank << 16) | ((addr + 2 + d) & 0xFFFF)
        ins.text = f"{mnem} ${ins.target & 0xFFFF:04X}"
    elif mode == 'rel16':
        d16 = o[0] | (o[1] << 8)
        d = d16 - 0x10000 if d16 >= 0x8000 else d16
        ins.target = (bank << 16) | ((addr + 3 + d) & 0xFFFF)
        ins.text = f"{mnem} ${ins.target & 0xFFFF:04X}"
    elif mode == 'blk':
        ins.text = f"{mnem} ${o[1]:02X},${o[0]:02X}"   # MVN dstbank,srcbank (operands reversed)
    else:
        ins.text = mnem
    return ins


def disasm(rom, pc24, m=0, x=0, count=None, until_flow=False, max_insns=512):
    """Linear disassembly with SEP/REP m/x tracking. Yields Insn.
    until_flow: stop AFTER RTS/RTL/RTI/STP or an unconditional JMP/BRA/BRL."""
    n = 0
    while True:
        ins = decode_one(rom, pc24, m, x)
        if ins is None:
            return
        yield ins
        n += 1
        if count is not None and n >= count:
            return
        if n >= max_insns:
            return
        if until_flow and (ins.mnem in FLOW_END or ins.mnem in FLOW_JUMP):
            return
        if ins.mnem == 'SEP':
            if ins.operand & 0x20:
                m = 1
            if ins.operand & 0x10:
                x = 1
        elif ins.mnem == 'REP':
            if ins.operand & 0x20:
                m = 0
            if ins.operand & 0x10:
                x = 0
        pc24 = ((pc24 & 0xFF0000) | ((pc24 + len(ins.raw)) & 0xFFFF))

# ── cfg-hazard guards (shared by trace_slice / resolve_miss) ────────────────

def paired_return_site(tgt_hex, rom=None):
    """If tgt looks like the RETURN of a JSR/JSL call site, give
    (kind, site_addr16, callee_pc24hex). tgt_hex = 'BBAAAA'."""
    rom = rom if rom is not None else load_rom()
    bank, addr = int(tgt_hex[:2], 16), int(tgt_hex[2:], 16)
    if addr < 0x8000:
        return None

    def rd(a):
        b = rom_read(rom, (bank << 16) | a, 1)
        return b[0] if b else None
    if addr >= 0x8003 and rd(addr - 3) in (0x20, 0xFC):
        lo, hi = rd(addr - 2), rd(addr - 1)
        return ('JSR', addr - 3, f"{bank:02X}{hi:02X}{lo:02X}")
    if addr >= 0x8004 and rd(addr - 4) == 0x22:
        lo, mid, bk = rd(addr - 3), rd(addr - 2), rd(addr - 1)
        return ('JSL', addr - 4, f"{bk:02X}{mid:02X}{lo:02X}")
    return None


def construct_ret_guard(meta, bank_hex, addr_hex):
    """cfg indirect_dispatch ret: match (the B8C2 nested-reentry class)."""
    if not meta:
        return None
    for d in meta.get('cfg', {}).get('indirect_dispatch', []):
        if f"RET:{addr_hex.upper()}" in d['text'].upper() and d['bank'] == bank_hex:
            return d
    return None
