# Optional SIM menu

Enable **Town 3D → SIM menu → Modern**. The default is **Original**; Town 3D
itself does not need to be enabled. `sim_menu_style = Modern` persists the
choice, and `AR_SIM_MENU_STYLE=1` is the diagnostic override.

**Town 3D → SIM menu scale (%)** adjusts the modern menu from 50–100% in
5% steps, with 50% as the default. It is available when SIM menu is Modern,
including with Town 3D disabled. `sim_menu_scale_percent = 50` persists the
preference; `AR_SIM_MENU_SCALE=50` is the diagnostic override. Changes apply
to the next captured frame, including when a menu is already open.
The scale applies to the dock, submenus, confirmations and Message Speed
selector. Descriptions and follow-up dialogue use their normal dialogue sizing;
HUD and native PiP sizing are independent. The SIM menu setting's help points
players to **Controls → Describe menu item** for its remappable binding.

Left/right selects one of the six original categories. Down or Use opens its
submenu; up from the first entry returns to the crossbar. Back returns one
level. The menu retains each category's selected row. Use activates the
selected native command. Describe defaults to keyboard **S** and the **top
face button** (SNES X, Xbox Y, PlayStation Triangle), with separate keyboard
and controller bindings under Controls. Describe is active only while this
menu owns input. Mouse navigation is not implemented.
Held directions repeat in the dock and inventory after 18 native frames,
then every 6 frames. Opposing directions are neutral; diagonals retain horizontal
priority. Use, Back and Describe never repeat, and the fresh-release barriers
remain in force across page/menu transitions.

## Ownership and presentation

- `sim_menu_model` owns navigation, item slot identity and fresh-release
  barriers. The game fiber publishes an immutable model, ROM artwork and
  dialogue page in `FrameSlot`; presentation never drives a second menu.
- `actraiser_sim_menu` takes over the town-specific `$8B7D` invocation whose
  saved JSR return is `$81BE`. It updates the original selection and fixed
  records, preserving the native layout in PiP. Shared Palace selectors are
  excluded by caller, descriptor and register-width checks.
- Presentation starts at the town owner's `$81AC` input poll, before the
  opening-button release wait presents its first frame. The contextual lair
  inspection has already allowed a menu at this point. This observer leaves
  native registers, input sampling and the release wait intact, and declines
  the replacement when the setting is off or the ROM artwork is unavailable.
- Black panels use the existing ROM-derived grey frame tiles. The 43 menu,
  item and Yes/No icon identities, both variants, and labels are resolved
  from the loaded ROM's tables. Pixels come from current VRAM/CGRAM through
  the runner OBJ rasterizer. The menu has no screenshot or icon-sheet asset.
- The crossbar is a centered dock with a neutral charcoal gradient that grows
  darker beneath the icons and fades into the town at its edges. All six ROM
  icons remain visible at twice their native size; a single fixed, centered
  title above them identifies the selected category with full-size native
  glyphs. Enhanced dock titles join the original label's row breaks into one
  line, so Direct the People uses the same font size as the other categories.
  The miracle submenu reserves a full name column before its separate SP
  column to keep longer names at the same size as shorter entries. Single-line
  label bounds also leave room for shadows and descenders without shrinking
  individual rows; their centers and row spacing remain fixed.
  The dock spreads out on wider displays, and selection uses the
  original color/grey variants. Submenus keep their native frames, attach just
  below the dock, and fit all eight inventory slots. All enhanced menu labels
  use a separate bounded localization frame. Its six dock titles, category
  title and eight inventory rows fit together without consuming the native
  HUD/dialogue budget. Full inventories therefore keep the selected font on
  every row and leave room for description text.
- Modern menu presentation uses the player's uniform scale. Dock panels stay anchored
  below the original HUD, while confirmation and Message Speed windows stay centered.
  Describe uses the original bottom dialogue box and retains the selected dock/submenu
  behind it. A separate ROM-framed icon/name plaque sits above the dialogue frame,
  aligned to the left for LTR text or mirrored to the right for RTL text. Its
  direction follows the resolved label (including automatic bidi detection).
  Neither plaque nor menu consumes dialogue/continuation space; input remains
  exclusively in Describe until acknowledgement or Back returns to the selection.
  Command follow-up dialogue uses the original bottom dialogue box without a
  menu icon/title, independently of the menu scale. Native PiP retains its
  established bottom-right position and size throughout all menu phases,
  including Describe, Message Speed and follow-up dialogue.
  PiP is an optional comparison overlay: overlap is expected, and menu/dialogue
  layout does not reserve space or reflow around it.
  The HUD and the authentic PiP keep their independent native presentation.
