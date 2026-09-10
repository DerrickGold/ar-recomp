# Dialogue and fixed-menu text: decompilation reference

This reference describes native text consumers, data identity, and observable
lifetimes. Addresses without a release qualifier refer to the verified USA
LoROM. They are not portable language-pack IDs. Retail scripts, font pixels,
generated decompilation, and replay captures must be obtained locally; none is
included here. For the author-facing contract see
[language-pack-format.md](language-pack-format.md).

## Two consumers, not one text format

| Native entry | Inputs / role | Important distinction |
| --- | --- | --- |
| `$01:8E29` | `DB:Y` dialogue stream; caller and wrapper context select its semantic invocation | Interactive reveal, input waits, continuation, scroll/clear, and yield back to a caller |
| `$01:8FC5` | Dialogue token reader within the interpreter | Dictionary expansion can emit several glyphs for one source token; token count is not displayed-character count |
| `$02:BF60` | `A = row<<8 | column`, `DB:Y = fixed record`; destination is saved in DP `$14` | Fixed menu/report composition, not the interactive control grammar |
| `$02:C1B7` | Same packed destination and source convention | Inverse record erasure; neither a general text decoder nor a whole-box clear |

Both text consumers write the BG3 staging map at `$7F:B000`. The native NMI
uploader `$02:AEEB` streams its status rows every frame and lower dialogue/menu
rows on `$F1`. Sky Palace's visible box artwork is BG2, separate from the BG3
glyphs. A cached string or unchanged scene ID therefore cannot establish that
its containing box still exists.

The menu bank interleaves records, pointers, coordinates, and instructions.
Sequentially parsing the whole bank with the dialogue grammar produces false
messages and false controls. Root fixed records at their proven direct callers,
pointer-table selections, and dynamic composer inputs instead.

## Native dialogue boundaries

| Byte / seam | Native behavior |
| --- | --- |
| `$00` | Ends the source stream and enters terminal input acknowledgement; the last page remains visible until subsequent native replacement or clearing |
| `$01` | Yields to the caller/menu; not an instruction to discard the already presented prompt |
| `$02` / `$01:8F97` | Waits for continuation, then selects clear versus retained-row advancement using `$0200` |
| `$03` | Native 30-frame delay |
| `$04` | Toggles native text presentation state; preserve its ordered control identity |
| `$05` | Resets/clears through the native text-cursor path; it can occur within an authored page, not only at page boundaries |
| `$06` | Inserts the release's addressing prefix and player name; separator behavior is release-specific, not part of the canonical name |
| `$0D` | Native line break |
| `$01:9032` | Clears the native dialogue window |
| `$01:905B` | Advances rows and scrolls the window contents upward at the bottom boundary |
| `$01:9099` | Continuation-arrow presentation; `X/2` identifies its BG3 cell |
| `$01:9261` | Waits for A/B release, then a fresh A/B press, through `$01:8C43` |
| `$01:9278` / `$01:901C` | Frame delay / glyph pacing; spaces bypass the glyph delay, other glyphs use `$0200` |
| `$01:9284` | Per-frame menu/Mode7 and OAM work followed by VBlank; not a replaceable bare sleep |

When `$0200` is zero, `$02` takes the clear path. Otherwise it retains prior
rows and advances/scrolls. The next token-reader entry is the reliable boundary
for publishing the new page: observing `$02` itself happens before its wait and
scroll complete. Treating every continuation as a fresh empty page is wrong.
Likewise, a source-end or menu-yield observation is not a text-lifetime end.

Interactive observation ABI 11 tracks locked `$01/$03/$04/$05` controls by
ordered ordinal. Exposure is not completion: the next `$01:8FC5` entry proves
the preceding native operation returned. Repeated host frame reads therefore
cannot acknowledge a delay or execute a reset twice. `$02` is an authored-page
boundary, not a locked anchor. The interpreter wrapper acknowledges `$01` only
after the native body returns; its caller/menu then owns input.

