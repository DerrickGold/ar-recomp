/* Included after the measured fixtures in action_effects_test.c. Expected
 * ownership and clocks are deliberately independent of the production registry.
 * Every registered family must supply a fixture here; a new row cannot silently
 * skip the parent-death / reuse / retirement contract. */
typedef struct LifecycleFixture {
  unsigned parent, child;
  bool spawn_proof, motion, attached;
} LifecycleFixture;

static bool SeedLifecycleFixture(uint8_t *ram, ActionSceneEffectFamily family,
                                 bool rematch, LifecycleFixture *fixture) {
  memset(ram, 0, kActRaiserWramSize);
  const unsigned parent = BossEffectSlot(49), child = BossEffectSlot(11);
  *fixture = (LifecycleFixture){.parent = parent, .child = child};
#define ROOM(group, map) do { ram[0x18] = group; ram[0x19] = map; } while (0)
  switch (family) {
    case kActionSceneFamily_BloodpoolBossFireball:
      SeedBloodpoolAct1BossFireball(ram);
      *fixture = (LifecycleFixture){0xCA0, 0xCE0, true, true, false};
      break;
    case kActionSceneFamily_BloodpoolFireball:
    case kActionSceneFamily_BloodpoolTrap:
      ROOM(2, 3);
      SeedMeasuredSceneObject(ram, 11, family == kActionSceneFamily_BloodpoolTrap);
      fixture->motion = family == kActionSceneFamily_BloodpoolFireball;
      break;
    case kActionSceneFamily_Centaur:
      ROOM(1, 1);
      SeedBossFamilyObject(ram, 11, 0xAD45, 0x63FB, 0x1D, 0xD, 0xAEC7, 0x20, parent);
      break;
    case kActionSceneFamily_NorthwallMagic:
      ROOM(6, 4);
      SeedBossFamilyObject(ram, 11, 0xE7C6, 0x5114, 3, 1, 0xE8BD, 0x20, parent);
      break;
    case kActionSceneFamily_MinotaurAxe:
    case kActionSceneFamily_IceDragonBall: {
      const bool axe = family == kActionSceneFamily_MinotaurAxe;
      ROOM(rematch ? 7 : axe ? 1 : 6, rematch ? (axe ? 2 : 7) : axe ? 4 : 8);
      const uint16_t source = axe ? (rematch ? 0xF6CA : 0xAF5D) : (rematch ? 0xF760 : 0xF161);
      SeedBossFamilyObject(ram, 49, source, 0x5300, 0, 0, 0, 0x4000, rematch ? 0x1C : 0);
      SeedBossFamilyObject(ram, 11, source, axe ? 0x50FB : 0x5D9C, axe ? 0 : 0x12,
                          axe ? 3 : 0x19, axe ? 0xB008 : 0xF2CA, 0x20, parent);
      fixture->spawn_proof = fixture->motion = true;
      break;
    }
    case kActionSceneFamily_FlamingWheelBody:
    case kActionSceneFamily_FlamingWheelShot: {
      ROOM(rematch ? 7 : 4, rematch ? 5 : 7);
      const uint16_t source = rematch ? 0xF712 : 0xD838;
      SeedBossFamilyObject(ram, 49, source, 0x5276, 5, 7, 0xD85E, 0x4000, rematch ? 0x1C : 0);
      if (family == kActionSceneFamily_FlamingWheelBody) {
        fixture->child = parent;
        fixture->parent = child;
      } else {
        SeedBossFamilyObject(ram, 11, source, 0x51B5, 0, 8, 0xA65D, 0x20, parent);
        Write16(ram, child + 6, 0xFFFF);
        Write16(ram, child + 8, 1);
        Write16(ram, child + 0x38, 8);
        Write16(ram, child + 0x28, 0x4000);
        fixture->spawn_proof = fixture->motion = true;
      }
      break;
    }
    case kActionSceneFamily_Tanzara:
      ROOM(7, 8);
      SeedBossFamilyObject(ram, 11, 0xF80F, 0x5D17, 0x16, 8, 0xFD77, 0x20, parent);
      break;
    case kActionSceneFamily_FillmoreOrb:
      ROOM(1, 3); /* Fillmore Act 2 room 2 red orbs. */
      for (unsigned at = child; at <= parent; at += parent - child) {
        Write16(ram, at + 0x16, 0x4000);
        ram[at + 0x18] = 0x7E;
        Write16(ram, at + 0x32, 0xB3BF);
      }
      Write16(ram, parent + 0x1A, 0x24);
      Write16(ram, parent + 0x20, 0x4A33);
      Write16(ram, child + 0x12, 0xB406);
      Write16(ram, child + 0x1A, 0xA);
      Write16(ram, child + 0x1E, 0xB3E9);
      Write16(ram, child + 0x3A, parent);
      Write16(ram, child + 0x22, 0x1B);
      Write16(ram, child + 0x20, 0x48F0);
      Write16(ram, child + 6, 2);
      fixture->spawn_proof = fixture->motion = true;
      break;
    case kActionSceneFamily_MarahnaOrb:
      ROOM(5, 7);
      SeedMarahnaFireball(ram, 11, 400, 500);
      SetMarahnaFireballOrbFrame(ram, 11, 7, 0x451C, 0); /* Authored hold. */
      break;
    case kActionSceneFamily_MarahnaSplit:
      ROOM(5, 7);
      SeedMarahnaFireball(ram, 49, 400, 500);
      Write16(ram, parent, 0x4000);
      Write16(ram, parent + 0x1A, 0xE);
      Write16(ram, parent + 0x1E, 0xE0A6);
      Write16(ram, parent + 0x20, 0x4597);
      Write16(ram, parent + 0x22, 0xC);
      SeedMarahnaSplitFireball(ram, 11, 49, 0, 3);
      fixture->spawn_proof = fixture->motion = true;
      break;
    case kActionSceneFamily_MarahnaSnake:
      ROOM(5, 6);
      SeedMarahnaSnakeFireballShot(ram, 49, 11, false, 0x1D, 0x4869);
      fixture->spawn_proof = fixture->motion = true;
      break;
    case kActionSceneFamily_MarahnaLink:
      ROOM(5, 6);
      SeedMarahnaLightningEndpoint(ram, 49, false, false, 400, 500);
      SeedMarahnaLightningEndpoint(ram, 50, true, false, 480, 500);
      SeedMarahnaLightningLink(ram, 11, 49, false, 440, 500);
      fixture->attached = true;
      break;
    case kActionSceneFamily_ViperBody:
    case kActionSceneFamily_ViperBolt:
    case kActionSceneFamily_ViperGround:
      ROOM(rematch ? 7 : 5, rematch ? 6 : 8);
      SeedMarahnaBossParent(ram, 49, 0, 7, 0x57C2);
      if (family == kActionSceneFamily_ViperBody) {
        fixture->child = parent;
        fixture->parent = child;
      } else {
        fixture->spawn_proof = fixture->motion = true;
        if (family == kActionSceneFamily_ViperBolt) {
          SeedMarahnaBossBolt(ram, 11, 49, false);
        } else {
          Write16(ram, parent + 0x12, 0x8683);
          Write16(ram, parent + 0x1A, 0xA);
          Write16(ram, parent + 0x1E, 0xE4D7);
          Write16(ram, parent + 0x20, 0x5307);
          Write16(ram, parent + 0x22, 0);
          SeedMarahnaBossGroundCharge(ram, 11, 49, false, 0x13);
        }
      }
      if (rematch) {
        Write16(ram, parent + 0x32, 0xF72A);
        Write16(ram, parent + 0x3A, 0x1C);
        Write16(ram, child + 0x32, 0xF72A);
      }
      break;
    case kActionSceneFamily_AitosLava:
      ROOM(4, 1);
      SeedAitosLavaFireball(ram, 11, 400, 500, 0x23); /* Intentional zero-speed hold. */
      break;
    case kActionSceneFamily_AitosStatue:
      ROOM(4, 6);
      SeedAitosStatueFire(ram, 11, 0xD5C0, 0x19, 0x1E, 0x4790);
      break;
    case kActionSceneFamily_AitosRock:
      ROOM(4, 1);
      SeedAitosMoltenRock(ram, 11, 400, 500, -2, 1);
      fixture->motion = true;
      break;
    case kActionSceneFamily_AitosSword:
      ROOM(4, 3);
      SeedAitosBossSwordVolley(ram, false);
      fixture->child = 0x1660;
      fixture->spawn_proof = fixture->motion = true;
      break;
    case kActionSceneFamily_PlayerSword:
      ROOM(2, 1);
      SeedSwordBeam(ram, 0x13, false);
      *fixture = (LifecycleFixture){0x8A0, BossEffectSlot(9), true, true, false};
      break;
    case kActionSceneFamily_WizardLightning:
      ROOM(rematch ? 7 : 2, rematch ? 3 : 8);
      SeedBloodpoolBoss(ram);
      SeedBloodpoolBossLightningStrike(ram, 11, 2, false);
      if (rematch) {
        Write16(ram, parent + 0x32, 0xF6E2);
        Write16(ram, child + 0x32, 0xF6E2);
      }
      fixture->spawn_proof = true;
      break;
    default: return false;
  }
#undef ROOM
  return true;
}

