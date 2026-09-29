/* ActRaiser cheats and extras: the player and developer cheats
 * (ActRaiser_ApplyCheats), the magic cycle, and level warp.
 * Phase: game (main thread). */
#include "actraiser/actraiser_rtl_internal.h"

/* Reload the selector-dependent part of the action OBJ atlas after a live
 * magic selection change. The native level-entry loader $02:BC9E copies 128
 * words from $06:A400 + (selector-1)*$80 to VRAM $2D40. Merely changing
 * $02AC during an action stage would therefore run the new spell with the old
 * spell's resident tiles and produce a misleading graphics failure. This
 * targeted host copy reproduces only that selector-dependent upload; the
 * common atlas and palettes remain untouched. */
static void ActRaiser_ReloadSelectedMagicTiles(uint8 selector) {
  if (selector < 1 || selector > 4) return;
  SrPpuStateSnapshot ppu;
  SrBorrowedU16Span vram = {.struct_size = sizeof(vram)};
  SrPpuVramWordPatch patches[0x80];
  if (!ActRaiser_QueryPpuState(&ppu) || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size < SNES_RUNNER_API_PPU_VRAM_PATCH_SIZE ||
      !ActRaiser_RunnerApi()->borrow_u16_memory ||
      !ActRaiser_RunnerApi()->compare_exchange_ppu_vram_words ||
      ActRaiser_RunnerApi()->borrow_u16_memory(
          ActRaiser_Runner(), SR_MEMORY_VRAM, &vram) != SR_RESULT_OK)
    return;
  uint16 source = (uint16)(0xA400 + (selector - 1) * 0x80);
  for (uint16 word = 0; word < 0x80; word++) {
    const uint16_t address = (uint16_t)(0x2D40 + word);
    patches[word] = (SrPpuVramWordPatch) {
      .word_address = address,
      .expected = vram.data[address],
      .replacement =
          cpu_read16(&g_cpu, 0x06, (uint16)(source + word * 2)),
    };
  }
  const SrPpuVramPatchRequest request = {
    .struct_size = sizeof(request),
    .flags = SR_PPU_VRAM_PATCH_ADDRESSES_SORTED,
    .lifetime_generation = ppu.lifetime_generation,
    .patches = patches,
    .patch_count = 0x80u,
  };
  (void)ActRaiser_RunnerApi()->compare_exchange_ppu_vram_words(ActRaiser_Runner(), &request);
}

static const char *const kActRaiserMagicNames[] = {
  "none", "Magical Fire", "Magical Stardust", "Magical Aura", "Magical Light"
};

uint8 ActRaiser_SelectedMagic(void) {
  uint8 selected = g_ram[kActRaiserWram_SelectedMagic];
  return selected <= 4 ? selected : 0;
}

/* Set by the host input thread's edge dispatch, consumed once per frame by
 * ActRaiser_ApplyMagicCycle. Both sides run on the main thread (the present
 * thread is the one that was split out), so a plain flag is sufficient and a
 * held key cannot queue a burst: it is cleared unconditionally on read. */
static bool s_magic_cycle_requested;

void ActRaiser_RequestMagicCycle(void) { s_magic_cycle_requested = true; }

/* Which spells the save actually owns. The four inventory bytes $0299-$029C
 * hold spell ids (1..4) or 0 for an empty slot, and they are NOT sorted or
 * positional — All magic writes 1/2/3/4 in order, but a real save fills them
 * in pickup order. Collect the distinct ids so the cycle visits each unlocked
 * spell exactly once, in canonical Fire/Stardust/Aura/Light order rather than
 * in whatever order the player happened to find them. */
static unsigned ActRaiser_UnlockedMagic(uint8 out[4]) {
  bool present[5] = { false, false, false, false, false };
  for (unsigned slot = 0; slot < 4; slot++) {
    uint8 id = g_ram[kActRaiserWram_MagicInventory + slot];
    if (id >= 1 && id <= 4) present[id] = true;
  }
  unsigned count = 0;
  for (uint8 id = 1; id <= 4; id++)
    if (present[id]) out[count++] = id;
  return count;
}

/* Debug aid: step the action-stage spell selection to the next spell the save
 * has unlocked. Armed by the "Cycle magic spell" cheat and triggered by the
 * kInputAction_MagicCycle binding (keyboard or pad) — it no longer reserves a
 * SNES button, so it cannot shadow L in normal play. */
