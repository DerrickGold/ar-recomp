#include "app/game_session.h"
#include "action/action_effect_manifest.h"
#include "app/game_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "actraiser/actraiser_action_bg.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/actraiser_rtl.h"
#include "actraiser_game.h"
#include "actraiser/regional/actraiser_actor_art.h"
#include "actraiser/regional/actraiser_regional_media.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "app/forced_input.h"
#include "app/input_replay.h"
#include "app/runtime_diagnostics.h"
#include "app/runtime_settings.h"
#include "app/session_fatal.h"
#include "app/settings.h"
#include "app/settings_session.h"
#include "audio/audio_session.h"
#include "dev/host_dev_tools.h"
#include "dev/native_audio_trace.h"
#include "dev/oracle_trace.h"
#include "dev/sfx_census.h"
#include "diorama/diorama.h"
#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_layer_manifest.h"
#include "host/host_display.h"
#include "host/host_frame_surfaces.h"
#include "host/host_input.h"
#include "host/host_localization.h"
#include "host/host_ppu_output.h"
#include "host/host_video.h"
#include "manual/manual_reader.h"
#include "present/present.h"
#include "present/presentation_textures.h"
#include "present/render_comparison.h"
#include "replacements/hd_replacement_host.h"
#include "render/crt_post.h"
#include "save/save_slot_host.h"
#include "settings_overlay/settings_overlay.h"
#include "settings_overlay/regional/regional_host.h"
#include "sim/sim_frame_capture.h"
#include "sim/sim_phase0_trace.h"
#include "sim/sim_render_metadata.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_map_build.h"
#include "sim/town/sim_town_ground_art.h"
#include "sim/world_nav/sim_world_navigation_towns.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/types.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/support/utf8_fs.h"

enum { kDefaultPowerOnSramFill = 0x60 };

/* Overlay, world map, diorama manifest, the injected overlay hooks (layer editor,
 * manual), input, and music. Injection rather than direct calls is what keeps
 * settings_overlay.c testable with no renderer at all -- see settings_overlay.h. */
static void PrepareSubsystems(const GameSessionConfig *config) {
  if (!config->headless || config->headless_video) {
    PresentationTextures_Create();
    HdReplacementHost_LoadTextures();
  }
  HostLocalization_Install(config->headless);
  if (!SettingsOverlay_Init(&g_render_device, g_window,
                            config->rom_data, config->rom_size))
    Die("font atlas creation for settings overlay failed");
  HostLocalization_InstallInterfaceFonts();
  /* The world-map image and pure development-builder tables are immutable ROM
   * data. Failure is not fatal: consumers retain the authentic presentation. */
  if (SimWorldMap_Init(config->rom_data, config->rom_size))
    SimWorldMapBuild_Init(config->rom_data, config->rom_size);
  if (!SimTownGroundArt_Init(config->rom_data, config->rom_size))
    fprintf(stderr, "[world-navigation] native town ground unavailable\n");
  if (!SimWorldNavigationTowns_Init(config->rom_data, config->rom_size))
    fprintf(stderr, "[world-navigation] initial town terrain unavailable\n");
  if (!Diorama_InitRomBackdrops(config->rom_data, config->rom_size))
    fprintf(stderr, "[diorama] named ROM backdrops unavailable\n");
  HostLocalization_LoadRegionalMedia();
  ActRaiserActorArt_Initialize(ActRaiserRegionalMedia_ActorArt());
  if (!ActRaiserActionBg_InitRoomScenes(config->rom_data, config->rom_size))
    fprintf(stderr, "[action-room-scene] immutable loader unavailable\n");
  /* Per-room diorama layer overrides. Absent file is the normal case and leaves
   * every room drawing as built. */
  DioramaLayerManifest_Load();
  ActionEffectManifest_Load();
  SettingsOverlay_SetInspectorInfoProvider(
      HostDevTools_FormatInspectorInfo);
  SettingsOverlayRegionalHost_InstallHooks();
  Diorama_InstallLayerEditor();

  /* The in-game manual, injected for the same reason: it owns textures and an
   * image decoder, and the overlay's own test links settings_overlay.c with no
   * renderer at all. Nothing is loaded here -- the availability hook reads and
   * indexes the file when the overlay first needs it; page textures remain lazy
   * until the player opens the reader. */
  static const SettingsOverlayManualHooks kManualHooks = {
    .available = ManualReader_Available,
    .is_open = ManualReader_IsOpen,
    .close = ManualReader_Close,
    .render = ManualReader_Render,
    .handle_key = ManualReader_HandleKey,
    .handle_pad = ManualReader_HandleGamepadEvent,
  };
  SettingsOverlay_SetManualHooks(&kManualHooks);

  RuntimeSettings_BeginSession();
  /* After the action observer is installed: the pad's save/load-state
   * bindings route through it. */
  HostInput_BeginSession();
  RenderComparison_Reset();
  Diorama_SeedCameraFromSettings();

  AudioSession_Begin();
}

