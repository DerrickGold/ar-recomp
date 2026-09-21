# Dialogue and fixed-menu text: decompilation reference

This reference describes native text consumers, data identity, and observable
lifetimes. Addresses without a release qualifier refer to the verified USA
LoROM. They are not portable language-pack IDs. Retail scripts, font pixels,
generated decompilation, and replay captures must be obtained locally; none is
included here. For the author-facing contract see
[language-pack-format.md](language-pack-format.md).

## Two consumers, not one text format

Extraction is owned by `installer/internal/localization`; the game
and Go author validator share the semantic contract. Profiles are declarative
Go-owned data. The former Python ROM extractor is removed, not a fallback.
See [local extraction commands](language-pack-format.md#tooling) for source
packs and diagnostic catalogues; neither catalogue JSON nor decoded ROM blobs
is a runtime language-pack format.

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

Ending-tour dialogue already uses the ordinary interpreter through wrapper
`$01:9314`: `$03:824D` indexes the seven-entry table at `$04:CC8A` using twice
the tour index at `$0341`, after checking `$0347 == 7`. At the interpreter seam
the caller continuation is `$01:9330` and wrapper context is `$03:8251`.
The Sky Palace introduction also calls through `$01:93A8` from `$01:85E9`;
the empty-temple epilogue uses that wrapper from `$01:884C`. These are existing
`dialogue.event.wrapper_*` runtime routes, not missing interpreter hooks.
The Western `dialogue.ending.slot_*` source bodies are connected to them by
author-script aliases; the Go/C extraction and edit checks verify those links
across all five releases. Native controls, software scrolling, fallback and
scene retirement use the same dialogue path as the rest of simulation mode.
World-map transitions and mode-8 graphical credits do not inherit its claims.

The dormant US sound-test routine `$02:97D4` draws source `$02:9871` via
`$02:BF60` at `$080B` (column 11, row 8), then redraws on counter changes.
Music/effect counters use `$0010/$0012`. It closes at `$02:985C` by composing
the blank source `$02:9896` at the same destination before restoring TM to
`$17`. That close is not a call to the ordinary erase routine. Surface 16
therefore retires on the exact blank-source/caller/destination tuple, as well
as ordinary region clears and scene changes. Source plus caller identifies
the modal; its destination overlaps an action-stage card. Scope is explicit
adapter metadata, not a numeric surface-ID range. Three label rows use a
fixed-row grid with two native spacer rows and fitted cached text. Explicit
label/counter fields keep the two right-aligned counter cells at columns 18–19;
extraction inserts their separators, while older single-field rows still work.
The input routine changes counters directly; there is no selector sprite.
No normal-play entry or new sound-test controls are enabled by localization.

Graphical credits form another producer/consumer pair rather than a third
dialogue grammar. Asset scene 08/01 decompresses twenty complete maps and a
separate 2bpp alphabet. The presenter copies one map into BG3 and owns its
fade/hold timing. These tile compositions cannot be decoded using the ordinary
dialogue codepoint table. See the [ending source map](rom-map.md) for the five
release-specific producers, offsets and timing. Extracting these assets alone
does not imply editable credit text or an enhanced-rendering runtime adapter.

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
native decoder remains authoritative. Do not derive enhanced typing from a
ratio of source bytes to translated characters: compressed words would reveal
in bursts separated by their hidden native glyph delays.

For an active enhanced dialogue only, the `$01:9278` delay entry is adapted
when M=1 and its stacked JSR return is `$9026` (the call from glyph helper
`$01:901C`). Each expanded non-space native glyph supplies one reveal
opportunity. The adapter reveals one authored Unicode grapheme, then calls the
original `$9284` once per `$0200` frame. Whitespace adds no delay; speed zero
remains instant. Remaining authored text at a native page/control/end boundary
uses that same clock. A shorter translation skips the surplus native glyph
delays instead of pausing after its text has finished. All other `$9278`
callers, fixed scripted delays, native mode and native control effects remain
unchanged. The leaf reproduces A.low=0, C=Z=1, N=0 and its two-byte RTS stack
consumption, preserving the high accumulator and other registers.

The cached dialogue window maps authored UTF-8 byte boundaries directly to
normalized display offsets. Whitespace trimming and reflow must not create
another reveal ratio. This bounded lookup is constant-time per capture and
does not re-shape or re-rasterize text at every character. Optional
`AR_LOCALIZATION_REVEAL_TRACE=1` diagnostics report frame/reveal-offset changes
without logging dialogue content.

The game-owned live-value capture uses ABI 2. It borrows the selected and native
fallback packs alongside WRAM/name inputs; sessions copy resolved values.
Missing city/enemy dictionary entries in a partial pack resolve from the native
fallback, without discarding the translated sentence. Authored entries take
priority; invalid or empty live text values still fail closed. Report revisions
include both pack revisions, so fallback updates cannot leave cached terms stale.

Enhanced sessions consume authored text with `Next`, `TickWait`, and
`AdvancePage`, stopping at native control barriers. A native `$02` drains the
current authored page and confirms only if an authored page boundary remains;
the caller still performs its native clear/scroll. At a locked control or
source end, additional pages are confirmed before the native effect executes.
All enhanced typing and optional waits call the original `$9284` per-frame work;
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
continues from the switched session's valid grapheme boundary after a yielding callback.
Completed terminal/menu-yield states do not reopen earlier pages or repeat game
events. Styling changes rebuild presentation only, not the dialogue program.

Enhanced activation crosses the renderer-neutral `ArTextPresentationHost` ABI 2
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
resources. Frame ABI 28 carries opaque, host-registered font resource IDs, not
filesystem paths; no platform font or texture handles enter the game-thread session.

Pack storage crosses the separate `ActRaiserLocalizationPackHost` ABI 1. The
host supplies the existing bounded `ArLanguagePackIo` VFS callbacks and the
native-pack locator; the game runtime owns parsing, contract validation,
selection and dialogue state. The desktop host installs the ordinary file
adapter and resolves the development environment override/default path. Other
ports can provide archive or memory storage without adding file or working-
directory policy to the game adapter.

Each snapshot also carries its effective source locale and paragraph direction,
copied from `ArDialoguePageSnapshot` before the resolving session is destroyed.
The fixed-composer resolver returns the same bounded value; compose state ABI 11,
HUD labels and credits cache it with the text. A partial RTL pack may therefore
publish native-English fallback and translated text together without sharing a
paragraph base. Fonts remain the selected presentation stack. Native formatted
HUD digits explicitly retain `en-US`/LTR.

Session ABI 3 also publishes semantic insertion ranges beside the original
logical UTF-8: names/text use first-strong isolation, formatted numbers use LTR,
and terms use their actual source's direction, including native fallback.
Ranges encompass whole graphemes. The compiler bounds them to 256 per resolved
message, rejecting an oversized begin/switch before replacing the active state.
Frame ABI 28 owns a 256-range pool; fixed composers retain ranges with their
cached text. Normalization, retained-page scrolling, name-entry edits, credits
padding and grid-cell slicing relocate these ranges along with the text.

Raster request ABI 12 carries borrowed source ranges and a slice offset. The
desktop backend creates layout-only isolate controls, closes them across
paragraph separators and maps shaped endpoints back to original UTF-8 bytes.
An inserted value cannot escape its isolate with unmatched controls. No hidden
bytes enter scripts, saved names, native anchors or the session's reveal clock.
These ranges and their directions participate in raster cache identity; reveal
progress alone still reuses the same raster. Alternative backends must implement
equivalent isolation/index mapping. Mixed-script visual qualification remains
separate from these automated contracts.

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
in the original cell budget is evidence for an intentional break. A word that
would overflow is evidence for a soft wrap. Exact-fit words count as fitting.
The former is exported as `@preferred-line`: raster request ABI 12 carries its
normalized byte position beside the text, and the backend keeps it only when
the segment before it still fits on one proportional line at the final font
size and projected width. If that segment already wrapped, the boundary is an
ordinary space. This is an export heuristic, not proof of authorial intent:
preserve candidates for unknown inserted widths, but keep hard breaks for
unverified geometry and Japanese text rather than assuming space-delimited words. Authored
`@line` remains authoritative and is never reinterpreted.

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

## Action HUD, cards and title options

Action maps (groups 1–7) and the title scene (`$18/$19 = 00/00`) use the
existing fixed composer, not the interactive dialogue scheduler. The fixed
route census has 90 entries. Source observations retain the native control
flow, and `$02:C1B7`/whole-map clears retire active and dormant replacements.

| USA fixed source | Packed destination | Purpose |
| --- | --- | --- |
| `$00:A851-$A8B2` (seven records) | `$080B` | Stage name |
| `$00:A8CB/$A8D1` | `$0A0D` | Act 1 / Act 2 |
| `$00:A8D8` | `$0C0D` | Clear |
| `$00:A8DF/$A8E6` | `$090D/$090C` | Ready / Time up |
| `$00:A8EF` | `$0B0D` | Shared action/SIM pause; `$02:BF37` erases it on resume |
| `$02:A9A7` | `$1100` | Continue / New game |
| `$02:A9D6` | `$120C` | Start, with its native selector |
| `$02:AA60` | `$110C` | Professional option on the fifth source row |

The pause route has gameplay scope and reuses `action.hud.pause` in SIM,
including its enhanced font, centered layout, and selected pack translation.
Existing language packs need no new entry. Other action cards remain scoped
to action maps, and resume retires the translated pause owner through the
native erase observation.

Title `$02:A92F` installs HDMA mode/screen tables at `$7E:6000/$6800`.
The logo band uses Mode 7 BG1; the lower options band uses Mode 1 BG3
(`BGMODE=$09`, `TM=$04`). It is not BG2 text over the Mode-7 logo. Options
retain columns 12 (native arrow) and 14 (wording); Continue/New game occupy
rows 17/19, Start row 18, Professional row 21. The two ordinary selector
streams `$02:AA34/$AA4A` clear Professional ownership. Native padding and
selector `>` are not new font glyphs. Copyright `$02:A9DE` is not claimed.
Other ROMs' mode/difficulty menus are separate semantic routes, not automatic
translations of the US Continue/New game workflow.

Title exit has a separate lifetime boundary: frame capture reports when Mode 7
has a nonidentity scale/rotation matrix. The runtime then releases the title
text and Professional-selector owners **after** consuming pending fixed-text
observations and before refreshing/publishing the frame. That order prevents
a redraw queued on the first spin frame from resurrecting a flat replacement.
Returning to an identity matrix alone does not restore it; a new native compose
does. Other owners, including city HUD text over the transformed world map,
are retained. This is native-art handback during the title exit, not a Mode 7
projection of the translated font; transient exit lettering remains native.

The action HUD template `$02:8E7E` uploads to `$7F:B040-$B0BF`. Its packed
graphical labels are not ASCII: TIME uses tiles `$01-$04`, SCORE `$05-$08,$04`,
PLAYER `$09-$0E`, ACT `$22-$27`. `$00:A4D6` writes ENEMY `$0F-$14` at
`$7F:B0C0-$B0CB`; earlier research mislabeled that strip READY. READY is the
separate fixed record above. Five optional `action.hud.*_label` contracts
make these transcriptions editable without invalidating older complete packs.
Japanese font source `$14:F2A6` has byte-identical English lettering at
`$60-$73`, selected by template `$02:8D27` and ENEMY writer `$00:A495`.
ACT and numeral tiles are at their ordinary indices. French/German packed
lettering is transcribed separately after ROM identity/census validation.

The runtime checks these exact destination cells before claiming labels.
Lives (row 1 columns 8–9), time (15–17) and score (26–30) use the native
writer's final `$30-$39` digit/blank cells, retaining BCD formatting and zero
padding. Labels/health bars are not inferred by scanning arbitrary glyphs.
The selected tile's CGRAM palette supplies 2bpp ink index 1 (opaque shadow),
2 (edge band) and 3 (body), with per-character banding; ACT uses a solid body colour. Numerals
request italic shaping, labels stay upright. Heart, multiplier, health bars,
and magic artwork stay in the native HUD. ACT's asymmetric ornaments also use
native pixels: the left end spans strip x=5–12 (tiles `$22/$23`), and the right
x=37–43 (tiles `$26/$27`). Their unequal top/bottom strokes extend into the
adjacent letter tiles; retaining only the two outer tiles truncates the artwork
into square-bracket fragments. The adapter captures each complete end from VRAM,
excluding letter pixels, and claims the six-cell label/frame together. The
presenter fits the text between the original-size ornaments and places them
against its ink bounds; no font bracket substitute or glyph stretching is used.
Missing artwork retains the entire native panel, not a partially replaced frame.

Frame ABI 28 passes palette RGB endpoints, shadow shape, numeral/field styling
and physical left/right/top gutters to the renderer-neutral presenter. Native
labels/capitals use ink rows 1–7; all ten digits use rows 0–7. HUD labels therefore
use a seven-pixel reference and one-pixel top inset, while counters use an
eight-pixel reference with no top inset. Town names retain the capital inset.
Raster request ABI 12 includes all style choices in cache identity.
Right alignment is independent of
Unicode direction. The font backend resets both primary and fallback styles
on each raster request; unchanged menus/counters reuse cached surfaces. Master
INIDISP brightness is applied to enhanced text/artwork at draw time, not baked
into its raster cache or applied twice to already-shaded native HUD pixels.
Proportional HUD labels anchor to their related graphics: TIME retains six
pixels of right padding, PLAYER/ENEMY four pixels before their health bars.
SCORE stays physically left-aligned at column 21 with the native scroll icons,
independently of Unicode shaping direction.
Lives/timer align left toward the multiplier/label; only score stays right-aligned
in its reserved digit field. This avoids widening the native gaps as glyph widths
or font size change. These physical gutters do not mirror with shaping direction,
do not move native graphics, and do not stretch the font. PLAYER/ENEMY also retain
their five-pixel left gutter. Their tile claims
cross the HUD's y=20/y=28 band boundaries by one scanline: projection can join
contiguous pieces only when their output horizontal placements agree, including
zero-height slivers at small HUD scales. No join bridges independent anchors.

### Styled ink, reveal and portability

The ordinary dialogue, menu, keyboard, town, title-option and action-notice
adapters explicitly publish the live palette-0 three-ink style. At full
brightness the US sampled palette is `$0000,$0000,$7F33,$7FFF`: transparency,
opaque black, blue `#9CCEFF`, white. A zero RGB shadow is still enabled.
Credits use their separate white/gold one-ink font and do not receive this style.
Native hearts, selectors, underlines and frame ornaments retain their own pixels;
they are not shadowed a second time.

World navigation is the first non-tilemap consumer of the same contract. The
native OAM composition is classified as a variable zero-to-nine-glyph location
prefix, a fixed 6x2 plaque, and the Palace's fixed 3x3 sprites. Capture preserves
those as three independent immutable layers. Frame ABI 28 can publish a bounded
`ArLocalizationScreenTextRecord` in authentic 256x224 coordinates without
pretending that OBJ owns BG3 cells. Enhanced presentation suppresses only the
captured glyph prefix, retains the native plaque and Palace, resolves
`city.*.name`, fits one line into `(156,25)-(232,33)`, and aligns it to the
logical leading edge (left for LTR, right for RTL). Its body, band and diagonal
shadow come from live OBJ CGRAM entries 132, 131 and 129. A hidden label,
unrecognized OAM, invalid pack value, failed shaping/upload, or forced blank
retains the native path. The translated surface is tinted by the captured
INIDISP brightness at draw time, so font-cache identity does not churn through
navigation fades.

Semantic grid value cells request whole-field italic shaping, including
locale-specific numerals. Mixed ordinary text such as `ACT-1` uses a separate
portable oblique treatment for complete ASCII-digit clusters only. It preserves
shaping, advances and baseline, and leaves combined/ligature clusters and
non-ASCII numerals unchanged. Whole-field italic takes precedence, so no glyph
is slanted twice. These vector-font effects approximate the hand-drawn retail
letterforms; they do not promise identical contours.

The rasterizer reserves effect padding before fitting, wrapping and cropping.
Padding never expands a text box's ownership. Bitmap ABI 3 optionally supplies
a tightly packed `pixel_owners` plane (cluster index + 1; zero for transparency).
The SDL backend supplies it for revealable text and carries ownership through
numeral slant, shadow and cropping. Typographic cluster rectangles remain layout
metrics, not overlapping effect masks. The cache samples ownership at the same
source pixel as mosaic/nearest upscaling and retains disjoint draw rectangles.
Partial reveal and spaced keyboards submit these in bounded geometry batches;
completed unshifted text still uses one whole-surface draw. No font work,
allocation or upload occurs merely because another character is revealed.
Effect metadata counts against the cache's byte budget, with independent
per-request/rectangle ceilings. Cache failure retains the existing native
fallback behavior. An alternate raster backend may omit ownership for its
legacy rectangle path, but must provide ownership to guarantee effect-safe
partial/shifted rendering.

For visual diagnostics, `AR_SHOT_REQUIRE_COMPOSITE=1` makes a missing final
readback or failed screenshot write a fatal test failure. Shot logs report
`capture=final-composite`, `native-framebuffer` or `failed`; without the strict
option, a native framebuffer fallback remains available. A native VRAM dump
does not establish which pixels the enhanced presenter actually drew.

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

Fixed-table content shapes are defined in
`tools/data/localization/presentation-shapes-v1.json`. The contract generator
emits Go author metadata and the portable C row registry; the game grid consults
the latter before publishing geometry. This shares accepted field counts and
reserved rows without moving native pixel coordinates into authoring or the
renderer. Validators count normalized logical rows, retaining internal blank
rows but discarding leading/trailing blanks. The JP reference speed scale has
eight positions; the US-runtime scale has ten. Profile-specific source shapes
do not grant playable packs a different native input range.

Dialogue session ABI 2 retains a compact authored-boundary bitmap beside each
resolved page. Literal `|` bytes and explicit authored newlines set bits;
captured names/numbers, localized terms (including aliases), and icons cannot
create them. Fixed-text normalization remaps that structure while collapsing
whitespace; inserted value newlines become inline spaces in tables. Compose
state ABI 11 retains the map and frame ABI 28 copies it into the pointer-free
text pool (one bit per UTF-8 byte, 2 KiB maximum per frame). Grid parsing and
column-plan cache keys consume that structure rather than reinterpreting all
resolved punctuation as layout. Ordinary dialogue/keyboard pipes remain text.
This avoids changing or banning valid player-name characters to protect menus.

The playable alphabet keyboard also has a generated content contract. Its
last five rows contain 13 space-separated grapheme keys; the preceding rows
hold the name and underline slot. Native input owns the last row's backspace
and finish columns, so those typed objects cannot be moved by a translation.
The author validator checks all pages and shares its key reader with native
selection mapping. The builder's grapheme data is generated from the same
embedded Unicode properties as the game, without a runtime Python dependency.
The 63-line/3,072-byte author budget leaves room for the runtime page indicator,
Unicode name expansion and enlarged gutters in the 4,096-byte compose buffer.

The name row is a live field: the presenter caches the prompt and keyboard
with that row blanked and draws each typed grapheme in its own fixed cell.
Typing a new grapheme rasterizes/uploads only that cell; reusing a cached
grapheme, deleting, or moving the selector needs no text rasterization or
upload. The source revision excludes the name and selector, but still changes
with the authored keyboard page. Layouts that cannot be split safely (including
right-to-left pages) retain the whole-page fallback. These are text-cache
savings; the scene still draws normally each frame. The arrow centers its
visible pixels on the selected key's cached ink, so other keys' descenders
cannot shift it. Artwork keys are identified by their typed inline objects and
use the row center shared by the action artwork.

Composition publishes explicit field bounds and cell count; underline objects
are decorations and do not enable or disable the live-field path. The initial
Unicode key lookup retains a normalized page until the source selection,
keyboard page, captured name, or entry generation changes. The final composition
still resolves the newly edited name each time. `AR_TEST_NAME_ENTRY_BENCH=1`
with the localization schedule test measures this native-compose/capture path.

`ArLocalizedTextPresenter_GetLiveLineStats` reports attempts and outcomes since
presenter reset. A whole-page fallback records its reason (unsupported layout,
invalid field, capacity, spans, raster/upload failure, metrics, or placement)
and logs the first occurrence of each reason, including the backend error when
available. Expected fallbacks remain supported, and transient failures retry.

### Graphical credits page adapter

Credits are a distinct 16-pixel alphabet, not `$901C` dialogue. In scene 08/01,
the asset loader produces twenty `$0800`-byte maps at `$7E:4000-$DFFF` and
uploads the credits font to VRAM word `$5000`. `$02:AB30` copies one map to
`$7F:B000`; native code retains all fade steps, holds, input and completion-save
writes. `ActRaiserLocalizationCredits_Append` matches the captured VRAM map to
those resident maps in that exact scene/font context. It observes no timer and
writes no RAM. Clears, mismatches and scene changes remove the claim; selection
changes invalidate only the resolved text cache. It can activate without any
preceding dialogue observation, including during a debug recovery.

The Go extractor decodes complete upper/lower tile compositions, including
narrow I/J, packed lettering, French accents and macrons. Unsupported or clipped
compositions fail extraction. `NativeCatalog.credits_text` contains per-page
Unicode lines and source rows; copyright/logo pages are explicitly artwork-only.
Staff-page IDs follow page indices; the JP terminal page is mapped by meaning
(`credits.the_end` is page 16, not the US page 17). Its dormant Special Mode page
is reference-only. Regional contributor lists are preserved, not rewritten into
US staffing or gameplay variants.

At presentation, one atomic grid owns all lettering cells: columns 0–31, rows
1–26. Rows 0 and 27–31 must be native blanks. This avoids clipping the SNES
first scanline and stretching the grid's vertical pitch. One to six authored
lines are centered on a four-cell pitch; all native lettering is masked even
when the replacement has fewer lines. A failed row keeps the entire native
page. A warm page resolves no text and uploads no new textures. Palette 0 ink
(CGRAM 1) remains white; a native palette-1 initial uses CGRAM 5 gold. The generic
cluster-accent field colors its whole shaped grapheme/ligature, not UTF-8 bytes.
Existing INIDISP brightness, font-size, mosaic and low-resolution settings apply.
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

Blank-cell clearing runs must remain distinct from prose. For example, the
Western `sim.menu.choice.miracles` source includes a padded blank row, and the
Japanese `title.selector.continue` source has blank runs between explicit row
advances. Author conversion removes only a complete inline run of ASCII spaces
or tabs; row advances and locked controls remain. Spaces adjacent to a visible
label or typed value remain content. This makes explicit the blank-row padding
already discarded by the author parser; it does not change native erase logic.
Some native routes also rely on an implicit source end, which the author parser
materializes as the same terminal operation as explicit `@end`.

## Town command dialogue and selectors

The modern SIM menu uses the existing dialogue scheduler for every prose body:
miracle and offering descriptions, Yes/No questions, Message Speed instructions
and samples, Progress Log acknowledgements, errors and action follow-ups.
Titles, item names, SP costs, choice labels and selector digits remain fixed
labels. Master/city reports retain their separate structured native layouts.
The [SIM menu reference](sim-menu-reference.md) maps each action and offering's
execution order; [RAM state](ram-map.md#town-command-state) is consumer-scoped.

A prompt belongs to a selector only when its action, source and caller match.
The five miracle questions are listed in the
[miracle seam table](sim-menu-reference.md#miracle-description-confirmation-and-execution-seams).
The other selector-associated streams are:

| Owner | `$01` source | Saved JSR return | Continuation PC |
| --- | --- | --- | --- |
| Progress Log save question | `$F99B` | `$8A9F` | `$8AA0` |
| Progress Log continue question | `$F9BA` | `$8ABD` | `$8ABE` |
| Message Speed question | `$FA7B` | `$8AFA` | `$8AFB` |

The saved return is the last JSR operand byte; localization's `caller_pc24`
identifies the following instruction. `$01` yields with the question still
visible. Yes/No input belongs to `$8D92`; Message Speed's separate number/arrow
selector is identified by `$8C43` polls with saved returns `$8B2A/$8B31`.
The latter commits `$0200` through native `$8AF5`, not a host-only slider.

Use fast-drains only audited optional descriptions/targeting instructions
through the actual interpreter; it does not skip a whole dialogue routine or
all calls through `$93A8`. Building Direction's instruction returns to `$824A`,
where the original code tests A bit `$40`, so a skipped instruction must supply
an explicit proceed result. Miracle Describe enters only the pure description
source through a checked synthetic RTS boundary, never the action's gates or
effect body. Other Describe entries use zero-gameplay-control `ArDialogueSession`
Help routes; [authoring rules](language-pack-format.md#script-basics)
are the same page/style rules as ordinary text. A Help acknowledgement cannot
become a native Yes or item dispatch. Fresh release/press barriers separate them.

Native scrolling can remove early lines from BG3 while the same logical page
is still active. Repositioned dialogue must retain the **complete current
page**, its reveal position, continuation and authored breaks, rather than
copy only the currently visible tile rows. This is the Progress Log case where
the salutation and first line otherwise disappear. Enhanced wrapping uses the
dialogue layout at the selected font size; ROM soft wraps do not become hard
breaks or trigger per-label shrinking. The main view and PiP share page progress.

Native execution handoff does not end text presentation. Use Offering retains
its native instruction/outcome/effect order, and Building Direction's terminal
follow-up remains ordinary bottom dialogue. `$8CCE` clears command text; a
scene change retires the owner. The original root panels need independent
suppression throughout those flows because text, frame tiles and fixed OBJ
records are separately owned. The renderer receives an immutable menu/dialogue
snapshot and acknowledges the relocated text's presentation to the scheduler.

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