- Main native/flat presentation removes the original menu group and draws
  the compact crossbar. The separated town path draws the original ground
  planes and independently captured world objects when a flat view is needed.
  This capture also runs while the modern menu is open with Town 3D disabled:
  filtering the menu records and selection overlays covers inventory sprites
  allocated after the HUD hourglass, rather than assuming one OAM prefix.
  Native PiP retains the original menu panels, icons and HUD.
- The existing HUD compositor remains responsible for the HUD. No launcher,
  controls footer, target descriptor or extra scene overlay is added.
- The artwork decoder is checked before claiming a menu. A failed preflight
  retains the native selector; a subsequent capture/upload failure is reported
  through the existing fatal presentation error path.

## Commands and dialogue

The [native reference](sim-menu-reference.md) remains the authority for native
action and Use Offering order.

Miracle Use runs the original action dispatcher and its town/SP gates. Only
the individually audited description and targeting
instruction call sites are fast-drained by the real text interpreter. Native
questions, errors and outcomes retain their dialogue. Errors and outcome text
reuse the original bottom dialogue box. Confirmation places the actual question beside the original
circle/cross icons and Yes/No labels and accepts vertical navigation (with
horizontal aliases). Native
No/Back cleanup completes before the original owner is asked to reopen the
same miracle entry. Successful actions and target cancellation retain native
completion behavior.

Miracle Describe invokes only its pure text source, on a synthetic, checked
RTS boundary. It cannot return into the miracle action handler. The original
text interpreter and localization scheduler own authored pages, reveal and
continuation. Back drains only this read-only invocation; Use first completes
the current reveal, then a fresh press acknowledges the page.

Other entries and held offerings use separate, zero-gameplay-control Help
sessions. Each display page contains at most the original 22 × 6 text cells,
with the full 192 × 72 native dialogue footprint. Icon/name metadata sits in
a separate plaque outside the native frame, whose bottom position and border
tiles match ordinary follow-up dialogue.
Overflow uses UTF-8 grapheme/source offsets, the native continuation tile,
and fresh acknowledgements. A completed Help session returns to its original
selection. It never selects Yes or dispatches an item.
Automatic overflow of these added Help pages keeps a short sentence opening
with its continuation when a nearby sentence boundary fits the native grid.
This does not edit ROM dialogue, authored page breaks, or authored line breaks.
The native and enhanced views share the resulting source offset and page advance.

**Use Offering** replaces only the held-item selector at `$8CF0`, saved return
`$84EF`. It preserves the original slot identity, checks it against live
inventory immediately before Use, and returns A=slot/C=0 to `$84FA`. The
original `$9C6E` dispatch owns everything after that: dialogue, effects,
consumption, placement selectors and their special stack unwinds. In
particular, Wheat/Bread/Skull instructions are retained, and immediate Bomb
effects receive no invented confirmation. Native execution ownership does not
release presentation: all instruction, error and outcome pages from this
dispatch use the original bottom dialogue box, without an item icon/title,
with the original continuation,
localization scheduling and effect/consumption order.

Message Speed uses its original `$8AF5` logic. Its question, sample and
cancellation text use the ordinary bottom dialogue box, with native reveal,
pages and acknowledgement. The original 0–9 scale, arrow and Fast/Slow labels
appear in a separate compact centered window only when the native selector
takes input. This window follows menu scale; the dialogue keeps its ordinary
footprint. Neither window needs an item icon or title. Native PiP retains its
established bottom-right position and size throughout this flow.
Progress Log keeps both native save/continue decisions, presented with their
questions and the original Yes/No icons. Its final acknowledgement uses the
original bottom dialogue box. No/Back retains the original branches;
only miracle cancellation has the deliberate return-to-selected-entry change.

