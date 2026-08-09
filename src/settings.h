#ifndef SETTINGS_H
#define SETTINGS_H
#include "types.h"
#include "sim/sim_render_metadata.h"

/* Live runtime settings — the first slice of the g_settings refactor described
 * in docs/settings-system.md (§3.1/§4). Existing cheat and widescreen behavior
 * gates are descriptor-backed here. Their enforcement was already per-frame,
 * so the live fields replace cached getenv() values without moving the seams.
 *
 * Seeded once at boot from the same AR_* env vars the gates used to read, with
 * the same defaults and special value encodings. */

/* Display-mode cycle (F9) — a preset selector over the flags below, for
 * capturing before/after widescreen comparisons without a UI. Custom is not
 * part of the cycle: it identifies an env/menu-authored combination which no
 * longer matches one of the three deterministic capture presets. */
typedef enum {
  kDisplayMode_43 = 0,      /* authentic 4:3: pillarbox, present centre 256px */
  kDisplayMode_WideRaw,     /* wide geometry, zero corrections ("before") */
  kDisplayMode_WideFull,    /* wide + every correction ("after") */
  kDisplayMode_Custom,
  kDisplayMode_Count
} DisplayMode;

enum { kDisplayMode_PresetCount = kDisplayMode_Custom };

typedef enum {
  kPixelAspect_Square = 0,
  kPixelAspect_Crt43,
  kPixelAspect_Count,
} PixelAspect;

typedef enum {
  kScreenAspect_43 = 0,
  kScreenAspect_169,
  kScreenAspect_1610,
  kScreenAspect_Stretch,   /* fill the window, ignore aspect (was a separate
                            * "Stretch to window" toggle) */
  kScreenAspect_Count,
} ScreenAspect;

/* Host window presentation. Borderless is desktop-fullscreen (SDL's default
 * fullscreen); Exclusive requests a real fullscreen video mode. */
typedef enum {
  kWindowMode_Windowed = 0,
  kWindowMode_Borderless,
  kWindowMode_Exclusive,
  kWindowMode_Count,
} WindowMode;

/* Present pacing. Vsync locks to the display; Unlimited disables vsync and
 * uses a soft 2x detected-refresh cap; Limit paces to frame_limit_fps. */
typedef enum {
  kRefreshMode_Vsync = 0,
  kRefreshMode_Unlimited,
  kRefreshMode_Limit,
  kRefreshMode_Count,
} RefreshMode;

/* Host audio-rate presets. The stored value is the stable menu/config enum;
 * Settings_AudioFrequencyHz translates it for SDL device creation. Auto
 * (the default) resolves to the playback device's native rate at open time
 * (Settings_AudioFrequencyHz returns 0 = "query the device") so the
 * resample chain is one hop on 48kHz-native hardware instead of
 * 32040->44100->48000. Appended after the original three so saved numeric
 * indices keep their meaning. */
typedef enum {
  kAudioFrequency_32040 = 0,
  kAudioFrequency_44100,
  kAudioFrequency_48000,
  kAudioFrequency_Auto,
  kAudioFrequency_Count,
} AudioFrequency;

/* B4 (followup doc): Free Cam is today's manual orbit/zoom, persisted and
 * player-owned. Dynamic Cam is the opt-in reactive camera that sways off
 * gameplay signals around its own dedicated baseline pose — mutually
 * exclusive with Free Cam, not a blend. */
typedef enum {
  kDioramaCam_Free = 0,
  kDioramaCam_Dynamic = 1,
  kDioramaCam_Count,
} DioramaCameraMode;

/* The same two-mode split for the simulation town, and for the same reason:
 * Free Cam's pose is player-owned and persists across a session, Dynamic Cam
 * has its own dedicated baseline that reactive motion leans around. Keeping
 * two poses rather than one is what makes switching modes restore each mode's
 * own camera instead of leaving Dynamic to sway around wherever the last
 * manual drag happened to leave things. */
typedef enum {
  kSimCam_Free = 0,
  kSimCam_Dynamic = 1,
  kSimCam_Count,
} SimCameraMode;

/* B5 (followup doc): promotes BG2 (the farthest/sky background layer in
 * ActRaiser Mode 1 action stages) from an ordinary in-box parallax plane to
 * an enveloping, dimmed, DoF'd skybox that fills the viewport — fixes the
 * dark void that rotates into view past the finite backdrop quad's edges
 * once the camera tilts/yaws/zooms. Enum, not a bool: the three looks are
 * mutually exclusive views of the same layer (matches display_mode's/
 * extended_aspect's mutually-exclusive-preset modeling). Default Off: BG2 is
 * a heuristic pick for "sky" (no programmatic flag says so — see
 * AR_WS_ONLYBG, actraiser_rtl.c), so this stays opt-in rather than changing
 * today's known-good look unprompted. */
