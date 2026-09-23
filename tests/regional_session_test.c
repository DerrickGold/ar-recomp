#define _POSIX_C_SOURCE 200809L
#include "regional/regional_session.h"
#include "byte_order.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(path) _mkdir(path)
#define REMOVE_DIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MAKE_DIR(path) mkdir(path, 0700)
#define REMOVE_DIR(path) rmdir(path)
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

static void Image(uint8_t *image, unsigned marker) {
  memset(image, 0, kActRaiserSramSize);
  image[0x100] = (uint8_t)marker;
  memcpy(image + 0x1439, "MASTER", 6);
  Save_RecomputeChecksum(image);
}

static bool EqualSession(const ArRegionalSession *a, const ArRegionalSession *b) {
  return a->slot == b->slot && a->revision == b->revision &&
      a->arrival_locked==b->arrival_locked && a->requested.arrival==b->requested.arrival && a->effective.arrival==b->effective.arrival &&
      !memcmp(a->campaign, b->campaign, sizeof(a->campaign)) &&
      !memcmp(&a->sim_actors,&b->sim_actors,sizeof(a->sim_actors)) &&
      !memcmp(&a->requested.sim_combat,&b->requested.sim_combat,sizeof(a->requested.sim_combat)) &&
      !memcmp(&a->effective.sim_combat,&b->effective.sim_combat,sizeof(a->effective.sim_combat)) &&
      !memcmp(&a->requested.sim_ai,&b->requested.sim_ai,sizeof(a->requested.sim_ai)) &&
      !memcmp(&a->effective.sim_ai,&b->effective.sim_ai,sizeof(a->effective.sim_ai)) &&
      a->requested.level_goals==b->requested.level_goals && a->effective.level_goals==b->effective.level_goals &&
      a->requested.construction==b->requested.construction && a->effective.construction==b->effective.construction &&
      !memcmp(&a->requested.support,&b->requested.support,sizeof(a->requested.support)) &&
      !memcmp(&a->effective.support,&b->effective.support,sizeof(a->effective.support)) &&
      !memcmp(&a->requested.town_status,&b->requested.town_status,sizeof(a->requested.town_status)) &&
      !memcmp(&a->effective.town_status,&b->effective.town_status,sizeof(a->effective.town_status)) &&
      a->requested.lair_reloads==b->requested.lair_reloads && a->effective.lair_reloads==b->effective.lair_reloads &&
      a->reloads.initialized_towns==b->reloads.initialized_towns &&
      a->reloads.approximate_towns==b->reloads.approximate_towns &&
      a->reloads.diverged_towns==b->reloads.diverged_towns &&
      !memcmp(a->reloads.delay,b->reloads.delay,sizeof(a->reloads.delay)) &&
      !memcmp(&a->requested.costs, &b->requested.costs, sizeof(a->requested.costs)) &&
      !memcmp(&a->effective.costs, &b->effective.costs, sizeof(a->effective.costs)) &&
      !memcmp(&a->requested.timers, &b->requested.timers, sizeof(a->requested.timers)) &&
      !memcmp(&a->effective.timers, &b->effective.timers, sizeof(a->effective.timers)) &&
      a->requested.retry_score == b->requested.retry_score &&
      a->effective.retry_score == b->effective.retry_score &&
      a->requested.town_wait == b->requested.town_wait && a->effective.town_wait == b->effective.town_wait &&
      a->requested.fishing == b->requested.fishing && a->effective.fishing == b->effective.fishing &&
      !memcmp(&a->requested.development,&b->requested.development,sizeof(a->requested.development)) &&
      !memcmp(&a->effective.development,&b->effective.development,sizeof(a->effective.development)) &&
      !memcmp(&a->requested.recovery, &b->requested.recovery, sizeof(a->requested.recovery)) &&
      !memcmp(&a->effective.recovery, &b->effective.recovery, sizeof(a->effective.recovery)) &&
      !memcmp(&a->requested.quake, &b->requested.quake, sizeof(a->requested.quake)) &&
      !memcmp(&a->effective.quake, &b->effective.quake, sizeof(a->effective.quake)) &&
      a->requested.score_page == b->requested.score_page && a->effective.score_page == b->effective.score_page &&
      a->requested.menu_return == b->requested.menu_return && a->effective.menu_return == b->effective.menu_return &&
      a->requested.speed_range == b->requested.speed_range && a->effective.speed_range == b->effective.speed_range &&
      a->requested.magic_gesture == b->requested.magic_gesture && a->effective.magic_gesture == b->effective.magic_gesture &&
      a->requested.lair_seeds == b->requested.lair_seeds && a->effective.lair_seeds == b->effective.lair_seeds &&
      a->requested.house_credit == b->requested.house_credit && a->effective.house_credit == b->effective.house_credit &&
      !memcmp(&a->requested.score_feedback,&b->requested.score_feedback,sizeof(a->requested.score_feedback)) &&
      !memcmp(&a->effective.score_feedback,&b->effective.score_feedback,sizeof(a->effective.score_feedback)) &&
      a->lairs.initialized_towns == b->lairs.initialized_towns &&
      a->lairs.approximate_towns == b->lairs.approximate_towns &&
      a->lairs.diverged_towns == b->lairs.diverged_towns &&
      !memcmp(a->lairs.stock,b->lairs.stock,sizeof(a->lairs.stock));
}

static void CheckArrival(void) {
  const uint8_t id[16]={1};const ArRegionalCostPolicy defaults={{0}};
  for(unsigned source=0;source<3;++source)for(unsigned continuing=0;continuing<2;++continuing) {
    ArRegionalSession session;CHECK(ArRegionalSession_NewGame(&session,0,id,&defaults));
    CHECK(ArRegionalSession_RequestArrival(&session,session.revision,(ArRegionalSource)source));
    CHECK(!session.arrival_locked && session.effective.arrival==0);
    bool japanese;
    CHECK(ArRegionalSession_BeginArrival(&session,continuing,&japanese));
    CHECK(session.arrival_locked && japanese==(source==1 && !continuing));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_BeginArrival(&session,false,&japanese) && revision==session.revision);
    CHECK(ArRegionalSession_RequestArrival(&session,session.revision,source==1?0:1));
    CHECK(ArRegionalSession_BeginArrival(&session,false,&japanese));
    CHECK(japanese==(source==1 && !continuing));
    const ArRegionalSession before=session;
    CHECK(!ArRegionalSession_RequestArrival(&session,session.revision-1,2) && EqualSession(&session,&before));
    CHECK(!ArRegionalSession_RequestArrival(&session,session.revision,3) && EqualSession(&session,&before));
  }
  ArRegionalSession session;CHECK(ArRegionalSession_NewGame(&session,0,id,&defaults));
  session.revision=UINT32_MAX;const ArRegionalSession before=session;bool japanese=true;
  CHECK(!ArRegionalSession_BeginArrival(&session,false,&japanese) && japanese && EqualSession(&before,&session));
}