static const ActionEffectInstance *LifecycleEffect(
    const ActionSceneEffectFrame *frame, unsigned address) {
  for (unsigned i = 0; i < frame->effect_count; ++i)
    if (frame->effects[i].record_address == address) return &frame->effects[i];
  return NULL;
}

static void CheckLifecycleFixture(uint8_t *ram, ActionSceneEffectFamily family,
                                  const LifecycleFixture *fixture) {
  static uint8_t baseline[kActRaiserWramSize], unchanged[kActRaiserWramSize];
  static ActionSceneEffectFrame frame;
  static ActionEffectObserver observer;
  const unsigned child = fixture->child, parent = fixture->parent;
  const unsigned slot = (child - 0x6A0) / 0x40;
  memcpy(baseline, ram, sizeof(baseline));
  for (unsigned mutation = 0; mutation < 6; ++mutation) {
    memcpy(ram, baseline, sizeof(baseline));
    ActionEffectObserver_Reset(&observer);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
    const ActionEffectInstance *effect = LifecycleEffect(&frame, child);
    CHECK(effect != NULL);
    if (!effect) {
      fprintf(stderr, "Missing lifecycle fixture effect for family %u at $%04X\n", family, child);
      return;
    }
    CHECK(observer.scene_tracks[slot].scene_family == family);
    const uint32_t generation = effect->generation;
    const int16_t vx = effect->velocity_x, vy = effect->velocity_y;
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 0);
    effect = LifecycleEffect(&frame, child);
    CHECK(effect && effect->generation == generation && !effect->age_ticks);

    if (mutation == 0) Write16(ram, parent + 0x30, 0x0032);
    if (mutation == 1) Write16(ram, parent + 0x12, 0xA593);
    if (mutation == 2) Write16(ram, parent, 0x4000);
    if (mutation == 3) Write16(ram, parent + 0x20, 0);
    if (mutation == 4) Write16(ram, parent + 0x1A, 0xFFFF);
    if (mutation == 5) memset(ram + parent, 0, 0x40);
    CHECK(!memcmp(ram + child, baseline + child, 0x40));
    memcpy(unchanged, ram, sizeof(unchanged));
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 8);
    CHECK(!memcmp(ram, unchanged, sizeof(unchanged)));
    effect = LifecycleEffect(&frame, child);
    if (fixture->attached && mutation != 0) {
      CHECK(!effect);
      continue;
    }
    CHECK(effect && effect->generation == generation);
    if (!effect) continue;
    CHECK(effect->age_ticks == (fixture->motion ? 0 : 8));
    CHECK(effect->phase_ticks == effect->age_ticks);
    CHECK(effect->flags & kActionEffectFlag_Visible);
    if (fixture->motion) {
      Write16(ram, child + 6, 0);
      Write16(ram, child + 8, 0);
      ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 24);
      effect = LifecycleEffect(&frame, child);
      CHECK(effect && effect->generation == generation && !effect->age_ticks);
      CHECK(effect && effect->velocity_x == vx && effect->velocity_y == vy);
      Write16(ram, child + 6, (uint16_t)vx);
      Write16(ram, child + 8, (uint16_t)vy);
      Write16(ram, child + 2, (uint16_t)(baseline[child + 2] | baseline[child + 3] << 8) + 4);
      ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
      effect = LifecycleEffect(&frame, child);
      CHECK(effect && effect->generation == generation && effect->age_ticks == 1);
    }
    if (fixture->attached) continue;
    /* A child cannot transfer its admission to another backlink or teleport
     * into a new lifetime after its birth evidence has gone away. */
    if (mutation == 5 && fixture->spawn_proof) {
      Write16(ram, child + 2, 12000);
      ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
      CHECK(!LifecycleEffect(&frame, child));
      memcpy(ram + child, baseline + child, 0x40);
      Write16(ram, child + 0x3A, 0x6A0);
      ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
      CHECK(!LifecycleEffect(&frame, child));
      memcpy(ram + child, baseline + child, 0x40);
      ActionEffectObserver_Reset(&observer);
      ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
      CHECK(!LifecycleEffect(&frame, child)); /* Cold admission remains strict. */
    }
  }
  for (unsigned removal = 0; removal < 4; ++removal) {
    memcpy(ram, baseline, sizeof(baseline));
    ActionEffectObserver_Reset(&observer);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
    const uint32_t generation = observer.scene_tracks[slot].generation;
    if (removal == 0) Write16(ram, child, 0x4000);
    if (removal == 1) Write16(ram, child + 0x20, 0);
    if (removal == 2) Write16(ram, child + 0x16, 0xDEAD);
    if (removal == 3) ++ram[0x19]; /* Room transition retires all history. */
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
    const ActionEffectInstance *effect = LifecycleEffect(&frame, child);
    CHECK(!effect || (removal == 3 && effect->generation != generation));
    memcpy(ram, baseline, sizeof(baseline));
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
    effect = LifecycleEffect(&frame, child);
    CHECK(effect && effect->generation != generation && !effect->age_ticks);
  }
  /* Visibility is separate from identity: no-draw must suppress light without
   * inventing a new projectile when its sprite reappears. */
  memcpy(ram, baseline, sizeof(baseline));
  ActionEffectObserver_Reset(&observer);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
  const uint32_t generation = observer.scene_tracks[slot].generation;
  Write16(ram, child, 0x2000);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
  const ActionEffectInstance *hidden = LifecycleEffect(&frame, child);
  CHECK(!hidden || !(hidden->flags & kActionEffectFlag_Visible));
  Write16(ram, child, 0);
  ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(baseline), 1);
  CHECK(observer.scene_tracks[slot].generation == generation);
}