typedef enum {
  kDioramaSky_Off = 0,
  kDioramaSky_Only = 1,
  kDioramaSky_Both = 2,
  kDioramaSky_Count,
} DioramaSkyMode;

/* Dimensions of Settings::input_bind. input_map.h static-asserts that these
 * still match its own InputClass/InputAction counts. */
enum {
  kSettingsInputClasses = 2,
  kSettingsInputActions = 25,
};

typedef enum {
  kSettingType_Bool,
  kSettingType_Int,
  kSettingType_Enum,
  kSettingType_Mask,
  kSettingType_Custom,
  /* An input binding. Stored, parsed, and serialized exactly like CUSTOM —
   * the separate type exists so the overlay can tell a binding row apart and
   * open its press-a-button capture mode instead of a text-entry field. */
  kSettingType_Binding,
  kSettingType_Action,
} SettingType;

typedef enum {
  kApply_Passive,
  kApply_Callback,
  kApply_Restart,
  kApply_Save,
  kApply_Action,
} SettingApplyKind;

/* A category is one TAB of the host settings overlay, not a top-level menu
 * entry: settings_overlay.c groups several of these under each nav section
 * (Video, Diorama, Town 3D, ...). Categories that used to hold 30-50 rows are
 * therefore split into several here — a category is meant to be a panel-sized
 * list, and the section above it is what the player navigates by. */
typedef enum {
  kSettingCat_Cheats,
  kSettingCat_Widescreen,
  kSettingCat_Display,
  kSettingCat_Presentation,      /* Diorama: layers, skybox, master toggle */
  kSettingCat_DioramaCamera,     /* Diorama: camera pose + reactive sway */
  kSettingCat_Simulation,        /* SIM 3D: town + world-navigation stages */
  kSettingCat_SimCamera,         /* Town 3D: camera pose + reactive sway */
  kSettingCat_SimLighting,       /* SIM 3D: compatible light/shadow tuning */
  kSettingCat_SimAtmosphere,     /* SIM 3D: clouds, haze, cull, backdrop */
  kSettingCat_Graphics,
  kSettingCat_Crt,               /* Video > CRT: fullscreen CRT post-process */
  kSettingCat_Audio,
  kSettingCat_Input,             /* device selection and analog tuning */
  kSettingCat_InputBinds,        /* one row per (device class, action) */
  kSettingCat_Save,
  kSettingCat_Extras,        /* System > Tools: host commands + debug switch */
  kSettingCat_Enhancements,  /* System > Game: gameplay QoL (bridge, turbo) */
  kSettingCat_Inspector,
  kSettingCat_Manual,        /* Manual: open the reader, and how it lays out */
  kSettingCat_RandoSeed,     /* Randomizer: master, seed, and what it applied */
  kSettingCat_RandoEnemies,  /* Randomizer: enemy stats and type shuffling */
  kSettingCat_RandoItems,    /* Randomizer: statue drops and placement */
  kSettingCat_RandoSim,      /* Randomizer: sim-mode monster lairs */
  kSettingCat_Count,
} SettingCategory;

/* True for every Town 3D tab. Callers that react to "the 3D town presentation
 * changed" (main.c's paused-redraw kick) want the whole group, not one tab. */
bool Settings_CategoryIsSim3D(SettingCategory category);


typedef enum SaveProgressEdit {
  kSaveProgressEdit_LeaveAsIs = 0,
  kSaveProgressEdit_Act1,
  kSaveProgressEdit_Act1Cleared,
  kSaveProgressEdit_Act2,
  kSaveProgressEdit_Act2Cleared,
  kSaveProgressEdit_Count,
} SaveProgressEdit;

typedef enum SaveEditorPage {
  /* Actions holds the backend/arming controls and the apply/import/export
   * commands that used to repeat on every page; the rest are staged payload. */
  kSaveEditorPage_Actions = 0,
  kSaveEditorPage_Progress,
  kSaveEditorPage_Status,
  kSaveEditorPage_Magic,
  kSaveEditorPage_Items,
  kSaveEditorPage_Scores,
  kSaveEditorPage_Count,
} SaveEditorPage;

typedef struct SettingDesc SettingDesc;
typedef bool (*SettingAvailableFn)(void);
typedef void (*SettingChangedFn)(const SettingDesc *desc);
typedef int (*SettingFormatFn)(char *buffer, int buffer_size,
                               const void *field);

