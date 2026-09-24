#include "regional/regional_profiles.h"
#include "regional/session/regional_session.h"
#include "regional/session/regional_session_action.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void TestActionRoomActivation(const ArRegionalSession *baseline) {
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    ArRegionalSession batch = *baseline;
    assert(ArRegionalSession_SetPopulationProfile(&batch, batch.revision, source));
    ArRegionalRules rules;
    assert(ArRegionalProfiles_Expand(&batch.requested, kArRegionalProfile_Gameplay, source, &rules));
    assert(ArRegionalProfiles_Expand(&rules, kArRegionalProfile_Presentation, source, &rules));
    rules.difficulty.level = kArRegionalDifficulty_Expert;
    assert(ArRegionalSession_RequestRules(&batch, batch.revision, &rules));
    ArRegionalSession serial = batch;
    ArRegionalActionRoomSnapshot expected = {0}, actual = {0};
    ArRegionalTimerPolicy timers;
    uint16_t expected_time, actual_time;
    assert(ArRegionalTimers_Resolve(&serial.requested.timers, 0, 0x0300, &expected_time));
    assert(ArRegionalSession_BeginTimers(&serial, &timers));
#define ACTIVATE(family, field) assert(ArRegionalSession_Begin##family(&serial, &expected.field))
    ACTIVATE(ActionMotion, motion);
    ACTIVATE(Emitters, emitters);
    ACTIVATE(Volley, statue_volley);
    ACTIVATE(Bosses, bosses);
    ACTIVATE(Difficulty, difficulty);
    ACTIVATE(Hazards, hazards);
    ACTIVATE(Terrain, terrain);
    ACTIVATE(Mosaic, mosaic);
    ACTIVATE(Poses, poses);
    ACTIVATE(Artwork, artwork);
    ACTIVATE(Placements, placements);
    ACTIVATE(ScoreLives, score_lives);
    ACTIVATE(Collision, collision);
    ACTIVATE(PlatformSkull, platform_skull);
    ACTIVATE(ActorStats, actor_stats);
    ACTIVATE(CastHold, cast_hold);
    ACTIVATE(Fire, fire_enemy);
#undef ACTIVATE
    expected.placement_difficulty = serial.effective.difficulty.level;
    assert(ArRegionalSession_BeginActionRoom(&batch, 0, 0x0300, &actual_time, &actual));
    assert(actual_time == expected_time);
    assert(!memcmp(&expected, &actual, sizeof(actual)));
    assert(!memcmp(&serial, &batch, sizeof(batch))); // Includes exact revision increments.
    if (source == kArRegionalSource_US) continue;

    // Allow one primitive to succeed, then exhaust revision space. No portion
    // of that failed room transaction may reach either session or outputs.
    batch = *baseline;
    assert(ArRegionalSession_SetPopulationProfile(&batch, batch.revision, source));
    assert(ArRegionalSession_RequestRules(&batch, batch.revision, &rules));
    batch.revision = UINT32_MAX - 1;
    const ArRegionalSession before = batch;
    const ArRegionalActionRoomSnapshot before_snapshot = actual;
    actual_time = 0xbeef;
    assert(!ArRegionalSession_BeginActionRoom(&batch, 0, 0x0300, &actual_time, &actual));
    assert(!memcmp(&batch, &before, sizeof(batch)) && actual_time == 0xbeef);
    assert(!memcmp(&actual, &before_snapshot, sizeof(actual)));
  }
}