static void ActRaiser_ApplyMagicCycle(void) {
  bool requested = s_magic_cycle_requested;
  s_magic_cycle_requested = false;

  if (!requested || !g_settings.cheat_magic_cycle) return;

  /* Action stages only: $02AC and the $2D40 tile window are act-mode state,
   * and the sim-mode equip menu owns the selection there. Gated here rather
   * than at the call site so a press made in town is dropped outright instead
   * of firing the moment the next act loads. */
  if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
    fprintf(stderr, "[magic-cycle] not in an action stage; ignored\n");
    return;
  }

  /* $00F8 is the act-mode cast state. Rewriting $02AC mid-cast would leave
   * the in-flight spell's actors running against the new spell's tiles. */
  if (g_ram[kActRaiserWram_MagicCastState] != 0) {
    fprintf(stderr, "[magic-cycle] cast still active; selection unchanged\n");
    return;
  }

  uint8 unlocked[4];
  unsigned count = ActRaiser_UnlockedMagic(unlocked);
  if (!count) {
    fprintf(stderr, "[magic-cycle] no spells unlocked; selection unchanged "
            "(enable the All magic cheat to test every spell)\n");
    return;
  }

  /* Advance past the current selection, wrapping. An unknown or unowned
   * current value (including 0 / "none") starts the cycle at the first
   * unlocked spell rather than being treated as an error. */
  uint8 current = g_ram[kActRaiserWram_SelectedMagic];
  unsigned index = 0;
  for (unsigned i = 0; i < count; i++) {
    if (unlocked[i] == current) { index = (i + 1) % count; break; }
  }
  uint8 next = unlocked[index];

  g_ram[kActRaiserWram_SelectedMagic] = next;
  ActRaiser_ReloadSelectedMagicTiles(next);
  fprintf(stderr, "[magic-cycle] selected %s (%u of %u unlocked, $02AC=$%02X, "
          "VRAM $2D40 refreshed)\n", kActRaiserMagicNames[next],
          index + 1, count, next);
}

/* Host-side cheat hooks (debug-menu scaffold). All settings-gated and seeded
 * OFF from their legacy env names, so they never affect a normal run. Applied
 * once per frame at the START of RunOneFrameOfGame (before the game's frame
 * logic), so a value pinned here is what the frame sees -> effective for death
 * prevention (HP) and physics override (moonjump). RAM is g_ram (WRAM): low
 * direct-page addrs map 1:1 ($1D player HP, $E6/$E7 timer, player object $08A0).
 * This is the framework the planned debug menu plugs into — see
 * docs/SEAMS.md "Gameplay / Tunable seams" + memory debug-menu-warp-roadmap. */
enum {
  kPackedBcdDigitRadix = 10,
  kPackedBcdPairPlaceValue = kPackedBcdDigitRadix * kPackedBcdDigitRadix,
};

static int ActRaiser_BcdTimerToSeconds(uint8 low, uint8 high) {
  int low_pair =
      (low & 0x0F) + ((low >> 4) & 0x0F) * kPackedBcdDigitRadix;
  int high_pair =
      (high & 0x0F) + ((high >> 4) & 0x0F) * kPackedBcdDigitRadix;
  return low_pair + high_pair * kPackedBcdPairPlaceValue;
}

