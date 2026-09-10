# Language pack authoring format

ActRaiser language packs are UTF-8 directories designed for ordinary text
editors and the builder's Localization workspace. A pack contains no ROM
addresses, dictionary tokens, or font-tile numbers. Version 1 deliberately
starts fresh; the unreleased prototype format is not supported.

The game currently applies packs to simulation-mode and Sky Palace menus and
dialogue. The semantic catalog covers the whole game so later integrations can
use the same packs without another format migration. Unsupported screens keep
their native presentation for now.

## Current runtime controls

The system overlay's **Localization** section separates **Game text** from
**Enhanced font** settings. Native rendering always uses the unchanged U.S.
text and font. Enhanced rendering uses the chosen text source and supports
80–140% font size, Crisp/Smooth sampling, and None/Low resolution/Mosaic
pixelation with strengths 2, 4, 6, or 8. Defaults are 140%, Crisp, and Mosaic 2.
Choose Pixelation = None for the full-resolution font without a pixelation
effect. Existing saved font preferences are retained when defaults change.
Pixelation strength is an upper bound: small windows and tightly fitted text
automatically use finer blocks to avoid reducing characters to unreadable dots.
These settings can be changed during play and are saved with other settings.

Automatic pack discovery and the builder's updated installation flow are not
connected yet. For now, Native US enhanced text requires a locally generated
complete pack at `game-assets/languages/native-us/pack.ini`; the optional
Configured pack source is supplied by the `AR_LOCALIZATION_PACK` manifest path
at launch. That path also defaults presentation to Enhanced unless an explicit
setting overrides it. A missing or incompatible source retains the prior
selection. Retail-derived extraction products are not bundled for distribution.

Before enabling enhanced text, the game tests opening the primary and ordered
fallback fonts and uploading a rasterized sample. Missing/corrupt font files or
an unavailable renderer reject the switch without adding dialogue pages or waits;
the prior selection stays active. Declared fallback fonts are used by the live
renderer. This readiness test does not verify every character in a translation:
include appropriately licensed fonts covering your language, run the coverage
check below, and preview its text.

If a dialogue later cannot be rendered, its remaining text and interaction fall
back to native for that invocation. Added-only waits/prompts retire; native
confirmations and game events remain player/game-controlled. Your pack selection
is retained, and the next message can use enhanced text again.

Scoped dialogue supports adding/removing `@page` boundaries and optional
`@wait` pauses. The game confirms authored pages before passing native control
barriers; removed pages do not leave redundant continuation prompts. Keep all
locked anchors in order, and place added content **before** a final `yield.*`
anchor, which hands input to the native menu. Content after it is rejected as
unreachable. Live source changes preserve the current semantic progress: shorter
messages clamp to a valid page, while additional pages remain reachable before
unexecuted native control barriers. Switching text rendering during a prompt
does not answer a native choice or confirm a native continuation. Already
completed terminal/menu-yield states remain complete. Changing font styling
does not restart typing. Installed-pack discovery is still separate from this
runtime behavior. Debug save-state restoration
is unsupported and is not a language-pack compatibility requirement. This does
not change normal battery saves or the intended live language-switching behavior.
`@empty` does work for scoped dialogue and ordinary fixed-menu text: it hides
the wording while keeping native choices, waits, boxes, and other artwork.
It does not skip a game event or automatically answer a prompt. Name-entry
keyboards still require their valid interactive grid and cannot be blanked.

## Checking font coverage

From a source checkout configured with SDL3_ttf enhanced-text support, build
the font checker and run it against your pack directory:

```sh
cmake --build build --target actraiser_font_coverage
python3 tools/check_language_fonts.py --pack path/to/my-language-pack \
  --sample 'Élise' --out font-coverage.json
```

The helper is also available with `-DAR_TESTS_ONLY=ON`, so configuring a
ROM-free checkout does not require generated game code.

This uses the same primary/ordered fallback font backend as the game, without
opening a window, loading a ROM, or modifying the pack. `--probe` selects a
helper built elsewhere; `--builtin-font` supplies the bundled primary font's
path for a relocated setup. Repeated `--sample` arguments test possible player
names or other dynamic values.

