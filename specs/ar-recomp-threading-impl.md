# ActRaiser Recomp — Multithreaded Architecture & Diorama Mode Implementation Plan

**Date:** 2026-07-20
**Base commit:** `c07c377` (SDL3 migration applied on top)
**Author:** Architecture design doc for handoff implementation

---

## Implementation status (updated 2026-08-11)

Both tracks are implemented. This doc remains the design record; treat the
notes below as the authoritative "what actually shipped and why it differs,"
rather than re-deriving from git history.

**2026-08-11 capture-model correction:** layer capture follows the union of
the SNES main and subscreen source masks. Marahna proves that an action stage
can put BG1 and OBJ exclusively on TS and full-add them to main-screen BG2.
The shipped path now preserves supported colour math through explicit alpha,
5-bit pixel transforms, or resolved additive planes; the older main-only
sketches below have been replaced accordingly.

**Visual track (M0-M3): DONE**, unchanged from earlier status — Diorama
rendering lives in `src/diorama/diorama.c` and its companion headers/modules.

**Optimization track (M4-M8): DONE**, with real deviations from this doc's
literal sketches, each called out in-code where it matters:
- **M4** (`[present-perf]` baseline) — as specified.
- **M5** (present isolation) — the present-thread experiment implemented the
  Appendix-D contract, then Phase 0 removed the thread (#18/P13). Current
  upload/composite runs on the producing thread before the next scanout, so
  large pixel surfaces need neither copies nor double buffering. `FrameSlot`
  remains as the immutable scalar/derived-state boundary that keeps present
  code off live PPU/settings state; interpolation history is an explicit
  `DioramaScrollSnapshot`. See the ownership note in `src/present.h`.
- **M6** (fixed-timestep loop) — as specified; record/replay determinism
  reverified after the refactor (0 divergence).
- **M7** (scroll interpolation) — implemented, but **shipped disabled by
  default** (`gpu_interp_enabled` in the new Graphics settings, off).
  Real bug found live: ActRaiser's BG2 parallax layer in action stages
  appears HDMA-driven, so the end-of-frame scroll-register snapshot this
  captures is not a stable value — interpolating it visibly vibrates BG2
  even when genuinely static. Needs either HDMA-BG detection or a different
  scroll data source before defaulting on. Also: the interpolation formula
  used is forward-extrapolation from the latest frame, not the doc's literal
  `prev + t*(curr-prev)` lerp (which would show a constant one-tick display
  lag, since present always happens at/after the latest frame's timestamp).
- **M8** (GPU shader path) — `SDL_CreateGPURenderState` (SDL 3.4.0) has no
  working examples anywhere; the exact MSL binding convention (vertex color
  at `[[user(locn0)]]`, texcoord at `[[user(locn1)]]`, current draw texture
  auto-bound at `[[texture(0)]]`/`[[sampler(0)]]`) was reverse-engineered
  from SDL's own compiled test shaders
  (`test/testgpurender_effects_{grayscale,CRT}.frag.msl.h` in the SDL repo),
  not from documentation. Shipped effects (`src/diorama/diorama.c`): rim lighting
  and a combined depth-of-field + parallax-aware edge-AA shader (the two
  target the same BG1/BG2 layers, so they had to be merged into one shader —
  SDL allows only one custom fragment shader per draw call, and DOF silently
  never rendered until this was fixed). Soft shadow blur is implemented but
  **shipped disabled by default** — it can bleed onto transparent gaps in
  the layer behind it (e.g. a hazy patch over the sky), needing depth/
  stencil-aware "only shadow opaque receivers" compositing to fix properly.
  Parallax-aware AA (the fourth doc-suggested effect, a screen-space
  technique) was not attempted.

**New: Graphics settings category** (`kSettingCat_Graphics`, between
Presentation and Audio) exposes all of the above as menu rows instead of
env vars: `gpu_shaders_enabled` (restart-required backend switch),
`gpu_fx_rim`/`gpu_fx_dof`/`gpu_fx_edgeaa` (default on, live-toggle),
`gpu_fx_shadow`/`gpu_interp_enabled` (default off, tooltips name the known
bugs above). The `AR_GPU_*`/`AR_INTERP_*` env vars from mid-session
still work as legacy boot-time seeds (`SettingDesc.env`) but are no longer
the primary control surface.

---

## Scope: Diorama targets ACTION stages only

**The action Diorama renderer (Phases 3–6) applies to ActRaiser's side-scrolling
ACTION stages only. Non-action scenes now have separate presentation systems.**

- **Action stages** are Mode 1 side-scrollers (BG1 playfield, BG2 parallax, BG3
  HUD, plus sprites) with a clear front-to-back layer stack. This maps naturally
  onto a flat-plane diorama/shadowbox.
- **Town simulation views are Mode 1, not Mode 7.** They now use the independent
  Sim3D semantic-ground/billboard pipeline rather than this action shadowbox.
- **World navigation is Mode 7** and now has its own optional forced-top-down
  full-plane 3D presentation. Sky Palace, menus, and other non-action screens
  retain their own flat/overlay policies. Those later systems remain outside
  this historical action-Diorama design.

**Gating mechanism (grounded in existing code):** ActRaiser already distinguishes
these via the map-group WRAM byte. Use the established helper:

```c
// src/actraiser_game.h:196
static inline int ActRaiser_IsActionMapGroup(uint8 map_group) {
  return map_group >= kActRaiserActionMapGroup_First   // 0x01 Fillmore
      && map_group <= kActRaiserActionMapGroup_Last;    // 0x07 DeathHeim
}
// map group lives at g_ram[kActRaiserWram_MapGroup]  (== 0x0018)
```

This is the SAME predicate the widescreen policy uses to decide per-scene behavior
(`ActRaiser_ApplyWidescreenPolicy`, actraiser_rtl.c:695), and it parallels the
existing split of `ws_action` vs `ws_sim` settings (settings.h:223-224). The
action-Diorama capture policy and renderer must **early-out whenever
`!ActRaiser_IsActionMapGroup(...)`**. The present dispatcher may then select the
separate town/world-navigation enhancement or the normal flat path; action
Diorama must never claim those scenes.

Phases 1–2 (present thread, fixed-timestep) and the interpolation infrastructure
(Phase 5) are mode-agnostic and apply to the whole game.

---

## Table of Contents

1. [Architecture Overview — Current State](#1-architecture-overview)
2. [Phase 1: Present Thread Decoupling](#2-phase-1-present-thread)
3. [Phase 2: Fixed-Timestep Game Loop (Decoupled Rates)](#3-phase-2-fixed-timestep)
4. [Phase 3: Full-Frame Per-Layer Capture](#4-phase-3-layer-capture)
5. [Phase 4: Diorama Renderer](#5-phase-4-diorama-renderer)
6. [Phase 5: High-Framerate Interpolation](#6-phase-5-interpolation)
7. [Phase 6: GPU Shader Path (Optional)](#7-phase-6-gpu-shaders)
8. [Edge Cases & Hazards](#8-edge-cases)
9. [Testing Strategy](#9-testing)
10. [Settings Integration](#10-settings)

---

## 1. Architecture Overview — Current State <a name="1-architecture-overview"></a>

### 1.1 Thread Model (as of SDL3 migration)

| Thread | Role | Key mutex |
|--------|------|-----------|
| Main/Game thread | Event poll → `RtlRunFrame` → `RtlDrawPpuFrame` → `PresentFramebuffer` (SDL_RenderPresent blocks on vsync) | `g_audio_mutex` (game side of APU lock) |
| SDL Audio thread | `AudioCallback` → `RtlRenderAudio` → cycles SPC in 256-batch locked intervals | `g_audio_mutex` (audio side) |

### 1.2 Frame Pipeline (single-threaded, sequential)

```
PollEvents()
  → RtlRunFrame(inputs)              [game logic: 65816 recomp, ~2-8ms]
  → RtlDrawPpuFrame()                [PPU scanline render: ~1-3ms]
      └→ ActRaiserDrawPpuFrame()
          ├─ PpuClearOverlayCaptures
          ├─ (widescreen/HD policy setup)
          ├─ ppu_runLine(g_ppu, 0..224) + HDMA + IRQ
          └─ writes: g_pixels, g_hud_bg_pixels, g_hud_obj_pixels, g_m7_overlay_pixels
  → PresentFramebuffer()             [SDL upload + compose + present: blocks on vsync ~10-14ms]
      └→ RenderFramebuffer()
          ├─ SDL_UpdateTexture(g_texture, g_pixels)
          ├─ SDL_UpdateTexture(g_hud_bg_texture, g_hud_bg_pixels)  [if HUD split active]
          ├─ SDL_UpdateTexture(g_hud_obj_texture, g_hud_obj_pixels)
          ├─ SDL_RenderTexture(g_texture, src_f, NULL)             [base frame]
          ├─ RenderMode7Overlay(viewport)                          [Mode-7 HD sub]
          ├─ RenderHudOverlay(viewport)                            [widescreen HUD]
          ├─ RenderHdReplacements(viewport)                        [HD texture subs]
          ├─ RenderSceneInspector(viewport)
          ├─ SettingsOverlay_Render(viewport)
          └─ SDL_RenderPresent(g_renderer)                         [VSYNC BLOCK]
```

### 1.3 Key Data Structures

| Symbol | Location | Size | Description |
|--------|----------|------|-------------|
| `g_pixels` | `main.c:91` | 448×240×4 = 430 KB | Main PPU framebuffer (ARGB8888, alpha=0) |
| `g_hud_bg_pixels` | `main.c:92` | 448×240×4 | BG3 overlay capture for widescreen HUD |
| `g_hud_obj_pixels` | `main.c:93` | 448×240×4 | OBJ overlay capture for widescreen HUD |
| `g_hd_overlay_pixels[5]` | `main.c:98` | per-source, 448×240×4 | HD replacement overlay captures |
| `g_m7_overlay_pixels` | `main.c:104` | 448×224×4×16 (4x scale) | Mode-7 HD override surface |
| `g_ppu` | global, `common_cpu_infra.c:1055` | `sizeof(Ppu)` | PPU state: VRAM, OAM, CGRAM, regs |
| `g_texture` | `main.c` | SDL_Texture (STREAMING) | Base frame GPU texture |
| `g_hud_bg_texture` | `main.c` | SDL_Texture (STREAMING) | HUD BG3 GPU texture |
| `g_hud_obj_texture` | `main.c` | SDL_Texture (STREAMING) | HUD OBJ GPU texture |
| `g_m7_texture` | `main.c:105` | SDL_Texture (STREAMING) | Mode-7 GPU texture |

### 1.4 PPU Overlay Capture System (existing)

The PPU already supports per-layer rendering to separate RGBA surfaces:

```c
// ppu.h:54-59
enum {
    kPpuOverlaySource_Bg1 = 0,
    kPpuOverlaySource_Bg2 = 1,
    kPpuOverlaySource_Bg3 = 2,
    kPpuOverlaySource_Bg4 = 3,
    kPpuOverlaySource_Obj = 4,
    kPpuOverlaySource_Count = 5,
};
```

Each source can be bound to an RGBA buffer (`PpuBindOverlaySurface`) and configured with a capture rectangle (`PpuSetOverlayCapture`). The `kPpuOverlayFlag_RemoveFromGame` flag removes captured pixels from the main composite — giving clean layer separation.

The new PPU path (`g_new_ppu = true`) already routes each BG layer through `PpuBeginBackgroundOverlay`/`PpuFinishBackgroundOverlay` (ppu.c:1290-1320) which redirects layer writes to the overlay buffer when a capture is active. OBJ capture happens per-sprite-pixel inside `ppu_evaluateSprites` (ppu.c:1561-1589).

**Current limitation:** ActRaiser uses the OLD PPU path when widescreen is disabled. The actual switch logic is: `g_new_ppu = g_settings.new_renderer || g_ws_active` (main.c:973/1023 area). With widescreen enabled (which is the shipping config), the NEW path already runs. The old path (`ppu_old.c:69-206`) does NOT support overlay captures — it composites all layers inline per-pixel via `ppu_getPixel` with no intermediate buffers. The new path does.

**Implication for diorama:** If the game is in widescreen mode (the production configuration), the new PPU path is already active, so no PPU-path switch is needed to *enable* capture. Capture still requires the per-frame capture policy (§4.2, including the OBJ `PpuSetOverlayOamRange` call), dedicated diorama buffers (§4.3), and mutual exclusion with HD replacements / the widescreen HUD split — it is not zero-work, just no engine-path change.

---

## 2. Phase 1: Present Thread Decoupling <a name="2-phase-1-present-thread"></a>

**Goal:** Move all SDL_Render calls to a dedicated present thread so the game thread is never blocked by vsync.

### 2.1 New Globals

```c
// main.c — new threading state
static SDL_Thread *g_present_thread;
static SDL_Mutex *g_present_mutex;
static SDL_Condition *g_present_ready_cond;   // game → present: "new frame available"
static SDL_Condition *g_present_done_cond;    // present → game: "safe to write buffers"

// Double-buffered pixel state (the present thread reads from "back", game writes to "front")
typedef struct FrameSlot {
    uint8_t pixels[kPpuBufWidth * 4 * 240];
    uint8_t hud_bg_pixels[kPpuBufWidth * 4 * 240];
    uint8_t hud_obj_pixels[kPpuBufWidth * 4 * 240];
    // Implementation correction: large overlay pixels are not copied or
    // pointer-owned by FrameSlot. Dedicated surfaces are protected by the
    // upload/capture ordering contract (§4.3); the slot carries immutable
    // request/content/additive metadata for those surfaces.
    // Metadata the present thread needs to composite correctly:
    int snes_width;
    int snes_height;
    int display_mode;
    int pixel_aspect;          // snapshot of g_active_pixel_aspect
    bool ws_active;            // snapshot of g_ws_active
    int ws_extra;              // snapshot of g_ws_extra

    // --- Widescreen HUD split state (read by BuildHudPresentationChunks) ---
    // ALL of these are live g_ppu fields today; the present thread must NOT read
    // g_ppu directly (the game thread rewrites them next frame). See §2.8.
    int   hud_split_height;    // g_ppu->wsHudSplitHeight
    int   hud_left_end;        // g_ppu->wsHudLeftEnd
    int   hud_right_start;     // g_ppu->wsHudRightStart
    int   hud_player_row_y;    // g_ppu->wsHudPlayerRowY
    int   hud_left_only_y;     // g_ppu->wsHudLeftOnlyY
    int   ppu_extra_left_right;// g_ppu->extraLeftRight
    uint8_t ppu_inidisp;       // g_ppu->inidisp (forced-blank/brightness at present)
    uint8_t bg_mode;           // PPU_mode(g_ppu) == (g_ppu->bgmode & 7). Used by
                               // the interpolation mode-change guard (§6.4). NOTE:
                               // diorama is action-only (Mode 1), so there is no
                               // runtime Mode-1/Mode-7 Z branch (§5.2) — this field
                               // exists for the §6.4 guard, not Z selection. It is
                               // the SNES PPU mode, NOT the display_mode aspect.

    // Per-source capture rects (RenderHudOverlay OBJ icon, RenderHdReplacements)
    PpuOverlayCapture overlay_captures[kPpuOverlaySource_Count];

    // OBJ HUD-icon promotion validates a pattern anywhere in OAM on the game
    // thread, then latches the range independently of overlayCaptures[OBJ]
    // (the diorama's later full-scene claim can replace that capture range).
    // Present resolves the icon position from this per-frame range and the OAM
    // snapshot rather than reading g_ppu->oam / g_ppu->highOam live. See §2.8.
    uint8_t hud_icon_first, hud_icon_count;
    uint16_t oam_snapshot[128 * 2];   // if any OBJ overlay/icon is active
    uint8_t  high_oam_snapshot[32];

    // Mode-7 override region (for RenderMode7Overlay positioning)
    SDL_Rect m7_src_rect;
    bool m7_active;
    // HD replacement state snapshot
    int hd_replacement_count;
    // (simplified: copy the active subset of HdReplacement rects+flags)
    struct { SDL_Rect src; SDL_FRect dst; int source; bool active; uint8_t alpha; } hd_rects[32];
} FrameSlot;

static FrameSlot g_frame_slots[2];
static int g_frame_write_idx;   // game thread writes here (0 or 1)
static bool g_frame_pending;    // true = present thread has work
static bool g_present_running;  // false = shutdown signal
```

### 2.2 Present Thread Function

```c
static int SDLCALL PresentThreadFn(void *userdata) {
    (void)userdata;
    SDL_LockMutex(g_present_mutex);
    while (g_present_running) {
        while (!g_frame_pending && g_present_running)
            SDL_WaitCondition(g_present_ready_cond, g_present_mutex);
        if (!g_present_running) break;

        int read_idx = 1 - g_frame_write_idx;  // read the slot game just finished
        g_frame_pending = false;
        SDL_UnlockMutex(g_present_mutex);

        // --- All SDL_Render calls happen here ---
        FrameSlot *slot = &g_frame_slots[read_idx];
        PresentFrameSlot(slot);  // uploads textures, composites, calls SDL_RenderPresent

        SDL_LockMutex(g_present_mutex);
        SDL_SignalCondition(g_present_done_cond);  // tell game thread buffer is consumed
    }
    SDL_UnlockMutex(g_present_mutex);
    return 0;
}
```

### 2.3 Game Thread Changes

Replace the current `PresentFramebuffer()` call at `main.c:2666` with:

```c
// After RtlDrawPpuFrame() fills g_pixels etc:
SDL_LockMutex(g_present_mutex);
// Wait until present thread consumed the PREVIOUS frame (avoid overwriting)
while (g_frame_pending)
    SDL_WaitCondition(g_present_done_cond, g_present_mutex);

// Copy frame data into the write slot
FrameSlot *slot = &g_frame_slots[g_frame_write_idx];
memcpy(slot->pixels, g_pixels, sizeof(g_pixels));
memcpy(slot->hud_bg_pixels, g_hud_bg_pixels, sizeof(g_hud_bg_pixels));
memcpy(slot->hud_obj_pixels, g_hud_obj_pixels, sizeof(g_hud_obj_pixels));
// (overlay/m7 layer buffers copied only when their feature is active)

// --- Snapshot ALL present-time-read state (see §2.8) ---
slot->snes_width  = g_snes_width;
slot->snes_height = g_snes_height;
slot->display_mode = g_settings.display_mode;   // read by §5.9 UV-crop
slot->pixel_aspect = g_active_pixel_aspect;
slot->ws_active   = g_ws_active;
slot->ws_extra    = g_ws_extra;
if (g_ppu) {
    slot->hud_split_height     = g_ppu->wsHudSplitHeight;
    slot->hud_left_end         = g_ppu->wsHudLeftEnd;
    slot->hud_right_start      = g_ppu->wsHudRightStart;
    slot->hud_player_row_y     = g_ppu->wsHudPlayerRowY;
    slot->hud_left_only_y      = g_ppu->wsHudLeftOnlyY;
    slot->ppu_extra_left_right = g_ppu->extraLeftRight;
    slot->ppu_inidisp          = g_ppu->inidisp;
    slot->bg_mode              = PPU_mode(g_ppu);   // (g_ppu->bgmode & 7)
    memcpy(slot->overlay_captures, g_ppu->overlayCaptures,
           sizeof(slot->overlay_captures));
    ActRaiser_HudObjIconRange(&slot->hud_icon_first,
                              &slot->hud_icon_count);
    // OAM only needed when an OBJ overlay/HUD icon is active this frame.
    // static_assert guards against a future Ppu struct change silently
    // under-copying (see §D18).
    _Static_assert(sizeof(slot->oam_snapshot) == sizeof(g_ppu->oam), "oam size");
    _Static_assert(sizeof(slot->high_oam_snapshot) == sizeof(g_ppu->highOam), "highOam size");
    if (g_ppu->overlayCaptures[kPpuOverlaySource_Obj].oamCount ||
        slot->hud_icon_count) {
        memcpy(slot->oam_snapshot, g_ppu->oam, sizeof(slot->oam_snapshot));
        memcpy(slot->high_oam_snapshot, g_ppu->highOam,
               sizeof(slot->high_oam_snapshot));
    }

    // Mode-7 overlay presentation state (read by RenderMode7Overlay).
    slot->m7_active = (g_ppu->m7Override.rgba != NULL);
    if (slot->m7_active) {
        // Same visible-columns sub-rect RenderMode7Overlay uploads (main.c:1703-1705).
        slot->m7_src_rect = (SDL_Rect){ Settings_VisibleX0() * kHdMode7Scale, 0,
                                        Settings_VisibleWidth() * kHdMode7Scale,
                                        g_snes_height * kHdMode7Scale };
    }
}

// HD replacement rects (read by RenderHdReplacements). Snapshot the active subset;
// mirrors the entry->active/.texture/.source/.brightness gate at main.c:1729-1750.
slot->hd_replacement_count = 0;
for (int i = 0; i < g_hd_replacement_count && slot->hd_replacement_count < 32; i++) {
    const HdReplacement *e = &g_hd_replacements[i];
    const PpuOverlayCapture *cap = g_ppu ? &g_ppu->overlayCaptures[e->source] : NULL;
    if (!e->active || !e->texture || !cap ||
        cap->x1 <= cap->x0 || cap->y1 <= cap->y0 ||
        !(cap->flags & kPpuOverlayFlag_RemoveFromGame))
        continue;
    int k = slot->hd_replacement_count++;
    slot->hd_rects[k].source = e->source;
    slot->hd_rects[k].active = true;
    // dst rect + alpha resolved exactly as RenderHdReplacements does (main.c:1737-1750),
    // using the snapshotted viewport/inidisp so no live read is needed at present time.
    slot->hd_rects[k].src = (SDL_Rect){ cap->x0, cap->y0, cap->x1 - cap->x0, cap->y1 - cap->y0 };
    slot->hd_rects[k].dst = /* computed from GetPresentationViewport() result, snapshotted */ (SDL_FRect){0};
    slot->hd_rects[k].alpha = (slot->ppu_inidisp & 0x80) ? 0
                             : (Uint8)((slot->ppu_inidisp & 0xf) * 255 / 15);
}

g_frame_write_idx = 1 - g_frame_write_idx;  // swap
g_frame_pending = true;
SDL_SignalCondition(g_present_ready_cond);
SDL_UnlockMutex(g_present_mutex);
```

**Copy-cost caveat (flagged for the implementer):** this critical section memcpies
~1.3 MB of pixel buffers (three 448×240×4 buffers) plus overlays every frame, on
the game thread, inside the present mutex. At 60 Hz that's ~78 MB/s — negligible
CPU, but it *does* serialize the game thread against the present thread for the
duration of the copy. Two options if profiling shows it matters:

- **Zero-copy pointer swap (preferred):** instead of one shared `g_pixels` that the
  game renders into and then copies, give the PPU **two** render-target buffer sets
  and swap which one it draws into each frame (`PpuBeginDrawing` already takes the
  target pointer, main.c:1669). The present thread reads the just-finished buffer by
  pointer; the game thread renders the next frame into the other. The only thing
  under the mutex is a pointer swap + the small metadata snapshot — no bulk memcpy.
  This requires `RebindPpuOutputSurfaces` (main.c:1666) to rebind all overlay
  surfaces to the new target set on each swap, and the HD/m7 buffers to be doubled
  too. More bookkeeping, but eliminates the copy entirely.
- **Keep the memcpy** if the ~0.15 ms/frame copy is acceptable (it almost certainly
  is for a 60 Hz game with frames budgeted at 16.6 ms). Simpler, fewer rebind bugs.

Recommend shipping the memcpy version first (simpler, correct), then switching to
pointer-swap only if the game thread's copy stall shows up in `AR_PERF` timings.

### 2.4 SDL_Renderer Thread Ownership

**Critical constraint:** SDL3's renderer is NOT thread-safe. ALL `SDL_Render*` calls must happen on the present thread once we move presentation there. This means:

- `SDL_CreateRenderer` can stay on the main thread (creation is a one-time setup before the present thread starts).
- `SDL_CreateTexture` for the streaming textures should happen on the present thread, OR the textures are created on the main thread before spawning the present thread. SDL3 allows texture creation from any thread *if* no concurrent render operations are happening. Pre-creation before thread start is simplest.
- The settings overlay (`SettingsOverlay_Render`) currently calls `SDL_RenderTexture`, `SDL_RenderFillRect`, etc. These MUST move to the present thread. Solution: the present thread calls `SettingsOverlay_Render(viewport)` as part of its composite pass. **Caveat — the overlay state is NOT cleanly one-way:** navigation statics (open panel, cursor, edit buffer) are written by game-thread event handlers while the present thread reads them, and the debug-panel rects are WRITTEN by the present-thread render then read by game-thread mouse handlers. That is a bidirectional race (low severity — paused-only, cosmetic — but real). Snapshot the navigation state into the slot and compute drag/resize rects on the game thread rather than in the render pass. See the Appendix C row and §8.5.
- `SDL_RenderReadPixels` (screenshot path at `main.c:564-578`) must also run on the present thread. Signal it via a flag; the present thread captures after `SDL_RenderPresent` and writes the file (or signals the game thread with the result).
- **Signature refactor (required, see §2.8):** `RenderFramebuffer`, `BuildHudPresentationChunks`, `RenderHudOverlay`, `RenderMode7Overlay`, `RenderHdReplacements`, and `RenderSceneInspector` currently read live `g_ppu->*` / `g_settings.*` / `g_snes_width`. They must be changed to take a `const FrameSlot *slot` and read the snapshotted copies. This is the bulk of the Phase 1 refactor work — not just moving the calls, but severing their reads from live global state. Grep for `g_ppu->` within main.c lines 1100-1800 to find every read site (enumerated in §2.8).

### 2.5 Paused-Frame Handling

When paused (`main.c:2339-2343`), the game thread currently calls `PresentFramebuffer()` every 16ms with the stale frame. With the present thread, instead:

- The present thread re-presents the last `FrameSlot` on its own vsync cadence.
- The game thread does NOT signal a new frame when paused — it only signals when `g_paused_redraw_pending` triggers a `RedrawPausedFrameIfNeeded()` (settings change while paused).
- The present thread's loop adds: if no new frame arrives within 16ms, re-present the last slot (keeps the window alive / avoids OS "not responding").

### 2.6 Shutdown Sequence

```c
// In main(), after `running = false`:
SDL_LockMutex(g_present_mutex);
g_present_running = false;
SDL_SignalCondition(g_present_ready_cond);  // wake present thread
SDL_UnlockMutex(g_present_mutex);
SDL_WaitThread(g_present_thread, NULL);
// Now safe to destroy renderer, window, etc.
```

### 2.7 Edge Cases

| Case | Handling |
|------|----------|
| Present thread slower than game thread (frame skip) | The game thread waits on `g_present_done_cond` before writing a new frame. This naturally throttles the game to the present rate. In Phase 2 (fixed timestep), this changes — the game runs ahead and the present thread picks up the latest. |
| Window resize during present | SDL3 delivers resize events on the game/event thread. A pure size change that only makes the present thread read a new `SDL_GetRenderOutputSize` is benign. BUT a resize that triggers `ResolveVideoGeometry`/`RebindPpuOutputSurfaces`/logical-presentation changes mutates renderer + PPU state on the game thread — that is the §2.9(a) hazard and MUST go through the command queue / quiesce path, not be assumed safe. See §2.9(a). |
| Settings overlay text input | `SDL_StartTextInput`/`SDL_StopTextInput` must be called on the thread that created the window (main thread). This is event handling, not rendering — stays on the game thread. |
| Headless mode (`headless = true`) | Skip present thread entirely. The game thread still fills `g_pixels` (for PPM captures) but never signals the present thread. The present thread is simply not spawned. |
| Turbo mode | Currently runs extra `RtlRunFrame` calls per host frame (main.c:2498-2500). With the present thread, turbo runs unlimited `RtlRunFrame` calls while the present thread independently picks up the latest complete frame. The present thread always shows the most recent — no need to render every turbo frame. |

### 2.8 Widescreen compatibility — the partial-bake hazard (READ THIS)

**The intuition "widescreen is fully baked into the layer textures before handoff"
is only HALF true.** Widescreen has two distinct halves, and only one is baked:

**Baked into pixels during `RtlRunFrame`/`RtlDrawPpuFrame` (game thread) — SAFE:**
- Margin content generation (`ActRaiser_WidescreenMarginRefresh`, Sky Palace BG2
  repair, sprite widening, `PpuSetExtraSpaceCentered`). These write into `g_pixels`
  and the HUD/overlay buffers, which the FrameSlot memcpy captures. No race.
- The extra-columns rendering (the internal 256→448 wide framebuffer). Baked.

BH8 note (2026-08-09): `ActRaiser_WidescreenMarginRefresh` was retired after
the bounded provider's default-on acceptance. Provider scanout is still baked
before `FrameSlot`; the Sky Palace and sprite statements remain current.

**Computed at PRESENT time, reading LIVE `g_ppu` fields — NOT baked, RACE RISK:**
The widescreen HUD split is re-projected every present from live PPU state.
`BuildHudPresentationChunks` (main.c:1146-1263) reads, at present time:
`g_ppu->wsHudSplitHeight`, `wsHudLeftEnd`, `wsHudRightStart`, `wsHudPlayerRowY`,
`wsHudLeftOnlyY`, `extraLeftRight`, `overlayCaptures[OBJ]`, `oam[]`, `highOam[]`,
`inidisp`. `RenderHdReplacements` reads `overlayCaptures[*]` and `inidisp`;
`RenderMode7Overlay` reads `m7Override`.

These fields are **written by the game thread every frame** (`PpuSetWidescreenHudSplit`,
actraiser_rtl.c:887/901; the OBJ capture policy; INIDISP register writes). If the
present thread reads them live while the game thread is one frame ahead writing
them, you get a torn HUD split (e.g. a stale `wsHudSplitHeight` with a fresh
`wsHudLeftEnd`) → the status bar bands mis-slice for one frame. Rare but real, and
exactly the class of bug that is miserable to reproduce.

**Resolution:** the present thread must read these from the `FrameSlot` snapshot,
NEVER from `g_ppu` directly. The FrameSlot (§2.1) now carries all of them. Concretely:
1. `BuildHudPresentationChunks`, `RenderHudOverlay`, `RenderHdReplacements`,
   `RenderMode7Overlay`, and `RenderSceneInspector` must be refactored to take a
   `const FrameSlot *` and read `slot->hud_*` / `slot->overlay_captures[]` /
   `slot->ppu_inidisp` instead of `g_ppu->*`.
2. The game thread populates these into the write slot in the same critical
   section that memcpies the pixel buffers (§2.3), immediately after
   `RtlDrawPpuFrame` returns (so the values match the pixels just rendered).
3. `g_snes_width`, `g_snes_height`, `g_ws_active`, `g_ws_extra`,
   `g_active_pixel_aspect`, `g_settings.hud_scale_percent`,
   `g_settings.scene_inspector` are also read at present time — snapshot them too
   (they change rarely, but a mid-present change would still tear).

**OAM snapshot cost note:** the OBJ HUD-icon path indexes a validated, per-frame
`hud_icon_first`/`hud_icon_count` range in `oam[]`/`highOam[]`. Snapshotting the
full 544-byte OAM per frame is cheap; do that rather than assuming a stable slot
(the simulation hourglass moves from slots 0-3 to 11-14 while its menu is open).
Do not reconstruct this range from `overlayCaptures[OBJ]`: the diorama's later
full-scene OBJ claim legitimately replaces it with 0-127. Skip the OAM copy only
when neither an OBJ overlay nor a promoted HUD icon is active.

**Widescreen + Diorama interaction:** default flat-HUD Diorama deliberately
reuses `BuildHudPresentationChunks` after the projected world, while tilted-HUD
mode binds BG3 as a Diorama plane. The producer explicitly rebinds BG3 for both
branches every frame and the present choice comes from `FrameSlot`, not live
settings. This is a combined-mode contract, not mutual exclusion.

### 2.9 Game-thread code that touches the renderer or PPU — the OTHER single-thread violations

§2.4 says all `SDL_Render*` calls move to the present thread, but several game-thread
paths ALSO mutate renderer/window state or rewrite `g_ppu` wholesale. Each is a
data race against the present thread once presentation is threaded. **The
present-thread refactor is not "move the render calls"; it is "establish that the
renderer and g_ppu have a single owner at any instant." Every item below must be
handled or the plan's central invariant is violated by ordinary user actions.**

**(a) Settings-change handler mutates the renderer on the game thread.**
`OnRuntimeSettingChanged` (main.c:1001) runs synchronously on the game thread (via
the settings observer during event handling and `ApplyScheduledSettingChange`) and
calls: `SDL_SetWindowFullscreen` (main.c:1016); `ApplyDisplayPresentation` →
`SDL_SetRenderLogicalPresentation` + `SDL_SetWindowSize` (main.c:945-951);
`ResolveVideoGeometry(true)` → `RebindPpuOutputSurfaces` (PpuBeginDrawing rebinds
the `g_pixels` target, main.c:1669) + `memset(g_pixels)` (main.c:981-983) and
mutates `g_snes_width`/`g_ws_extra`/`display_mode` (main.c:971-972, 979). All of
this races the present thread's `SDL_Render*` — and it fires on ordinary display /
widescreen / aspect / fullscreen changes, most of which happen while paused (when
§2.5 keeps the present thread re-presenting).
**Fix:** route every renderer-state / window-geometry / PPU-surface-rebind
mutation onto the present thread via a command queue drained inside
`PresentThreadFn`, OR fully quiesce (drain + block) the present thread before
applying a geometry change. Do NOT call these from the game thread while the
present thread is live.

**(b) Savestate load (F7) rewrites all of g_ppu on the game thread.**
`RtlLoadSnapshot` → `snes_saveload` (common_rtl.c:198) deserializes VRAM/CGRAM/OAM/
registers into `g_ppu` guarded only by `RtlApuLock` (the audio mutex), dispatched
from the F7 key handler (main.c:2240-2241) on the game thread. If the present thread
reads `g_ppu` concurrently (it does — see (d)), an F7 mid-present tears the frame.
(No pointer field is in the savestate, so this is a torn-scalar/garbled-frame race,
not a crash — but still UB.) Boot-time `AR_LOADSTATE` (main.c:2172) is pre-loop and
safe.
**Fix:** gate `RtlLoadSnapshot` (and any interactive load) behind the present
handshake — take the present mutex or quiesce the present thread so `g_ppu` is
never rewritten while the present thread reads it.

**(c) Screenshot/PPM capture runs a full render pass on the game thread.**
`WriteFramebufferPpm` (main.c:584) calls the whole `RenderFramebuffer()` (main.c:589)
— `SDL_UpdateTexture`/`RenderClear`/`RenderTexture`/`RenderReadPixels` — from
`TakeFullSnapshot` (F2, main.c:2279) and the `AR_SHOT_*` paths (main.c:2656), during
live play. §2.4/§8.6 only mention moving `SDL_RenderReadPixels`; they miss that the
entire render pass issues from the game thread.
**Fix:** signal a capture request and let the present thread do the render +
readpixels + file write (or hand back the surface). Never call `RenderFramebuffer()`
from the game thread once presentation is threaded.

**(d) The present-time render functions themselves read live g_ppu (root cause).**
This is the umbrella under §2.8 and findings (a)-(c): `RenderFramebuffer`,
`RenderMode7Overlay` (reads `g_ppu->m7Override.rgba`, main.c:1699, and the shared
`g_m7_overlay_pixels` buffer), `RenderHdReplacements` (reads `g_ppu->inidisp` and
`overlayCaptures[]`, main.c:1718/1733/1747), `RenderHudOverlay`/
`BuildHudPresentationChunks`, and `RenderSceneInspector` all dereference live
`g_ppu`/`g_settings`/`g_snes_width` — NOT the FrameSlot. If `PresentFrameSlot`
simply calls the existing `RenderFramebuffer()`, the snapshot fields are dead and
every read races the next `RtlDrawPpuFrame`.
**Fix (mandatory, gates Phase 1):** pass `const FrameSlot *slot` through every
`Render*` helper and forbid `g_ppu`/`g_settings`/`g_snes_width` dereferences on the
present thread. **The enforcement is physical TU isolation, not a macro — see §D6:**
move the present functions into `src/present.c` which does not declare those
globals, so any stray live read is an undeclared-symbol compile error (a `#define
g_ppu <poison>` inside a shared `main.c` cannot work — the game thread there reads
`g_ppu` legitimately). Also snapshot *derived* values (`Settings_VisibleX0/Width`
results) since a global-name fence can't catch helper-laundered reads (§D3).

**(e) The `g_m7_overlay_pixels` buffer is ~6.4 MB and is NOT double-buffered.**
`FrameSlot` declares `m7_overlay_pixels` (and `hd_overlay_pixels[]`) as bare
pointers, so they alias the single live global that the game thread rewrites
per-scanline. Copying the m7 buffer is expensive (kPpuBufWidth·4·4 × 224·4 ≈
6.4 MB/frame), so the memcpy approach is a poor fit here.
**Fix:** double-buffer the m7 and HD overlay surfaces by pointer swap — bind an
alternate buffer set via `PpuBindMode7OverlaySurface`/`PpuBindOverlaySurface` each
frame and hand the present thread the just-finished pointer. This is the same
zero-copy approach recommended for the base framebuffer in §2.3 and it closes the
m7/HD races in one move.

**Net:** the clean way to satisfy all of (a)-(e) is a single-frame-owner design:
(1) PPU output buffer sets swapped under the present mutex; (2) `Render*` reads
only the slot; (3) renderer/window/savestate mutations serialized against present.

> **AUTHORITATIVE DECISIONS (read Appendix D — these SUPERSEDE the sketch above):**
> the enforcement is NOT the `#define g_ppu <poison>` in a shared TU (it can't work
> there) — it is physically moving present code to `src/present.c` (§D6). The
> serialization mechanism is NOT a command queue — it is **quiesce** (§D8). The
> single snapshot writer is `FrameSlot_Capture` (§D5). Buffer ownership is one
> `read_idx` advanced in one place (§D7). Implement per Appendix D; treat §2.9(a)-(e)
> as the rationale, Appendix D as the spec.

---

## 3. Phase 2: Fixed-Timestep Game Loop <a name="3-phase-2-fixed-timestep"></a>

**Goal:** Game logic runs at exactly 60.098 Hz (NTSC) by wall clock, independent of display refresh rate.

### 3.1 Replace the Frame-Locked Loop

Current loop (`main.c:2183`):
```c
while (running) {
    PollEvents();
    // ... (paused check, input) ...
    RtlRunFrame(inputs);
    RtlDrawPpuFrame();
    PresentFramebuffer();  // blocks on vsync = rate limiter
}
```

New loop:
```c
static const uint64_t kFrameNs = 16639267;  // 1e9 / 60.098 (NTSC rate)
static const int kMaxCatchupFrames = 3;      // spiral-of-death cap

uint64_t accumulator = 0;
uint64_t last_time = SDL_GetTicksNS();

while (running) {
    uint64_t now = SDL_GetTicksNS();
    uint64_t dt = now - last_time;
    last_time = now;
    accumulator += dt;

    // Cap accumulator to prevent death spiral after long stalls (window drag, sleep)
    if (accumulator > kFrameNs * kMaxCatchupFrames)
        accumulator = kFrameNs * kMaxCatchupFrames;

    PollEvents();  // always poll at the outer rate for input responsiveness

    if (g_paused || SettingsOverlay_IsOpen()) {
        accumulator = 0;  // don't accumulate while paused
        RedrawPausedFrameIfNeeded();
        SDL_Delay(1);     // yield CPU; present thread re-presents stale frame
        continue;
    }

    bool produced_frame = false;
    while (accumulator >= kFrameNs) {
        uint32 inputs = SampleInputs();  // moved to function
        RtlRunFrame(inputs);
        accumulator -= kFrameNs;
        produced_frame = true;
    }

    if (produced_frame) {
        RtlDrawPpuFrame();
        SubmitFrameToPresent();   // Phase 1's signal to present thread
    } else {
        SDL_Delay(1);  // yield until next tick
    }
}
```

### 3.2 Turbo Mode Adaptation

In turbo mode, remove the accumulator cap and run unlimited frames per outer iteration:
```c
if (g_turbo) {
    int mult = g_settings.turbo_multiplier;
    for (int tf = 1; tf < mult; tf++) {
        RtlRunFrame(inputs);
    }
}
```

This remains inside the `while (accumulator >= kFrameNs)` body. The present thread picks up the latest frame; intermediate turbo frames are never presented (no wasted GPU work).

### 3.3 NTSC Rate Constant

The SNES master clock is 21.477272 MHz, with 262 scanlines × 1364 dots = 357368 master clocks per frame, giving **60.0988 fps**. Use:
```c
static const uint64_t kFrameNs = 16639267;  // floor(1e9 / 60.0988)
```

Not 16666667 (60.00 Hz) — the slight difference causes perceptible audio drift over minutes if the emulator rate doesn't match the real NTSC cadence.

### 3.4 Edge Cases

| Case | Handling |
|------|----------|
| Monitor at exactly 60Hz | The game produces one frame per outer iteration; the present thread shows each one once. Indistinguishable from the current behavior. |
| Monitor at 120Hz | The game still ticks once per 16.6ms. The present thread presents the same frame twice (or interpolated — Phase 5). The outer loop's `SDL_Delay(1)` keeps it from busy-spinning. |
| `SDL_GetTicksNS` precision | SDL3 uses `mach_absolute_time` on macOS (nanosecond precision). Sufficient. On Windows, `QueryPerformanceCounter` backs it. |
| First frame after unpause | `last_time = SDL_GetTicksNS()` immediately before the first tick after unpause (reset `accumulator = 0` when entering pause). Otherwise the accumulated pause duration fires dozens of catch-up frames. |
| AR_PACE=1 (headless throttle) | The existing `SDL_Delay(16)` pacing for headless (main.c:2674-2681) stays as-is for headless mode. The fixed-timestep loop only engages when `!headless`. |

### 3.5 Per-frame housekeeping the skeleton MUST NOT drop

The §3.1 skeleton shows only PollEvents + RtlRunFrame + RtlDrawPpuFrame +
SubmitFrame, but the current loop body (main.c:2336-2664) does a lot of mandatory
per-frame work between `RtlRunFrame` and present. Porting the loop means placing
each of these explicitly, and DECIDING per item whether it runs once per game tick
(inside the `while (accumulator >= kFrameNs)` loop) or once per outer host
iteration. Getting the multiplicity wrong breaks save persistence, audio, and the
oracle harness.

| Current call | Line | Correct placement in new loop | Why |
|---|---|---|---|
| Input record/replay (`$0088`-keyed) | 2384-2441 | **Per game tick** (inside accumulator loop, with `SampleInputs`) | One 8-byte record per emulated frame; replay overrides `inputs` per game-frame. Running once per outer iter drops frames and breaks frame-exact oracle diffing (see §3.7). |
| `ApplyScheduledSettingChange` | 2442 | Per outer iteration | Applies a queued setting once; not frame-coupled. |
| `RtlRunFrame` turbo multiply | 2498-2500 | Per game tick | Already inside the tick (§3.2). |
| `ar_uploader_complete_tick` | 2562 | Per outer iteration | SPC upload completion poll; once per host iter is fine. |
| `MusicReplacements_FrameTick` | 2566 | Per outer iteration | Music-replacement live policy poll. |
| `MusicReplacements_SetHostPaused` | 2336 | Per outer iteration, ALWAYS (even paused) | The §3.1 pause branch must still call this — dropping it breaks host-pause of the HD music decoder. |
| `SaveSystem_AutoPersistIfChanged` | 2598 | Per outer iteration | Battery-save persistence; once per iter. Skipped under `AR_INPUT_REPLAY`. |
| `AR_WARP_AT` | 2572 | Per outer iteration | One-shot warp trigger. |
| PPM screenshot capture | 2636-2664 | Per outer iteration (see §2.4 — must move to present thread) | Reads the rendered frame. |
| `AR_PERF` / `SNESRECOMP_APU_PROFILE` instrumentation | 2451-2535 | Wrap the per-tick RtlRunFrame | These measure per-frame time. |
| Diorama camera settings flush (new) | — | Per outer iteration | If `g_diorama_settings_dirty` and ~0.5s since the last camera adjust (or on menu-close/mode-toggle/quit): `Settings_Save`, clear the flag. Debounces the per-motion camera writes (§5.6). |

**Rule of thumb:** anything that reads/writes emulator state per emulated frame
(input, record/replay, per-frame instrumentation) goes inside the accumulator
loop; anything that is a host-side poll or one-shot (music tick, autosave, setting
apply, warp, screenshot) goes once per outer iteration.

### 3.6 Headless keeps its own loop (no fixed-timestep, no present thread)

The fixed-timestep loop engages ONLY when `!headless`. Headless (main.c:2668-2692)
must stay uncapped by default (the oracle/replay tooling depends on running as fast
as the CPU allows); `AR_PACE` opt-in throttling and `AR_QUIT_FRAMES` stop stay as
today, and no present thread is spawned (§2.7). Do not route headless through the
§3.1 accumulator — it would pace headless to 60 Hz and call `SubmitFrameToPresent`
with no present thread.

### 3.7 Record/replay determinism under catch-up

The differential-oracle record/replay (main.c:2384-2441) writes one record per host
frame and replays by overriding `inputs` per game-frame `$0088`. Because the new
loop runs a variable number of `RtlRunFrame` per outer iteration, record/replay
MUST move inside the per-tick loop (one record per emulated frame, replay applied
for each about-to-execute game frame). Left once-per-outer-iteration it silently
misaligns and breaks frame-exact diffing. (Note: `tools/oracle/snesref.cpp` is a
standalone SDL2 libretro frontend sharing no runtime code — it is unaffected and
needs no change. The real oracle hazard is this in-loop hook, not snesref.)

---

## 4. Phase 3: Full-Frame Per-Layer Capture <a name="4-phase-3-layer-capture"></a>

**Goal:** In ACTION STAGES ONLY (see Scope banner), render each Mode-1 PPU layer
(BG1, BG2, BG3, OBJ) to its own full-frame RGBA surface so the diorama renderer
can project them as independent planes. Sim/Mode-7 scenes are excluded and use the
normal flat present path.

### 4.0 Do the widescreen margin hacks survive per-layer capture? (YES — verified)

**This was an open design question: do the widescreen margin-expansion hacks write
into separate layers (so the diorama can render them in 3D), or do they paint into
the composited framebuffer after everything is stacked (which would lose them)?**

**Answer: they inject data BEFORE the scanline renderer, so they are captured
per-layer and ARE available to the diorama.** Verified against the code:

- **BG margins** (`actraiser_widescreen_bg.c`): the margin refresh writes only
  **BG tilemap VRAM cells** (file header, lines 13-16: "It does not touch PPU/OAM/
  CGRAM registers… The only persistent writes are the explicitly range-checked BG
  tilemap VRAM words"). Sky Palace decodes a ROM metatile page into offscreen BG2
  columns. Both populate VRAM *before* `ppu_runLine`, so the PPU draws the margin
  tiles as part of BG1/BG2 — and the BG1/BG2 overlay capture picks them up.
- **BH8 update:** action world margins now enter the same captured BG planes
  through the virtual tile-word provider during scanout. Only the separate Sky
  Palace temporary VRAM patch remains in `actraiser_widescreen_bg.c`.
- **Sprite margins** (`actraiser_widescreen_sprites.c`): the hacks widen
  per-definition emission and margin-object drawing into **OAM** ("keeps the wide
  viewport inside the 512px world **before OAM is composed**", lines 13-14). They
  feed the PPU's sprite evaluator (`ppu_evaluateSprites`), so widened sprites land
  in the OBJ capture.
- **The core widening** (`PpuSetExtraSpace`/`PpuSetExtraSpaceCentered`, ppu.c:198-227)
  is a **per-scanline PPU render concept**: `extraLeftCur`/`extraRightCur`/
  `extraLeftRight` are consumed inside the scanline renderer's window/margin math
  (ppu.c:404-547), producing a 256→448-wide render. Every layer is drawn at the
  widened width by the same renderer that feeds the overlay capture.
- **Nothing paints the composited framebuffer post-stack:** the only writes to
  `renderBuffer`/`g_pixels` are the PPU's own per-pixel composite (ppu.c:1381/1413,
  ppu_old.c:132). There is no "draw margins onto the finished frame" step. Confirmed
  by grep: no `g_pixels[...] =` writes exist outside the PPU renderer.

**Crucially, the overlay capture spans the full widened width, including the
margins.** `PpuSetOverlayCapture` accepts x from `-kPpuExtraLeftRight` (−96) to
`256+96` (ppu.c:145-167), and `PpuWriteOverlayRenderLine` writes across the whole
widened surface using a `texture_extra` offset so margin columns land in the
capture buffer (ppu.c:1270-1287). So when §4.2 captures each layer with
`PpuSetOverlayCapture(source, -g_ws_extra, 0, width, 224, …)`, the widescreen
margin content is included in each layer's plane — the diorama renders the widened
BG1/BG2/OBJ, margins and all, not just the authentic 256-wide center.

**Implication:** no changes to the widescreen world generation are needed for
diorama compatibility. It already produces per-layer, margin-inclusive content
ahead of capture. BG3 is the deliberate exception: default flat-HUD mode keeps
the widescreen HUD capture and presents its split/anchored bands after the
Diorama world, while tilted-HUD mode explicitly rebinds BG3 to its Diorama
plane. Both branches rebind every frame because omitting a source does not undo
the preceding frame's surface binding.

### 4.1 Prerequisite: Switch ActRaiser to New PPU Path

The old PPU path (`ppu_old.c`) does NOT support overlay captures — it composites per-pixel inline. The new path (`ppu.c:PpuDrawWholeLine`) already routes each BG through the overlay capture machinery.

**Change (NOT just editing line 121):** `g_new_ppu` is already initialized `true` at `main.c:121`, but that initializer is overwritten at runtime by `g_new_ppu = g_settings.new_renderer || g_ws_active` (main.c:973, recomputed at 1023 on `new_renderer` toggle). So editing line 121 alone is a no-op. To guarantee the new path whenever diorama is active, force it at the recompute sites: `g_new_ppu = g_settings.new_renderer || g_ws_active || g_settings.diorama_mode`. In the shipping widescreen config `g_ws_active` is already true, so this only matters for a 4:3 + `new_renderer`-off configuration (see the availability gate in §10.6).

**Risk assessment:** The new PPU path is more accurate and faster (vectorized compositing), but ActRaiser may have been tuned for old-PPU quirks. This needs testing with a ROM. If any visual differences appear, they are bugs in the new PPU path that should be fixed (it's the intended production path for all snesrecomp games).

**Fallback:** If new-PPU differences are blockers, add overlay capture support to the old PPU path. This requires modifying `ppu_handlePixel` (ppu_old.c:78) to write each layer's contribution to separate per-layer buffers before compositing. The `ppu_getPixel` function (ppu_old.c:139) already identifies which layer won priority per pixel — tap that to write the winning pixel to its layer's surface.

### 4.2 Diorama Capture Policy

The implemented policy captures **visual sources**, not just TM. Marahna action
mode uses TM=`$06` (BG2+BG3), TS=`$11` (BG1+OBJ), CGWSEL=`$02`, and
CGADSUB=`$03`: BG1 and OBJ exist exclusively as the subscreen input to a full
colour add. A main-only gate therefore removes the playable plane and every
actor even though authentic scanout is healthy.

The current policy is equivalent to:

```c
bool diorama_active = Diorama_IsActiveThisFrame();
if (diorama_active) {
    uint8_t visual_sources =
        g_ppu->screenEnabled[0] | g_ppu->screenEnabled[1];
    uint8_t full_add_sub_sources =
        DioramaCaptureBlend_FullAddSubscreenSources(
            g_ppu->cgwsel, g_ppu->cgadsub,
            g_ppu->screenEnabled[0], g_ppu->screenEnabled[1]);

    static const PpuOverlaySource sources[] = {
        kPpuOverlaySource_Bg1, kPpuOverlaySource_Bg2,
        kPpuOverlaySource_Obj,
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        PpuOverlaySource source = sources[i];
        if (!(visual_sources & (1 << source)))
            continue;
        uint8_t flags = kPpuOverlayFlag_RemoveFromGame;
        if (full_add_sub_sources & (1 << source))
            flags |= kPpuOverlayFlag_MarkFullAddSubscreen;
        // Eligible half-add/fixed-subtract flags are classified separately.
        PpuSetOverlayCapture(..., source, ..., flags);
    }
    if (visual_sources & (1 << kPpuOverlaySource_Obj))
        PpuSetOverlayOamRange(g_ppu, 0, 128);

    // BG3 is rebound explicitly every frame. Flat-HUD mode leaves the existing
    // HUD capture authoritative; tilted-HUD mode binds the Diorama BG3 plane.
}
```

The frontend union is only the frame-level admission test. During scanout the
PPU chooses the owning rendering per line, after HDMA may have changed TM/TS:
prefer main when the source is present there, otherwise export its subscreen
rendering. A source on both screens is not exported twice.

Full-add is stricter than source capture. The helper admits it only when
CGWSEL is exactly `$02`, neither half nor subtract is active, TM and TS visual
sources are disjoint, and a main winner can enable math. The PPU then exports a
sparse TS plane only where that source wins subscreen priority and the resolved
main winner is selected by CGADSUB. `FrameSlot.diorama_plane_additive_mask`
carries that immutable fact to present; the compositor orders main world,
additive TS, then BG3. Unsupported overlapping/window/direct-colour states fail
closed instead of being inferred from one frame-start register sample.

OBJ still requires the full OAM range: `PpuSetOverlayCapture` clears
`oamCount`, and zero means no sprite pixels are captured. A separately promoted
HUD icon range remains in the ordinary OBJ capture but is excluded from the
full-add OBJ scratch so relocating it cannot leave an icon-shaped hole in the
world addend.

Transition frames need no map-number allowlist. `Diorama_IsActiveThisFrame`
owns the mode gate, source union suppresses genuinely disabled layers, and
forced blank remains the black-frame authority. BG3's explicit per-frame rebind
is also load-bearing: omitting a source from a capture list does not unbind the
surface used on the preceding frame.

### 4.3 Layer Buffers

Diorama uses dedicated host surfaces rather than
`g_hd_overlay_pixels[]`. The primary BG1/BG2/BG3/OBJ planes and the BG1/BG2
high-priority plus OBJ1-OBJ3 bands remain distinct destinations. OBJ surfaces
also carry the resolve-only apron documented in rendering-engine.md §13j, so
their pitch is not interchangeable with the displayed width.

The pixel buffers are **not double-buffered**. Single-threaded ordering uploads
them after scanout and before the next tick can overwrite them. `FrameSlot`
retains immutable metadata for architectural isolation, not for a concurrent
pixel handoff. This is the implementation deviation recorded in the status
banner and present.h ownership contract.

Surface binding is explicit per frame:

- BG1, BG2, OBJ, and every priority band bind to their Diorama surfaces;
- with `diorama_hud_flat=true`, BG3 binds to the narrow
  `g_hud_bg_pixels` surface and the established HUD capture remains
  authoritative (extended through the authentic height for title/pause body
  rows);
- with a tilted HUD, BG3 binds to its Diorama surface and receives the full
  scene capture;
- OBJ capture claims OAM 0..127; a validated promoted-HUD range is separately
  tagged for exclusion from the full-add scratch only.

The explicit BG3 branch is self-healing across flat↔tilted toggles. There is no
generic “unbind” operation: leaving BG3 out of an array would preserve the old
destination and create a live tilted ghost plus a frozen flat HUD.

`overlayCaptures[source]` still provides one capture rectangle per source.
A post-scanout HD-replacement capture cannot independently claim that same
source. Pipeline-level replacements remain compatible because they reach the
ordinary PPU scanout and are naturally present in the separated surfaces.

The buffers use the alpha/blend policy in §5.8. Request, content, and additive
plane masks are latched after scanout so present uploads only frame-owned
surfaces and never rescans CPU pixels or reads live PPU/settings state.

### 4.4 What "RemoveFromGame" Leaves in `g_pixels`

With all layers captured and removed, the main composite in `PpuDrawWholeLine` (ppu.c:1425-1476) resolves every pixel to palette index 0 (the backdrop) because `bgBuffers[0].data[i]` will be 0 for all positions where an overlay captured the winning pixel. The backdrop is CGRAM[0] — the solid background color.

The diorama renderer uses `g_pixels` (now just the backdrop plane) as the
farthest-Z surface. **Caution:** `g_pixels` carries alpha=0 (the PPU never writes
the alpha byte — in the NEW path diorama uses, the composite at ppu.c:1436-1438 and
ppu.c:1473 writes only RGB, leaving the top byte 0; same result as the old path's
ppu_old.c:136) — the backdrop plane must be drawn with
`SDL_BLENDMODE_NONE` (or have its alpha forced to 0xFF), or it blits invisible.
See §5.8.

### 4.5 Edge Cases

| Case | Handling |
|------|----------|
| Per-tile/per-OBJ priority | Implemented priority-band surfaces split BG1/BG2 low/high and OBJ priorities 0-3. Each resolved pixel lands in exactly one band; Diorama orders the bands with the Mode-1 interleave, and full-add metadata covers every band belonging to an admitted source. |
| Color math (subscreen blending) | See §8.1 below. |
| Non-action / Mode 7 | Action Diorama is gated off. Mode-1 town Sim3D and Mode-7 world-navigation 3D are independent present branches with their own immutable frame contracts; see §8.4. |
| Forced blank (INIDISP bit 7) | PPU outputs black. Diorama renderer detects forced blank and shows nothing (or a static "powered off" screen). |
| BG3 mode 1 high priority | In SNES mode 1 with `bg3priority` set, BG3 high-priority tiles render ABOVE everything (even sprites). The overlay capture puts these in the BG3 surface. In diorama, BG3 would be placed at a fixed Z. This is a fidelity difference — acceptable for the effect. |

---

## 5. Phase 4: Diorama Renderer <a name="5-phase-4-diorama-renderer"></a>

**Goal:** Project each layer as a textured quad at a different Z depth, with a mild camera tilt to create a parallax diorama/shadowbox effect.

### 5.1 Camera Model

```c
typedef struct DioramaCamera {
    float tilt_x;       // pitch (radians), positive = top tilts away from viewer
    float tilt_y;       // yaw (radians), positive = right side tilts away
    float distance;     // camera distance from the scene center (affects perspective strength)
    float fov_y;        // vertical field of view (radians); ~0.3 for mild perspective
} DioramaCamera;

// fov_y has no user setting — it is a fixed named constant:
static const float kDioramaFovY = 0.4f;   // ~23 degrees — mild perspective

// g_diorama_cam is NOT initialized with literal defaults (that would create a
// second source of truth vs the settings descriptors — see §D13). It is SEEDED
// from g_settings at init and on "Reset Camera":
static DioramaCamera g_diorama_cam;   // zero-init; populated by Diorama_SeedCameraFromSettings()

static void Diorama_SeedCameraFromSettings(void) {
    g_diorama_cam.tilt_x   = g_settings.diorama_tilt_x_mrad / 1000.0f;   // mrad -> rad
    g_diorama_cam.tilt_y   = g_settings.diorama_tilt_y_mrad / 1000.0f;
    g_diorama_cam.distance = g_settings.diorama_distance_x100 / 100.0f;  // x100 -> float
    g_diorama_cam.fov_y    = kDioramaFovY;
}
// The descriptor defaults (§10.2: tilt_x=150, tilt_y=0, distance=500) are the
// single source of truth; the ~8.6°/5.0 defaults live ONLY there.
```

### 5.2 Layer Z Assignments

```c
typedef struct DioramaLayer {
    int source;         // kPpuOverlaySource_* or -1 for backdrop, -2 for Mode-7
    float z;            // depth (0 = farthest, 1 = nearest)
    bool visible;       // toggled by settings
    SDL_Texture *texture;
    uint8_t *pixels;    // CPU-side RGBA buffer
} DioramaLayer;

// ACTION STAGES ONLY (Mode 1). Sim mode is out of scope (see Scope banner).
// Action-stage Mode 1 layer roles:
//   BG1 = main playfield (4bpp)
//   BG2 = parallax background (4bpp)
//   BG3 = status bar / text (2bpp)
//   BG4 = unused in Mode 1 (never captured)
// The Z values are the primary tuning knob — named constants, not buried
// literals (§D12), so a tuning pass edits one findable block:
static const float kDioramaZ_Backdrop = 0.00f;  // farthest
static const float kDioramaZ_Bg2      = 0.20f;
static const float kDioramaZ_Bg1      = 0.50f;
static const float kDioramaZ_Obj      = 0.75f;
static const float kDioramaZ_Hud      = 0.95f;  // nearest (BG3/HUD)

static DioramaLayer g_diorama_layers[] = {
    { .source = -1,                        .z = kDioramaZ_Backdrop },  // solid color
    { .source = kPpuOverlaySource_Bg2,     .z = kDioramaZ_Bg2 },       // distant parallax
    { .source = kPpuOverlaySource_Bg1,     .z = kDioramaZ_Bg1 },       // main playfield
    { .source = kPpuOverlaySource_Obj,     .z = kDioramaZ_Obj },       // sprites
    { .source = kPpuOverlaySource_Bg3,     .z = kDioramaZ_Hud },       // HUD/status bar
};
// No mode-aware Z table is needed: the renderer only engages in action stages,
// which are uniformly Mode 1. There is no runtime mode-detection branch inside
// the renderer — the gate lives in the capture policy (§4.2). If a future scene
// uses a different action-stage BG mode, extend here.
```

### 5.3 Perspective Projection (CPU-side)

Each layer is a quad in 3D space. Project vertices on the CPU, submit via `SDL_RenderGeometry`.

```c
typedef struct Vec3 { float x, y, z; } Vec3;
typedef struct Vec2 { float x, y; } Vec2;

// Build a view-projection matrix from the camera state. Column-major (SDL/GL
// convention: out_mat[col*4 + row], consumed by ProjectPoint below). Layers span
// X,Y in [-0.5, 0.5] at their Z depth; Z in [0,1] is remapped so 0 (backdrop) is
// farthest. Aspect is applied as the output aspect so the unit square isn't
// stretched. near/far bracket the layer Z range with margin.
static void Mat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[k*4 + r] * b[c*4 + k];
            out[c*4 + r] = s;
        }
}
static void BuildViewProjection(const DioramaCamera *cam, int out_w, int out_h,
                                float out_mat[16]) {
    const float kNear = 0.1f, kFar = 100.0f;
    float aspect = (out_h > 0) ? (float)out_w / (float)out_h : 1.0f;
    float f = 1.0f / tanf(cam->fov_y * 0.5f);

    // Perspective (column-major). fov_y is vertical; divide x term by aspect.
    float proj[16] = {
        f/aspect, 0, 0,                                   0,
        0,        f, 0,                                   0,
        0,        0, (kFar+kNear)/(kNear-kFar),          -1,
        0,        0, (2*kFar*kNear)/(kNear-kFar),         0,
    };

    // Layers live around origin in Z; place the stack in front of the camera.
    // Map layer z in [0,1] to world Z in [-0.5, +0.5] (0 -> -0.5 = farthest).
    // View = rotateX(tilt_x) * rotateY(tilt_y) then translate back by distance.
    float cx = cosf(cam->tilt_x), sx = sinf(cam->tilt_x);
    float cy = cosf(cam->tilt_y), sy = sinf(cam->tilt_y);
    float rotY[16] = { cy,0,sy,0,  0,1,0,0,  -sy,0,cy,0,  0,0,0,1 };
    float rotX[16] = { 1,0,0,0,  0,cx,sx,0,  0,-sx,cx,0,  0,0,0,1 };
    float trans[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,-cam->distance,1 };

    float rot[16], view[16], vp[16];
    Mat4Mul(rotX, rotY, rot);       // rot = rotX * rotY
    Mat4Mul(trans, rot, view);      // view = translate * rot  (rotate then push back)
    Mat4Mul(proj, view, vp);        // vp = proj * view
    for (int i = 0; i < 16; i++) out_mat[i] = vp[i];
}
// Callers pass layer world Z as (layer->z - 0.5f) to center the [0,1] stack on
// the origin before this matrix; see BuildLayerMesh.

// Project a 3D point to 2D screen coordinates.
static Vec2 ProjectPoint(const float mvp[16], Vec3 world, int screen_w, int screen_h) {
    // Standard: clip = mvp * [world.x, world.y, world.z, 1]
    // ndc = clip.xyz / clip.w
    // screen = (ndc.xy * 0.5 + 0.5) * [screen_w, screen_h]
    float clip[4];
    clip[0] = mvp[0]*world.x + mvp[4]*world.y + mvp[8]*world.z  + mvp[12];
    clip[1] = mvp[1]*world.x + mvp[5]*world.y + mvp[9]*world.z  + mvp[13];
    clip[2] = mvp[2]*world.x + mvp[6]*world.y + mvp[10]*world.z + mvp[14];
    clip[3] = mvp[3]*world.x + mvp[7]*world.y + mvp[11]*world.z + mvp[15];
    float inv_w = 1.0f / clip[3];
    return (Vec2){
        (clip[0] * inv_w * 0.5f + 0.5f) * screen_w,
        (1.0f - (clip[1] * inv_w * 0.5f + 0.5f)) * screen_h,  // Y-flip for screen coords
    };
}
```

### 5.4 Quad Subdivision for Perspective Correction

`SDL_RenderGeometry` uses affine (not perspective-correct) texture interpolation. For a mild tilt this produces visible warping on large quads. Solution: subdivide each layer quad into an NxM grid.

```c
#define DIORAMA_SUBDIV_X 8
#define DIORAMA_SUBDIV_Y 6
// Total per layer: 8*6*2 = 96 triangles, 9*7 = 63 vertices
// Total for 5 layers: 480 triangles — trivial for any GPU.

static void BuildLayerMesh(const float mvp[16], float z_depth,
                           int screen_w, int screen_h,
                           SDL_Vertex *out_verts, int *out_indices,
                           int *num_verts, int *num_indices) {
    // The quad spans [-0.5, 0.5] in X and Y at the given z_depth.
    // Subdivide into SUBDIV_X * SUBDIV_Y quads (each = 2 triangles).
    int vi = 0;
    for (int row = 0; row <= DIORAMA_SUBDIV_Y; row++) {
        for (int col = 0; col <= DIORAMA_SUBDIV_X; col++) {
            float u = (float)col / DIORAMA_SUBDIV_X;
            float v = (float)row / DIORAMA_SUBDIV_Y;
            Vec3 world = { u - 0.5f, v - 0.5f, z_depth };
            Vec2 screen = ProjectPoint(mvp, world, screen_w, screen_h);
            out_verts[vi].position = (SDL_FPoint){ screen.x, screen.y };
            out_verts[vi].tex_coord = (SDL_FPoint){ u, v };
            out_verts[vi].color = (SDL_FColor){ 1, 1, 1, 1 };
            vi++;
        }
    }
    *num_verts = vi;
    // Build triangle indices (2 per sub-quad, standard grid)
    int ii = 0;
    int cols = DIORAMA_SUBDIV_X + 1;
    for (int row = 0; row < DIORAMA_SUBDIV_Y; row++) {
        for (int col = 0; col < DIORAMA_SUBDIV_X; col++) {
            int tl = row * cols + col;
            out_indices[ii++] = tl;
            out_indices[ii++] = tl + 1;
            out_indices[ii++] = tl + cols;
            out_indices[ii++] = tl + 1;
            out_indices[ii++] = tl + cols + 1;
            out_indices[ii++] = tl + cols;
        }
    }
    *num_indices = ii;
}
```

### 5.5 Render Pass

This runs on whichever thread owns the renderer: in milestones M2-M3 (diorama built
first) it runs **synchronously** in the existing present path; after M5 (present
thread) it runs on the present thread. Either way it reads a `FrameSlot` — the
signature takes `const FrameSlot *` so the same code works before and after the
threading refactor (synchronously, `slot` is just the current frame's state).

```c
static void RenderDiorama(const FrameSlot *slot, SDL_Renderer *renderer,
                          int out_w, int out_h) {
    float mvp[16];
    BuildViewProjection(&g_diorama_cam, mvp);

    // Clear to a dark background
    SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
    SDL_RenderClear(renderer);

    // Render layers back-to-front (sorted by Z ascending = farthest first)
    for (int i = 0; i < g_diorama_layer_count; i++) {
        DioramaLayer *layer = &g_diorama_layers[i];
        if (!layer->visible || !layer->texture) continue;

        // Upload this frame's pixels to the layer's texture.
        // THREADING (implemented contract — see §4.3): layer pixels live in
        // dedicated Diorama surfaces, not g_hd_overlay_pixels and not a
        // FrameSlot memcpy. Upload/capture ordering protects those surfaces;
        // FrameSlot carries immutable request/content/additive masks.
        SDL_UpdateTexture(layer->texture, NULL, layer->pixels,
                          slot->snes_width * 4);

        // BLEND MODE IS PER-LAYER, NOT UNIFORM (see §5.8 alpha model):
        //  - Backdrop plane (source == -1, from g_pixels) carries ALPHA = 0
        //    (new-path composite ppu.c:1436-1438/1473 writes RGB only; the old
        //    path's ppu_old.c:136 does the same). Under BLEND it would blit
        //    fully TRANSPARENT -> invisible backdrop. It is the farthest, fully
        //    opaque plane, so draw it with NONE.
        //  - BG/OBJ capture planes carry straight alpha (0xFF opaque / 0x00
        //    empty, PpuOverlayColor ppu.c:1219-1224) -> BLEND is correct.
        bool is_backdrop = (layer->source == -1);
        SDL_SetTextureBlendMode(layer->texture,
            is_backdrop ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(layer->texture, SDL_SCALEMODE_NEAREST);

        // Build the subdivided mesh for this layer's Z plane
        SDL_Vertex verts[(DIORAMA_SUBDIV_X+1) * (DIORAMA_SUBDIV_Y+1)];
        int indices[DIORAMA_SUBDIV_X * DIORAMA_SUBDIV_Y * 6];
        int nv, ni;
        BuildLayerMesh(mvp, layer->z, out_w, out_h, verts, indices, &nv, &ni);
        SDL_RenderGeometry(renderer, layer->texture, verts, nv, indices, ni);
    }

    // UI overlays (settings, inspector) always render flat on top
    SDL_Rect viewport = { 0, 0, out_w, out_h };
    SettingsOverlay_Render(viewport);
}
```

### 5.6 Camera Controls

The camera is a **player personalization within a limited range**: the player nudges
tilt and zoom to taste, but the clamps (below) keep the adjustment inside a band
where the diorama effect always reads correctly — you can angle and zoom the
shadowbox, but you cannot flatten it to nothing, flip it, or zoom past the planes.
The player's chosen angle/zoom persists (it's stored in the same settings the menu
edits), so it's remembered across sessions and is effectively a per-player camera
preference.

**One input-source-agnostic adjust seam (do this — it is the whole point of the
refactor).** All camera input — mouse today, right stick later — funnels through a
SINGLE function that applies deltas, clamps, and writes back. Input handlers only
convert their raw events into `(d_yaw, d_pitch, d_zoom)` deltas; they never touch
`g_diorama_cam` or `g_settings` directly. This means adding the right stick later is
purely "emit deltas from axis events into the existing function" — zero new
clamp/write-back/persistence logic.

```c
// The single choke point. Deltas are in RADIANS (tilt) and DISTANCE UNITS (zoom).
// Clamp ranges are the "limited range" guardrail and MUST match §10.2 exactly.
static const float kDioramaTiltMin = -0.7f, kDioramaTiltMax = 0.7f;   // ±700 mrad
static const float kDioramaDistMin =  2.0f, kDioramaDistMax = 20.0f;  // 200..2000 x100

static float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void Diorama_AdjustCamera(float d_yaw, float d_pitch, float d_zoom) {
    g_diorama_cam.tilt_y   = Clampf(g_diorama_cam.tilt_y   + d_yaw,   kDioramaTiltMin, kDioramaTiltMax);
    g_diorama_cam.tilt_x   = Clampf(g_diorama_cam.tilt_x   + d_pitch, kDioramaTiltMin, kDioramaTiltMax);
    g_diorama_cam.distance = Clampf(g_diorama_cam.distance + d_zoom,  kDioramaDistMin, kDioramaDistMax);
    // Write back so the value persists and the menu stays in sync (single source
    // of truth, §D13). fov_y is not player-adjustable.
    g_settings.diorama_tilt_x_mrad   = (int)(g_diorama_cam.tilt_x   * 1000.0f);
    g_settings.diorama_tilt_y_mrad   = (int)(g_diorama_cam.tilt_y   * 1000.0f);
    g_settings.diorama_distance_x100 = (int)(g_diorama_cam.distance * 100.0f);
    g_diorama_settings_dirty = true;  // debounced settings.ini save (see below)
}
```

**Mouse mapping (implement now).** Handlers emit deltas into `Diorama_AdjustCamera`:

| Input | Emits | Constant |
|-------|-------|----------|
| Right-drag horizontal | `Diorama_AdjustCamera(dx_px * kDioramaDragRadPerPx, 0, 0)` | drag sensitivity |
| Right-drag vertical | `Diorama_AdjustCamera(0, dy_px * kDioramaDragRadPerPx, 0)` | drag sensitivity |
| Scroll wheel | `Diorama_AdjustCamera(0, 0, -wheel_y * kDioramaZoomStep)` | wheel UP = zoom IN (smaller distance) |
| Middle-click | reset: set the 3 settings to descriptor defaults, then `Diorama_SeedCameraFromSettings()` | — |

```c
static const float kDioramaDragRadPerPx = 0.005f;  // ~0.29 deg/px; full ±0.7 sweep ≈ 280px drag
static const float kDioramaZoomStep     = 0.5f;    // distance units per wheel notch
```

Right-drag reuses the existing motion/`WindowPointToOutput` handling; `dx/dy` are
per-event pixel deltas from `SDL_EVENT_MOUSE_MOTION` while the right button is held
(mirror the debug-panel drag-tracking pattern, main.c:2306-2312). Only accept camera
drag while `Diorama_IsActiveThisFrame()` (§D14) is true.

**Right stick (FUTURE — seam only, do not build in v1).** `SDL_INIT_GAMEPAD` is
already initialized (main.c:1971) but no gamepad is opened and no axis events are
handled yet. When added, the right-stick handler is small: open the gamepad, and on
each frame read `SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX/RIGHTY)`, apply a
deadzone, scale by a per-second rate × frame dt, and call `Diorama_AdjustCamera`.
A trigger or stick-click maps to reset. Because it goes through the same choke
point, it inherits the identical clamps, write-back, and persistence — nothing else
changes. Suggested constants for that future work (not needed now):

```c
// FUTURE: static const float kDioramaStickRadPerSec = 1.2f;  // ~69 deg/s at full deflection
//         static const float kDioramaStickZoomPerSec = 6.0f; // distance units/s
//         static const int   kDioramaStickDeadzone = 8000;   // of 32767
```

**Persistence debounce.** `Diorama_AdjustCamera` runs every mouse-motion/frame, so
do NOT `Settings_Save` inside it — set `g_diorama_settings_dirty` and flush once in
the per-outer-iteration housekeeping (§3.5) when it's been set and no adjust has
happened for ~0.5s (or on menu close / mode toggle / quit). This matches how other
live-tweaked settings avoid per-event disk writes.

When diorama mode is toggled OFF, animate `tilt_x`/`tilt_y` → 0 and `fov_y` → 0 over ~0.3s (lerp), which visually flattens back to the normal 2D view. This is purely a present-thread animation — no game state change (the persisted settings keep the player's chosen angle for next time; the animation uses a separate transient copy).

### 5.7 Layer Visibility UI

In the settings overlay, add a "Diorama" section with per-layer visibility toggles. Allow the user to hide individual layers (e.g., hide BG3/HUD to see the playfield clearly, or hide BG1 to see the parallax background alone).

### 5.8 Alpha and blend model — the single source of truth

The SNES has no alpha channel. It supplies source presence, per-pixel priority,
and main/subscreen colour arithmetic. The host starts from **presence**:
palette index zero becomes transparent and any drawn BG/OBJ pixel becomes
straight-alpha RGBA with alpha `$FF`. Priority becomes the separated Diorama
plane ordering. Colour math is handled only by an explicit proven transform; it
must never be guessed from opacity or layer depth.

`PpuCapturedOverlayColor` applies the supported extraction policies:

- ordinary drawn pixel: straight alpha `$FF`; empty pixel: all-zero RGBA;
- eligible subscreen half-add: alpha `$80` (BG per layer, OBJ per eligible
  palette group);
- eligible fixed-colour BG subtraction: RGB is transformed in native 5-bit
  component space before master-brightness expansion, alpha remains `$FF`;
- disjoint full-add subscreen input: alpha remains `$FF`, but the PPU writes a
  sparse plane containing only pixels where the source is the resolved TS
  winner and the main winner enables math.

The resulting compositor contract is:

| Plane/content | Buffer representation | SDL blend |
| --- | --- | --- |
| Backdrop `g_pixels` | RGB with alpha zero in the new PPU path | `SDL_BLENDMODE_NONE` |
| Ordinary BG/OBJ | straight alpha `$FF` drawn / zero empty | `SDL_BLENDMODE_BLEND` |
| Half-added BG or OBJ pixel | straight alpha `$80` | `SDL_BLENDMODE_BLEND` |
| Resolved full-add TS plane | sparse straight-alpha `$FF` addends | `SDL_BLENDMODE_ADD` |
| Fixed-colour-subtracted BG | already-corrected RGB, binary alpha | `SDL_BLENDMODE_BLEND` |

`SDL_BLENDMODE_BLEND` implements straight source-over; the captured RGB is not
premultiplied. Do not use a premultiplied blend without converting the buffers.
`SDL_BLENDMODE_ADD` is used only for the resolved full-add subset published in
`FrameSlot.diorama_plane_additive_mask`. The three-pass order is ordinary main
world, additive TS, then BG3. Ordinary scenes retain their historical painter
pass.

INIDISP brightness is already baked into captured RGB. COLDATA/CGADSUB effects
are not automatically baked merely because the native flat framebuffer is
correct: only the explicitly classified half-add, disjoint full-add, and
fixed-colour-subtract forms above are reproduced. Overlapping main/sub source
ownership, unsupported windows/direct colour, and general subtraction fail
closed as described in §8.1.

Nearest sampling preserves transparent edges. A future linear-sampling path
must premultiply or otherwise pad edge colours to avoid dark fringes. Optional
per-layer opacity may multiply vertex alpha, but it is a presentation effect and
must remain separate from the hardware colour-math classification.

### 5.9 Aspect/crop fidelity — the diorama bypasses the flat present pipeline

The flat path applies two corrections the diorama render (§5.5) does NOT, because
it draws geometry directly instead of going through logical presentation:

- **CRT pixel-aspect (PAR) stretch.** The flat path encodes the ~7:6 horizontal
  stretch into the logical size (`SDL_SetRenderLogicalPresentation(vis_w*7, h*6, …)`,
  main.c:945/1044) or, in pure 4:3, into the window sizing (main.c:931-934).
  `ProjectPoint` (§5.3) maps straight to raw `screen_w/screen_h`, so the diorama
  renders square-pixel — geometrically squished vs the normal view whenever
  `pixel_aspect == kPixelAspect_Crt43`. **Fix:** scale the projection's X by 7/6
  when `slot->pixel_aspect == kPixelAspect_Crt43` (else 1); decide explicitly
  whether `ignore_aspect_ratio` suppresses it (main.c:1049-1052).
- **VisibleX0/VisibleWidth crop.** The flat path crops to the active sub-rect
  (`src = {Settings_VisibleX0(), 0, Settings_VisibleWidth(), h}`, main.c:1759). §5.4
  maps full [0,1] UV across the whole `snes_width` texture (which includes the
  `ws_extra` margin columns). In a widescreen-allowed build showing 4:3, the diorama
  would expose margins the flat path hides. **Fix:** restrict each layer's UV rect
  to `[VisibleX0/snes_width, (VisibleX0+VisibleWidth)/snes_width]` using
  `slot->display_mode`.

The FrameSlot already snapshots `pixel_aspect` and `display_mode` (§2.1); the
diorama render code just has to consume them.

**Inert flat-path controls in diorama mode (document in §10):** while diorama is
active, `hud_scale_percent` and the widescreen HUD split, Mode-7 overlay, and HD
replacements do NOT apply (RenderDiorama does not call `RenderHudOverlay`/
`RenderMode7Overlay`/`RenderHdReplacements`; it repurposes the BG3/OBJ captures as
Z-planes). Cycling Display Mode / `ignore_aspect_ratio` also has no coherent effect
on the 3D geometry unless the PAR/crop fixes above are implemented. Either mark
those controls disabled while diorama is on, or define how they map onto the planes.

---

## 6. Phase 5: High-Framerate Interpolation <a name="6-phase-5-interpolation"></a>

**Goal:** At display rates above 60Hz, interpolate layer positions between game frames for smooth motion without re-running game logic.

### 6.1 Per-Frame Scroll Snapshot

After each `RtlDrawPpuFrame`, snapshot the PPU scroll registers:

```c
typedef struct FrameScrollState {
    int16_t bg_hscroll[4];   // per-layer horizontal scroll
    int16_t bg_vscroll[4];   // per-layer vertical scroll
    int16_t m7_matrix[4];    // Mode-7 affine matrix (A,B,C,D)
    int16_t m7_center[2];    // Mode-7 center X,Y
    uint64_t timestamp_ns;   // wall-clock when this frame was produced
} FrameScrollState;

// Ring buffer of last 2 frames for lerp
static FrameScrollState g_scroll_ring[2];
static int g_scroll_write_idx;
```

Populate after `ActRaiserDrawPpuFrame` completes:
```c
FrameScrollState *ss = &g_scroll_ring[g_scroll_write_idx];
ss->timestamp_ns = SDL_GetTicksNS();
for (int i = 0; i < 4; i++) {
    ss->bg_hscroll[i] = g_ppu->hScroll[i];
    ss->bg_vscroll[i] = g_ppu->vScroll[i];
}
// The Mode-7 matrix is int16_t m7matrix[8] laid out {a,b,c,d,x,y,h,v}
// (ppu.h:141). There is NO m7xCenter/m7yCenter field — the center is
// m7matrix[4] (x) and m7matrix[5] (y). Copy a,b,c,d and the center out of
// the one array. (If the raw 13-bit sign-extended center is needed, apply
// the ((int16_t)(v<<3))>>3 extension used at ppu.c:1103-1104.)
memcpy(ss->m7_matrix, g_ppu->m7matrix, sizeof(ss->m7_matrix)); // a,b,c,d in [0..3]
ss->m7_center[0] = g_ppu->m7matrix[4];   // NOT g_ppu->m7xCenter (does not exist)
ss->m7_center[1] = g_ppu->m7matrix[5];   // NOT g_ppu->m7yCenter (does not exist)
g_scroll_write_idx = 1 - g_scroll_write_idx;
```

Note `hScroll`/`vScroll` are `uint16` 10-bit values (ppu.h:139-140); the wrap
detection in §6.4 must treat them as modular over 0..1023, and `bg_mode` (a
FrameSlot field snapshotting `PPU_mode`) feeds the §6.4 interpolation mode-change
guard — NOT a diorama Z branch (diorama is Mode-1 only, §5.2). `display_mode` is the
4:3/Wide aspect setting, NOT the SNES PPU BG mode.

### 6.2 Present-Time Interpolation

On the present thread, each vsync frame:
```c
uint64_t now = SDL_GetTicksNS();
FrameScrollState *prev = &g_scroll_ring[1 - g_scroll_write_idx];
FrameScrollState *curr = &g_scroll_ring[g_scroll_write_idx];

float t = 0.0f;
uint64_t span = curr->timestamp_ns - prev->timestamp_ns;
if (span > 0 && span < 50000000) {  // sanity: <50ms between frames
    uint64_t elapsed = now - curr->timestamp_ns;
    t = (float)elapsed / (float)span;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
}

// Interpolated scroll for each BG layer
for (int i = 0; i < 4; i++) {
    float h = prev->bg_hscroll[i] + t * (curr->bg_hscroll[i] - prev->bg_hscroll[i]);
    float v = prev->bg_vscroll[i] + t * (curr->bg_vscroll[i] - prev->bg_vscroll[i]);
    // Apply as UV offset to that layer's quad mesh
    float du = (h - curr->bg_hscroll[i]) / (float)g_snes_width;
    float dv = (v - curr->bg_vscroll[i]) / (float)g_snes_height;
    // Shift all tex_coords in the layer's mesh by (du, dv)
}
```

### 6.3 How Interpolation Manifests Visually

**In flat (2D) mode:** Scroll interpolation shifts the source rect by a sub-pixel amount between game frames. At 120Hz, you see intermediate scroll positions that the game logic never computed — this makes scrolling backgrounds appear to move at 120fps instead of 60fps. The pixel content doesn't change (same texture), just *where* you sample it.

**In diorama mode:** Each layer's quad shifts slightly in X/Y based on its interpolated scroll. The parallax effect is enhanced because each layer moves at its own rate (BG2 scrolls slower than BG1 in a side-scroller), and those differences are now visible at 120fps.

### 6.4 Edge Cases

| Case | Handling |
|------|----------|
| Scroll register wraps (0→1023→0) | Detect wrap: if `abs(curr - prev) > 512`, assume a wrap and interpolate through the modular distance. |
| **UV shift pushes tex_coords outside [0,1]** | §6.2 shifts each vertex's `tex_coord` by `(du,dv)` to fake sub-frame scroll, which pushes UVs past the [0,1] edge. `SDL_RenderGeometry`'s default addressing is `SDL_TEXTURE_ADDRESS_AUTO`, which **wraps** for power-of-two textures (SDL_render.h:119-125) — so the opposite (possibly opaque) edge of a layer wraps into the near edge. **DECISION (not "either/or"):** call `SDL_SetRenderTextureAddressMode(renderer, SDL_TEXTURE_ADDRESS_CLAMP, SDL_TEXTURE_ADDRESS_CLAMP)` ONCE at the top of the `ComposeDiorama` pass and restore `SDL_TEXTURE_ADDRESS_AUTO` immediately after it — one renderer call, scoped so the flat path (which interpolates by shifting the source rect, not UVs, §6.3) and the UI/overlay tail are unaffected. This is preferred over per-vertex UV clamping (63 verts/layer). Keep NEAREST. Fallback only if a backend lacks the entrypoint: clamp the shifted UVs inside `BuildLayerMesh`. |
| HDMA-driven per-scanline scroll | The scroll snapshot is the register value at frame end (after HDMA). HDMA effects are baked into the pixel data — interpolation only shifts the whole texture, not per-scanline. This means HDMA wave effects are still 60fps, but whole-frame scroll is smooth. Acceptable. |
| Mode change (Mode 1 ↔ Mode 7) | Detect mode change between frames. On a mode change, set `t = 0` (no interpolation that frame) to avoid lerping between incompatible states. |
| Frame skip (turbo mode) | Only the most recent two frames populate the ring. Turbo may produce many frames between presents — the ring always has the two LATEST. `span` will be small (sub-ms), so `t` will be >1 and clamped. The present just shows the latest frame with no interpolation artifact. |
| Sprite interpolation | Sprites have per-object positions in OAM, not a single scroll register. Full sprite interpolation requires tracking OAM slot identity across frames (which slot is "the same sprite"). This is a significant extension — defer to a future phase. For now, sprites are not interpolated (they display at their game-frame positions). |

---

## 7. Phase 6: GPU Shader Path (Optional) <a name="7-phase-6-gpu-shaders"></a>

**Goal:** Replace the CPU-projected `SDL_RenderGeometry` approach with a GPU vertex+fragment shader for real perspective-correct rendering and per-layer post-processing.

### 7.1 SDL3 GPU Renderer Integration

> **This section is an illustrative API-usage sketch, not copy-paste-ready code.**
> Phase 6 / M8 is optional (the M2 `SDL_RenderGeometry` path is fully sufficient).
> The fragment shader source (`msl_source`/`msl_source_len`) is intentionally not
> provided — it must be authored during M8. The snippet hardcodes
> `SDL_GPU_SHADERFORMAT_MSL` (Metal-only); a real implementation must query
> `SDL_GetGPUShaderFormats(device)` and supply MSL (Metal), SPIR-V (Vulkan), or
> DXIL (D3D12) for the active backend. Do not attempt to build this section
> verbatim.

SDL 3.4.12 supports the `"gpu"` renderer backend. Key API surfaces:

```c
// Create renderer with GPU backend:
SDL_Renderer *renderer = SDL_CreateRenderer(window, SDL_GPU_RENDERER);

// Retrieve the GPU device (for shader creation):
SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
SDL_GPUDevice *device = SDL_GetPointerProperty(props,
    SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);

// Create a fragment shader:
SDL_GPUShaderCreateInfo shader_info = {
    .code = msl_source,
    .code_size = msl_source_len,
    .entrypoint = "fragment_main",
    .format = SDL_GPU_SHADERFORMAT_MSL,
    .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
    .num_samplers = 1,
    .num_uniform_buffers = 1,
};
SDL_GPUShader *frag = SDL_CreateGPUShader(device, &shader_info);

// Bind into a render state:
SDL_GPURenderStateCreateInfo state_info = {
    .fragment_shader = frag,
};
SDL_GPURenderState *state = SDL_CreateGPURenderState(renderer, &state_info);

// Use during rendering:
SDL_SetGPURenderState(renderer, state);
// ... SDL_RenderGeometry calls now use custom shader ...
SDL_SetGPURenderState(renderer, NULL);  // restore default
```

### 7.2 Vertex Shader (Perspective Projection)

With the GPU renderer, vertex positions in `SDL_RenderGeometry` are still 2D screen-space. However, a custom vertex shader (if the GPU backend exposes one) or a fragment shader with uniforms can implement:

- **Depth-of-field blur** per layer (layers farther from the focal plane get a Gaussian blur)
- **Drop shadows** between layers (each layer casts a soft shadow on the one behind it)
- **Edge glow / rim lighting** on sprite silhouettes
- **Parallax-aware anti-aliasing** at layer edges

### 7.3 When to Pursue This

Path B is an enhancement over the `SDL_RenderGeometry` approach — not a prerequisite. The CPU-projected path (Phase 4) produces a fully working diorama. This phase adds visual polish.

**Recommended trigger:** pursue once the basic diorama is working and you want to add post-processing effects that can't be expressed as SDL blend modes.

---

## 8. Edge Cases & Hazards <a name="8-edge-cases"></a>

### 8.1 Color Math in Diorama Mode

Main and subscreen are two independently priority-resolved operands, not two
mutually exclusive scene definitions. TM (`$212C`) and TS (`$212D`) select
BG1-BG4/OBJ membership; TMW/TSW apply their windows; CGWSEL (`$2130`) chooses
the fixed colour or resolved subscreen as the second operand and controls colour
windows/direct colour; CGADSUB (`$2131`) selects source layers plus add/subtract
and half mode.

Marahna is the decisive action-stage example. Both `0501` gf2331 and `0502`
gf9728 record TM=`$06` (BG2+BG3), TS=`$11` (BG1+OBJ),
CGWSEL=`$02`, CGADSUB=`$03`. BG1 and OBJ are therefore visible only as the
resolved subscreen input to full addition. Capturing TM alone loses the playable
layer and sprites; capturing TM and TS as ordinary opaque planes loses the
water/detail created by the arithmetic.

The shipped separated-plane path supports only forms with an exact
representation:

| Native form | Admission | Representation |
| --- | --- | --- |
| Subscreen half-add | addend is TS, add not subtract, half set; selected BG is not itself on TS | alpha `$80` source-over; OBJ remains per palette group |
| Disjoint subscreen full-add | CGWSEL exactly `$02`; no half/subtract; TM/TS visual masks disjoint; a main source enables math | PPU-resolved sparse TS winner planes, saturated additive host pass |
| Full fixed-colour BG subtraction | CGWSEL zero; subtract set, half clear; nonzero fixed colour; selected BG | bake subtraction in SNES 5-bit space before brightness expansion |
| INIDISP master brightness | any supported scene | already baked into captured RGB |

The full-add PPU filter is per pixel, not a frame-level source-mask shortcut. It
reconstructs main and subscreen winners after windows/priority, keeps a TS pixel
only when that exact source wins, and checks that the main winner enables math.
BG3 is reinserted after the additive world; relocated HUD OAM is excluded from
the addend scratch. The immutable additive plane mask crosses the game/present
thread boundary in `FrameSlot`.

Overlapping TM/TS ownership is deliberately rejected by the full-add classifier:
the two screens may use different windows, while one isolated source buffer
cannot encode both renderings. Direct colour, colour-window math/prevention,
half-subtract, general subtraction, and fixed-colour addition likewise have no
generic separated-plane representation today. “Fail closed” here means no
guessed transform is applied; it does not prove visual equivalence. A newly
observed unsupported action frame must be measured and either receive a proven
capture policy or use the authentic flat composite for that frame.

Fades retain the same qualification. INIDISP fades are safe because brightness
is in RGB. A COLDATA/CGADSUB fade is safe only if it matches a supported form
above; fixed-colour addition is still an explicit action-stage acceptance risk.
Never infer fade safety solely from the fact that ordinary flat scanout looks
correct.

The durable implementation/evidence reference is rendering-engine.md §13.4;
the ownership boundary is recorded in SEAMS.md.

### 8.2 Per-Tile and OBJ Priority Split

The original single-plane simplification has been superseded. The PPU overlay
API now binds priority-band surfaces. BG1 and BG2 each split priority-0 and
priority-1 tiles; OBJ splits priorities 0-3. The source z-buffer decides the
winner first, then routes that winning pixel to exactly one band, so overlapping
sprite parts cannot appear in several planes.

The Diorama plane order reproduces the Mode-1 interleave:
OBJ0/OBJ1 behind the playfield, BG2-low, BG1-low, OBJ2, BG2-high, BG1-high,
OBJ3, then BG3. Authored depth/rake overrides may change presentation geometry,
but the capture identity remains explicit (`bg1`, `bg1hi`, `bg2`,
`bg2hi`, and `obj0`-`obj3`).

A full-add source marks all of its priority bands in
`diorama_plane_additive_mask`; each band is already sparse at pixels that lost
the native subscreen resolve. This keeps priority splitting and the Marahna
colour-math handoff orthogonal rather than collapsing either one.

### 8.3 Window Effects — TWO kinds, only one is baked into captures

SNES windows come in two flavors that behave DIFFERENTLY under capture. They are
NOT a single "windows just work" case:

- **Per-layer visibility windows** (a window disables a specific BG/OBJ inside a
  region): baked into the capture correctly. The layer's pixel is gated during the
  draw itself (`layerActive` via `IS_SCREEN_WINDOWED`/`ppu_getWindowState`,
  ppu_old.c:149-151, and the new-path equivalent), so a windowed-out region is
  simply *transparent* in that layer's capture buffer — exactly what we want for a
  separate plane. This is the "windows" the overlay header contract refers to
  (ppu.h:363-365).
- **The color/math window** (window index 5: clip-to-black regions and prevent-math
  regions): **NOT baked into captures.** The clip-to-black is applied via
  `clip_color_mask` in the *composite* loop (ppu.c:1428, 1449-1451; old path
  ppu_old.c:84-93), which writes the flat framebuffer. `PpuOverlayColor` never
  applies it. So a color-window spotlight/iris/clip effect would be absent from the
  captured layers — the clipped region would show the layer's normal pixels in
  diorama mode instead of black.

**Action required (on-target check):** determine whether ActRaiser action stages use
the color window (iris transitions, spotlight reveals, boss-intro clips). If they
do, diorama mode must fall back to the flat path for those frames, or replicate the
clip in the compositor. Per-layer visibility windows need no handling. Until
verified, do not claim the color window is handled.

### 8.4 Non-action presentation — separate pipelines

The original plan incorrectly grouped every simulation/overworld screen under
Mode 7. The current map is:

- town simulation views are ordinary PPU Mode 1 and may use the independent
  Sim3D ground, semantic billboard, weather, lighting, and HUD pipeline;
- world navigation is Mode 7 and may use its independent forced-top-down
  full-plane 3D scene;
- Sky Palace, menus, title, and other screens use their own flat/overlay paths.

`Diorama_IsActiveThisFrame` remains action-map-only. This action capture policy
must never bind its BG/OBJ surfaces in those other branches, even when a
presentation setting named “Diorama” or shared visual effects are enabled. The
non-action systems publish their own immutable `FrameSlot.sim` data and do not
reuse the action layer order or additive-plane mask.

`g_m7_overlay_pixels` is an HD Mode-7 replacement surface, not a generic town
floor. The shipped town renderer derives its scene from Mode-1 captures plus
semantic state; world-navigation 3D owns its complete Mode-7 plane separately.
See rendering-engine.md §13b/§13h and the simulation/world-navigation specs.

### 8.5 Settings Overlay / Scene Inspector

These draw directly via SDL_Render calls. In diorama mode, they render AFTER the layer projection, flat against the screen (at Z=infinity, no perspective). The present thread calls them after `RenderDiorama()`, same as the flat path calls them after `RenderFramebuffer()`.

### 8.6 Screenshots (WriteFramebufferPpm / SDL_RenderReadPixels)

**Content:** the PPM capture is intended to show the FLAT authentic composite
regardless of diorama mode (diagnostic screenshots should reflect real game output);
the BMP hotkey (`SDL_RenderReadPixels`) captures what's on screen — the 3D
projection while diorama is active. Those content behaviors are the desired ones.

**Threading (correction — this is NOT a plain `g_pixels` read):**
`WriteFramebufferPpm` (main.c:584) actually runs the whole `RenderFramebuffer()`
(main.c:589) — a full `SDL_UpdateTexture`/`RenderClear`/`RenderTexture`/
`RenderReadPixels` pass — and only falls back to reading `g_pixels` directly when
there is no renderer (headless). It is reachable during live play (F2 /
`AR_SHOT_*`). Once presentation is threaded this render pass MUST be routed to the
present thread, not issued from the game thread. See §2.9(c) — this section
describes only the intended *content*, not a safe threading model.

### 8.7 Input Mapping in Diorama Mode

Mouse-click hit-testing (scene inspector) uses `WindowPointToOutput` to map screen coordinates to SNES pixel space. In diorama mode, a screen-space click must be unprojected through the camera matrix to find which layer/pixel it hits. This requires ray-casting against each layer's projected quad.

**v1:** Disable scene inspector click-to-inspect in diorama mode (it's a debug tool; disabling is fine).

**v2:** Implement ray-plane intersection for each layer. The clicked point defines a ray from the camera through the screen pixel; test intersection against each layer's Z-plane in back-to-front order; the first layer with a non-transparent pixel at the intersection point is the hit.

### 8.8 Layer capture only exists in specific PPU modes

Overlay capture is implemented **only in the new PPU path**, and within it only
the Mode 1 and Mode 7 branches of `PpuDrawBackgrounds` wrap layers in
`PpuBeginBackgroundOverlay`/`PpuFinishBackgroundOverlay` (ppu.c:1324-1376). There
is no Mode 0 branch, and **BG4 is never drawn in any branch**, so
`kPpuOverlaySource_Bg4` is effectively dead.

For the action-only scope this is fine: ActRaiser action stages are Mode 1, which
captures BG1/BG2/BG3+OBJ — exactly the layers the diorama uses. The plan's layer
list (§5.2) deliberately excludes BG4. Do **not** add BG4 to the capture set; it
would never receive pixels and would waste a full-frame buffer + texture upload.

**Precondition to assert at startup:** the diorama requires the new PPU path
(`g_new_ppu`). Since action stages in the shipping widescreen config already force
`g_new_ppu = true` (main.c:973/1023, via `g_ws_active`), this holds in practice.
But if a user runs with widescreen off AND `new_renderer` off, action stages use
the OLD PPU path, which has no capture support — the diorama would see empty layer
buffers. Gate the diorama toggle on `g_new_ppu` (see §10.6) and show a message if
unavailable rather than presenting a black/empty diorama.

---

## 9. Testing Strategy <a name="9-testing"></a>

Test groups are labeled by architectural Phase; the milestone crosswalk is in
Appendix A (M0-M3 = Phases 3+4 diorama; M5 = Phase 1; M6 = Phase 2; M7 = Phase 5).

### 9.1 Present Thread Tests (Phase 1 / milestone M5)

| Test | Verifies |
|------|----------|
| Build with `AR_PERF=1`, confirm `[perf]` frame times drop (RtlRunFrame no longer includes vsync wait) | Game thread is decoupled from vsync |
| Turbo mode (`T` key): confirm fps counter exceeds 60 | Turbo no longer blocked by present |
| Resize window during gameplay | No crash, no tearing, no deadlock |
| Open/close settings overlay | Overlay renders correctly on present thread |
| `AR_SHOT_AT_GF=100` screenshot | File written correctly from present thread |
| Kill window (X button) while game is mid-frame | Clean shutdown, no hang on `SDL_WaitThread` |

### 9.2 Fixed-Timestep Tests (Phase 2 / milestone M6)

| Test | Verifies |
|------|----------|
| Run on a 144Hz display: game speed is identical to 60Hz | Accumulator pacing works |
| `AR_PERF=1` reports fps=60 steady-state on any display | Game rate independent of display |
| Pause for 5 seconds, unpause: no burst of catch-up frames | Accumulator reset on unpause |
| Minimize window for 30s, restore: smooth resume | `kMaxCatchupFrames` cap works |
| Turbo: `[perf]` shows >60 game-fps | Turbo bypass works with accumulator |

### 9.3 Layer Capture Tests (Phase 3 / milestones M0-M1)

| Test | Verifies |
|------|----------|
| Enable diorama mode, verify each layer buffer has non-zero pixels | Capture policy fires |
| **In a sprite-heavy action frame, verify the OBJ capture buffer is non-empty AND the backdrop has no sprite pixels** | **OAM range 0..128 set (regression guard for the `oamCount==0` → sprites-bake-into-backdrop bug, §4.2)** |
| Verify `g_pixels` contains only the backdrop color when all layers captured | RemoveFromGame works |
| Verify every opaque captured pixel has alpha==0xFF and every empty position alpha==0x00 | Presence→binary-alpha synthesis (§5.8) |
| Compare flat-composite (diorama off) to manual straight-alpha over of all layer buffers + opaque backdrop | Layer separation is lossless (minus color math) |
| Toggle `g_new_ppu` and compare rendered output for several ActRaiser scenes | New PPU matches old PPU |
| Run with BG2/BG3/OBJ disabled (bgmode register) — respective capture buffers empty | Layer-enable detection works |

### 9.4 Diorama Renderer Tests (Phase 4 / milestones M2-M3)

| Test | Verifies |
|------|----------|
| Diorama mode shows all layers with visible Z separation | Projection works |
| Rotate camera: layers parallax (BG2 moves more than BG1) | Depth assignment correct |
| Toggle individual layers off in settings: they disappear | Visibility controls work |
| Reset camera: returns to default tilt | Reset logic works |
| Run in software renderer (`SDL_SOFTWARE_RENDERER`): still works | No GPU-only dependency in Path A |
| Compare center pixel with flat mode: RGB matches | Projection doesn't color-shift |

### 9.5 Interpolation Tests (Phase 5 / milestone M7)

**Mechanical proxies (the gate — not "looks smoother").** The interpolation lerp
(§6.2) is pure math; test it the way D17 tests the projection math. Instrument the
present thread to log a `[interp]` line (t, per-layer du/dv) and assert:

| Test | Mechanical pass condition |
|------|---------------------------|
| **Stability on a static scene** | When `prev->bg_hscroll[i] == curr->bg_hscroll[i]` (and vscroll) for a layer, the computed `du==dv==0` at every `t` (the §6.2 formula collapses to the current offset), so the rendered mesh byte-equals the non-interpolated mesh. NOTE: assert on `prev==curr`, NOT on `t==0` — at t==0 the formula gives `h=prev`, so `du=(prev-curr)/width` which is nonzero unless prev==curr. |
| **Active + parallax ratio** | On a scripted horizontal scroll, `t` sweeps 0→1 monotonically between two game frames, and `\|du(BG2)\| < \|du(BG1)\|` (the slower parallax plane moves less, §6.3). |
| **Wrap** | Force BG2 hscroll 1023→0 (10-bit modular, §6.1); assert the `abs(curr-prev) > 512` branch (§6.4) fires and the interpolated per-frame delta magnitude is `< 512` (modular), not ~1023. |
| **Determinism unchanged** | Interpolation is present-only. Re-run the M6 `$0088` replay-diff; assert still zero game-state divergence. |
| Mode change (Mode1↔Mode7 via `bg_mode`) | On a mode change between frames, set `t=0` (no lerp across incompatible states); assert no interpolation applied that frame. |

PASS = all asserts. "Visibly smoother on a 120Hz display" is retained only as a
non-gating sanity note, not the acceptance criterion.

---

## 10. Settings Integration <a name="10-settings"></a>

### 10.1 New Settings Fields

Add to `Settings` struct (`settings.h:135`):

```c
// After the existing widescreen block:

/* Diorama 3D presentation mode — ACTION STAGES ONLY (see Scope banner).
 * Renders each Mode-1 PPU layer as a separate plane in a pseudo-3D
 * perspective view. Requires the new PPU path (g_new_ppu); inert in sim mode. */
bool diorama_mode;

/* Camera params. NOTE: the settings system has NO float type — the type enum
 * is Bool/Int/Enum/Mask/Custom/Action only (settings.h:49-54). Store camera
 * angles as SCALED INTEGERS (milliradians) and distance as hundredths, then
 * convert to float at use. This keeps them serializable by the existing
 * kSettingType_Int path with no new machinery. */
int diorama_tilt_x_mrad;       // camera pitch, milliradians  (e.g. 150 = 0.15 rad)
int diorama_tilt_y_mrad;       // camera yaw,   milliradians
int diorama_distance_x100;     // camera distance * 100        (e.g. 500 = 5.0)

/* Per-layer visibility. Action-stage Mode 1 has no BG4 (never captured — §8.8),
 * so there is no diorama_layer_bg4. */
bool diorama_layer_bg1;
bool diorama_layer_bg2;
bool diorama_layer_bg3;
bool diorama_layer_obj;
bool diorama_layer_backdrop;

/* Presentation rate decoupling (mode-agnostic — whole game). */
bool uncapped_framerate;       // present at display refresh (vs. game-locked 60Hz)
bool scroll_interpolation;     // interpolate BG scroll at >60Hz
```

**Why scaled ints, not floats:** verified against `settings.h:49-54` — the
`SettingDesc` type enum is `{Bool, Int, Enum, Mask, Custom, Action}`. There is no
float type and no float precedent in `g_setting_descs[]`. Adding one would mean a
new type in the parser, formatter, and serializer. Scaled ints reuse the existing
`kSettingType_Int` path (with `Settings_SetLong`/`Settings_GetLong`, settings.h:266-267)
for free. Milliradian precision (0.001 rad ≈ 0.057°) is far finer than any
perceptible camera step. Alternatively, use `kSettingType_Custom` with a
formatter if you want the menu to display "8.6°" — Custom already supports
arbitrary get/set/format hooks.

### 10.2 Setting Descriptors

The codebase does NOT write raw `SettingDesc` struct literals — it uses the
`BOOL_SETTING`/`INT_SETTING`/`ACTION_SETTING` macros (settings.c:491-509). Add
these exact rows to `g_setting_descs[]` in `settings.c`. Signatures for reference:
`BOOL_SETTING(id, env, label, help, cat, def, sticky, available_fn, changed_fn)`;
`INT_SETTING(id, env, label, help, cat, def, lo, hi, parser_fn, available_fn)`;
`ACTION_SETTING(id, label, help)` (forces `kSettingCat_Extras` — see §10.3 for the
category caveat).

```c
/* Presentation category — requires kSettingCat_Presentation added to the enum,
 * Settings_CategoryName, and kCategoryOrder[] (see §10.3). If you instead fold
 * into Display (§10.3 option 2), use kSettingCat_Display below. */

/* Diorama master + rate toggles. Diorama rows gate on new-PPU capability
 * (Diorama_NewPpuCapable, §D14); the rate/inert-control availability fns are
 * per §D15. env=NULL means no boot-env seed. */
BOOL_SETTING(diorama_mode, NULL, "Diorama 3D", 
             "Render action-stage layers as tilted 3D planes (action stages only; needs new renderer).",
             kSettingCat_Presentation, 0, false, Diorama_NewPpuCapable, OnDioramaModeChanged),
BOOL_SETTING(uncapped_framerate, NULL, "Uncapped FPS",
             "Present at display refresh rate; game logic stays at 60Hz.",
             kSettingCat_Presentation, 0, false, NULL, NULL),
BOOL_SETTING(scroll_interpolation, NULL, "Smooth scrolling",
             "Interpolate background scroll between game frames at >60Hz.",
             kSettingCat_Presentation, 0, false, NULL, NULL),

/* Camera params as scaled ints (no float SettingType — §10.1). Ranges are final:
 * tilt +/-0.7 rad (~40 deg) in milliradians; distance 2.0..20.0 in hundredths.
 * NULL parser = plain integer parse. available = diorama_mode is on. */
INT_SETTING(diorama_tilt_x_mrad, NULL, "Camera pitch",
            "Diorama camera tilt (milliradians).",
            kSettingCat_Presentation, 150, -700, 700, NULL, Diorama_ModeIsOn),
INT_SETTING(diorama_tilt_y_mrad, NULL, "Camera yaw",
            "Diorama camera yaw (milliradians).",
            kSettingCat_Presentation, 0, -700, 700, NULL, Diorama_ModeIsOn),
INT_SETTING(diorama_distance_x100, NULL, "Camera distance",
            "Diorama camera distance (x100).",
            kSettingCat_Presentation, 500, 200, 2000, NULL, Diorama_ModeIsOn),

/* Per-layer visibility. available = diorama_mode is on. */
BOOL_SETTING(diorama_layer_bg1, NULL, "Show BG1", "Diorama: show the BG1 plane.",
             kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
BOOL_SETTING(diorama_layer_bg2, NULL, "Show BG2", "Diorama: show the BG2 plane.",
             kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
BOOL_SETTING(diorama_layer_bg3, NULL, "Show BG3/HUD", "Diorama: show the BG3 (HUD) plane.",
             kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
BOOL_SETTING(diorama_layer_obj, NULL, "Show sprites", "Diorama: show the sprite plane.",
             kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
BOOL_SETTING(diorama_layer_backdrop, NULL, "Show backdrop", "Diorama: show the backdrop plane.",
             kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
```

Also add the two availability helpers (in `settings.c` or `diorama.c`, declared in
a header both see):
```c
bool Diorama_ModeIsOn(const SettingDesc *d)     { (void)d; return g_settings.diorama_mode; }
bool Diorama_NewPpuCapable(const SettingDesc *d) { (void)d; return g_settings.new_renderer || g_ws_active; }
```
And the "Reset Camera" action (see §10.3 for why a plain `ACTION_SETTING` lands in
Extras and how to place it in the Presentation section instead).

### 10.3 Settings Overlay Section

Add a "Presentation" or "Diorama" section to the overlay menu (rendered by `settings_overlay.c`). Layout:

```
┌─ Presentation ───────────────────────────┐
│ Display Mode:   [4:3 / Wide / Full]       │
│ Uncapped FPS:   [OFF / ON]                │
│ Scroll Smooth:  [OFF / ON]                │
│                                           │
│ ── Diorama (action stages only) ──        │
│ Enable:         [OFF / ON]  (needs new PPU)│
│ Layers: [✓BG1] [✓BG2] [✓BG3] [✓OBJ]      │
│ Tilt / Distance [camera controls]         │
│ Reset Camera    [action]                  │
└───────────────────────────────────────────┘
```

Show a subtitle or disabled-state hint clarifying diorama is action-only, so a
user who enables it in the town screen understands why nothing changed.

**The menu grouping requires an enum change — not just "add a section".** The
overlay groups rows by the fixed `SettingCategory` enum `{Cheats, Widescreen,
Display, Audio, Save, Extras, Inspector}` (settings.h:65-73, named by
`Settings_CategoryName`, settings.c:1150) and iterates them via `kCategoryOrder[]`
(settings_overlay.c:147). There is **no Presentation/Diorama category**, and
`ACTION_SETTING` hardcodes `kSettingCat_Extras` (settings.c:499-502) — so a "Reset
Camera" action built with it lands in the Extras menu, not this panel. Two options:
1. Add `kSettingCat_Presentation` to the enum + `Settings_CategoryName` +
   `kCategoryOrder`, and add a dedicated action macro that uses it (since
   `ACTION_SETTING` forces Extras). Confirm `Settings_IsMenuVisible`
   (settings.c:940-980) needs no special-case for the new keys.
2. Or place all diorama rows under the existing `kSettingCat_Display` and drop the
   separate header. Simpler; loses the dedicated "Diorama" grouping.

### 10.4 Configuration Persistence

These settings serialize to `settings.ini` like all existing settings. The camera position (tilt/distance) persists across sessions so the user's preferred angle is remembered.

### 10.5 Hotkey

| Key | Action |
|-----|--------|
| `D` | Toggle diorama mode on/off (with animated transition). No-op outside action stages. |
| `1`-`4` (with diorama active) | Toggle layer visibility (BG1, BG2, BG3, OBJ). No BG4. |

**Hotkey conflict check (verified against main.c:2206-2281):** `P` (pause), `T`
(turbo), `S`/`A`/`Z`/`X`/`Q`/`W` (mapped to SNES buttons via `HandleInput`,
main.c:266-271), and the F-keys are all taken. `D` is currently free. Digits `1`-`4`
are NOT currently bound as game inputs (only the letters above map to buttons), so
using them for layer toggles is safe — but gate them behind "diorama active" so
they stay available for any future use.

### 10.6 Availability gating (diorama requires new PPU + action stage)

Two gates, both using existing mechanisms:

1. **Menu availability** — the diorama settings rows should be greyed out / show
   "requires new renderer" when `!(g_settings.new_renderer || g_ws_active)`. Use
   the existing `SettingDesc` availability hook (`Settings_IsAvailable`,
   settings.h:264) — the same mechanism that already gates widescreen-dependent
   rows. Return false when the new PPU path can't be active.

2. **Runtime engagement** — even with the toggle ON, the renderer only engages
   when `ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])` is true (§4.2).
   Sim mode, title, world map, and menus present flat. The `D` hotkey may be
   pressed anywhere, but toggling it on outside an action stage simply arms it;
   the diorama appears when the player next enters an action stage.

**User feedback:** when `D` is pressed but the new PPU path is unavailable, print
a one-line stderr message (mirroring the F9 pattern at main.c:2254-2256: "F9 needs
ExtendedAspectRatio…") rather than silently doing nothing.

---

## Appendix A: Demo-Driven Milestone Roadmap

The numbered Phases (§2-§7) are an *architectural* decomposition — they group work
by subsystem, not by demoable value. Two of them (Phase 1 present thread, Phase 2
fixed timestep) are large internal refactors that produce **no visible change** on
their own, so sequencing strictly by phase would mean a long stretch with nothing
to show.

This roadmap re-sequences the same work into **milestones that each end in
something you can put on screen and get a reaction to.** The key insight driving
the order:

> **The diorama visual does NOT require the threading work.** Layer capture runs in
> the existing synchronous present path (the new PPU path is already active in the
> shipping widescreen config), and the projection is just draw calls. So build and
> demo the whole shadowbox effect FIRST, synchronously, then optimize the pipeline
> underneath it with threading. Building diorama-first also sidesteps the entire
> §2.9 threading-hazard cluster during the visually interesting milestones — those
> hazards only exist once presentation moves off-thread (M6+).

Each milestone below lists: **Demo** (what you show), **Depends on**, **Touches**
(plan sections / risks), and rough **Effort**.

### M0 — Layer-capture PNG dump (proof the premise)  ·  Effort: 0.5-1 day
- **Demo:** run an action stage, press a debug key, and get `bg1.png`, `bg2.png`,
  `bg3.png`, `obj.png`, `backdrop.png` on disk — each layer cleanly separated with
  correct transparency. "The PPU really can hand us every layer as its own image."
- **Value:** validates the single riskiest assumption (lossless per-layer capture,
  incl. the OBJ `PpuSetOverlayOamRange(0,128)` requirement and alpha model) before
  any rendering or threading work. If a layer comes back empty or wrong, you learn
  it here for pennies.
- **Depends on:** nothing (new PPU path already on in widescreen).
- **Touches:** §4.0-§4.4 (capture policy), §5.8 (alpha), §8.8. Exercises the
  sprite-OAM-range fix and confirms color-math/window/fade losses (§8.1/§8.3) by eye.
- Reuses existing `WriteFramebufferPpm`-style dumping; no SDL render changes.

### M1 — Flat multi-plane recomposite (identity transform)  ·  Effort: 1-2 days
- **Demo:** the game looks **pixel-identical to today**, but is now drawn by
  compositing the separate layer textures back-to-front (identity transform) instead
  of blitting the single baked framebuffer. A/B toggle proves it's lossless.
- **Value:** proves the capture → separate-texture → recomposite pipeline round-trips
  with no visual regression. This is where the backdrop `BLENDMODE_NONE` fix and the
  per-plane blend modes (§5.8) get shaken out — still fully synchronous, so no races.
- **Depends on:** M0.
- **Touches:** §5.5 render loop (identity mesh), §5.8 alpha model. Still single-threaded.

### M2 — Static diorama tilt (the "wow")  ·  Effort: 2-3 days
- **Demo:** flip a toggle and the layers separate into a tilted 3D shadowbox — the
  headline effect. Fixed camera angle, no interactivity yet.
- **Value:** the feature is now visible and sellable. Achieved with **zero threading
  work** — runs in the synchronous present path (vsync-blocked, but that's invisible
  in a 60fps demo).
- **Depends on:** M1.
- **Touches:** §5.2 (Z table), §5.3 (projection), §5.4 (mesh subdivision), §5.9
  (PAR/crop). Action-stage gating (§4.2 scope banner).

### M3 — Interactive camera + layer toggles + settings  ·  Effort: 2-3 days
- **Demo:** right-drag to orbit the diorama, scroll to zoom, number keys to
  show/hide individual layers, `D` to toggle the mode; angle persists across runs.
- **Value:** turns the static novelty into something a user plays with. First point
  where the settings UI work lands.
- **Depends on:** M2.
- **Touches:** §5.6 (camera controls), §5.7 (layer UI), §10 (settings — scaled-int
  camera fields, new-PPU gate, menu category). §8.7 (input mapping: v1 disables
  click-inspect in diorama).

> **Milestone gate:** at the end of M3 the entire diorama feature ships and demos on
> the CURRENT synchronous render loop. Everything below is performance/quality
> optimization of the pipeline beneath it, and can be scheduled independently.

### M4 — Instrumented baseline (measure before you optimize)  ·  Effort: 0.5 day
- **Demo:** an `AR_PERF`-style HUD line showing per-frame game-ms vs present-ms vs
  vsync-wait, on the current synchronous loop. "Here's the vsync wall we're about to
  remove."
- **Value:** establishes the numbers M5/M6 are judged against; nothing to optimize
  blindly.
- **Depends on:** none (can be done anytime; placed here because it motivates M5).

### M5 — Present thread (decouple from vsync)  ·  Effort: 4-6 days (highest risk)
- **Demo:** turbo mode visibly runs faster; the M4 HUD shows game-ms no longer
  includes the vsync wait; window stays responsive during heavy frames.
- **Value:** frees the game thread from vsync. This is the big refactor — and now
  it's a *measurable optimization of a working feature*, not a prerequisite for
  seeing anything.
- **Depends on:** M1 (the render path it moves off-thread).
- **Touches:** §2 in full, and critically **all of §2.8 + §2.9** (snapshot every
  live `g_ppu`/`g_settings` read; command-queue the settings-handler/savestate/
  screenshot renderer mutations; pointer-double-buffer the framebuffers + m7/HD
  overlays). This is where the 7 high-severity threading findings live.

### M6 — Fixed-timestep game loop  ·  Effort: 2-3 days
- **Demo:** run on a 120/144Hz monitor — game speed is identical to 60Hz; the
  present rate is decoupled from the tick rate.
- **Value:** correct game speed on any display; enables >60Hz presentation.
- **Depends on:** M5.
- **Touches:** §3 in full, esp. §3.5 (port ALL per-frame housekeeping with correct
  per-tick/per-iteration placement), §3.6 (headless), §3.7 (record/replay).

### M7 — High-framerate scroll interpolation  ·  Effort: 3-4 days
- **Demo:** on a 120Hz+ display, scrolling backgrounds and the diorama camera are
  visibly smoother than 60fps; in diorama mode each layer parallaxes at its own rate.
- **Value:** the payoff of M5+M6 — the diorama specifically benefits (camera + per-
  layer scroll interpolate cheaply as vertex/UV updates).
- **Depends on:** M6 (present cadence >60Hz) + M2 (planes to interpolate).
- **Touches:** §6 (esp. §6.1 `m7matrix[4]/[5]` fix, §6.4 UV-clamp edge case).
- **Objective acceptance (not just "looks smoother"):** (1) with interpolation OFF,
  log the presented BG1 scroll offset each present — on a 120Hz display it repeats
  each value ~2× (stepped); with interpolation ON, consecutive presented offsets
  differ by ~half the per-tick delta (monotonic sub-steps). Assert via a logged
  `[interp]` line, not by eye. (2) A static scene (no scroll change) shows ZERO
  positional jitter across presents (interpolation `t` clamps, offsets identical).
  (3) Frame-exact game state is unchanged vs M6 (interpolation is present-only):
  re-run the M6 `$0088` replay-diff, assert still zero divergence.

### M8 — GPU shader path (optional polish)  ·  Effort: 3-5 days
- **Demo:** per-layer depth-of-field blur, drop shadows between planes, rim lighting.
- **Value:** visual richness beyond what `SDL_RenderGeometry` blend modes allow.
- **Depends on:** M2 (a working diorama to enhance).
- **Touches:** §7 (illustrative sketch only — see the §7.1 banner). Optional; the
  `SDL_RenderGeometry` path (M2) is fully sufficient without it.
- **Objective acceptance:** this milestone is aesthetic and has no pass/fail gate
  beyond "the effect renders without regressing M2's correctness." Required guard:
  toggling the shader OFF must produce output byte-identical to M2 (the shader is
  additive polish, not a new render path). No other objective criterion — ship it
  when it looks right to you. Explicitly the ONLY milestone without a mechanical
  acceptance test, by nature.

### Two independent tracks

```
VISUAL track (ships the feature, no threading):
  M0 dump → M1 recomposite → M2 static tilt → M3 interactive  ── feature complete & demoable

OPTIMIZATION track (makes it fast/smooth, all the threading risk):
  M4 baseline → M5 present thread → M6 fixed timestep → M7 interpolation

  M8 GPU shaders  ── optional, hangs off M2
```

The two tracks share only M1 (M5 moves M1's render path off-thread). A single
developer would do M0→M3 first (fast wins, demoable feature), then M4→M7 (the hard
refactor, now de-risked because it optimizes something already working). Two
developers could run the tracks in parallel after M1, but M5 must re-integrate
whatever M2/M3 added to the render path.

**Architectural dependency graph (reference):**
```
Phase 1 Present Thread ──┬─→ Phase 2 Fixed Timestep ─→ Phase 5 Interpolation
                         │
Phase 3 Layer Capture ───┴─→ Phase 4 Diorama ─→ Phase 6 GPU Shaders
```
Note the graph says Phase 4 "requires Phase 1," but that is only true for the
*threaded* end state — Phase 4 renders correctly on the synchronous loop first
(M2/M3), and Phase 1 (M5) later moves it off-thread. The milestone order encodes
that nuance; the phase graph does not.

---

## Appendix B: File Change Map

| File | Milestones | Nature of changes |
|------|-----------|-------------------|
| `src/actraiser/actraiser_rtl.c` | M0, M2 | Diorama capture policy in `ActRaiserDrawPpuFrame` (§4.2) |
| `src/diorama/diorama.c` (NEW) | M1-M3, M7, M8 | Layer buffers, camera model, projection, mesh, interpolation |
| `src/diorama/diorama.h` (NEW) | M1-M3, M7 | Public types (DioramaCamera, DioramaLayer, FrameScrollState) |
| `src/main.c` | M0-M7 | Debug dump (M0); recomposite + diorama render pass (M1-M3); present thread + FrameSlot + §2.9 command queue (M5); loop restructure (M6) |
| `src/settings.h` | M3, M6 | New fields (scaled-int camera, toggles) |
| `src/settings.c` | M3, M6 | New descriptors + `kSettingCat_Presentation` (§10.3) |
| `src/settings_overlay.c` | M3 | New Presentation/Diorama menu section |
| `CMakeLists.txt` | M1 | Add `src/diorama/diorama.c` to build |

(Milestone→Phase map: M0-M3 = Phases 3+4; M4 = instrumentation only, no phase;
M5 = Phase 1; M6 = Phase 2; M7 = Phase 5; M8 = Phase 6.)

---

## Appendix C: Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| **Present-time render functions read live `g_ppu`/`g_settings`, not the snapshot** (root cause of most threading races) | Certain if unaddressed | High (data race/corruption) | §2.9(d): pass `const FrameSlot*` through every `Render*`; poison `g_ppu` in the present TU; snapshot all read fields |
| **Settings-change handler mutates renderer/window/PPU surfaces on the game thread** (fullscreen, logical presentation, `g_pixels` rebind) | Certain if unaddressed | High (renderer data race) | §2.9(a): command-queue renderer mutations onto the present thread, or quiesce present before geometry change |
| **Savestate F7 rewrites all of `g_ppu` on the game thread** while present reads it | Medium | High (torn frame/UB) | §2.9(b): gate `RtlLoadSnapshot` behind the present handshake |
| **§3.1 loop drops per-frame housekeeping** (autosave, music tick, record/replay, SPC upload) | Certain if implemented literally | High (save loss / audio / oracle break) | §3.5: port every item with explicit per-tick vs per-iteration placement |
| **`g_m7_overlay_pixels` (~6.4 MB) not double-buffered** — present reads while game rewrites | Medium | Medium | §2.9(e): pointer double-buffer m7 + HD overlays, not memcpy |
| Screenshot/PPM runs full render pass on game thread | Medium (F2/AR_SHOT) | Medium | §2.9(c): move capture to present thread |
| Diorama buffers collide with HD replacements + HUD (shared `g_hd_overlay_pixels`, single capture slot) | Certain if reused | Medium | §4.3: dedicated diorama buffer set; mutually exclude HD/HUD while diorama on |
| `§6.1` reads nonexistent `m7xCenter`/`m7yCenter` (compile error) | Certain | Medium | §6.1: use `m7matrix[4]`/`[5]` |
| No `SettingCategory` for a Presentation/Diorama menu; `ACTION_SETTING` forces Extras | Certain if section added | Medium | §10.3: add `kSettingCat_Presentation` (+ action macro) or fold into Display |
| Diorama bypasses PAR (7:6) stretch + VisibleX0/Width crop | Certain | Medium | §5.9: scale projection X by PAR; restrict layer UV to visible sub-rect |
| Present reads `g_settings` display/aspect mutated live | Medium | Medium | §2.9(d)/§5.9: snapshot display_mode/pixel_aspect/ws_extra into FrameSlot |
| Settings overlay statics raced both directions (nav + debug-panel rects) | Low (paused-only) | Low | Snapshot nav state; compute drag rects on game thread |
| Scene-inspector / inspector-info-panel read live `g_ram`/`g_ppu` from present | Low (paused-only) | Low | Snapshot inspector selection + WRAM bytes into FrameSlot |
| New PPU path visual differences for ActRaiser | Medium | High (blocks Phase 3) | Test with ROM first; fix new-PPU bugs or add overlay to old path |
| SDL3 `"gpu"` renderer not available on all platforms | Low | Low (Phase 6 only; Path A doesn't need it) | GPU path is optional; `SDL_RenderGeometry` works on all backends |
| Present thread deadlock on window close | Medium | Medium | Careful shutdown sequence (§2.6); timeout on condition wait |
| Audio dropout from lock contention with faster game loop | Low | Low | Already mitigated by existing 256-batch APU lock design; Phase 2 doesn't change lock behavior |
| Unsupported colour-math form appears in Diorama mode | Unknown | Medium | §8.1: supported half-add, disjoint full-add, and fixed-colour-subtract states have exact policies; measure any other register topology and add a proven policy or use the authentic flat composite for that frame |
| **COLDATA fixed-colour-add fade appears in action Diorama** (INIDISP brightness and the measured fixed-colour subtraction are reproduced; general fixed-colour addition is not) | Unknown | Medium | Log the complete TM/TS/TMW/TSW/CGWSEL/CGADSUB/COLDATA state; fall back to flat present or implement the native 5-bit operation only after a live fixture pins it |
| **Color-window clip-to-black lost in diorama** (per-layer visibility windows ARE baked; the color/math window index 5 is composite-time only) | Unknown (needs ROM check) | Medium | §8.3: verify action-stage color-window use (iris/spotlight); fall back to flat path or replicate clip in compositor |
| Phase-5 UV interpolation wraps opposite-edge content | Low | Low | §6.4: set `SDL_TEXTURE_ADDRESS_CLAMP` for game planes or clamp shifted UVs |
| HDMA-heavy scenes (Mode 7 title, wavy effects) look wrong at >60Hz | Medium | Low | Interpolation only shifts whole-frame scroll; HDMA baked into pixels; visual is "correct at 60fps, static between" |
| OAM slot identity tracking for sprite interpolation | High (complex) | Low (defer) | Sprites are not interpolated in the initial implementation; acceptable |
| Diorama enabled in sim mode does nothing (user confusion) | Medium | Low | Renderer gates on `ActRaiser_IsActionMapGroup`; menu subtitle + stderr message clarify action-only scope (§8.4, §10.6) |
| Sim-mode 3D (Mode-7 terrain) requested later | — | — | Explicitly OUT OF SCOPE here; deferred to a follow-up design session (true 3D terrain projection, not flat planes) |

---

## Appendix D: Design decisions to lock in BEFORE coding

These are pitfalls that are technically-correct-on-paper but lead to fragile or
trap-laden code. Each has a decision to make now. **Scope note:** this is a
pragmatic solo-dev C codebase (direct globals, env-var debug flags, plain static
functions — no DI/vtables/frameworks). Every decision below is deliberately the
*smallest change that removes the trap* — a design review flagged 18 of the raw
proposals as over-engineering, and these are the trimmed versions. Resist the urge
to "clean up" further than stated.

### D1. One present dispatcher; share the UI tail, branch the scene (M1)
**Trap:** `RenderFramebuffer` already does 6 jobs; adding `RenderDiorama` as a
second top-level present function makes an implementer copy-paste it and let the
shared overlay/UI tail (inspector, settings overlay) drift.
**Decide:** during M1's flat recomposite, extract ONE `ComposeOverlayTail(viewport)`
(the Mode7/HUD/HdReplacements/SceneInspector/SettingsOverlay calls) and a plain
dispatcher: `if (diorama_active) ComposeDiorama(...); else ComposeFlat(...);` then
the shared tail. Flat calls the full tail; diorama v1 calls only
`SettingsOverlay_Render` (inspector disabled per §8.7). Plain static functions in
`present.c` (see D6) — **no** 4-stage pipeline, no shared "UploadTextures" stage
(the flat/diorama uploads are disjoint — forcing them together removes no
duplication). The dispatcher (not `RenderDiorama`) owns the tail — resolves the
§5.5-vs-§8.5 ambiguity.

### D2. One PAR accessor; kill the 7:6 copy-paste (M1)
**Trap:** the `pixel_aspect==Crt43 ? 7:6` correction is already copy-pasted 3× in
the flat path (main.c:931-949, 1043-1047, 1063-1066); §5.9's diorama X-scale would
be a 4th. Each is gated on `!ignore_aspect_ratio` independently → drift.
**Decide:** add ONE accessor `void Settings_PixelAspect(int *num, int *den)` (returns
{7,6} when `Crt43 && !ignore_aspect_ratio`, else {1,1}), mirroring the existing
`Settings_VisibleX0/VisibleWidth` convention. All 3 flat sites + the diorama
projection consume it. This also pins the `ignore_aspect_ratio` gate in one place,
closing §5.9's open "decide explicitly" question. Do NOT build a multi-field
"PresentGeometry producer" — crop is already centralized (VisibleX0/Width) and the
letterbox viewport is flat-path-only.

### D3. Snapshot DERIVED values, and forbid the laundering helpers (M5)
**Trap:** the "snapshot g_ppu fields + forbid g_ppu" rule misses values reached
through helpers — `Settings_VisibleX0/VisibleWidth()`, `g_ws_extra`,
`g_active_pixel_aspect` contain no `g_ppu->`/`g_settings.` text, so they launder a
live read past any grep/poison of `g_ppu`.
**Decide:** the FrameSlot snapshots the **resolved results**, not the inputs: add
`slot->visible_x0`, `slot->visible_width` (computed via the helpers in the capture
critical section), and keep `pixel_aspect`. Present-thread code must NOT call
`Settings_Visible*()` / `GetPresentationViewport()` at all — it reads the
snapshotted ints. (BuildHudPresentationChunks keeps a live-value path for its
game-thread hit-test caller — see D4.)

### D4. HUD-chunk geometry takes explicit inputs, not a fake slot (M5)
**Trap:** `BuildHudPresentationChunks` has TWO callers — the present-thread
renderer AND the game-thread mouse hit-test (`InspectWindowPoint`, main.c:1488)
which legitimately wants live values and has no slot. Forcing it to take a
`const FrameSlot*` creates a fake-slot hack on the game side.
**Decide:** give it an explicit `HudProjectionInputs` struct (the wsHud* fields,
extraLeftRight, OBJ-icon x/y, vis_w, scale). Present callers fill it from the slot;
the hit-test fills it from live state. One algorithm, two callers, no implicit
globals.

### D5. FrameSlot has ONE writer function with a complete contract (M5)
**Trap:** the §2.3 snapshot is ~30 assignments open-coded in the mutex critical
section with `// ...` TODOs (m7 rect, hd_rects[32]) — there's no single place that
IS the contract "everything the present thread reads is set here," so fields get
forgotten → torn frames.
**Decide:** one function `FrameSlot_Capture(FrameSlot *dst)`, the SOLE writer, that
populates EVERY field (finish the elided m7/hd_rects/OAM cases — mirror
RenderHdReplacements' reads at main.c:1729-1750). No `// ...` survives in shipped
code. Called once immediately after `RtlDrawPpuFrame`.

### D6. Physically isolate present code in present.c — the compile-time race fence (M5)
**Trap:** §2.9(d)'s `#define g_ppu <poison>` cannot work inside `main.c` (the game
thread there legitimately reads g_ppu every frame), and it only fences one of
several raced globals. Enforcement-by-grep-and-discipline silently rots: any future
`g_ppu`/`g_settings` read added to a render function reintroduces a race.
**Decide:** in M5, move the present/compose functions
(`RenderFramebuffer`/`Present*`, `RenderHudOverlay`, `BuildHudPresentationChunks`,
`RenderMode7Overlay`, `RenderHdReplacements`, `RenderSceneInspector`, HUD-chunk
helpers) into a new `src/present.c` that does NOT declare/include `g_ppu`,
`g_settings`, `g_snes_width`, `g_ws_extra`, `g_active_pixel_aspect`, or
`Settings_Visible*()`. It reads ONLY `const FrameSlot *`. Any stray live read
becomes an **undeclared-symbol compile error** — the structural guard the poison
macro was reaching for, now actually working, and it catches the D3 laundering
reads too. This makes the snapshot invariant hold *by construction*, not discipline.

### D7. Buffering: ONE frame-generation owner (M5)
**Trap:** the plan has three handoff models on three counters (FrameSlot memcpy for
base+hud; `g_diorama_pixels` pointer-swap; m7/hd pointer-swap). Three swap points =
where a race hides.
**Decide:** one `read_idx` selected in the single critical section that sets
`g_frame_pending`, selecting base/hud/diorama/m7/hd sets *together*. Reuse the
existing `RebindPpuOutputSurfaces` (main.c:1666) which already rebinds all surfaces
as a unit. **You do not have to convert base/hud to pointer-swap first** — shipping
the memcpy for base/hud (M5) and pointer-swap for the big m7/diorama sets is fine
as long as the *selector* advances atomically in one place. (If you later unify on
pointer-swap for all, change the §2.1 embedded arrays to pointers so base/hud
aren't allocated twice — do not keep both.)

### D8. Quiesce, don't build a command queue (M5)
**Trap:** §2.9(a) waffles between "command queue" and "quiesce"; an implementer
builds a fire-and-forget queue and a resize/savestate lands a frame late or out of
order.
**Decide:** pick QUIESCE. For sync-rendezvous ops (geometry/fullscreen change, F7
load): the owner thread parks the present thread (finish current present, block on
the existing condition var), runs the EXISTING inline code
(`ResolveVideoGeometry`/`RtlLoadSnapshot`) itself, then releases. Reuses the
present mutex + the two condition vars already in §2.1; ordering is plain program
order; keeps `SDL_SetWindowFullscreen/Size` on the window-owner thread. No new
subsystem.

### D9. Create all textures on the main thread before spawning present (M5)
**Trap:** §2.4 leaves texture creation ambiguous; lazy per-frame `calloc`+
`SDL_CreateTexture` on the present thread invites device-loss/failure with no
recovery path.
**Decide:** create ALL textures (g_texture, HUD bg/obj, m7, HD, diorama base
layers) on the main thread before the present thread starts. Enable/disable and any
geometry-driven recreation go through the D8 quiesce path on the owner thread,
never lazily mid-present. In `ComposeDiorama`, a missing/failed layer texture logs
once and is skipped — never silently vanishes.

### D10. Shutdown: join present BEFORE overlay/texture teardown (M5)
**Trap:** §2.6 says "join before destroying renderer, window, etc." but real
teardown (main.c:2718) calls `SettingsOverlay_Destroy` (frees the font atlas the
present thread now uses) and DestroyTexture calls *before* DestroyRenderer.
**Decide:** set `g_present_running=false`, broadcast the cond(s), `SDL_WaitThread`
— and place this immediately after the main loop, BEFORE `SettingsOverlay_Destroy`
and the DestroyTexture block, not merely before DestroyRenderer.

### D11. Handshake protocol: timed wait + full predicates (M5)
**Trap:** §2.2's untimed `SDL_WaitCondition` contradicts §2.5's "re-present every
16ms," and the submit wait `while (g_frame_pending)` doesn't test
`g_present_running` → deadlock on abnormal present exit.
**Decide:** (1) present ready-wait becomes `SDL_WaitConditionTimeout(..., ~16ms)`,
re-presenting the last slot on timeout (this IS §2.5); (2) submit predicate becomes
`while (g_frame_pending && g_present_running)`; (3) signal `g_present_done_cond` on
the shutdown/error path too, not only normal completion; (4) the D8 quiesce flag
also gates the §2.5 paused re-present timeout loop.

### D12. Named Z constants; hidden layers skip in place (M2)
**Trap:** the tunable Z table is buried as literal floats in a struct initializer;
every tuning pass is a recompile-hunt.
**Decide:** name the five Z values (`kDioramaZ_Backdrop=0.00f … kDioramaZ_Hud=0.95f`)
so the tunable knob is one findable place. Hidden layers hold their absolute Z
(skip-in-place, which the loop already does) — do NOT build a redistribute-across-
visible-span scheme.

### D13. Camera defaults: settings descriptors are the single source (M3)
**Trap:** the persisted setting default (§10.2) and the `g_diorama_cam` C
initializer (§5.1) are independent → "Reset Camera" and boot diverge.
**Decide:** the `SettingDesc` defvals own tilt_x_mrad=150 / tilt_y=0 /
distance_x100=500; seed `g_diorama_cam` from `g_settings` at init and on Reset (via
/1000 and /100). `fov_y` has no setting — keep it a named code constant
`kDioramaFovY=0.4f`.

### D14. One layer-descriptor table; gating predicate is one function (M2-M3)
**Trap:** the per-layer {source, settings-bool, Z} mapping is spelled out
separately in the capture loop (§4.2), render loop (§5.2), and hotkey handler
(§10.5) → drift (the plan already special-cases "no bg4"). Same for the diorama
gate (5 spellings across capture/render/availability/g_new_ppu/hotkey).
**Decide:** (1) one static table in `diorama.c` mapping each toggleable layer to
`{kPpuOverlaySource_*, &g_settings.diorama_layer_*, Z}`; all three sites iterate it.
(2) one predicate `Diorama_IsActiveThisFrame()` = `diorama_mode && new-PPU-capable
&& ActRaiser_IsActionMapGroup(...)` used by BOTH capture and render early-out.
Keep the flat named bools (they match the ws_* house style) — do NOT collapse to a
bitmask.

### D15. Availability hooks enforce mutual exclusion at the settings layer (M3)
**Trap:** "diorama vs HD replacements exclusive" and "HUD scale / display mode inert
in diorama" are enforced implicitly at render time → user sets contradictory combos
and sees controls that silently do nothing.
**Decide:** use the existing (currently-unused) `SettingDesc.available` hook — the
same mechanism §10.6 already commits to. Diorama-inert controls
(`hd_replacements`, `hud_scale_percent`, `display_mode`, aspect) get
`available = !diorama_mode`; diorama rows get `available = new-PPU-capable`. Grey-out,
not silent no-op.

### D16. Test the race and the determinism, not just "non-black" (M5/M6)
**Trap:** §9's tests are eyeball "renders / non-black / feels smooth" — they cannot
catch the g_ppu/renderer/FrameSlot data races or the §3.7 replay misalignment,
which are the actual hard parts.
**Decide:** (1) add `-DAR_TSAN=ON` (a 5-line mirror of the existing `AR_SANITIZE`
block: `-fsanitize=thread -fno-omit-frame-pointer -g`); run M5 present-thread
bring-up under it. (2) Add a required M6 acceptance step: record a `.rec` on the
old loop, replay through the new accumulator loop headless with `AR_WRAM_TRACE`,
diff aligned on `$0088`, assert zero divergence — proves the record/replay hook
fires once per emulated tick (§3.7). Uses existing tooling; no new CI.

### D17. Unit-test the pure projection math (M2)
**Trap:** `ProjectPoint`/`BuildViewProjection`/`BuildLayerMesh` are pure float math
— the cheapest, highest-signal test target — but §9.4 only eyeballs a center pixel.
**Decide:** add `tests/actraiser_diorama_math_test.c` when `diorama.c` lands,
following the existing plain-C `CHECK`-macro convention (link SDL3 for the
`SDL_Vertex`/`FPoint`/`FColor` types — correct §9's "m only" note). Assert
geometric invariants (identity transform → screen coords equal input rect; a known
tilt projects a known corner to a known point; layer ordering by Z). **When M7
lands, extend this same test file to cover the §6.2 interpolation lerp** (also pure
math): the stability/parallax/wrap invariants in §9.5 are unit-assertable here
without threading — do them as math asserts, not just present-thread logging.

### D18. Buffer lifetime + capacity invariant stated once (M2/M5)
**Decide (documentation only, no code):** ALL PPU-output buffers (`g_pixels`,
`g_hud_*`, `g_hd_overlay_pixels`, `g_m7_overlay_pixels`, `g_diorama_pixels`) are
allocated ONCE at max capacity (`kPpuBufWidth*4*240`, m7 ×`kHdMode7Scale`); the
active width is tracked SOLELY by the bind pitch (`g_snes_width*4`); there is NO
realloc on resize and NO per-mode free — reclaimed only at process exit. This
matches the existing convention (main.c:88-90, 1640, 1659). Add
`static_assert(sizeof(slot->oam_snapshot)==sizeof(g_ppu->oam))` at the OAM copy so
a struct change can't silently under-copy. Do NOT add per-mode teardown.