static void CheckPopulation(void) {
  ArRegionalRules rules={0};
  for(unsigned combination=0;combination<243;++combination) {
    unsigned digits=combination;bool reduced=false;
    for(unsigned i=0;i<5;++i) { rules.support.source[i]=digits%3;reduced|=digits%3==1;digits/=3; }
    for(unsigned level=0;level<3;++level)for(unsigned hint=0;hint<3;++hint)for(unsigned tablet=0;tablet<3;++tablet) {
      rules.level_goals=level;rules.story.source[0]=hint;rules.story.source[1]=tablet;
      CHECK(ArRegionalRules_PopulationCompatible(&rules)==(!reduced || (level==1 && hint==1 && tablet==1)));
    }
  }
  const uint8_t id[16]={99};uint8_t image[kActRaiserSramSize];Image(image,99);SaveError error;
  const char *path="regional-population.srm";
  remove(path);remove("regional-population.srm.archeckpoint");
  ArRegionalSession session,loaded;ArRegionalCostPolicy defaults={{0}};
  CHECK(ArRegionalSession_NewGame(&session,0,id,&defaults));
  session.requested.story.source[2]=session.effective.story.source[2]=1;
  for(unsigned n=0;n<6;++n) {
    const ArRegionalSource source=(n+1)%3;
    ArRegionalSession before=session;uint32_t revision=session.revision;
    CHECK(!ArRegionalSession_SetPopulationProfile(&session,revision-1,source) && EqualSession(&session,&before));
    CHECK(ArRegionalSession_SetPopulationProfile(&session,revision,source));
    CHECK(session.revision==revision+1 && session.requested.level_goals==source && session.effective.level_goals==source);
    CHECK(session.requested.story.source[0]==source && session.effective.story.source[1]==source);
    CHECK(session.requested.story.source[2]==1 && session.effective.story.source[2]==1);
    for(unsigned i=0;i<5;++i) CHECK(session.requested.support.source[i]==source && session.effective.support.source[i]==source);
    before=session;
    CHECK(ArRegionalSession_SetPopulationProfile(&session,session.revision,source) && EqualSession(&session,&before));
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,n?image:NULL,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
    if(source==1) {
      CHECK(!ArRegionalSession_RequestLevelGoals(&session,session.revision,0) && EqualSession(&session,&before));
      ArRegionalStoryPolicy story=session.requested.story;story.source[0]=0;
      CHECK(!ArRegionalSession_RequestStory(&session,session.revision,&story) && EqualSession(&session,&before));
      story=session.requested.story;story.source[1]=2;
      CHECK(!ArRegionalSession_RequestStory(&session,session.revision,&story) && EqualSession(&session,&before));
      story=session.requested.story;story.source[2]=0;
      CHECK(ArRegionalSession_RequestStory(&session,session.revision,&story));
      story.source[2]=1;CHECK(ArRegionalSession_RequestStory(&session,session.revision,&story));
    }
  }
  session.revision=UINT32_MAX;ArRegionalSession before=session;
  CHECK(!ArRegionalSession_SetPopulationProfile(&session,session.revision,1) && EqualSession(&session,&before));
  CHECK(!ArRegionalSession_SetPopulationProfile(&session,session.revision,kArRegionalSource_Count));
  session.revision=1;session.requested.support.source[0]=1;
  CHECK(!ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
  remove(path);remove("regional-population.srm.archeckpoint");
}

static void CheckConstruction(void) {
  const uint8_t id[16]={98};uint8_t image[kActRaiserSramSize];Image(image,98);SaveError error;
  const char *path="regional-construction.srm";
  remove(path);remove("regional-construction.srm.archeckpoint");
  ArRegionalSession session,loaded;ArRegionalCostPolicy defaults={{0}};
  CHECK(ArRegionalSession_NewGame(&session,0,id,&defaults));
  for(unsigned n=0;n<6;++n) {
    const ArRegionalSource source=(ArRegionalSource)((n+1)%3);
    const ArRegionalSource old=session.effective.construction;
    CHECK(ArRegionalSession_RequestConstruction(&session,session.revision,source));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_RequestConstruction(&session,revision,source) && session.revision==revision);
    CHECK(!ArRegionalSession_RequestConstruction(&session,revision-1,source));
    CHECK(session.effective.construction==old);
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,n?image:NULL,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
    bool jp=false;CHECK(ArRegionalSession_BeginConstruction(&session,&jp) && jp==(source==1));
    CHECK(session.revision==revision+1 && session.effective.construction==source);
    CHECK(ArRegionalSession_BeginConstruction(&session,&jp) && session.revision==revision+1);
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
  }
  ArRegionalSession before=session;bool jp=true;
  CHECK(!ArRegionalSession_RequestConstruction(&session,session.revision,3) && EqualSession(&before,&session));
  CHECK(!ArRegionalSession_BeginConstruction(&session,NULL) && EqualSession(&before,&session));
  session.revision=UINT32_MAX;
  CHECK(!ArRegionalSession_RequestConstruction(&session,session.revision,1));
  session.requested.construction=1;before=session;
  CHECK(!ArRegionalSession_BeginConstruction(&session,&jp) && jp && EqualSession(&before,&session));
  remove(path);remove("regional-construction.srm.archeckpoint");
}

static void CheckSimCombat(void) {
  const uint8_t id[16]={97};uint8_t image[kActRaiserSramSize];Image(image,97);SaveError error;
  const char *path="regional-sim-combat.srm";
  ArRegionalSession session,loaded;ArRegionalCostPolicy defaults={{0}};
  CHECK(ArRegionalSession_NewGame(&session,0,id,&defaults));
  ArRegionalSimCombatPolicy policy;CHECK(ArRegionalSimCombat_Init(&policy,1));
  CHECK(ArRegionalSession_RequestSimCombat(&session,session.revision,&policy));
  ArRegionalSimAiPolicy ai;CHECK(ArRegionalSimAi_Init(&ai,1));
  CHECK(ArRegionalSession_RequestSimAi(&session,session.revision,&ai));
  CHECK(!ArRegionalSession_RequestSimAi(&session,session.revision-1,&ai));
  const ArRegionalSession requested=session;
  CHECK(!ArRegionalSession_RequestSimCombat(&session,session.revision-1,&policy));
  CHECK(!ArRegionalSession_BeginSimActor(&session,0,0) && EqualSession(&session,&requested));
  CHECK(ArRegionalSimActors_LoadTown(&session.sim_actors,0));
  CHECK(ArRegionalSession_BeginSimActor(&session,0,0) && session.sim_actors.active[0].combat==31);
  CHECK(session.sim_actors.active[0].ai==63 && session.revision==requested.revision+1);
  CHECK(ArRegionalSimActors_SaveTown(&session.sim_actors,0));
  CHECK(ArRegionalSimCombat_Init(&policy,0) && ArRegionalSession_RequestSimCombat(&session,session.revision,&policy));
  ai.source[kArRegionalSimAi_TargetPool]=0;
  CHECK(ArRegionalSession_RequestSimAi(&session,session.revision,&ai));
  CHECK(ArRegionalSession_BeginSimActor(&session,0,1) && !session.sim_actors.active[1].combat && session.sim_actors.active[0].combat==31);
  CHECK(session.sim_actors.active[0].ai==63 && session.sim_actors.active[1].ai==55);
  CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,NULL,image,&error));
  CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
  CHECK(ArRegionalSimActors_LoadTown(&loaded.sim_actors,1));
  CHECK(ArRegionalSession_BeginSimActor(&loaded,1,0) && !loaded.sim_actors.active[0].combat);
  CHECK(loaded.sim_actors.active[0].ai==55);
  CHECK(ArRegionalSimActors_LoadTown(&loaded.sim_actors,0) && loaded.sim_actors.active[0].combat==31);
  CHECK(loaded.sim_actors.active[0].ai==63);
  CHECK(ArRegionalSession_BeginSimActor(&loaded,0,0) && !loaded.sim_actors.active[0].combat);
  CHECK(ArRegionalSession_Save(&loaded,kSaveFileFormat_NativeSrm,path,image,image,&error));
  CHECK(ArRegionalSession_Load(&session,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
  CHECK(session.sim_actors.cached[0].combat==31 && session.sim_actors.active[0].combat==0); /* old cache, new generation */
  session.revision=UINT32_MAX;policy.source[0]=1;
  CHECK(!ArRegionalSession_RequestSimCombat(&session,session.revision,&policy));
  ai.source[0]=0;CHECK(!ArRegionalSession_RequestSimAi(&session,session.revision,&ai));
  session.requested.sim_combat=policy;const ArRegionalSession frozen=session;
  CHECK(!ArRegionalSession_BeginSimActor(&session,0,0) && EqualSession(&frozen,&session));
  remove(path);remove("regional-sim-combat.srm.archeckpoint");
}
static void CheckLevelGoals(void) {
  const uint8_t id[16]={0x4c};ArRegionalSession session,loaded;
  ArRegionalCostPolicy costs;CHECK(ArRegionalCosts_Init(&costs,0));CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  const char *path="regional-level-codec.srm";remove(path);remove("regional-level-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];Image(image,9);SaveError error;
  for(unsigned source=0;source<3;++source) {
    CHECK(ArRegionalSession_RequestLevelGoals(&session,session.revision,(ArRegionalSource)source));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_RequestLevelGoals(&session,revision,(ArRegionalSource)source) && session.revision==revision);
    CHECK(!ArRegionalSession_RequestLevelGoals(&session,revision-1,(ArRegionalSource)source));
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,source?image:NULL,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
    bool jp;CHECK(ArRegionalSession_BeginLevelGoals(&session,&jp) && jp==(source==1));
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
  }
  const ArRegionalSession before=session;
  CHECK(!ArRegionalSession_RequestLevelGoals(&session,session.revision,3) && EqualSession(&session,&before));
  session.revision=UINT32_MAX;CHECK(!ArRegionalSession_RequestLevelGoals(&session,session.revision,1));
  session.requested.level_goals=1;bool sentinel=false;
  CHECK(!ArRegionalSession_BeginLevelGoals(&session,&sentinel) && !sentinel);
  remove(path);remove("regional-level-codec.srm.archeckpoint");
}

static void CheckTownStatus(void) {
  const uint8_t id[16]={0x48}; ArRegionalSession session,loaded;
  ArRegionalCostPolicy costs; CHECK(ArRegionalCosts_Init(&costs,kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  const char *path="regional-status-codec.srm";
  remove(path);remove("regional-status-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];Image(image,9);SaveError error;
  ArRegionalTownStatusPolicy policy;
  for(unsigned combination=0;combination<243;++combination) {
    unsigned digits=combination;
    for(unsigned i=0;i<kArRegionalTownStatus_Count;++i) { policy.source[i]=(ArRegionalSource)(digits%3);digits/=3; }
    CHECK(ArRegionalSession_RequestTownStatus(&session,session.revision,&policy));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_RequestTownStatus(&session,revision,&policy) && session.revision==revision);
    CHECK(!ArRegionalSession_RequestTownStatus(&session,revision-1,&policy));
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,combination?image:NULL,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&session,&loaded));
    ArRegionalTownStatusSnapshot snapshot;
    CHECK(ArRegionalSession_BeginTownStatus(&session,&snapshot));
    for(unsigned i=0;i<kArRegionalTownStatus_Count;++i) CHECK(snapshot.japanese[i]==(policy.source[i]==1));
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&session,&loaded));
  }
  const ArRegionalSession before=session;
  policy.source[1]=kArRegionalSource_Count;
  CHECK(!ArRegionalSession_RequestTownStatus(&session,session.revision,&policy) && EqualSession(&before,&session));
  ArRegionalTownStatus_Init(&policy,kArRegionalSource_Japan);session.revision=UINT32_MAX;
  CHECK(!ArRegionalSession_RequestTownStatus(&session,session.revision,&policy));
  session.requested.town_status=policy;
  ArRegionalTownStatusSnapshot sentinel={{7,7,7,7,7}},unchanged=sentinel;
  CHECK(!ArRegionalSession_BeginTownStatus(&session,&sentinel) && !memcmp(&sentinel,&unchanged,sizeof(sentinel)));
  remove(path);remove("regional-status-codec.srm.archeckpoint");
}