struct SettingDesc {
  const char *key;          /* stable settings.ini/menu id */
  const char *env;          /* boot-only compatibility seed */
  const char *label;
  const char *tooltip;
  SettingType type;
  SettingApplyKind apply;
  SettingCategory category;
  void *field;
  long defval, minval, maxval, step;
  bool sticky;              /* disabling stops enforcement, cannot undo history */
  const char *const *enum_labels;
  int enum_count;
  SettingAvailableFn available;
  SettingChangedFn on_change;
  bool (*parse)(const char *text, void *field);
  SettingFormatFn format;
  /* T2d: false (the zero default every positional row already gets) means this
   * setting's AR_* environment seed keeps the historical leading-zero /
   * default-polarity parse. Set it true -- via `.modern_env = true` on a
   * literal row, or BOOL_SETTING_MODERN -- for settings whose env var is a
   * modern alias parsed by the same human-readable parser as settings.ini.
   * The distinction is NOT derivable from any other field here (category, type,
   * apply kind and the parse/format hooks were all measured and all mix), which
   * is why it is stated per row. Tail metadata stays after every positional
   * field so old rows safely receive zero defaults. */
  bool modern_env;
  /* Explicit exception to category/type-derived debug visibility. Numeric 3D
   * rows are normally authoring controls, but a small number are intentional
   * player-facing gameplay choices. Keep that policy on the descriptor instead
   * of teaching Settings_IsDebugOnly key strings. Must remain a tail field for
   * the positional-row compatibility described above. */
  bool player_visible;
};

typedef enum {
  kSettingChange_Rejected = -1,
  kSettingChange_Unchanged = 0,
  kSettingChange_Applied = 1,
  kSettingChange_AppliedStickyDisable = 2,
  kSettingChange_RestartPending = 3,
} SettingChangeResult;

typedef void (*SettingsChangeObserver)(const SettingDesc *desc,
                                       SettingChangeResult result);
typedef bool (*SettingsActionObserver)(const SettingDesc *desc);

typedef struct SettingsPin {
  uint32 off;
  uint8 val;
} SettingsPin;

