# Renderer thread-affinity map

Why this exists: the present thread issues SDL renderer calls while the main
thread also touches the renderer. An OpenGL context is current on exactly one
thread, so on Linux/GL the present thread's draws land with no current context —
black window, game running fine underneath. Metal (macOS) and Vulkan tolerate it;
that is why the Steam Deck only renders with the Vulkan renderer forced.

This maps every renderer/texture call site by the thread it actually runs on, so
we can choose between "move renderer creation to the present thread" and "move
rendering back to the main thread" with real numbers.

Generated 2026-07-23 against `1ad3645`. Line numbers will drift.

---

## A. Present thread

Entered from `PresentThreadFn` (main.c:617) via `PresentUpload` and
`PresentComposite`. This is the overwhelming majority of all renderer work.

| File | Scope | Notes |
|---|---|---|
| `present.c` | ~30 functions, essentially the whole file | All compositing, sim3D, shadow/rim/cloud passes, HUD |
| `diorama.c` | `Diorama_Upload`, `Diorama_Composite` + helpers | `DrawDioramaSkybox/Shoebox`, `BuildDioramaSupersample`, shader `Ensure*` |
| `settings_overlay.c` | `SettingsOverlay_Render`, `SettingsOverlay_RenderDebugPanel` + all `Draw*`/glyph helpers | Called from present.c:2711, 2893, 2912, 558 |
| `main.c` | `PresentThreadFn` itself | `SDL_RenderPresent` x3, `SDL_SetRenderVSync` |

**Texture creation also happens here, lazily, mid-frame** — this is the part that
makes "just move creation to the main thread" insufficient:

- `present.c::EnsureHudCompositeTexture` (353)
- `present.c::CreateSimShadowTarget` (961)
- `present.c::EnsureSimCloudTexture` (1859) — also `SDL_LockTexture`
- `present.c::EnsureSimUnderlayTexture` (1583) — 2x create + lock
- `present.c::UploadSimTownCanvas` (1544)
- `diorama.c::EnsureDioramaSupersampleTexture` (465)

## B. Main thread, while the present thread is live

### B1. Guarded by `PresentThread_Quiesce()` / `Resume()` — 4 sites

| Site | Function | Renderer work |
|---|---|---|
| main.c:1008 | `WriteFramebufferPpm` | `SDL_RenderReadPixels` x3 |
| main.c:1335 | `OnSettingsAction` | via `Diorama_ResetCamera` path |
| main.c:1605 | `OnRuntimeSettingChanged` | `ApplyDisplayPresentation`, `ApplyRefreshVsync`, **`SDL_CreateRenderer`** |
| main.c:2008 | `Diorama_OnModeChanged` | geometry rebind |

The quiesce handshake is doing exactly the job it was designed for. It is not the
bug — but note it only serializes access; it does **not** migrate the GL context,
so these sites are only safe because the present thread is parked, not because
they are on the right thread.

### B2. Unguarded, main thread, renderer live

These are the ones that matter for a fix:

- **`PresentThread_Resume` (main.c:604) calls `SDL_RenderPresent` on the main
  thread.** Directly in the resume path, no quiesce (it *is* the resume).
- `WindowPointToOutput` (1724), `InspectWindowPoint` (1788),
  `SettingsOverlay_DragDebugPanel` (2858) call `SDL_GetRenderOutputSize` from
  event handlers. Read-only queries, but still renderer calls off the render
  thread.

### B3. Structurally safe — before thread start (main.c:3370) or after join (3755)

No action needed; listed so they aren't mistaken for live-thread hazards.

- `main()` init block: 47 calls — `SDL_CreateRenderer` x5, `SDL_CreateTexture` x9,
  `SDL_DestroyTexture` x10, blend/scale mode setup
- `LoadHdReplacements` (3165), `BindHdReplacementSurfaces` (3261)
- `SettingsOverlay_Init` (3226) → all four atlas builders (`CreateFontAtlas`,
  `CreateIconAtlas`, `CreateDebugFontAtlas`, `CreateDialogFrameTexture`)
- `SettingsOverlay_Destroy` (3791), renderer/texture teardown

### B4. Fallback-only paths (`g_present_thread_active == false`)

Live only when the present thread was never started (headless-with-video, or
`SDL_CreateThread` failed). Main thread renders, which is correct there.

- `RtlDrawPpuFrame` (544), `SubmitFrameToPresent` (763), `main()` idle path (3675)

---

## Read of the map

Main-thread renderer use while the present thread is live is **small and already
almost entirely funnelled through quiesce**: four guarded sites, one stray
`SDL_RenderPresent` in `PresentThread_Resume`, and three read-only size queries in
event handlers.

That asymmetry favours **moving renderer creation onto the present thread** over
inverting the threading model:

- The present thread already owns ~95% of renderer calls including lazy texture
  creation, so it is where the GL context wants to be.
- The B2 set is ~4 call sites to route across the thread boundary.
- The B1 set already stops the present thread; those can become "run this on the
  present thread while it is parked" without changing the handshake shape.
- B3 is the real work: renderer + initial texture creation happens throughout
  `main()`'s init block and must move to a thread-start-time callback.

The competing option — rendering back on the main thread, emulation on the worker
— is more architecturally orthodox but reverses the M4–M8 present-thread work.

## Loose end found while mapping

`present.c::PresentSimUnderlay_Reset` (1522) has no callers anywhere in `src/`.
Possibly dead since the underlay rework.

## Method note

The call-site census came from a regex sweep over `src/*.c` attributing each SDL
renderer/texture call to its enclosing function. Attribution drifts on multi-line
declarations — e.g. it reported `SDL_CreateRenderer`/`SDL_SetRenderVSync` inside
`settings.c::FormatGamepadSlot`, which on inspection is comment prose in the
settings descriptor table, not code. Spot-check before trusting any single row.
