#include "actraiser/actraiser_ar2_master_internal.h"
#include "actraiser_game.h"

#include <stdio.h>
#include <string.h>

/* Native handlers whose animations continue while AR1 owns player control. */
enum {
  kHurtFirst = 0x9C47,
  kHurtLast = 0x9CF1,
  kCastFirst = 0x9DE1,
  kCastLast = 0x9F12,
  kStageClearFirst = 0xA1CE,
  kStageClearLast = 0xA314,
  kDepartureFirst = 0xA315,
  kDepartureLast = 0xA343,
  kAnimationWait = 0x8661,
  kDepartureAnimationReturn = 0xA31D,
  kAr1AnimOrbRing = 0x10,
  kAr1AnimDeath = 0x0E,
  kAr1AnimationBank = 0x06,
  kAr1AnimationAddress = 0x8000,
  kObjTileWordBase = 0x2000,
  kAr1AtlasBank = 0x07,
  kAr1AtlasAddress = 0x8000,
  kPlayerPaletteIndex = 128 + 4 * 16,
  kOamFlipBits = 0xC000,
  kOamPaletteBits = 0x0E00,
};

/* ActRaiser fades OBJ palette 4 through eight palettes at $0B:8180: the
 * statue's stone (step 0), a blend, a glow (4-6) and the master's own colours
 * (7). The arrival walks it up ($9755's orb requests step 0, $97E4 advances)
 * and a stage clear walks it back down ($A2E5). Each step is an NMI palette
 * request: source $CB/$CD, CGRAM index $CE, palette count $CF ($02:AE75). */
enum {
  kFadeBank = 0x0B,
  kFadeTable = 0x8180,
  kFadeSteps = 8,
  kPaletteRequestSource = 0x00CB,
  kPaletteRequestBank = 0x00CD,
  kPaletteRequestIndex = 0x00CE,
  kPaletteRequestCount = 0x00CF,
};

/* AR2 animations shown while native ActRaiser code owns the player. */
enum {
  kAr2AnimArrival = 0x00, /* step 0: front-facing statue, sword raised vertically */
  kAr2AnimIdle = 0x01,
  kAr2AnimDeath = 0x05,
  kAr2AnimHurt = 0x08,
  kAr2AnimCast = 0x22,
};

/* OBJ table-0 tiles that only ActRaiser's own master draws from, at $80 and
 * up: its common-atlas compositions ($06:8000 states $00-$11) minus every
 * tile of the orb and arrival composite ($12, $15, $16 and $10's orb ring),
 * the sword beam ($13, $14), the death and pickup effects ($06:A800) and the
 * item score digits streamed to $D8-$DB. No enemy or boss animation uses
 * table 0, and scroll casts borrow only $00-$7F ($9E74). AR2 frames are
 * copied onto these, so nothing else loses its graphics; the largest frame
 * needs 54 of the 76. */
static const uint8_t kMasterOnlyTiles[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB4, 0xEF, 0xF0, 0xEF, 0x9C, 0xBD, 0xFC, 0x63, 0xFF, 0x03, 0x0F, 0x00, 0x9F, 0x07, 0xFF, 0x07};

typedef struct Ar2Pose {
  uint8_t anim, step;
  int16_t timer;
  uint16_t last_frame;
  int frame;
  bool started;
} Ar2Pose;

typedef struct Ar2Presentation {
  const Ar2MasterAssets *assets; /* Borrowed until session shutdown. */
  Ar2MasterPacking packing[kAr2MasterMaxFrames];
  Ar2Pose pose;
  ActRaiserAr2MasterDraw draw;
  bool enabled;
  uint16_t map, game_frame, object;
  int frame;
  bool vram_owned;
  uint8_t fade_step;
  uint16_t fade_palettes[kFadeSteps][16];
  bool fade_ready;
} Ar2Presentation;

static Ar2Presentation s_present;