static void CheckLairReloads(void) {
  const uint8_t id[16]={0x47}; ArRegionalSession session,loaded;
  ArRegionalCostPolicy costs; CHECK(ArRegionalCosts_Init(&costs,kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  CHECK(!ArRegionalSession_RequestLairReloads(&session,session.revision,kArRegionalSource_Japan));
  CHECK(ArRegionalLairReloads_Init(&session.reloads));
  CHECK(ArRegionalLairReloads_ReduceTown(&session.reloads,3));
  session.reloads.approximate_towns=2;
  const char *path="regional-reloads-codec.srm";
  remove(path);remove("regional-reloads-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];Image(image,4);SaveError error;
  for(unsigned i=0;i<3;++i) {
    const ArRegionalSource source=(ArRegionalSource)((i+1)%3);
    CHECK(ArRegionalSession_RequestLairReloads(&session,session.revision,source));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_RequestLairReloads(&session,revision,source) && session.revision==revision);
    CHECK(!ArRegionalSession_RequestLairReloads(&session,revision-1,source));
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,i?image:NULL,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
    const ArRegionalLairReloads before=session.reloads;
    CHECK(ArRegionalSession_BeginLairReloads(&session));
    CHECK(session.effective.lair_reloads==source && !memcmp(&before,&session.reloads,sizeof(before)));
  }
  ArRegionalSession before=session;
  CHECK(!ArRegionalSession_RequestLairReloads(&session,session.revision,kArRegionalSource_Count));
  CHECK(EqualSession(&session,&before));session.revision=UINT32_MAX;
  CHECK(!ArRegionalSession_RequestLairReloads(&session,session.revision,kArRegionalSource_Japan));
  session.requested.lair_reloads=kArRegionalSource_Japan;
  CHECK(!ArRegionalSession_BeginLairReloads(&session));session=before;
  session.reloads.diverged_towns=4;
  CHECK(!ArRegionalSession_RequestLairReloads(&session,session.revision,kArRegionalSource_Japan));
  CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
  CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
  remove(path);remove("regional-reloads-codec.srm.archeckpoint");
}

static void CheckScoreFeedback(void) {
  const uint8_t id[16]={0x73}; ArRegionalSession session,loaded;
  ArRegionalCostPolicy costs; CHECK(ArRegionalCosts_Init(&costs,kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  ArRegionalScorePolicy policy;
  CHECK(ArRegionalScore_Init(&policy,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestScoreFeedback(&session,session.revision,&policy));
  for(unsigned town=0;town<6;++town)CHECK(ArRegionalLairHistory_InitTown(&session.lairs,town));
  const char *path="regional-score-codec.srm";
  remove(path);remove("regional-score-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];Image(image,9);SaveError error;
  for(unsigned combination=0;combination<81;++combination) {
    unsigned digits=combination,expected=0;
    for(unsigned i=0;i<kArRegionalScore_Count;++i) {
      policy.source[i]=(ArRegionalSource)(digits%3);digits/=3;
      if(i<kArRegionalScore_Phase && policy.source[i]==kArRegionalSource_Japan)expected|=4u<<i;
    }
    CHECK(ArRegionalSession_RequestScoreFeedback(&session,session.revision,&policy));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_RequestScoreFeedback(&session,revision,&policy) && session.revision==revision);
    CHECK(!ArRegionalSession_RequestScoreFeedback(&session,revision-1,&policy));
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,combination?image:NULL,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
    ArRegionalLairAccounting snapshot;unsigned projection=99;
    CHECK(ArRegionalSession_BeginLairAccounting(&session,&snapshot));
    CHECK(ArRegionalLairAccounting_Projection(&snapshot,&projection) && projection==expected);
    ArRegionalScoreSnapshot completion;
    const ArRegionalLairHistory history=session.lairs;
    CHECK(ArRegionalSession_BeginScoreCompletion(&session,&completion));
    CHECK(completion.japanese[kArRegionalScore_Phase]==(policy.source[kArRegionalScore_Phase]==kArRegionalSource_Japan));
    CHECK(!memcmp(&session.lairs,&history,sizeof(history)));
    CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
    CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready && EqualSession(&loaded,&session));
  }
  const ArRegionalSession before=session;
  policy.source[2]=kArRegionalSource_Count;
  CHECK(!ArRegionalSession_RequestScoreFeedback(&session,session.revision,&policy) && EqualSession(&session,&before));
  ArRegionalScore_Init(&policy,kArRegionalSource_Japan);
  session.revision=UINT32_MAX;
  CHECK(!ArRegionalSession_RequestScoreFeedback(&session,session.revision,&policy));
  session=before; CHECK(ArRegionalLairHistory_MarkDiverged(&session.lairs,0));
  CHECK(!ArRegionalSession_RequestScoreFeedback(&session,session.revision,&policy));
  remove(path);remove("regional-score-codec.srm.archeckpoint");
}

static void CheckLairSeeds(void) {
  const uint8_t id[16]={71};
  ArRegionalCostPolicy costs; CHECK(ArRegionalCosts_Init(&costs,kArRegionalSource_US));
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  CHECK(!ArRegionalSession_RequestLairSeeds(&session,session.revision,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestHouseCredit(&session,session.revision,kArRegionalSource_Japan));
  for(unsigned town=0; town<6; ++town) CHECK(ArRegionalLairHistory_InitTown(&session.lairs,town));
  CHECK(ArRegionalSession_RequestLairSeeds(&session,session.revision,kArRegionalSource_Japan));
  CHECK(session.effective.lair_seeds==kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestLairSeeds(&session,session.revision-1,kArRegionalSource_US));
  ArRegionalLairAccounting snapshot;
  CHECK(ArRegionalSession_BeginLairAccounting(&session,&snapshot) && snapshot.seeds==kArRegionalSource_Japan);
  const uint32_t revision=session.revision;
  CHECK(ArRegionalSession_BeginLairAccounting(&session,&snapshot) && revision==session.revision);
  CHECK(ArRegionalSession_RequestHouseCredit(&session,session.revision,kArRegionalSource_Japan));
  CHECK(session.effective.house_credit == kArRegionalSource_US);
  const char *path="regional-seed-codec.srm";
  remove(path); remove("regional-seed-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize]; Image(image,7); SaveError error;
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,NULL,image,&error));
  CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(EqualSession(&session,&loaded));
  CHECK(!ArRegionalSession_RequestHouseCredit(&session,session.revision-1,kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginLairAccounting(&session,&snapshot) && snapshot.house_credit==kArRegionalSource_Japan);
  CHECK(ArRegionalSession_RequestLairSeeds(&session,session.revision,kArRegionalSource_Europe));
  CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
  CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(EqualSession(&session,&loaded) && loaded.effective.lair_seeds==kArRegionalSource_Japan);
  CHECK(ArRegionalLairHistory_MarkDiverged(&session.lairs,1));
  CHECK(!ArRegionalSession_BeginLairAccounting(&session,&snapshot));
  CHECK(!ArRegionalSession_RequestHouseCredit(&session,session.revision,kArRegionalSource_US));
  CHECK(!ArRegionalSession_RequestLairSeeds(&session,session.revision,kArRegionalSource_US));
  CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
  CHECK(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(EqualSession(&session,&loaded)); /* Quarantine retains both requested/effective choices. */
  remove(path); remove("regional-seed-codec.srm.archeckpoint");
}

static void CheckQuakeActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {42};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_RequestQuake(&session, 2, kArRegionalSource_Japan) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestQuake(&session, 1, kArRegionalSource_Count) && EqualSession(&before, &session));
  CHECK(ArRegionalSession_RequestQuake(&session, 1, kArRegionalSource_Japan));
  CHECK(session.effective.quake.source[0] == kArRegionalSource_US);
  ArRegionalQuakeSnapshot snapshot;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot));
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) CHECK(snapshot.random[i]);
  before = session;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot) && EqualSession(&before, &session));
  session.requested.quake.source[0] = kArRegionalSource_Europe;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot) && !snapshot.random[0] && snapshot.random[1]);
  ArRegionalSource source = kArRegionalSource_Count;
  CHECK(!ArRegionalQuake_GroupSource(&session.effective.quake, &source) && source == kArRegionalSource_Count);
  CHECK(ArRegionalSession_RequestQuake(&session, session.revision, kArRegionalSource_US));
  session.revision = UINT32_MAX; before = session;
  CHECK(!ArRegionalSession_BeginQuake(&session, &snapshot) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestQuake(&session, UINT32_MAX, kArRegionalSource_Japan) && EqualSession(&before, &session));
}

static void CheckScorePageActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool enabled = false;
  CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) && enabled && session.revision == 1);
  CHECK(!ArRegionalSession_RequestScorePage(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestScorePage(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) && enabled == (source != kArRegionalSource_Japan));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX; ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginScorePage(&session, &enabled) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestScorePage(&session, UINT32_MAX, kArRegionalSource_US) && EqualSession(&before, &session));
  CHECK(!ArRegionalScorePage_Resolve(kArRegionalSource_Count, &enabled) && enabled);
  CHECK(!ArRegionalScorePage_Resolve(kArRegionalSource_US, NULL));
}

