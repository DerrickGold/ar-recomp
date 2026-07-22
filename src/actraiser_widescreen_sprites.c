/* Staged action and simulation-town sprite handling for widescreen.
 *
 * $00:8D68 widens per-definition emission. The audited $00:8C98 replacement
 * independently controls initialized margin-object drawing (Stage D1) and
 * the $0400 activation boundary (Stage D2). Keeping those decisions separate
 * is essential: a fidelity run can disable either without replacing binaries.
 *
 * AR_WS_SPRITES=0 restores authentic per-definition emission.
 * AR_WS_MARGIN_OBJECTS=0 restores authentic object draw coverage.
 * AR_WS_MARGIN_ACTIVATION=0 restores the authentic $0400 boundary; Stage D2
 * is enabled by default after direct Fillmore validation.
 *
 * $01:B4C6 is the town camera follow/clamp. Its faithful port preserves the
 * ROM's complete 0..256 camera range; the PPU narrows only the unavailable
 * side margin at a finite-world edge. $01:ADAD/$01:AE6F widen only $0A00+
 * world records, and $01:B473 extends the dedicated angel-projectile lifetime
 * check to the same finite horizontal window.
 * AR_WS_SIM=0 restores the authentic camera; AR_WS_SIM_SPRITES=0 keeps both
 * sprite/projectile predicates authentic. */

#include "cpu_state.h"
#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "settings.h"
#include "sim_render_metadata.h"
#include "snes/ppu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

extern Ppu *g_ppu;
extern bool g_ws_active;
extern int g_ws_extra;
extern RecompReturn bank_00_923A_M0X0(CpuState *cpu);

RecompReturn ActRaiser_BuildObjectSprites(CpuState *cpu);

typedef enum ActionObjectField {
  kActionObject_Status = 0x00,
  kActionObject_WorldX = 0x02,
  kActionObject_WorldY = 0x04,
  kActionObject_LeftExtent = 0x0A,
  kActionObject_TopExtent = 0x0C,
  kActionObject_RightExtent = 0x0E,
  kActionObject_BottomExtent = 0x10,
  kActionObject_Handler = 0x12,
  kActionObject_Type = 0x16,
  kActionObject_DefinitionBank = 0x18,
  kActionObject_Definition = 0x20,
  kActionObject_FlipAttributes = 0x28,
  kActionObject_Flags = 0x30,
} ActionObjectField;

typedef enum SimRecordField {
  kSimRecord_Behavior = 0x00,
  kSimRecord_ScriptCursor = 0x02,
  kSimRecord_FrameTimer = 0x04,
  kSimRecord_ActorFlags = 0x06,
  kSimRecord_Composition = 0x08,
  kSimRecord_WorldX = 0x0A,
  kSimRecord_WorldY = 0x0C,
  kSimRecord_Type = 0x0E,
  kSimRecord_Status = 0x10,
  kSimRecord_State = 0x12,
} SimRecordField;

enum {
  kSpriteDp_ComponentCount = 0x0C,
  kSpriteDp_ScreenOriginX = 0x14,
  kSpriteDp_ScreenOriginY = 0x16,
  kSpriteDp_AttributeBias = 0x8F,
  kSpriteDp_CameraOriginX = 0x94,
  kSpriteDp_CameraOriginY = 0x96,
  kSpriteDp_OamCursor = 0x98,
  kSpriteDp_OamHighCursor = 0x9A,
  kSpriteDp_OamHighSlotsRemaining = 0x9C,
  kSpriteDp_FlipAttributes = 0x9E,
  kSpriteDefinitionPartBytes = 7,
  kSimSpriteDefinitionPartBytes = 5,
  kSpriteDrawBias = 16,
  kSpriteBiasedWidth = kActRaiserAuthenticWidth + kSpriteDrawBias,
  kSpriteBiasedHeight = kActRaiserAuthenticHeight + kSpriteDrawBias,
  kActivationProbeObjectCount = 64,
  kBuildHudSpritesReturnAddress = 0x8CDD,
  kBuildObjectSpritesReturnAddress = 0x8D35,
  kParkedActionOamEntry = 0xE080,
  kParkedSimOamEntry = 0xE000,
  kActionSpriteAttributeBias = 0x0E00,
  kObjectFlipAttributeXor = 0x0100,
  kObjectSpriteAttributeBiasFlags = 0x2008,
  kDefinitionFlipHorizontal = 0x4000,
  kDefinitionFlipVertical = 0x8000,
  kOamEntryBytes = 4,
  kOamYFieldOffset = 1,
  kOamTileAttributeOffset = 2,
  kActionDefinitionHeaderBytes = 4,
  kActionPartFlags = 0,
  kActionPartXOffsets = 1,
  kActionPartYOffsets = 3,
  kActionPartTileAttributes = 5,
  kSimCameraCenterX = 0x0080,
  kSimCameraCenterY = 0x0070,
  kSimVerticalViewportHeight = 0x00E0,
  kSimProjectileAnchorX = 4,
  kSimOamBiasedWidth = kActRaiserAuthenticWidth + kSpriteDrawBias,
  kSimOamBiasedHeight = 0x00F0,
};

/* Sim3D_CullProximity mirrors this window so it can be evaluated without a
 * CPU. The mirror is only sound while the two agree. */
_Static_assert(kSimOamBiasedWidth == kSimSpriteWindowBiasedWidth,
               "cull-lead window drifted from the emitter's x predicate");
_Static_assert(kSimOamBiasedHeight == kSimSpriteWindowBiasedHeight,
               "cull-lead window drifted from the emitter's y predicate");

static inline uint16 ws_dp16(CpuState *cpu, uint16 off) {
  uint16 a = (uint16)(cpu->D + off);
  return (uint16)(g_ram[a] | (g_ram[(uint16)(a + 1)] << 8));
}

static inline void ws_dp16w(CpuState *cpu, uint16 off, uint16 v) {
  uint16 a = (uint16)(cpu->D + off);
  g_ram[a] = (uint8)v;
  g_ram[(uint16)(a + 1)] = (uint8)(v >> 8);
}

static int ws_sprite_widen_enabled(void) {
  return g_settings.ws_sprites;
}

static int ws_sprite_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_SPRDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

typedef struct WsActivationCandidate {
  uint8 present;
  uint8 definition_bank;
  uint16 status;
  uint16 flags;
  uint16 handler;
  uint16 definition_address;
  uint16 object_type;
} WsActivationCandidate;

/* Match $8C98's bounding-box interpretation without touching $0400. The
 * object's leading/trailing extents are unsigned distances from its origin;
 * subtraction is interpreted in the same 16-bit signed screen space used by
 * the ROM's wrap-aware two-edge tests. */
static int ws_axis_visible(uint16 pos, uint16 leading, uint16 trailing,
                           uint16 camera, int window_lo, int window_hi,
                           int *screen_lo, int *screen_hi) {
  int lo = (int)(int16_t)(uint16)(pos - leading - camera);
  int hi = (int)(int16_t)(uint16)(pos + trailing - camera);
  if (screen_lo) *screen_lo = lo;
  if (screen_hi) *screen_hi = hi;
  return lo < window_hi && hi >= window_lo;
}

/* AR_WS_ACTDBG=1: read-only Stage-D reconnaissance. Log transitions for
 * drawable object slots whose bounding boxes intersect a live side margin but
 * not the authentic 256px activation window. This deliberately does not call
 * object logic, alter $0400, build OAM, or load graphics. */