Take Offering and Listen hand off to the cathedral. Master status, city status
and Palace navigation retain their standalone windows. The departing town's
panels and menu sprites remain suppressed until the game changes scenes, then
the destination takes over normally. World placement selectors retain native
control with the town menu hidden. The independent native PiP remains authentic.

The adapter's `Dialogue`, `MessageSpeed` and `Handoff` phases own presentation
only. `$8E29` starts a command dialogue, `$8CCE` clears it, and the exact Message
Speed input callers `$8B2A/$8B31` identify its scale. This avoids treating a
native subroutine call as a request to restore the entire old town menu.
Audited Yes/No source-and-caller pairs keep their modern window while text
reveals, before the native selector takes input. Message Speed instead keeps
its question in the bottom dialogue box while its scale opens separately.
Terminal action text uses the plain bottom dialogue box; Describe retains its item context.
This includes Direct the People / Building Direction follow-ups after the
native planning selector, including cancellation and completion messages.
An operation having gameplay effects does not make its follow-up text a
selector: only the audited Yes/No prompts use the combined confirmation layout.

The relocated text chunk identifies its captured BG3 source to the localization
compositor and reports successful dialogue presentation back to the scheduler.
Without both, enhanced text silently falls back even when the replacement
window is visible. Confirmations measure the actual captured native ink or
shaped localized ink, including shadows, before centering a fitted window.
Question reveal and active Yes/No share that same geometry; choices appear only
when their native selector takes input. For ROM-font prompts, a bounded measurement
of the eight audited prompt records expands dictionary tokens and counts native
rows without interpreting effects or publishing text. Enhanced prompts use their
complete shaped page. Header and translated choice widths come from rendered
surfaces; the ROM ASCII fallback alone uses a fixed glyph advance. The header
can use the available screen width without estimating UTF-8 bytes as characters.
Message Speed reveals its question in the bottom dialogue box before showing
its separate number/arrow window; the dialogue does not move at that transition.
For confirmations, the icon/name/cost and question form the left column;
original Yes/No icons and the captured native labels form a vertical column on the right. Up/Down
uses the native selection logic; Left/Right remain compatible aliases.
Both paths project from the full native text area; a short question does not
reserve a full blank page and a longer question is not cropped to a guessed
tile-row height. Both confirmations and follow-up dialogue restore the whole
current page if the localized native-sized viewport scrolled earlier lines
away during reveal. Dialogue windows retain at least the full native text
footprint and grow vertically for the complete shaped ink, including shadows
and continuation markers. Command dialogue reuses only the captured BG2 bottom
frame/paper and BG3 text; the old native menu above it remains suppressed.
Describe redraws the modern dock and its originating submenu behind that box;
action follow-ups remain plain dialogue without the dock or metadata. Its original
border thickness and bottom position are retained, extending the middle rows
upward only when enhanced glyphs need more room. Reveal progress, page boundaries
and acknowledgement timing remain owned by the native dialogue flow.
During one Describe invocation, the presenter retains the largest required
height so a shorter continuation cannot move the box or plaque downward. A later
longer page may grow it. Another invocation, font-size change or viewport change
resets this presentation-only extent; it never changes pagination or reveal.
Enhanced town rendering hides inactive town-position brackets
while the menu owns input and restores them for native targeting; the native
2D scanout retains the game's world sprites.

## Localization

All non-label text in the replacement menu uses the dialogue system. This
includes Yes/No questions, miracle and offering descriptions, Message Speed
instructions/sample text, Progress Log acknowledgements, and action instructions,
errors and outcomes. Native-script text keeps its existing scheduled dialogue
session; read-only Help uses `ArDialogueSession`. Both publish `DialogueWindow`
layouts and share the dialogue renderer's wrapping, reveal, waits, page advances,
continuation and language/style handling. No prose goes through label fitting.

`PrepareMenuDialogue` is the common presentation entry for every such body;
the surrounding confirmation or bottom-window layout only chooses placement
and artwork. Menu titles, item names, SP costs, Yes/No choice labels and selector
digits remain labels or native fixed compositions. Standalone native status
reports retain their existing structured layouts and execution ownership.

