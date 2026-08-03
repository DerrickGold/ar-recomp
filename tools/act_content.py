#!/usr/bin/env python3
"""act_content.py — dump ActRaiser's action-mode and sim-mode CONTENT tables.

The content seams a randomizer needs are three separate ROM structures; this
prints all of them from the stock ROM so a proposed edit can be diffed against
the original. See docs/SEAMS.md "Content / randomizer seams".

  --tables   per-region object-type tables ($00:95DD list) and the 12-byte
             spawn records behind them: ATK/HP/score/flags/handler per type.
  --levels   the bank-$0A level layout streams ($0A:B100 index): player start,
             terrain damage boxes, and the 4-byte object placement entries
             (tile X, tile Y, spawn param $38, object type).
  --census   per-stage and per-region summary of which object types are used
             and which items the type-$80 statues hold.
  --lairs    sim-mode monster-lair table (ROM $03:B825, 24 x 9 bytes).
  --assets   per-map asset script ($05:8000, VM at $02:B1F7): which compressed
             animation blob each map loads into $7E:4000 / $7E:5000. Add
             --script to dump every command with its operands.

Examples:
  tools/act_content.py --tables
  tools/act_content.py --levels | less
  tools/act_content.py --census
  tools/act_content.py --lairs
"""
import argparse
import collections
import sys

from ar_lib import load_rom

REGION = {0: 'special/common', 1: 'Fillmore', 2: 'Bloodpool', 3: 'Kasandora',
          4: 'Aitos', 5: 'Marahna', 6: 'Northwall', 7: 'Death Heim'}

# Object-type handler tables, one per $18 region ($00:95DD pointer list).
TABLES = [0x96AF, 0xA8F6, 0xB449, 0xC11E, 0xCD9B, 0xD928, 0xE722, 0xF39A]

# Item ids carried in object field $38 and applied by $00:879D.
ITEM = {0: 'magic +1', 1: '1UP', 2: 'screen clear', 3: 'sword power-up',
        4: 'heal 1/4 max', 5: 'full heal', 6: '+100 pts', 7: '+50 pts'}

# Sim-mode lair monster types (world-record class, record +$0E).
MONSTER = {0x12: 'Blue Dragon', 0x13: 'Napper Bat', 0x14: 'Red Demon',
           0x15: 'Skull Head'}
TOWN = ['Fillmore', 'Bloodpool', 'Kasandora', 'Aitos', 'Marahna', 'Northwall']


class Bank:
    """Byte access into one LoROM bank ($8000-$FFFF window)."""

    def __init__(self, rom, bank):
        self.rom, self.bank = rom, bank

    def off(self, addr):
        return self.bank * 0x8000 + (addr - 0x8000)

    def u8(self, addr):
        return self.rom[self.off(addr)]

    def u16(self, addr):
        o = self.off(addr)
        return self.rom[o] | (self.rom[o + 1] << 8)


# ---------------------------------------------------------------- object types

def iter_type_table(b0, base):
    """Yield (type_index, pointer) for one region's object-type table.

    The tables carry no count. The nearest forward target seen while walking is
    the payload boundary; a zero word is an unused type slot, not a terminator
    (Bloodpool's $B449 has a five-slot hole before ten more live records).
    """
    a, end = base, 0x10000
    hard = min(0x10000, base + 0x100)
    idx = 0
    while a < end and a < hard:
        ptr = b0.u16(a)
        a += 2
        if ptr and not (base < ptr <= 0xFFFF):
            break
        if ptr:
            end = min(end, ptr)
        yield idx, ptr
        idx += 1


