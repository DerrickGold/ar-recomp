#include "host/imported_world_travel.h"
#include "host/imported_game_session.h"
#include "dev/host_dev_tools.h"
#include "app/run_dir.h"
#include "platform/sdl/dev_tools_readback_sdl.h"
#include "snesrecomp/support/utf8_fs.h"
#include "actraiser2/ar2_content.h"
#include "actraiser2/ar2_presentation.h"
#include "actraiser_game.h"
#include "app/input_map.h"
#include "app/session_fatal.h"
#include "app/settings.h"
#include "app/settings_session.h"
#include "audio/audio_session.h"
#include "host/host_display.h"
#include "host/host_input.h"
#include "host/host_video.h"
#include "platform/sdl/render_sdl_interop.h"
#include "render/presentation_layout.h"
#include "present/render_preparation.h"
#include "settings_overlay/settings_overlay.h"
#include "sim/world_nav/world_navigation_travel.h"
#include "snesrecomp/game/audio_timing.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/support/digest.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum TravelPhase { kTravel_Descending, kTravel_Level, kTravel_Fading, kTravel_Ascending };
static int s_destination = -1;
typedef struct ImportedVisit {
  bool active, ready, leave, retry, focused, was_paused, transition_captured;
  bool snapshot_pending;
  enum TravelPhase phase;
  uint8_t stage, before[32];
  CpuState cpu_before;
  int frame_before;
  InputMapActionFn action_handler;
  SDL_AudioStream *audio;
  ArImportedGameFrame frame;
  unsigned startup_ticks, visit_frame, replay_frame, replay_value;
  uint32_t replay_input;
  char replay_command[32];
  FILE *replay, *trace;
  int replay_fields;
  uint64_t deadline, last_ns, descent_begin_ns;
  unsigned descent_updates;
  double descent_max_gap_ms;
  double phase_ticks;
} ImportedVisit;
static ImportedVisit s_visit;

static void LevelInput(const SDL_Event *event, bool *leave, bool *retry);
static void FinishVisit(void);

static bool OnWorldMap(void) {
  return !s_visit.active && Ar2Content_Atlas() && !SettingsOverlay_IsOpen() &&
         g_ram[kActRaiserWram_MapGroup] == kActRaiserMapGroup_NonAction &&
         g_ram[kActRaiserWram_CurrentMap] == kActRaiserNonActionMap_WorldMap;
}

bool ImportedWorldTravel_HandleEvent(const SDL_Event *event) {
  if (s_visit.active) {
    switch (event->type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      s_visit.focused = false;
      HostInput_ClearHeld();
      Diorama_SetDragging(false);
      return false;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: s_visit.focused = true; return false;
    case SDL_EVENT_RENDER_DEVICE_RESET:
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_LOST:
      /* Release imported handles before the shared resource owner resets. */
      FinishVisit();
      return false;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
      LevelInput(event, &s_visit.leave, &s_visit.retry);
      return true;
    default: return false;
    }
  }
  const bool toggle = (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
                       event->key.scancode == SDL_SCANCODE_C) ||
      (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
       event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK);
  if (!toggle || !OnWorldMap()) return false;
  if (WorldNavigationTravel_Toggle(g_ram, kSnesWramSize, Ar2Content_Atlas(),
                                    g_settings.sim3d_world_navigation)) {
    HostInput_ClearHeld();
    HostInput_RequestPausedRedraw();
  }
  return true;
}

bool ImportedWorldTravel_Pending(void) {
  /* Deterministic smoke controls use the same continent and selection path,
   * at the main loop's acknowledged owner boundary. */
  static bool configured;
  static const char *at;
  if (!configured) { at = getenv("AR_IMPORTED_TRAVEL_AT"); configured = true; }
  if (at && OnWorldMap()) {
    char *end;
    const unsigned long tick = strtoul(at, &end, 10);
    if (end == at || (*end && *end != ',')) at = NULL;
    else if ((unsigned long)snes_frame_counter >= tick &&
             WorldNavigationTravel_Toggle(g_ram, kSnesWramSize, Ar2Content_Atlas(),
                                    g_settings.sim3d_world_navigation))
      at = *end ? end + 1 : NULL;
  }
  int destination = WorldNavigationTravel_TakeDestination();
  const Ar2WorldAtlas *atlas = Ar2Content_Atlas();
  if (OnWorldMap() && destination >= 0 && (size_t)destination < atlas->destination_count &&
      Ar2Content_Playable(&atlas->destinations[destination])) s_destination = destination;
  return s_destination >= 0;
}