void ActRaiser_WidescreenSpriteActivationProbe(void) {
  static int enabled = -1;
  static WsActivationCandidate prior[kActivationProbeObjectCount];
  if (enabled < 0) {
    const char *e = getenv("AR_WS_ACTDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  if (!enabled || !g_ppu ||
      !ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]))
    return;

  int margin_left = g_ppu->extraLeftCur;
  int margin_right = g_ppu->extraRightCur;
  if (!(margin_left | margin_right))
    return;

  uint16 camera_x = ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
  uint16 camera_y = ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);
  unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);

  for (int slot = 0; slot < kActivationProbeObjectCount; slot++) {
    uint16 object_address = (uint16)(
        kActRaiserWram_ActionObjectTable +
        slot * kActRaiserActionObjectStride);
    uint16 status = ActRaiser_ReadWram16(
        (uint16)(object_address + kActionObject_Status));
    if (status & kActRaiserObjectStatus_End)
      break;

    int screen_left = 0, screen_right = 0;
    int eligible = !(status & kActRaiserObjectStatus_IneligibleMask);
    int vertical = eligible && ws_axis_visible(
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_WorldY)),
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_TopExtent)),
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_BottomExtent)),
        camera_y, 0, kActRaiserAuthenticHeight, NULL, NULL);
    int authentic = vertical && ws_axis_visible(
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_WorldX)),
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_LeftExtent)),
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_RightExtent)),
        camera_x, 0, kActRaiserAuthenticWidth,
        &screen_left, &screen_right);
    int wide = vertical && ws_axis_visible(
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_WorldX)),
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_LeftExtent)),
        ActRaiser_ReadWram16(
            (uint16)(object_address + kActionObject_RightExtent)),
        camera_x, -margin_left,
        kActRaiserAuthenticWidth + margin_right,
        &screen_left, &screen_right);
    int candidate = wide && !authentic;

    uint16 flags = ActRaiser_ReadWram16(
        (uint16)(object_address + kActionObject_Flags));
    uint16 handler = ActRaiser_ReadWram16(
        (uint16)(object_address + kActionObject_Handler));
    uint8 definition_bank =
        g_ram[(uint16)(object_address + kActionObject_DefinitionBank)];
    uint16 definition_address = ActRaiser_ReadWram16(
        (uint16)(object_address + kActionObject_Definition));
    uint16 object_type = ActRaiser_ReadWram16(
        (uint16)(object_address + kActionObject_Type));
    WsActivationCandidate *previous = &prior[slot];

    if (candidate) {
      int changed = !previous->present || previous->status != status ||
                    previous->flags != flags ||
                    previous->handler != handler ||
                    previous->definition_bank != definition_bank ||
                    previous->definition_address != definition_address ||
                    previous->object_type != object_type;
      if (changed) {
        fprintf(stderr,
                "[ws-activate] gf=%u slot=%d obj=$%04X event=%s side=%c "
                "span=[%d,%d] margins=%d/%d status=$%04X flags30=$%04X "
                "handler=$%04X type=$%04X def=$%02X:%04X\n",
                game_frame, slot, object_address,
                previous->present ? "change" : "enter",
                screen_right < 0 ? 'L' : 'R', screen_left, screen_right,
                margin_left, margin_right, status,
                flags, handler, object_type, definition_bank,
                definition_address);
      }
      previous->present = 1;
      previous->status = status;
      previous->flags = flags;
      previous->handler = handler;
      previous->definition_bank = definition_bank;
      previous->definition_address = definition_address;
      previous->object_type = object_type;
    } else if (previous->present) {
      fprintf(stderr,
              "[ws-activate] gf=%u slot=%d obj=$%04X event=exit reason=%s "
              "span=[%d,%d] status=$%04X flags30=$%04X\n",
              game_frame, slot, object_address,
              authentic ? "authentic" : "outside",
              screen_left, screen_right, status, flags);
      previous->present = 0;
    }
  }
}

static int ws_margin_objects_enabled(void) {
  return g_settings.ws_margin_objects;
}

static int ws_margin_activation_enabled(void) {
  return g_settings.ws_margin_activation;
}

static int ws_scan_axis_visible(uint16 pos, uint16 leading, uint16 trailing,
                                uint16 camera, int left, int right,
                                uint16 limit) {
  uint16 span = (uint16)(limit + left + right);
  uint16 edge0 = (uint16)(pos - leading - camera + left);
  if (edge0 < span)
    return 1;
  if (edge0 & 0x8000) {
    uint16 edge1 = (uint16)(pos + trailing - camera + left);
    if (edge1 < span)
      return 1;
  }
  return 0;
}

/* Stage D1/D2 replacement for $00:8C98. Object drawing and $0400 activation
 * use separate horizontal windows and retain independent fidelity switches.
 * This reproduces the ROM's PHP/PLP stack byte and its normal two-bit-per-slot
 * high-table flush, which the historical scan port did not. */