typedef struct Settings {
  int display_mode;
  /* Absolute host-output HUD scale percent. 0 follows the game's current
   * presentation scale; 100 means one output pixel per SNES pixel vertically. */
  int hud_scale_percent;
  /* Independent host settings-menu content scale. 0 auto-fits the complete
   * output window in 0.25x steps; 100 is one source pixel per output pixel. */
  int menu_scale_percent;
  /* Master toggle for manifest-driven HD graphics replacements
   * (game-assets/manifest.ini). The menu disables it when no art loaded. */
  bool hd_replacements;

  /* Application presentation settings. Video buffers reserve the PPU's
   * maximum width, so screen/pixel aspect changes can select a new live
   * render width without reallocating emulated state. */
  int extended_aspect;
  int pixel_aspect;
  int window_scale;         /* windowed client-size multiple */
  int window_mode;          /* WindowMode: windowed / borderless / exclusive */
  bool new_renderer;
  /* Load-only compatibility alias. Runtime code must use
   * Settings_IgnoreAspectRatio(), derived from extended_aspect. */
  bool ignore_aspect_ratio;
  int refresh_mode;         /* RefreshMode: vsync / unlimited / limit */
  int frame_limit_fps;      /* target FPS when refresh_mode == Limit */

  /* Audio controls. The SDL callback consumes an atomic mirror of the master
   * value; the game-thread COP hook reads the dialogue toggle directly. */
  bool audio_enabled;
  int  audio_frequency;      /* AudioFrequency preset */
  int  audio_samples;
  int  audio_master_volume;  /* final host PCM gain, 0..100 percent */
  bool audio_dialog_blip;    /* per-glyph Sky Palace dialogue sound */
  /* Master toggle for manifest-driven music replacement ([music:] entries of
   * game-assets/manifest.ini). Silently inert when the manifest/audio files
   * are absent. Live: turning it off mid-song stops the stream and unmutes
   * the SPC driver's music voices; the next song change is fully authentic. */
  bool music_replacements;

  /* Extras: gameplay enhancements and host-tool command parameters. These
   * persist; corresponding ACTION rows are host commands and are never
   * serialized. */
  bool fix_bridge_limit;  /* migrate completed bridges to the SRAM extension
                             area so they stop consuming 128-cap records */
  int turbo_multiplier;
  uint16 warp_target;
  bool scene_inspector;      /* click-to-inspect live PPU/asset identity */
  /* In-game manual: two-up openings rather than one page at a time. On by
   * default because artwork -- maps especially -- is drawn across the gutter,
   * so single-page is the mode that CUTS pictures in half. Kept as a choice
   * rather than removed: a wide, short manual reads better one page at a time,
   * since a two-up spread of it is nearly 3:1 and fits to a strip. */
  bool manual_spreads;
  /* Reveals developer-only rows in the settings overlay — the diorama/town
   * numeric tuning dials, layer A/B toggles, and the scene inspector tools.
   * Off (default) keeps the menu to the master toggles and major on/off
   * effects a player tunes for performance. See Settings_IsDebugOnly. */
  bool show_debug_settings;

  /* Battery-save preferences and staged verified field edits. The active
   * backend is snapshotted when the save system attaches at boot. Region
   * values are SaveProgressEdit selectors; zero never mutates SRAM. */
  int save_backend;          /* SaveBackend numeric value */
  bool save_edit_armed;
  bool save_autobackup;
  int save_editor_page;
  int save_region_progress[6];
  /* Zero leaves the field untouched. Fields whose real range includes zero
   * store real+1 and use a formatter to keep that staging sentinel distinct. */
  int save_master_level;
  int save_master_hp;
  int save_master_mp;
  int save_lives;
  int save_angel_sp_current;
  int save_angel_sp_max;
  int save_angel_hp_current;
  int save_angel_hp_max;
  int save_message_speed;
  char save_player_name[9];
  int save_professional_mode;
  int save_death_heim_state;
  int save_equipped_magic;
  int save_magic_slots[4];
  int save_item_slots[8];
  int save_scores[6][2];

  /* Cheat values. Zero/false means disabled. Stateful enforcement latches are
   * deliberately kept private to ActRaiser_ApplyCheats, not stored here. */
  bool cheat_all_magic;
  bool cheat_ranged_sword;
  int  cheat_inf_mp;
  bool cheat_inf_sp;
  bool cheat_angel_hp;
  int  cheat_inf_hp;
  bool cheat_freeze_timer;
  bool cheat_moonjump;
  int  cheat_moonjump_speed;
  bool cheat_no_knockback;   /* full-invuln "ignore hits"; on/off */
  /* Arms the "Cycle magic spell" input action (kInputAction_MagicCycle). The
   * binding alone does nothing; this is the switch that makes it live, and
   * while it is on the presentation layer shows a cheat badge so a recording
   * or bug report can never be mistaken for stock behaviour. */
  bool cheat_magic_cycle;
  uint8 pin_count;
  SettingsPin pins[32];

  /* Randomizer (src/randomizer.c). Every field is read only by the ROM-image
   * transform, so changing one re-runs the transform rather than moving a
   * per-frame gate: stat edits land at the next spawn, placement edits at the
   * next level load. Master defaults OFF so the stock image stays byte-exact
   * for the A/B visual-regression harness. */
  bool rando_enable;
  int  rando_seed;
  int  rando_enemy_hp;        /* percent of stock HP,  100 = unchanged */
  int  rando_enemy_atk;       /* percent of stock ATK, 100 = unchanged */
  int  rando_enemy_types;     /* RandomizerMode */
  int  rando_enemy_scope;     /* RandomizerScope */
  int  rando_statue_drops;    /* RandomizerMode */
  int  rando_statue_spots;    /* RandomizerMode (shuffle only in practice) */
  int  rando_lair_spots;      /* RandomizerMode */
  int  rando_lair_types;      /* RandomizerMode */

  /* Widescreen behavior. All default ON; the per-frame gates read these. */
  bool ws_action;             /* AR_WS_ACTION            action stages wide */
  bool ws_sim;                /* AR_WS_SIM               sim town wide */
  /* Load-only compatibility alias. Action true-content margins are owned by
   * the default HLE provider; runtime code must not consume this field. */
  bool ws_bgrefresh;          /* retired AR_WS_BGREFRESH */
  bool ws_skypalace_bg;       /* AR_WS_SKYPALACE_BG      sky palace BG2 repair */
  bool ws_sprites;            /* AR_WS_SPRITES           widen sprite emission */
  bool ws_margin_objects;     /* AR_WS_MARGIN_OBJECTS    draw margin objects */
  bool ws_margin_activation;  /* AR_WS_MARGIN_ACTIVATION extend $0400 window */
  bool ws_bg2_padding;        /* AR_WS_BG2_MIRROR        pad decorative BG2 */
  bool ws_sim_sprites;        /* AR_WS_SIM_SPRITES       widen sim components */

  /* Simulation-town 3D presentation.  The master and two masks are captured
   * into each FrameSlot; render stages consume only their resolved effective
   * masks.  D1 exposes the controls while the implemented-capability mask is
   * still zero, so every selection safely resolves to authentic output. */
  bool sim3d_mode;
  /* Extra host-renderable range around the authentic sim window. Raising it
   * also extends projectile lifetime and can increase world-record pressure,
   * so this is intentionally labelled gameplay-affecting. */
  int sim_view_range;
  /* Inter-town map $09 as a forced-top-down 3D scene. Independent of the town
   * master because it has no town canvas, separated layers, or free camera. */
  bool sim3d_world_navigation;
  /* World-navigation effects have their own stage gates. They share compatible
   * numeric tuning with town 3D, but never inherit its master or its
   * sprite-window/cull assumptions. */
  bool sim3d_world_navigation_lighting;
  bool sim3d_world_navigation_clouds;
  /* The enhanced renderer's stages. These toggles are the only stored form:
   * there is no feature mask setting and no A/B profile pair. Comparing two
   * builds of the scene means toggling stages across separate runs, which is
   * what the checkpoints do. */
  bool sim3d_separated_composite;
  bool sim3d_ground_projection;
  bool sim3d_object_billboards;
  bool sim3d_virtual_height;
  bool sim3d_shadows;
  bool sim3d_soft_shadows;
  bool sim3d_rim_light;
  bool sim3d_effect_lighting;
  bool sim3d_particles;
  bool sim3d_world_underlay;
  bool sim3d_cloud_shroud;
  bool sim3d_cull_haze;
  bool sim3d_backdrop;
  bool sim3d_picker_exit_ease;
  uint16 sim3d_diagnostic_layers;
  int sim3d_tilt_x_mrad;       /* camera pitch, milliradians */
  int sim3d_tilt_y_mrad;       /* camera yaw, milliradians */
  int sim3d_distance_x100;     /* camera distance, hundredths; 0 = auto */
  int sim3d_height_scale_x100; /* virtual-height scale, percent of the
                                * classified plane; 100 = catalogue default */
  int sim3d_shadow_opacity_pct; /* ground shadow darkness, percent; 0 = off */
  int sim3d_height_pop_pct;      /* extra billboard scale at the catalogue
                                  * flight plane, percent; 0 = true perspective */
  int sim3d_light_azimuth_deg;   /* direction the shadow is thrown */
  int sim3d_light_elevation_deg; /* 90 = straight overhead, no offset */
  int sim3d_shadow_softness_pct; /* D4b blur radius; 0 = hard shadow */
  int sim3d_rim_strength_pct;    /* D4c rim contribution; 0 = unlit sprites */
  int sim3d_underlay_haze_pct;   /* world-map underlay fade; 100 = hidden */
  int sim3d_cloud_opacity_pct;   /* shroud density; 0 = no clouds */
  int sim3d_cloud_falloff_px;    /* clear-to-full ramp, authentic px */
  int sim3d_cloud_inset_px;      /* ramp start inside the edge, px */
  int sim3d_cull_lead_px;        /* per-record cover lead before the edge, px */
  int sim3d_cull_haze_pct;       /* out-of-range town->underlay crossfade, % */
  int sim3d_cull_dim_pct;        /* out-of-range darkening toward black, % */
  int sim3d_cull_haze_lead_px;   /* haze ramp width before the edge, px */
  int sim3d_cull_corner_px;      /* lit-window corner radius, px */
  int sim3d_underlay_defocus_pct;/* world map focus falloff strength, % */
  int sim3d_cloud_altitude_px;   /* shroud height above the ground plane, px */
  int sim3d_cloud_drift_pct;     /* shroud drift rate, % of base velocity */
  bool sim3d_cull_lift_inset;    /* inset lit window by the max draw lift */
  int sim3d_backdrop_strength_pct;/* sky gradient departure from flat, % */
  int sim3d_backdrop_horizon_pct; /* synthetic horizon, % of viewport height */
  int sim3d_camera_mode;         /* SimCameraMode: free vs dynamic */
  int sim3d_dyncam_baseline_tilt_x_mrad;
  int sim3d_dyncam_baseline_tilt_y_mrad;
  int sim3d_dyncam_baseline_distance_x100;
  int sim3d_reactive_strength;   /* sim lean + kick scale, % */

  /* Portable action-stage spell polish. Independent of Town 3D and diorama:
   * the same captured lifecycle is projected through either action renderer. */
  bool action_effect_lighting;
  bool action_effect_particles;

  /* Diorama 3D presentation. Camera angles are scaled ints (no float setting
   * type); the live DioramaCamera is seeded from these and writes back on
   * every adjustment, so the menu and the mouse controls share one source of
   * truth. Engages only in action stages, and only on the new PPU path. */
  bool diorama_mode;
  int  diorama_tilt_x_mrad;      /* camera pitch, milliradians */
  int  diorama_tilt_y_mrad;      /* camera yaw, milliradians */
  int  diorama_distance_x100;    /* camera distance, hundredths */
  int  diorama_depth_shade;      /* % strength of per-plane depth shading */
  int  diorama_vertical_extend;  /* scanlines of world drawn above the screen */
  bool diorama_layer_bg1;
  bool diorama_layer_bg2;
  bool diorama_layer_bg3;
  bool diorama_layer_obj;
  bool diorama_layer_backdrop;
  /* A5 (followup doc): true (default) = BG3 excluded from the diorama
   * capture, drawn via the anchored PresentHudOverlayComposited path (A7) —
   * flat, widescreen-spread, readable. false = BG3 captured as a diorama
   * layer and drawn as an unanchored tilted plane (today's pre-A7 look) —
   * kept as an A/B curiosity, not a real anchored alternative (see A5's
   * load-bearing constraint: screen-space anchored rects can't be projected
   * onto a tilted mesh). */
  bool diorama_hud_flat;
  /* B4-mode (followup doc): DioramaCameraMode selector. */
  int diorama_camera_mode;
  /* B4-baseline (followup doc): Dynamic Cam's OWN dedicated pose, separate
   * from the Free-Cam angle above — Dynamic Cam sways around this, not
   * around whatever Free Cam was last left at. Same mrad/x100 scaled-int
   * convention as diorama_tilt_x/y_mrad/diorama_distance_x100. Defaults are
   * a gentle 3/4 tilt with symmetric left/right room (tilt_y=0) and the same
   * auto-fit distance sentinel (0) as the free-cam default. */
  int diorama_dyncam_baseline_tilt_x_mrad;
  int diorama_dyncam_baseline_tilt_y_mrad;
  int diorama_dyncam_baseline_distance_x100;
  /* B4 (followup doc): 0-100, scales every reactive offset (velocity-lean,
   * positional pan, event kicks — later checkpoints); 0 disables sway
   * entirely, snapping to the baseline pose above. */
  int diorama_reactive_strength;
  /* B5 (followup doc): DioramaSkyMode selector — see the enum comment. */
  int diorama_skybox;
  /* Margin fix (SPEC-backdrop-clip.md): at a level's start/end the live
   * widescreen margin collapses to 0, but every diorama consumer samples the
   * FIXED capture span — so the never-rendered columns showed as an opaque
   * black wedge at the screen edge. On: pad captured layers out to the full
   * budget (Fix A), fill the framebuffer's gap strips with the scene backdrop
   * instead of black (Fix C), and crop the skybox quad's UV to BG2's actually
   * valid span where padding cannot reach it (Fix B). Off restores every
   * pre-fix path exactly, so this is a live A/B for the artifact. */
  bool diorama_margin_fix;
  /* B6 (followup doc): put the layer stack inside a floor/ceiling/side-wall
   * enclosure so the level's off-screen edges are masked by box surfaces
   * instead of ending in void. Composes with B5: skybox fills the box's far
   * opening. Independent toggle so each can be A/B'd alone. */
  bool diorama_shoebox;

  /* Graphics (kSettingCat_Graphics, M8/M7 GPU shader + interpolation work).
   * gpu_shaders_enabled picks SDL's "gpu" renderer backend at boot — fixed
   * for the process lifetime (kApply_Restart). The per-effect toggles below
   * apply live (diorama.c rereads g_settings every frame, same pattern as
   * the diorama_layer_* toggles above) and are only meaningful/available
   * once gpu_shaders_enabled actually took effect (Settings_IsAvailable
   * gates on the real runtime g_gpu_shaders_active, not just this flag, in
   * case backend creation silently fell back). gpu_fx_shadow defaults OFF:
   * a known visual bug (shadow blur can bleed onto transparent gaps in a
   * receiving layer) keeps it opt-in until fixed — see diorama.c.
   * gpu_interp_enabled also defaults OFF, but as of B1b (followup doc) its
   * source bug is fixed (present.c ComputeDioramaScrollDelta now reads the
   * stable WRAM camera, not the HDMA-polluted PPU scroll registers) — OFF
   * pending in-game confirmation the fix is stable, not a known issue. */
  bool gpu_shaders_enabled;
  bool gpu_fx_rim;
  bool gpu_fx_dof;
  bool gpu_fx_edgeaa;
  bool gpu_fx_shadow;
  bool gpu_interp_enabled;

  /* CRT post-process (kSettingCat_Crt, the Video > CRT tab).
   *
   * Unlike the gpu_fx_* effects above — which are diorama-only — this is a
   * fullscreen pass over the finished frame, so it applies to every render
   * mode at once (flat, diorama, sim3D, world navigation). It needs the same
   * "gpu" backend, so its rows gate on GpuShadersActive too.
   *
   * All knobs are x100 fixed point because the settings system is integer
   * only; crt_post.c divides by 100 on the way to the shader. The defaults
   * are the values that were tuned by eye against Fillmore Act 1 — treat
   * them as the shipped look, not placeholders.
   *
   * Every knob except the master toggle is developer-only (Settings_IsDebugOnly
   * hides Int rows in this category): they are for authoring the look, not
   * things a player tunes for performance. */
  bool crt_enabled;
  int crt_curvature_x100;    /* barrel warp; 0 = flat glass                  */
  int crt_scanline_x100;     /* beam darkening between source scanlines      */
  int crt_mask_x100;         /* aperture-grille phosphor tint                */
  int crt_aberration_x100;   /* RGB split, in output pixels                  */
  int crt_bandwidth_x100;    /* horizontal signal smear, in SOURCE pixels    */
  int crt_vignette_x100;     /* corner falloff                               */
  int crt_brightness_x100;   /* lifts the darkening mask+scanlines cause     */
  /* Load-only compatibility alias for refresh_mode == Unlimited. */
  bool uncapped_framerate;

  /* Input mapping. The dimensions mirror input_map.h's InputClass /
   * InputAction enums (statically asserted there); they are spelled out here
   * so settings.h stays free of the SDL dependency input_map.h carries. */
  uint32 input_bind[kSettingsInputClasses][kSettingsInputActions];
  int input_device;           /* InputDeviceMode: auto / keyboard / gamepad */
  int input_gamepad_slot;     /* 0 = first connected, else 1-based slot */
  int input_bind_page;        /* which class the Input category lists */
  int input_stick_deadzone;   /* percent of full stick travel */
  bool input_stick_as_dpad;
  int input_cam_deadzone;     /* percent, for the analog camera actions */
  int input_cam_sensitivity;  /* percent of the base orbit/zoom rate */
  bool input_cam_invert_y;
} Settings;

