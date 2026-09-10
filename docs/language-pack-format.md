# Language pack authoring format

ActRaiser language packs are UTF-8 directories designed for ordinary text
editors and the builder's Localization workspace. A pack contains no ROM
addresses, dictionary tokens, or font-tile numbers. Version 1 deliberately
starts fresh; the unreleased prototype format is not supported.

The game currently applies packs to simulation-mode and Sky Palace menus and
dialogue. The semantic catalog covers the whole game so later integrations can
use the same packs without another format migration. Unsupported screens keep
their native presentation for now.

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
A blank source line starts a paragraph. Retail fixed-width line endings are not
authoritative for enhanced variable-width rendering.

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