RecompReturn ActRaiser_ObjectVisibilityScanWide(CpuState *cpu) {
  cpu_mirrors_to_p(cpu);
  cpu_write8(cpu, 0x00, cpu->S, cpu->P);
  cpu->S = (uint16)(cpu->S - 1);          /* PHP */
  cpu->P &= (uint8)~0x30;                /* REP #$30 */
  cpu_p_to_mirrors(cpu);

  uint16 saved_stack_pointer = cpu->S;
  ws_dp16w(cpu, kSpriteDp_CameraOriginX,
           (uint16)(ws_dp16(cpu, kActRaiserWram_Bg1CameraX) -
                    kSpriteDrawBias));
  ws_dp16w(cpu, kSpriteDp_CameraOriginY,
           (uint16)(ws_dp16(cpu, kActRaiserWram_Bg1CameraY) -
                    kSpriteDrawBias));
  ws_dp16w(cpu, kSpriteDp_OamHighCursor, kActRaiserOamHighTable);
  ws_dp16w(cpu, kSpriteDp_OamHighSlotsRemaining, 4);
  ws_dp16w(cpu, 0x00, saved_stack_pointer);
  for (int offset = 0; offset < kActRaiserOamLowTableBytes; offset += 2) {
    g_ram[kActRaiserOamShadow + offset] = 0x80;
    g_ram[kActRaiserOamShadow + offset + 1] = 0xE0;
  }

  cpu->A = saved_stack_pointer;
  cpu->X = 0;
  cpu->Y = 0;
  cpu->_flag_Z = 1;
  cpu->_flag_N = 0;
  cpu->P = (uint8)((cpu->P & ~0x82) | 0x02);
  {
    uint16 call_s = cpu->S;
    cpu_write8(cpu, 0x00, cpu->S,
               (uint8)(kBuildHudSpritesReturnAddress >> 8));
    cpu->S--;
    cpu_write8(cpu, 0x00, cpu->S,
               (uint8)kBuildHudSpritesReturnAddress);
    cpu->S--;
    cpu->host_return_valid = 1;
    RecompReturn r = bank_00_923A_M0X0(cpu);
    cpu->S = call_s;
    if (r != RECOMP_RETURN_NORMAL)
      return r;
  }

  uint16 object_address = kActRaiserWram_ActionObjectTable;
  uint16 oam_offset = cpu->Y;
  uint16 terminal_status = 0;
  int oam_full = 0;
  int live_l = 0, live_r = 0;
  if (g_ppu &&
      ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
    live_l = g_ppu->extraLeftCur;
    live_r = g_ppu->extraRightCur;
  }
  int draw_l = ws_margin_objects_enabled() ? live_l : 0;
  int draw_r = ws_margin_objects_enabled() ? live_r : 0;
  int activation_wide = ws_margin_activation_enabled();
  int activation_l = activation_wide ? live_l : 0;
  int activation_r = activation_wide ? live_r : 0;
  const char *actdbg = getenv("AR_WS_ACTDBG");
  int activation_debug = actdbg && actdbg[0] && actdbg[0] != '0';
  uint16 camera_x = ws_dp16(cpu, kActRaiserWram_Bg1CameraX);
  uint16 camera_y = ws_dp16(cpu, kActRaiserWram_Bg1CameraY);

  for (;;) {
    uint16 status = cpu_read16(
        cpu, cpu->DB, (uint16)(object_address + kActionObject_Status));
    if (status & kActRaiserObjectStatus_End) {
      terminal_status = status;
      break;
    }
    if (!(status & kActRaiserObjectStatus_IneligibleMask)) {
      uint16 world_x = cpu_read16(
          cpu, cpu->DB,
          (uint16)(object_address + kActionObject_WorldX));
      uint16 world_y = cpu_read16(
          cpu, cpu->DB,
          (uint16)(object_address + kActionObject_WorldY));
      uint16 left_extent = cpu_read16(
          cpu, cpu->DB,
          (uint16)(object_address + kActionObject_LeftExtent));
      uint16 top_extent = cpu_read16(
          cpu, cpu->DB,
          (uint16)(object_address + kActionObject_TopExtent));
      uint16 right_extent = cpu_read16(
          cpu, cpu->DB,
          (uint16)(object_address + kActionObject_RightExtent));
      uint16 bottom_extent = cpu_read16(
          cpu, cpu->DB,
          (uint16)(object_address + kActionObject_BottomExtent));
      int vertical = ws_scan_axis_visible(
          world_y, top_extent, bottom_extent, camera_y, 0, 0,
          kActRaiserAuthenticHeight);
      int authentic = vertical &&
          ws_scan_axis_visible(world_x, left_extent, right_extent,
                               camera_x, 0, 0,
                               kActRaiserAuthenticWidth);
      int draw = vertical &&
          ws_scan_axis_visible(world_x, left_extent, right_extent,
                               camera_x, draw_l, draw_r,
                               kActRaiserAuthenticWidth);
      int activation = vertical && ws_scan_axis_visible(
          world_x, left_extent, right_extent, camera_x,
          activation_l, activation_r, kActRaiserAuthenticWidth);

      if (draw && !(status & kActRaiserObjectStatus_NoDraw)) {
        cpu->X = object_address;
        cpu->Y = oam_offset;
        uint16 call_s = cpu->S;
        cpu_write8(cpu, 0x00, cpu->S,
                   (uint8)(kBuildObjectSpritesReturnAddress >> 8));
        cpu->S--;
        cpu_write8(cpu, 0x00, cpu->S,
                   (uint8)kBuildObjectSpritesReturnAddress);
        cpu->S--;
        cpu->host_return_valid = 1;
        RecompReturn r = ActRaiser_BuildObjectSprites(cpu);
        cpu->S = call_s;
        if (r != RECOMP_RETURN_NORMAL)
          return r;
        oam_offset = cpu->Y;
        if (cpu->_flag_C) {
          oam_full = 1;
          break;
        }
      }

      uint16 flags = cpu_read16(
          cpu, cpu->DB,
          (uint16)(object_address + kActionObject_Flags));
      uint16 next_flags = activation
          ? (uint16)(flags & ~kActRaiserObjectFlag_OutsideActivation)
          : (uint16)(flags | kActRaiserObjectFlag_OutsideActivation);
      if (activation_debug &&
          ((flags ^ next_flags) &
           kActRaiserObjectFlag_OutsideActivation)) {
        int screen_left = (int)(int16_t)(uint16)(
            world_x - left_extent - camera_x);
        int screen_right = (int)(int16_t)(uint16)(
            world_x + right_extent - camera_x);
        uint16 handler = cpu_read16(
            cpu, cpu->DB,
            (uint16)(object_address + kActionObject_Handler));
        uint16 object_type = cpu_read16(
            cpu, cpu->DB,
            (uint16)(object_address + kActionObject_Type));
        uint8 definition_bank = cpu_read8(
            cpu, cpu->DB,
            (uint16)(object_address + kActionObject_DefinitionBank));
        uint16 definition_address = cpu_read16(
            cpu, cpu->DB,
            (uint16)(object_address + kActionObject_Definition));
        fprintf(stderr,
                "[ws-activation-state] gf=%u slot=%u obj=$%04X "
                "$0400=%u->%u mode=%s authentic=%d draw=%d active=%d "
                "span=[%d,%d] margins=%d/%d handler=$%04X type=$%04X "
                "def=$%02X:%04X\n",
                (unsigned)ws_dp16(cpu, kActRaiserWram_GameFrame),
                (unsigned)((object_address -
                    kActRaiserWram_ActionObjectTable) /
                    kActRaiserActionObjectStride), object_address,
                !!(flags & kActRaiserObjectFlag_OutsideActivation),
                !!(next_flags & kActRaiserObjectFlag_OutsideActivation),
                activation_wide ? "wide" : "authentic",
                authentic, draw, activation, screen_left, screen_right,
                live_l, live_r, handler, object_type, definition_bank,
                definition_address);
      }
      cpu_write16(cpu, cpu->DB,
                  (uint16)(object_address + kActionObject_Flags),
                  next_flags);
    }
    object_address =
        (uint16)(object_address + kActRaiserActionObjectStride);
  }

  if (!oam_full) {
    uint8 acc = g_ram[(uint16)(cpu->D + 0x00)];
    uint16 count = ws_dp16(cpu, kSpriteDp_OamHighSlotsRemaining);
    do {
      acc >>= 2;
      count--;
    } while (count != 0);
    ws_dp16w(cpu, kSpriteDp_OamHighSlotsRemaining, count);
    g_ram[ws_dp16(cpu, kSpriteDp_OamHighCursor)] = acc;
    /* SEP #$20; LDA $00 changes only A.low. A.high remains the terminator
     * status high byte and X remains the terminator object address. */
    cpu->A = (uint16)((terminal_status & 0xFF00) | acc);
    cpu->X = object_address;
    cpu->Y = oam_offset;
    cpu->_flag_Z = 1;
    cpu->_flag_N = 0;
    cpu->P = (uint8)((cpu->P & ~0x82) | 0x02);
  }

  cpu->S = (uint16)(cpu->S + 1);          /* PLP */
  cpu->P = cpu_read8(cpu, 0x00, cpu->S);
  cpu_p_to_mirrors(cpu);
  if (cpu->x_flag) {
    cpu->X &= 0x00FF;
    cpu->Y &= 0x00FF;
  }
  cpu->S = (uint16)(cpu->S + 2);          /* RTS */
  return RECOMP_RETURN_NORMAL;
}

/* hle_func replacement for $00:8D68. Entry contract from original $8C98:
 * M=0, X=0, X=object base, Y=next OAM-shadow byte offset. Return preserves
 * the object in X, advances Y, and reports OAM-full through carry. */