extern Settings g_settings;
extern const SettingDesc g_setting_descs[];
extern const int g_setting_desc_count;

/* Boot-layer staging. config.ini is parsed first and stages known registry
 * values here; Settings_InitWithFile then resolves:
 * defaults < config.ini < settings.ini < real process environment.
 * Unknown diagnostic AR_* and SNESREF_* keys continue through config.c's legacy
 * environment bridge. */
void Settings_ClearConfigLayer(void);
bool Settings_StageConfigValue(const char *key, const char *value);
bool Settings_StageConfigEnvironment(const char *env, const char *value);

/* Settings_Init is the test/tool convenience path and finalizes against the
 * current g_ws budget. The game uses InitWithFile before allocating that
 * budget, then calls FinalizeDisplayMode once g_ws_active is authoritative. */
void Settings_Init(void);
void Settings_InitWithFile(const char *path);
void Settings_FinalizeDisplayMode(void);

/* Descriptor-driven persistence. Load ignores unknown keys for forward
 * compatibility and returns false on I/O or parse errors. Save replaces the
 * target atomically and never rewrites the developer-owned config.ini. */
bool Settings_Load(const char *path);
/* Turn off every Settings_Save write for the process. A replay is a DIAGNOSTIC
 * run and must not mutate the player's configuration -- the same reason
 * InputReplay_ShouldProtectSaveData refuses to persist SRAM. Settings were not
 * covered by that, and the gap was not theoretical: with
 * `diorama_camera_mode = Dynamic Cam` the dynamic camera drifts its own baseline
 * during a long replay and the 500 ms flush in Diorama_FlushSettingsIfDirty
 * wrote that drift to settings.ini (observed 2026-07-27:
 * diorama_dyncam_baseline_distance_x100 moved 294 -> 330 purely from replaying).
 * Guarded inside Settings_Save so none of its callers can miss it. */