static uint16_t Read16(const CpuState *cpu, uint16_t address) {
  return cpu_read16((CpuState *)cpu, 0, address);
}
static uint16_t Field(const CpuState *cpu, uint16_t object, unsigned offset) {
  return Read16(cpu, (uint16_t)(object + offset));
}
static bool InActionStage(const CpuState *cpu) {
  return ActRaiser_IsActionMapGroup(cpu_read8((CpuState *)cpu, 0, kActRaiserWram_MapGroup));
}

static bool StageClearHandler(uint16_t handler) {
  return handler >= kStageClearFirst && handler <= kStageClearLast;
}

static bool NativeMasterAnimations(const CpuState *cpu, uint16_t object) {
  return Field(cpu, object, kActRaiserActionObject_AnimationAddress) == kAr1AnimationAddress &&
         cpu_read8((CpuState *)cpu, 0, object + kActRaiserActionObject_AnimationBank) ==
             kAr1AnimationBank;
}

static bool DepartureOrb(const CpuState *cpu, uint16_t object) {
  const uint16_t handler = Field(cpu, object, kActRaiserActionObject_Handler);
  return (handler >= kDepartureFirst && handler <= kDepartureLast) ||
         (handler == kAnimationWait &&
          Field(cpu, object, kActRaiserActionObject_ResumeAddress) == kDepartureAnimationReturn);
}

/* $A64F swaps the player and defeated boss records; $A652 publishes the new
 * Master slot in camera subject $8A. $A2FE later allocates the departing orb,
 * usually reusing the freed $08A0. Follow the clear owner, not that old slot.
 * Before arrival has assigned $8A, the statue still lives at $08A0. */
static uint16_t MasterObject(const CpuState *cpu) {
  const uint16_t subject = Read16(cpu, kActRaiserWram_ActionCameraSubject);
  if (subject >= kActRaiserWram_PlayerObject &&
      subject < kActRaiserWram_ActionObjectTable +
                    kActRaiserActionObjectCount * kActRaiserActionObjectStride &&
      (subject - kActRaiserWram_ActionObjectTable) % kActRaiserActionObjectStride == 0 &&
      !(Field(cpu, subject, kActRaiserActionObject_Status) & kActRaiserObjectStatus_InactiveMask) &&
      NativeMasterAnimations(cpu, subject) &&
      StageClearHandler(Field(cpu, subject, kActRaiserActionObject_Handler)))
    return subject;
  return DepartureOrb(cpu, kActRaiserWram_PlayerObject) ? 0 : kActRaiserWram_PlayerObject;
}

void Ar2MasterPresentation_Reset(void) {
  s_present = (Ar2Presentation){.map = 0xFFFF, .frame = -1, .fade_step = kFadeSteps - 1};
}

bool Ar2MasterPresentation_Init(const Ar2MasterAssets *assets, char *error, size_t error_size) {
  Ar2MasterPresentation_Reset();
  for (unsigned i = 0; i < assets->frame_count; i++) {
    if (!Ar2MasterAssets_PackFrame(&assets->frames[i], kMasterOnlyTiles, &s_present.packing[i])) {
      snprintf(error, error_size, "frame %u does not fit the free OBJ tiles", i);
      return false;
    }
  }
  s_present.assets = assets;
  return true;
}

/* Advance native-owned poses by game time, including updates without a draw.
 * Work is bounded by the remaining animation steps; the final pose holds. */
static int PoseFrame(uint8_t anim, uint16_t game_frame) {
  Ar2Pose *pose = &s_present.pose;
  if (!pose->started || pose->anim != anim) {
    *pose = (Ar2Pose){
        .anim = anim, .timer = -1, .last_frame = (uint16_t)(game_frame - 1), .started = true};
  }
  const Ar2MasterAnim *program = &s_present.assets->anims[anim];
  unsigned ticks = (uint16_t)(game_frame - pose->last_frame);
  pose->last_frame = game_frame;
  while (ticks) {
    const unsigned until_next = pose->timer >= 0 ? (unsigned)pose->timer + 1 : 1;
    if (ticks < until_next) {
      pose->timer -= (int16_t)ticks;
      break;
    }
    ticks -= until_next;
    if (pose->step == program->step_count) {
      pose->timer = 0x7FFF;
      break;
    }
    pose->frame = program->steps[pose->step].frame;
    pose->timer = (int16_t)program->steps[pose->step].duration;
    pose->step++;
  }
  return pose->frame;
}

