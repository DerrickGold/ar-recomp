#!/usr/bin/env python3
"""Static yield-point census: find + classify every RDNMI/HVBJOY read in the ROM.

Motivation (DEBUG.md §7.12, the sim-effect 1/2-speed bug): host-frame yields can
only come from (1) cfg-HLE'd wait routines, (2) the runtime's $4210 spin
detector, (3) the idle coroutine. The spin detector is a HEURISTIC (same block
re-read, ring-adjacent, same host frame), and both pacing bugs so far were
sites the heuristic misclassified ($8465 once-per-frame ack; $93CB twice-per-
frame shared ack). This tool makes the ground truth static and reviewable:

  - scans the ROM for ALL addressing forms of $4210/$4212 reads
    (abs AD, long AF, BIT 2C, CMP CD — the long form is what the $9284/$8465
    scans historically missed),
  - classifies each site by local shape:
      SPIN   read followed by BPL/BMI branching back to the read itself
             (the only shape that must yield — exactly once per wait)
      CLEAR  read immediately followed by another $4210 read (the canonical
             3-read wait's first read; must NOT yield)
      POST   read immediately after a SPIN's fallthrough (must NOT yield)
      ACK    isolated single read (NMI-ack / status check; must NOT yield —
             both historical false positives were ACKs)
      OTHER  anything else -> review by hand
  - reports which enclosing wait routines are cfg-HLE'd (those sites never
    execute) and which rely on the runtime spin detector.

Wait routines whose whole body is HLE'd (hle_func / the runner's
ActRaiser_WaitForVblank) never execute their reads; everything else's SPIN
sites are the complete set of legitimate runtime yield points. If a new pacing
bug appears, re-run this and diff: a new un-HLE'd SPIN means a new wait to
verify; an ACK showing up in [vbl]/[vbl-reject] logs means the detector is
pairing a non-wait again.

Usage: python3 tools/find_yield_points.py [rom]      (default ar.sfc)
"""
import sys

ROM_PATH = sys.argv[1] if len(sys.argv) > 1 else "ar.sfc"
rom = open(ROM_PATH, "rb").read()
N = len(rom)

# (opcode bytes matching a read of the target register, length, mnemonic)
def read_forms(lo, hi):
    return [
        (bytes([0xAD, lo, hi]), 3, "LDA abs"),
        (bytes([0xAF, lo, hi, 0x00]), 4, "LDA long"),
        (bytes([0x2C, lo, hi]), 3, "BIT abs"),
        (bytes([0xCD, lo, hi]), 3, "CMP abs"),
    ]

def cpu_addr(off):
    return f"{off >> 15:02x}:{(off & 0x7fff) | 0x8000:04x}"

def pc16(off):
    return (off & 0x7fff) | 0x8000

def find_sites(lo, hi):
    sites = []  # (off, length, form)
    for pat, ln, form in read_forms(lo, hi):
        i = rom.find(pat)
        while i != -1:
            sites.append((i, ln, form))
            i = rom.find(pat, i + 1)
    return sorted(sites)

def classify(sites, reg_name):
    site_offs = {off for off, _, _ in sites}
    rows = []
    for off, ln, form in sites:
        nxt = off + ln
        shape = "ACK"
        detail = ""
        # SPIN: next insn is BPL/BMI back to this read
        if nxt + 1 < N and rom[nxt] in (0x10, 0x30):
            disp = rom[nxt + 1]
            tgt = nxt + 2 + (disp - 256 if disp >= 128 else disp)
            if tgt == off:
                shape = "SPIN"
                detail = "BPL @self" if rom[nxt] == 0x10 else "BMI @self"
            elif tgt in site_offs:
                shape = "SPIN(2blk)"
                detail = f"branch to other read @{cpu_addr(tgt)}"
        # CLEAR: immediately followed by another read of the same reg
        if shape == "ACK" and nxt in site_offs:
            shape = "CLEAR"
        # POST: immediately preceded by a spin's fallthrough (spin read + branch)
        if shape == "ACK":
            for poff, pln, _ in sites:
                if poff + pln + 2 == off and rom[poff + pln] in (0x10, 0x30):
                    shape = "POST"
                    break
        rows.append((off, form, shape, detail))
    return rows

# Wait-routine entries the runner HLEs (reads never execute). Keep in sync with
# recomp/*.cfg hle_func lines + the runner's ActRaiser_WaitForVblank targets.
HLED_RANGES = [
    (0x00, 0x8418, 0x8428, "hle ActRaiser_WaitForVblank (bank00.cfg)"),
    (0x02, 0xA85E, 0xA86E, "hle ActRaiser_WaitForVblank (bank02.cfg A85E)"),
    # NOTE deliberately NOT listed: $02:9AC1-9ACC. It sits between the HLE'd
    # $9964/$9A56 upload routines and IS unreachable through them, but the
    # boot-time APU bring-up at $02:98E0-990D calls it natively (JSR $9AC1 x7).
    # The first census marked it HLE'd -> its spin ($9AC4) was left off the
    # runtime whitelist -> boot hung in that spin. A wait being inside an
    # HLE'd routine's ADDRESS RANGE proves nothing; only whole-routine cfg
    # `hle_func` coverage does. Verify reachability, don't assume.
]

def hled(off):
    bank = off >> 15
    pc = pc16(off)
    for b, lo_, hi_, why in HLED_RANGES:
        if bank == b and lo_ <= pc < hi_:
            return why
    return None

exit_code = 0
for reg, (lo, hi) in [("$4210 RDNMI", (0x10, 0x42)), ("$4212 HVBJOY", (0x12, 0x42))]:
    sites = find_sites(lo, hi)
    rows = classify(sites, reg)
    print(f"== {reg}: {len(rows)} read sites ==")
    spins = []
    for off, form, shape, detail in rows:
        h = hled(off)
        note = f"  [HLE'd: {h}]" if h else ""
        d = f"  ({detail})" if detail else ""
        print(f"  {cpu_addr(off)}  {form:<9} {shape:<10}{d}{note}")
        if shape.startswith("SPIN") and not h:
            spins.append(off)
        if shape == "OTHER":
            exit_code = 1
    if reg.startswith("$4210"):
        print(f"\n  -> {len(spins)} live SPIN site(s) (legit runtime yield points):"
              f" {', '.join(cpu_addr(o) for o in spins)}")
        print("  -> every other live site above must NEVER yield; if one appears"
              " in [vbl] output, the spin detector is false-pairing again"
              " (DEBUG.md pacing-bug symptom row).\n")
sys.exit(exit_code)