static void ShowError(const char *message) {
  fprintf(stderr, "[imported-world] %s\n", message);
  if (!getenv("AR_HEADLESS_VIDEO"))
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ActRaiser 2", message, g_window);
}

/* These host actions use no suspended AR1 gameplay state. The shared menu
 * exposes display/audio/control preferences until AR2 campaign adapters exist. */
static void LevelAction(InputAction action) {
  switch (action) {
  case kInputAction_Menu:
    HostInput_ClearHeld();
    if (SettingsOverlay_IsOpen()) SettingsOverlay_Close();
    else SettingsOverlay_Open();
    break;
  case kInputAction_Pause: HostInput_TogglePause(); break;
  case kInputAction_CamReset: Diorama_ResetCamera(); break;
  default: break;
  }
}

static int DisplayExtra(void) {
  int width = 256, height = 224;
  ArRenderDevice_GetOutputSize(&g_render_device, &width, &height);
  const bool automatic = g_settings.extended_aspect == kScreenAspect_Auto;
  if (!automatic && (g_settings.display_mode == kDisplayMode_43 || !g_settings.ws_action)) return 0;
  return ArPresentationLayout_ResolveHorizontalMargin(256, 224,
      automatic ? width : Settings_ExtendedAspectX(),
      automatic ? height : Settings_ExtendedAspectY(),
      g_settings.pixel_aspect == kPixelAspect_Crt43, 128);
}

static void LevelInput(const SDL_Event *event, bool *leave, bool *retry) {
  if (SettingsOverlay_IsOpen()) {
    (void)HostInput_HandleEvent(event);
    return;
  }
  if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
      switch (event->key.scancode) {
      case SDL_SCANCODE_ESCAPE:
      case SDL_SCANCODE_F1: LevelAction(kInputAction_Menu); return;
      case SDL_SCANCODE_M: *leave = true; return;
      case SDL_SCANCODE_R: *retry = true; return;
      case SDL_SCANCODE_P: LevelAction(kInputAction_Pause); return;
      case SDL_SCANCODE_F2:
      case SDL_SCANCODE_C: HostDevTools_TakeFullSnapshot(); return;
      case SDL_SCANCODE_D:
      case SDL_SCANCODE_TAB:
        Settings_SetLong(Settings_Find("diorama_mode"), !g_settings.diorama_mode);
        return;
      default: break;
      }
    }
    InputMap_HandleKey(event->key.scancode, event->type == SDL_EVENT_KEY_DOWN, event->key.repeat);
  } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
             event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
    *leave = true;
  } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
    if (event->button.button == SDL_BUTTON_RIGHT)
      Diorama_SetDragging(event->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    if (event->button.button == SDL_BUTTON_MIDDLE && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
      Diorama_ResetCamera();
  } else if (event->type == SDL_EVENT_MOUSE_MOTION && Diorama_IsDragging()) {
    Diorama_AdjustCamera(event->motion.xrel * Diorama_DragRadPerPx(),
                         event->motion.yrel * Diorama_DragRadPerPx(), 0);
  } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
    Diorama_AdjustCamera(0, 0, -event->wheel.y * Diorama_ZoomStep());
  } else {
    InputMap_HandleEvent(event);
  }
}

static void CaptureFrame(const char *path) {
  if (!path) return;
  SDL_Surface *surface = SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(&g_render_device), NULL);
  if (surface) SDL_SaveBMP(surface, path);
  SDL_DestroySurface(surface);
}

void ImportedWorldTravel_RequestSnapshot(void) {
  if (s_visit.active) s_visit.snapshot_pending = true;
}