void Settings_SetPersistenceEnabled(bool enabled);

bool Settings_Save(const char *path);

/* Descriptor/mutation API used by the host overlay and settings.ini loader.
 * All runtime writes go through these functions so range normalization,
 * profile invalidation, callbacks, and sticky/restart results stay uniform. */
const SettingDesc *Settings_Find(const char *key);
bool Settings_IsAvailable(const SettingDesc *desc);
bool Settings_IsMenuVisible(const SettingDesc *desc);
/* True for a developer-only row: the fine numeric tuning dials of the diorama
 * and town 3D renderers, their internal layer/stage A/B toggles, and the scene
 * inspector tools. Hidden from the menu unless g_settings.show_debug_settings
 * is on. Master mode toggles, major on/off effects, and camera mode/reset stay
 * visible either way. */
bool Settings_IsDebugOnly(const SettingDesc *desc);
bool Settings_GetLong(const SettingDesc *desc, long *value);
SettingChangeResult Settings_SetLong(const SettingDesc *desc, long value);
SettingChangeResult Settings_SetText(const SettingDesc *desc, const char *text);
SettingChangeResult Settings_Reset(const SettingDesc *desc);
/* Restore every non-action descriptor in one registry category to its
 * built-in default. Returns the strongest result produced by the batch
 * (restart-required beats sticky-disable, which beats an ordinary change). */