Labels use existing semantic routes. Miracle Help uses the existing miracle
description routes. Other read-only Help has 36 optional routes:

- `sim.help.category.0` through `.5`;
- `sim.help.action.01`–`.04` and `.10`–`.15`;
- `sim.help.item.01` through `.20` (IDs 12/13 and 16/17/18 remain distinct).

These routes have no required gameplay anchors or values. Older packs remain
valid and fall back to built-in English Help. Authored Help may use pages,
waits, supported fonts, direction and styles. Font/palette/shadow treatment
uses the existing localization pipeline; built-in Help binds the native
dialogue inks. Native PiP mirrors translated neutral Help inside the native
dialogue area, using the same page and reveal snapshot. Help publishes a
nonzero revision even for its zero-based first page. Its screen-space record
uses the ordinary dialogue layout: wrapping at the requested font size,
logical reveal, language shaping and a reserved continuation footer. Native
22-column wrapping determines the shared page boundary and ROM glyph positions,
but those soft line breaks are never inserted into the enhanced text. Authored
hard breaks and style/bidi offsets survive unchanged. The main dialogue frame
grows upward to show the complete current page without shrinking the font;
native PiP scrolls within its original text area. The native continuation arrow
stays in the footer, independently of the enhanced paragraph's line count.
Only dialogue-owning phases report native presentation tickets; navigation and
neutral Help cannot falsely fail the preceding native prompt after replacing it.

## Validation

The play build and these CTest targets pass:
`sim_menu_model`, `sim_menu_help`, `actraiser_sim_menu`,
`actraiser_localization_schedule`, `actraiser_settings`,
`actraiser_language_pack_runtime`, `actraiser_dialogue_session`, and
`actraiser_render_comparison`. The installer localization tests and generated
language-contract consistency check also pass.

Adapter tests cover all five miracle description call boundaries, exact skip
scope, native register/stack preservation, original-mode exclusion, artwork
preflight failure and No/Back restoration. Help tests cover authored pages,
overflow, waits, combining graphemes, independent sessions and slot identity.
Screen-dialogue presenter tests also compare its raster plan with ordinary
dialogue, verify a stable font across different page lengths and font settings,
and check hard breaks, logical reveal and scrolling without rerasterization.
Scheduler fixtures exercise added pages/waits on Sun description and confirmation,
the Progress Log continue question and acknowledgement, Direct the People follow-up,
and offering-inventory cancellation. Each publishes dialogue text and pays its
page acknowledgement before returning to the original native caller/selector.
Selector-source checks distinguish pending miracle/save/Message Speed prompts
from terminal dialogue before input ownership changes. Plain-dialogue captures
cover the Progress Log acknowledgement in native and enhanced views, offering
effects and both Harmonious Music pages; final game state matches the prior
replays. The revised Progress Log GIF also shows PiP moving above the text box.
Direct the People additionally has a captured planning-selector cancellation:
the original "Have you changed your mind?" acknowledgement uses the plain
bottom box, returns to town, and allows a fresh menu open. Its final native
state matches the earlier recording; the adapter test covers its exact source
and caller independently of its misleading `cancel_confirm` route name.

Isolated-save game replays and final-composite screenshots exercised native
2D, enhanced flat, Town 3D, fullscreen native comparison and native PiP; Sun
confirmation/Back/Describe; the full eight-item inventory; Wheat Help with
continuation; and native offering dialogue/placement handoff. These are
targeted checks, not a claim that every town/event/item outcome was replayed.

The player scale setting was checked at 50%, 80% and 100%, including changes
while the same submenu remained open. At 50%, native/enhanced captures cover
the full inventory, Sun confirmation, multi-page Wheat Help and Progress Log.
A 244-frame native opening/closing/reopening replay found no original-panel
flash and retained identical HUD pixels. Native targeting restores its original
selection brackets after handoff. Settings tests cover bounds, availability
with Town 3D off, and save/load; the settings-overlay and UI catalog tests pass.

Confirmation sizing was additionally checked against full-screen original
Sun, save and continue prompts, with both ROM and enhanced fonts. The local
review gallery contains the three side-by-side comparisons and refreshed
walkthrough/miracle/save GIFs. Nine isolated-save replays matched their previous
final scene, SP, inventory, selection and event state after the sizing change.

