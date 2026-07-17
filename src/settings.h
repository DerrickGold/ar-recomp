#pragma once
#include "types.h"

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
  kScreenAspect_Count,
} ScreenAspect;

/* Host audio-rate presets. The stored value is the stable menu/config enum;
 * Settings_AudioFrequencyHz translates it for SDL device creation. */
typedef enum {
  kAudioFrequency_32040 = 0,
  kAudioFrequency_44100,
  kAudioFrequency_48000,
  kAudioFrequency_Count,
} AudioFrequency;

typedef enum {
  kSettingType_Bool,
  kSettingType_Int,
  kSettingType_Enum,
  kSettingType_Mask,
  kSettingType_Custom,
  kSettingType_Action,
} SettingType;

typedef enum {
  kApply_Passive,
  kApply_Callback,
  kApply_Restart,
  kApply_Save,
  kApply_Action,
} SettingApplyKind;

typedef enum {
  kSettingCat_Cheats,
  kSettingCat_Widescreen,
  kSettingCat_Aspect,
  kSettingCat_Display,
  kSettingCat_Audio,
  kSettingCat_Save,
  kSettingCat_Qol,
} SettingCategory;

typedef enum SaveProgressEdit {
  kSaveProgressEdit_LeaveAsIs = 0,
  kSaveProgressEdit_Act1,
  kSaveProgressEdit_Act1Cleared,
  kSaveProgressEdit_Act2,
  kSaveProgressEdit_Act2Cleared,
  kSaveProgressEdit_Count,
} SaveProgressEdit;

typedef enum SaveEditorPage {
  kSaveEditorPage_Progress = 0,
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
   * (game-assets/manifest.ini). Silently inert when the manifest/art files
   * are absent or the run is headless (no overlay bindings). */
  bool hd_replacements;

  /* Application presentation settings. Video buffers reserve the PPU's
   * maximum width, so screen/pixel aspect changes can select a new live
   * render width without reallocating emulated state. */
  int extended_aspect;
  int pixel_aspect;
  int window_scale;
  bool fullscreen;
  bool new_renderer;
  bool ignore_aspect_ratio;

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

  /* Quality-of-life command parameters. These persist; the corresponding
   * ACTION rows are host commands and are never serialized. */
  int turbo_multiplier;
  uint16 warp_target;
  bool scene_inspector;      /* click-to-inspect live PPU/asset identity */

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
  int  cheat_moonjump_speed;
  uint16 cheat_moonjump_button;
  int  cheat_no_knockback;
  uint8 pin_count;
  SettingsPin pins[32];

  /* Widescreen behavior. All default ON; the per-frame gates read these. */
  bool ws_action;             /* AR_WS_ACTION            action stages wide */
  bool ws_sim;                /* AR_WS_SIM               sim town wide */
  bool ws_bgrefresh;          /* AR_WS_BGREFRESH         true-content margins */
  bool ws_skypalace_bg;       /* AR_WS_SKYPALACE_BG      sky palace BG2 repair */
  bool ws_sprites;            /* AR_WS_SPRITES           widen sprite emission */
  bool ws_margin_objects;     /* AR_WS_MARGIN_OBJECTS    draw margin objects */
  bool ws_margin_activation;  /* AR_WS_MARGIN_ACTIVATION extend $0400 window */
  bool ws_bg2_padding;        /* AR_WS_BG2_MIRROR        pad decorative BG2 */
  bool ws_sim_sprites;        /* AR_WS_SIM_SPRITES       widen sim components */
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
bool Settings_Save(const char *path);

/* Descriptor/mutation API used by the host overlay and settings.ini loader.
 * All runtime writes go through these functions so range normalization,
 * profile invalidation, callbacks, and sticky/restart results stay uniform. */
const SettingDesc *Settings_Find(const char *key);
bool Settings_IsAvailable(const SettingDesc *desc);
bool Settings_IsMenuVisible(const SettingDesc *desc);
bool Settings_GetLong(const SettingDesc *desc, long *value);
SettingChangeResult Settings_SetLong(const SettingDesc *desc, long value);
SettingChangeResult Settings_SetText(const SettingDesc *desc, const char *text);
SettingChangeResult Settings_Reset(const SettingDesc *desc);
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
int  Settings_CycleDisplayMode(void);
const char *Settings_DisplayModeName(int mode);

/* The framebuffer sub-rect the current mode should present: columns
 * [Settings_VisibleX0(), +Settings_VisibleWidth()). 4:3 presents only the
 * authentic centre 256 so the margins are cropped rather than shown as bars.
 * Host textures retain maximum capacity while the PPU uses the current active
 * width/pitch; changing ratios therefore requires no texture reallocation. */
int Settings_VisibleX0(void);
int Settings_VisibleWidth(void);
int Settings_ExtendedAspectX(void);
int Settings_ExtendedAspectY(void);
int Settings_AudioFrequencyHz(void);