The reader **loops internally** from `$8FFF` to `$8FC5` after dictionary
expansion. Consecutive dictionary words, and the literal/control following them,
do not re-enter the function hook. An entry-only observer misses those controls:
for example, the page break in the Sky Palace magic descriptions follows a
dictionary token. The reader wrapper observes its returned `A.low` and final
`Y-1` before `$8E57` dispatches that code. Source-byte progress includes the
dictionary-chain cursor delta; it is still not an expanded glyph count. The
native decoder and its glyph pacing are not reimplemented.

Enhanced sessions consume authored text with `Next`, `TickWait`, and
`AdvancePage`, stopping at native control barriers. A native `$02` drains the
current authored page and confirms only if an authored page boundary remains;
the caller still performs its native clear/scroll. At a locked control or
source end, additional pages are confirmed before the native effect executes.
Added-page typing and optional waits call the original `$9284` per-frame work;
page confirmation reproduces `$9261`'s two `$8C43` / `BIT #$C0` loops
(release, then press), calling the original `$8C43` input-frame body at every
poll. This lets a source change retire an authored-only prompt between frames
without synthesizing input or skipping menu/Mode7 work. Native `$00`
terminal acknowledgement is retained, and `$01` remains a caller-owned menu
yield. Put all added content before a final `yield.*` anchor; both validators
reject unreachable content after it. Native-font mode only observes and keeps
all original waits and control bodies.

A `$05` clear
also retains the corresponding anchor identity, allowing the presentation
window to start inside an authored page, not only at page zero-byte offsets.
Clear-style `$02` advancement discards that anchor; retained-row advancement
keeps it. The window caches this source/page/clear identity and omits the
cleared prefix from both layout and reveal accounting. Session compilation
validates control/wait positions as whole Unicode grapheme boundaries and
caches their cluster offsets. Authored-window history is independent of native
page count: removing a source page must not clear retained translated rows.
Continuation artwork uses original tile `$5F`, relocated inside the enhanced
window, with the native DP `$88` bit-4 blink clock. Added pages do not require a
fabricated native tilemap cursor or a custom arrow glyph.

### Live source and presentation handoff

Activation during native dialogue synchronizes the semantic session from the
current reader observation before selecting the new source, even if no enhanced
frame has been drawn yet. An already-running native `$9099` owns its input
confirmation: the following reader entry advances the enhanced page once,
without asking again. In the reverse direction, if retail mode is selected
inside an adapted **native** `$02` prompt, the adapter delegates to the original
`$9099` on the existing JSR frame. A setting change cannot automatically confirm
or scroll that native page. Only an **added authored-only** prompt may disappear
without native confirmation when it no longer belongs to the selected source.

An enhanced/retail/enhanced round trip while paused retains authored page/reveal,
optional-wait progress, and completed controls. Native cursor and game-frame
observations distinguish that round trip from actual execution in retail mode;
after native progress, the session resynchronizes at the native semantic anchor.
Source contraction clamps the window to its valid page range. Active optional
waits retain their remaining duration on enhanced-source changes, and reveal
targets are recomputed against the new page length after a yielding callback.
Completed terminal/menu-yield states do not reopen earlier pages or repeat game
events. Styling changes rebuild presentation only, not the dialogue program.

Enhanced activation crosses the renderer-neutral `ArTextPresentationHost` ABI 1
before switching the semantic session or enabling authored page/wait scheduling.
The presenter opens the requested primary and ordered fallback fonts, shapes and
rasters a small probe, and creates/uploads its texture. A missing backend,
unreadable/corrupt font, or failed upload rejects the selection and retains the
previous content/presentation settings. Untouched native mode does not call this
host or load fonts. This is font-stack readiness, not exhaustive glyph coverage
or a guarantee against later allocation/layout failures.

The presenter retains at most one prepared font candidate separately from its
active surfaces. Only a frame carrying the approved font identity consumes that
candidate, so a later semantic rejection cannot destroy the previous frame's
resources. Repeated readiness checks and steady-state frames reuse font/cache
resources. Frame ABI 13 carries resolved fallback paths as bounded, owned strings;
no platform font or texture handles enter the game-thread session.

