#!/usr/bin/env python3
"""diff_mx.py — CPU m/x-flag oracle differ.

Compares the recomp's per-game-frame (m,x) against snes9x ground truth to find
the FIRST decode-time flag divergence (the root of the boss->sim misdecode
cascade, where a leaked m/x selects a garbage variant).

Inputs (both keyed on the game-frame counter $0088, so they align despite
different boot lengths):
  recomp side : AR_MX_OUT file       lines "gframe m x"
  oracle side : SNESREF_MX_OUT file  lines "gframe m x tableidx"
                (m/x = -1 means snes9x's opcode table was unresolved that frame)

IMPORTANT — game-frame offset: $0088 has DIFFERENT absolute values in the two
runs, and the two runs need NOT share input. By default the differ AUTO-ANCHORS
on the boss->sim transition ($1A!=0, else $18 leaving action 0x01) found
independently in each file -- the transition is the same scripted code however
you reached it -- and derives the offset from that. So just PLAY each binary
live to beat the Fillmore boss; no input replay needed. (Override with --offset
N if ever needed; --from/--to are in RECOMP game-frames.)

Usage:
  python3 diff_mx.py <recomp_mx> <oracle_mx> [--offset N] [--from GF] [--to GF] [--context M]

Capture recipe (play each LIVE; cheats OFF on the recomp so its run is faithful):
  # recomp -- play to the boss; it will crash in the transition (fine, the
  # AR_MX_OUT trace is flushed up to the crash):
  env AR_INF_HP=0 AR_FREEZE_TIMER=0 AR_MOONJUMP=0 AR_NO_KNOCKBACK=0 \
      AR_MX_OUT=/tmp/recomp_mx.txt ./build/ActRaiserRecomp ar.sfc --config dev-config.ini
  # oracle -- play live (window opens), beat the boss, watch it enter sim:
  cd tools/oracle && env SNESREF_MX_OUT=/tmp/oracle_mx.txt \
      SNESREF_SRAM_IN=../../saves/save.srm ./snesref ./snes9x_libretro.dylib ../../ar.sfc
  python3 diff_mx.py /tmp/recomp_mx.txt /tmp/oracle_mx.txt
"""
import sys


def load(path):
    """gframe -> (m, x, g18, g1a), last sample of each game-frame wins.

    Accepts both line shapes:
      recomp: "gframe m x g18 g1a"           (5 cols)
      oracle: "gframe m x idx g18 g1a"       (6 cols, idx skipped)
    """
    d = {}
    with open(path) as f:
        for ln in f:
            p = ln.split()
            try:
                gf, m, x = int(p[0]), int(p[1]), int(p[2])
                if len(p) >= 6:        # oracle: idx at [3], g18/g1a at [4],[5]
                    g18, g1a = int(p[4]), int(p[5])
                elif len(p) >= 5:      # recomp: g18/g1a at [3],[4]
                    g18, g1a = int(p[3]), int(p[4])
                else:
                    g18 = g1a = -1
            except (ValueError, IndexError):
                continue
            d[gf] = (m, x, g18, g1a)
    return d


def find_anchor(d):
    """Game-frame of the boss->sim transition request = the LAST rising edge of
    ($1A != 0) while $18 == 01 (a transition staged from INSIDE an action stage).

    Why last + "$18==01": $1A!=0 also fires on overworld->act ENTRY (raised while
    $18==00) and on act1->act2; we want the act->sim EXIT after the boss. The
    recomp crashes in its setup so this edge is near the end of its trace; the
    oracle's is right before $18 becomes 08 (sim). Anchoring HERE (not act-entry)
    keeps the few transition frames aligned even though the two runs were played
    separately (earlier gameplay length drifts the offset)."""
    anchor, prev1a = None, 0
    for gf in sorted(d):
        m, x, g18, g1a = d[gf]
        if g1a != -1:
            if g1a != 0 and prev1a == 0 and g18 == 1:
                anchor = gf
            prev1a = g1a
    return anchor


