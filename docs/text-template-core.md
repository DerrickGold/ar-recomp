# Following a text template

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

The code path is short enough to follow directly:

| Step | Entry point | What to inspect |
| --- | --- | --- |
| Parse | `ArLanguagePack_ParseDocument` in `src/localization/language_pack.c` | Script path/line, message ID, defaults, typed operations and named treatments |
| Substitute and progress | `ArDialogueSession_BeginSource` and `GetPage` in `src/localization/dialogue_session.c` | Host value contract, resolved UTF-8, style ranges, waits/controls and origin |
| Resolve appearance | `ArTextTemplate_ResolveAppearance` in `src/localization/text_template.c` | Named inks, host palette bindings, font role and authored scale |
| Shape and wrap | `ArSdlBidiText_CreateStyled` in `src/platform/sdl/bidi_text_sdl.c` | Bidi/script order, actual styled advances, preferred breaks and line baselines |
| Shape one contextual run | `ArSdlStyledRun_Create` in `src/platform/sdl/styled_run_sdl.c` | Selected font variant, SDL glyph operations, complete cluster boundaries |
| Paint | `ArSdlStyledPaint_Apply` in `src/platform/sdl/styled_paint_sdl.c` | Concrete inks, selective numeral slant, shadow and pixel ownership |
| Fit and retain | `ArSdlTextRasterizer` and `ArTextSurfaceCache` | Final size, immutable complete-page bitmap, cache identity and reveal geometry |

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
cluster. A font role's primary/fallback assets are leased by a backend instance;
active layouts pin the font variants they use. At most 32 variants are retained.

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

The game adapter now follows this path:

1. `dialogue_session` expands typed values and returns text, style ranges and
   the requested/resolved message IDs with the actual template path and line.
2. `actraiser_localization_runtime` normalizes text into one owned
   `ActRaiserResolvedText`. The composer, HUD, credits and destination label use
   that result. Name-entry edits move its value and style ranges together.
3. `actraiser_localization_text_style` compiles only the appearances used by the
   message. Native inks keep a symbolic binding until the current frame samples
   their source. Frame storage owns the resolved appearance and template origin.
4. `localized_text_presenter` sends whole pages or grid-cell views to the shared
   renderer. The view offset keeps styles attached to the original words.
   Cache identity includes appearances; revealing more characters reuses the
   already fitted layout.

Layout names and native inks are declared in
`tools/data/localization/semantic-catalog-v1.json` and generated for both the C
runtime and Go authoring tools. The machine-readable
[authoring reference](language-authoring-reference.json) lists each route's
layout and the available ink sources. The catalog's filename version refers to
its schema, independently of a language pack's format version.

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

The game and Workshop share retained-page composition in
`actraiser_dialogue_window`, fixed-page normalization in
`actraiser_localization_fixed_text`, and the name-entry, credits, HUD and
world-label adapters. The preview worker (`--text-preview-v1`) runs without a
ROM or settings file against immutable private snapshots. It returns bounded
PNG sheets, reveal clocks, controls and source/font diagnostics; the browser
only plays these images. This prevents browser font metrics from diverging
from the game. The protocol version is independent of pack format version.

The name-entry field preserves eight fixed cells and uniform underlines while
its surrounding keyboard uses explicit gutters. Styled field edits retain the
background layout. Font provenance identifies actual fallback resources and
fitted size, and survives layout caching. Arabic, Hebrew, Japanese and mixed
Latin/script fixtures exercise the real supplied fonts.

The native game owns gameplay, scene palettes, hardware observations and native
art capture. Preview inputs supply sample values/colors; reserved artwork is
clearly identified. Reusing the portable core in another game requires a source
and value contract, control acknowledgements, font resources and a renderer
host. ActRaiser route IDs, palettes, geometry, ROM control timing and v1 migration
remain outside the portable parser/session/shaping mechanics.