The production adapter bounds resolved dialogue to 16,384 UTF-8 bytes, counting
one separator per authored page and captured values. This conservative limit
covers even a retained window spanning the entire message; it does not assume
that a later clear will reclaim storage. Bounded begin/switch operations reject
the candidate before session mutation or authored waits. Actual frame-pool
pressure from simultaneous menus is checked again when publishing the window.

Late presentation failures have a separate feedback path. A monotonic ticket
identifies each scheduled logical window across source changes, page/clear
boundaries and game resets. A complete visible frame reports success only after
the enhanced window, required continuation artwork and final HUD composite have
drawn. Failed raster/upload/draw, unsupported projection or a native-only visible
path instead latches failure for that ticket. Forced blank and zero brightness
are intentional hiding, not failures. Old/repeated frame feedback cannot cancel
a newer window or erase a latched failure. No game RAM or interpreter controls
are exposed to the presenter.

At the next game-thread gate, a failed window retires authored waits/pages and
uses native text for the rest of that invocation. It does not change the user's
pack preference, automatically retry the broken invocation, answer a native
choice, or acknowledge a locked control. If failure happens inside a real `$02`,
the continuation wrapper delegates the original `$9099` confirmation; an
added-only prompt can simply retire. The next dialogue invocation may use the
enhanced source again. Failed partial HUD composites are discarded, not covered
with transparent native pixels that could leave stale enhanced ink behind.

The USA/Europe-English window begins at byte offset `$04CA` in the BG3 map:
column 5, row 19, 24 columns, six tile rows. German/French begin at `$04C8`:
column 4, row 19, 25 columns. Japanese begins at `$04CA`, with 22 columns;
its clear routine is `$01:8F72`. The extractor checks the initialization and
clear-loop instruction shapes before relying on those dimensions.

For Western exports, a short native line whose following whole word would fit
in the original cell budget is evidence for an intentional hard break. A word
that would overflow is evidence for a soft wrap. Exact-fit words count as
fitting. This is an export heuristic, not proof of authorial intent: preserve
breaks for unknown inserted widths or unverified geometry, and preserve
Japanese breaks without assuming space-delimited words. Authored `@line`
remains authoritative; the live renderer must not reinterpret it.

## Partial and whole-menu erasure

The shared YES/NO interaction is `$01:8D92`. Accept/reject/cancel join at
`$01:8DED`, call wrapper `$01:8CA7` from `$01:8DF8`, and reach `$02:C1B7`
from `$01:8CB0`. This can remove only the choice labels while preserving a
parent heading and the lower dialogue.

The inverse eraser recognizes only `$00` (stop) and `$0D` (next source row).
Every other byte advances one column and clears that cell **and the cell one
tile row above it**, through stores at `$02:C1E9/$C1ED`. It does not expand
dictionary tokens or interpret placeholders. A row break returns to the
original starting column on the next row. Split footprints at physical
32-column boundaries; an enclosing rectangle can erase untouched neighbors.
This routine has no frame yield.

Five decoded direct callers are `$00:A7A9/$A7B3/$A7E0`, `$01:8CB0`, and
`$02:BF37`; all use 16-bit accumulator and indices. The other direct callers
are not evidence of additional enhanced-menu coverage.

Whole clears are distinct:

- `$01:8CCE`: `$7F:B100-$B7FF`, retaining the first four status rows.
- `$02:ABC4/$02:BA41`: `$7F:B000-$B6FF`, 28 complete rows.

The fixed-compose/erase observation predicates in `recomp/bank01.cfg` and
`bank02.cfg` always return false so the native routine still runs. The partial-erasure
observer scans only bounded ROM/WRAM records, rejects unsupported index width,
and avoids duplicating MMIO reads. Clear events retire intersecting active and
dormant text ownership; changing language must not resurrect a closed menu.
Retiring ownership does not require flushing reusable raster-cache entries.