The JSON report identifies missing Unicode code points by script, message ID,
and source line, and records the tested font paths and SHA-256 hashes. Aliased
text is checked at its literal definition. Comments, command names and placeholder
names are not text; unresolved dynamic placeholders are listed separately.
Exit status is 0 for complete scalar coverage, 1 for missing glyphs, and 2 for
invalid input or an unavailable font/backend. This is separate from pack semantic
validation and can also check reference-only regional extracts.

Coverage is advisory, not proof of correct shaping, ligatures, emoji sequences,
or layout. Layout controls, typed inline objects, and Unicode default-ignorable
characters do not need standalone glyphs. Combining accents and ordinary spaces
are checked. Always visually review complex scripts and real dynamic values.

During play, a missing glyph logs a warning without changing dialogue progression.
Warnings are deduplicated and capped at 64 distinct characters per active font
stack, with one suppression notice after that. Coverage results are cached and
checked only when text is rasterized, not on cached menu or reveal frames.
Use the authoring report to find the corresponding source locations.

## Directory layout

```text
my-language-pack/
  pack.ini
  translation-progress.tsv
  text/
    sky-palace.artext
    simulation.artext
  fonts/
    OptionalFallback.ttf
```

`translation-progress.tsv` is editor metadata. The runtime ignores it. Font
files and authored scripts need redistribution terms appropriate for the pack.
Do not distribute scripts, fonts, or graphics extracted from a retail ROM.

## Manifest

`pack.ini` uses a small, strict INI subset:

```ini
[pack]
format = actraiser-language-pack
version = 1
id = example.fr-ca
locale = fr-CA
name = Canadian French
autonym = Français canadien
author = Example Author
license = CC-BY-4.0
direction = auto
target = us-runtime
source_profile = us
fallback = native-us
coverage = partial

[fonts]
primary = builtin:actraiser-sans
fallback = fonts/OptionalFallback.ttf

[scripts]
source = text/sky-palace.artext
source = text/simulation.artext
```

- `id` is the stable package identity. Locale is metadata, so several packages
  can target the same locale. Discovery will sort by autonym, package name, and
  package ID.
- `locale` is a BCP-47 language tag such as `en-CA`, `fr-FR`, or `ja-JP`.
- `name` is the package name; `autonym` is the language's own display name.
- `direction` is `ltr`, `rtl`, or `auto`.
- Player-selectable packs use `target = us-runtime` and
  `source_profile = us`. A local extract from another official ROM uses
  `target = reference-only`; it cannot accidentally replace the U.S. execution
  baseline.
- `fallback` is fixed to `native-us` in version 1. Missing or rejected messages
  therefore fall back to the locally extracted U.S. enhanced source and then
  to untouched native ROM rendering.
- `coverage = partial` permits per-message fallback. `complete` requires every
  catalog message available in the declared source profile.
- `source` and pack-relative font rows may repeat. Paths must be portable,
  relative, and unable to escape the pack directory.

Unknown sections, keys, values, commands, and duplicate IDs are errors. A
failed load never partially activates a pack.

## Script basics

An `.artext` message starts with `::` and its generated semantic ID:

```text
:: sky.menu.fight_monsters
Affronter les monstres
@end

:: sky.action_mode.confirm
@anchor reset_text_cursor.00
{master_name}, souhaitez-vous commencer ?
@page
Cette page supplémentaire est validée indépendamment du nombre de pages natif.
@anchor yield.01
@end
```

Ordinary physical lines in one paragraph are joined with wrappable whitespace.
A blank source line starts a paragraph. Source extraction compares each native
dialogue line plus the next complete word with the region's native cell width.
If the word fits (including an exact fit), the exporter keeps the break as
`@line`; if it overflows, the exporter makes that break wrappable whitespace.
This preserves short greetings and similar deliberate-looking lines while
allowing width-induced wraps to adapt to enhanced fonts.

This is an inference, not proof of the original author's intent: review the
exported `@line` commands and add or remove them as appropriate. Unknown dynamic
widths (such as the player name), missing geometry, Japanese text without
reliable space-delimited words, and unprofiled ending layouts preserve their
breaks conservatively. Fixed menu/table rows are always kept. This conversion
only happens during export; loading an edited pack never reinterprets `@line`.
Existing packs must be re-extracted or edited to gain these breaks; back up
translations before replacing generated files.

The supported commands are:

| Command | Meaning |
| --- | --- |
| `@line` | Intentional hard line break. |
| blank line or `@paragraph` | Paragraph break. |
| `@page` | Authored page break. Pages may be added, removed, or reordered. |
| `@wait N` | Optional presentation delay of 1–600 frames. |
| `@anchor ID` | Machine-owned native control position; do not edit it. |
| `@event ID ...` | Allow-listed semantic event. Version 1 initially has no author events enabled. |
| `@empty` | Intentionally show no text. Required anchors remain beside it. |
| `@alias ID` | Reuse another included message with a compatible contract. |
| `@end` | Explicit end; it is added implicitly if omitted. |

Use `@@` at the beginning of a text line to display a literal `@`. Use `\#` or
`\;` for a literal leading comment marker. Double braces (`{{` and `}}`) emit
literal braces.

## Locked anchors and safe extension

The extractor converts native cursor resets, text-state changes, fixed delays,
and yield/continuation points into named locked anchors. Their exact ordered
list is part of the semantic route contract. Validation rejects a pack that
adds, removes, renames, or reorders them.
Do not place an anchor or wait between a base character and its combining
accent, or inside another multi-codepoint character: session compilation
rejects controls that split a Unicode grapheme.

Text and authored pages around those anchors are flexible. A translation can
have more pages than the native message, fewer pages, or be intentionally
empty. The runtime can therefore preserve semantic/control progress while
clamping presentation state during a live language switch, without replaying a
native control or inventing a game event.

`@alias` is only accepted when the referenced message is included in the same
pack, its locked-anchor contract is compatible, and every placeholder it uses
is available to the aliasing route. Alias cycles are rejected.

## Placeholders

Placeholders use generated semantic names rather than memory addresses:

```text
Il reste {lair_count} repaires près de {town_name}.
```

The five-ROM semantic catalog records the union of values genuinely used by
each logical route. This matters when official translations phrase the same
message differently: a value seen only in Japanese may still be available to a
French community translation of that route. Unknown or context-inappropriate
placeholders fail validation.

The current catalog contains localized text/term values, numeric values, and
typed icons. Examples include `{master_name}`, `{town_name}`, `{lair_count}`,
`{master_level}`, per-town report values, action scores, and
`{icon.name_entry.finish}`. The builder presents the route-specific list and
type rather than asking authors to memorize it.

Values are snapshotted when a semantic message begins. A language switch does
not mix counters or names captured at different moments.

Number placeholders may request a minimum digit count: `{master_level:02}`
or `{city_northwall_population:03}`. These display `02` and `002` for a value
of 2. Formats `01` through `09` are supported; larger values are never
truncated. Omit the suffix for unpadded numbers. Extracted scripts retain the
native decimal padding; score fields suppress leading zeroes and use column
alignment instead. Number formatting is not valid on names or icons.

## Fixed menu and report rows

Report totals belong in their own explicit cell, for example
`Resident count | {total_population:04}`. Use `|`, not a run of spaces, to
separate editable fields. Extraction preserves this boundary from the typed
value even when a native dictionary word contributes only one trailing space.

Reports use `|` between cells, so a translated cell can contain multiple words:

```text
Next level | {next_level_population:04}
```

Keep the cell order and `@line` rows of the extracted template: those rows
align labels and values with the game's artwork and selectors. Blank rows
are deliberate. Edit the words inside a cell without adding spaces to align
it. The speed scale has one cell per digit; its selector stays in the native
column. Dialogue paragraphs still use word wrapping, not report columns.

Enhanced Cities and Score reports measure the headings and all data rows to
share horizontal space between columns. Unused space can accommodate a longer
heading without making that word smaller. Gaps tighten before the table's
shared font size is reduced; numeric values remain right-aligned in their
columns. The original box, divider and row positions stay fixed. Report titles,
the Master status artwork and cursor-driven menus retain their separate layout
rules. Do not add alignment padding to translations—the renderer handles it.

## Name-entry keyboard pages

`name_entry.prompt_and_alphabet` may contain multiple keyboard pages separated
by `@page`. Each page ends with exactly five logical rows of 13 selectable
grapheme keys. Put the alphabet and digits most players need on page 1, then
place accented letters or other language-specific symbols on later pages.

The enhanced name-entry screen displays `< current/total >` above the keyboard.
Moving left from the first column or right from the last column cycles to the
adjacent page. The selector retains its logical row and moves to the opposite
edge. The renderer reserves an arrow gutter before every key, so variable-width
letters do not overlap the authentic selection arrow.