RecompReturn ActRaiser_BuildObjectSprites(CpuState *cpu) {
  uint16 object_address = cpu->X;
  uint16 oam_offset = cpu->Y;
  int oam_full = 0;

  uint16 screen_origin_x = (uint16)(
      cpu_read16(cpu, cpu->DB,
                 (uint16)(object_address + kActionObject_WorldX)) -
      cpu_read16(cpu, cpu->DB,
                 (uint16)(object_address + kActionObject_LeftExtent)) -
      ws_dp16(cpu, kSpriteDp_CameraOriginX));
  uint16 screen_origin_y = (uint16)(
      cpu_read16(cpu, cpu->DB,
                 (uint16)(object_address + kActionObject_WorldY)) -
      cpu_read16(cpu, cpu->DB,
                 (uint16)(object_address + kActionObject_TopExtent)) -
      ws_dp16(cpu, kSpriteDp_CameraOriginY));
  ws_dp16w(cpu, kSpriteDp_ScreenOriginX, screen_origin_x);
  ws_dp16w(cpu, kSpriteDp_ScreenOriginY, screen_origin_y);

  uint16 flip_attributes = (uint16)(
      cpu_read16(cpu, cpu->DB,
                 (uint16)(object_address + kActionObject_FlipAttributes)) ^
      kObjectFlipAttributeXor);
  ws_dp16w(cpu, kSpriteDp_FlipAttributes, flip_attributes);
  if (cpu_read16(cpu, cpu->DB,
                 (uint16)(object_address + kActionObject_Flags)) &
      kObjectSpriteAttributeBiasFlags) {
    ws_dp16w(cpu, kSpriteDp_AttributeBias,
             (uint16)(ws_dp16(cpu, kSpriteDp_AttributeBias) |
                      kActionSpriteAttributeBias));
  }

  uint8 definition_bank = cpu_read8(
      cpu, cpu->DB,
      (uint16)(object_address + kActionObject_DefinitionBank));
  uint16 definition_address = (uint16)(
      cpu_read16(cpu, cpu->DB,
                 (uint16)(object_address + kActionObject_Definition)) +
      kActionDefinitionHeaderBytes);
  uint16 component_count =
      cpu_read8(cpu, definition_bank, definition_address);
  definition_address++;
  ws_dp16w(cpu, kSpriteDp_ComponentCount, component_count);

  int margin_left = 0;
  int margin_right = 0;
  if (ws_sprite_widen_enabled() && g_ppu &&
      ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
    margin_left = g_ppu->extraLeftCur;
    margin_right = g_ppu->extraRightCur;
  }

  for (;;) {
    flip_attributes = ws_dp16(cpu, kSpriteDp_FlipAttributes);
    uint16 y_offsets = (uint16)(
        cpu_read8(cpu, definition_bank,
                  (uint16)(definition_address + kActionPartYOffsets)) |
        (cpu_read8(cpu, definition_bank,
                   (uint16)(definition_address +
                            kActionPartYOffsets + 1)) << 8));
    uint16 component_offset_y =
        (flip_attributes & kDefinitionFlipVertical)
            ? (uint16)(y_offsets >> 8) : (uint16)(y_offsets & 0xFF);
    uint16 biased_y = (uint16)(
        component_offset_y + ws_dp16(cpu, kSpriteDp_ScreenOriginY));

    if (biased_y < kSpriteBiasedHeight) {
      /* CMP failed with carry clear, so the ROM's SBC #$0010 stores y-$11. */
      uint16 stored_y = (uint16)(biased_y - (kSpriteDrawBias + 1));
      cpu_write16(cpu, definition_bank,
                  (uint16)(kActRaiserOamShadow + oam_offset +
                           kOamYFieldOffset),
                  stored_y);

      uint16 tile_attributes = (uint16)(
          cpu_read8(cpu, definition_bank,
                    (uint16)(definition_address +
                             kActionPartTileAttributes)) |
          (cpu_read8(cpu, definition_bank,
                     (uint16)(definition_address +
                              kActionPartTileAttributes + 1)) << 8));
      uint16 rendered_attributes = (uint16)(
          (tile_attributes ^ flip_attributes) |
          ws_dp16(cpu, kSpriteDp_AttributeBias));
      cpu_write16(cpu, definition_bank,
                  (uint16)(kActRaiserOamShadow + oam_offset +
                           kOamTileAttributeOffset),
                  rendered_attributes);

      uint16 x_offsets = (uint16)(
          cpu_read8(cpu, definition_bank,
                    (uint16)(definition_address + kActionPartXOffsets)) |
          (cpu_read8(cpu, definition_bank,
                     (uint16)(definition_address +
                              kActionPartXOffsets + 1)) << 8));
      uint16 component_offset_x =
          (flip_attributes & kDefinitionFlipHorizontal)
              ? (uint16)(x_offsets >> 8) : (uint16)(x_offsets & 0xFF);
      uint16 biased_x = (uint16)(
          component_offset_x + ws_dp16(cpu, kSpriteDp_ScreenOriginX));

      /* Authentic: x<$110 => screen-x in [-16,256). Wide:
       * (x+L)<$110+L+R => screen-x in [-16-L,256+R). The 16px left reach is
       * exactly ActRaiser's maximum OAM tile width. The historical fixed-64
       * reach over-emitted invisible definitions and is intentionally gone. */
      uint16 wide_x = (uint16)(biased_x + margin_left);
      uint16 wide_bound =
          (uint16)(kSpriteBiasedWidth + margin_left + margin_right);
      if (wide_x < wide_bound) {
        uint16 screen_x = (uint16)(biased_x - kSpriteDrawBias);
        cpu_write8(cpu, definition_bank,
                   (uint16)(kActRaiserOamShadow + oam_offset),
                   (uint8)screen_x);

        uint8 acc = g_ram[(uint16)(cpu->D + 0x00)];
        acc = (uint8)((acc >> 1) | (((screen_x >> 8) & 1) << 7));
        acc = (uint8)((acc >> 1) |
                      ((cpu_read8(cpu, definition_bank,
                                  (uint16)(definition_address +
                                           kActionPartFlags)) & 1) << 7));
        g_ram[(uint16)(cpu->D + 0x00)] = acc;

        uint16 slots = (uint16)(
            ws_dp16(cpu, kSpriteDp_OamHighSlotsRemaining) - 1);
        ws_dp16w(cpu, kSpriteDp_OamHighSlotsRemaining, slots);
        if (slots == 0) {
          uint16 high_table_address =
              ws_dp16(cpu, kSpriteDp_OamHighCursor);
          g_ram[high_table_address] = acc;
          ws_dp16w(cpu, kSpriteDp_OamHighCursor,
                   (uint16)(high_table_address + 1));
          ws_dp16w(cpu, kSpriteDp_OamHighSlotsRemaining, 4);
        }

        if (ws_sprite_debug_enabled()) {
          int signed_screen_x = screen_x & 0x8000
              ? (int)screen_x - 0x10000 : (int)screen_x;
          if (signed_screen_x < -kSpriteDrawBias ||
              signed_screen_x >= kActRaiserAuthenticWidth) {
            unsigned game_frame =
                ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
            fprintf(stderr,
                    "[ws-sprite] gf=%u obj=$%04X def=$%02X:%04X x=%d y=%u tile=$%02X\n",
                    game_frame, object_address, definition_bank,
                    (unsigned)(uint16)(definition_address - 1),
                    signed_screen_x,
                    (unsigned)(stored_y & 0xFF),
                    (unsigned)(tile_attributes & 0xFF));
          }
        }

        oam_offset = (uint16)(oam_offset + kOamEntryBytes);
        if (oam_offset == kActRaiserOamLowTableBytes) {
          oam_full = 1;
          break;
        }
      } else {
        /* Original reject occurs after y/tile writes. Re-park x/y only. */
        cpu_write16(cpu, definition_bank,
                    (uint16)(kActRaiserOamShadow + oam_offset),
                    kParkedActionOamEntry);
      }
    }

    definition_address =
        (uint16)(definition_address + kSpriteDefinitionPartBytes);
    component_count--;
    ws_dp16w(cpu, kSpriteDp_ComponentCount, component_count);
    if (component_count == 0)
      break;
  }

  ws_dp16w(cpu, kSpriteDp_AttributeBias,
           (uint16)(ws_dp16(cpu, kSpriteDp_AttributeBias) &
                    ~kActionSpriteAttributeBias));
  cpu->A = kActionSpriteAttributeBias;
  cpu->X = object_address;
  cpu->Y = oam_offset;
  cpu->_flag_C = oam_full ? 1 : 0;
  cpu->_flag_Z = object_address == 0;
  cpu->_flag_N = (object_address & 0x8000) != 0;
  cpu->P = (uint8)((cpu->P & ~0x83) |
                   (cpu->_flag_N ? 0x80 : 0) |
                   (cpu->_flag_Z ? 0x02 : 0) |
                   (cpu->_flag_C ? 0x01 : 0));
  cpu->m_flag = 0;
  cpu->P &= (uint8)~0x20;

  /* Emulate the replaced RTS; the generated paired caller then restores S. */
  cpu->S = (uint16)(cpu->S + 2);
  return RECOMP_RETURN_NORMAL;
}