SettingChangeResult Settings_ResetCategory(SettingCategory category);
int Settings_FormatValue(const SettingDesc *desc, char *buffer, int buffer_size);
void Settings_SetChangeObserver(SettingsChangeObserver observer);
void Settings_SetActionObserver(SettingsActionObserver observer);
bool Settings_InvokeAction(const SettingDesc *desc);
const char *Settings_CategoryName(SettingCategory category);
const char *Settings_ApplyKindName(SettingApplyKind apply);
const char *Settings_ChangeResultName(SettingChangeResult result);

/* Apply a deterministic display-mode preset over the ws_* flags. Presets
 * overwrite them; descriptor mutations of individual widescreen fields
 * automatically reclassify the resulting FULL/RAW/CUSTOM combination. */
void Settings_SetDisplayMode(int mode);
/* Reconcile a pre-change render profile after screen/PAR geometry updates.
 * Wide RAW/FULL/CUSTOM survives a wide-to-wide change; entering wide from 4:3
 * selects FULL, and losing the wide framebuffer selects authentic 4:3. */
void Settings_ReconcileDisplayModeAfterGeometryChange(int previous_mode);
int  Settings_CycleDisplayMode(void);
const char *Settings_DisplayModeName(int mode);

/* The framebuffer sub-rect the current mode should present: columns
 * [Settings_VisibleX0(), +Settings_VisibleWidth()). 4:3 presents only the
 * authentic centre 256 so the margins are cropped rather than shown as bars.
 * Host textures retain maximum capacity while the PPU uses the current active
 * width/pitch; changing ratios therefore requires no texture reallocation. */
