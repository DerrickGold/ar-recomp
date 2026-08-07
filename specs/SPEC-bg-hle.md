# SPEC-bg-hle — native background decode for action stages

**Status: Proposed (BH1).** The decode is fully mapped and validated; nothing is
implemented. Read §1 first — it is the finding that changes the cost of this
work by an order of magnitude.

Companion reference: `docs/rendering-engine.md` §4 (streaming), §12a/§12b (the
host margin refresher and its two cadence bugs), `docs/bug-ledger.md` §37.

---

## 1. The level map is already decompressed in WRAM

The premise this track started from — "we would have to reverse the bank-$0A
compressed level stream" — is **wrong, and in our favour**.

`$02:B8A0` never touches ROM. It reads the level map out of **WRAM bank `$7E`**,
where level entry has already expanded it. The map is a flat array of 256-byte
**pages**, each page being 16×16 metatiles = 256×256 pixels of world:

```
page_index = (world_y >> 8) * (width >> 8) + (world_x >> 8) + (State46 >> 8)
metatile   = $7E:[ (page_index << 8) | (world_y & $F0) | ((world_x & $F0) >> 4) ]
```

One byte per 16×16 metatile. That single expression is the whole "level format"
question, and it is why `$B8A0`'s fetch is **page-keyed**: only the *high* byte
of `world_x` selects a page, so a non-256-aligned `world_x` silently decodes the
same page at the wrong offset. That constraint — the source of every page-hole
workaround in the host refresher — is an artifact of this addressing, not of the
data.

Metatile expansion (`$02:B95A`) is equally small. `State52` points at a metatile
table, also in `$7E`, 8 bytes per entry = four 16-bit tilemap words in
TL, TR, BL, BR order:

```
word_k = ( $7E:[State52 + id*8 + k*2] & State54 ) | (State6B << 8)
```

`State54` is an AND mask and `State6B` supplies the attribute bits (palette /
priority / flip) in the high byte. `$02:BED3` is nothing but the SNES hardware
multiply (`$4202/$4203` → `$4216/$4217`).

**Consequence: there is no format to reverse, no compression to decode, and no
opaque decoder state.** The five "opaque" words are a WRAM base pointer, a table
pointer, a mask, an attribute byte, and a width. A complete native background
decoder is roughly the twenty lines above.

## 2. Validation — the oracle already passes

`tools/` prototype (see §6) reimplements the above and compares every tile the
current view displays against live VRAM, from snapshot WRAM alone:

| snapshot | BG1 | BG2 |
| --- | --- | --- |
| `20260806-224602/snap_00_gf3427` | 1789/1792 (99.83%) | **1792/1792** |
| `20260806-224602/snap_01_gf3545` | 1789/1792 (99.83%) | **1792/1792** |
| `20260806-231345/snap_00_gf2363` | 1784/1792 (99.55%) | **1792/1792** |
| `20260806-231345/snap_01_gf2726` | 1784/1792 (99.55%) | **1792/1792** |

BG2 is exact. **Every** BG1 disagreement is a margin-edge cell, and they are
precisely the stale cells of bug-ledger §37 — the HLE says `…0FF` (empty) where
VRAM held a trunk or a foliage fragment. The oracle reproduced that bug from a
completely independent direction, having been written after the fix and never
told where to look.

That is the acceptance gate for this whole track, available on day one: **run
the native decoder beside `$B8A0` and diff.** A disagreement is either an HLE
error or a streaming bug, and §37 shows the diff can tell you which.

## 3. What this buys

The 64×64 ring, the 512px page-keyed decode windows, the neighbour-band partial
drains, the record-ordering rules, and the drain-span/cadence coupling all exist
because a decoder built for a 256×224 window is being made to serve a 446px-wide,
band-extended view. A native walker addresses the world directly, so that entire
class disappears:

- arbitrary surface size — no ring, no wrap, no aliasing between cells 512px apart
- no page alignment rule, so no neighbour bands and no partial drains
- no refresh-key cadence, so no stale-margin class (§12b, §37)
- no WRAM/CPU/math-unit save-restore transaction per frame
- vertical extend stops being a special case — the band is just more rows

## 4. What stays hard

1. **The metatile table and map are WRAM state the game owns.** Level entry,
   room transitions, and tile animation (§7) all rewrite them. The HLE must read
   them live, never cache across a transition.
2. **Tile animation** (`docs/rendering-engine.md` §7) writes VRAM char data, not
   the tilemap — likely orthogonal, but unverified.
3. **Non-action modes are out of scope.** Sim towns, Sky Palace and the world map
   have their own paths; this spec is action stages only.
4. **Byte-identity.** The native path must be able to produce the authentic
   256×224 tilemap bit-for-bit, or it cannot be the default. §2 says BG2 already
   does; BG1 needs a clean run once §37's fix is in the baseline.
5. **Who owns VRAM.** The game's own streamers keep writing the ring. Either the
   HLE writes a separate surface the renderer samples instead, or it keeps
   feeding the ring and we gain correctness but not size. **This is the real
   design decision** — see BH3.

## 5. Phases

- **BH1 — oracle harness (do first).** Promote the prototype to `tools/` proper
  and add a runtime mode that decodes every displayed tile natively each frame
  and reports disagreements, like the §12b static-world check. Run a full
  playthrough. Deliverable: a disagreement count of zero, or a list of the map
  features that break the model. No rendering changes.
- **BH2 — decode into a host surface.** Native decoder writes an arbitrary-size
  tilemap surface. Still not displayed; still diffed against VRAM.
- **BH3 — renderer samples the host surface.** The design decision in §4.5.
  Behind a setting, defaulting off. Authentic byte-identity gate must hold with
  it on at 256 wide.
- **BH4 — retire the workarounds.** Once BH3 is the default for action stages,
  `ws_build_visible_row`, `ws_build_band_rows`, the neighbour drains and the
  refresh key all become dead code. This is the payoff; do not start it early.

## 6. Prototype

The validator used for §2 is ~40 lines of Python over `*.wram.bin` +
`*.vram.bin` from any snapshot. It has no dependencies on the running build,
which is what makes it a trustworthy oracle. Promote it to `tools/bg_hle.py`
with the same interface as the other `ar_lib.py` consumers.

## 7. Addressing summary (reference)

| Per-layer state (X = 0 BG1, 4 BG2) | Meaning |
| --- | --- |
| `$22/$24` | camera x / y |
| `$2E` | layer pixel width — `>> 8` is the page-grid width |
| `$46` | `>> 8` = first page index of the level map in `$7E` |
| `$48` | tilemap VRAM base (`$6000` BG1 / `$7000` BG2) |
| `$52` | metatile table pointer (bank `$7E`) |
| `$54` | AND mask applied to each table word |
| `$6B` | attribute byte OR'd into the high byte |

Record layout produced by `$B8A0` (`$102` bytes, `kUploadRecordBytes`):

| Offset | Contents |
| --- | --- |
| `$00` | destination VRAM word address |
| `$02` | 32 words, top tile row |
| `$42` | 32 words, bottom tile row |
| `$82` | 32 words, top row of the other 256px half |
| `$C2` | 32 words, bottom row of the other 256px half |

VRAM base = `State48 + ((world_y & $F0) << 2) + ((world_y & $100) << 3)` — a
512px vertical ring. Which half of the record lands in ring columns 0–31 versus
32–63 is selected by `world_x & $100` (`$B8A0` swaps the two `$B95A` calls),
**which is why the record is indexed by absolute ring column** — the fact proved
empirically in §12b before it was read out of the disassembly here.