static void CheckLivesDisplayActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {44};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool zero_based = true;
  CHECK(ArRegionalSession_BeginLivesDisplay(&session, &zero_based) && !zero_based && session.revision == 1);
  CHECK(!ArRegionalSession_RequestLivesDisplay(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestLivesDisplay(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestLivesDisplay(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginLivesDisplay(&session, &zero_based) && zero_based == (source == kArRegionalSource_Japan));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginLivesDisplay(&session, &zero_based) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestLivesDisplay(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX; ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginLivesDisplay(&session, &zero_based) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestLivesDisplay(&session, UINT32_MAX, kArRegionalSource_US) && EqualSession(&before, &session));
  CHECK(!ArRegionalLivesDisplay_Resolve(kArRegionalSource_Count, &zero_based) && !zero_based);
  CHECK(!ArRegionalLivesDisplay_Resolve(kArRegionalSource_US, NULL));
}

static void CheckSourcesActivation(void) {
  ArRegionalSession session;
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs,kArRegionalSource_US);
  const uint8_t id[16]={45};
  CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  ArRegionalSourcesSnapshot snapshot;
  for(unsigned life=0;life<3;++life)for(unsigned magic=0;magic<3;++magic) {
    ArRegionalSourcesPolicy policy={{life,magic}};
    const ArRegionalSession before=session;
    CHECK(!ArRegionalSession_RequestSources(&session,session.revision+1,&policy));
    CHECK(EqualSession(&before,&session));
    CHECK(ArRegionalSession_RequestSources(&session,session.revision,&policy));
    CHECK(!memcmp(&before.effective,&session.effective,sizeof(session.effective)));
    CHECK(ArRegionalSession_BeginSources(&session,&snapshot));
    CHECK(snapshot.automatic[0]==(life!=1) && snapshot.automatic[1]==(magic!=1));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_BeginSources(&session,&snapshot) && session.revision==revision);
  }
  ArRegionalSourcesPolicy policy={{1,0}};
  CHECK(ArRegionalSession_RequestSources(&session,session.revision,&policy));
  session.revision=UINT32_MAX;
  ArRegionalSession before=session;
  CHECK(!ArRegionalSession_BeginSources(&session,&snapshot) && EqualSession(&before,&session));
  policy.source[0]=0;
  CHECK(!ArRegionalSession_RequestSources(&session,UINT32_MAX,&policy) && EqualSession(&before,&session));
  CHECK(!ArRegionalSession_RequestSources(&session,UINT32_MAX,NULL));
}

static void CheckSkullWait(void) {
  ArRegionalSession session;
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs,kArRegionalSource_US);
  const uint8_t id[16]={46};
  CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  uint16_t frames=123;
  CHECK(!strcmp(ArRegionalSkullWait_Descriptor()->key,"skull_post_effect_frames"));
  CHECK(!ArRegionalSkullWait_Resolve(kArRegionalSource_Count,&frames) && frames==123);
  CHECK(!ArRegionalSkullWait_Resolve(kArRegionalSource_US,NULL));
  CHECK(!ArRegionalSession_RequestSkullWait(&session,2,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestSkullWait(&session,1,kArRegionalSource_Count));
  for(unsigned source=0;source<3;++source) {
    const ArRegionalRules before=session.effective;
    CHECK(ArRegionalSession_RequestSkullWait(&session,session.revision,(ArRegionalSource)source));
    CHECK(!memcmp(&before,&session.effective,sizeof(before)));
    CHECK(ArRegionalSession_BeginSkullWait(&session,&frames) && frames==(source==1?0:90));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_BeginSkullWait(&session,&frames) && session.revision==revision);
  }
  CHECK(ArRegionalSession_RequestSkullWait(&session,session.revision,kArRegionalSource_Japan));
  session.revision=UINT32_MAX; const ArRegionalSession before=session;
  CHECK(!ArRegionalSession_BeginSkullWait(&session,&frames) && EqualSession(&before,&session));
  CHECK(!ArRegionalSession_RequestSkullWait(&session,UINT32_MAX,kArRegionalSource_US) && EqualSession(&before,&session));
}

static void CheckStory(void) {
  ArRegionalSession session;ArRegionalCostPolicy costs;ArRegionalCosts_Init(&costs,0);
  const uint8_t id[16]={47};CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  ArRegionalStorySnapshot snapshot;
  for(unsigned n=0;n<27;++n) {
    ArRegionalStoryPolicy policy;unsigned digits=n;
    for(unsigned i=0;i<3;++i){policy.source[i]=(ArRegionalSource)(digits%3);digits/=3;}
    const ArRegionalSession before=session;
    CHECK(!ArRegionalSession_RequestStory(&session,session.revision+1,&policy) && EqualSession(&before,&session));
    CHECK(ArRegionalSession_RequestStory(&session,session.revision,&policy));
    CHECK(!memcmp(&before.effective,&session.effective,sizeof(session.effective)));
    CHECK(ArRegionalSession_BeginStory(&session,&snapshot));
    for(unsigned i=0;i<3;++i)CHECK(snapshot.value[i]==ArRegionalStory_Descriptor((ArRegionalStoryRule)i)->value[policy.source[i]]);
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_BeginStory(&session,&snapshot) && session.revision==revision);
  }
  ArRegionalStoryPolicy policy={{1,0,2}};
  CHECK(ArRegionalSession_RequestStory(&session,session.revision,&policy));
  session.revision=UINT32_MAX;const ArRegionalSession before=session;
  CHECK(!ArRegionalSession_BeginStory(&session,&snapshot) && EqualSession(&before,&session));
  policy.source[0]=0;
  CHECK(!ArRegionalSession_RequestStory(&session,UINT32_MAX,&policy) && EqualSession(&before,&session));
  policy.source[0]=3;
  CHECK(!ArRegionalSession_RequestStory(&session,UINT32_MAX,&policy) && EqualSession(&before,&session));
}

static void CheckMenuReturnActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool enabled = false;
  CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) && !enabled && session.revision == 1);
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestMenuReturn(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) && enabled == (source == kArRegionalSource_Japan));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestMenuReturn(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX; ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginMenuReturn(&session, &enabled) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, UINT32_MAX, kArRegionalSource_US) && EqualSession(&before, &session));
  CHECK(!ArRegionalMenuReturn_Resolve(kArRegionalSource_Count, &enabled) && !enabled);
  CHECK(!ArRegionalMenuReturn_Resolve(kArRegionalSource_US, NULL));
}

static void CheckMagicGestureActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16]={42};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs,kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  bool gesture=true;
  CHECK(ArRegionalSession_BeginMagicGesture(&session,false,&gesture) && !gesture && session.revision==1);
  CHECK(!ArRegionalSession_RequestMagicGesture(&session,2,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestMagicGesture(&session,1,kArRegionalSource_Count));
  for (unsigned source=0;source<kArRegionalSource_Count;++source) {
    const bool previous=gesture;
    CHECK(ArRegionalSession_RequestMagicGesture(&session,session.revision,(ArRegionalSource)source));
    const ArRegionalSession pending=session;
    CHECK(ArRegionalSession_BeginMagicGesture(&session,false,&gesture) && gesture==previous && EqualSession(&session,&pending));
    CHECK(ArRegionalSession_BeginMagicGesture(&session,true,&gesture) && gesture==(source==kArRegionalSource_Japan));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_BeginMagicGesture(&session,true,&gesture) && session.revision==revision);
  }
  CHECK(ArRegionalSession_RequestMagicGesture(&session,session.revision,kArRegionalSource_Japan));
  session.revision=UINT32_MAX; const ArRegionalSession before=session;
  CHECK(ArRegionalSession_BeginMagicGesture(&session,false,&gesture) && !gesture && EqualSession(&before,&session));
  CHECK(!ArRegionalSession_BeginMagicGesture(&session,true,&gesture) && !gesture && EqualSession(&before,&session));
  CHECK(!ArRegionalSession_RequestMagicGesture(&session,UINT32_MAX,kArRegionalSource_US));
  CHECK(!ArRegionalMagicGesture_Resolve(kArRegionalSource_Count,&gesture) && !gesture);
  CHECK(!ArRegionalMagicGesture_Resolve(kArRegionalSource_US,NULL));
}

static void CheckSpeedRangeActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  uint16_t enabled = 9;
  CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) && enabled == 9 && session.revision == 1);
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestSpeedRange(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) && enabled == (source == kArRegionalSource_Japan ? 7 : 9));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestSpeedRange(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX; ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginSpeedRange(&session, &enabled) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, UINT32_MAX, kArRegionalSource_US) && EqualSession(&before, &session));
  CHECK(!ArRegionalSpeedRange_Resolve(kArRegionalSource_Count, &enabled) && enabled == 9);
  CHECK(!ArRegionalSpeedRange_Resolve(kArRegionalSource_US, NULL));
}

static size_t ReadBytes(const char *path, uint8_t *bytes, size_t capacity) {
  FILE *file = fopen(path, "rb");
  CHECK(file);
  if (!file) return 0;
  size_t size = fread(bytes, 1, capacity, file);
  CHECK(!ferror(file));
  CHECK(fgetc(file) == EOF);
  CHECK(!fclose(file));
  return size;
}