static void TakeSnapshot(void) {
  static unsigned number;
  s_visit.snapshot_pending = false;
  char directory[4096], prefix[4096], path[4096];
  RunDirFile(directory, sizeof(directory), "snapshots");
  if (!SDL_CreateDirectory(directory)) {
    fprintf(stderr, "[snap] cannot create %s: %s\n", directory, SDL_GetError());
    return;
  }
  RunDirFile(prefix, sizeof(prefix), "snapshots/ar2_%02u_f%u", number++, s_visit.frame.frame);
  bool ok = ImportedGameSession_Snapshot(prefix);
  DevToolsRgb24Capture capture = {0};
  if (snprintf(path, sizeof(path), "%s.ppm", prefix) >= (int)sizeof(path)) return;
  if (ArSdlDevTools_CaptureRgb24(&g_render_device, &capture)) {
    FILE *file = sr_fopen(path, "wb");
    if (file) {
      fprintf(file, "P6\n%d %d\n255\n", capture.width, capture.height);
      for (int y = 0; y < capture.height; y++)
        ok &= fwrite(capture.pixels + y * capture.pitch_bytes, 3, capture.width, file) ==
              (size_t)capture.width;
      ok &= !ferror(file);
      ok &= fclose(file) == 0;
    } else ok = false;
    capture.release(capture.owner);
  } else ok = false;
  fprintf(stderr, "[snap] AR2 %s -> %s.{wram,vram,cgram,oam,highoam}.bin, "
      ".ppu.json, .cpu.json, .ppm\n",
      ok ? "complete" : "incomplete", prefix);
}

static void ReleaseLevel(void) {
  ImportedGameSession_Close();
  SDL_DestroyAudioStream(s_visit.audio);
  s_visit.audio = NULL;
  Ar2Presentation_Close();
}

static void FinishVisit(void) {
  if (!s_visit.active) return;
  ReleaseLevel();
  if (s_visit.trace) fclose(s_visit.trace);
  if (s_visit.replay) fclose(s_visit.replay);
  SettingsOverlay_Close();
  SettingsOverlay_SetHostSettingsOnly(false);
  uint8_t after[32];
  sr_support_sha256(g_ram, kSnesWramSize, after);
  const bool intact = !memcmp(s_visit.before, after, 32) &&
      !memcmp(&s_visit.cpu_before, &g_cpu, sizeof(g_cpu)) &&
      snes_frame_counter == s_visit.frame_before;
  fprintf(stderr, "[imported-world] AR1 session %s at frame %d after %u visit frames\n",
          intact ? "unchanged" : "CHANGED", s_visit.frame_before, s_visit.visit_frame);
  InputMap_SetActionHandler(s_visit.action_handler);
  HostInput_ClearHeld();
  if (HostInput_IsPaused() != s_visit.was_paused) HostInput_TogglePause();
  Diorama_SetDragging(false);
  WorldNavigationTravel_SetApproach(0);
  HostDisplay_InvalidatePresentHistory();
  HostDisplay_ResetVsyncPacing();
  s_visit = (ImportedVisit){0};
  if (!intact) SessionFatal_Request("AR1 session changed during imported travel");
}

void ImportedWorldTravel_EndSession(void) {
  FinishVisit();
  ImportedGameSession_EndSession();
  s_destination = -1;
}

bool ImportedWorldTravel_Active(void) { return s_visit.active; }
bool ImportedWorldTravel_DrawWorld(void) {
  return !s_visit.active || s_visit.phase == kTravel_Descending ||
         s_visit.phase == kTravel_Ascending;
}

static bool OpenLevel(void) {
  size_t size;
  const uint8_t *rom = Ar2Content_Rom(&size);
  s_visit.ready = false;
  s_visit.startup_ticks = 0;
  if (s_visit.audio) SDL_ClearAudioStream(s_visit.audio);
  if (!ImportedGameSession_Start(rom, size, s_visit.stage,
                                 DisplayExtra(), g_settings.diorama_mode)) {
    ShowError(ImportedGameSession_Error());
    return false;
  }
  return true;
}