void ActRaiser_ApplyCheats(void) {

  /* AR_PIN=<parcode>[,<parcode>...] — generic PAR/ZSNES cheat-code pinner
   * (2026-07-06). Each code is the standard 8-hex-digit PAR form BBAAAAVV
   * (bank $7E/$7F, 16-bit addr, byte value), applied every frame in EVERY
   * mode (unlike the mode-gated hand cheats below). Turns the whole
   * ./codes.txt catalogue (flamingspinach's 88 engineered codes — see
   * docs/ram-map.md "Cheat-derived WRAM map") into ready-made debug cheats
   * AND address-mapping probes with zero per-cheat C. Example:
   *   AR_PIN=7E00210A,7E029901   (INF MP + HAVE FIRE — the §7.18 kit)
   * Bad tokens are reported once and skipped. Max 32 pins. */
  for (int i = 0; i < g_settings.pin_count; i++)
    g_ram[g_settings.pins[i].off] = g_settings.pins[i].val;

  /* Action-stage gameplay tweaks only. MapGroup $01-$06 selects an ordinary
   * action region, $07 is Death Heim, $00 is the non-action town/world/UI
   * engine, and $08 is the ending presenter. Gate on the whole action range so
   * cheats persist across every region, not just Fillmore ($18==$01) — that
   * bug disabled them after warping to region 2+. The player object/HP/timer
   * fields are shared by the action engine across every region, so the same
   * writes apply everywhere. */
  /* ── ALL-MODE cheats (above the action-stage gate: they feed the sim-mode
   * equip menu / angel, so they must pin in every mode) ─────────────────── */

  /* AR_ALL_MAGIC=1: unlock all four spells. HAVE flags $0299-$029C = 01/02/03/04
   * (cheat-map values, docs/ram-map.md). Pinned in ALL modes so the sim-mode
   * equip menu lists them; SELECTING one still goes through the menu (the equip
   * routine $01:915D derives $02AC from these). docs/SEAMS.md has the full
   * magic wiring map and cast-gate interaction. */
  {
    if (g_settings.cheat_all_magic) {
      g_ram[kActRaiserWram_MagicInventory + 0] = 0x01; /* Magical Fire */
      g_ram[kActRaiserWram_MagicInventory + 1] = 0x02; /* Magical Stardust */
      g_ram[kActRaiserWram_MagicInventory + 2] = 0x03; /* Magical Aura */
      g_ram[kActRaiserWram_MagicInventory + 3] = 0x04; /* Magical Light */
    }
  }

  /* AR_RANGED_SWORD=1: retain the native sword power-up ($E4 = $80,
   * PAR 7E00E480). This also makes the melee sword deal 2 instead of 1
   * through $9DC8. The native beam deals 2 per accepted hit; it survives
   * contact and can hit again after the victim's hit timer expires. */
  if (g_settings.cheat_ranged_sword)
    g_ram[kActRaiserWram_RangedSwordFlag] = 0x80;

  /* AR_INF_MP: infinite magic scrolls. =1 -> pin the WORKING count $21 to 10
   * (PAR 7E00210A); =<n> -> pin to n. Deliberately does NOT touch the
   * PERSISTENT count $0295; $21 is the act-mode working copy loaded from
   * $0295 at $02:84E0, so the cheat never bakes into save.srm. */
  if (g_settings.cheat_inf_mp)
    g_ram[kActRaiserWram_WorkingMagicPoints] =
        (uint8)g_settings.cheat_inf_mp;

  /* AR_INF_SP=1: infinite sim-mode SP (miracle points). Self-calibrating: pins
   * current SP $0282/16 to max SP $0284/16 once max is known (vs the PAR code's
   * blunt $FF, which over-fills early-game maxima). */
  {
    if (g_settings.cheat_inf_sp &&
        ActRaiser_ReadWram16(kActRaiserWram_AngelMaximumSp)) {
      ActRaiser_WriteWram16(
          kActRaiserWram_AngelCurrentSp,
          ActRaiser_ReadWram16(kActRaiserWram_AngelMaximumSp));
    }
  }

  /* AR_ANGEL_HP=1: infinite sim-mode angel health. Self-calibrating: pins
   * current HP $0286 to max HP $0287 (the PAR code 7E028608 hardcodes 8, which
   * would UNDER-fill after level-ups raise the max). */
  if (g_settings.cheat_angel_hp &&
      g_ram[kActRaiserWram_AngelMaximumHp])
    g_ram[kActRaiserWram_AngelCurrentHp] =
        g_ram[kActRaiserWram_AngelMaximumHp];

  /* Live action-stage magic selection/asset reload for effect testing. Above
   * the action-stage gate so a request made outside an act is consumed and
   * discarded rather than queued until the next act loads; the handler
   * applies the gate itself. */
  ActRaiser_ApplyMagicCycle();

  if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) return;

  /* AR_INF_HP: infinite health. =1 -> auto: pin player HP ($1D) to the
   * high-water max seen this stage (self-calibrates to "full" once you've been
   * at full, so we needn't know max HP statically). =<n> -> pin to literal n. */
  {
    static int previous_cheat_mode;
    static unsigned highest_hp;
    int cheat_mode = g_settings.cheat_inf_hp;
    if (cheat_mode != previous_cheat_mode) {
      /* Entering/re-entering auto mode must calibrate from the current stage,
       * not reuse a high-water value captured before a live toggle. */
      if (cheat_mode == 1) highest_hp = 0;
      previous_cheat_mode = cheat_mode;
    }
    if (cheat_mode) {
      if (cheat_mode > 1) {
        g_ram[kActRaiserWram_PlayerHp] = (uint8)cheat_mode;
      } else {
        unsigned current_hp = g_ram[kActRaiserWram_PlayerHp];
        if (current_hp > highest_hp && current_hp <= 0xFF)
          highest_hp = current_hp;
        if (highest_hp)
          g_ram[kActRaiserWram_PlayerHp] = (uint8)highest_hp;
      }
    }
  }

  /* AR_FREEZE_TIMER=1: pin the action-stage timer ($E6/$E7, BCD) to its first
   * captured value -> infinite time.
   *
   * Backs off automatically once the boss-defeat point-tally sequence starts
   * draining the timer: normal countdown never drops the BCD value by more
   * than 1 (roughly once per real-time second), so any single-frame drop
   * bigger than that can only be the drain script deliberately driving the
   * timer down, not the stage clock. When that happens, stop re-pinning and
   * let the game own the timer for the rest of THIS stage -- otherwise the
   * frozen timer blocks the drain and the boss->sim transition never
   * completes. No separate "boss defeated" flag needed; the abnormal
   * decrement rate IS the signal (2026-07-01). Latch resets on region change
  * ($18) so a fresh stage re-arms the freeze instead of staying stuck off
  * from a previous boss fight. */
  {
    static uint8 captured_timer_low;
    static uint8 captured_timer_high;
    static int timer_captured;
    static int drain_detected;
    static uint8 last_map_group = kActRaiserUnknownMapGroup;
    static int cheat_was_enabled;
    if (!g_settings.cheat_freeze_timer) {
      /* A future live off/on toggle starts a fresh capture and drain latch. */
      cheat_was_enabled = 0;
      timer_captured = 0;
      drain_detected = 0;
      last_map_group = kActRaiserUnknownMapGroup;
    } else {
      if (!cheat_was_enabled) {
        cheat_was_enabled = 1;
        timer_captured = 0;
        drain_detected = 0;
        last_map_group = kActRaiserUnknownMapGroup;
      }
      if (g_ram[kActRaiserWram_MapGroup] != last_map_group) {
        last_map_group = g_ram[kActRaiserWram_MapGroup];
        timer_captured = 0;
        drain_detected = 0;
      }
      if (!timer_captured) {
        captured_timer_low = g_ram[kActRaiserWram_ActionTimerLow];
        captured_timer_high = g_ram[kActRaiserWram_ActionTimerHigh];
        timer_captured = 1;
      }
      if (!drain_detected) {
        int captured_seconds = ActRaiser_BcdTimerToSeconds(
            captured_timer_low, captured_timer_high);
        int current_seconds = ActRaiser_BcdTimerToSeconds(
            g_ram[kActRaiserWram_ActionTimerLow],
            g_ram[kActRaiserWram_ActionTimerHigh]);
        if (captured_seconds - current_seconds > 1) {
          drain_detected = 1;
        } else {
          g_ram[kActRaiserWram_ActionTimerLow] = captured_timer_low;
          g_ram[kActRaiserWram_ActionTimerHigh] = captured_timer_high;
        }
      }
    }
  }

  /* AR_MOONJUMP=1: hold the game's jump button (SNES B) to FLY UP. We move the player
   * Y-POSITION ($08A4, +$04) directly rather than the Y-velocity ($08A8, +$08) —
   * object fields are polymorphic by state, so $08A8 is "Y-velocity" only in the
   * AIR state; while grounded it means something else (writing it there didn't
   * launch and leaked into other movement). Position is always position, so
   * decrementing $08A4 (screen-Y grows downward, so −Y = up) is a reliable
   * state-independent fly. AR_MOONJUMP_SPEED sets up-speed in pixels/frame
   * (default 6). The trigger follows the game's fixed jump mapping instead of
   * exposing a second, potentially contradictory cheat binding. */
  {
    enum { kActRaiserJoypadJump = 0x8000 };  /* auto-joypad SNES B bit */
    if (g_settings.cheat_moonjump) {
      SrInputStateSnapshot input;
      uint16 buttons = ActRaiser_QueryInputState(&input)
          ? input.auto_joypad[0] : 0u;
      if (buttons & kActRaiserJoypadJump) {
        uint16 player_y =
            ActRaiser_ReadWram16(kActRaiserWram_PlayerPositionY);
        player_y = (uint16)(player_y - g_settings.cheat_moonjump_speed);
        ActRaiser_WriteWram16(kActRaiserWram_PlayerPositionY, player_y);
      }
    }
  }

  /* AR_NO_KNOCKBACK=1: permanent invuln -> no hit registers -> no damage, no
   * knockback, no hitstun (speedrun "ignore hits"), using the game's own
   * i-frames. The hit-check gates on the INVULN FLAG ($08D0 bit 0x2000, +$30),
   * which the game sets on a hit and clears when the i-frame TIMER ($08C6, +$26)
   * counts down to 0. This authentic invulnerability state also suppresses
   * water drag; disable the cheat for movement/terrain-physics validation.
   * So we (a) pin the timer to 0xFF so the game never clears
   * the flag, and (b) SET the flag ourselves each frame so invuln is active from
   * frame one (without needing a first hit to bootstrap it — that was the
   * "works only after getting hit once" gap). Offsets found via AR_WATCHOBJ=08A0
   * while taking a hit. AR_NO_KNOCKBACK=<hexoff> (other than 1) instead raw-pins
   * $08A0+off to 0xFF for experimentation. */
  {
    static int prior_mode;
    int mode = g_settings.cheat_no_knockback;
    if (prior_mode == 1 && mode != 1) {
      /* Release only the two fields owned by full-invulnerability mode. Raw
       * experimental offset pins are intentionally not guessed/restored. */
      g_ram[kActRaiserWram_PlayerInvulnerabilityTimer] = 0;
      g_ram[kActRaiserWram_PlayerFlags + 1] &=
          (uint8)~kActRaiserPlayerFlag_InvulnerableHighByte;
    }
    prior_mode = mode;
    if (mode) {
      if (mode == 1) {
        g_ram[kActRaiserWram_PlayerInvulnerabilityTimer] = 0xFF;
        /* MAGIC EXCEPTION: the cast gate ($00:9843 ->
         * $00:9DE1) does BIT #$2008 on player state $08D0 and refuses to cast
         * while the invuln flag ($2000) is set -- pinning it unconditionally made
         * magic permanently dead. Lift the pin ONLY when a cast will actually
         * fire this frame, i.e. when every condition of the game's own gate
         * holds: cast button held ($00A0 & $C0 -- the NMI joypad shadow of
         * $4218&$F4, bit7=A bit6=X, level-sensitive so the 1-frame NMI lag is
         * fine), no cast in progress ($F8==0), magic equipped ($02AC!=0), and
         * MP available ($21>0). The instant the cast starts the game sets $F8
         * nonzero -> the pin snaps back ON for the whole cast animation.
         * Residual vulnerability: only the 1-2 frames between press and cast
         * start; holding the button with no magic/MP no longer drops invuln
         * at all (the old version was vulnerable the entire time the button
         * was held). */
        int cast_imminent =
            (g_ram[kActRaiserWram_InputHeldHigh] & 0xC0) != 0 &&
            g_ram[kActRaiserWram_MagicCastState] == 0 &&
            g_ram[kActRaiserWram_SelectedMagic] != 0 &&
            g_ram[kActRaiserWram_WorkingMagicPoints] != 0;
        if (cast_imminent)
          g_ram[kActRaiserWram_PlayerFlags + 1] &=
              (uint8)~kActRaiserPlayerFlag_InvulnerableHighByte;
        else
          g_ram[kActRaiserWram_PlayerFlags + 1] |=
              kActRaiserPlayerFlag_InvulnerableHighByte;
      } else {
        g_ram[kActRaiserWram_PlayerObject + (unsigned)mode] = 0xFF;
      }
    }
  }
}