static void CheckDisk(SaveFileFormat format, const char *path, const uint8_t *expected) {
  uint8_t actual[kActRaiserSramSize];
  SaveError error;
  CHECK(Save_LoadFile(format, path, actual, &error));
  CHECK(!memcmp(actual, expected, sizeof(actual)));
}

static SaveCheckpointStatus AcceptOpaque(const uint8_t *bytes, size_t size, void *context) {
  (void)context;
  return bytes && size ? kSaveCheckpoint_Ready : kSaveCheckpoint_Invalid;
}

static void CheckSessionState(void) {
  const uint8_t id[16] = {1}, zero[16] = {0};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_NewGame(&session, 7, zero, &defaults));
  CHECK(EqualSession(&before, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, 1, kArRegionalCostGroup_Miracles, kArRegionalSource_Japan));
  CHECK(session.revision == 2 && session.effective.costs.source[kArRegionalCost_Rain] == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestCosts(&session, 1, kArRegionalCostGroup_Miracles, kArRegionalSource_US));
  CHECK(session.revision == 2);
  ArRegionalCostSnapshot quote;
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(session.revision == 2 && quote.price[kArRegionalCost_Rain] == 20);
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(session.revision == 3 && quote.price[kArRegionalCost_Rain] == 16);
  CHECK(ArRegionalSession_RequestCosts(&session, 3, kArRegionalCostGroup_Miracles, kArRegionalSource_US));
  CHECK(quote.price[kArRegionalCost_Rain] == 16); /* An accepted quote stays frozen. */
  CHECK(session.effective.costs.source[kArRegionalCost_Rain] == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(quote.price[kArRegionalCost_Rain] == 20 && session.revision == 5);
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_RequestCosts(&session, UINT32_MAX, kArRegionalCostGroup_Scrolls, kArRegionalSource_Japan));
  CHECK(EqualSession(&before, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, UINT32_MAX, kArRegionalCostGroup_Scrolls, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(EqualSession(&before, &session));
  session.requested.costs.source[kArRegionalCost_Light] = kArRegionalSource_Japan;
  before = session;
  ArRegionalCostSnapshot old_quote = quote;
  CHECK(!ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(EqualSession(&before, &session));
  CHECK(!memcmp(&quote, &old_quote, sizeof(quote)));

  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  before = session;
  CHECK(ArRegionalSession_RequestTimers(&session, 1, kArRegionalSource_Japan));
  CHECK(session.revision == 2);
  CHECK(!memcmp(&before.requested.costs, &session.requested.costs, sizeof(before.requested.costs)));
  CHECK(session.effective.timers.source[0] == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestTimers(&session, 1, kArRegionalSource_US));
  ArRegionalTimerPolicy snapshot;
  CHECK(ArRegionalSession_BeginTimers(&session, &snapshot));
  CHECK(session.revision == 3 && snapshot.source[0] == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_RequestTimers(&session, 3, kArRegionalSource_Europe));
  CHECK(snapshot.source[0] == kArRegionalSource_Japan);
  before = session;
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginTimers(&session, &snapshot));
  CHECK(snapshot.source[0] == kArRegionalSource_Japan);
  CHECK(!memcmp(&session.effective.timers, &before.effective.timers, sizeof(snapshot)));
  CHECK(!ArRegionalSession_RequestTimers(&session, UINT32_MAX, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  bool clear = false;
  CHECK(ArRegionalSession_RequestRetryScore(&session, session.revision, kArRegionalSource_Japan));
  CHECK(session.effective.retry_score == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestRetryScore(&session, 1, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginRetryScore(&session, &clear) && clear);
  CHECK(session.effective.retry_score == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_RequestRetryScore(&session, session.revision, kArRegionalSource_Europe));
  before = session;
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginRetryScore(&session, &clear) && clear);
  CHECK(session.effective.retry_score == before.effective.retry_score);
  CHECK(!ArRegionalSession_RequestRetryScore(&session, UINT32_MAX, kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  uint16_t wait = 999;
  CHECK(!ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Count));
  CHECK(ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Japan));
  CHECK(session.effective.town_wait == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestTownWait(&session, 1, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginTownWait(&session, &wait) && wait == 150);
  CHECK(session.effective.town_wait == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Europe));
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_BeginTownWait(&session, &wait) && wait == 150);
  CHECK(EqualSession(&before, &session));
  CHECK(ArRegionalSession_RequestTownWait(&session, UINT32_MAX, kArRegionalSource_Europe));
  CHECK(EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestTownWait(&session, UINT32_MAX, kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session,7,id,&defaults));
  bool reconcile = true;
  CHECK(ArRegionalSession_RequestFishing(&session,session.revision,kArRegionalSource_Europe));
  CHECK(ArRegionalSession_BeginFishing(&session,&wait,&reconcile) && wait==255 && !reconcile);
  CHECK(ArRegionalSession_RequestFishing(&session,session.revision,kArRegionalSource_Japan));
  CHECK(ArRegionalSession_BeginFishing(&session,&wait,&reconcile) && wait==128 && reconcile);
  CHECK(ArRegionalSession_BeginFishing(&session,&wait,&reconcile) && wait==128 && !reconcile);
  CHECK(ArRegionalSession_RequestFishing(&session,session.revision,kArRegionalSource_US));
  session.revision=UINT32_MAX; before=session;
  CHECK(!ArRegionalSession_BeginFishing(&session,&wait,&reconcile) && wait==128 && !reconcile);
  CHECK(EqualSession(&session,&before));
  CHECK(!ArRegionalSession_RequestFishing(&session,UINT32_MAX,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestFishing(&session,UINT32_MAX,kArRegionalSource_Count));
  CHECK(ArRegionalSession_NewGame(&session,7,id,&defaults));
  ArRegionalDevelopmentSnapshot development={0};
  CHECK(ArRegionalSession_RequestDevelopment(&session,1,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestDevelopment(&session,1,kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginDevelopment(&session,&development) && development.service_divider==5 && development.long_cycle==480);
  CHECK(ArRegionalSession_RequestDevelopment(&session,session.revision,kArRegionalSource_US));
  session.revision=UINT32_MAX;before=session;
  CHECK(!ArRegionalSession_BeginDevelopment(&session,&development) && development.long_cycle==480);
  CHECK(EqualSession(&session,&before));
  CHECK(!ArRegionalSession_RequestDevelopment(&session,UINT32_MAX,kArRegionalSource_Japan));
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  ArRegionalRecoverySnapshot recovery = {0};
  unsigned changed = 99;
  CHECK(ArRegionalSession_RequestRecovery(&session, 1, kArRegionalSource_Europe));
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && !changed);
  CHECK(recovery.cycle_sp && !recovery.angel_calls);
  CHECK(ArRegionalSession_RequestRecovery(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && changed == 3);
  CHECK(!recovery.cycle_sp && recovery.angel_calls == 60);
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && !changed);
  session.requested.recovery.source[kArRegionalRecovery_SP] = kArRegionalSource_US;
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && changed == 1);
  CHECK(recovery.cycle_sp && recovery.angel_calls == 60);
  CHECK(ArRegionalSession_RequestRecovery(&session, session.revision, kArRegionalSource_US));
  session.revision = UINT32_MAX; before = session;
  CHECK(!ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && changed == 1);
  CHECK(EqualSession(&session, &before));
}

static void CheckPersistence(SaveFileFormat format, const char *path) {
  char companion[128], companion_tmp[132], native_tmp[128];
  snprintf(companion, sizeof(companion), "%s.archeckpoint", path);
  snprintf(companion_tmp, sizeof(companion_tmp), "%s.tmp", companion);
  snprintf(native_tmp, sizeof(native_tmp), "%s.tmp", path);
  remove(path); remove(companion); remove(companion_tmp); remove(native_tmp);
  uint8_t a[kActRaiserSramSize], b[kActRaiserSramSize], c[kActRaiserSramSize];
  Image(a, 1); Image(b, 2); Image(c, 3);
  const uint8_t first_id[16] = {1, 2, 3}, next_id[16] = {4, 5, 6};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session, loaded;
  CHECK(ArRegionalSession_NewGame(&session, 0, first_id, &defaults));
  CHECK(ArRegionalLairHistory_InitTown(&session.lairs,0));
  CHECK(ArRegionalLairHistory_KillAttempt(&session.lairs,0));
  CHECK(ArRegionalLairHistory_AdoptTown(&session.lairs,5,kArRegionalSource_US,(uint16_t[4]){20,0,301,65535}));
  loaded = session;
  SaveError error;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  CHECK(EqualSession(&loaded, &session));
  /* Companion failure cannot create or replace the native file. */
  CHECK(!MAKE_DIR(companion_tmp));
  CHECK(!ArRegionalSession_Save(&session, format, path, NULL, a, &error));
  FILE *probe = fopen(path, "rb"); CHECK(!probe); if (probe) fclose(probe);
  CHECK(!REMOVE_DIR(companion_tmp));
  /* Crash window for the very first save: journal exists but native replace
   * failed. The exact retry is allowed without losing either payload. */
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!ArRegionalSession_Save(&session, format, path, NULL, a, &error));
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(ArRegionalSession_Save(&session, format, path, NULL, a, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));
  CHECK(ArRegionalSession_Load(&loaded, 1, path, a, &error) == kSaveCheckpoint_Mismatch);
  CHECK(EqualSession(&loaded, &session));

  /* Save a pending choice without modifying native SRAM. The blocked native
   * temporary path proves this path needs only an atomic companion replace. */
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Miracles, kArRegionalSource_Japan));
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(ArRegionalSession_Save(&session, format, path, a, a, &error));
  CHECK(!REMOVE_DIR(native_tmp));
  CheckDisk(format, path, a);
  ArRegionalSession saved_a = session;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_a));

  ArRegionalCostSnapshot quote;
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(quote.price[kArRegionalCost_Lightning] == 12);
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Miracles, kArRegionalSource_US));
  ArRegionalSession saved_b = session;
  /* Failure AFTER journaling must cold-load the OLD image's metadata. */
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!ArRegionalSession_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_a));
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(ArRegionalSession_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_b));
  /* Quitting an unsaved New Game leaves the saved campaign untouched. */
  CHECK(ArRegionalSession_NewGame(&session, 0, next_id, &defaults));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_b));
  CHECK(ArRegionalSession_Save(&session, format, path, b, c, &error));
  CheckDisk(format, path, c);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Scrolls, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTimers(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_Save(&session, format, path, c, c, &error));
  /* A metadata-only edit must not discard the preceding native checkpoint. */
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_b));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Mismatch);
  CHECK(EqualSession(&loaded, &saved_b));

  /* Same name/checksum is insufficient: compare bytes beyond the retail sum. */
  memcpy(a, c, sizeof(a)); a[0x1ff8] = 42;
  CHECK(Save_ChecksumValid(a) && Save_ComputeChecksum(a) == Save_ComputeChecksum(c));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Mismatch);

  uint8_t journal[26000], after[26000];
  size_t journal_size = ReadBytes(companion, journal, sizeof(journal));
  CHECK(journal_size > 8192);
  /* External save replacement cannot be overwritten using stale session data. */
  CHECK(Save_WriteFile(format, path, a, &error));
  CHECK(!ArRegionalSession_Save(&session, format, path, c, b, &error));
  CheckDisk(format, path, a);
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size);
  CHECK(!memcmp(journal, after, journal_size));
  CHECK(Save_WriteFile(format, path, c, &error));

  /* Future outer schema, corruption and truncation are NOT legacy Missing. */
  memcpy(after, journal, journal_size); after[8] = 2;
  CHECK(Save_WriteCompanionFile(companion, after, journal_size, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Unsupported);
  CHECK(!ArRegionalSession_Save(&session, format, path, c, c, &error));
  CheckDisk(format, path, c);
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size && after[8] == 2);
  memcpy(after, journal, journal_size); after[100] ^= 1;
  CHECK(Save_WriteCompanionFile(companion, after, journal_size, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Invalid);
  const size_t cuts[] = {1, 7, 12, 8192};
  for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
    CHECK(Save_WriteCompanionFile(companion, journal, cuts[i], &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Invalid);
    CHECK(!ArRegionalSession_Save(&session, format, path, c, c, &error));
  }
  CHECK(Save_WriteCompanionFile(companion, journal, journal_size, &error));

  /* Future feature schema inside a valid storage envelope also stays intact. */
  uint8_t payload[kSaveCheckpointPayloadMax], future[kSaveCheckpointPayloadMax];
  size_t payload_size = 0;
  CHECK(SaveCheckpoint_Read(path, c, payload, sizeof(payload), &payload_size, &error) == kSaveCheckpoint_Ready);
  memcpy(future, payload, payload_size); future[8] = 255;
  CHECK(SaveCheckpoint_Commit(format, path, c, c, future, payload_size, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Unsupported);
  CHECK(!ArRegionalSession_Save(&session, format, path, c, c, &error));
  /* Even a nonmatching retained payload cannot be silently erased on rotation. */
  CHECK(SaveCheckpoint_Commit(format, path, c, a, payload, payload_size, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  journal_size = ReadBytes(companion, journal, sizeof(journal));
  CHECK(!ArRegionalSession_Save(&session, format, path, a, b, &error));
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size);
  CHECK(!memcmp(journal, after, journal_size));
  CheckDisk(format, path, a);

  remove(path); remove(companion); remove(companion_tmp); remove(native_tmp);
  /* A new campaign may replace a pre-feature save with no companion. Failed
   * replacement must keep the old image recognizably legacy, not corrupt. */
  CHECK(Save_WriteFile(format, path, a, &error));
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!ArRegionalSession_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(ArRegionalSession_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  remove(path); remove(companion); remove(companion_tmp); remove(native_tmp);
}

static void CheckFeatureCodec(void) {
  const char *path = "actraiser-regional-codec-test.srm";
  const char *companion = "actraiser-regional-codec-test.srm.archeckpoint";
  remove(path); remove(companion);
  uint8_t image[kActRaiserSramSize]; Image(image, 7);
  const uint8_t id[16] = {7};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session, loaded;
  CHECK(ArRegionalSession_NewGame(&session, 2, id, &defaults));
  CHECK(ArRegionalLairHistory_InitTown(&session.lairs,2));
  CHECK(ArRegionalLairHistory_HouseLost(&session.lairs,2,0x20,1));
  CHECK(ArRegionalLairHistory_MarkDiverged(&session.lairs,2));
  CHECK(ArRegionalLairHistory_AdoptTown(&session.lairs,5,kArRegionalSource_US,(uint16_t[4]){0,1,301,65535}));
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Scrolls, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTimers(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestRetryScore(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestFishing(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestDevelopment(&session,session.revision,kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestRecovery(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestQuake(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestMenuReturn(&session, session.revision, kArRegionalSource_Japan));
  SaveError error;
  CHECK(ArRegionalSession_RequestSpeedRange(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestMagicGesture(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, NULL, image, &error));
  uint8_t original[kSaveCheckpointPayloadMax], mutated[kSaveCheckpointPayloadMax];
  size_t size = 0;
  CHECK(SaveCheckpoint_Read(path, image, original, sizeof(original), &size, &error) == kSaveCheckpoint_Ready);
  if (size < 36 || size >= sizeof(original)) return;
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    memcpy(mutated, original, size);
    size_t bytes = size;
    SaveCheckpointStatus expected = kSaveCheckpoint_Invalid;
    switch (mutation) {
      case 0: mutated[0] ^= 1; break; /* Wrong codec magic. */
      case 1: mutated[8] = 255; expected = kSaveCheckpoint_Unsupported; break;
      case 2: memset(mutated + 16, 0, 16); break; /* Missing campaign identity. */
      case 3: memset(mutated + 32, 0, 4); break; /* Invalid generation. */
      case 4: mutated[37 + mutated[36]] = 'x'; expected = kSaveCheckpoint_Unsupported; break;
      case 5: mutated[37 + mutated[36] + 4] ^= 1; expected = kSaveCheckpoint_Unsupported; break;
      case 6: --bytes; break;
      case 7: mutated[bytes++] = 0; break;
      case 8: { /* Duplicate the first named leaf in place of the second. */
        size_t first = original[36] + 9u, second = original[36 + first] + 9u;
        memcpy(mutated + 36 + first, original + 36, first);
        memcpy(mutated + 36 + first * 2, original + 36 + first + second,
               size - 36 - first - second);
        bytes = size + first - second;
        break;
      }
    }
    CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
                                mutated, bytes, AcceptOpaque, NULL, &error));
    loaded = session;
    CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == expected);
    CHECK(EqualSession(&loaded, &session));
    CHECK(!ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  }
  /* Reordering valid named fields is supported; they are not enum ordinals. */
  enum { records = kArRegionalCostRule_Count + kArRegionalTimerRule_Count + 14 + kArRegionalDevelopmentRule_Count + kArRegionalRecovery_Count + kArRegionalQuake_Count + kArRegionalLairCount + kArRegionalScore_Count + kArRegionalSourceItem_Count + kArRegionalStory_Count + kArRegionalTownStatus_Count + kArRegionalSimCombat_Count + kArRegionalSimAi_Count + kArRegionalSupport_Count };
  CHECK(ByteOrder_ReadLe16(original + 10) == records);
  size_t offsets[records], offset = 36;
  for (unsigned i = 0; i < records; ++i) {
    offsets[i] = offset;
    offset += original[offset] + 9u;
  }
  const size_t rules_end=offset;
  CHECK(offset + kArRegionalLairHistoryEncodedBytes + kArRegionalLairReloadEncodedBytes + kArRegionalSimActorsEncodedBytes + 9 == size);
  memcpy(mutated, original, 36); offset = 36;
  for (unsigned i = records; i-- > 0;) {
    size_t length = original[offsets[i]] + 9u;
    memcpy(mutated + offset, original + offsets[i], length);
    offset += length;
  }
  memcpy(mutated+offset,original+rules_end,size-rules_end);
  offset+=size-rules_end;
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
                              mutated, offset, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));

  /* Pricing-only checkpoints migrate in memory with US timer defaults, never
   * assuming that previous prices were a full regional preset. The old on-disk
   * record remains recoverable and can be retained beside a new save. */
  for (unsigned kind=0; kind<2; ++kind) {
    memcpy(mutated,original,size);
    const size_t seed_value=offsets[32]+1+original[offsets[32]];
    if (!kind) memcpy(mutated+seed_value,"eu",2); /* Same values, inconsistent table-wide source. */
    else mutated[seed_value+4]^=1; /* Table edits cannot silently change a retained policy. */
    CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
        mutated,size,AcceptOpaque,NULL,&error));
    loaded=session;
    CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==
        (kind?kSaveCheckpoint_Unsupported:kSaveCheckpoint_Invalid));
    CHECK(EqualSession(&loaded,&session));
  }
  const size_t price_bytes = offsets[kArRegionalCostRule_Count];
  memcpy(mutated, original, price_bytes);
  memcpy(mutated, "ARPRICE", 8);
  ByteOrder_WriteLe16(mutated + 8, 1);
  ByteOrder_WriteLe16(mutated + 10, kArRegionalCostRule_Count);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, price_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded.requested.costs, &session.requested.costs, sizeof(loaded.requested.costs)));
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i)
    CHECK(loaded.requested.timers.source[i] == kArRegionalSource_US &&
          loaded.effective.timers.source[i] == kArRegionalSource_US);
  CHECK(loaded.requested.retry_score == kArRegionalSource_US);
  /* Timer-era v1 preserves timer choices; only the new retry rule defaults. */
  const size_t v1_bytes = offsets[15];
  memcpy(mutated, original, v1_bytes);
  ByteOrder_WriteLe16(mutated + 8, 1);
  ByteOrder_WriteLe16(mutated + 10, 15);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v1_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded.requested.timers, &session.requested.timers, sizeof(loaded.requested.timers)));
  CHECK(loaded.requested.retry_score == kArRegionalSource_US &&
        loaded.effective.retry_score == kArRegionalSource_US);
  CHECK(loaded.requested.town_wait == kArRegionalSource_US && loaded.effective.town_wait == kArRegionalSource_US);
  /* Retry-era v2 retains its score rule but cannot imply town timing. */
  const size_t v2_bytes = offsets[16];
  memcpy(mutated, original, v2_bytes);
  ByteOrder_WriteLe16(mutated + 8, 2);
  ByteOrder_WriteLe16(mutated + 10, 16);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v2_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.retry_score == kArRegionalSource_Japan);
  CHECK(loaded.requested.town_wait == kArRegionalSource_US && loaded.effective.town_wait == kArRegionalSource_US);
  const size_t v3_bytes = offsets[17];
  memcpy(mutated,original,v3_bytes);
  ByteOrder_WriteLe16(mutated+8,3); ByteOrder_WriteLe16(mutated+10,17);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v3_bytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.town_wait == kArRegionalSource_Japan);
  CHECK(loaded.requested.fishing == kArRegionalSource_US && loaded.effective.fishing == kArRegionalSource_US);
  const size_t v4_bytes=offsets[18];memcpy(mutated,original,v4_bytes);
  ByteOrder_WriteLe16(mutated+8,4);ByteOrder_WriteLe16(mutated+10,18);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v4_bytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.fishing==kArRegionalSource_Japan);
  for(unsigned i=0;i<kArRegionalDevelopmentRule_Count;++i)
    CHECK(loaded.requested.development.source[i]==kArRegionalSource_US && loaded.effective.development.source[i]==kArRegionalSource_US);
  const size_t v5_bytes = offsets[21]; memcpy(mutated, original, v5_bytes);
  ByteOrder_WriteLe16(mutated + 8, 5); ByteOrder_WriteLe16(mutated + 10, 21);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v5_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.development.source[0] == kArRegionalSource_Japan);
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i)
    CHECK(loaded.requested.recovery.source[i] == kArRegionalSource_US &&
          loaded.effective.recovery.source[i] == kArRegionalSource_US);
  const size_t v6_bytes = offsets[23]; memcpy(mutated, original, v6_bytes);
  ByteOrder_WriteLe16(mutated + 8, 6); ByteOrder_WriteLe16(mutated + 10, 23);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v6_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.recovery.source[0] == kArRegionalSource_Japan);
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i)
    CHECK(loaded.requested.quake.source[i] == kArRegionalSource_US &&
          loaded.effective.quake.source[i] == kArRegionalSource_US);
  const size_t v7_bytes = offsets[28]; memcpy(mutated, original, v7_bytes);
  ByteOrder_WriteLe16(mutated + 8, 7); ByteOrder_WriteLe16(mutated + 10, 28);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v7_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.quake.source[0] == kArRegionalSource_Japan);
  CHECK(loaded.requested.score_page == kArRegionalSource_US && loaded.effective.score_page == kArRegionalSource_US);
  const size_t v8_bytes = offsets[29]; memcpy(mutated, original, v8_bytes);
  ByteOrder_WriteLe16(mutated + 8, 8); ByteOrder_WriteLe16(mutated + 10, 29);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v8_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.score_page == kArRegionalSource_Japan);
  CHECK(loaded.requested.menu_return == kArRegionalSource_US && loaded.effective.menu_return == kArRegionalSource_US);
  const size_t v9_bytes = offsets[30]; memcpy(mutated, original, v9_bytes);
  ByteOrder_WriteLe16(mutated + 8, 9); ByteOrder_WriteLe16(mutated + 10, 30);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v9_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.menu_return == kArRegionalSource_Japan);
  CHECK(loaded.requested.speed_range == kArRegionalSource_US && loaded.effective.speed_range == kArRegionalSource_US);
  const size_t v10_bytes=offsets[31]; memcpy(mutated,original,v10_bytes);
  ByteOrder_WriteLe16(mutated+8,10); ByteOrder_WriteLe16(mutated+10,31);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v10_bytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.speed_range==kArRegionalSource_Japan);
  CHECK(loaded.requested.magic_gesture==kArRegionalSource_US && loaded.effective.magic_gesture==kArRegionalSource_US);
  const size_t v11_bytes=offsets[32];
  memcpy(mutated,original,v11_bytes); ByteOrder_WriteLe16(mutated+8,11); ByteOrder_WriteLe16(mutated+10,32);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v11_bytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.magic_gesture==kArRegionalSource_Japan);
  CHECK(!loaded.lairs.initialized_towns && !loaded.lairs.approximate_towns);
  for(unsigned p=0;p<kArRegionalLairProjections;++p)
    for(unsigned n=0;n<kArRegionalLairCount;++n)CHECK(!loaded.lairs.stock[p][n]);
  /* v12 retains its canonical history but has no seed selector. */
  memcpy(mutated,original,v11_bytes); ByteOrder_WriteLe16(mutated+8,12); ByteOrder_WriteLe16(mutated+10,32);
  memcpy(mutated+v11_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v11_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.lair_seeds==kArRegionalSource_US && loaded.effective.lair_seeds==kArRegionalSource_US);
  CHECK(!memcmp(loaded.lairs.stock,session.lairs.stock,sizeof(session.lairs.stock)));
  const size_t v16_bytes=offsets[61];
  memcpy(mutated,original,v16_bytes); ByteOrder_WriteLe16(mutated+8,16); ByteOrder_WriteLe16(mutated+10,61);
  memcpy(mutated+v16_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v16_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.lives_display==kArRegionalSource_US && loaded.effective.lives_display==kArRegionalSource_US);
  CHECK(!memcmp(&loaded.requested.score_feedback,&session.requested.score_feedback,sizeof(session.requested.score_feedback)));
  CHECK(!memcmp(&loaded.lairs,&session.lairs,sizeof(session.lairs)));
  CHECK(ArRegionalSession_RequestLivesDisplay(&session,session.revision,kArRegionalSource_Japan));
  /* v13 retains seed policy/history and defaults only the new house rule. */
  const size_t v13_bytes=offsets[56];
  memcpy(mutated,original,v13_bytes); ByteOrder_WriteLe16(mutated+8,13); ByteOrder_WriteLe16(mutated+10,56);
  memcpy(mutated+v13_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v13_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.house_credit==kArRegionalSource_US && loaded.effective.house_credit==kArRegionalSource_US);
  CHECK(!memcmp(loaded.lairs.stock,session.lairs.stock,sizeof(session.lairs.stock)));
  const size_t v14_bytes=offsets[57];
  memcpy(mutated,original,v14_bytes); ByteOrder_WriteLe16(mutated+8,14); ByteOrder_WriteLe16(mutated+10,57);
  memcpy(mutated+v14_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v14_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  for(unsigned i=0;i<kArRegionalScore_Count;++i)
    CHECK(loaded.requested.score_feedback.source[i]==kArRegionalSource_US &&
          loaded.effective.score_feedback.source[i]==kArRegionalSource_US);
  const size_t v15_bytes=offsets[60];
  memcpy(mutated,original,v15_bytes); ByteOrder_WriteLe16(mutated+8,15); ByteOrder_WriteLe16(mutated+10,60);
  memcpy(mutated+v15_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v15_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.score_feedback.source[kArRegionalScore_Phase]==kArRegionalSource_US &&
        loaded.effective.score_feedback.source[kArRegionalScore_Phase]==kArRegionalSource_US);
  for(unsigned i=0;i<kArRegionalScore_Phase;++i) {
    CHECK(loaded.requested.score_feedback.source[i]==session.requested.score_feedback.source[i]);
    CHECK(loaded.effective.score_feedback.source[i]==session.effective.score_feedback.source[i]);
  }
  CHECK(!memcmp(loaded.lairs.stock,session.lairs.stock,sizeof(session.lairs.stock)));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  const size_t v17_bytes=offsets[62];
  memcpy(mutated,original,v17_bytes); ByteOrder_WriteLe16(mutated+8,17); ByteOrder_WriteLe16(mutated+10,62);
  memcpy(mutated+v17_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v17_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  for(unsigned i=0;i<kArRegionalSourceItem_Count;++i)
    CHECK(loaded.requested.sources.source[i]==kArRegionalSource_US && loaded.effective.sources.source[i]==kArRegionalSource_US);
  CHECK(!memcmp(&loaded.lairs,&session.lairs,sizeof(session.lairs)));
  ArRegionalSourcesPolicy sources={{kArRegionalSource_Japan,kArRegionalSource_Europe}};
  const size_t v18_bytes=offsets[64];
  memcpy(mutated,original,v18_bytes); ByteOrder_WriteLe16(mutated+8,18); ByteOrder_WriteLe16(mutated+10,64);
  memcpy(mutated+v18_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v18_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.skull_wait==kArRegionalSource_US && loaded.effective.skull_wait==kArRegionalSource_US);
  CHECK(!memcmp(&loaded.lairs,&session.lairs,sizeof(session.lairs)));
  CHECK(ArRegionalSession_RequestSkullWait(&session,session.revision,kArRegionalSource_Japan));
  uint16_t frames;
  CHECK(ArRegionalSession_BeginSkullWait(&session,&frames) && !frames);
  CHECK(ArRegionalSession_RequestSkullWait(&session,session.revision,kArRegionalSource_Europe));
  const size_t v19_bytes=offsets[65];
  memcpy(mutated,original,v19_bytes);ByteOrder_WriteLe16(mutated+8,19);ByteOrder_WriteLe16(mutated+10,65);
  memcpy(mutated+v19_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v19_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  for(unsigned i=0;i<kArRegionalStory_Count;++i)
    CHECK(loaded.requested.story.source[i]==kArRegionalSource_US && loaded.effective.story.source[i]==kArRegionalSource_US);
  CHECK(!memcmp(&loaded.lairs,&session.lairs,sizeof(session.lairs)));
  ArRegionalStoryPolicy story={{1,2,0}};ArRegionalStorySnapshot story_snapshot;
  const size_t v20_bytes=offsets[68];
  memcpy(mutated,original,v20_bytes);ByteOrder_WriteLe16(mutated+8,20);ByteOrder_WriteLe16(mutated+10,68);
  memcpy(mutated+v20_bytes,original+rules_end,kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v20_bytes+kArRegionalLairHistoryEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.lair_reloads==kArRegionalSource_US && loaded.effective.lair_reloads==kArRegionalSource_US &&
      !loaded.reloads.initialized_towns);
  CHECK(!memcmp(&loaded.lairs,&session.lairs,sizeof(session.lairs)));
  const size_t v21_bytes=offsets[69];
  memcpy(mutated,original,v21_bytes);ByteOrder_WriteLe16(mutated+8,21);ByteOrder_WriteLe16(mutated+10,69);
  const size_t old_history_size=kArRegionalLairHistoryEncodedBytes+kArRegionalLairReloadEncodedBytes;
  memcpy(mutated+v21_bytes,original+rules_end,old_history_size);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v21_bytes+old_history_size,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  for(unsigned i=0;i<kArRegionalTownStatus_Count;++i)
    CHECK(loaded.requested.town_status.source[i]==kArRegionalSource_US && loaded.effective.town_status.source[i]==kArRegionalSource_US);
  CHECK(!memcmp(&loaded.lairs,&session.lairs,sizeof(session.lairs)) && !memcmp(&loaded.reloads,&session.reloads,sizeof(session.reloads)));
  const size_t v22_bytes=offsets[74];
  memcpy(mutated,original,v22_bytes);ByteOrder_WriteLe16(mutated+8,22);ByteOrder_WriteLe16(mutated+10,74);
  memcpy(mutated+v22_bytes,original+rules_end,old_history_size);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v22_bytes+old_history_size,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.level_goals==0 && loaded.effective.level_goals==0);
  const size_t v23_bytes=offsets[75];
  memcpy(mutated,original,v23_bytes);ByteOrder_WriteLe16(mutated+8,23);ByteOrder_WriteLe16(mutated+10,75);
  memcpy(mutated+v23_bytes,original+rules_end,old_history_size);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v23_bytes+old_history_size,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  uint16_t combat;
  CHECK(ArRegionalSimCombat_Resolve(&loaded.requested.sim_combat,&combat) && !combat);
  CHECK(!loaded.sim_actors.active_town_tag && !loaded.sim_actors.cached[0].combat);
  const size_t v24_bytes=offsets[80];
  memcpy(mutated,original,v24_bytes);ByteOrder_WriteLe16(mutated+8,24);ByteOrder_WriteLe16(mutated+10,80);
  memcpy(mutated+v24_bytes,original+rules_end,old_history_size);
  const ArRegionalSimActors old_actors={.cached={{.combat=31}},.active={{.combat=7}},.active_town_tag=1};
  CHECK(ArRegionalSimActors_EncodeVersion(&old_actors,mutated+v24_bytes+old_history_size,
      sizeof(mutated)-v24_bytes-old_history_size,1));
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v24_bytes+old_history_size+kArRegionalSimActorsV1EncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.sim_actors.cached[0].combat==31 && loaded.sim_actors.active[0].combat==7 && !loaded.sim_actors.active[0].ai);
  CHECK(ArRegionalSimAi_Resolve(&loaded.requested.sim_ai,&combat) && !combat);
  CHECK(!memcmp(&loaded.lairs,&session.lairs,sizeof(session.lairs)) && !memcmp(&loaded.reloads,&session.reloads,sizeof(session.reloads)));
  const size_t v25_bytes=offsets[86];
  memcpy(mutated,original,v25_bytes);ByteOrder_WriteLe16(mutated+8,25);ByteOrder_WriteLe16(mutated+10,86);
  memcpy(mutated+v25_bytes,original+rules_end,old_history_size+kArRegionalSimActorsEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v25_bytes+old_history_size+kArRegionalSimActorsEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.construction==0 && loaded.effective.construction==0);
  const size_t v26_bytes=offsets[87];
  memcpy(mutated,original,v26_bytes);ByteOrder_WriteLe16(mutated+8,26);ByteOrder_WriteLe16(mutated+10,87);
  memcpy(mutated+v26_bytes,original+rules_end,old_history_size+kArRegionalSimActorsEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v26_bytes+old_history_size+kArRegionalSimActorsEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  for(unsigned i=0;i<5;++i) CHECK(loaded.requested.support.source[i]==0 && loaded.effective.support.source[i]==0);
  CHECK(!memcmp(&loaded.lairs,&session.lairs,sizeof(session.lairs)) && !memcmp(&loaded.reloads,&session.reloads,sizeof(session.reloads)));
  CHECK(!memcmp(&loaded.sim_actors,&session.sim_actors,sizeof(session.sim_actors)));
  const size_t v27_bytes=offsets[92];
  memcpy(mutated,original,v27_bytes);ByteOrder_WriteLe16(mutated+8,27);ByteOrder_WriteLe16(mutated+10,92);
  memcpy(mutated+v27_bytes,original+rules_end,old_history_size+kArRegionalSimActorsEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v27_bytes+old_history_size+kArRegionalSimActorsEncodedBytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(!loaded.arrival_locked && !loaded.requested.arrival && !loaded.effective.arrival);
  CHECK(ArRegionalSession_RequestStory(&session,session.revision,&story));
  CHECK(ArRegionalSession_BeginStory(&session,&story_snapshot));
  story.source[2]=1;CHECK(ArRegionalSession_RequestStory(&session,session.revision,&story));
  ArRegionalSourcesSnapshot sources_snapshot;
  CHECK(ArRegionalSession_RequestSources(&session,session.revision,&sources));
  CHECK(ArRegionalSession_RequestArrival(&session,session.revision,kArRegionalSource_Japan));
  bool japanese;CHECK(ArRegionalSession_BeginArrival(&session,false,&japanese) && japanese);
  CHECK(ArRegionalSession_RequestArrival(&session,session.revision,kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginSources(&session,&sources_snapshot));
  sources.source[1]=kArRegionalSource_Japan;
  CHECK(ArRegionalSession_RequestSources(&session,session.revision,&sources));
  CHECK(ArRegionalSession_Save(&session,kSaveFileFormat_NativeSrm,path,image,image,&error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));
  remove(path); remove(companion);
}

int main(void) {
  CheckArrival();
  CheckPopulation();
  CheckConstruction();
  CheckSimCombat();
  CheckLevelGoals();
  CheckTownStatus();
  CheckLairReloads();
  CheckScoreFeedback();
  CheckLairSeeds();
  CheckMagicGestureActivation();
  CheckScorePageActivation();
  CheckLivesDisplayActivation();
  CheckSourcesActivation();
  CheckSkullWait();
  CheckStory();
  CheckMenuReturnActivation();
  CheckSpeedRangeActivation();
  CheckQuakeActivation();
  CheckSessionState();
  CheckPersistence(kSaveFileFormat_NativeSrm, "actraiser-regional-session-test.srm");
  CheckPersistence(kSaveFileFormat_Ini, "actraiser-regional-session-test.ini");
  CheckFeatureCodec();
  if (failures) fprintf(stderr, "%d regional session failures\n", failures);
  else puts("regional sessions: activation, cold load, recovery and native/INI checkpoint tests passed");
  return failures ? 1 : 0;
}