static int NativePoseFrame(uint16_t handler, uint8_t ar1_anim, uint16_t game_frame) {
  if (ActRaiser_PlayerArrivalAnimationActive(handler) || StageClearHandler(handler)) {
    s_present.pose.started = false;
    /* Hold the opening arrival frame, whose one-tick ROM duration would
     * otherwise immediately lower the sword. Casting uses a different pose. */
    return s_present.assets->anims[kAr2AnimArrival].steps[0].frame;
  }
  uint8_t anim = kAr2AnimIdle;
  if (ar1_anim == kAr1AnimDeath)
    anim = kAr2AnimDeath;
  else if (handler >= kHurtFirst && handler <= kHurtLast)
    anim = kAr2AnimHurt;
  else if (handler >= kCastFirst && handler <= kCastLast)
    anim = kAr2AnimCast;
  return PoseFrame(anim, game_frame);
}

/* The orb ring drawn over the statue lives in a shared native composition,
 * in the orb's palette; it is kept in front of the AR2 statue.
 * The master's own parts are the ones in palette 0 of the composition. */
static void KeepNativeOverlay(const CpuState *cpu, uint16_t object, ActRaiserAr2MasterDraw *out) {
  CpuState *bus = (CpuState *)cpu;
  const uint8_t bank = cpu_read8(bus, 0, (uint16_t)(object + kActRaiserActionObject_AnimationBank));
  const uint16_t composition = Field(cpu, object, kActRaiserActionObject_Composition);
  if (!composition) return;
  const unsigned count = cpu_read8(bus, bank, (uint16_t)(composition + 4));
  for (unsigned i = 0; i < count && out->count < kActRaiserAr2MasterMaxDrawParts; i++) {
    const uint16_t at = (uint16_t)(composition + 5 + i * 7);
    const uint16_t attributes = cpu_read16(bus, bank, (uint16_t)(at + 5));
    if (!(attributes & kOamPaletteBits)) continue;
    out->parts[out->count++] = (ActRaiserAr2MasterPart){
        .x = {cpu_read8(bus, bank, (uint16_t)(at + 1)), cpu_read8(bus, bank, (uint16_t)(at + 2))},
        .y = {cpu_read8(bus, bank, (uint16_t)(at + 3)), cpu_read8(bus, bank, (uint16_t)(at + 4))},
        .attributes = attributes,
        .large = (cpu_read8(bus, bank, at) & 1) != 0};
  }
}

static void BuildDraw(const CpuState *cpu, uint16_t object, int frame,
                      ActRaiserAr2MasterDraw *out) {
  const uint16_t handler = Field(cpu, object, kActRaiserActionObject_Handler);
  if (ActRaiser_PlayerArrivalAnimationActive(handler)) KeepNativeOverlay(cpu, object, out);
  /* AR2 parts sit on the feet: the native origin is the object minus its
   * live extents, and the controller keeps those at AR1's standard box. */
  const Ar2MasterFrame *f = &s_present.assets->frames[frame];
  const Ar2MasterPacking *packing = &s_present.packing[frame];
  const int left = (int16_t)Field(cpu, object, kActRaiserActionObject_LeftExtent);
  const int origin_y = (int16_t)Field(cpu, object, kActRaiserActionObject_TopExtent) +
                       (int16_t)Field(cpu, object, kActRaiserActionObject_BottomExtent) - f->top;
  for (unsigned i = 0; i < f->part_count && out->count < kActRaiserAr2MasterMaxDrawParts; i++) {
    const Ar2MasterPart *source = &f->parts[i];
    out->parts[out->count++] = (ActRaiserAr2MasterPart){
        .x = {source->x + left - f->left, source->x_flipped + left - f->right},
        .y = {source->y + origin_y, source->y_flipped + origin_y},
        .attributes = (uint16_t)((source->attributes & kOamFlipBits) | packing->part_tiles[i]),
        .large = source->large};
  }
}