/* Level-warp: stage the game's OWN sim->act transition to a chosen region/map,
 * bypassing the (broken) sim-mode UI. The intro/overworld stages an act entry by
 * writing the transition-DEST vars + a request flag, which the transition
 * processor then consumes (full fade + level-load + mode switch). Observed entry
 * into Fillmore act 1: $1B=01 (-> $18 region), $1A=01 (-> $19 act), $FB|=0x80
 * (request). We replicate that. Best triggered from a transition-capable state
 * (the intro, $18==00, which WORKS — unlike the post-act sim cascade). Hooked to
 * the Warp action in runtime_settings.c, using AR_WARP=<region_hex><map_hex>. The map
 * byte is written directly to $19 and is not a uniform act number. */
void ActRaiser_Warp(unsigned region, unsigned map) {
  uint8 source_map_group = g_ram[kActRaiserWram_MapGroup];
  uint8 source_map = g_ram[kActRaiserWram_CurrentMap];
  g_ram[kActRaiserWram_DestinationMapGroup] = (uint8)region;
  g_ram[kActRaiserWram_DestinationMap] = (uint8)map;
  g_ram[kActRaiserWram_TransitionRequest] |= kActRaiserTransitionRequestBit;
  fprintf(stderr, "[warp] from $18=$%02X $19=$%02X staged region=$%02X "
          "map=$%02X ($1B/$1A/$FB set); transition processor should pick it up.\n",
          source_map_group, source_map, region & 0xFF, map & 0xFF);
  if (ActRaiser_IsActionMapGroup(source_map_group)) {
    fprintf(stderr, "[warp] WARNING: action->action is not a naturally observed "
            "transition; inherited timing/object state may affect fidelity.\n");
  }
}