Names support up to eight Unicode grapheme clusters. `{master_name}` uses the
accepted Unicode spelling in later enhanced dialogue, independently of which
pack is selected. Native rendering keeps the original keyboard-position
fallback spelling. Unicode is preserved in a separate `.arname` save companion,
not in the ROM-compatible SRAM name field; see
[Unicode name persistence](save-format.md#unicode-player-names-and-emulator-interchange).

Use one Unicode grapheme per position. A precomposed letter such as `é` is one
key; an emoji or a base letter plus combining marks is also one key when it is
a single grapheme. Backspace and finish are typed placeholders rather than font
characters:

```text
:: name_entry.prompt_and_alphabet
Choose a name.
@line
A B C D E F G H I J K L M
@line
N O P Q R S T U V W X Y Z
@line
a b c d e f g h i j k l m
@line
n o p q r s t u v w x y z
@line
0 1 2 3 4 5 6 7 8 9 {icon.name_entry.backspace} - {icon.name_entry.finish}
@page
Choose a name.
@line
À Á Â Ä Ç È É Ê Ë Ì Í Î Ï
@line
Ñ Ò Ó Ô Ö Ù Ú Û Ü Ý Œ Æ ß
@line
à á â ä ç è é ê ë ì í î ï
@line
ñ ò ó ô ö ù ú û ü ý œ æ ÿ
@line
’ - . , ? ! {icon.name_entry.backspace} ( ) / : ; {icon.name_entry.finish}
@end
```

Keep the row and grapheme counts exact on every page. A malformed page cannot
take ownership from the native keyboard and therefore falls back safely;
builder-side diagnostics for the complete page shape are required before the
version 1 editor ships. Keyboard pages are pack-authored and are not limited
to the official French character inventory.

## Progress tracking

The editor stores one tab-separated row per message:

```text
# format=actraiser-language-progress version=1
sky.action_mode.confirm	wip
sky.menu.fight_monsters	done
```

Valid statuses are `not_started`, `wip`, and `done`. They do not change runtime
behavior and can be committed with a community pack to preserve collaborative
translation state.

## Limits

Version 1 rejects rather than truncates content above these limits:

- 64 script files and 8 fallback fonts per pack;
- 16 MiB per script and 64 MiB per pack-relative font;
- 16,384 messages and 4,096 operations per message;
- 64 authored pages and 256 KiB of UTF-8 text per message;
- 600 frames per authored `@wait` and 3,600 authored wait frames per message.

Native locked delays are part of the generated control contract and do not
consume the author-wait budget.

The scoped Sky Palace/simulation runtime has a smaller presentation limit:
**16,384 bytes per resolved dialogue**, including substituted values and one
separator per page. This is a whole-message budget even when pages clear the
box. A new over-budget invocation uses native text; switching an active message
to an over-budget candidate keeps the prior selection. Simultaneous menu text
shares the frame buffer, and a rendered window must also fit the backend's
4,096-pixel height limit. Those later capacity/layout failures use the native
fallback described above rather than silently truncating text or leaving
invisible authored prompts. These are runtime limits, not a claim that every
pack passing the portable format validator will fit every layout/font setting.

## Tooling

The low-level tools operate on local extraction products:

```sh
python3 tools/language_pack_extract.py \
  ar.sfc ar-eu.sfc ar-ger.sfc ar-fra.sfc ar-jp.sfc \
  --out-dir game-assets/languages/sources --require-complete

python3 tools/language_pack_v1.py catalog \
  --extraction game-assets/languages/sources/en-US.extraction.json \
  --extraction game-assets/languages/sources/en-GB.extraction.json \
  --extraction game-assets/languages/sources/de-DE.extraction.json \
  --extraction game-assets/languages/sources/fr-FR.extraction.json \
  --extraction game-assets/languages/sources/ja-JP.extraction.json \
  --out game-assets/languages/sources/semantic-catalog.json

python3 tools/language_pack_v1.py validate \
  --pack game-assets/languages/example.fr-ca \
  --catalog game-assets/languages/sources/semantic-catalog.json
```

The builder will wrap these operations, create the U.S. starting source, show
messages in a navigable tree, offer contextual placeholder/control pickers,
and save progress incrementally.