/* ── Simulation-town world sprite composition ─────────────────────────────
 *
 * $01:ACD9 has two record scans which share these leaf emitters:
 *   fixed/UI  $06A0-$09FF (48 x $12 bytes)
 *   world     $0A00-$1087 (44 x $26 bytes)
 *
 * The original $01:ADAD/$01:AE6F bodies differ only in their attribute-word
 * transform. Keep every other ROM behavior—including the unusual $80 offset
 * rule, vertical clipping, rejected-slot parking, OAM high-table packing, and
 * shared $98 cursor—inside one faithful port. Widescreen changes only the
 * horizontal predicate, only for the world array, and only in town mode.
 *
 * AR_WS_SIM_SPRITES=0 restores the authentic predicate in the same generated
 * binary. AR_WS_SIM_SPRDBG=1 logs only components newly admitted into a live
 * side margin; it never changes OAM or game state. */

static int ws_sim_sprite_widen_enabled(void) {
  /* Still gated on the AR_WS_SIM master, as before the refactor. */
  return g_settings.ws_sim_sprites && g_settings.ws_sim;
}

static int ws_sim_sprite_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_SIM_SPRDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

/* The ROM sign-extends component offsets $81-$FF, but deliberately treats
 * $80 as +128. Do not replace this with an int8_t cast. */
static uint16 ws_sim_part_offset(uint8 v) {
  return v >= 0x81 ? (uint16)(0xFF00 | v) : (uint16)v;
}

static void ws_sim_live_margins(int *left, int *right) {
  *left = 0;
  *right = 0;
  if (!g_ws_active || !ws_sim_sprite_widen_enabled() ||
      !ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                  g_ram[kActRaiserWram_CurrentMap]))
    return;

  int camera_x = (int)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
  if (camera_x < 0) camera_x = 0;
  if (camera_x > kActRaiserTownCameraMaximumX)
    camera_x = kActRaiserTownCameraMaximumX;
  *left = camera_x < g_ws_extra ? camera_x : g_ws_extra;
  int available_right = kActRaiserTownCameraMaximumX - camera_x;
  *right = available_right < g_ws_extra ? available_right : g_ws_extra;
}

void ActRaiser_SimSpriteMargins(int *left, int *right) {
  ws_sim_live_margins(left, right);
}

/* Widening the SIM sprite emitter for the 3D view: PROTOTYPED, MEASURED, AND
 * REVERTED 2026-07-22. Recorded because both directions look obviously right
 * and both are wrong.
 *
 * HORIZONTAL. Lifting the emitter's predicate from the widescreen `g_ws_extra`
 * cap to the whole finite town admitted +16.9% more sprites, left the
 * source-record count byte-identical (so it stayed observational), and passed
 * the D2 gate at zero mismatch. It still had to come out: OAM X is nine bits
 * over a 512-wide town, so once the margin reaches the wrap the emitter admits
 * parts on both sides of it, and enemies appear wrapped onto the opposite edge
 * of the map. The same wrap is what blew up a multi-part composition's union
 * bounding box past the 512-pixel atlas (ledger 24). The `g_ws_extra` cap was
 * doing more than bounding the widescreen margin; it was keeping the emitter
 * clear of the wrap.
 *
 * VERTICAL. PpuObjScreenY reads the Y byte as `y >= 224 ? y - 256 : y`, so it
 * expresses screen rows -32..223: the 32 rows above the screen look free, and
 * there is no ninth Y bit to extend downward with. But a part at row -5 with a
 * 16-pixel body draws rows 0..10 -- inside the authentic 224-row viewport --
 * so admitting them makes sprite pixels appear that the original game hides.
 * A checkpoint run confirmed it by failing the authentic-framebuffer
 * comparison (ledger 25).
 *
 * Whichever direction, the emitter must never feed `ws_sim_live_margins`'s
 * consumer either: that predicate's false result *destroys* the record
 * ($B44B branches to destruction on carry set), so widening it would change
 * projectile lifetime and world-record slot pressure -- gameplay, not
 * presentation.
 *
 * The area the 3D view draws but OAM cannot populate is instead shrouded, so
 * an empty extension never reads as missing actors. See the cloud shroud in
 * `src/present.c`. */

/* AR_SIMCAT=1 is a read-only ROM-research probe for the simulation rendering
 * catalogue.  The composition leaves are the one place where record identity,
 * behavior state, current visual frame, OAM allocation, and live OBJ registers
 * are all available together.  Log only identity changes so long deterministic
 * replays remain compact enough to diff and post-process. */
typedef struct SimCatalogSignature {
  uint16 composition;
  uint16 type;
  uint16 semantic_state;
  uint8 valid;
} SimCatalogSignature;

static int ws_sim_catalog_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_SIMCAT");
    enabled = e && e[0] && e[0] != '0';
  }
  return enabled;
}