An intentionally blank translation has the same lifetime as visible text.
The frame represents it with a valid source revision/cell claim and zero
UTF-8 bytes, clusters, and inline objects. The presenter suppresses only the
claimed native cells, retains explicitly preserved native artwork and
continuation indicators, and skips font initialization/rasterization for an
all-blank frame. Missing or invalid content remains native; it must not be
confused with a successfully resolved empty replacement. This presentation
contract does not acknowledge native input or skip interpreter controls.

## Source identity and dynamic cells

`$01:8C79` resolves a one-based selected item against a pointer base two bytes
before the first entry. Normalize that to a first-entry table plus zero-based
slot; the subsequent `$02:BF60` call returns to `$01:8C93`. Two logical slots
can share a source pointer, so pointer identity alone is insufficient.

Dialogue wrappers `$01:9314/$933C/$935E/$937D/$9396` use long calls, while
`$01:93A8/$93B4` use short calls. The lightweight `$93B4` wrapper keeps its
caller's bank. Preserve both direct interpreter caller and wrapper context
when distinguishing invocations and locked native controls.

Menus/reports contain fixed rows, blank rows, independent number fields,
rules, and pictograms. Do not reflow them as ordinary prose. Specific native
facts include:

- The selected-town HUD label is composed at packed destination `$0106`:
  column 6, row 1. Its replacement owns **12 columns and one row**, not two.
  Row 2 is the live angel-health bar. The USA non-action path in `$02:C206`
  clears 12 words starting at `$7F:B08C` through `$02:C375`, then passes
  current/max HP (`$0286/$0287`) to `$02:C386`. That helper writes two HP
  units per full tile, with a half-tile remainder. A 24-HP bar occupies
  columns 6-17 (`x=48..143`); the magic OBJ starts at `x=148`.
  Enhanced town labels use the portable `SingleLineLabel` layout: leading
  edge anchoring, one fitted line, and vertically cropped/centered ink within
  the owned row. Accents, resizing and pixelation cannot claim the health
  row. BG3's fetch phase places row 1 at visible scanline 7 with zero scroll.