def cmd_tables(rom):
    b0 = Bank(rom, 0x00)
    print('Object-type tables ($00:95DD list). Record layout, 12 bytes at B, '
          'handler at B+$0C:')
    print('  +0 word -> obj $16 (animation-table base in WRAM)')
    print('  +2 byte -> obj $18 lo (data bank of that table, $7E/$7F)')
    print('  +3 byte -> obj $28 hi (spawn sub-param)')
    print('  +4 word -> obj $30 (flags: $0001 attacker, $0200 pickup, '
          '$4000 boss)')
    print('  +6 byte -> obj $1A (initial animation index)')
    print('  +7 byte -> obj $2A ATK      +8 byte -> obj $2C HP')
    print('  +9 byte -> obj $2E score    +A word -> obj $14 (secondary '
          'handler; $A3E1 = respawns)')
    for base, (reg, name) in zip(TABLES, REGION.items()):
        print(f"\n=== $18=${reg:02X} {name}   table ${base:04X} ===")
        print(' ty  recB  $16   bnk $28h flags  anim ATK  HP  scr  $14   '
              'handler  notes')
        for idx, ptr in iter_type_table(b0, base):
            if ptr == 0:
                print(f" {idx:02X}  ----  (unused type slot)")
                continue
            r = [b0.u8(ptr + i) for i in range(12)]
            f16 = r[4] | (r[5] << 8)
            notes = []
            if f16 & 0x4000:
                notes.append('BOSS')
            if f16 & 0x0200:
                notes.append('PICKUP')
            if f16 & 0x0001:
                notes.append('attacker')
            if (r[10] | (r[11] << 8)) == 0xA3E1:
                notes.append('respawns')
            print(f" {idx:02X}  {ptr:04X}  {r[0] | (r[1] << 8):04X}   "
                  f"{r[2]:02X}  {r[3]:02X}  {f16:04X}  {r[6]:02X}  "
                  f"{r[7]:3d} {r[8]:3d}  {r[9]:02X}  "
                  f"{r[10] | (r[11] << 8):04X}  ${ptr + 0x0C:04X}   "
                  f"{' '.join(notes)}")
    print("\nNote: a table value is decoded as a record only when the spawning "
          "object's $38 != $FF.\nWith $38 == $FF ($00:9590) the value is "
          "installed as the handler directly and no stats are copied.")


# ----------------------------------------------------------------- level data

def level_index(ba):
    out, a = [], 0xB100
    while ba.u16(a) != 0xFFFF:
        out.append((ba.u16(a), ba.u16(a + 2)))
        a += 4
    return out


def walk_objects(ba, y, emit):
    """Walk one object-placement batch. Returns the next batch's address or None.

    Entry forms (byte 0):
      $00-$FB  4 bytes: tileX, tileY, $38 spawn param, object type
      $FC      3 bytes: goto absolute bank-$0A address (word)
      $FD      2 bytes: reserve N object slots
      $FE      5 bytes: checkpoint / wave gate (handler $A813)
      $FF      1 byte:  end of list
    """
    seen = set()
    while True:
        if y in seen:
            emit('    <cycle>')
            return None
        seen.add(y)
        op = ba.u8(y)
        if op == 0xFF:
            emit('    $FF  end of wave')
            return None
        if op == 0xFE:
            b = [ba.u8(y + i) for i in range(5)]
            emit(f"    $FE  CHECKPOINT/WAVE GATE (handler $A813): trips when "
                 f"playerX > tile ${b[1]:02X} (px {b[1] * 16}) and playerY > "
                 f"tile ${b[2]:02X} (px {b[2] * 16}); respawn point tile "
                 f"(${b[3]:02X},${b[4]:02X}) px ({b[3] * 16},{b[4] * 16}); "
                 f"next wave @ $0A:{y + 5:04X}")
            return y + 5
        if op == 0xFD:
            emit(f"    $FD  reserve {ba.u8(y + 1)} object slots")
            y += 2
            continue
        if op == 0xFC:
            t = ba.u16(y + 1)
            emit(f"    $FC  goto $0A:{t:04X}")
            y = t
            continue
        x, ty, p, t = (ba.u8(y), ba.u8(y + 1), ba.u8(y + 2), ba.u8(y + 3))
        extra = (f"   ** STATUE: {ITEM.get(p, '?')} **" if t == 0x80 else '')
        emit(f"    @$0A:{y:04X}  tile(${x:02X},${ty:02X}) px({x * 16:5d},"
             f"{ty * 16:4d})  $38=${p:02X}  type=${t:02X}{extra}")
        y += 4


def stage_header(ba, base, emit):
    """Emit the player start + terrain damage boxes; return the object-list addr."""
    y = base
    px, py, pf = ba.u8(y), ba.u8(y + 1), ba.u8(y + 2)
    y += 3
    emit(f"  player start tile(${px:02X},${py:02X}) px({px * 16},{py * 16})"
         f"  param ${pf:02X}")
    n = 0
    rows = []
    while ba.u8(y) != 0xFF:
        b = [ba.u8(y + i) for i in range(5)]
        rows.append(f"    [{n:02d}] x ${b[0]:02X}..${b[1]:02X}  "
                    f"y ${b[2]:02X}..${b[3]:02X}  dmg/flag=${b[4]:02X}")
        y += 5
        n += 1
    emit(f"  {n} terrain damage box(es) (5-byte ROM records -> $1AE2 count / "
         f"$1AE4 10-byte RAM records, tested by $00:8C44):")
    for r in rows:
        emit(r)
    return y + 1