void TestRegionalProfiles(void) {
  ArRegionalRules baseline={0},all,changed;
  ArRegionalProfileSummary summary[kArRegionalProfile_Count];
  assert(ArRegionalProfiles_Describe(&baseline,summary));
  for(unsigned i=0;i<kArRegionalProfile_Count;++i)
    assert(summary[i].members && summary[i].source==kArRegionalSource_US && (summary[i].matching_sources&1));
  assert(!ArRegionalProfiles_Mask(kArRegionalProfile_Count));
  assert(!*ArRegionalProfiles_Key(kArRegionalProfile_Count));
  assert((ArRegionalProfiles_Mask(kArRegionalProfile_Gameplay)|ArRegionalProfiles_Mask(kArRegionalProfile_Presentation))==((1u<<kArRegionalProfile_GroupCount)-1));
  assert(!(ArRegionalProfiles_Mask(kArRegionalProfile_Gameplay)&ArRegionalProfiles_Mask(kArRegionalProfile_Presentation)));
  for(unsigned source=0;source<kArRegionalSource_Count;++source) {
    assert(ArRegionalProfiles_Expand(&baseline,kArRegionalProfile_Gameplay,source,&all));
    assert(ArRegionalProfiles_Expand(&all,kArRegionalProfile_Presentation,source,&all));
    assert(ArRegionalRules_PopulationCompatible(&all));
    assert(ArRegionalProfiles_Describe(&all,summary));
    for(unsigned i=0;i<kArRegionalProfile_Count;++i)assert(summary[i].source==source);
    /* The value type currently consists solely of source enums and one
     * difficulty enum. Check every stored word to catch an omitted field in
     * the typed inventory. This is a test of that layout, not runtime
     * reflection, a storage format, or a way of mutating policies. */
    _Static_assert(sizeof(ArRegionalDifficulty)==sizeof(ArRegionalSource),"rules coverage layout");
    _Static_assert(sizeof(ArRegionalRules)%sizeof(ArRegionalSource)==0,"rules coverage size");
    const size_t difficulty=offsetof(ArRegionalRules,difficulty)+offsetof(ArRegionalDifficultyPolicy,level);
    for(size_t at=0;at<sizeof(all);at+=sizeof(ArRegionalSource)) {
      ArRegionalSource value;memcpy(&value,(const unsigned char *)&all+at,sizeof(value));
      assert(value==(at==difficulty?kArRegionalSource_US:(ArRegionalSource)source));
    }
    for(unsigned group=0;group<kArRegionalProfile_GroupCount;++group) {
      assert(ArRegionalProfiles_Expand(&baseline,group,source,&changed));
      assert(ArRegionalProfiles_Describe(&changed,summary));
      for(unsigned i=0;i<kArRegionalProfile_GroupCount;++i)
        assert(summary[i].source==(i==group?(ArRegionalSource)source:kArRegionalSource_US));
      uint16_t delta;
      assert(ArRegionalProfiles_Changes(&baseline,&changed,&delta));
      assert(!(delta&~(1u<<group)));
      assert(ArRegionalProfiles_Expand(&changed,group,0,&changed));
      assert(!memcmp(&baseline,&changed,sizeof(changed)));
    }
  }
  // Source provenance survives uniform selections; equivalent mixed sources
  // derive a real preset instead of inventing Custom or changing the rules.
  changed=baseline;changed.actor_artwork.source[0]=kArRegionalSource_Europe;
  assert(ArRegionalProfiles_Describe(&changed,summary));
  assert(summary[kArRegionalProfile_Artwork].source==kArRegionalSource_US);
  uint16_t delta=123;
  assert(ArRegionalProfiles_Changes(&baseline,&changed,&delta) && !delta);
  changed.actor_artwork.source[0]=kArRegionalSource_Japan;
  assert(ArRegionalProfiles_Describe(&changed,summary));
  assert(summary[kArRegionalProfile_Artwork].source==kArRegionalSource_Count);
  assert(summary[kArRegionalProfile_Presentation].source==kArRegionalSource_Count);
  assert(ArRegionalProfiles_Changes(&baseline,&changed,&delta) && delta==(1u<<kArRegionalProfile_Artwork));
  ArRegionalProfileCache cache = {0};
  assert(ArRegionalProfiles_RefreshCache(&cache, &changed, &baseline));
  assert(cache.valid && cache.pending_groups == delta);
  assert(!memcmp(cache.profiles, summary, sizeof(summary)));
  const ArRegionalProfileCache cached = cache;
  assert(ArRegionalProfiles_RefreshCache(&cache, &changed, &baseline));
  assert(!memcmp(&cache, &cached, sizeof(cache)));
  assert(ArRegionalProfiles_RefreshCache(&cache, &changed, &changed));
  assert(!cache.pending_groups); // Activation refreshes even without a UI edit.
  assert(ArRegionalProfiles_RefreshCache(&cache, &baseline, &changed));
  assert(cache.pending_groups == delta); // Debug restoration is value-keyed.
  ArRegionalRules invalid = changed;
  invalid.actor_artwork.source[0] = kArRegionalSource_Count;
  const ArRegionalProfileCache before_invalid = cache;
  assert(!ArRegionalProfiles_RefreshCache(&cache, &invalid, &changed));
  assert(!memcmp(&cache, &before_invalid, sizeof(cache)));
  assert(ArRegionalProfiles_Expand(&changed,kArRegionalProfile_Lives,2,&all));
  assert(!memcmp(&changed.actor_artwork,&all.actor_artwork,sizeof(all.actor_artwork)));
  // Difficulty owns its modifiers. Only EU placement filtering additionally
  // affects Stage's pending state; stage/combat presets preserve difficulty.
  all=baseline;all.difficulty.level=kArRegionalDifficulty_Expert;
  assert(ArRegionalProfiles_Changes(&baseline,&all,&delta) && !delta);
  assert(ArRegionalProfiles_Expand(&all,kArRegionalProfile_Gameplay,2,&all));
  changed=all;changed.difficulty.level=kArRegionalDifficulty_Beginner;
  assert(ArRegionalProfiles_Changes(&all,&changed,&delta));
  assert(delta && !(delta&~((1u<<kArRegionalProfile_Stage)|(1u<<kArRegionalProfile_Difficulty))));
  all.difficulty.level=kArRegionalDifficulty_Expert;
  assert(ArRegionalProfiles_Expand(&all,kArRegionalProfile_Stage,0,&changed));
  assert(!memcmp(&changed.difficulty,&all.difficulty,sizeof(all.difficulty)));
  assert(ArRegionalProfiles_Expand(&all,kArRegionalProfile_Combat,1,&changed));
  assert(!memcmp(&changed.difficulty,&all.difficulty,sizeof(all.difficulty)));
  assert(ArRegionalProfiles_Expand(&all,kArRegionalProfile_Presentation,1,&changed));
  assert(changed.difficulty.level==kArRegionalDifficulty_Expert);
  // Failed calls preserve outputs, including late invalid fields.
  const ArRegionalRules before=all;
  changed=baseline;changed.actor_artwork.source[6]=kArRegionalSource_Count;
  assert(!ArRegionalProfiles_Expand(&changed,kArRegionalProfile_Gameplay,1,&all));
  assert(!memcmp(&before,&all,sizeof(all)));
  assert(!ArRegionalProfiles_Expand(&baseline,kArRegionalProfile_Count,1,&all));
  assert(!ArRegionalProfiles_Expand(&baseline,kArRegionalProfile_Gameplay,kArRegionalSource_Count,&all));
  assert(!ArRegionalProfiles_Expand(NULL,0,0,&all));
  assert(!ArRegionalProfiles_Describe(&changed,summary));
  delta=123;assert(!ArRegionalProfiles_Changes(&baseline,&changed,&delta) && delta==123);

  ArRegionalSession session;
  ArRegionalCostPolicy prices;assert(ArRegionalCosts_Init(&prices,0));
  const uint8_t id[16]={42};
  assert(ArRegionalSession_NewGame(&session,0,id,&prices));
  for(unsigned town=0;town<6;++town)assert(ArRegionalLairHistory_InitTown(&session.lairs,town));
  assert(ArRegionalLairReloads_Init(&session.reloads));
  const ArRegionalSession original=session;
  TestActionRoomActivation(&original);
  assert(ArRegionalProfiles_Expand(&session.requested,kArRegionalProfile_Gameplay,1,&all));
  assert(!ArRegionalSession_RequestRules(&session,session.revision,&all)); // No implicit quake.
  assert(!memcmp(&session,&original,sizeof(session)));
  assert(ArRegionalProfiles_Expand(&session.requested,kArRegionalProfile_Presentation,1,&all));
  assert(!ArRegionalSession_RequestRules(&session,session.revision+1,&all));
  assert(ArRegionalSession_RequestRules(&session,session.revision,&all));
  assert(session.revision==original.revision+1);
  assert(!memcmp(&session.effective,&original.effective,sizeof(session.effective)));
  assert(!memcmp(&session.lairs,&original.lairs,sizeof(session.lairs)));
  const unsigned revision=session.revision;
  assert(ArRegionalSession_RequestRules(&session,revision,&all) && session.revision==revision);
  for(unsigned source=0;source<3;++source) {
    // Simulate the confirmed conversion owner on an isolated candidate.
    assert(ArRegionalSession_SetPopulationProfile(&session,session.revision,source));
    assert(ArRegionalProfiles_Expand(&session.requested,kArRegionalProfile_Gameplay,source,&all));
    assert(ArRegionalSession_RequestRules(&session,session.revision,&all));
    assert(ArRegionalProfiles_Describe(&session.requested,summary));
    assert(summary[kArRegionalProfile_Gameplay].source==source);
  }
  session=original;session.lairs.diverged_towns=1;
  assert(ArRegionalProfiles_Expand(&session.requested,kArRegionalProfile_Resources,1,&all));
  const ArRegionalSession quarantined=session;
  assert(!ArRegionalSession_RequestRules(&session,session.revision,&all));
  assert(!memcmp(&session,&quarantined,sizeof(session)));
  session=original;session.revision=UINT32_MAX;
  assert(ArRegionalProfiles_Expand(&session.requested,kArRegionalProfile_Presentation,1,&all));
  assert(!ArRegionalSession_RequestRules(&session,UINT32_MAX,&all) && session.revision==UINT32_MAX);
}
