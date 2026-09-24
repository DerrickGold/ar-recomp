# Text templates and the standalone dialogue sample

V2 packs keep wording, typed values and appearance in readable templates.
The builder performs v1 migration; the game consumes explicit v2 presentation.
Workshop playback and the standalone host both exercise the actual rendering
code. Start with the [authoring guide](language-pack-format.md#v2-templates-and-appearance)
for editing, migration and visible-text tracing.

```text
@define-style prose band=#DBEAFE body=#FFFFFF shadow=#172038
@define-style emphasis band=#FFAB42 body=#FFE5A3 shadow=#241A10 shape=keyline

:: example.greeting
@style prose
Welcome, {name}.
@line
You found <i>{count:02}</i> <span font="display" style="emphasis" scale="120%">stars</span>.
@wait 8
@line
<span scale="80%">Look up and make a wish.</span>
@anchor example.arrived
@page
The next page keeps its own reveal clock.
@end
```

The template shows the words, value names, styling, explicit line/page breaks,
waits, and host control. Styles are flat ink/shadow definitions. Font roles are
separate: `display` names a stack supplied by the host or declared in a pack's
`[font.display]` manifest section. The default stack is `body` (`[fonts]`).

`@font`, `@style`, `@scale`, and `@numerals` set message defaults. Inline scopes
override only the properties they specify. An inline scale is relative to the
message scale; nested inline scales replace their outer inline scale. The final
font size is rounded once, to the nearest pixel, from the host's base size times
message percent times inline percent. The existing host handles player/output
scaling before this step. Authored percentages range from 25% through 400%; a
result outside the backend's 1–4096 pixel range is an error. Uniform fitting
reduces the base size and keeps the authored scale relationships.

Values are typed data, never parsed as tags or commands. A term supplies words
and inherits the appearance of its placeholder; author its styling where the
placeholder is used. Term definitions cannot contain presentation directives or
inline styles. An alias inherits its target's complete message presentation.

Physical source lines join with spaces, including within a tag scope. Close
tags before explicit structural commands or `|` cell separators. Escape a
literal `<` as `\<`, a backslash as `\\`, and braces as `{{` and `}}`.

Font/size/italic changes require a complete shaped-cluster boundary in both
neighboring variants. For example, changing the font halfway through a ligature
can produce an explicit diagnostic; styling the complete word avoids it. Color
changes never break a ligature: its first logical character selects its ink.
Style boundaries inside a grapheme, such as between a letter and its combining
accent, are invalid. The backend shapes each used font with the surrounding
directional/script text and reshapes at actual wrap boundaries.

The complete page is measured before its first character appears. Each line
retains its own top, baseline and height, including at least the default font's
strut. Reveal advances logical text while pixels follow their owning shaped
cluster.

## Run the independent host

With the repository's SDL build dependencies available:

```sh
cmake -S . -B build-tests -DAR_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests --target ar_dialogue_sample
ctest --test-dir build-tests -R '^ar_dialogue_sample_offscreen$' --output-on-failure
```

The test writes `build-tests/dialogue-sample.png`, a four-frame comparison. It
checks independent clocks, host event delivery, a page advance, and restoring
one session without changing the other. The sample keeps prepared pages while
revealing them; a tick does not rebuild font layout.

For an interactive window:

```sh
build-tests/ar_dialogue_sample --interactive \
  game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf \
  tests/fixtures/unicode-fonts/fonts/NotoSansJP-Bold.otf
```

Tab selects a panel, Space advances the selected panel, Return advances both,
F5 saves the selected session, F9 restores it, and Escape exits. See
`tools/dialogue_sample.c` for the entire host: synthetic scripts, a two-value
resolver, one event handler, explicit ticks, font resources, and drawing.

Session page pointers are borrowed until a session begin/switch/restore/destroy.
The session owns template and origin data independently of the pack. Bitmap
pixels, line metrics and cluster metadata belong to the rasterizer until
`ArTextRasterizer_ReleaseBitmap`. Cache metadata belongs to its cache entry.
Stable session state must be serialized by named fields, never as a raw C struct.

The machine-readable [authoring reference](language-authoring-reference.json)
lists each route's layout and available ink sources.

Placement policies never flatten explicit template breaks. Fitted labels can
disable automatic word wrapping while retaining `@line` and paragraph spacing;
each line uses the host's alignment. Bounds remain game-owned: a layout that
cannot fit produces a preview diagnostic and leaves the native game surface.

| Ink family | Native source |
| --- | --- |
| `native:dialogue.band/body/shadow` | CGRAM 2 / 3 / 1 |
| `native:hud.band/body/shadow` | Entries 2 / 3 / 1 of the recognized HUD descriptor's palette |
| `native:credits.body/accent` | CGRAM 1 / 5 |
| `native:world.band/body/shadow` | CGRAM 131 / 132 / 129 |

Inks are captured before master brightness, which presentation applies once.
A HUD field uses its own descriptor palette. Reusing that treatment in dialogue
uses the active, recognized HUD palette; if no HUD is recognized, that binding
is unavailable and the affected surface retains native pixels. It never silently
uses the dialogue palette. Fixed `#RRGGBB` inks do not need a native source.
Unknown bindings are rejected by both game contract validation and the builder.

The game currently bounds a composed text at 64 distinct appearances and 512
style spans, and 32 font/size/italic variants across its retained pages; a frame
shares a 512-span pool. Exceeding presentation capacity keeps
the affected native surface. These are host presentation budgets; the portable
parser's larger limits are independent. Workshop validation enforces those budgets before saving, previewing or publishing.

Reusing the portable core in another game requires a source
and value contract, control acknowledgements, font resources and a renderer
host. ActRaiser route IDs, palettes, geometry, ROM control timing and v1 migration
remain outside the portable parser/session/shaping mechanics.