void ImportedWorldTravel_Begin(void) {
  const int destination = s_destination;
  s_destination = -1;
  const Ar2WorldAtlas *atlas = Ar2Content_Atlas();
  if (!OnWorldMap() || !g_window || destination < 0 ||
      (size_t)destination >= atlas->destination_count) return;
  s_visit.stage = atlas->destinations[destination].flags_and_scene & 255;
  s_visit.active = s_visit.focused = true;
  s_visit.phase = kTravel_Descending;
  s_visit.last_ns = SDL_GetTicksNS();
  s_visit.descent_begin_ns = s_visit.last_ns;
  s_visit.was_paused = HostInput_IsPaused();
  s_visit.action_handler = InputMap_GetActionHandler();
  sr_support_sha256(g_ram, kSnesWramSize, s_visit.before);
  s_visit.cpu_before = g_cpu;
  s_visit.frame_before = snes_frame_counter;
  HostInput_ClearHeld();
  InputMap_SetActionHandler(LevelAction);
  SettingsOverlay_SetHostSettingsOnly(true);
  AudioSession_SetPaused(true);
  s_visit.replay = getenv("AR_IMPORTED_REPLAY") ? fopen(getenv("AR_IMPORTED_REPLAY"), "r") : NULL;
  s_visit.replay_fields = s_visit.replay
      ? fscanf(s_visit.replay, "%u %31s %u", &s_visit.replay_frame,
               s_visit.replay_command, &s_visit.replay_value) : EOF;
  s_visit.trace = getenv("AR_IMPORTED_TRACE") ? fopen(getenv("AR_IMPORTED_TRACE"), "w") : NULL;
  if (s_visit.trace) fprintf(s_visit.trace,
      "visit,frame,scene,input,player_x,player_y,health,lives,diorama,level,display_extra\n");
  if (getenv("AR_IMPORTED_REPLAY") && !s_visit.replay) {
    ShowError("Cannot read level replay");
    FinishVisit();
    return;
  }
  if (!OpenLevel()) { FinishVisit(); return; }
  fprintf(stderr, "[imported-world] descending to %s in window %u\n",
          atlas->destinations[destination].label, SDL_GetWindowID(g_window));
}

static void BeginAscent(void) {
  const double elapsed = s_visit.phase == kTravel_Descending
      ? 90 - fmin(90, s_visit.phase_ticks) : 0;
  ImportedGameSession_Cancel();
  SDL_DestroyAudioStream(s_visit.audio);
  s_visit.audio = NULL;
  Ar2Presentation_Close();
  SettingsOverlay_Close();
  s_visit.phase = kTravel_Ascending;
  s_visit.phase_ticks = elapsed;
  s_visit.transition_captured = false;
  s_visit.leave = s_visit.retry = false;
  WorldNavigationTravel_SetApproach(1 - elapsed / 90);
  HostDisplay_InvalidatePresentHistory();
}

static bool PrepareLevel(void) {
  if (s_visit.ready) return true;
  const ImportedGameState state = ImportedGameSession_Poll(&s_visit.frame, &s_visit.startup_ticks);
  if (state == kImportedGame_Loading) return true;
  if (state == kImportedGame_Failed) {
    ShowError(ImportedGameSession_Error());
    return false;
  }
  if (!Ar2Presentation_Open(&g_render_device)) {
    ShowError(SDL_GetError());
    return false;
  }
  s_visit.ready = true;
  return true;
}

static bool ReplayLevel(void) {
  while (s_visit.replay_fields != EOF && s_visit.replay_frame <= s_visit.visit_frame) {
    if (s_visit.replay_fields != 3 || s_visit.replay_frame != s_visit.visit_frame) {
      ShowError("Malformed level replay");
      return false;
    }
    const char *command = s_visit.replay_command;
    if (!strcmp(command, "input")) s_visit.replay_input = s_visit.replay_value;
    else if (!strcmp(command, "view"))
      Settings_SetLong(Settings_Find("diorama_mode"), s_visit.replay_value != 0);
    else if (!strcmp(command, "menu")) {
      if (s_visit.replay_value) SettingsOverlay_Open();
      else SettingsOverlay_Close();
    } else if (!strcmp(command, "retry")) s_visit.retry = true;
    else if (!strcmp(command, "snapshot")) HostDevTools_TakeFullSnapshot();
    else if (!strcmp(command, "return")) s_visit.leave = true;
    else { ShowError("Unknown level replay command"); return false; }
    s_visit.replay_fields = fscanf(s_visit.replay, "%u %31s %u", &s_visit.replay_frame,
                                   s_visit.replay_command, &s_visit.replay_value);
  }
  return true;
}