void Ar2MasterPresentation_Update(const CpuState *cpu, bool enabled, int controlled_frame) {
  if (!s_present.assets || !cpu) return;
  enabled = enabled && InActionStage(cpu);
  const uint16_t object = enabled ? MasterObject(cpu) : 0;
  enabled = enabled && object != 0;
  const uint16_t map = Read16(cpu, kActRaiserWram_MapGroup);
  if (map != s_present.map || enabled != s_present.enabled || object != s_present.object)
    s_present.pose.started = false;
  s_present.map = map;
  s_present.object = object;
  s_present.enabled = enabled;
  s_present.frame = -1;
  s_present.draw = (ActRaiserAr2MasterDraw){0};
  if (!enabled) return;

  const uint16_t game_frame = Read16(cpu, kActRaiserWram_GameFrame);
  int frame = object == kActRaiserWram_PlayerObject ? controlled_frame : -1;
  if (frame >= 0) {
    s_present.pose.started = false;
    s_present.fade_step = kFadeSteps - 1;
  } else {
    frame = NativePoseFrame(
        Field(cpu, object, kActRaiserActionObject_Handler),
        (uint8_t)Field(cpu, object, kActRaiserActionObject_AnimationState),
        game_frame);
  }
  if (frame < 0 || frame >= s_present.assets->frame_count) return;
  BuildDraw(cpu, object, frame, &s_present.draw);
  s_present.frame = frame;
  s_present.game_frame = game_frame;
}

bool ActRaiserAr2Master_DrawObject(const CpuState *cpu, uint16_t object,
                                   ActRaiserAr2MasterDraw *out) {
  if (!out) return false;
  out->count = 0;
  if (!cpu || !s_present.assets || !s_present.enabled || s_present.frame < 0 ||
      !InActionStage(cpu) ||
      Read16(cpu, kActRaiserWram_MapGroup) != s_present.map)
    return false;
  if (object == s_present.object) {
    *out = s_present.draw;
    return true;
  }
  /* $A315 runs arrival's composite in reverse on the orb child before $A321
   * selects the free-flying orb. Its palette-0 body would draw AR1 tiles over
   * our statue. Preserve only the native ring; the clear owner draws the body.
   * Other orb animations and unrelated children retain their native OAM. */
  if (DepartureOrb(cpu, object) && NativeMasterAnimations(cpu, object) &&
      Field(cpu, object, kActRaiserActionObject_SpawnerBacklink) == s_present.object &&
      Field(cpu, object, kActRaiserActionObject_AnimationState) == kAr1AnimOrbRing) {
    KeepNativeOverlay(cpu, object, out);
    return true;
  }
  return false;
}

static void WriteVramWords(CpuState *cpu, uint16_t word_address, const uint8_t *bytes,
                           size_t count) {
  cpu_write8(cpu, 0, 0x2115, 0x80);
  cpu_write8(cpu, 0, 0x2116, (uint8_t)word_address);
  cpu_write8(cpu, 0, 0x2117, (uint8_t)(word_address >> 8));
  for (size_t i = 0; i + 1 < count; i += 2) {
    cpu_write8(cpu, 0, 0x2118, bytes[i]);
    cpu_write8(cpu, 0, 0x2119, bytes[i + 1]);
  }
}

/* Restore the native atlas and the current step of the statue fade. */
static void RestoreMaster(CpuState *cpu) {
  uint8_t bytes[32];
  for (unsigned tile = 0; tile < 256; tile++) {
    if (!(kMasterOnlyTiles[tile >> 3] >> (tile & 7) & 1)) continue;
    for (unsigned i = 0; i < sizeof(bytes); i++)
      bytes[i] = cpu_read8(cpu, kAr1AtlasBank, (uint16_t)(kAr1AtlasAddress + tile * 32 + i));
    WriteVramWords(cpu, (uint16_t)(kObjTileWordBase + tile * 16), bytes, sizeof(bytes));
  }
  cpu_write8(cpu, 0, 0x2121, kPlayerPaletteIndex);
  for (unsigned i = 0; i < 32; i++)
    cpu_write8(cpu, 0, 0x2122,
               cpu_read8(cpu, kFadeBank, (uint16_t)(kFadeTable + s_present.fade_step * 32 + i)));
  s_present.vram_owned = false;
}

