#include "actraiser/actraiser_ar2_master_internal.h"

#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_game.h"
#include "app/settings.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/support/utf8_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RecompReturn bank_00_8538_M0X0(CpuState *cpu);
extern RecompReturn bank_00_88C9_M0X0(CpuState *cpu);
extern RecompReturn bank_00_8A3C_M0X0(CpuState *cpu);
extern RecompReturn bank_00_8A3C_M1X0(CpuState *cpu);
extern RecompReturn bank_00_8970_M0X0(CpuState *cpu);
extern RecompReturn bank_00_8970_M1X0(CpuState *cpu);
extern RecompReturn bank_00_8FE7_M0X0(CpuState *cpu);
extern RecompReturn bank_00_9CF2_M0X0(CpuState *cpu);

/* ---- Native ActRaiser contracts this module relies on ---------------------
 * $9D4F is the shared player tail every AR1 control handler branches to: hurt
 * ($0008) and timer expiry divert to $9C47, invulnerability counts down,
 * $8F14 applies +06/+08 against the terrain, the camera deltas $7C/$7E and
 * focus $80/$82 update, the platform flag clears and +2A becomes the sword
 * attack (1, or 2 while $E4 holds the power-up). It ends in the dispatcher's
 * RTS. $9DE1 is the (regionally hooked) scroll-cast gate. $9CF1 is a lone RTS. */
enum {
  kNativePlayerTail = 0x009D4F,
  kNativeCastGate = 0x009DE1,
  kNativeReturn = 0x009CF1,
  kNativeLeafLabel = 0x009832,
  kControlHandler = 0x9832,
  kAr1Bottom = 24, /* feet below the AR1 player origin */
  kAr1HalfWidth = 16,
  kAr1TopStanding = 40, /* >= 25: $8970 tests the body from y-23 */
  kAr1TopLow = 16,      /* < 25: $8970 tests from y-7, the crouch height */
  kFlagAttacker = 0x0001,
  kFlagHurt = 0x0008,
  kFlagBusy = 0x0010, /* native cast/hurt immunity; AR2 dive invulnerability */
  kFlagInvulnerable = 0x2000,
  kFlagSlowZone = 0x8000,
  kFlipLeft = 0x4000,
  kBeamStanding = 0x13,
  kBeamCrouching = 0x14,
  kVictimCooldown = 16, /* AR2 victims are protected $10 updates after a hit */
};

/* SNES auto-joypad word; AR2 injects facing into the unused low bits. */
enum {
  kPadB = 0x8000,
  kPadY = 0x4000,
  kPadUp = 0x0800,
  kPadDown = 0x0400,
  kPadLeft = 0x0200,
  kPadRight = 0x0100,
  kPadA = 0x0080,
  kPadX = 0x0040,
  kFaceLeft = 0x0008,
  kFaceRight = 0x0004,
};

/* ---- Assets ------------------------------------------------------------- */
static uint8_t *s_pack_image;
static Ar2MasterAssets s_assets;
static bool s_available;

void ActRaiserAr2Master_Load(const char *asset_path) {
  ActRaiserAr2Master_Shutdown();
  FILE *file = asset_path ? sr_fopen(asset_path, "rb") : NULL;
  if (!file) return; /* Optional pack: absence is the normal AR1 installation. */
  long size = -1;
  if (!fseek(file, 0, SEEK_END)) size = ftell(file);
  if (size < 48 || size > kAr2MasterPackMaximumBytes || fseek(file, 0, SEEK_SET)) {
    fclose(file);
    return;
  }
  uint8_t *image = malloc((size_t)size);
  bool read = image && fread(image, 1, (size_t)size, file) == (size_t)size && fgetc(file) == EOF &&
              !ferror(file);
  if (fclose(file)) read = false;
  char error[160] = "cannot read character assets";
  if (read && Ar2MasterAssets_ParsePack(image, (size_t)size, &s_assets, error, sizeof(error)) &&
      Ar2MasterPresentation_Init(&s_assets, error, sizeof(error))) {
    s_pack_image = image;
    s_available = true;
    fprintf(stderr, "[ar2-master] Loaded %u character frames from %s\n",
            (unsigned)s_assets.frame_count, asset_path);
    return;
  }
  fprintf(stderr, "[ar2-master] %s ignored: %s\n", asset_path, error);
  memset(&s_assets, 0, sizeof(s_assets));
  free(image);
}

bool ActRaiserAr2Master_Available(void) { return s_available; }

const Ar2MasterAssets *ActRaiserAr2Master_Assets(void) { return s_available ? &s_assets : NULL; }

/* ---- Controller state ---------------------------------------------------
 * One instance: ActRaiser has a single player slot. States are named after
 * the AR2 script address that implements them. */
typedef enum Ar2State {
  kSt_IdleDispatch,        /* $8076 */
  kSt_Idle,                /* $809A */
  kSt_StandSlash,          /* $80F6 */
  kSt_CrouchFaceRight,     /* $8152 */
  kSt_CrouchFaceLeft,      /* $8156 */
  kSt_CrouchKeepAttack,    /* $815A */
  kSt_Crouch,              /* $8160 */
  kSt_WalkRight,           /* $81A4 */
  kSt_WalkLeft,            /* $81A8 */
  kSt_Walk,                /* $81AA */
  kSt_LookUpFaceRight,     /* $8200 */
  kSt_LookUpFaceLeft,      /* $8204 */
  kSt_LookUpKeepAttack,    /* $8208 */
  kSt_LookUp,              /* $820E */
  kSt_StandJump,           /* $825E */
  kSt_StandJumpHigh,       /* $829F */
  kSt_StandJumpMid,        /* $82CB */
  kSt_StandJumpLow,        /* $82F7 */
  kSt_CrouchJump,          /* $8323 */
  kSt_CrouchSlash,         /* $835D */
  kSt_RunJump,             /* $83AD */
  kSt_RunJumpHigh,         /* $83E8 */
  kSt_RunJumpMid,          /* $8414 */
  kSt_RunJumpLow,          /* $8440 */
  kSt_UpSlash,             /* $846C */
  kSt_AirSlash,            /* $84BC */
  kSt_FallFromWalk,        /* $8534 */
  kSt_Fall,                /* $8563 */
  kSt_Land,                /* $8592 */
  kSt_DoubleJump,          /* $85E0 */
  kSt_RunDoubleJump,       /* $861B */
  kSt_GlideStart,          /* $8646 */
  kSt_GlideLoopFaceRight,  /* $8666 */
  kSt_GlideLoopFaceLeft,   /* $866A */
  kSt_GlideLoop,           /* $866C */
  kSt_GlideFaceRight,      /* $86B6 */
  kSt_GlideFaceLeft,       /* $86BA */
  kSt_Glide,               /* $86BC */
  kSt_Skid,                /* $870D */
  kSt_FloatFaceRight,      /* $8731 */
  kSt_FloatFaceLeft,       /* $8735 */
  kSt_Float,               /* $8737 */
  kSt_DiveFaceRight,       /* $8772 */
  kSt_DiveFaceLeft,        /* $8776 */
  kSt_Dive,                /* $8778 */
  kSt_DownThrust,          /* $87AD */
  kSt_RunDownThrust,       /* $87B8 */
  kSt_DownThrustHold,      /* $87C1 */
  kSt_GlideLoopSlash,      /* $87F4 */
  kSt_DiveAttackFaceRight, /* $882C */
  kSt_DiveAttackFaceLeft,  /* $8830 */
  kSt_DiveAttack,          /* $8832 */
  kSt_GlideSlash,          /* $8894 */
  kSt_None = 0xFF,
} Ar2State;

typedef struct Ar2Event {
  uint16_t mask;
  uint8_t target;
  bool release;
} Ar2Event;

enum { kMaxEvents = 12, kMaxVictims = 8 };

typedef struct Ar2Victim {
  uint16_t object;
  uint16_t frame;
} Ar2Victim;

typedef struct Ar2Master {
  bool active;
  uint16_t last_frame; /* game frame of the last controlled update */
  uint8_t state, pc;
  int16_t timer;      /* +0E: script waits until this underflows */
  uint8_t anim, step; /* +$7F0008 and +18 */
  int frame;          /* displayed frame, -1 none */
  Ar2MotionAxis motion_x, motion_y;
  int16_t motion_dx; /* $7F0022: this frame's program dx */
  Ar2Event events[kMaxEvents];
  uint8_t event_count;
  uint16_t require_release;                 /* $0603 */
  uint8_t on_land, on_ground_lost, on_wall; /* $7F1000/$1004/$1006 */
  bool airborne;                            /* +06 & $0200 */
  bool keep_motion;                         /* +06 & $0080 */
  bool invulnerable;                        /* +04 & $0040 */
  uint8_t attack;                           /* $7F2016: 0 is the shield (ignored) */
  uint8_t loop_counter;                     /* +24 */
  bool facing_left;
  uint8_t beam; /* pending AR1 power-up beam for this swing */
  /* Physics bookkeeping between frames. */
  int16_t last_dx, last_dy, intended_dx;
  uint16_t x_before;
  bool grounded;
  /* The current frame's box in world coordinates: the sword's reach for the
   * hit pass, or the shield ($7F2016 attack type 0) for the contact pass. */
  bool attack_active, shield_active;
  int box_x0, box_y0, box_x1, box_y1;
  Ar2Victim victims[kMaxVictims];
  bool shield_contacts[kActRaiserActionObjectCount];
} Ar2Master;

