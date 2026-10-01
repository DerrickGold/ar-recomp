#ifndef AR_ACTION_LIGHT_KINDS_H
#define AR_ACTION_LIGHT_KINDS_H
#include "action_effects.h"
/* Families that actually submit lighting geometry. Keep data validation and
 * receiver sampling on the same catalog; particle-only sources stay excluded. */
static inline bool ActionLightKind_Supported(unsigned kind) {
  switch(kind) {
    case kActionEffect_AuthoredLight:case kActionEffect_AuthoredFan:case kActionEffect_WallTorch:
    case kActionEffect_ForestCanopyLight:case kActionEffect_ForestForwardLight:
    case kActionEffect_CaveSheen:case kActionEffect_CaveAmbientLight:case kActionEffect_TowerWindowLight:
    case kActionEffect_BloodpoolMoonlight:case kActionEffect_BloodpoolWater:case kActionEffect_BloodpoolTimber:
    case kActionEffect_CastleLight:case kActionEffect_CastleSky:
    case kActionEffect_AitosLavaPit:case kActionEffect_AitosLavaReservoir:
    case kActionEffect_AitosWaterSplash:case kActionEffect_AitosWaterfall:case kActionEffect_AitosWaterfallMist:
    case kActionEffect_EnemyFireball:case kActionEffect_FillmoreStatueOrb:case kActionEffect_LightningTrap:
    case kActionEffect_SwordBeam:case kActionEffect_MarahnaFireball:case kActionEffect_MarahnaLightningLink:
    case kActionEffect_MarahnaBossLightning:case kActionEffect_AitosLavaFireball:case kActionEffect_AitosStatueFire:
    case kActionEffect_AitosMoltenRock:case kActionEffect_MinotaurAxe:case kActionEffect_FlamingWheel:
    case kActionEffect_FlamingWheelProjectile:case kActionEffect_IceDragonIceBall:case kActionEffect_TanzaraProjectile:
    case kActionEffect_CentaurLightning:case kActionEffect_BloodpoolBossLightning:case kActionEffect_NorthwallBossMagic:return true;
    default:return false;
  }
}
#endif