static void TestSceneLifecycleContracts(void) {
  static uint8_t ram[kActRaiserWramSize];
  for (unsigned family = 1; family < kActionSceneFamily_Count; ++family) {
    LifecycleFixture fixture;
    const bool seeded = SeedLifecycleFixture(ram, family, false, &fixture);
    CHECK(seeded); /* Adding a registry row without a fixture fails here. */
    if (seeded) CheckLifecycleFixture(ram, family, &fixture);
  }
  const ActionSceneEffectFamily rematches[] = {
    kActionSceneFamily_MinotaurAxe, kActionSceneFamily_IceDragonBall,
    kActionSceneFamily_FlamingWheelBody, kActionSceneFamily_FlamingWheelShot,
    kActionSceneFamily_ViperBody, kActionSceneFamily_ViperBolt,
    kActionSceneFamily_ViperGround, kActionSceneFamily_WizardLightning,
  };
  for (unsigned i = 0; i < sizeof(rematches) / sizeof(rematches[0]); ++i) {
    LifecycleFixture fixture;
    CHECK(SeedLifecycleFixture(ram, rematches[i], true, &fixture));
    CheckLifecycleFixture(ram, rematches[i], &fixture);
  }
  /* Wizard floor children own a different backlink than the original strike. */
  for (unsigned rematch = 0; rematch < 2; ++rematch) {
    LifecycleFixture fixture;
    SeedLifecycleFixture(ram, kActionSceneFamily_WizardLightning, rematch, &fixture);
    SeedBloodpoolBossLightningImpact(ram, 12, 9);
    Write16(ram, BossEffectSlot(12) + 0x3A, fixture.child);
    Write16(ram, BossEffectSlot(12) + 0x32, rematch ? 0xF6E2 : 0xBDFF);
    fixture.parent = fixture.child;
    fixture.child = BossEffectSlot(12);
    CheckLifecycleFixture(ram, kActionSceneFamily_WizardLightning, &fixture);
  }
}