static Ar2Master s_master = {.state = kSt_None, .frame = -1};
static bool s_stage_enabled; /* controller selection, sampled by presentation at each sprite pass */
static uint16_t s_latched_map = 0xFFFF;
static bool s_hit_delegate;

static uint16_t Read16(const CpuState *cpu, uint16_t address) {
  return cpu_read16((CpuState *)cpu, 0, address);
}
static void Write16(CpuState *cpu, uint16_t address, uint16_t value) {
  cpu_write16(cpu, 0, address, value);
}
static uint16_t Field(const CpuState *cpu, uint16_t object, unsigned offset) {
  return Read16(cpu, (uint16_t)(object + offset));
}
static void SetField(CpuState *cpu, uint16_t object, unsigned offset, uint16_t value) {
  Write16(cpu, (uint16_t)(object + offset), value);
}

static bool InActionStage(const CpuState *cpu) {
  return ActRaiser_IsActionMapGroup(cpu_read8((CpuState *)cpu, 0, kActRaiserWram_MapGroup));
}

static bool SettingEnabled(void) { return s_available && g_settings.ar2_master; }

/* Apply at a game update, never independently in NMI: OAM and its tiles must
 * change together. A live AR2 controller gets one final call to return native
 * control; hurt/cast/death/arrival continue under their existing native owner. */
static void UpdateControllerSelection(const CpuState *cpu) {
  const uint16_t map = (uint16_t)(cpu_read8((CpuState *)cpu, 0, kActRaiserWram_MapGroup) << 8 |
                                  cpu_read8((CpuState *)cpu, 0, kActRaiserWram_CurrentMap));
  if (map != s_latched_map) {
    s_master = (Ar2Master){.state = kSt_None, .frame = -1};
    s_latched_map = map;
  }
  const uint16_t handler = Read16(cpu, kActRaiserWram_PlayerHandler);
  if (handler < kControlHandler || handler > 0x9BEE) s_master.active = false;
  if (!s_master.active) s_stage_enabled = SettingEnabled();
}

void ActRaiserAr2Master_UpdatePresentation(const CpuState *cpu) {
  if (!s_available || !cpu) return;
  UpdateControllerSelection(cpu);
  const uint16_t game_frame = Read16(cpu, kActRaiserWram_GameFrame);
  const bool controlled = s_master.active && (uint16_t)(game_frame - s_master.last_frame) <= 1;
  Ar2MasterPresentation_Update(cpu, s_stage_enabled, controlled ? s_master.frame : -1);
}

void ActRaiserAr2Master_Shutdown(void) {
  Ar2MasterPresentation_Reset();
  free(s_pack_image);
  s_pack_image = NULL;
  s_available = false;
  memset(&s_assets, 0, sizeof(s_assets));
  s_master = (Ar2Master){.state = kSt_None, .frame = -1};
  s_stage_enabled = false;
  s_latched_map = 0xFFFF;
}

static void Leaf(CpuState *cpu, RecompReturn (*function)(CpuState *)) {
  if (!cpu_invoke_rts_leaf(cpu, function, kNativeLeafLabel))
    ActRaiserHleFatal("AR2 master helper violated its synchronous RTS contract");
}

