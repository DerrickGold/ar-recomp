# Builder UI style guide

Use this guide when adding a Builder page, control, dialog, or first-run screen. Start with the existing screen that does the closest job. Reuse its markup, styles, and interaction code. When changing a shared convention, update its implementation and this guide together.

## Give each task one home

| Destination | What belongs here |
| --- | --- |
| Play | Launch the game or take the next step toward a playable build. Put the dismissible rebuild notice above the page and About ActRaiser below the actions. Use the existing game scenery and respect the motion setting. |
| Setup and Build status | Use the ROM checklist for both file selection and drop results. Keep a row’s clear action inside its card. During a build, show status, actions, and stages above the expanding log. Keep navigation available. Offer the build report after failure. |
| Manage Mods | Install, list, filter, preview, enable, disable, resolve conflicts, and uninstall eligible packs. Show the active filter and how to clear it. Identify bundled, required, or externally managed content where that affects an action. |
| Workshop | Create and edit on desktop. **Save** keeps work in the project. **Export .o2r…** produces a shareable file. **Install in game** installs the pack and opens its type in Manage Mods. Other entry points should lead to these same flows. |
| Top bar | Keep the interface language selector beside build status on every desktop page. Pair a globe with the current language’s own name. Use the same cue on Big Screen’s Play menu. |
| Builder Settings | Scenery, launch preferences, help, and project links. |

