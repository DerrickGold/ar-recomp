/* Required contract for every native scene-effect family.
 * SCENE_RULE(name, kind, matcher, phase filter (None = any), room,
 *            ownership, dependency validator, clock)
 * Record: own record only. Spawn: validate dependency once per generation.
 * Attached: validate dependency every capture. Matchers receive NO WRAM.
 * Motion clocks freeze an unchanged position; Gameplay clocks allow holds.
 * Adding a row also requires a lifecycle fixture: the test checks every ID. */
SCENE_RULE(Centaur, CentaurLightning, MatchCentaurLightning, None, Centaur,
           Record, NULL, Gameplay)
SCENE_RULE(NorthwallMagic, NorthwallBossMagic, MatchNorthwallBossMagic, None, NorthwallMagic,
           Record, NULL, Gameplay)
SCENE_RULE(MinotaurAxe, MinotaurAxe, MatchMinotaurAxe, None, Minotaur,
           Spawn, MinotaurAxeParentIsValid, Motion)
SCENE_RULE(FlamingWheelBody, FlamingWheel, MatchFlamingWheel, None, FlamingWheel,
           Record, NULL, Gameplay)
SCENE_RULE(FlamingWheelShot, FlamingWheelProjectile, MatchFlamingWheelProjectile,
           None, FlamingWheel,
           Spawn, FlamingWheelProjectileParentIsValid, Motion)
SCENE_RULE(IceDragonBall, IceDragonIceBall, MatchIceDragonIceBall, None, IceDragon,
           Spawn, IceDragonIceBallParentIsValid, Motion)
SCENE_RULE(Tanzara, TanzaraProjectile, MatchTanzaraProjectile, None, Tanzara,
           Record, NULL, Gameplay)
SCENE_RULE(BloodpoolBossFireball, EnemyFireball, MatchBloodpoolAct1BossFireball,
           None, BloodpoolAct1,
           Spawn, BloodpoolAct1BossFireballParentIsValid, Motion)
SCENE_RULE(BloodpoolFireball, EnemyFireball, MatchEnemyFireball, None, BloodpoolAct2,
           Record, NULL, Motion)
SCENE_RULE(FillmoreOrb, FillmoreStatueOrb, MatchFillmoreStatueOrb, None, FillmoreStatue,
           Spawn, FillmoreStatueOrbParentIsValid, Motion)
SCENE_RULE(MarahnaOrb, MarahnaFireball, MatchMarahnaFireball, MarahnaFireballOrb, Marahna,
           Record, NULL, Gameplay)
SCENE_RULE(MarahnaSplit, MarahnaFireball, MatchMarahnaFireball, MarahnaFireballSplit, Marahna,
           Spawn, MarahnaFireballSplitParentIsValid, Motion)
SCENE_RULE(MarahnaSnake, MarahnaFireball, MatchMarahnaFireball, MarahnaSnakeFireballShot, Marahna,
           Spawn, MarahnaSnakeFireballParentIsValid, Motion)
SCENE_RULE(MarahnaLink, MarahnaLightningLink, MatchMarahnaLightningLink, None, Marahna,
           Attached, MarahnaLightningLinkEndpointsAreValid, Gameplay)
SCENE_RULE(ViperBody, MarahnaBossLightning, MatchMarahnaBossBody, None, Viper,
           Record, NULL, Gameplay)
SCENE_RULE(ViperBolt, MarahnaBossLightning, MatchMarahnaBossLightning,
           MarahnaBossLightningBolt, Viper,
           Spawn, MarahnaBossLightningParentIsValid, Motion)
SCENE_RULE(ViperGround, MarahnaBossLightning, MatchMarahnaBossLightning,
           MarahnaBossLightningGroundCharge, Viper,
           Spawn, MarahnaBossLightningParentIsValid, Motion)
SCENE_RULE(AitosLava, AitosLavaFireball, MatchAitosLavaFireball, None, AitosLava,
           Record, NULL, Gameplay)
SCENE_RULE(AitosStatue, AitosStatueFire, MatchAitosStatueFire, None, AitosStatue,
           Record, NULL, Gameplay)
SCENE_RULE(AitosRock, AitosMoltenRock, MatchAitosMoltenRock, None, AitosLava,
           Record, NULL, Motion)
SCENE_RULE(AitosSword, SwordBeam, MatchAitosBossSwordBeam, None, AitosBoss,
           Spawn, AitosBossSwordBeamParentIsValid, Motion)
SCENE_RULE(PlayerSword, SwordBeam, MatchPlayerSwordBeam, None, Any,
           Spawn, PlayerSwordBeamParentIsValid, Motion)
SCENE_RULE(BloodpoolTrap, LightningTrap, MatchLightningTrap, None, BloodpoolAct2,
           Record, NULL, Gameplay)
SCENE_RULE(WizardLightning, BloodpoolBossLightning, MatchBloodpoolBossLightning, None, Wizard,
           Spawn, BloodpoolBossLightningParentIsValid, Gameplay)