static RecompReturn Tail(uint32_t target, uint32_t source) {
  if (!cpu_hle_tailcall_request(target, source))
    ActRaiserHleFatal("AR2 master has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}

/* ---- AR2 script primitives ---------------------------------------------- */
static const Ar2MasterFrame *CurrentFrame(void) {
  return s_master.frame >= 0 ? &s_assets.frames[s_master.frame] : NULL;
}

static void ClearEvents(Ar2Master *m) { m->event_count = 0; }

static void On(Ar2Master *m, uint16_t mask, uint8_t target) {
  if (m->event_count < kMaxEvents) m->events[m->event_count++] = (Ar2Event){mask, target, false};
}

static void OnRelease(Ar2Master *m, uint16_t mask, uint8_t target) {
  if (m->event_count < kMaxEvents) m->events[m->event_count++] = (Ar2Event){mask, target, true};
}

static void StopMotion(Ar2Master *m) {
  /* $80:ACEB: timer, both programs, this frame's deltas and the player's
   * input events. */
  m->timer = 0;
  Ar2MotionAxis_Start(&m->motion_x, 0);
  Ar2MotionAxis_Start(&m->motion_y, 0);
  m->motion_dx = 0;
  ClearEvents(m);
}

/* COP $1C */
static void Reset(Ar2Master *m) {
  StopMotion(m);
  m->on_land = m->on_ground_lost = m->on_wall = kSt_None;
  m->attack = 1;
  m->invulnerable = false;
  m->airborne = false;
  m->keep_motion = false;
}

/* COP $1D */
static void ResetInput(Ar2Master *m) {
  ClearEvents(m);
  m->on_land = m->on_ground_lost = m->on_wall = kSt_None;
  m->airborne = false;
  m->keep_motion = false;
}

static void SetAnim(Ar2Master *m, uint8_t anim) {
  m->anim = anim < kAr2MasterAnimCount ? anim : 0;
  m->step = 0;
}

static void SetAnimMotion(Ar2Master *m, uint8_t anim, uint8_t motion) {
  SetAnim(m, anim);
  uint16_t x, y;
  Ar2MasterAssets_MotionPrograms(&s_assets, motion, &x, &y);
  Ar2MotionAxis_Start(&m->motion_x, x);
  Ar2MotionAxis_Start(&m->motion_y, y);
}

/* $80:A02A. Loads the next step and returns false, or handles the end of the
 * animation and returns true. */
static bool Advance(Ar2Master *m) {
  const Ar2MasterAnim *anim = &s_assets.anims[m->anim];
  if (m->step >= anim->step_count) {
    m->step = 0;
    if (!m->keep_motion) { /* $80:ACDB */
      StopMotion(m);
      m->on_land = m->on_ground_lost = kSt_None;
    }
    return true;
  }
  m->frame = anim->steps[m->step].frame;
  m->timer = (int16_t)anim->steps[m->step].duration;
  m->step++;
  return false;
}

static void FaceRight(Ar2Master *m) { m->facing_left = false; }
static void FaceLeft(Ar2Master *m) { m->facing_left = true; }

static uint16_t Held(const CpuState *cpu) {
  return (uint16_t)(Read16(cpu, 0x4218) & Read16(cpu, kActRaiserWram_InputEnableMask));
}

/* $05FF: held input that is not waiting for release, plus the facing bits. */
static uint16_t Pressed(const CpuState *cpu, const Ar2Master *m) {
  uint16_t pressed = (uint16_t)(Held(cpu) & ~m->require_release);
  return (uint16_t)(pressed | (m->facing_left ? kFaceLeft : kFaceRight));
}

/* $82:8CC0: turn toward a held direction when an action starts. */
static void FaceHeld(const CpuState *cpu, Ar2Master *m) {
  const uint16_t pressed = Pressed(cpu, m);
  if (pressed & kPadRight)
    FaceRight(m);
  else if (pressed & kPadLeft)
    FaceLeft(m);
}

static bool All(uint16_t value, uint16_t mask) { return (value & mask) == mask; }

typedef enum StepResult { kYield, kContinue } StepResult;

static StepResult Goto(Ar2Master *m, uint8_t state) {
  m->state = state;
  m->pc = 0;
  return kContinue;
}

/* Reuse the native request hooks, including volume, extended channels and
 * diagnostics. $9A91 posts COP $01 for a sword swing; $8B24 posts BRK $0C
 * when an attack strikes a deflecting object. Preserve the caller's A/flags. */
static void NativeSound(CpuState *cpu, void (*hook)(CpuState *), uint8_t id, uint32_t site) {
  if (!hook) return;
  const uint16_t a = cpu->A;
  cpu_trace_block(cpu, site);
  cpu->A = (uint16_t)((a & 0xFF00) | id);
  hook(cpu);
  cpu->A = a;
}

static void NewSwing(CpuState *cpu, Ar2Master *m, uint8_t beam) {
  m->beam = beam;
  NativeSound(cpu, g_cpu_cop_hook, 0x01, 0x009A91);
}

static void GroundEvents(Ar2Master *m, bool walk_and_turn) {
  On(m, kPadUp | kPadY, kSt_UpSlash);
  On(m, kPadDown | kPadY, kSt_CrouchSlash);
  On(m, kPadY, kSt_StandSlash);
  On(m, kPadDown, kSt_Crouch);
  On(m, kPadUp, kSt_LookUp);
  On(m, kFaceRight | kPadRight | kPadB, kSt_RunJump);
  On(m, kFaceLeft | kPadLeft | kPadB, kSt_RunJump);
  On(m, kPadB, kSt_StandJump);
  if (walk_and_turn) {
    On(m, kPadRight, kSt_WalkRight);
    On(m, kPadLeft, kSt_WalkLeft);
  }
}

static void JumpEvents(Ar2Master *m, uint8_t double_jump, uint8_t thrust) {
  if (double_jump != kSt_None) On(m, kPadB, double_jump);
  On(m, kPadDown | kPadY, thrust);
  On(m, kPadY, kSt_AirSlash);
}

static void GlideEvents(Ar2Master *m, uint8_t face_right, uint8_t face_left, uint8_t slash) {
  On(m, kFaceLeft | kPadRight, face_right);
  On(m, kFaceRight | kPadLeft, face_left);
  On(m, kPadDown | kPadY, kSt_DiveAttack);
  On(m, kPadY, slash);
  On(m, kPadDown, kSt_Dive);
  On(m, kPadUp, kSt_Float);
  On(m, kPadB, kSt_FallFromWalk);
}

/* A falling/jumping state body: arm, then loop its animation. */
static StepResult AirLoop(Ar2Master *m, uint8_t self, uint8_t anim, uint8_t motion,
                          uint8_t double_jump, uint8_t thrust, uint8_t posture_unused) {
  (void)posture_unused;
  if (m->pc == 0) {
    Reset(m);
    m->airborne = true;
    JumpEvents(m, double_jump, thrust);
    m->on_land = kSt_Land;
    SetAnimMotion(m, anim, motion);
    m->pc = 1;
  }
  if (!Advance(m)) return kYield;
  return Goto(m, self);
}

/* ActRaiser's terrain query ($00:91C3): the BG1 metatile under a 16px tile,
 * from the page-major map (16x16 metatiles per 256 pixels of level width),
 * classed by the quadrant-solidity table built at level entry. */
enum {
  kLevelTilesWide = 0x0084,
  kLevelTilesHigh = 0x0086,
  kQuadrantSolidity = 0x05A0,
  kSolidityOpen = 0x00,
  kSolidityTopHalf = 0x03, /* a standable platform */
};

static uint8_t TileSolidity(CpuState *cpu, unsigned tile_x, unsigned tile_y) {
  const unsigned pages_across = Read16(cpu, kActRaiserWram_Bg1Width) >> 8;
  const unsigned at =
      ((tile_y >> 4) * pages_across + (tile_x >> 4)) * 256 + (tile_y & 15) * 16 + (tile_x & 15);
  const uint8_t metatile = cpu_read8(cpu, 0x7E, (uint16_t)(kActRaiserWram_Bg1Map + at));
  return cpu_read8(cpu, 0, (uint16_t)(kQuadrantSolidity + metatile));
}

/* Crouch-jump drop-through ($80:AB08): only a top-half platform tile under
 * the feet with open space below it. */
static bool CanDropThrough(CpuState *cpu, uint16_t player) {
  const unsigned feet = (unsigned)(Field(cpu, player, kActRaiserActionObject_WorldY) + kAr1Bottom);
  const unsigned tile_x = Field(cpu, player, kActRaiserActionObject_WorldX) >> 4;
  const unsigned tile_y = feet >> 4;
  if (tile_x >= Read16(cpu, kLevelTilesWide) || tile_y + 1 >= Read16(cpu, kLevelTilesHigh))
    return false;
  return TileSolidity(cpu, tile_x, tile_y) == kSolidityTopHalf &&
         TileSolidity(cpu, tile_x, tile_y + 1) == kSolidityOpen;
}

/* One pass of the AR2 master script from the current state and pc. */
static StepResult Step(CpuState *cpu, uint16_t player, Ar2Master *m) {
  switch (m->state) {
  case kSt_IdleDispatch: {
    const uint16_t pressed = Pressed(cpu, m);
    if (All(pressed, kPadUp | kPadY)) return Goto(m, kSt_UpSlash);
    if (All(pressed, kPadDown | kPadY)) return Goto(m, kSt_CrouchSlash);
    if (pressed & kPadY) return Goto(m, kSt_StandSlash);
    return Goto(m, kSt_Idle);
  }
  case kSt_Idle:
    if (m->pc == 0) {
      Reset(m);
      m->attack = 0;
      m->on_ground_lost = kSt_Fall;
      GroundEvents(m, true);
      SetAnim(m, 0x01);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_Idle);
  case kSt_StandSlash:
    if (m->pc == 0) {
      Reset(m);
      FaceHeld(cpu, m);
      m->on_ground_lost = kSt_Fall;
      On(m, kPadUp | kPadY, kSt_UpSlash);
      On(m, kPadDown | kPadY, kSt_CrouchSlash);
      On(m, kPadDown, kSt_CrouchKeepAttack);
      On(m, kPadUp, kSt_LookUpKeepAttack);
      On(m, kFaceRight | kPadRight | kPadB, kSt_RunJump);
      On(m, kFaceLeft | kPadLeft | kPadB, kSt_RunJump);
      On(m, kPadB, kSt_StandJump);
      m->require_release |= kPadY;
      SetAnim(m, 0x04);
      NewSwing(cpu, m, kBeamStanding);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_IdleDispatch);
  case kSt_CrouchFaceRight:
    FaceRight(m);
    return Goto(m, kSt_Crouch);
  case kSt_CrouchFaceLeft:
    FaceLeft(m);
    return Goto(m, kSt_Crouch);
  case kSt_CrouchKeepAttack:
    m->require_release &= (uint16_t)~kPadY;
    return Goto(m, kSt_Crouch);
  case kSt_Crouch:
    if (m->pc == 0) {
      Reset(m);
      m->attack = 0;
      m->on_ground_lost = kSt_Fall;
      OnRelease(m, kPadDown, kSt_Idle);
      On(m, kFaceRight | kPadLeft | kPadDown, kSt_CrouchFaceLeft);
      On(m, kFaceLeft | kPadRight | kPadDown, kSt_CrouchFaceRight);
      On(m, kPadY, kSt_CrouchSlash);
      On(m, kPadUp, kSt_LookUp);
      On(m, kPadB, kSt_CrouchJump);
      SetAnim(m, 0x03);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_Crouch);
  case kSt_WalkRight:
    FaceRight(m);
    return Goto(m, kSt_Walk);
  case kSt_WalkLeft:
    FaceLeft(m);
    return Goto(m, kSt_Walk);
  case kSt_Walk:
    if (m->pc == 0) {
      Reset(m);
      m->on_ground_lost = kSt_FallFromWalk;
      OnRelease(m, kFaceLeft | kPadRight, kSt_Idle);
      OnRelease(m, kFaceRight | kPadLeft, kSt_Idle);
      On(m, kFaceRight | kPadLeft, kSt_WalkLeft);
      On(m, kFaceLeft | kPadRight, kSt_WalkRight);
      On(m, kPadUp | kPadY, kSt_UpSlash);
      On(m, kPadDown | kPadY, kSt_CrouchSlash);
      On(m, kPadY, kSt_StandSlash);
      On(m, kPadDown, kSt_Crouch);
      On(m, kPadUp, kSt_LookUp);
      On(m, kPadB, kSt_RunJump);
      SetAnimMotion(m, 0x1A, 0x00);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_Walk);
  case kSt_LookUpFaceRight:
    FaceRight(m);
    return Goto(m, kSt_LookUp);
  case kSt_LookUpFaceLeft:
    FaceLeft(m);
    return Goto(m, kSt_LookUp);
  case kSt_LookUpKeepAttack:
    m->require_release &= (uint16_t)~kPadY;
    return Goto(m, kSt_LookUp);
  case kSt_LookUp:
    if (m->pc == 0) {
      Reset(m);
      m->attack = 0;
      m->on_ground_lost = kSt_Fall;
      OnRelease(m, kPadUp, kSt_Idle);
      On(m, kFaceRight | kPadLeft | kPadUp, kSt_LookUpFaceLeft);
      On(m, kFaceLeft | kPadRight | kPadUp, kSt_LookUpFaceRight);
      On(m, kPadY, kSt_UpSlash);
      On(m, kPadDown, kSt_Crouch);
      On(m, kFaceRight | kPadRight | kPadB, kSt_RunJump);
      On(m, kFaceLeft | kPadLeft | kPadB, kSt_RunJump);
      On(m, kPadB, kSt_StandJump);
      SetAnim(m, 0x02);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_LookUp);
  case kSt_StandJump:
  case kSt_RunJump: {
    const bool running = m->state == kSt_RunJump;
    /* Holding B through the first three takeoff frames picks the height. */
    switch (m->pc) {
    case 0:
      Reset(m);
      m->airborne = true;
      if (!running) On(m, kPadB, kSt_DoubleJump);
      m->require_release |= kPadB;
      SetAnimMotion(m, 0x0F, running ? 0x0B : 0x0A);
      (void)Advance(m);
      m->pc = 1;
      return kYield;
    case 1:
      if (!(Held(cpu) & kPadB)) return Goto(m, running ? kSt_RunJumpLow : kSt_StandJumpLow);
      (void)Advance(m);
      m->pc = 2;
      return kYield;
    case 2:
      if (!(Held(cpu) & kPadB)) return Goto(m, running ? kSt_RunJumpMid : kSt_StandJumpMid);
      (void)Advance(m);
      m->pc = 3;
      return kYield;
    default:
      return Goto(m, running ? kSt_RunJumpHigh : kSt_StandJumpHigh);
    }
  }
  case kSt_StandJumpHigh:
    return AirLoop(m, m->state, 0x10, 0x0D, kSt_DoubleJump, kSt_DownThrust, 2);
  case kSt_StandJumpMid:
    return AirLoop(m, m->state, 0x10, 0x06, kSt_DoubleJump, kSt_DownThrust, 2);
  case kSt_StandJumpLow:
    return AirLoop(m, m->state, 0x10, 0x07, kSt_DoubleJump, kSt_DownThrust, 2);
  case kSt_RunJumpHigh:
    return AirLoop(m, m->state, 0x10, 0x0E, kSt_RunDoubleJump, kSt_DownThrust, 2);
  case kSt_RunJumpMid:
    return AirLoop(m, m->state, 0x10, 0x02, kSt_RunDoubleJump, kSt_DownThrust, 2);
  case kSt_RunJumpLow:
    return AirLoop(m, m->state, 0x10, 0x03, kSt_RunDoubleJump, kSt_DownThrust, 2);
  case kSt_CrouchJump: {
    if (CanDropThrough(cpu, player)) {
      SetField(cpu, player, kActRaiserActionObject_WorldY,
               (uint16_t)(Field(cpu, player, kActRaiserActionObject_WorldY) + 16));
      m->require_release |= kPadB;
      return Goto(m, kSt_Fall);
    }
    const uint16_t pressed = Pressed(cpu, m);
    if (All(pressed, kFaceRight | kPadRight | kPadB) || All(pressed, kFaceLeft | kPadLeft | kPadB))
      return Goto(m, kSt_RunJump);
    return Goto(m, kSt_StandJump);
  }
  case kSt_CrouchSlash:
    if (m->pc == 0) {
      Reset(m);
      FaceHeld(cpu, m);
      m->on_ground_lost = kSt_Fall;
      On(m, kPadUp | kPadY, kSt_UpSlash);
      On(m, kPadUp, kSt_LookUpKeepAttack);
      On(m, kFaceRight | kPadRight | kPadB, kSt_RunJump);
      On(m, kFaceLeft | kPadLeft | kPadB, kSt_RunJump);
      On(m, kPadB, kSt_StandJump);
      m->require_release |= kPadY;
      SetAnim(m, 0x0C);
      NewSwing(cpu, m, kBeamCrouching);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_IdleDispatch);
  case kSt_UpSlash:
    if (m->pc == 0) {
      Reset(m);
      FaceHeld(cpu, m);
      m->on_ground_lost = kSt_Fall;
      On(m, kPadDown | kPadY, kSt_CrouchSlash);
      On(m, kPadDown, kSt_CrouchKeepAttack);
      On(m, kFaceRight | kPadRight | kPadB, kSt_RunJump);
      On(m, kFaceLeft | kPadLeft | kPadB, kSt_RunJump);
      On(m, kPadB, kSt_StandJump);
      m->require_release |= kPadY;
      SetAnim(m, 0x0D);
      NewSwing(cpu, m, kBeamStanding);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_IdleDispatch);
  case kSt_AirSlash:
    switch (m->pc) {
    case 0:
      ResetInput(m);
      m->airborne = true;
      m->on_land = kSt_Land;
      m->require_release |= kPadY;
      SetAnim(m, 0x1B);
      NewSwing(cpu, m, kBeamStanding);
      m->pc = 1;
      /* fall through */
    case 1:
      /* Loads the four swing frames, then waits out the last one. */
      if (m->step != 4) {
        (void)Advance(m);
        return kYield;
      }
      {
        const uint16_t pressed = Pressed(cpu, m);
        if (All(pressed, kPadDown | kPadY)) return Goto(m, kSt_DownThrust);
        if (pressed & kPadY) return Goto(m, kSt_AirSlash);
      }
      On(m, kPadDown | kPadY, kSt_DownThrust);
      On(m, kPadY, kSt_AirSlash);
      m->anim = 0x10; /* resume the fall pose at its held third step */
      m->step = 2;
      m->pc = 2;
      /* fall through */
    default:
      if (Advance(m)) return Goto(m, kSt_AirSlash);
      return kYield;
    }
  case kSt_FallFromWalk:
    return AirLoop(m, m->state, 0x10, 0x09, kSt_None, kSt_DownThrust, 2);
  case kSt_Fall:
    return AirLoop(m, m->state, 0x10, 0x08, kSt_None, kSt_DownThrust, 2);
  case kSt_Land:
    if (m->pc == 0) {
      Reset(m);
      GroundEvents(m, true);
      SetAnim(m, 0x0E);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_IdleDispatch);
  case kSt_DoubleJump:
  case kSt_RunDoubleJump:
    if (m->pc == 0) {
      const bool running = m->state == kSt_RunDoubleJump;
      Reset(m);
      m->airborne = true;
      FaceHeld(cpu, m);
      m->on_land = kSt_Land;
      m->require_release |= kPadB;
      On(m, kPadB, kSt_FallFromWalk);
      SetAnimMotion(m, 0x11, running ? 0x11 : 0x10);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    if (m->state == kSt_DoubleJump && (Pressed(cpu, m) & kPadUp)) return Goto(m, kSt_Float);
    return Goto(m, kSt_GlideStart);
  case kSt_GlideStart:
    if (m->pc == 0) {
      Reset(m);
      m->airborne = true;
      m->on_land = kSt_Land;
      On(m, kPadB, kSt_FallFromWalk);
      SetAnimMotion(m, 0x13, 0x14);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_GlideLoop);
  case kSt_GlideLoopFaceRight:
    FaceRight(m);
    return Goto(m, kSt_GlideLoop);
  case kSt_GlideLoopFaceLeft:
    FaceLeft(m);
    return Goto(m, kSt_GlideLoop);
  case kSt_GlideLoop:
    switch (m->pc) {
    case 0:
      m->loop_counter = 4;
      m->pc = 1;
      /* fall through */
    case 1:
      Reset(m);
      m->airborne = true;
      m->on_land = kSt_Skid;
      GlideEvents(m, kSt_GlideLoopFaceRight, kSt_GlideLoopFaceLeft, kSt_GlideLoopSlash);
      SetAnimMotion(m, 0x14, 0x16);
      m->pc = 2;
      /* fall through */
    default:
      if (!Advance(m)) return kYield;
      if (--m->loop_counter != 0) {
        m->pc = 1;
        return kContinue;
      }
      return Goto(m, kSt_Glide);
    }
  case kSt_GlideFaceRight:
    FaceRight(m);
    return Goto(m, kSt_Glide);
  case kSt_GlideFaceLeft:
    FaceLeft(m);
    return Goto(m, kSt_Glide);
  case kSt_Glide:
    if (m->pc == 0) {
      Reset(m);
      m->airborne = true;
      m->on_land = kSt_Skid;
      GlideEvents(m, kSt_GlideFaceRight, kSt_GlideFaceLeft, kSt_GlideSlash);
      SetAnimMotion(m, 0x15, 0x18);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_Glide);
  case kSt_Skid:
    if (m->pc == 0) {
      Reset(m);
      m->airborne = true;
      m->on_ground_lost = kSt_FallFromWalk;
      m->on_wall = kSt_IdleDispatch;
      SetAnimMotion(m, 0x17, 0x1A);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_IdleDispatch);
  case kSt_FloatFaceRight:
    FaceRight(m);
    return Goto(m, kSt_Float);
  case kSt_FloatFaceLeft:
    FaceLeft(m);
    return Goto(m, kSt_Float);
  case kSt_Float:
    if (m->pc == 0) {
      Reset(m);
      m->airborne = true;
      m->on_land = kSt_Land;
      On(m, kPadY, kSt_Fall);
      On(m, kFaceRight | kPadLeft, kSt_FloatFaceLeft);
      On(m, kFaceLeft | kPadRight, kSt_FloatFaceRight);
      OnRelease(m, kPadUp, kSt_Fall);
      SetAnimMotion(m, 0x12, 0x13);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_Float);
  case kSt_DiveFaceRight:
    FaceRight(m);
    return Goto(m, kSt_Dive);
  case kSt_DiveFaceLeft:
    FaceLeft(m);
    return Goto(m, kSt_Dive);
  case kSt_Dive:
    if (m->pc == 0) {
      Reset(m);
      m->airborne = true;
      m->on_land = kSt_Land;
      On(m, kFaceLeft | kPadRight, kSt_DiveFaceRight);
      On(m, kFaceRight | kPadLeft, kSt_DiveFaceLeft);
      On(m, kPadY, kSt_DiveAttack);
      SetAnimMotion(m, 0x18, 0x1C);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_Dive);
  case kSt_DownThrust:
  case kSt_RunDownThrust:
    ResetInput(m);
    m->attack = m->state == kSt_RunDownThrust ? 3 : 2;
    return Goto(m, kSt_DownThrustHold);
  case kSt_DownThrustHold:
    if (m->pc == 0) {
      m->airborne = true;
      m->keep_motion = true;
      m->invulnerable = true;
      m->on_land = kSt_Land;
      m->require_release |= kPadY;
      SetAnim(m, 0x1F);
      m->beam = 0; /* Held thrust: no repeated swing or beam as the pose loops. */
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_DownThrust);
  case kSt_GlideLoopSlash:
  case kSt_GlideSlash:
    if (m->pc == 0) {
      ResetInput(m);
      m->airborne = true;
      m->keep_motion = true;
      m->attack = 2;
      m->on_land = kSt_Skid;
      m->require_release |= kPadY;
      SetAnim(m, 0x1D);
      NewSwing(cpu, m, kBeamStanding);
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    if (Pressed(cpu, m) & kPadY) return Goto(m, m->state);
    return Goto(m, m->state == kSt_GlideLoopSlash ? kSt_GlideLoop : kSt_Glide);
  case kSt_DiveAttackFaceRight:
    FaceRight(m);
    return Goto(m, kSt_DiveAttack);
  case kSt_DiveAttackFaceLeft:
    FaceLeft(m);
    return Goto(m, kSt_DiveAttack);
  case kSt_DiveAttack:
    if (m->pc == 0) {
      Reset(m);
      m->airborne = true;
      m->attack = 3;
      m->invulnerable = true;
      m->on_land = kSt_Land;
      On(m, kFaceLeft | kPadRight, kSt_DiveAttackFaceRight);
      On(m, kFaceRight | kPadLeft, kSt_DiveAttackFaceLeft);
      m->require_release |= kPadY;
      SetAnimMotion(m, 0x1C, 0x1C);
      m->beam = 0; /* Held dive: no repeated swing or beam as the pose loops. */
      m->pc = 1;
    }
    if (!Advance(m)) return kYield;
    return Goto(m, kSt_DiveAttack);
  default:
    return Goto(m, kSt_IdleDispatch);
  }
}

static void RunScript(CpuState *cpu, uint16_t player, Ar2Master *m) {
  /* Every AR2 transition either yields or changes state; a long chain means
   * a transcription error, never a legitimate frame. */
  for (unsigned guard = 0; guard < 64; guard++) {
    if (m->state == kSt_None) m->state = kSt_IdleDispatch;
    if (Step(cpu, player, m) == kYield) return;
  }
  ActRaiserHleFatal("AR2 master script did not yield");
}

static void FireEvent(Ar2Master *m, uint8_t target) {
  ClearEvents(m);
  m->timer = 0;
  Goto(m, target);
}

/* NMI $80:B19A: the first matching event replaces the script position. */
static void PollEvents(const CpuState *cpu, Ar2Master *m) {
  const uint16_t pressed = Pressed(cpu, m);
  for (unsigned i = 0; i < m->event_count; i++) {
    const Ar2Event *event = &m->events[i];
    const bool fired = event->release ? (uint16_t)(~pressed & event->mask) == event->mask
                                      : All(pressed, event->mask);
    if (fired && event->target != kSt_None) {
      FireEvent(m, event->target);
      return;
    }
  }
}

/* Physics outcomes of the previous frame, in AR2's physics order: the wall,
 * then a landing or a lost floor ($80:A6C5..$A6A4). */
static void PhysicsEvents(CpuState *cpu, uint16_t player, Ar2Master *m) {
  const int16_t moved = (int16_t)(Field(cpu, player, kActRaiserActionObject_WorldX) - m->x_before);
  if (m->on_wall != kSt_None && m->intended_dx && moved != m->intended_dx) {
    const uint8_t target = m->on_wall;
    m->on_wall = kSt_None;
    StopMotion(m);
    Goto(m, target);
    return;
  }
  if (m->on_land != kSt_None && m->last_dy > 0 && m->grounded) {
    const uint8_t target = m->on_land;
    m->on_land = kSt_None;
    StopMotion(m);
    Goto(m, target);
    return;
  }
  if (m->on_ground_lost != kSt_None && m->last_dy >= 0 && !m->grounded) {
    const uint8_t target = m->on_ground_lost;
    StopMotion(m);
    m->on_land = m->on_ground_lost = kSt_None;
    Goto(m, target);
  }
}

static bool Grounded(CpuState *cpu) {
  Leaf(cpu, bank_00_8FE7_M0X0);
  return cpu->_flag_C != 0;
}

static void SyncAr1Extents(CpuState *cpu, uint16_t player) {
  /* Keep the feet where the native code expects them while restoring the
   * standard control-state extents. */
  const uint16_t bottom = Field(cpu, player, kActRaiserActionObject_BottomExtent);
  if (bottom != kAr1Bottom && bottom < 64)
    SetField(cpu, player, kActRaiserActionObject_WorldY,
             (uint16_t)(Field(cpu, player, kActRaiserActionObject_WorldY) + bottom - kAr1Bottom));
  const Ar2MasterFrame *frame = CurrentFrame();
  const bool low = frame && frame->hurt_height && frame->hurt_height < 36;
  SetField(cpu, player, kActRaiserActionObject_LeftExtent, kAr1HalfWidth);
  SetField(cpu, player, kActRaiserActionObject_RightExtent, kAr1HalfWidth);
  SetField(cpu, player, kActRaiserActionObject_TopExtent, low ? kAr1TopLow : kAr1TopStanding);
  SetField(cpu, player, kActRaiserActionObject_BottomExtent, kAr1Bottom);
}

static void Enter(CpuState *cpu, uint16_t player, Ar2Master *m) {
  const uint16_t held = Held(cpu);
  *m = (Ar2Master){.state = kSt_IdleDispatch, .frame = -1};
  m->facing_left = (Field(cpu, player, kActRaiserActionObject_FlipAttributes) & kFlipLeft) != 0;
  m->on_land = m->on_ground_lost = m->on_wall = kSt_None;
  m->attack = 1;
  /* Buttons held through a hurt, cast or arrival must be pressed again. */
  m->require_release = (uint16_t)(held & (kPadB | kPadY));
  SyncAr1Extents(cpu, player);
  SetField(cpu, player, kActRaiserActionObject_VelocityY, 0);
  m->grounded = Grounded(cpu);
  if (!m->grounded) m->state = kSt_Fall;
  m->x_before = Field(cpu, player, kActRaiserActionObject_WorldX);
  m->active = true;
}

/* AR2's resolver ($C0:98F8) reads one box per frame. Attack type 0 makes it
 * the shield: the idle, crouch and look-up poses hold it chest-high in front,
 * low in front, and overhead. Any other type makes it the sword's reach. */
static void UpdateAttack(CpuState *cpu, uint16_t player, Ar2Master *m) {
  const Ar2MasterFrame *frame = CurrentFrame();
  const bool was_active = m->attack_active;
  const bool boxed = frame && frame->has_attack;
  m->attack_active = boxed && m->attack != 0;
  m->shield_active = boxed && m->attack == 0;
  if (!m->shield_active) memset(m->shield_contacts, 0, sizeof(m->shield_contacts));
  if (!boxed) return;
  const int x = (int16_t)Field(cpu, player, kActRaiserActionObject_WorldX);
  const int feet = (int16_t)Field(cpu, player, kActRaiserActionObject_WorldY) + kAr1Bottom;
  if (m->facing_left) {
    m->box_x1 = x - frame->attack_x;
    m->box_x0 = m->box_x1 - frame->attack_width;
  } else {
    m->box_x0 = x + frame->attack_x;
    m->box_x1 = m->box_x0 + frame->attack_width;
  }
  m->box_y0 = feet + frame->attack_y;
  m->box_y1 = m->box_y0 + frame->attack_height;
  if (!m->attack_active) return;
  /* AR1's sword power-up throws its beam as a swing becomes active. */
  if (!was_active && m->beam) {
    const uint16_t a = cpu->A, y = cpu->Y;
    Leaf(cpu, bank_00_88C9_M0X0);
    if (!cpu->_flag_C) {
      Leaf(cpu, bank_00_8538_M0X0);
      if (!cpu->_flag_C) {
        cpu->A = m->beam;
        Leaf(cpu, bank_00_9CF2_M0X0);
      }
    }
    cpu->A = a;
    cpu->Y = y;
  }
  if (!was_active) m->beam = 0;
}

static bool CastRequested(CpuState *cpu, const Ar2Master *m) {
  if (m->airborne || !m->grounded) return false;
  switch (m->state) {
  case kSt_Idle:
  case kSt_Walk:
  case kSt_Crouch:
  case kSt_LookUp:
  case kSt_Land:
  case kSt_IdleDispatch:
    break;
  default:
    return false;
  }
  /* Native gate prerequisites ($9DE1); regional pricing runs natively. */
  if (cpu_read8(cpu, 0, kActRaiserWram_MagicCastState) ||
      !Read16(cpu, kActRaiserWram_SelectedMagic) ||
      (Field(cpu, kActRaiserWram_PlayerObject, kActRaiserActionObject_Flags) &
       (kFlagInvulnerable | kFlagHurt)) ||
      !cpu_read8(cpu, 0, kActRaiserWram_WorkingMagicPoints))
    return false;
  if (ActRaiser_RegionalMagicGestureEntry(cpu))
    return All(Pressed(cpu, m), kPadUp | kPadY); /* JP: Up + attack */
  return (Read16(cpu, kActRaiserWram_InputHeldHigh) & (kPadA | kPadX)) != 0;
}

static bool ControlGate(const CpuState *cpu) {
  return cpu && !cpu->PB && !cpu->DB && !cpu->D && !cpu->emulation && !cpu->m_flag &&
         !cpu->x_flag && InActionStage(cpu) && cpu->X == kActRaiserWram_PlayerObject;
}

bool ActRaiser_Ar2MasterControlEntry(CpuState *cpu) {
  if (!s_available || !ControlGate(cpu)) return false;
  UpdateControllerSelection(cpu);
  return s_stage_enabled;
}

RecompReturn ActRaiser_Ar2MasterControl(CpuState *cpu) {
  if (!ActRaiser_Ar2MasterControlEntry(cpu))
    ActRaiserHleFatal("Unsupported AR2 master control entry");
  Ar2Master *m = &s_master;
  const uint16_t player = cpu->X;
  if (!SettingEnabled()) {
    /* Native idle dispatch checks the floor and enters its falling controller
     * when airborne. Preserve world position, facing, HP, camera and timers;
     * discard only AR2 attack/motion state and its temporary dive immunity. */
    if (m->invulnerable) {
      const uint16_t flags = Field(cpu, player, kActRaiserActionObject_Flags);
      SetField(cpu, player, kActRaiserActionObject_Flags, (uint16_t)(flags & ~kFlagBusy));
    }
    *m = (Ar2Master){.state = kSt_None, .frame = -1};
    SyncAr1Extents(cpu, player);
    SetField(cpu, player, kActRaiserActionObject_AnimationState, 0);
    SetField(cpu, player, kActRaiserActionObject_AnimationIndex, 0);
    SetField(cpu, player, kActRaiserActionObject_Wait, 0);
    s_stage_enabled = false;
    return Tail(kControlHandler, kControlHandler);
  }
  const uint16_t game_frame = Read16(cpu, kActRaiserWram_GameFrame);
  SetField(cpu, player, kActRaiserActionObject_Handler, kControlHandler);

  /* Native hurt and expired-timer death own the player from the tail on. */
  if (Field(cpu, player, kActRaiserActionObject_Flags) & kFlagHurt) {
    m->active = false;
    SetField(cpu, player, kActRaiserActionObject_VelocityX, 0);
    SetField(cpu, player, kActRaiserActionObject_VelocityY, 0);
    return Tail(kNativePlayerTail, kControlHandler);
  }

  if (!m->active || (uint16_t)(game_frame - m->last_frame) > 1) {
    Enter(cpu, player, m);
  } else {
    m->grounded = Grounded(cpu);
    PhysicsEvents(cpu, player, m);
  }
  m->last_frame = game_frame;

  /* NMI-side input upkeep: released buttons stop waiting for release. */
  m->require_release &= Held(cpu);
  PollEvents(cpu, m);

  if (CastRequested(cpu, m)) {
    m->active = false;
    SetField(cpu, player, kActRaiserActionObject_VelocityX, 0);
    SetField(cpu, player, kActRaiserActionObject_VelocityY, 0);
    SetField(cpu, player, kActRaiserActionObject_FlipAttributes,
             (uint16_t)((Field(cpu, player, kActRaiserActionObject_FlipAttributes) & ~kFlipLeft) |
                        (m->facing_left ? kFlipLeft : 0)));
    return Tail(kNativeCastGate, kControlHandler);
  }

  if (--m->timer < 0) {
    m->timer = 0;
    RunScript(cpu, player, m);
  }

  /* Motion programs, AR2's air steering helper ($82:8CE8) and AR1's
   * slow-zone halving (flag $8000, rounding down as $00:84EC does). */
  int16_t dx = Ar2MotionAxis_Step(&s_assets, &m->motion_x);
  if (m->facing_left) dx = (int16_t)-dx;
  const int16_t dy = Ar2MotionAxis_Step(&s_assets, &m->motion_y);
  m->motion_dx = dx;
  if (m->airborne) {
    const uint16_t held = Held(cpu);
    if (held & kPadLeft) {
      if (dx >= 0) dx--;
    } else if ((held & kPadRight) && dx <= 0) {
      dx++;
    }
  }
  uint16_t flags = Field(cpu, player, kActRaiserActionObject_Flags);
  if (flags & kFlagSlowZone) {
    flags &= (uint16_t)~kFlagSlowZone;
    dx = (int16_t)(dx >> 1);
  }
  flags = (uint16_t)((flags & ~kFlagBusy) | (m->invulnerable ? kFlagBusy : 0) | kFlagAttacker);
  SetField(cpu, player, kActRaiserActionObject_Flags, flags);

  SyncAr1Extents(cpu, player);
  UpdateAttack(cpu, player, m);
  SetField(cpu, player, kActRaiserActionObject_VelocityX, (uint16_t)dx);
  SetField(cpu, player, kActRaiserActionObject_VelocityY, (uint16_t)dy);
  SetField(cpu, player, kActRaiserActionObject_FlipAttributes,
           (uint16_t)((Field(cpu, player, kActRaiserActionObject_FlipAttributes) & ~kFlipLeft) |
                      (m->facing_left ? kFlipLeft : 0)));
  SetField(cpu, player, kActRaiserActionObject_AnimationState, 0);
  SetField(cpu, player, kActRaiserActionObject_AnimationIndex, 0);
  SetField(cpu, player, kActRaiserActionObject_Wait, 0);
  m->last_dx = dx;
  m->last_dy = dy;
  m->intended_dx = dx;
  m->x_before = Field(cpu, player, kActRaiserActionObject_WorldX);
  static int trace = -1;
  if (trace < 0) trace = getenv("AR_AR2_MASTER_TRACE") != NULL;
  if (trace)
    fprintf(stderr,
            "[ar2-master] gf=%u state=%u pc=%u anim=%02X step=%u frame=%d timer=%d "
            "pos=%u,%u d=%d,%d ground=%d left=%d atk=%d held=%04X\n",
            game_frame, m->state, m->pc, m->anim, m->step, m->frame, m->timer,
            Field(cpu, player, kActRaiserActionObject_WorldX),
            Field(cpu, player, kActRaiserActionObject_WorldY), dx, dy, m->grounded, m->facing_left,
            m->attack_active, Held(cpu));
  return Tail(kNativePlayerTail, kControlHandler);
}

/* ---- Attacks through AR1's resolver ------------------------------------ */
bool ActRaiser_Ar2MasterHitPassEntry(CpuState *cpu) {
  return !s_hit_delegate && s_available && s_stage_enabled && s_master.active &&
         s_master.attack_active && cpu && InActionStage(cpu);
}

RecompReturn ActRaiser_Ar2MasterHitPass(CpuState *cpu) {
  const uint16_t player = kActRaiserWram_PlayerObject;
  uint16_t saved[4];
  for (unsigned i = 0; i < 4; i++)
    saved[i] = Field(cpu, player, kActRaiserActionObject_LeftExtent + i * 2);
  /* $8A3C prefilters each attacker by its extents box. Cover the swing. */
  const int x = (int16_t)Field(cpu, player, kActRaiserActionObject_WorldX);
  const int y = (int16_t)Field(cpu, player, kActRaiserActionObject_WorldY);
  const Ar2Master *m = &s_master;
  int left = saved[0], top = saved[1], right = saved[2], bottom = saved[3];
  if (x - m->box_x0 > left) left = x - m->box_x0;
  if (m->box_x1 - x > right) right = m->box_x1 - x;
  if (y - m->box_y0 > top) top = y - m->box_y0;
  if (m->box_y1 - y > bottom) bottom = m->box_y1 - y;
  SetField(cpu, player, kActRaiserActionObject_LeftExtent, (uint16_t)left);
  SetField(cpu, player, kActRaiserActionObject_TopExtent, (uint16_t)top);
  SetField(cpu, player, kActRaiserActionObject_RightExtent, (uint16_t)right);
  SetField(cpu, player, kActRaiserActionObject_BottomExtent, (uint16_t)bottom);
  s_hit_delegate = true;
  const RecompReturn result = cpu->m_flag ? bank_00_8A3C_M1X0(cpu) : bank_00_8A3C_M0X0(cpu);
  s_hit_delegate = false;
  for (unsigned i = 0; i < 4; i++)
    SetField(cpu, player, kActRaiserActionObject_LeftExtent + i * 2, saved[i]);
  return result;
}

bool ActRaiser_Ar2MasterHitTestEntry(CpuState *cpu) {
  return s_available && s_stage_enabled && cpu && !cpu->m_flag && !cpu->x_flag &&
         InActionStage(cpu) && cpu->Y == kActRaiserWram_PlayerObject;
}

RecompReturn ActRaiser_Ar2MasterHitTest(CpuState *cpu) {
  Ar2Master *m = &s_master;
  const uint16_t victim = cpu->X;
  const uint16_t frame = Read16(cpu, kActRaiserWram_GameFrame);
  bool hit = false;
  if (m->active && m->attack_active) {
    const int vx = (int16_t)Field(cpu, victim, kActRaiserActionObject_WorldX);
    const int vy = (int16_t)Field(cpu, victim, kActRaiserActionObject_WorldY);
    const int v_left = vx - (int16_t)Field(cpu, victim, kActRaiserActionObject_LeftExtent);
    const int v_right = vx + (int16_t)Field(cpu, victim, kActRaiserActionObject_RightExtent);
    const int v_top = vy - (int16_t)Field(cpu, victim, kActRaiserActionObject_TopExtent);
    const int v_bottom = vy + (int16_t)Field(cpu, victim, kActRaiserActionObject_BottomExtent);
    hit = m->box_x0 < v_right && m->box_x1 > v_left && m->box_y0 < v_bottom && m->box_y1 > v_top;
    unsigned free_slot = kMaxVictims, oldest = 0;
    for (unsigned i = 0; hit && i < kMaxVictims; i++) {
      const Ar2Victim *entry = &m->victims[i];
      const uint16_t age = (uint16_t)(frame - entry->frame);
      if (!entry->object || age >= kVictimCooldown) {
        if (free_slot == kMaxVictims) free_slot = i;
        continue;
      }
      if (entry->object == victim) hit = false;
      if (age > (uint16_t)(frame - m->victims[oldest].frame)) oldest = i;
    }
    if (hit) m->victims[free_slot < kMaxVictims ? free_slot : oldest] = (Ar2Victim){victim, frame};
  }
  cpu->_flag_C = hit ? 1 : 0;
  cpu->P = (uint8_t)((cpu->P & ~CPU_P_C) | (hit ? CPU_P_C : 0));
  return Tail(kNativeReturn, 0x008B67);
}

/* ---- Shield --------------------------------------------------------------
 * AR2 stops only shots an enemy marked blockable ($C0:99B5, object +06 bit 2).
 * ActRaiser has no such mark, so shots are recognised by the code they run:
 * an object parked in the shared delay ($8661) or repeat ($8683) handler
 * keeps its continuation in +1E, any other object runs its +12 handler. The
 * addresses below were surveyed from every $8538/$853D spawn site, and checked
 * in play where a recording reaches them (Fillmore 1-2, Aitos 2, Northwall 2,
 * every Death Heim rematch and Tanzra). Original and rematch bosses run the
 * same code. A shot is stopped only while it moves; the native player
 * contact pass ($8970, every other frame) applies damage, so the shield runs
 * inside it, ahead of the native test. */
typedef enum ShieldPolicy {
  kShield_Destroy, /* fire and forget: retire it, as $85B7 does off-screen */
  /* Its shooter waits on it: do what the shot's own exit does ($C398, $CA7E,
   * $CBFC), clearing the shooter's pending flag (+38 via +3A), and retire it.
   * The exit itself cannot be dispatched to: it is not a function entry. */
  kShield_Release,
  kShield_Absorb, /* native code still tracks it: no damage, object lives */
} ShieldPolicy;

typedef struct ShieldShot {
  uint16_t code;
  uint8_t policy;
  const char *name;
} ShieldShot;

static const ShieldShot kShieldShots[] = {
    /* $A655: every region's straight-line shot (Wizard bolts, Flaming Wheel
     * fireballs, cave and statue shooters, ice fragments, ...). */
    {0xA65D, kShield_Destroy, "straight shot"},
    {0xB36A, kShield_Destroy, "Fillmore thrown rock"},
    {0xB008, kShield_Destroy, "Minotaur axe"},
    {0xB8FB, kShield_Destroy, "Bloodpool boss fireball"},
    {0xB90D, kShield_Destroy, "Bloodpool boss fireball"},
    {0xBDD9, kShield_Destroy, "Bloodpool statue fireball"},
    {0xBDF0, kShield_Destroy, "Bloodpool statue fireball"},
    {0xC0F6, kShield_Destroy, "Wizard orb"},
    {0xC0FC, kShield_Destroy, "Wizard orb"},
    {0xC2E5, kShield_Destroy, "Pharaoh summon shot"},
    {0xC389, kShield_Release, "thrown bomb"},
    {0xC38F, kShield_Release, "thrown bomb"},
    {0xC954, kShield_Destroy, "Kasandora shot"},
    {0xCA75, kShield_Release, "Kasandora shot"},
    {0xCB40, kShield_Destroy, "Kasandora burst"},
    {0xCBF3, kShield_Release, "Kasandora shot"},
    {0xDBA5, kShield_Destroy, "Marahna shot"},
    {0xE158, kShield_Destroy, "Marahna swooping orb"},
    {0xE165, kShield_Destroy, "Marahna swooping orb"},
    {0xE16B, kShield_Destroy, "Marahna swooping orb"},
    {0xE17B, kShield_Destroy, "Marahna swooping orb"},
    {0xE181, kShield_Destroy, "Marahna swooping orb"},
    {0xE563, kShield_Destroy, "Viper bolt"},
    {0xE570, kShield_Destroy, "Viper bolt"},
    {0xE578, kShield_Destroy, "Viper bolt"},
    {0xE57E, kShield_Destroy, "Viper floor charge"},
    {0xE890, kShield_Destroy, "Northwall boss magic"},
    {0xE8AE, kShield_Destroy, "Northwall boss magic"},
    {0xE9F5, kShield_Destroy, "Northwall thrown rock"},
    {0xE9FB, kShield_Destroy, "Northwall thrown rock"},
    {0xEB05, kShield_Destroy, "Northwall shot"},
    {0xEB18, kShield_Destroy, "Northwall shot"},
    {0xF274, kShield_Destroy, "Ice Dragon icicle"},
    {0xF27A, kShield_Destroy, "Ice Dragon icicle"},
    {0xF280, kShield_Destroy, "Ice Dragon icicle"},
    {0xF2CA, kShield_Destroy, "Ice Dragon ice ball"},
    {0xFBEA, kShield_Destroy, "Tanzra orb"},
    {0xFBF5, kShield_Destroy, "Tanzra orb"},
    {0xFC0B, kShield_Destroy, "Tanzra orb"},
    {0xFC13, kShield_Destroy, "Tanzra orb"},
    {0xFC21, kShield_Destroy, "Tanzra orb"},
    {0xFD44, kShield_Destroy, "Tanzra bolt"},
    {0xFD77, kShield_Destroy, "Tanzra falling fire"},
    {0xFD9E, kShield_Destroy, "Tanzra burst"},
    /* The sphere summons at the wall; rocks and lava fireballs reset from
     * their spawn records and launch again. */
    {0xC27B, kShield_Absorb, "Pharaoh sphere"},
    {0xCF16, kShield_Absorb, "Aitos molten rock"},
    {0xCF1C, kShield_Absorb, "Aitos molten rock"},
    {0xCFC2, kShield_Absorb, "Aitos lava fireball"},
    {0xCFCD, kShield_Absorb, "Aitos lava fireball"},
    {0xCFE3, kShield_Absorb, "Aitos lava fireball"},
    {0xCFFE, kShield_Absorb, "Aitos lava fireball"},
};

enum {
  kStatusEnd = 0x8000,
  kStatusNoContact = 0x6800, /* free, spawning or hidden: $8970 skips */
  kStatusHidden = 0x2000,
  kStatusFree = 0x4000,
  kStatusFlagsIgnored = 0x0400, /* $8970 tests it whatever its flags say */
  kFlagsNoContact = 0x0499,     /* attackers, hit or busy objects, ... */
  kFlagPickup = 0x0200,
  kDelayHandler = 0x8661,
  kRepeatHandler = 0x8683,
  kObjectTableEnd = kActRaiserWram_ActionObjectTable +
      kActRaiserActionObjectCount * kActRaiserActionObjectStride,
};

static bool s_contact_delegate;

static bool ShieldTrace(void) {
  static int trace = -1;
  if (trace < 0) trace = getenv("AR_AR2_SHIELD_TRACE") != NULL;
  return trace != 0;
}

static bool ShieldUp(const CpuState *cpu) {
  const uint16_t game_frame = Read16(cpu, kActRaiserWram_GameFrame);
  return s_stage_enabled && s_master.active && s_master.shield_active &&
         (uint16_t)(game_frame - s_master.last_frame) <= 1;
}

/* $8970's own filter for objects that can touch the player. */
static bool CanTouchPlayer(const CpuState *cpu, uint16_t object) {
  const uint16_t status = Field(cpu, object, kActRaiserActionObject_Status);
  const uint16_t flags = Field(cpu, object, kActRaiserActionObject_Flags);
  if (status & kStatusNoContact) return false;
  if (!(status & kStatusFlagsIgnored) && (flags & kFlagsNoContact)) return false;
  return !(flags & kFlagPickup);
}

static uint16_t ShotCode(const CpuState *cpu, uint16_t object) {
  const uint16_t handler = Field(cpu, object, kActRaiserActionObject_Handler);
  return handler == kDelayHandler || handler == kRepeatHandler
             ? Field(cpu, object, kActRaiserActionObject_ResumeAddress)
             : handler;
}

static const ShieldShot *StoppableShot(const CpuState *cpu, uint16_t object) {
  if (!Field(cpu, object, kActRaiserActionObject_VelocityX) &&
      !Field(cpu, object, kActRaiserActionObject_VelocityY))
    return NULL;
  const uint16_t code = ShotCode(cpu, object);
  for (size_t i = 0; i < sizeof(kShieldShots) / sizeof(kShieldShots[0]); i++)
    if (kShieldShots[i].code == code) return &kShieldShots[i];
  return NULL;
}

static bool TouchesShield(const CpuState *cpu, uint16_t object) {
  const Ar2Master *m = &s_master;
  const int x = (int16_t)Field(cpu, object, kActRaiserActionObject_WorldX);
  const int y = (int16_t)Field(cpu, object, kActRaiserActionObject_WorldY);
  return m->box_x0 < x + (int16_t)Field(cpu, object, kActRaiserActionObject_RightExtent) &&
         m->box_x1 > x - (int16_t)Field(cpu, object, kActRaiserActionObject_LeftExtent) &&
         m->box_y0 < y + (int16_t)Field(cpu, object, kActRaiserActionObject_BottomExtent) &&
         m->box_y1 > y - (int16_t)Field(cpu, object, kActRaiserActionObject_TopExtent);
}

static void TraceContact(const CpuState *cpu, uint16_t object, bool shield, bool touches,
                         const ShieldShot *shot, const char *action) {
  const uint16_t parent = Field(cpu, object, kActRaiserActionObject_SpawnerBacklink);
  const bool linked = parent >= kActRaiserWram_ActionObjectTable && parent < kObjectTableEnd;
  const uint16_t player = kActRaiserWram_PlayerObject;
  fprintf(stderr,
          "[ar2-shield] gf=%u obj=$%04X src=$%04X h=$%04X res=$%04X code=$%04X st=$%02X "
          "vis=$%02X anim=$%02X:%04X parent=$%04X psrc=$%04X pos=%d,%d vel=%d,%d "
          "ext=%d,%d,%d,%d atk=%u hp=%u flags=$%04X status=$%04X player=%d,%d "
          "shield=%d touch=%d shot=%s action=%s\n",
          Read16(cpu, kActRaiserWram_GameFrame), object,
          Field(cpu, object, kActRaiserActionObject_SourceDescriptor),
          Field(cpu, object, kActRaiserActionObject_Handler),
          Field(cpu, object, kActRaiserActionObject_ResumeAddress), ShotCode(cpu, object),
          (unsigned)(Field(cpu, object, kActRaiserActionObject_AnimationState) & 0xFF),
          (unsigned)(Field(cpu, object, kActRaiserActionObject_Visual) & 0xFF),
          cpu_read8((CpuState *)cpu, 0, (uint16_t)(object + kActRaiserActionObject_AnimationBank)),
          Field(cpu, object, kActRaiserActionObject_AnimationAddress), parent,
          linked ? Field(cpu, parent, kActRaiserActionObject_SourceDescriptor) : 0,
          (int16_t)Field(cpu, object, kActRaiserActionObject_WorldX),
          (int16_t)Field(cpu, object, kActRaiserActionObject_WorldY),
          (int16_t)Field(cpu, object, kActRaiserActionObject_VelocityX),
          (int16_t)Field(cpu, object, kActRaiserActionObject_VelocityY),
          (int16_t)Field(cpu, object, kActRaiserActionObject_LeftExtent),
          (int16_t)Field(cpu, object, kActRaiserActionObject_TopExtent),
          (int16_t)Field(cpu, object, kActRaiserActionObject_RightExtent),
          (int16_t)Field(cpu, object, kActRaiserActionObject_BottomExtent),
          Field(cpu, object, kActRaiserActionObject_Attack),
          Field(cpu, object, kActRaiserActionObject_HitPoints),
          Field(cpu, object, kActRaiserActionObject_Flags),
          Field(cpu, object, kActRaiserActionObject_Status),
          (int16_t)Field(cpu, player, kActRaiserActionObject_WorldX),
          (int16_t)Field(cpu, player, kActRaiserActionObject_WorldY), shield, touches,
          shot ? shot->name : "-", action);
}

bool ActRaiser_Ar2MasterContactEntry(CpuState *cpu) {
  return !s_contact_delegate && s_available && cpu && !cpu->x_flag && InActionStage(cpu) &&
         (ShieldUp(cpu) || ShieldTrace());
}

RecompReturn ActRaiser_Ar2MasterContact(CpuState *cpu) {
  static const char *const kActions[] = {"destroy", "release", "absorb"};
  const bool shield = ShieldUp(cpu);
  const bool trace = ShieldTrace();
  uint16_t masked[kActRaiserActionObjectCount];
  uint16_t masked_status[kActRaiserActionObjectCount];
  bool contacts[kActRaiserActionObjectCount] = {false};
  unsigned masked_count = 0;
  for (uint16_t object = kActRaiserWram_PlayerObject; object < kObjectTableEnd;
       object = (uint16_t)(object + kActRaiserActionObjectStride)) {
    const uint16_t status = Field(cpu, object, kActRaiserActionObject_Status);
    if (status & kStatusEnd) break;
    if (!CanTouchPlayer(cpu, object)) continue;
    const ShieldShot *shot = StoppableShot(cpu, object);
    const bool touches = shield && shot && TouchesShield(cpu, object);
    if (trace)
      TraceContact(cpu, object, shield, touches, shot, touches ? kActions[shot->policy] : "-");
    if (!touches) continue;
    const unsigned slot =
        (object - kActRaiserWram_ActionObjectTable) / kActRaiserActionObjectStride;
    contacts[slot] = shot->policy == kShield_Absorb;
    if (!contacts[slot] || !s_master.shield_contacts[slot]) {
      /* Match the native deflection's victim/attacker identity before retiring
       * the projectile, then return the untouched registers to $8970. */
      const uint16_t x = cpu->X, y = cpu->Y;
      cpu->X = object;
      cpu->Y = kActRaiserWram_PlayerObject;
      NativeSound(cpu, g_cpu_brk_hook, 0x0C, 0x008B24);
      cpu->X = x;
      cpu->Y = y;
    }
    if (shot->policy == kShield_Release) {
      const uint16_t shooter = Field(cpu, object, kActRaiserActionObject_SpawnerBacklink);
      if (shooter >= kActRaiserWram_ActionObjectTable && shooter < kObjectTableEnd)
        SetField(cpu, shooter, kActRaiserActionObject_LocalCounter, 0);
    }
    if (shot->policy != kShield_Absorb) {
      SetField(cpu, object, kActRaiserActionObject_Status, kStatusFree);
      continue;
    }
    /* Out of this pass only: $8970 skips hidden objects. */
    masked[masked_count] = object;
    masked_status[masked_count++] = status;
    SetField(cpu, object, kActRaiserActionObject_Status, (uint16_t)(status | kStatusHidden));
  }
  /* Persistent hazards can overlap across many contact passes. Sound only on
   * entry, and allow another impact after separation or lowering the shield. */
  memcpy(s_master.shield_contacts, contacts, sizeof(contacts));
  s_contact_delegate = true;
  const RecompReturn result = cpu->m_flag ? bank_00_8970_M1X0(cpu) : bank_00_8970_M0X0(cpu);
  s_contact_delegate = false;
  for (unsigned i = 0; i < masked_count; i++)
    SetField(cpu, masked[i], kActRaiserActionObject_Status, masked_status[i]);
  return result;
}