static void ws_sim_catalog_record(CpuState *cpu, uint16 record,
                                  int world_record, uint16 composition,
                                  uint16 oam_before) {
  static SimCatalogSignature signatures[
      kActRaiserSimFixedRecordCount + kActRaiserSimWorldRecordCount];
  if (!ws_sim_catalog_enabled() ||
      !ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                  g_ram[kActRaiserWram_CurrentMap]))
    return;

  unsigned index;
  if (world_record) {
    index = kActRaiserSimFixedRecordCount +
        (unsigned)(record - kActRaiserWram_SimWorldRecords) /
            kActRaiserSimWorldRecordStride;
  } else {
    index = (unsigned)(record - kActRaiserWram_SimFixedRecords) /
        kActRaiserSimFixedRecordStride;
  }
  if (index >= sizeof(signatures) / sizeof(signatures[0])) return;

  SimCatalogSignature next = {0};
  next.composition = composition;
  next.type = cpu_read16(cpu, cpu->DB,
                         (uint16)(record + kSimRecord_Type));
  if (world_record) {
    next.semantic_state = (uint16)(
        cpu_read16(cpu, cpu->DB, (uint16)(record + kSimRecord_State)) &
        0x7FFF);
  }
  next.valid = 1;

  SimCatalogSignature *prior = &signatures[index];
  if (prior->valid && prior->composition == next.composition &&
      prior->type == next.type &&
      prior->semantic_state == next.semantic_state)
    return;
  *prior = next;

  unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  unsigned obj1 = g_ppu ? (unsigned)PPU_objTileAdr1(g_ppu) : 0;
  unsigned obj2 = g_ppu ? (unsigned)PPU_objTileAdr2(g_ppu) : 0;
  unsigned obsel = g_ppu ? g_ppu->obsel : 0;
  if (world_record) {
    uint16 raw_state = cpu_read16(
        cpu, cpu->DB, (uint16)(record + kSimRecord_State));
    uint16 status = cpu_read16(
        cpu, cpu->DB, (uint16)(record + kSimRecord_Status));
    fprintf(stderr,
            "[simcat] gf=%u town=%u tier=W idx=%u rec=%04X type=%02X "
            "state=%04X behavior=%04X script=%04X timer=%04X frame=%04X "
            "x=%04X y=%04X status=%04X flags=%04X "
            "f14=%04X f16=%04X f18=%04X vx=%04X vy=%04X "
            "f1e=%04X f20=%04X f22=%04X b24=%02X b25=%02X "
            "oam=%03X obsel=%02X obj1=%04X obj2=%04X\n",
            game_frame, g_ram[kActRaiserWram_CurrentMap],
            index - kActRaiserSimFixedRecordCount, record,
            next.type & 0xFF, raw_state,
            cpu_read16(cpu, cpu->DB,
                       (uint16)(record + kSimRecord_Behavior)),
            cpu_read16(cpu, cpu->DB,
                       (uint16)(record + kSimRecord_ScriptCursor)),
            cpu_read16(cpu, cpu->DB,
                       (uint16)(record + kSimRecord_FrameTimer)),
            composition,
            cpu_read16(cpu, cpu->DB,
                       (uint16)(record + kSimRecord_WorldX)),
            cpu_read16(cpu, cpu->DB,
                       (uint16)(record + kSimRecord_WorldY)),
            status,
            cpu_read16(cpu, cpu->DB,
                       (uint16)(record + kSimRecord_ActorFlags)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x14)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x16)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x18)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x1A)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x1C)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x1E)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x20)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x22)),
            cpu_read8(cpu, cpu->DB, (uint16)(record + 0x24)),
            cpu_read8(cpu, cpu->DB, (uint16)(record + 0x25)),
            oam_before, obsel, obj1, obj2);
  } else {
    uint16 status = cpu_read16(
        cpu, cpu->DB, (uint16)(record + kSimRecord_Status));
    fprintf(stderr,
            "[simcat] gf=%u town=%u tier=F idx=%u rec=%04X list=%04X "
            "timer=%04X script=%04X loop=%04X base=%04X frame=%04X "
            "x=%04X y=%04X status=%04X oam=%03X "
            "obsel=%02X obj1=%04X obj2=%04X\n",
            game_frame, g_ram[kActRaiserWram_CurrentMap], index, record,
            next.type,
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x00)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x02)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x04)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x06)),
            composition,
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x0A)),
            cpu_read16(cpu, cpu->DB, (uint16)(record + 0x0C)),
            status, oam_before, obsel, obj1, obj2);
  }
}

static int ws_sim_camera_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_SIM_CAMDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

static uint16 ws_sim_authentic_camera(uint16 target, uint16 center,
                                      uint16 maximum) {
  uint16 camera = (uint16)(target - center);
  if (camera & 0x8000)
    return 0;
  return camera >= maximum ? maximum : camera;
}

/* Faithful replacement for $01:B4C6, the camera writer called before the
 * town's behavior/OAM pass. Native horizontal follow is
 *
 *   cameraX = clamp($0AEE-$80, 0, $100)
 *
 * for a 512px world and 256px viewport. Earlier Wide Full code additionally
 * clamped this to [extra,$100-extra] and depended on synthesized margin columns
 * for the map edges. That made the left/right 43 columns unreachable at 16:9
 * whenever margin reconstruction was incomplete, while Wide Raw still exposed
 * the real edge. Preserve the ROM's 0..$100 range and let
 * PpuSetExtraSideSpace independently collapse the unavailable outer margin.
 * Vertical follow, both shake fields, their one-frame clear, final A=0,
 * caller P/DB, and the RTL stack contract remain authentic.
 *
 * RAW widescreen deliberately retains the native camera as a before/after
 * reference. AR_WS_SIM=0 and non-town $00 submodes do the same. */
RecompReturn ActRaiser_UpdateSimCamera(CpuState *cpu) {
  const uint8 saved_p = cpu->P;
  const uint8 saved_db = cpu->DB;
  const uint16 saved_x = cpu->X;
  const uint16 saved_y = cpu->Y;

  const uint16 target_x = cpu_read16(cpu, 0x01, 0x0AEE);
  const uint16 target_y = cpu_read16(cpu, 0x01, 0x0AF0);
  const uint16 native_x = ws_sim_authentic_camera(
      target_x, kSimCameraCenterX, kActRaiserTownCameraMaximumX);
  uint16 camera_x = native_x;
  uint16 camera_y = ws_sim_authentic_camera(
      target_y, kSimCameraCenterY, kActRaiserTownCameraMaximumY);

  const int wide = g_ws_active && g_settings.ws_sim &&
      g_settings.display_mode != kDisplayMode_43 &&
      g_settings.display_mode != kDisplayMode_WideRaw &&
      ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                 g_ram[kActRaiserWram_CurrentMap]);
  const uint16 left = 0;
  const uint16 right = kActRaiserTownCameraMaximumX;
  if (camera_x < left) camera_x = left;
  if (camera_x > right) camera_x = right;

  const uint16 shake_x = cpu_read16(cpu, 0x7F, 0x9F65);
  const uint16 shake_y = cpu_read16(cpu, 0x7F, 0x9F67);
  const uint16 shaken_x = (uint16)(camera_x + shake_x);
  const uint16 shaken_y = (uint16)(camera_y + shake_y);
  const int accept_x = !(shaken_x & 0x8000) &&
                       shaken_x >= left && shaken_x <= right;
  const int accept_y = !(shaken_y & 0x8000) &&
                       shaken_y <= kActRaiserTownCameraMaximumY;
  if (accept_x) camera_x = shaken_x;
  if (accept_y) camera_y = shaken_y;

  ws_dp16w(cpu, 0x22, camera_x);
  ws_dp16w(cpu, 0x24, camera_y);
  cpu_write16(cpu, 0x7F, 0x9F65, 0);
  cpu_write16(cpu, 0x7F, 0x9F67, 0);

  if (wide && ws_sim_camera_debug_enabled()) {
    static uint16 last_camera = 0xFFFF;
    static uint8 last_town = 0xFF;
    static int last_clamped = -1;
    const int clamped = camera_x != native_x;
    if (camera_x != last_camera ||
        g_ram[kActRaiserWram_CurrentMap] != last_town ||
        clamped != last_clamped) {
      fprintf(stderr,
              "[ws-sim-camera] gf=%u town=%u target=%u native=%u wide=%u "
              "bounds=%u-%u shake=%d/%d\n",
              (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
              (unsigned)g_ram[kActRaiserWram_CurrentMap],
              (unsigned)target_x, (unsigned)native_x, (unsigned)camera_x,
              (unsigned)left, (unsigned)right, accept_x, accept_y);
      last_camera = camera_x;
      last_town = g_ram[kActRaiserWram_CurrentMap];
      last_clamped = clamped;
    }
  }

  /* PLB/PLP restore the caller's DB/P; RTL consumes the three-byte JSL frame.
   * A is zero from the two shake-field clears. X/Y are untouched by the ROM. */
  cpu->A = 0;
  cpu->DB = saved_db;
  cpu->P = saved_p;
  cpu_p_to_mirrors(cpu);
  cpu->X = saved_x;
  cpu->Y = saved_y;
  if (cpu->x_flag) {
    cpu->X &= 0x00FF;
    cpu->Y &= 0x00FF;
  }
  cpu->S = (uint16)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}