def cmd_levels(rom):
    ba = Bank(rom, 0x0A)
    idx = level_index(ba)
    print(f"Level index: {len(idx)} entries at $0A:B100 "
          f"(key word = $19<<8 | $18, then offset from $B100).")
    print("These are MAPS, not stages: an act spans several consecutive $19 "
          "maps. The 12 acts are\n6 kingdoms x 2; act 2 begins at $19 = "
          "2/2/3/4/4/5 for regions $01-$06 (ram-map $7E:0019),\nwhich is also "
          "what the professional-mode order table $02:9013 enumerates.")
    for key, o in idx:
        print(f"  $18=${key & 0xFF:02X} $19=${key >> 8:02X}  "
              f"{REGION.get(key & 0xFF, '?')} map {key >> 8}  "
              f"-> $0A:{0xB100 + o:04X}")
    for key, o in idx:
        base = 0xB100 + o
        print(f"\n===== $18=${key & 0xFF:02X} $19=${key >> 8:02X}  "
              f"{REGION.get(key & 0xFF, '?')} map {key >> 8}  "
              f"@ $0A:{base:04X} =====")
        y = stage_header(ba, base, print)
        wave = 0
        while y is not None:
            print(f"  --- wave {wave} @ $0A:{y:04X} ---")
            y = walk_objects(ba, y, print)
            wave += 1


def cmd_census(rom):
    ba = Bank(rom, 0x0A)
    per_region = collections.defaultdict(collections.Counter)
    all_items = collections.Counter()
    print(f"{'stage':<20} {'objs':>4} {'statues':>7}  types used")
    for key, o in level_index(ba):
        y = stage_header(ba, 0xB100 + o, lambda _s: None)
        types, items = collections.Counter(), collections.Counter()
        while y is not None:
            nxt = []
            y = walk_objects(ba, y, nxt.append)
            for line in nxt:
                if 'type=$' not in line:
                    continue
                t = int(line.split('type=$')[1][:2], 16)
                types[t] += 1
                if t == 0x80:
                    p = int(line.split('$38=$')[1][:2], 16)
                    items[p] += 1
                    all_items[p] += 1
        reg = key & 0xFF
        per_region[reg].update(types)
        label = f"{REGION.get(reg, '?')} {key >> 8}"
        print(f"{label:<20} {sum(types.values()):4d} {sum(items.values()):7d}"
              f"  " + ' '.join(f"${t:02X}x{c}" for t, c in sorted(types.items())))
        if items:
            print(f"{'':20} {'':4} {'':7}  statues: "
                  + ', '.join(f"{ITEM.get(k, '?')} x{v}"
                              for k, v in sorted(items.items())))
    print("\nTotal statue drops by item id: "
          + ', '.join(f"${k:02X} {ITEM.get(k, '?')} = {v}"
                      for k, v in sorted(all_items.items()))
          + f"   ({sum(all_items.values())} statues)")
    print("\nObject types used per region:")
    for r in sorted(per_region):
        print(f"  {REGION.get(r, '?')}: "
              + ' '.join(f"${t:02X}x{c}" for t, c in sorted(per_region[r].items())))


# ----------------------------------------------------------------- sim lairs

# ------------------------------------------------------- per-map asset script

SCRIPT_BASE = 0x05 * 0x8000     # $05:8000, cursor $A2 in the VM
SCRIPT_HEADER = 3               # leading "SY\0" entry, matches no ($18,$19)

# Command byte -> handler + operand length. The VM ($02:B1F7) tests bits MSB
# first, so the HIGHEST set bit selects; the skip chain at $02:B264 gives sizes.
CMD = {7: ('$02:B28E', 6), 6: ('$02:B330', 6), 5: ('$02:B363', 7),
       4: ('$02:B3EB', 4), 3: ('$02:B4E8', 1), 2: ('$02:B631', 3),
       1: ('$02:B63B song', 5), 0: ('$02:B69C anim blob', 6)}


def high_bit(v):
    for b in range(7, -1, -1):
        if v >> b & 1:
            return b
    return None


def linear_to_snes(l):
    """Script pointers are 24-bit LINEAR file offsets; `$02:B4C0` converts them
    in place to LoROM (bank = L>>15, addr = $8000 | (L & $7FFF))."""
    return (l >> 15) & 0xFF, 0x8000 | (l & 0x7FFF)