/* The AR2 palette at every step of the statue fade, from ActRaiser's own
 * stone and master palettes (read once through the bus). */
static void PrepareFade(CpuState *cpu) {
  if (s_present.fade_ready) return;
  uint16_t native[kFadeSteps][16];
  for (unsigned step = 0; step < kFadeSteps; step++)
    for (unsigned i = 0; i < 16; i++)
      native[step][i] = cpu_read16(cpu, kFadeBank, (uint16_t)(kFadeTable + step * 32 + i * 2));
  for (unsigned step = 0; step < kFadeSteps; step++)
    Ar2MasterAssets_TranslateFade(s_present.assets->palette, native[0], native[kFadeSteps - 1],
                                  native[step], s_present.fade_palettes[step]);
  s_present.fade_ready = true;
}

/* Observe native fades even while AR1 is selected so a mid-fade switch
 * starts at the current colour. Only consume the request when AR2 owns the
 * prepared OAM; otherwise $02:AE75 must still upload the native palette. */
static void ObserveNativeFade(CpuState *cpu) {
  if (cpu_read8(cpu, 0, kPaletteRequestCount) != 1 ||
      cpu_read8(cpu, 0, kPaletteRequestIndex) != kPlayerPaletteIndex ||
      cpu_read8(cpu, 0, kPaletteRequestBank) != kFadeBank)
    return;
  const uint16_t offset = (uint16_t)(Read16(cpu, kPaletteRequestSource) - kFadeTable);
  if (offset % 32 || offset / 32 >= kFadeSteps) return;
  s_present.fade_step = (uint8_t)(offset / 32);
  if (s_present.enabled) cpu_write8(cpu, 0, kPaletteRequestCount, 0);
}

void ActRaiserAr2Master_UploadVideo(CpuState *cpu) {
  if (!s_present.assets || !cpu) return;
  if (!InActionStage(cpu)) {
    s_present.vram_owned = false; /* every mode change reloads OBJ VRAM */
    return;
  }
  ObserveNativeFade(cpu);
  if (!s_present.enabled) {
    if (s_present.vram_owned) RestoreMaster(cpu);
    return;
  }
  PrepareFade(cpu);
  cpu_write8(cpu, 0, 0x2121, kPlayerPaletteIndex);
  for (unsigned i = 0; i < 16; i++) {
    cpu_write8(cpu, 0, 0x2122, (uint8_t)s_present.fade_palettes[s_present.fade_step][i]);
    cpu_write8(cpu, 0, 0x2122, (uint8_t)(s_present.fade_palettes[s_present.fade_step][i] >> 8));
  }
  s_present.vram_owned = true; /* Palette ownership also needs restoring without a visible frame. */
  /* Level loads replace the atlas without telling us, so the frame's tiles
   * are re-sent every NMI: at most 54 tiles of register writes. */
  const uint16_t game_frame = Read16(cpu, kActRaiserWram_GameFrame);
  if (s_present.frame < 0 || (uint16_t)(game_frame - s_present.game_frame) > 2) return;
  const Ar2MasterFrame *frame = &s_present.assets->frames[s_present.frame];
  const Ar2MasterPacking *packing = &s_present.packing[s_present.frame];
  for (unsigned i = 0; i < packing->copy_count; i++) {
    const Ar2MasterTileCopy *copy = &packing->copies[i];
    WriteVramWords(cpu, (uint16_t)(kObjTileWordBase + copy->tile * 16),
                   s_present.assets->data + frame->tile_offset + copy->source * 32u, 32);
  }
}