static void ws_sim_set_nz16(CpuState *cpu, uint16 value) {
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & 0x8000) != 0;
  cpu->P = (uint8)((cpu->P & ~0x82) |
                   (cpu->_flag_N ? 0x80 : 0) |
                   (cpu->_flag_Z ? 0x02 : 0));
}

static uint16 ws_sim_adc16(CpuState *cpu, uint16 lhs, uint16 rhs) {
  const uint32 sum = (uint32)lhs + (uint32)rhs +
                     (uint32)(cpu->_flag_C ? 1 : 0);
  const uint16 result = (uint16)sum;
  cpu->_flag_C = sum > 0xFFFF;
  cpu->_flag_V = ((~(lhs ^ rhs) & (lhs ^ result) & 0x8000) != 0);
  ws_sim_set_nz16(cpu, result);
  cpu->P = (uint8)((cpu->P & ~0x41) |
                   (cpu->_flag_V ? 0x40 : 0) |
                   (cpu->_flag_C ? 0x01 : 0));
  return result;
}

static void ws_sim_cmp16(CpuState *cpu, uint16 lhs, uint16 rhs) {
  const uint16 result = (uint16)(lhs - rhs);
  cpu->_flag_C = lhs >= rhs;
  ws_sim_set_nz16(cpu, result);
  cpu->P = (uint8)((cpu->P & ~0x01) |
                   (cpu->_flag_C ? 0x01 : 0));
}

/* $01:B473 is the visibility/lifetime leaf used only by the angel's arrow
 * record $0B0A. The original state-2 update moves the arrow, calls this leaf,
 * and immediately destroys the record when carry returns set. That made the
 * arrow disappear at x=0/256 before the widened ADAD emitter could draw it.
 *
 * Keep the ROM's x+4 anchor, 512x512 hard bounds, 224px vertical viewport,
 * DP-$00 scratch writes, and carry contract. Only the horizontal camera
 * comparisons gain the finite live town margins. */