def iter_script(rom):
    """Yield (mode, sub, [(cmd, operands), ...]) for every asset-script entry."""
    y = SCRIPT_HEADER
    while y < 0x7FF0:
        mode, sub = rom[SCRIPT_BASE + y], rom[SCRIPT_BASE + y + 1]
        y += 2
        cmds = []
        while True:
            c = rom[SCRIPT_BASE + y]
            y += 1
            if c == 0:
                break
            n = CMD[high_bit(c)][1]
            cmds.append((c, rom[SCRIPT_BASE + y:SCRIPT_BASE + y + n]))
            y += n
        yield mode, sub, cmds
        if (mode, sub) == (0x07, 0x08):
            return


def cmd_assets(rom, verbose=False):
    print("Per-map asset script: table $05:8000, walked by the VM at $02:B1F7")
    print("(seek $02:B250 matches ($18,$19); commands dispatch by highest set "
          "bit).\n")
    rows = []
    for mode, sub, cmds in iter_script(rom):
        obj = boss = None
        for c, ops in cmds:
            if high_bit(c) == 0 and len(ops) == 6:
                l = ops[3] | (ops[4] << 8) | (ops[5] << 16)
                if ops[0] == 0:
                    obj = l
                else:
                    boss = l
        rows.append((mode, sub, obj, boss))
        if verbose:
            print(f"$18=${mode:02X} $19=${sub:02X}  "
                  f"{REGION.get(mode, '?')} map {sub}")
            for c, ops in cmds:
                b = high_bit(c)
                print(f"    cmd=${c:02X} bit{b} {CMD[b][0]:<20} "
                      f"ops={' '.join(f'{v:02X}' for v in ops)}")

    def fmt(l):
        if not l:
            return '(inherits)'
        b, a = linear_to_snes(l)
        return f"${l:06X} = ${b:02X}:{a:04X}"

    print(f"{'map':<22} {'objects -> $7E:4000':<26} {'boss -> $7E:5000'}")
    for mode, sub, obj, boss in rows:
        if mode == 0:
            continue
        print(f"{REGION.get(mode, '?') + ' map ' + str(sub):<22} "
              f"{fmt(obj):<26} {fmt(boss)}")
    objs = [(m, s, o) for m, s, o, _ in rows if m and o]
    print(f"\n{len(objs)} maps load an object blob; every other map INHERITS "
          "whatever is already\nin $7E:4000. The load points are the act-entry "
          "maps, so the enemy-animation\nallow-list is per ACT: enemies may be "
          "shuffled freely among the maps of one act.")
    for m, s, o in objs:
        print(f"  {REGION.get(m, '?')} map {s}: ${o:06X}")


def cmd_lairs(rom):
    b3 = Bank(rom, 0x03)
    base = 0xB825
    print("Monster-lair seed table ROM $03:B825 (file 0x1B825), 24 x 9 bytes,")
    print("installed by $03:B7C6 into the parallel $7F:95xx/96xx arrays.")
    print("X/Y are 16px town-map cells (0..31); a lair's selector square is "
          "cell>>2.")
    print("Spawn position is (X*16 + $18, Y*16 + 8) written to world record "
          "+$0A/+$0C by $03:B99C.\n")
    print(" i  town        X     Y    image  type                 count  "
          "respawn  world record")
    for i in range(24):
        a = base + i * 9
        b = [b3.u8(a + k) for k in range(9)]
        resp = b[5] | (b[6] << 8)
        rec = b[7] | (b[8] << 8)
        print(f"{i:2d}  {TOWN[i // 4]:<10} ${b[0]:02X}   ${b[1]:02X}   "
              f"${b[2]:02X}    ${b[3]:02X} {MONSTER.get(b[3], '?'):<14} "
              f"{b[4]:5d}  ${resp:04X}    ${rec:04X}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--tables', action='store_true')
    ap.add_argument('--levels', action='store_true')
    ap.add_argument('--census', action='store_true')
    ap.add_argument('--assets', action='store_true',
                    help='per-map asset script ($05:8000) + animation blobs')
    ap.add_argument('--script', action='store_true',
                    help='with --assets, also dump every command')
    ap.add_argument('--lairs', action='store_true')
    ap.add_argument('--rom', help='ROM path (default ar.sfc / $AR_ROM)')
    args = ap.parse_args()
    if not (args.tables or args.levels or args.census or args.lairs
            or args.assets):
        ap.print_help()
        return 1
    rom = load_rom(args.rom)
    if args.tables:
        cmd_tables(rom)
    if args.levels:
        cmd_levels(rom)
    if args.census:
        cmd_census(rom)
    if args.assets:
        cmd_assets(rom, args.script)
    if args.lairs:
        cmd_lairs(rom)
    return 0


if __name__ == '__main__':
    sys.exit(main())