For repeatable graphics diagnostics, `AR_HEADLESS=1 AR_HEADLESS_VIDEO=1`
allows `AR_TEST_RENDER_VIEW=native` or `pip`. The override waits for a current
authentic frame and is ignored in ordinary sessions. `AR_SIM_MENU_TRACE=1`
logs controller entry and navigation. Use a temporary save, the existing
replay controls, and `AR_QUIT_FRAMES` to bound a diagnostic run.

Shareable review recordings use H.264 MP4 at 896×752 (including the caption
strip), 10 fps at original speed, with no audio. The local research directory
`development/research/sim-menu/review-gifs` retains the historical clip IDs.
Run `refresh_mp4.py` there to recapture every published scenario at 50% from
the current play build, encode directly from the PNG frames, and compare native
return states. Each video is fully decoded to check integrity; metadata records
its source frames and binary hash. `build_mp4_gallery.py` publishes only after
the complete batch passes, producing the gallery, a videos-only
`sim-menu-review-mp4.zip` and the portable `sim-menu-review-gallery.zip`.
`serve_review.py` binds localhost port 8766 and supports byte-range requests
for browser seeking. The gallery's checkpoints, playback controls and final
frames allow inspection of each handover without waiting through a full loop.

The focused `polish.html` review adds before/after slow-reveal comparisons,
native/enhanced Sun questions, steady Wheat/RTL descriptions, long translated
titles at 50%/100% menu scale with 140% font size, held navigation, and native
destination transitions. It retains each capture's build hash separately from
the earlier full-flow gallery. Slow-reveal and transition clips are encoded
at 60 fps, with frame stepping and reduced playback speed in the review page.
Across 1,069 consecutive captured opening/closing/Cathedral/Palace frames,
the native town panels did not flash into the main view. The native PiP keeps
its intentionally original menu. Pixel measurements confirm one modal extent
through Sun reveal/selector transitions, and one Wheat box
extent across both Help pages. Arabic and German strings are local layout
fixtures. This pass excludes audio changes and successful-action coverage.

The Message Speed review adds the `polish-speed-split-*` clips:
ROM-font and enhanced/PiP acceptance, slow reveal and cancellation, and native
2D at 100% menu scale with 140% font size. The 741-frame slow/cancel capture
keeps the bottom dialogue stationary while the separate 112x48 logical-pixel
selector appears and closes. Accepting saves speed 2; cancelling retains speed
3. Both paths acknowledge back to town and reopen the dock. Earlier combined
and resizing Message Speed clips are explicitly labelled historical in the
review. The release build and adapter/text-presenter tests pass for this change.
The refreshed `polish-speed-fixed-pip` clip replaces the enhanced/PiP review
after restoring the inset's established bottom-right placement and size.
Menu states no longer alter PiP geometry; the native Help text mirror remains.

Intermediate-scale checks at 55%, 65% and 85% use nine isolated replays and
510 original-resolution (1792x1344) captures. Native 2D/ROM-font and Town 3D/
enhanced-font runs cover all categories, confirmations, the eight-item
inventory, both Wheat Help pages and Message Speed. Three additional German
title fixtures use the 140% font setting. `scale-audit.html` exposes matching
checkpoints, with full-resolution images for inspecting borders and shadows;
`audit_intermediate_scales.py` verifies build identity, native return state,
HUD ink coverage and unchanged ordinary-dialogue body pixels across scales.
The audit found an existing shared-snapshot limit that made the final two
inventory rows fall back to ROM lettering. All modern labels now use their
own frame; a regression test fills the native frame to capacity and verifies
all eight labels, Describe metadata and confirmation measurements remain
available without modifying native HUD/dialogue records. Borders, icons and
long labels fit at all three tested scales after that fix.

`sim_menu_art` additionally tests full-prompt measurement with synthetic ROM
data, native dictionary words/row breaks, salutation bounds and fail-closed
handling of unknown controls/sources. Model tests cover directional repeat,
opposing/diagonal inputs, held action suppression and stable dialogue session
identity. The model, Help, artwork, adapter, localization scheduler and text
presenter tests all pass after this polish pass.
