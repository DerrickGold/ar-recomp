# Renderer thread migration — draft

Companion to `ar-recomp-renderer-thread-map.md`, which established that the
present thread already owns ~95% of renderer calls (including six lazy
mid-frame texture creations), while the main thread's live-renderer use is four
quiesce-guarded sites, one stray `SDL_RenderPresent`, and three read-only size
queries.

Goal: the GL context ends up on the thread that actually draws.

---

## 0. The constraint that shapes everything

**Do not hard-swap the owner thread.** macOS/Metal is the platform that works
today, and Metal has main-thread expectations of its own; moving renderer
creation to the present thread could break the working platform to fix the
broken one. The window and `SDL_PollEvent` must stay on the main thread
regardless — SDL requires event pumping on the thread that initialised video.

So the migration introduces a **renderer-owner selector**, not a rewrite:

```c
/* Which thread creates and drives the renderer. Metal wants main; GL contexts
 * are thread-affine and must live where the draws happen. Overridable so both
 * can be A/B'd on either platform when something looks wrong. */
typedef enum { kRenderOwner_Main, kRenderOwner_Present } RenderOwner;
static RenderOwner g_render_owner;   /* AR_RENDER_OWNER=main|present */
```

Default: `kRenderOwner_Present` on Linux, `kRenderOwner_Main` elsewhere, env
override always wins. Every step below is a no-op when the owner is `Main`,
which is what keeps macOS bit-identical through the whole series.

## 1. New primitive: run-on-render-thread

The quiesce protocol already parks the present thread in a wait loop. That park
is the natural place to execute work on its behalf.

```c
typedef void (*RenderTaskFn)(void *ctx);

/* Runs fn on whichever thread owns the renderer and blocks until it returns.
 * With no present thread (headless, thread-creation failure) or owner == Main,
 * calls fn directly — so every call site reads the same on both paths. */
static void PresentThread_Run(RenderTaskFn fn, void *ctx);
```

State added beside the existing quiesce flags:

```c
static RenderTaskFn g_render_task;
static void        *g_render_task_ctx;
static bool         g_render_task_done;
```

The present thread's existing quiesce wait loop (main.c:626-638) gains a task
drain before it re-waits: if `g_render_task` is set, unlock, run it, relock, set
`g_render_task_done`, signal `g_present_done_cond`.

This deliberately reuses the quiesce handshake rather than adding a second
synchronisation scheme — the thread is already parked at exactly the moment the
main thread wants renderer access.

## 2. Output-size cache (kills the read-only B2 queries)

`WindowPointToOutput` (1724), `InspectWindowPoint` (1788) and
`SettingsOverlay_DragDebugPanel` (2858) call `SDL_GetRenderOutputSize` from
event handlers purely to map coordinates.

Replace with a cache the present thread refreshes each composite:

```c
static _Atomic int g_render_out_w, g_render_out_h;
```

Event handlers read the atomics. A frame of staleness is irrelevant for
hit-testing a click, and it removes three renderer calls from the main thread
outright rather than routing them.

## 3. Convert the four quiesce sites

`WriteFramebufferPpm` (1008), `OnSettingsAction` (1335),
`OnRuntimeSettingChanged` (1605), `Diorama_OnModeChanged` (2008).

Shape today:

```c
PresentThread_Quiesce();
...renderer work inline on the main thread...
PresentThread_Resume();
```

Shape after:

```c
PresentThread_Run(DoTheRendererWork, &ctx);
```

Each body lifts into a small static function taking a context struct. Note
`OnRuntimeSettingChanged` contains an `SDL_CreateRenderer` — a full renderer
rebuild — which is the single most important one to get onto the owner thread,
since a renderer created on the wrong thread re-creates the original bug.

**This step is behaviour-neutral by design.** With owner == Main the work still
runs on the main thread; only the routing changes. Land and verify it on macOS
before step 4 touches ownership.

## 4. Move creation to the present thread (the actual flip)

Today `main()` creates renderer + all boot textures, *then* spawns the present
thread at 3370 — the comment there even documents the ordering as deliberate.
That inverts:

- Everything from `SDL_CreateRenderer` through the boot texture set, HD
  replacement binding (3165, 3261) and `SettingsOverlay_Init` (3226) moves into
  a `RenderOwner_BootInit()` function.
- When owner == Present, `PresentThreadFn` calls it first, then signals ready.
- `main()` spawns the thread earlier and waits on a ready/failed condition
  before continuing. On failure it must fall back to owner == Main and the
  existing synchronous path, not die — a thread-creation failure currently
  degrades gracefully and should keep doing so.
- `SettingsOverlay_Init` needs its renderer handle at a different time; check it
  has no main-thread-only dependencies before moving it.

Window creation stays on main. Only the renderer crosses.

## 5. Retire the resume-present

`PresentThread_Resume` (604) calls `SDL_RenderPresent` on the main thread so the
first post-quiesce frame isn't stale. Replace with a `g_present_force_repaint`
flag the present thread consumes on its next idle pass — it already re-presents
the last slot on timeout, so this is a flag, not new machinery.

## 6. Verify

1. macOS, owner == Main — must be indistinguishable from today. This is the
   regression gate for steps 1-3.
2. macOS, `AR_RENDER_OWNER=present` — proves the flip works where we can debug.
3. Deck, owner == Present, GL renderer **not** forced to Vulkan — the actual
   test. Success is a visible window on the default backend.
4. Deck, `AR_RENDER_OWNER=main` — should reproduce the black window, confirming
   we fixed the thing we think we fixed rather than something incidental.

Step 4 is the one that distinguishes a real fix from a coincidence, so don't
skip it.

## Open questions

- **Does SDL3 permit `SDL_CreateRenderer` off the main thread for a
  main-thread window on every backend we care about?** GL and Vulkan yes;
  Metal is the doubt, which is why owner stays Main on macOS. Worth confirming
  against SDL's own docs before step 4 rather than discovering it empirically.
- **Does the Wayland backend need more than context affinity?** libwayland's
  event queue isn't thread-safe for concurrent use; if window-state calls still
  originate on main while drawing happens on present, Wayland may need its own
  handling even after this lands. X11/GL should be fully fixed by context
  affinity alone — so if X11 comes up and Wayland doesn't, that is the tell.
- **90Hz jitter (item 6)** should be re-measured only after this lands; frame
  pacing measured against an undefined renderer state is noise.