static void UpdateAudio(bool paused) {
  if (!s_visit.audio && g_settings.audio_enabled) {
    const SDL_AudioSpec spec = {.format = SDL_AUDIO_S16, .channels = 2, .freq = 48000};
    s_visit.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (s_visit.audio) SDL_ResumeAudioStreamDevice(s_visit.audio);
  }
  if (s_visit.audio) {
    SDL_SetAudioStreamGain(s_visit.audio, g_settings.audio_master_volume / 100.f);
    if (paused || !g_settings.audio_enabled) SDL_ClearAudioStream(s_visit.audio);
  }
}

bool ImportedWorldTravel_Update(bool hidden) {
  if (!s_visit.active) return true;
  Diorama_FlushSettingsIfDirty();
  SettingsSession_PollWrites();
  const uint64_t now = SDL_GetTicksNS();
  const bool headless = getenv("AR_HEADLESS_VIDEO") != NULL;
  const double elapsed = headless ? 1 :
      (double)(now - s_visit.last_ns) / RTL_NTSC_FRAME_INTERVAL_NS;
  s_visit.last_ns = now;
  const int extra = DisplayExtra();
  if (s_visit.phase == kTravel_Level && !ReplayLevel()) { BeginAscent(); return true; }
  const bool paused = (s_visit.phase == kTravel_Level && HostInput_IsPaused()) ||
                      SettingsOverlay_IsOpen() ||
                      !s_visit.focused || hidden;
  if (s_visit.phase == kTravel_Descending) {
    ++s_visit.descent_updates;
    const double gap_ms = elapsed * RTL_NTSC_FRAME_INTERVAL_NS / 1e6;
    s_visit.descent_max_gap_ms = fmax(s_visit.descent_max_gap_ms, gap_ms);
  }
  /* Match AR1's timing. A cold load may finish on black after the descent;
   * loading never blocks the world animation or its event pump. */
  if (!paused) s_visit.phase_ticks += fmin(5, elapsed);
  /* Optional readback of the preceding, completed world frame. Main-loop
   * automation can verify both halves without advancing the suspended ROM. */
  const char *transition_capture = getenv("AR_IMPORTED_TRANSITION_CAPTURE");
  if (transition_capture && ImportedWorldTravel_DrawWorld() &&
      s_visit.phase_ticks >= 45 && !s_visit.transition_captured) {
    char path[4096];
    if (snprintf(path, sizeof(path), "%s-%s.bmp", transition_capture,
          s_visit.phase == kTravel_Descending ? "descent" : "ascent") < (int)sizeof(path))
      CaptureFrame(path);
    s_visit.transition_captured = true;
  }
  if (s_visit.leave && s_visit.phase != kTravel_Ascending && s_visit.phase != kTravel_Fading) {
    if (s_visit.phase == kTravel_Descending) { BeginAscent(); return true; }
    s_visit.phase = kTravel_Fading;
    s_visit.phase_ticks = 0;
    SettingsOverlay_Close();
  }
  if (s_visit.phase == kTravel_Descending) {
    if (!PrepareLevel()) { BeginAscent(); return true; }
    WorldNavigationTravel_SetApproach(s_visit.phase_ticks / 90);
    if (s_visit.phase_ticks < 90 || !s_visit.ready) return true;
    s_visit.phase = kTravel_Level;
    s_visit.phase_ticks = 0;
    s_visit.deadline = now + RTL_NTSC_FRAME_INTERVAL_NS;
    HostInput_ClearHeld();
    fprintf(stderr, "[imported-world] level ready; hid %u startup frames; descent %.1f ms, "
        "%u updates, largest gap %.1f ms%s\n", s_visit.startup_ticks,
        (SDL_GetTicksNS() - s_visit.descent_begin_ns) / 1e6, s_visit.descent_updates,
        s_visit.descent_max_gap_ms, headless ? " (headless ticks)" : "");
  } else if (s_visit.phase == kTravel_Ascending) {
    WorldNavigationTravel_SetApproach(1 - s_visit.phase_ticks / 90);
    if (s_visit.phase_ticks >= 90) FinishVisit();
    return true;
  } else if (s_visit.phase == kTravel_Fading && s_visit.phase_ticks >= 18) {
    BeginAscent();
    return true;
  }
  if (s_visit.retry && s_visit.phase == kTravel_Level) {
    s_visit.retry = false;
    ReleaseLevel();
    if (!OpenLevel()) { BeginAscent(); return true; }
  }
  if (!s_visit.ready) {
    if (!PrepareLevel()) { BeginAscent(); return true; }
    if (!s_visit.ready) {
      ArRenderDevice_Clear(&g_render_device, (ArRenderColorF){0, 0, 0, 1});
      if (headless) ArSdlRenderBackend_SubmitPending(&g_render_device);
      else HostDisplay_PresentCompositedFrame(kHostDisplayPresent_GameTick);
      return true;
    }
    s_visit.deadline = now + RTL_NTSC_FRAME_INTERVAL_NS;
  }
  const bool stopped = paused || s_visit.phase != kTravel_Level;
  UpdateAudio(stopped);
  if (stopped) s_visit.deadline = now;
  else {
    const uint32_t input = s_visit.replay ? s_visit.replay_input : InputMap_State();
    if (now > s_visit.deadline + 5 * RTL_NTSC_FRAME_INTERVAL_NS) s_visit.deadline = now;
    for (int tick = 0; tick < 5 && (headless ? tick == 0 : now >= s_visit.deadline); tick++) {
      ArImportedGameFrame frame;
      if (!ImportedGameSession_Step(input, extra, g_settings.diorama_mode, &frame)) {
        TakeSnapshot();
        ShowError(ImportedGameSession_Error());
        BeginAscent();
        return true;
      }
      if (frame.event == kArImportedGameEvent_ReturnToWorld) { BeginAscent(); return true; }
      s_visit.frame = frame;
      if (s_visit.audio && g_settings.audio_enabled &&
          SDL_GetAudioStreamQueued(s_visit.audio) < 48000)
        SDL_PutAudioStreamData(s_visit.audio, frame.pcm, frame.pcm_frames * 4);
      s_visit.deadline += RTL_NTSC_FRAME_INTERVAL_NS;
    }
  }
  if (hidden) { SDL_Delay(10); return true; }
  HostInput_ApplyDioramaPresentationCamera();
  RenderPreparation_PrepareSelectedFeatures(&g_render_device);
  ArRenderRectI viewport;
  if (!Ar2Presentation_Draw(&s_visit.frame, now, &viewport)) {
    ShowError("Could not render imported level");
    BeginAscent();
    return true;
  }
  if (s_visit.phase == kTravel_Fading) {
    const ArRenderRectF area = {viewport.x, viewport.y, viewport.w, viewport.h};
    ArRenderDevice_DrawSolidRect(&g_render_device, &area,
        (ArRenderColorF){0, 0, 0, s_visit.phase_ticks / 18}, kArRenderBlendMode_Alpha);
  }
  if (SettingsOverlay_IsOpen()) { SettingsOverlay_Tick(); SettingsOverlay_Render(viewport); }
  if (s_visit.snapshot_pending) TakeSnapshot();
  const ArImportedGameFrame *frame = &s_visit.frame;
  if (s_visit.trace) fprintf(s_visit.trace, "%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d\n",
      s_visit.visit_frame, frame->frame, frame->scene,
      s_visit.replay ? s_visit.replay_input : InputMap_State(), frame->player_x, frame->player_y,
      frame->health, frame->lives, g_settings.diorama_mode,
      frame->video.level, frame->video.display_extra);
  if (getenv("AR_IMPORTED_CAPTURE_AT") &&
      s_visit.visit_frame == (unsigned)atoi(getenv("AR_IMPORTED_CAPTURE_AT")))
    CaptureFrame(getenv("AR_IMPORTED_CAPTURE"));
  bool presented = true;
  if (headless) ArSdlRenderBackend_SubmitPending(&g_render_device);
  else presented = HostDisplay_PresentCompositedFrame(SettingsOverlay_IsOpen()
      ? kHostDisplayPresent_Menu
      : stopped ? kHostDisplayPresent_Paused : kHostDisplayPresent_GameTick);
  ++s_visit.visit_frame;
  if (getenv("AR_IMPORTED_FRAMES") &&
      s_visit.visit_frame >= (unsigned)atoi(getenv("AR_IMPORTED_FRAMES"))) s_visit.leave = true;
  return presented;
}
