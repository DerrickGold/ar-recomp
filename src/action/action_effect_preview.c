#include "action_effect_preview.h"
#include <limits.h>
static const uint8_t kKinds[]={
  kActionEffect_EnemyFireball,kActionEffect_FillmoreStatueOrb,kActionEffect_LightningTrap,
  kActionEffect_SwordBeam,kActionEffect_MarahnaFireball,kActionEffect_MarahnaLightningLink,
  kActionEffect_MarahnaBossLightning,kActionEffect_AitosLavaFireball,kActionEffect_AitosStatueFire,
  kActionEffect_AitosMoltenRock,kActionEffect_MinotaurAxe,kActionEffect_FlamingWheel,
  kActionEffect_FlamingWheelProjectile,kActionEffect_IceDragonIceBall,kActionEffect_TanzaraProjectile,
  kActionEffect_CentaurLightning,kActionEffect_BloodpoolBossLightning,kActionEffect_NorthwallBossMagic,
  kActionEffect_LandingDust};
unsigned ActionEffectPreview_Count(void){return sizeof(kKinds)/sizeof(kKinds[0]);}
unsigned ActionEffectPreview_Kind(unsigned index){return index<ActionEffectPreview_Count()?kKinds[index]:0;}
bool ActionEffectPreview_Build(const ActionEffectPreviewEvent *event,uint16_t clock,ActionEffectInstance *out) {
  if(!event||!out||event->duration<1||event->duration>4096 ||
      event->velocity_x<-16||event->velocity_x>16||event->velocity_y<-16||event->velocity_y>16)return false;
  const unsigned age=(uint16_t)(clock-event->start);
  if(age>=event->duration)return false;
  ActionEffectInstance e={.kind=event->kind,.generation=event->seed,.pulse_generation=event->seed,
    .world_x=event->x,.world_y=event->y,.velocity_x=event->velocity_x,.velocity_y=event->velocity_y,
    .age_ticks=age,.phase_ticks=age,.pulse_ticks=age,.visual=0,.obj_priority=2,
    .role=kActionEffectRole_Body,.flags=kActionEffectFlag_Visible,
    .phase=kActionEffectPhase_EnemyFireballFlight,.render_layer=kActionEffectRenderLayer_WorldOverlay,
    .projection_plane=kActionEffectProjectionPlane_Obj,
    .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-8,-8,8,8}}};
  bool flight=false;
  switch(event->kind) {
    case kActionEffect_EnemyFireball:flight=true;break;
    case kActionEffect_FillmoreStatueOrb:e.visual=0x1b;flight=true;break;
    case kActionEffect_LightningTrap:e.phase=kActionEffectPhase_LightningActive;e.visual=0x1f;e.geometry.data.rect=(ActionEffectLocalRect){0,-88,8,88};break;
    case kActionEffect_SwordBeam:e.phase=kActionEffectPhase_SwordBeamFlight;e.visual=0x30;e.geometry.data.rect=(ActionEffectLocalRect){32,-33,48,-1};flight=true;break;
    case kActionEffect_MarahnaFireball:e.phase=kActionEffectPhase_MarahnaFireballOrb;e.visual=8;flight=true;break;
    case kActionEffect_MarahnaLightningLink:e.phase=kActionEffectPhase_MarahnaLightningActive;e.visual=0x2e;e.animation_state=0x27;e.geometry.data.rect=(ActionEffectLocalRect){-40,-4,40,4};break;
    case kActionEffect_MarahnaBossLightning:e.phase=kActionEffectPhase_MarahnaBossLightningBolt;e.visual=0x11;e.geometry.data.rect=(ActionEffectLocalRect){-32,0,0,32};flight=true;break;
    case kActionEffect_AitosLavaFireball:e.phase=kActionEffectPhase_AitosLavaFireballFlight;flight=true;break;
    case kActionEffect_AitosStatueFire:e.phase=kActionEffectPhase_AitosStatueFireBreath;e.visual=0x1e;e.geometry.data.rect=(ActionEffectLocalRect){-16,-8,48,8};break;
    case kActionEffect_AitosMoltenRock:e.phase=kActionEffectPhase_AitosMoltenRockFlight;e.visual=0x2b;flight=true;break;
    case kActionEffect_MinotaurAxe:e.phase=kActionEffectPhase_MinotaurAxeFlight;flight=true;break;
    case kActionEffect_FlamingWheel:e.phase=kActionEffectPhase_FlamingWheelBody;e.visual=0xf;e.composition=0x5276;e.geometry.data.rect=(ActionEffectLocalRect){-32,-32,32,32};break;
    case kActionEffect_FlamingWheelProjectile:e.phase=kActionEffectPhase_FlamingWheelProjectileFlight;e.composition=0x51b5;flight=true;break;
    case kActionEffect_IceDragonIceBall:e.phase=kActionEffectPhase_IceDragonIceBallFlight;e.visual=0x12;flight=true;break;
    case kActionEffect_TanzaraProjectile:e.phase=kActionEffectPhase_TanzaraProjectileFlight;e.visual=0x16;flight=true;break;
    case kActionEffect_CentaurLightning:e.phase=kActionEffectPhase_BossLightningStrike;e.visual=0x20;e.geometry.data.rect=(ActionEffectLocalRect){-64,-1,8,111};break;
    case kActionEffect_BloodpoolBossLightning:e.phase=kActionEffectPhase_BossLightningStrike;e.visual=5;e.geometry.data.rect=(ActionEffectLocalRect){-30,-83,8,21};break;
    case kActionEffect_NorthwallBossMagic:e.phase=kActionEffectPhase_NorthwallMagicCharge;e.visual=0xa;break;
    case kActionEffect_LandingDust:
      if(age>=kActionLandingDustLifetime)return false;
      e.phase=kActionEffectPhase_CaveEnvironment;e.dust_strength=2;
      e.projection_plane=kActionEffectProjectionPlane_Bg1;e.render_layer=kActionEffectRenderLayer_WorldDust;
      e.geometry.data.rect=(ActionEffectLocalRect){-32,-24,32,0};break;
    default:return false;
  }
  const int x=e.world_x+(flight?(int)age*event->velocity_x:0),y=e.world_y+(flight?(int)age*event->velocity_y:0);
  if(x<INT16_MIN||x>INT16_MAX||y<INT16_MIN||y>INT16_MAX)return false;
  e.world_x=x;e.world_y=y;*out=e;return true;
}
