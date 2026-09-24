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

Ending-tour dialogue already uses the ordinary interpreter through wrapper
`$01:9314`: `$03:824D` indexes the seven-entry table at `$04:CC8A` using twice
the tour index at `$0341`, after checking `$0347 == 7`. At the interpreter seam
the caller continuation is `$01:9330` and wrapper context is `$03:8251`.
The Sky Palace introduction also calls through `$01:93A8` from `$01:85E9`;
the empty-temple epilogue uses that wrapper from `$01:884C`. These scenes
share the ordinary dialogue interpreter. World-map transitions
and mode-8 graphical credits use separate presentation paths.

The dormant US sound-test routine `$02:97D4` draws source `$02:9871` via
`$02:BF60` at `$080B` (column 11, row 8), then redraws on counter changes.
Music/effect counters use `$0010/$0012`. It closes at `$02:985C` by composing
the blank source `$02:9896` at the same destination before restoring TM to
`$17`. That close is not a call to the ordinary erase routine. Its destination
overlaps an action-stage card, so the source and caller
are needed to distinguish the modal. Three label rows have two spacer rows;
the right-aligned counter fields occupy columns 18–19.
The input routine changes counters directly; there is no selector sprite.
No normal-play entry to the sound test has been established.

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

The reader **loops internally** from `$8FFF` to `$8FC5` after dictionary
expansion. Consecutive dictionary words, and the literal/control following them,
do not re-enter the function hook. An entry-only observer misses those controls:
for example, the page break in the Sky Palace magic descriptions follows a
dictionary token. Returned `A.low` and final `Y-1` identify the token dispatched at `$8E57`.
Source-byte progress includes dictionary cursor movement and is not an
expanded glyph count.

The USA/Europe-English window begins at byte offset `$04CA` in the BG3 map:
column 5, row 19, 24 columns, six tile rows. German/French begin at `$04C8`:
column 4, row 19, 25 columns. Japanese begins at `$04CA`, with 22 columns;
its clear routine is `$01:8F72`. The extractor checks the initialization and
clear-loop instruction shapes before relying on those dimensions.

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

Title `$02:A92F` installs HDMA mode/screen tables at `$7E:6000/$6800`.
The logo band uses Mode 7 BG1; the lower options band uses Mode 1 BG3
(`BGMODE=$09`, `TM=$04`). It is not BG2 text over the Mode-7 logo. Options
retain columns 12 (native arrow) and 14 (wording); Continue/New game occupy
rows 17/19, Start row 18, Professional row 21. The two ordinary selector
streams are `$02:AA34/$AA4A`; copyright uses source `$02:A9DE`.
Other ROMs' mode/difficulty menus are separate semantic routes, not automatic
translations of the US Continue/New game workflow.

The action HUD template `$02:8E7E` uploads to `$7F:B040-$B0BF`. Its packed
graphical labels are not ASCII: TIME uses tiles `$01-$04`, SCORE `$05-$08,$04`,
PLAYER `$09-$0E`, ACT `$22-$27`. `$00:A4D6` writes ENEMY `$0F-$14` at
`$7F:B0C0-$B0CB`; earlier research mislabeled that strip READY. READY is the
separate fixed record above. Japanese font source `$14:F2A6` has byte-identical English lettering at
`$60-$73`, selected by template `$02:8D27` and ENEMY writer `$00:A495`.
ACT and numeral tiles are at their ordinary indices. French/German packed
lettering is transcribed separately after ROM identity/census validation.

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
- Name entry uses `$01:EF3B` for the prompt/alphabet and `$01:EFE6` for the
  arrow record. Its selector is BG3 character `$3E`, palette 0, no flips;
  capture the current native 2bpp artwork, not a Unicode arrow substitute.
  Dialogue `$06` inserts the live WRAM name at `$0288`, via `$01:9003`,
  not the last saved SRAM name at `$1439`. Name-entry finish at
  `$02:C526-$C53C` terminates the buffer and trims trailing native spaces;
  `$034D` and the cursor scratch are not lifetime-wide name-length metadata.
- Message-speed digits are ten distinct selectable positions (0 through 9).
  The paired directional graphic is artwork, separate from Fast/Slow labels
  and the moving selector. Fixed-composer codes `$3D/$3C` identify it in
  Western releases; Japanese uses `$1D/$1C`.
- Population symbols are context-dependent: fixed reports use `$3A/$3B`
  (French `$5B/$5C`). The same codes elsewhere may be punctuation.
- USA act scores are twelve packed-BCD words at `$02B3-$02CA`, not 24 words.
  Displayed scores append a decimal zero; leading-zero suppression must
  match the native formatter, including the zero case.
- French Cities source `$01:F4E8` fits the five-letter item heading at the
  normal native font size. Its growth/level/item headings start at relative
  columns 13/18/21 versus USA 14/19/22; both header rows occupy 26 cells.
  These heading positions differ from data-value anchors. A fixed three-cell
  allocation for the enhanced final heading is not native French behavior.

### Graphical credits pages

Credits use a distinct 16-pixel alphabet. In scene 08/01, the loader
produces twenty `$0800`-byte maps at `$7E:4000-$DFFF` and uploads the
font to VRAM word `$5000`. `$02:AB30` copies a selected map to
`$7F:B000`; `$AB65` begins its VBlank wait and `$AEEB` uploads BG3.
The native presenter owns fades, holds, input and completion-save writes.
Only the lower-row DMA makes the new lettering visible; the unconditional
top-four-row upload is insufficient. CGRAM 1 supplies white lettering,
and palette-1 initials use CGRAM 5 gold.

The Go extractor decodes complete upper/lower tile compositions, including
narrow I/J, packed lettering, French accents and macrons. Unsupported or clipped
compositions fail extraction. `NativeCatalog.credits_text` contains per-page
Unicode lines and source rows; copyright/logo pages are explicitly artwork-only.
Staff-page IDs follow page indices; the JP terminal page is mapped by meaning
(`credits.the_end` is page 16, not the US page 17). Its dormant Special Mode page
is reference-only. Regional contributor lists are preserved, not rewritten into
US staffing or gameplay variants.

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

The saved return is the last JSR operand byte; execution resumes at the
following instruction. `$01` yields with the question still
visible. Yes/No input belongs to `$8D92`; Message Speed's separate number/arrow
selector is identified by `$8C43` polls with saved returns `$8B2A/$8B31`.
The latter commits `$0200` through `$8AF5`. US/Europe offer speeds 0–9;
Japan offers 0–7. See the
[regional selector contracts](regional-differences-technical.md#town-menu-return-behavior-and-message-speed-choices).

Building Direction's instruction returns to `$824A`, where the original code
tests A bit `$40` before proceeding. `$8CCE` clears command text.

Native scrolling can remove early lines from BG3 while the same logical page
is still active. The visible tile rows therefore do not always contain the
complete page; Progress Log can scroll its salutation and first line away.