def main():
    args = sys.argv[1:]
    frm, to, ctx, offset = None, None, 8, 0
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--from":
            frm = int(args[i + 1]); i += 2
        elif a == "--to":
            to = int(args[i + 1]); i += 2
        elif a == "--context":
            ctx = int(args[i + 1]); i += 2
        elif a == "--offset":
            offset = int(args[i + 1]); i += 2
        else:
            pos.append(a); i += 1
    if len(pos) != 2:
        print(__doc__); sys.exit(2)

    rec = load(pos[0])
    orc_raw = load(pos[1])

    # Always locate each run's boss->sim transition request (for windowing +
    # auto-offset). See find_anchor: the LAST $1A-rising-while-$18==01 edge.
    ra, oa = find_anchor(rec), find_anchor(orc_raw)
    if offset == 0:
        if ra is None or oa is None:
            print(f"AUTO-ANCHOR FAILED (recomp transition={ra}, oracle={oa}). No "
                  "boss->sim request ($1A rising while $18==01) found — did both "
                  "runs reach + beat the boss? Else pass --offset manually.")
            sys.exit(1)
        offset = ra - oa
        print(f"auto-anchor on boss->sim request: recomp @gf {ra}, oracle @gf {oa} "
              f"-> offset {offset}")
    # Shift oracle frames into recomp's $0088 space.
    orc = {g + offset: v for g, v in orc_raw.items()}
    common = sorted(set(rec) & set(orc))
    if not common:
        print("NO COMMON GAME-FRAMES after alignment — check both runs reached the "
              "transition. recomp gframes "
              f"{min(rec, default='-')}..{max(rec, default='-')}, oracle "
              f"{min(orc, default='-')}..{max(orc, default='-')} (offset {offset}).")
        sys.exit(1)

    def divs(frames):
        out = []
        for g in frames:
            om, ox = orc[g][0], orc[g][1]
            if om < 0 or ox < 0:        # oracle table unresolved; skip
                continue
            rm, rx = rec[g][0], rec[g][1]
            if (rm, rx) != (om, ox):
                out.append((g, (rm, rx), (om, ox)))
        return out

    # The boss->sim transition window is what matters. Default it to [ra-2 .. end];
    # divergences BEFORE the transition (e.g. $18==00 title) are frame-boundary
    # sampling phase noise (the two emulators aren't on the same instruction at
    # vblank) unless they persist — report them separately, don't bury the signal.
    win_lo = frm if frm is not None else (ra - 2 if ra is not None else common[0])
    win_hi = to if to is not None else common[-1]
    win = [g for g in common if win_lo <= g <= win_hi]
    pre = [g for g in common if g < win_lo]

    pre_d = divs(pre)
    win_d = divs(win)
    print(f"compared {len(common)} common frames; transition window "
          f"[{win_lo}..{win_hi}] = {len(win)} frames.")
    if pre_d:
        print(f"({len(pre_d)} pre-transition divergences at gf "
              f"{pre_d[0][0]}..{pre_d[-1][0]} — likely frame-boundary sampling "
              "noise on the working pre-boss path; ignore unless persistent.)")

    if not win_d:
        print("\nNO m/x DIVERGENCE in the transition window. Either the recomp "
              "decodes the transition correctly at frame granularity (the leak is "
              "INTRA-frame, settling by vblank -> needs per-instruction snes9x "
              "trace), or the window/anchor is off (try --from/--to around the "
              f"recomp crash, ~gf {rec and max(rec)}).")
        return

    g0, (rm0, rx0), (om0, ox0) = win_d[0]
    flag = ("m" if rm0 != om0 else "") + ("x" if rx0 != ox0 else "")
    print(f"\n*** FIRST TRANSITION DIVERGENCE at game-frame {g0} ***")
    print(f"    leaked flag(s): {flag}   recomp=(m{rm0} x{rx0})  "
          f"oracle=(m{om0} x{ox0})")

    print("\ncontext (gframe: recomp vs oracle, * = mismatch):")
    for g in common:
        if g < g0 - ctx or g > g0 + ctx:
            continue
        rm, rx, r18, r1a = rec[g]
        om, ox = orc[g][0], orc[g][1]
        mark = " *" if (om >= 0 and ox >= 0 and (rm, rx) != (om, ox)) else ""
        print(f"  {g:6d}: recomp m{rm} x{rx}   oracle m{om} x{ox}   "
              f"[$18={r18:02x} $1A={r1a:02x}]{mark}")

    print(f"\n{len(win_d)} diverging frames in the transition window; first at "
          f"{g0} (recomp last frame {max(rec)}). The recomp's frame-end {flag} is "
          "wrong here -> inspect that frame's transition path ($8053->$8193->C147) "
          "for the bad SEP/REP/PLP.")


if __name__ == "__main__":
    main()