/* Register the game, bring up the SNES, apply the deterministic visual patches
 * (which must sit between cart_load and Randomizer_Init), fill power-on WRAM and
 * battery SRAM and load the persisted save before replay/audio can advance. */
static void StartRunner(const GameSessionConfig *config) {
  if (RtlRegisterGame(&kActRaiserGameModule) != SR_RESULT_OK)
    Die("The linked game module is incompatible with this runner.");
  if (!SnesInit(config->rom_data, (int)config->rom_size)) Die("SnesInit failed");
  /* Runtime clocks are process globals; a replacement runner starts at the
   * same power-on boundary as the initial session. SRAM is loaded below. */
  RtlReset(1);
  if (!RuntimeDiagnostics_Bind(RtlGameRunner()))
    Die("runner diagnostics observer bind failed");
  if (!NativeAudioTrace_Init(RtlGameRunner()))
    Die("native audio trace observer bind failed");

  /* Lifecycle initialization applies deterministic visual source-data
   * adjustments after cartridge loading and before Randomizer_Init snapshots
   * the live ROM. A signature mismatch is safe but important: it means effects
   * metadata and the running visual script no longer share the investigated
   * USA-ROM contract. */
  const ActRaiserRomSetupResult rom_setup =
      ActRaiser_LastRomSetupResult();
  if (!rom_setup.visual_patches_applied)
    fprintf(stderr,
            "[sim-visuals] house-fire cadence patch skipped: "
            "unexpected ROM signature\n");

  if (!rom_setup.randomizer_initialized && g_settings.rando_enable) {
    Die("The Randomizer is enabled, but its pristine ROM snapshot could not "
        "be created. Verify that the configured ROM is supported and that "
        "enough memory is available, or disable Randomizer in settings.ini.");
  }

  HdReplacementHost_BindSurfaces();
  HostPpuOutput_Rebind();
  /* Frame-0 margin state: pillarboxed-authentic (render the 256 columns
   * centered in the wide framebuffer). The ABI surface rebind above configures
   * it; ActRaiser_ApplyWidescreenPolicy reapplies per-frame policy after the
   * PPU reset clears the live fields. */

  /* Power-on WRAM fill. The SNES does not clear WRAM at power-on; snes9x (our
   * reference emulator) fills it with the 0x55 pattern, and ActRaiser's title
   * sequence depends on that — with zero-filled WRAM the title's per-frame loop
   * takes a path that underflows the SNES stack and crashes into the $2100
   * open-bus reads (bank_02_AF86). Match snes9x: fill g_ram with 0x55 before
   * boot so uninitialized-RAM reads agree with the reference. AR_WRAM_INIT
   * overrides with an exact dump (used by the differential harness). */
  {
    const char *fenv = getenv("AR_WRAM_FILL");
    int fill = fenv
        ? (int)strtoul(fenv, NULL, 0)
        : kActRaiserPowerOnWramFill;
    memset(g_ram, fill, kActRaiserWramSize);
    const char *wp0 = getenv("AR_WRAM_INIT");
    if (wp0 && wp0[0]) {
      FILE *f = sr_fopen(wp0, "rb");
      if (f) { size_t n = fread(g_ram, 1, kActRaiserWramSize, f); fclose(f);
        fprintf(stderr, "[wram-init] seeded %zu bytes from %s\n", n, wp0); }
      else fprintf(stderr, "AR_WRAM_INIT: cannot open %s\n", wp0);
    }
  }

  /* Power-on battery SRAM fill. A never-written cartridge battery is NOT zero;
   * snes9x (our reference) powers SRAM up to the 0x60 pattern, and ActRaiser
   * validates its save data — an all-zero SRAM is misread as a corrupt/level-0
   * save (the "must be level 1" symptom) instead of "blank -> new game". Match
   * the reference so the save-validity check behaves identically. Only applies
   * to a fresh cart (cart_load zero-fills it); a real .sav load overrides. */
  {
    const char *senv = getenv("AR_SRAM_FILL");
    int sfill = senv ? (int)strtoul(senv, NULL, 0)
                     : kDefaultPowerOnSramFill;
    if (g_sram && g_sram_size > 0) memset(g_sram, sfill, g_sram_size);
  }

  SaveSlotHost_AttachBatterySave(config->headless);

  OracleTrace_Init(RtlGameRunner());
  ForcedInput_Init();
  InputReplay_Init();
  SaveSlotHost_InitializeRegionalCampaign();
  SettingsOverlayRegionalHost_InstallPrompts(config->headless);
  if (!InputReplay_SetPolicyDigest(ActRaiserRegional_ReplayDigest, NULL))
    Die("Regional replay identity could not be initialized.");
  SettingsSession_Start();

  /* Do not silently run a debug replay from power-on when its requested start
   * state cannot be restored. Runner snapshots omit the recompiled CPU and
   * suspended game continuation; even a native-font restore is unsafe. */
  { const char *ls = getenv("AR_LOADSTATE");
    if (ls && ls[0]) {
      Die("AR_LOADSTATE is unsupported: debug snapshots do not restore "
          "recompiled game execution. No snapshot was loaded. Unset "
          "AR_LOADSTATE and use a battery save with an input recording.");
    } }
  /* Bind cold-boot state before starting asynchronous audio production, whose
   * demand callback can advance APU state even before the first game tick.
   * Unsupported restores fail before any replay header is written. */
  if (!InputReplay_BeginSession(RtlGameRunner(), RtlGameIdentifier()))
    Die(InputReplay_LastError());

  /* Observers need a live runner, and must bind before audio starts. */
  SfxCensus_Init();
  AudioSession_StartOutput();
  SaveSlotHost_InstallHooks();
}