- Name entry uses `$01:EF3B` for the prompt/alphabet and `$01:EFE6` for the
  arrow record. Its selector is BG3 character `$3E`, palette 0, no flips;
  capture the current native 2bpp artwork, not a Unicode arrow substitute.
  Dialogue `$06` inserts the live WRAM name at `$0288`, via `$01:9003`,
  not the last saved SRAM name at `$1439`. Name-entry finish at
  `$02:C526-$C53C` terminates the buffer and trims trailing native spaces;
  `$034D` and the cursor scratch are not lifetime-wide name-length metadata.
  The following interpreter invocation orders after the keyboard's composer
  generation. Publish its accepted Unicode name before snapshotting dialogue
  values and release keyboard ownership at that boundary, not a later frame.
  Keep UTF-8 in host state and a [save companion](save-format.md#unicode-player-names-and-emulator-interchange);
  WRAM/SRAM remain native-compatible. A mismatched live fallback name must
  never use a previously saved Unicode spelling.
- Message-speed digits are ten distinct selectable positions (0 through 9).
  The paired directional graphic is artwork, separate from Fast/Slow labels
  and the moving selector. Fixed-composer codes `$3D/$3C` identify it in
  Western releases; Japanese uses `$1D/$1C`.
- Population symbols are context-dependent: fixed reports use `$3A/$3B`
  (French `$5B/$5C`). The same codes elsewhere may be punctuation.
- USA act scores are twelve packed-BCD words at `$02B3-$02CA`, not 24 words.
  Displayed scores append a decimal zero; leading-zero suppression must
  match the native formatter, including the zero case.
- Report icons and text must share a visible-ink baseline, not merely the
  same font bounding rectangle. Scaling/pixelation can change ink extents.
- French Cities source `$01:F4E8` fits the five-letter item heading at the
  normal native font size. Its growth/level/item headings start at relative
  columns 13/18/21 versus USA 14/19/22; both header rows occupy 26 cells.
  These heading positions differ from data-value anchors. A fixed three-cell
  allocation for the enhanced final heading is not native French behavior.

Enhanced Cities/Score column rows use measured widths from their complete
snapshot (header row 3 and data rows 7 onward). One column plan serves all
rows: preserve native-preferred spacing where possible, share spare width,
then reduce gutters from one native cell to two native pixels before reducing
the **shared** font size. Individual column headings are not independently
shrunk. Native borders, row anchors, divider artwork and game state do not
move; title rows and Master/cursor-based layouts remain separate contracts.
An unfit report retains native rendering rather than masking a partial table.

The renderer caches eight content/font/settings/extent-specific column plans
and reuses the same raster requests for measurement and drawing. Plans hold
only scalar geometry, never borrowed texture metadata. All cold report fits
are resolved before retaining any frame's surface references, because fitting
probes can evict raster-cache entries. An unchanged report needs no repeated
font work or uploads; a changed pack, size, pixelation, locale or viewport
recomputes its plan.

## Regional dictionary consumers

Western dictionaries have 128 fixed 12-byte entries selected by a token's low
seven bits. Their offset is release-specific; Japanese uses direct glyphs
and diacritic operations instead. A shared dictionary storage layout does
not establish identical consumer behavior.

| Release | Interactive reader | Fixed reader | Full 12-byte interactive entry |
| --- | --- | --- | --- |
| USA | `$01:8FC5` | `$02:C0DF` | No additional space |
| Europe English | `$01:8FC5` | `$02:C6F8` | No additional space |
| German | `$01:8FBD` | `$02:C701` | Append one `$20` at `$01:8FF7-$8FFE` |
| French | `$01:8FBD` | `$02:C6EA` | Append one `$20` at `$01:8FF7-$8FFE` |

Both consumers stop **after emitting `$20`**, including its cell advance.
Fixed readers additionally stop **before emitting `$00`**. Interactive readers
do not test zero inside dictionary entries. Fixed readers never append a space
after a full entry. `$40` is a blank glyph but is not a terminator. USA dialogue
signals upload through DP `$F1`; the European releases use DP `$F2`.

The fixed zero check is before its BG3 store; the interactive space check is
after its store, cursor advances, and upload request, but before character
delay. After expansion each reader restores the caller's source bank/cursor
and continues reading the next source token; dictionary glyphs are not fed
back through the top-level control interpreter.

`native_dictionary_consumers` in the extractor verifies the rooted reader
calls and instruction patterns (index ×12, bank, bounds, tests, branches and
full-entry tail) for every Western ROM before extraction. Japanese bypasses
this dictionary path. These checks are separate from the complete-source
census. A former `$40` terminator assumption emitted trailing padding that
prose normalization hid but fixed-menu layout could not hide.

Report totals are typed fields, not whitespace-delimited words. Export inserts
explicit cell separators before `total_population` / `total_score` regardless
of whether the preceding dictionary word contributes zero, one, or several
spaces. This avoids merging a total into a label when native padding changes.

## Replacement boundaries

Native game controls and input remain authoritative. Semantic routes and typed
values belong on the game side; immutable text/layout/artwork snapshots cross
the renderer-neutral ABI. A renderer must not discover dialogue state by
scanning glyph pixels or reading mutable CPU scratch. Preserve native frame,
priority, window, fade, icon, and cursor ownership separately from enhanced
glyph ownership.

Changing page count and switching language during a wait require a real
control-state adapter; matching a native page index and source-token ratio is
insufficient. Keep acknowledged native controls distinct
from authored visual pages so shortened text cannot repeat a gameplay event
and extended text cannot lose its terminal acknowledgement.

Debug hardware/RAM snapshots do not capture the recompiled CPU or suspended
game call stack. Host restore requests are rejected before any memory changes;
resetting text caches after a raw restore would not repair the execution
continuation. Exact dialogue restoration would require that runtime capability
as well as the session's stable progress, captured values, and control ledger.
It is not required for language packs, normal battery saves, or live switching.