RecompReturn ActRaiser_SimProjectileVisible(CpuState *cpu) {
  const uint16 record = cpu->X;
  const uint16 camera_x = ws_dp16(cpu, kActRaiserWram_Bg1CameraX);
  const uint16 camera_y = ws_dp16(cpu, kActRaiserWram_Bg1CameraY);
  int margin_left = 0, margin_right = 0;
  ws_sim_live_margins(&margin_left, &margin_right);

  /* Preserve the original DP scratch value even though the widened upper
   * bound itself remains host-local. */
  cpu->_flag_C = 0;  /* CLC */
  cpu->P &= (uint8)~0x01;
  const uint16 authentic_right = ws_sim_adc16(
      cpu, camera_x, kActRaiserAuthenticWidth);
  ws_dp16w(cpu, 0x00, authentic_right);

  uint16 value = cpu_read16(
      cpu, cpu->DB, (uint16)(record + kSimRecord_WorldX));
  cpu->A = value;
  ws_sim_set_nz16(cpu, value);
  cpu->_flag_C = 0;  /* CLC */
  cpu->P &= (uint8)~0x01;
  value = ws_sim_adc16(cpu, value, kSimProjectileAnchorX);
  cpu->A = value;

  int culled = (value & 0x8000) != 0;
  if (!culled) {
    ws_sim_cmp16(cpu, value, kActRaiserTownWorldWidth);
    culled = cpu->_flag_C;
  }

  const uint16 wide_left = (uint16)(camera_x - margin_left);
  const uint16 wide_right = (uint16)(authentic_right + margin_right);
  if (!culled) {
    ws_sim_cmp16(cpu, value, wide_left);
    culled = !cpu->_flag_C;
  }
  if (!culled) {
    ws_sim_cmp16(cpu, value, wide_right);
    culled = cpu->_flag_C;
  }

  if (!culled) {
    cpu->A = camera_y;
    ws_sim_set_nz16(cpu, camera_y);
    cpu->_flag_C = 0;  /* CLC */
    cpu->P &= (uint8)~0x01;
    const uint16 vertical_bottom = ws_sim_adc16(
        cpu, camera_y, kSimVerticalViewportHeight);
    cpu->A = vertical_bottom;
    ws_dp16w(cpu, 0x00, vertical_bottom);

    value = cpu_read16(
        cpu, cpu->DB, (uint16)(record + kSimRecord_WorldY));
    cpu->A = value;
    ws_sim_set_nz16(cpu, value);
    if (value & 0x8000) {
      culled = 1;
    } else {
      ws_sim_cmp16(cpu, value, kActRaiserTownWorldWidth);
      if (cpu->_flag_C) {
        culled = 1;
      } else {
        ws_sim_cmp16(cpu, value, camera_y);
        if (!cpu->_flag_C) {
          culled = 1;
        } else {
          ws_sim_cmp16(cpu, value, vertical_bottom);
          culled = cpu->_flag_C;
        }
      }
    }
  }

  if (!culled && (margin_left || margin_right) &&
      ws_sim_sprite_debug_enabled()) {
    const uint16 x4 = (uint16)(
        cpu_read16(cpu, cpu->DB,
                   (uint16)(record + kSimRecord_WorldX)) +
        kSimProjectileAnchorX);
    if (x4 < camera_x || x4 >= authentic_right) {
      fprintf(stderr,
              "[ws-sim-projectile] gf=%u record=$%04X world=%u,%u "
              "camera=%u,%u margins=%d/%d\n",
              (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
              record,
              (unsigned)cpu_read16(cpu, cpu->DB,
                                   (uint16)(record + kSimRecord_WorldX)),
              (unsigned)cpu_read16(cpu, cpu->DB,
                                   (uint16)(record + kSimRecord_WorldY)),
              (unsigned)camera_x, (unsigned)camera_y,
              margin_left, margin_right);
    }
  }

  /* $B44B branches to destruction on carry set. SEC/CLC do not disturb the
   * final comparison's N/Z/V state. */
  cpu->_flag_C = culled ? 1 : 0;
  cpu->P = (uint8)((cpu->P & ~0x01) | (culled ? 0x01 : 0));
  cpu->S = (uint16)(cpu->S + 2);  /* replaced RTS */
  return RECOMP_RETURN_NORMAL;
}

static RecompReturn ws_sim_build_sprites(CpuState *cpu, int alternate_attr) {
  const uint16 record = cpu->X;
  const uint16 world_records_end = (uint16)(
      kActRaiserWram_SimWorldRecords +
      kActRaiserSimWorldRecordStride * kActRaiserSimWorldRecordCount);
  const int world_record = record >= kActRaiserWram_SimWorldRecords &&
                           record < world_records_end;
  int margin_left = 0, margin_right = 0;
  if (world_record)
    ws_sim_live_margins(&margin_left, &margin_right);

  const uint16 base_x = (uint16)(
      cpu_read16(cpu, cpu->DB, (uint16)(record + kSimRecord_WorldX)) -
      ws_dp16(cpu, 0x94));
  const uint16 base_y = (uint16)(
      cpu_read16(cpu, cpu->DB, (uint16)(record + kSimRecord_WorldY)) -
      ws_dp16(cpu, 0x96));
  ws_dp16w(cpu, 0x14, base_x);
  ws_dp16w(cpu, 0x16, base_y);

  uint16 part = cpu_read16(
      cpu, cpu->DB, (uint16)(record + kSimRecord_Composition));
  const uint16 oam_before = ws_dp16(cpu, 0x98);
  ws_sim_catalog_record(cpu, record, world_record, part, oam_before);
  SimRenderMetadata_BeginRecord(
      record, world_record != 0, alternate_attr != 0, part,
      cpu_read16(cpu, cpu->DB, (uint16)(record + kSimRecord_WorldX)),
      cpu_read16(cpu, cpu->DB, (uint16)(record + kSimRecord_WorldY)),
      cpu_read16(cpu, cpu->DB, (uint16)(record + kSimRecord_Type)),
      world_record
          ? (uint16)(cpu_read16(
                cpu, cpu->DB, (uint16)(record + kSimRecord_State)) & 0x7FFF)
          : 0,
      cpu_read16(cpu, cpu->DB, (uint16)(record + kSimRecord_Status)),
      oam_before);
  /* The biased origin the window predicate below is about to be applied to,
   * handed over rather than re-derived: see SimRenderMetadata_RecordAnchor. */
  SimRenderMetadata_RecordAnchor((int16_t)base_x, (int16_t)base_y);
  uint16 count = cpu_read8(cpu, cpu->DB, part);
  part = (uint16)(part + 1);
  ws_dp16w(cpu, 0x0E, count);

  uint16 oam = oam_before;
  uint16 final_a = 0;
  int final_c = 0;
  unsigned part_index = 0;

  do {
    const uint16 x_biased = (uint16)(
        base_x + ws_sim_part_offset(cpu_read8(
                     cpu, cpu->DB, (uint16)(part + 1))));
    const int authentic_x = x_biased < kSimOamBiasedWidth;
    const uint16 wide_x = (uint16)(x_biased + margin_left);
    const uint16 wide_bound = (uint16)(
        kSimOamBiasedWidth + margin_left + margin_right);

    if (wide_x < wide_bound) {
      const uint16 screen_x = (uint16)(x_biased - 0x0010);
      cpu_write8(cpu, cpu->DB, (uint16)(0x0380 + oam), (uint8)screen_x);

      uint16 highp = ws_dp16(cpu, 0x9A);
      uint8 mask = (uint8)ws_dp16(cpu, 0x9C);
      uint8 high = cpu_read8(cpu, cpu->DB, highp);
      if (screen_x & 0x0100)
        high |= mask;
      else
        high &= (uint8)~mask;
      if (cpu_read8(cpu, cpu->DB, part) & 0x01)
        high |= (uint8)(mask << 1);
      else
        high &= (uint8)~(mask << 1);
      cpu_write8(cpu, cpu->DB, highp, high);

      const uint16 y_biased = (uint16)(
          base_y + ws_sim_part_offset(cpu_read8(
                       cpu, cpu->DB, (uint16)(part + 2))));
      if (y_biased < kSimOamBiasedHeight) {
        const uint8 screen_y = (uint8)(y_biased - 0x0011);
        cpu_write8(cpu, cpu->DB, (uint16)(0x0381 + oam), screen_y);

        const uint16 raw_attr =
            cpu_read16(cpu, cpu->DB, (uint16)(part + 3));
        const uint16 attr = alternate_attr
            ? (uint16)((raw_attr & 0xF1FF) | 0x0600 | ws_dp16(cpu, 0x8F))
            : (uint16)(raw_attr | ws_dp16(cpu, 0x8F));
        cpu_write16(cpu, cpu->DB, (uint16)(0x0382 + oam), attr);
        SimRenderMetadata_RecordPart(oam, attr);
        final_a = attr;

        if (world_record && !authentic_x && ws_sim_sprite_debug_enabled()) {
          int sx = screen_x & 0x01FF;
          if (sx >= kActRaiserAuthenticWidth + margin_right)
            sx -= kActRaiserTownWorldWidth;
          fprintf(stderr,
                  "[ws-sim-sprite] gf=%u emitter=%s record=$%04X "
                  "part=%u oam=$%03X x=%d y=%u margins=%d/%d "
                  "tile=$%03X attr=$%04X\n",
                  (unsigned)ActRaiser_ReadWram16(
                      kActRaiserWram_GameFrame),
                  alternate_attr ? "AE6F" : "ADAD", record, part_index,
                  oam, sx, (unsigned)screen_y, margin_left, margin_right,
                  (unsigned)(attr & 0x01FF), attr);
        }

        oam = (uint16)(oam + 4);
        if (oam == kActRaiserOamLowTableBytes) {
          final_c = 1;  /* CPX #$0200 equality carry survives PLX. */
          break;
        }

        if (mask == 0x40) {
          ws_dp16w(cpu, 0x9A, (uint16)(highp + 1));
          ws_dp16w(cpu, 0x9C, 1);
        } else {
          ws_dp16w(cpu, 0x9C, (uint16)(mask << 2));
        }
      } else {
        /* The ROM has already touched x/high bits at this point, then parks
         * the unallocated low-table slot without advancing either cursor. */
        cpu_write16(cpu, cpu->DB, (uint16)(0x0380 + oam), 0xE000);
        SimRenderMetadata_RecordClippedPart(kSimClip_Vertical);
      }
    } else {
      cpu_write16(cpu, cpu->DB, (uint16)(0x0380 + oam), 0xE000);
      SimRenderMetadata_RecordClippedPart(kSimClip_Horizontal);
    }

    {
      uint16 next = (uint16)(part + 5);
      final_c = next < part;  /* C from the ROM's TYA;CLC;ADC #5. */
      part = next;
      final_a = part;
    }
    count = (uint16)(count - 1);
    ws_dp16w(cpu, 0x0E, count);
    part_index++;
  } while (count != 0);

  SimRenderMetadata_EndRecord(oam);
  ws_dp16w(cpu, 0x98, oam);
  cpu->A = final_a;
  cpu->X = record;  /* PLX */
  cpu->Y = part;
  cpu->_flag_C = final_c;
  cpu->_flag_Z = record == 0;
  cpu->_flag_N = (record & 0x8000) != 0;
  cpu->P = (uint8)((cpu->P & ~0x83) |
                   (cpu->_flag_N ? 0x80 : 0) |
                   (cpu->_flag_Z ? 0x02 : 0) |
                   (cpu->_flag_C ? 0x01 : 0));
  cpu->m_flag = 0;
  cpu->x_flag = 0;
  cpu->P &= (uint8)~0x30;

  /* Emulate the replaced RTS; the generated paired caller restores S. The
   * original PHX/PLX pair is net-neutral and represented by the saved record. */
  cpu->S = (uint16)(cpu->S + 2);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_BuildSimSprites(CpuState *cpu) {
  return ws_sim_build_sprites(cpu, 0);
}

RecompReturn ActRaiser_BuildSimSpritesAlt(CpuState *cpu) {
  return ws_sim_build_sprites(cpu, 1);
}