/* Stop producers before their observers/runner, and release game textures
 * before the process-owned render device. The next session uses the same boot
 * path while SDL, the window and the render device remain alive. */
static GameSessionResult StopSession(void) {
  const bool fatal_session = SessionFatal_Requested();
  const bool settings_flush_failed = !SettingsSession_Finish(fatal_session);
  bool save_flush_failed = !SaveSlotHost_FlushBatterySave();
  RuntimeSettings_EndSession();

  DumpDiagState(fatal_session
                    ? "fatal"
                    : RuntimeSettings_LifecycleRequest() ==
                              kRuntimeLifecycle_Restart
                          ? "restart" : "exit");
  SimPhase0Trace_Close();
  SimRenderMetadata_TraceClose();
  SimFrameCapture_Shutdown();
  ActRaiserActionBg_Shutdown();
  ActRaiserActorArt_Shutdown();
  HostLocalization_Shutdown();

  /* Stop the sole audio producer before reading observer-owned capture state
   * or removing subscriptions. The run directory remains live for reports. */
  AudioSession_End();
  SfxCensus_Report();
  NativeAudioTrace_Report();

  InputReplay_Shutdown();
  OracleTrace_Shutdown();
  NativeAudioTrace_Shutdown();
  HdReplacementHost_Shutdown();
  HostPpuOutput_Reset();
  PresentRendererResources_Reset();
  SimTownGroundArt_Shutdown();
  SimWorldNavigationTowns_Shutdown();
  DioramaFrameGeneration_Shutdown();
  Diorama_Shutdown(&g_render_device);
  ManualReader_DestroyTextures();
  SettingsOverlay_Destroy();
  HostLocalization_ReleaseFonts();
  /* Release the game coroutine's stack mapping / fiber. Safe here: the game
   * thread is this thread and the main loop has exited, so nothing can be
   * running on that stack. */
  ActRaiser_DestroyGameCoroutine();
  HostInput_EndSession();
  RuntimeDiagnostics_Unbind();
  SnesShutdown();
  PresentationTextures_Destroy();
  HostFrameSurfaces_ReleaseDioramaPlanes();
  /* Owns a full-window render target plus a GPU shader and render state, and
   * all three must go before the renderer that created them. */
  CrtPost_Shutdown(&g_render_device);
  HostDisplay_InvalidatePresentHistory();
  if (!SaveSlotHost_Close()) save_flush_failed = true;
  return (GameSessionResult){
    .settings_failed = settings_flush_failed,
    .save_failed = save_flush_failed,
  };
}

GameSessionResult GameSession_Run(const GameSessionConfig *config) {
  PrepareSubsystems(config);
  StartRunner(config);
  fprintf(stderr, "[lifecycle] game session ready; window %u; frame %d\n",
          g_window ? SDL_GetWindowID(g_window) : 0, snes_frame_counter);
  ActRaiserLocalizationRuntime_ApplySettings();
  if (!HostLocalization_ExitRequested()) GameLoop_Run(config);

  const bool reset = RuntimeSettings_LifecycleRequest() == kRuntimeLifecycle_Restart;
  GameSessionResult result = StopSession();
  const bool fatal = SessionFatal_Requested();
  result.restart = reset && !fatal && !result.settings_failed && !result.save_failed;
  result.exit_code = fatal || (reset && !result.restart) ? 1 : 0;
  if (result.restart) {
    fprintf(stderr, "[lifecycle] soft reset; keeping window %u\n",
            g_window ? SDL_GetWindowID(g_window) : 0);
  } else if (reset) {
    fprintf(stderr,
        "[lifecycle] reset stopped after a session or persistence failure; saves preserved\n");
  }
  return result;
}