Desktop and Big Screen read the same installation, availability, and conflict choices, including packs discovered through RecompLauncher. Big Screen supports playing, building, browsing files, and managing installed mods. Entering Workshop switches to the desktop authoring interface. [Builder workflows](../docs/builder-workshop.md#find-the-right-task) · [Launcher integration](../docs/desktop-packaging.md#recomplauncher)

For example, adding a new pack type gives it an entry in the existing type filter, a preview, and the applicable install and activation actions. A conflict choice or a disable action leaves the competing packs installed. Reserve removal for an explicit, confirmed uninstall.

## Size and align by purpose

| Element | Use the existing pattern |
| --- | --- |
| Page | Use `.panel`, `--workspace-width`, and `--workspace-gutter` for desktop content. The page title, toolbar, cards, and banner share an outside edge. A card supplies its own inside padding. |
| Text and color | Use `--font-ui` for controls and working text. Desktop page, section, and subsection headings start at 32, 19, and 16 CSS pixels. Keep display typography for the game identity. Use `--ink` for content, `--muted` for supporting text, and `--gold` for the primary action and focus. Pair state colors with words or symbols. |
| Buttons and fields | The desktop defaults are a 48 px minimum control height and 14 px control text. Use the shared primary, `.secondary`, `.quiet`, and `.danger` button styles. Line up the **visible control boxes** of selects, text fields, file pickers, and adjacent actions. Put help or pending-file text on its own line underneath. |
| Compact controls | Density belongs to the group. Mod table actions use a 38 px minimum, Workshop tabs use 42 px, and translation tool icons use 32 px squares. Keep compact tools together. Use Big Screen’s scaled buttons and focus rings for controller actions. |

The shared tokens also style the standalone game-folder chooser. Change a common control there when both surfaces need the change. Desktop layout, controller layout, and desktop authoring each have a stylesheet for their own needs. [Shared controls](internal/workshopui/controls.css) · [Desktop theme](internal/builder/web/theme.css) · [Big Screen](internal/builder/web/big-screen.css) · [Translation editor](internal/builder/localization.css)

- Put related actions in one wrapping row. Use the parent’s gap for spacing and keep Play and Manage Mods together when they fit. Place a link icon directly beside its text. Let a longer translation wrap the group before shrinking its text or hit targets.
- Place each interactive element beside its peers in the markup. A ROM card can contain a select button and a separate clear button. Each gets its own hit target, accessible name, and focus stop.
- Let the **available panel width** decide when editor columns collapse. Keep the message tree beside the editor while both are usable. For repeated cards, share title and description rows so a two-line title keeps neighboring descriptions aligned.
- Give paths and filenames room to wrap. In the controller file browser the current folder has its own full-width line below the title. Keep status and actions reachable as lists, previews, and logs grow. Sticky controls use the measured header height.
- Show controls when they have a job. Hide empty scenario groups, replace keyboard hints with the active controller glyphs, and keep short success messages local to the action.
- Use visible labels for fields. Give compact icon actions a localized accessible name and a tooltip available on focus and hover. Keep required instructions visible. Localize interface text, including errors and glyph descriptions, while preserving names supplied by the user. [Translation toolbar](internal/builder/localization.html) · [Interface catalog](internal/interfacecatalog/messages.json)

## Match the input and the state

| When adding… | Follow this interaction |
| --- | --- |
| An installed mod | On desktop, use an Enabled checkbox and aligned action buttons. In Big Screen, give each mod one focusable row. Up and Down highlight a row. The bottom face button toggles an eligible pack, the left face button previews it, the top face button opens its actions, and the shoulders page. Show the active controller’s glyphs. Restore focus to the same mod after a refresh, or to a neighboring row if it was removed. [Mod list](internal/builder/web/mod-packs.js) · [Controller list](internal/builder/web/big-screen-customize.js) |
| A preview or confirmation | Open a labeled in-app dialog. Keep focus in the topmost dialog, give destructive confirmations a safe initial choice, and return focus on close. Back and Escape dismiss the current layer. Put one expanded audio transport per track in a music preview. For other packs show what the player can inspect, such as artwork, manual pages, or translation coverage and credits. [Confirmation](internal/builder/web/confirm.js) · [Mod preview](internal/builder/web/mod-preview.js) · [Controller navigation](internal/workshopui/big-screen.js) |
| A file operation | Keep the real file input, its label, and its native picker on desktop. In Big Screen, open the folder directly and expose parent-folder and drive navigation. Drops and picker selections use the same validation. For exports, use the shared archive saver so the desktop shell opens the system save dialog and the browser fallback downloads the file. [File inputs](internal/builder/web/file-input.js) · [ROM browser](internal/builder/web/rom-browser.js) · [Archive saver](internal/builder/web/archive-save.js) |
| Feedback or unsaved work | Validate at selection time and name the remedy, such as “Choose an Ogg Vorbis file.” Use the shared error dialog for failures a player could miss offscreen. Show progress at the action, prevent duplicate submissions, and retain inputs on failure or cancellation. Count actual changes to draft content as unsaved. Opening a menu, searching, or listening to a preview is safe exploration. [Shared feedback](internal/workshopui/feedback.js) · [Workshop publishing](internal/builder/web/workshop-publishing.js) |

## Review a change in the running Builder

Use the rows that exercise the changed surface. Compare neighboring controls at their visible edges and test the action each one performs.

| Check | What to look for |
| --- | --- |
| Desktop fit | Try 1280 × 800 and 900 × 680. For responsive navigation, also narrow to 760 px. Capture the changed page with its menu open or its longest field populated. Check for early wrapping, stray indents, page-wide horizontal scrolling, and focus rings hidden by a sticky header. |
| Big Screen fit | Try 854 × 480, 1280 × 800, and 1920 × 1080. From a focused row, reach its actions, open a dialog, scroll the content, and return with Back. Verify that glyphs stay with their labels and the footer stays reachable. |
| Content and state | Try a longer interface language such as German, a long path or pack name, and the relevant empty, populated, pending, or failed state. Switch language with an edit in progress. Verify that the selection and draft survive. |
| Actual input | Use a pointer to open a dropdown and select a **different** option. Verify the value and its effect, then repeat with the keyboard. Exercise controller actions in Big Screen. Check native-control changes in the affected host renderer (macOS WKWebView, Windows WebView2, or Linux WebKitGTK). Mac dropdowns in particular need an actual popup selection. Record which hosts and input methods were exercised. |

Run the relevant suites from the repository root. The Builder suite covers layout contracts and interaction behavior. The desktop host suite covers the first-run chooser and shell behavior. Use the running UI to check geometry and native popup selection. [Checkout launch instructions](README.md#develop-from-a-checkout) · [Builder layout tests](internal/builder/layout_test.go) · [Native renderer smoke checks](desktop-shell/smoke_enabled.go)

```sh
go -C installer test ./internal/builder ./internal/workshopui
go -C installer/desktop-shell test ./internal/host
```