static const ActionEffectActor *LifecycleActor(
    const ActionSceneEffectFrame *frame, unsigned address) {
  for (unsigned i = 0; i < frame->actor_count; ++i)
    if (frame->actors[i].address == address) return &frame->actors[i];
  return NULL;
}

static void TestAuthoredParentProvenance(void) {
  static uint8_t ram[kActRaiserWramSize], before[kActRaiserWramSize];
  static ActionEffectObserver observer;
  static ActionSceneEffectFrame frame;
  ActionEffectActorSelector selector = {0};
  CHECK(ActionEffectActorSelector_Set(&selector, "actor-target", "family"));
  CHECK(ActionEffectActorSelector_Set(&selector, "actor-source", "B786"));
  CHECK(ActionEffectActorSelector_Set(&selector, "actor-parent", "B786"));
  CHECK(ActionEffectActorSelector_Set(&selector, "actor-limit", "4"));
  CHECK(ActionEffectActorSelector_Valid(&selector));
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    SeedBloodpoolAct1BossFireball(ram);
    ActionEffectObserver_Reset(&observer);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    const unsigned parent = 0xCA0, child = 0xCE0;
    const ActionEffectActor *actor = LifecycleActor(&frame, child);
    CHECK(actor && ActionEffectActorSelector_Matches(&selector, actor));
    if (!actor) continue;
    const uint32_t generation = actor->generation;
    if (mutation == 0) Write16(ram, parent, 0x4000);
    if (mutation == 1) Write16(ram, parent + 0x32, 0xDEAD);
    if (mutation == 2) Write16(ram, parent + 0x20, 0);
    if (mutation == 3) memset(ram + parent, 0, 0x40);
    memcpy(before, ram, sizeof(ram));
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 8);
    CHECK(!memcmp(before, ram, sizeof(ram)));
    actor = LifecycleActor(&frame, child);
    CHECK(actor && actor->generation == generation && actor->age == 8);
    CHECK(actor && ActionEffectActorSelector_Matches(&selector, actor));
    Write16(ram, child + 0x3A, 0x6A0); /* New provenance cannot inherit the old selector. */
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    actor = LifecycleActor(&frame, child);
    CHECK(actor && actor->generation != generation && !actor->parent_source);
    CHECK(actor && !ActionEffectActorSelector_Matches(&selector, actor));
    /* Unknown birth provenance stays unknown even if that parent slot is
     * subsequently occupied by a matching source. */
    Write16(ram, 0x6A0 + 0x32, 0xB786);
    ActionSceneEffects_CaptureFrame(&observer, &frame, ram, sizeof(ram), 1);
    actor = LifecycleActor(&frame, child);
    CHECK(actor && !actor->parent_source);
  }
}