int Settings_VisibleX0(void);
int Settings_VisibleWidth(void);

/* Diorama availability predicates, shared by the descriptor table and the
 * host render/hotkey paths so the gate has exactly one spelling (§D14). */
bool Diorama_ModeIsOn(void);
bool Diorama_NewPpuCapable(void);
bool Sim3D_ModeIsOn(void);
int Settings_ExtendedAspectX(void);
int Settings_ExtendedAspectY(void);
bool Settings_IgnoreAspectRatio(void);
int Settings_AudioFrequencyHz(void);

/* HD replacements are only actionable when at least one manifest image was
 * decoded and uploaded. The host publishes that resource state after load. */
void Settings_SetHdReplacementsAvailable(bool available);
bool Settings_HdReplacementsAvailable(void);

/* The host display's detected refresh rate in whole Hz (0 = unknown). main.c
 * pushes it from SDL at boot and whenever the window's display/mode changes;
 * the overlay shows it next to the Vsync refresh option so "Vsync" is not a
 * blind label. Purely informational — it drives no behavior. */
void Settings_SetHostRefreshHz(int hz);
int Settings_HostRefreshHz(void);

/* Whether vsync is actually active on the renderer (read back from SDL, not
 * merely requested — SDL_SetRenderVSync can be rejected by a backend). */
void Settings_SetHostVsyncActive(bool active);
bool Settings_HostVsyncActive(void);

/* Backing pixels per window point for the window's current display
 * (SDL_GetWindowPixelDensity), pushed from main.c. */
void Settings_SetHostPixelDensity(float density);
float Settings_HostPixelDensity(void);

/* Convert a user-pinned scale percentage (hud_scale_percent,
 * menu_scale_percent — both defined as SNES/source pixels per OUTPUT pixel)
 * into the physical-pixel percentage the renderer needs, now that
 * SDL_WINDOW_HIGH_PIXEL_DENSITY makes the output size physical. A pinned 100%
 * keeps its apparent size on a 2x display instead of halving. Percent 0 (auto)
 * passes through untouched — those paths derive from the output size and are
 * already density-correct. */
int Settings_ScalePercentToOutput(int percent);
/* Folds the SIM 3D stage toggles into the one mask the resolver and the frame
 * payload work in. The toggles are the only stored state; no mask is
 * persisted, so this is the single conversion point. */
SimRenderFeatureMask Settings_Sim3DRequestedFeatures(void);

#endif  /* SETTINGS_H */
