/* Required contract for every native scene-effect family.
 * SCENE_RULE(name, kind, matcher, phase filter (None = any), room,
 *            ownership, dependency validator, clock, predecessor)
 * Record: own record only. Spawn: validate dependency once per generation.
 * Attached: validate dependency every capture. Matchers receive NO WRAM.
 * Motion clocks freeze an unchanged position; Gameplay clocks allow holds.
 * Predecessor declares a same-actor phase transition that retains admission.
 * Adding a row or transition requires a lifecycle fixture; tests check both. */
SCENE_RULE(Centaur, CentaurLightning, MatchCentaurLightning, None, Centaur,
           Record, NULL, Gameplay, None)
SCENE_RULE(NorthwallMagic, NorthwallBossMagic, MatchNorthwallBossMagic, None, NorthwallMagic,
           Record, NULL, Gameplay, None)
SCENE_RULE(MinotaurAxe, MinotaurAxe, MatchMinotaurAxe, None, Minotaur,
           Spawn, MinotaurAxeParentIsValid, Motion, None)
SCENE_RULE(FlamingWheelBody, FlamingWheel, MatchFlamingWheel, None, FlamingWheel,
           Record, NULL, Gameplay, None)
SCENE_RULE(FlamingWheelShot, FlamingWheelProjectile, MatchFlamingWheelProjectile,
           None, FlamingWheel,
           Spawn, FlamingWheelProjectileParentIsValid, Motion, None)
SCENE_RULE(IceDragonBall, IceDragonIceBall, MatchIceDragonIceBall, None, IceDragon,
           Spawn, IceDragonIceBallParentIsValid, Motion, None)
SCENE_RULE(Tanzara, TanzaraProjectile, MatchTanzaraProjectile, None, Tanzara,
           Record, NULL, Gameplay, None)
SCENE_RULE(BloodpoolBossFireball, EnemyFireball, MatchBloodpoolAct1BossFireball,
           None, BloodpoolAct1,
           Spawn, BloodpoolAct1BossFireballParentIsValid, Motion, None)
SCENE_RULE(BloodpoolFireball, EnemyFireball, MatchEnemyFireball, None, BloodpoolAct2,
           Record, NULL, Motion, None)
SCENE_RULE(FillmoreOrb, FillmoreStatueOrb, MatchFillmoreStatueOrb, None, FillmoreStatue,
           Spawn, FillmoreStatueOrbParentIsValid, Motion, None)
SCENE_RULE(MarahnaOrb, MarahnaFireball, MatchMarahnaFireball, MarahnaFireballOrb, Marahna,
           Record, NULL, Gameplay, None)
SCENE_RULE(MarahnaSplit, MarahnaFireball, MatchMarahnaFireball, MarahnaFireballSplit, Marahna,
           Spawn, MarahnaFireballSplitParentIsValid, Motion, None)
SCENE_RULE(MarahnaSnake, MarahnaFireball, MatchMarahnaFireball, MarahnaSnakeFireballShot, Marahna,
           Spawn, MarahnaSnakeFireballParentIsValid, Motion, None)
SCENE_RULE(MarahnaLink, MarahnaLightningLink, MatchMarahnaLightningLink, None, Marahna,
           Attached, MarahnaLightningLinkEndpointsAreValid, Gameplay, None)
SCENE_RULE(ViperBody, MarahnaBossLightning, MatchMarahnaBossBody, None, Viper,
           Record, NULL, Gameplay, None)
SCENE_RULE(ViperBolt, MarahnaBossLightning, MatchMarahnaBossLightning,
           MarahnaBossLightningBolt, Viper,
           Spawn, MarahnaBossLightningParentIsValid, Motion, None)
SCENE_RULE(ViperGround, MarahnaBossLightning, MatchMarahnaBossLightning,
           MarahnaBossLightningGroundCharge, Viper,
           Spawn, MarahnaBossLightningParentIsValid, Motion, ViperBolt)
SCENE_RULE(AitosLava, AitosLavaFireball, MatchAitosLavaFireball, None, AitosLava,
           Record, NULL, Gameplay, None)
SCENE_RULE(AitosStatue, AitosStatueFire, MatchAitosStatueFire, None, AitosStatue,
           Record, NULL, Gameplay, None)
SCENE_RULE(AitosRock, AitosMoltenRock, MatchAitosMoltenRock, None, AitosLava,
           Record, NULL, Motion, None)
SCENE_RULE(AitosSword, SwordBeam, MatchAitosBossSwordBeam, None, AitosBoss,
           Spawn, AitosBossSwordBeamParentIsValid, Motion, None)
SCENE_RULE(PlayerSword, SwordBeam, MatchPlayerSwordBeam, None, Any,
           Spawn, PlayerSwordBeamParentIsValid, Motion, None)
SCENE_RULE(BloodpoolTrap, LightningTrap, MatchLightningTrap, None, BloodpoolAct2,
           Record, NULL, Gameplay, None)
SCENE_RULE(WizardLightning, BloodpoolBossLightning, MatchBloodpoolBossLightning, None, Wizard,
           Spawn, BloodpoolBossLightningParentIsValid, Gameplay, None)
