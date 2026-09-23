# ActRaiser: regional differences

This article compares ActRaiser's US, Japanese, European English, German and
French releases, using the US game as the baseline. It covers action rules,
individual stages, town development, menus and presentation.

**Europe** in a comparison table means all three checked European releases.
They are grouped where the evidence agrees; language-specific differences
are named separately. A US/Japan-only comparison does not imply that Europe
matches either one.

Findings come from ROM analysis and tests of the original game unless marked
**reported** or **under investigation**. This is not a list of regional
options already implemented in ActRaiser Recompiled. Story and boss spoilers
follow.

## Contents

- [At a glance](#at-a-glance)
- [Which differences affect play?](#which-differences-affect-play)
- [Releases, modes and timing](#releases-modes-and-timing)
- [Action rules](#action-mode)
  - [Magic](#casting-magic)
  - [Lives, retries and score](#lives-continues-and-score)
  - [European difficulty](#european-difficulty)
  - [European Action Mode items](#european-action-mode-items-and-magic)
- [Action stages](#a-tour-of-the-action-stages)
  - [Fillmore](#fillmore) · [Bloodpool](#bloodpool) · [Kasandora](#kasandora)
  - [Aitos](#aitos) · [Marahna](#marahna) · [Northwall](#northwall) · [Death Heim](#death-heim)
- [Simulation mode](#simulation-mode)
  - [Development and recovery](#town-development-and-recovery)
  - [Miracles and rebuilding](#miracle-costs)
  - [Population and levels](#housing-and-population-support)
  - [Documented population maxima](#documented-population-maxima)
  - [Population-triggered story events](#population-prerequisites-for-story-events)
  - [Lairs and score rewards](#monster-lairs-and-sealing)
  - [Monsters](#simulation-enemy-combat-and-behavior)
  - [Offerings](#offerings-and-the-magic-skull)
  - [Town story events](#town-story-events)
- [Menus, story and presentation](#sky-palace-story-graphics-and-music)
- [What remains unverified](#what-remains-unverified)
- [About the research](#about-the-research)

## At a glance

| Feature | US | Japan | Europe |
| --- | --- | --- | --- |
| Action-only mode name | Professional! | Special | Action Mode |
| Action-only starting attempts / health | 5 / 24 HP | 3 / 24 HP | 5 / 8 HP |
| Action magic inventory | Generic scrolls; one per cast | Generic scrolls; one to four per cast | Story: one scroll per cast. Action: individual spells, newest first |
| Score on checkpoint retry | Retained | Cleared | Retained in the tested Action Mode retry |
| Routine town development | More frequent work than Japan | Slower schedule and some longer waits | Checked schedule uses US service counts; PAL timing still applies |
| Construction-cycle SP recovery | One tenth of maximum SP | No periodic refill | US amount |
| Angel-health recovery | One quarter of maximum health per construction cycle | Small, regular increments between cycles | US construction-cycle amount |
| Earthquake and houses | Highest tier protected | Random destruction, including upgraded houses | US house-tier rule |
| Ordinary / upgraded field support | 32 / 48 | 16 / 24 | 32 / 48 |
| Final level's population requirement | 4,600 | 3,000 | 4,600 |
| Initial lair reserves | Generally lower than Japan | Generally higher | US starting tables |
| Sources of Life and Magic | Activate when collected | Carried until used | Activate when collected |

These are the checked rules, not a single ranking of regional difficulty.
Europe also has its own enemy stats, hazards, boss behavior and selectable
difficulty. Its rules are not simply the US game running at a lower frame rate.

## Which differences affect play?

The differences fall into several categories. Their impact is not measured
by how much ROM data changed or how noticeable a new picture is.

| Kind of difference | Examples | Effect on the game |
| --- | --- | --- |
| Rules and challenge | Stage geometry, enemy placements, damage, hitboxes, attack choices, magic costs, item effects and lives | Changes what the player can do, encounter or survive |
| Development and progression | Town schedules, support, population prerequisites, recovery, lair reserves and score rewards | Changes resource availability, waiting and access to later events or levels |
| Interaction and information | Magic controls, menu returns, score-page access and city-report classification | Changes how the game is operated or explained; some also affect when simulation resumes |
| Presentation only | Text, music, clothing, symbols and validated visual-only pose changes | Changes what the player sees or hears while leaving the checked rules intact; readability and appearance can still matter |
| Internal or unestablished | Relocated data, unowned animation entries and state differences without a demonstrated gameplay consequence | Does not by itself establish a player-facing feature or difficulty difference |

Some changes cross these boundaries. Kasandora's bird-headed swordsmen have
both a redraw and altered sword collision extents. Marahna's arrows differ
in appearance and speed. Other animation edits change attack timing or the
collision footprint of a pose. Those gameplay effects are distinct from the
artwork, even when the original ROM stores them together.

Likewise, Death Heim's reveal is visual, but the Japanese route also changes
when the island is unlocked and announced. Town-menu behavior can keep the
simulation paused after a report closes. Neither should be treated as a
purely cosmetic preference.

Language, presentation and rules are therefore separate aspects of a regional
release. The Japanese names do not require Japanese combat or town rules,
and the European translations do not imply identical graphics or native
timing. Shared behaviors and unsupported reports are identified in their
respective sections rather than counted as additional regional features.

## Releases, modes and timing

The European English, German and French releases offer Story Mode and Action
Mode immediately, without a completed-game save. Both offer Beginner, Normal
and Expert, with Normal selected initially. Story saves retain the chosen
difficulty; Continue restores it and its timer rate without asking again.

| Release | Approximate native refresh rate |
| --- | ---: |
| US | 60 frames/s |
| Japan | 60 frames/s |
| European English | 50 frames/s |
| German | 50 frames/s |
| French | 50 frames/s |

Throughout this article, **frames** and **updates** describe active game
timing, excluding pauses. They are not interchangeable with seconds.
An unchanged frame count takes about 20% longer in Europe. Shortening an
animation can compensate for that difference, but a change in its movement,
damage or attack choices is a separate matter. Matching durations alone
does not establish the developers' intent.

Evidence: [European modes and difficulty](regional-differences-technical.md#european-difficulty-and-placement-contracts),
[Story save and Continue](regional-differences-technical.md#european-story-save-and-continue),
[PAL timing interpretation](regional-differences-technical.md#pal-timing-interpretation).

The retained debugging tools also differ. US and Japan contain an action-debug
block for scene stepping, coordinate display and other controls. That block
and its supporting tables are absent at the corresponding location in all
three European ROMs, although their Music Mode sound test remains. An
alternative European activation path has not been established. See
[debug facilities](unused-content.md#debugging-facilities) for the controls
and the distinction between controlled tests and original activation paths.

## Action mode

### Casting magic

With the original controls, the Japanese game casts magic through Up plus
the attack button. The US game accepts A or X for magic, separating casting
from sword attacks while holding Up.

The scroll costs are:

| Spell | US | Japan | European Story Mode |
| --- | ---: | ---: | ---: |
| Magical Fire | 1 | 1 | 1 |
| Stardust | 1 | 2 | 1 |
| Aura | 1 | 3 | 1 |
| Light | 1 | 4 | 1 |

European Action Mode uses an inventory of individual spells instead of this
scroll-price model; see [items and magic](#european-action-mode-items-and-magic).

In Japan, the more expensive spells consume reserves that could otherwise
fund several casts of Magical Fire.

### Lives, continues, and score

The lives display uses different conventions. The US number includes the
current attempt; Japan displays the spare attempts. A Japanese display of
zero therefore still accompanies a playable last attempt.

On an action checkpoint retry, the US game keeps the accumulated score while
Japan clears it. When the player runs out of attempts in normal play, the
tested return to the Sky Palace preserves the score in both versions;
departing to fight again clears it. Restarting after the action-only mode's
Game Over starts a fresh score in both versions.

### Apples: healing and placement

A half apple restores one quarter of maximum health, rounded down, in the
tested US and Japanese releases. A whole apple restores the missing health.
For example, a half apple restores six points when maximum health is 24.
Recovery is gradual, with the same cadence and maximum-health limit in both.

Placement changes affect how much health is available: the US release
upgrades half apples and adds whole apples at several locations. The
stage-by-stage comparison below lists these changes alongside other pickups.
It describes normal-mode items; US and Japan replace magic-scroll statues
with screen-clear items and sword-power statues with extra lives in
Professional/Special mode.

### Time limits, hazards, and enemy strength

Several action sections have larger starting timer values in the US game.
Confirmed examples include the opening sections of Fillmore and Bloodpool
(300 US versus 200 Japanese), and sections of Northwall Act 1 (200 versus
100). Marahna's Act 2 boss section and its Death Heim counterpart also use
300 versus 200. These are values on the game timer, not measurements in
wall-clock seconds; a limit can belong to one section rather than an entire act.

Enemy health, attack strength, terrain, traps, and pickups also vary. Some
Japanese attacks are considerably stronger, while at least one compared Aitos
enemy attack is weaker. Trap danger depends on placement and collision rules
as well as damage.

Many Japanese terrain traps deal 24 HP of damage—the Master's maximum
health—where the US release deals only 1. They use the normal hazard contact
and invulnerability checks, rather than an unconditional death command.
Some 24-HP traps remain in the US game, including parts of Bloodpool,
Kasandora, Marahna and Northwall. Other changes are smaller: Fillmore Act 1
and parts of Northwall Act 2 use 2 HP in Japan and 1 in the US.

Some other terrain zones slow the player instead of damaging him. Those
zones and their horizontal slowdown rule match between US and Japan;
they are not additional Japanese death traps.

Professional/Special mode promotes eligible one-point enemy attack or health
values to two in US and Japan. Higher values and terrain-box damage are
unaffected by this particular rule.

European hazards are not simply the US set with a difficulty multiplier.
Many damage boxes that deal 1 HP in the US tables deal 24 HP in Europe,
including boxes in Fillmore's caves and parts of Bloodpool, Kasandora,
Marahna and Northwall. Other boxes deal 2 HP instead, including those in
Fillmore's opening forest and Northwall Act 2. Their checked boundaries
match the US boxes. These authored values apply independently of the
difficulty selection: Beginner does not make a 24-HP box harmless.

European Story and Action Mode also select separate placement tables. Seven
shared room layouts change pickup entries between modes; their player starts
and damage-box lists match. Item numbers need to be interpreted with the selected
mode's pickup rules, not given their US names automatically. The checked
pickup and spell-inventory distinctions are described under [European Action Mode items](#european-action-mode-items-and-magic).

See the [action-rule evidence](regional-differences-technical.md#action-rules-in-code)
and [terrain and hazard rules](regional-differences-technical.md#terrain-and-damage-box-contracts)
for the complete measured tables. European placement and hazard checks are
in the [difficulty and placement comparison](regional-differences-technical.md#room-tables-and-hazards).

### European difficulty

All three European releases use these checked rules:

| Rule | Beginner | Normal | Expert |
| --- | --- | --- | --- |
| Enemy placements | Common placements | Also admits Normal-or-higher placements | Also admits Expert-only placements |
| Eligible enemy starting HP | 2 becomes 1 | Base value | 1 becomes 2 |
| Tested enemy-contact damage | Base value | Base value | Base value + 1 |
| Updates per displayed timer unit | 72 | 60 | 48 |
| Approximate seconds per timer unit | 1.44 | 1.20 | 0.96 |

Placement filtering also applies to later waves. It does not remove pickups
whose item numbers happen to match the difficulty markers. The HP adjustment
leaves other values unchanged and exempts some object categories; Expert does
not simply give every boss more health. Terrain damage is not scaled by this
difficulty rule.

The underlying enemy stats differ from the US release too. Aitos's moving
skulls have a base 2 HP rather than the US record's 0, and several Marahna
enemies have 5 HP rather than 3. Death Heim's six returning bosses retain
24 HP but have base contact damage ranging from 2 to 5, rather than the US
records' 1. Applicable difficulty adjustments are layered on those values.

Boss-specific difficulty changes appear with their stages below. The timer
settings do not mean that every animation runs faster on Expert.

Evidence: [difficulty rules](regional-differences-technical.md#spawn-filtering-hp-and-contact-damage),
[timer behavior](regional-differences-technical.md#selection-and-timer),
[five-ROM actor stats](regional-differences-technical.md#five-rom-placed-actor-stat-census).

### European Action Mode items and magic

European Action Mode stores individual spells, rather than a stock of
interchangeable scrolls. The most recently collected spell is cast first;
afterward, the HUD changes to the next spell underneath it. Collecting a
different spell therefore changes what the next cast will do without
discarding the spells already held. Story Mode uses a selected spell and
spends one scroll per cast.

The tested checkpoint retry keeps the spell stack, score and increased
maximum health, but removes sword power. After Game Over, Start returns to
the title screen. Starting Action Mode again clears the stack and score,
restores five displayed lives and eight health, and offers a fresh difficulty
choice with Normal selected.

Completing the tested Aitos and Marahna first acts also keeps the stack and
increased maximum health. The native clear sequence refills current health,
awards the boss's score, clears sword power and advances to Act 2. It does
not exchange the remaining spells for a fresh inventory.

Several pickup entries change purpose between modes, with separate artwork
for Action Mode:

| Story Mode effect | Corresponding Action Mode effect |
| --- | --- |
| One generic scroll | Collect Magical Fire |
| Extra life | Add one current HP and one maximum HP, each capped at 24 |
| Screen-clear effect | Collect Stardust |
| Half apple | Collect Aura |
| 500 points | Collect Light |
| Whole apple, sword power, 1,000 points | Same checked effects |

These are matching pickup entries, not a claim that their pictures look alike.

Action Mode awards an extra life when the score crosses 20,000, 40,000,
60,000 or 80,000 displayed points. Story Mode does not use this score award.
The award routine has an edge case: unlike the ordinary extra-life pickup,
it does not cap the stored life count at 99, so an award at that count wraps
it to zero.

Action spells leave the inventory when casting finishes, rather than when
the effect begins. Story spends its scroll before the effect. A controlled
room-change test confirms this distinction; it is not a known normal-play
method of obtaining free spells.

Evidence: [European item and spell inventory contracts](regional-differences-technical.md#european-items-and-spell-inventory),
[cast ordering and capacity](regional-differences-technical.md#european-spell-capacity-and-interrupted-casts),
[retry and new runs](regional-differences-technical.md#european-action-retry-and-new-run-inventory),
[completed acts](regional-differences-technical.md#european-completed-act-inventory).

## A tour of the action stages

Each kingdom brings together the US/Japanese comparison and the known
European differences. Findings concern the named attacks and phases, not
every possible fight or route. Terrain and pickup paragraphs describe the
US/Japanese normal-mode comparison unless they say otherwise.

Four first-act bosses had no US/Japanese changes in their examined attack
programs or animation data: Fillmore's Centaur, Bloodpool's winged boss,
Aitos's dragon and Northwall's humanoid boss. Their movement also matched
when compared from the same attack phase in normal and Special mode.
European exceptions are described with the relevant stages below.
See the [first-act boss comparison](regional-differences-technical.md#first-act-boss-program-comparison)
for the tests and their limits.

### Fillmore

**Act 1: birds and leaping enemies.** Attacking birds move horizontally by
four pixels per update in Japan, compared with three in the US. The leaping
enemy also moves farther horizontally through most of its jump: three pixels
per update instead of two. Its measured launch, descent and landing phases
take the same time; the change is horizontal movement, not a shorter animation.

**Act 1: tree attacks.** In Japan, the orb-spitting tree also releases two
seeds. They fall to the ground and become moving plant
enemies before the tree fires its two familiar orbs. The US version leaves
out this seed-and-plant phase.

The measured pre-shot hold lasts 129 frames in Japan and 64 in the US. The
seed phase and longer hold occur in both normal and Special mode.

**Act 2: cave-enemy recovery.** Two projectile-firing enemy types recover
sooner in Japan. One waits 40 frames after firing instead of 60. The other
shortens each of its two measured recovery sequences by 16 frames. These
changes, and the Act 1 movement differences above, occur in both normal and
Special mode. They do not make every part of each enemy's behavior faster.

**Act 2: Minotaur attacks.** The original fight repeats its jump-and-throw
cycle every 180 active frames in the US game and 147 in Japan. Most of the
difference is the idle period: 48 frames in the US, versus 16 in Japan.
The US boss also spends one extra frame finishing its throw.

The axe starts 72 pixels horizontally from the boss in the US and 48 in
Japan, on whichever side it faces. Its flight speed matches between US and Japan.

In Death Heim, US and Japan shorten the jump preparation and throwing
wind-up, repeat the attack every 117 frames, and make the axe travel faster.
The regional 72-versus-48-pixel spawn offset still applies. These results hold
in normal and Special mode; Special increases contact damage without changing
the measured attack timing.

**European Minotaur.** Compared with the US fight, its first pauses before
throwing and jumping are shorter. Subsequent jump movement is unchanged per
update, so this does not make the entire European jump faster in real time.

**Act 2: wall emitters.** US and Japan place twelve fireball emitters in the
same positions. Their active firing intervals differ in all three regions:

| Wall-emitter behavior | US | Japan | Europe |
| --- | ---: | ---: | ---: |
| Updates between shots | 360 | 180 | 255 |
| Position relative to US placement | Baseline | Same | 14 pixels inward on either side of the shaft |

The European interval is about 5.1 seconds, compared with 6.0 seconds in the
US game. The US/Japanese shots follow the same rolling and falling behavior;
Special mode increases damage without changing their measured intervals.
This establishes firing and positioning changes, not fewer authored emitters.
The relationship between every decorative wall statue and its artwork still
needs a separate comparison.

**Terrain and pickups.** The US opening terrain and starting position differ:
the Master starts 112 pixels lower. A later checkpoint respawns him 32 pixels
higher, alongside a raised landing surface. Several spike strips are shortened
or rearranged, as well as made less damaging.

Act 1 adds a 500-point pickup, upgrades a half apple to a whole apple, and
replaces a 1,000-point pickup with a 1UP. Act 2 upgrades two half apples and
replaces a magic scroll with a 1,000-point pickup.

Evidence: [tree attack](regional-differences-technical.md#fillmore-act-1-tree-seed-controller-and-pre-shot-wait),
[ordinary enemy movement and recovery](regional-differences-technical.md#ordinary-enemy-movement-and-attack-recovery),
[Minotaur](regional-differences-technical.md#minotaur-timing-and-room-inheritance),
[wall emitters](regional-differences-technical.md#fillmore-act-2-wall-emitter-cadence),
[European emitter behavior](regional-differences-technical.md#remaining-ordinary-enemy-timing-and-motion),
[European Minotaur](regional-differences-technical.md#minotaur-wind-ups),
[terrain and pickups](regional-differences-technical.md#authored-pickup-differences).
The Act 2 theme change is covered under [Music](#music).

### Bloodpool

**Act 2: statue volleys.** US statues fire one fireball per volley. Japanese
statues fire two, 16 frames apart. The second shot has its
own firing animation.

The extra shot makes the complete Japanese cycle longer: 153 frames between
the first shots of successive volleys, compared with 137 in the US. Both
facing directions behave this way in normal and Special mode. All ten statue
placements match between US and Japan.

**Act 2: skeletal swordsmen.** Japan shortens the recovery at the end of
two attacks. The full straight-attack animation takes 46 active frames
instead of 62; the high attack takes 55 instead of 63. The differences are
in the final held pose, not the speed of every sword movement. The European
versions use the US timings.

**Wizard boss.** During its first form, the US Wizard pauses after firing its
three-projectile spread. This adds 31 active frames before it decides whether
to disappear or begin its lightning attack. The Japanese version has no such
pause. The difference remains in Death Heim and in Special mode; it is not a
general slowdown of all the Wizard's animations. US and Japan switch to the
second form when the boss next checks its health and finds fewer than 12 HP.

**Terrain and pickups.** The US release adds a whole apple in Act 1 and
upgrades a half apple in Act 2. In the castle, one Japanese trap strip is
replaced by solid floor. Several other strips still deal 24 HP in US and
Japan; the US change is not a blanket removal of lethal traps.

Evidence: [statue volleys](regional-differences-technical.md#bloodpool-act-2-statues-single-versus-double-volley),
[swordsman recovery](regional-differences-technical.md#remaining-ordinary-enemy-timing-and-motion),
[Wizard phases](regional-differences-technical.md#wizard-original-fight-and-rematch),
[terrain and pickups](regional-differences-technical.md#authored-pickup-differences).

### Kasandora

**Act 1: Antlion.** Japan places the encounter's horizontal trigger 128 pixels
earlier. Its behavior after firing also differs. The US boss pauses for 36
frames after each six-projectile volley, then checks the Master's distance.
Japan checks immediately: if the Master is close, it proceeds to the next
phase; otherwise it waits before firing again.

With the Master staying at least 64 pixels away horizontally, successive
volleys begin every 48 active frames in the US and 73 in Japan. The US boss
therefore fires more frequently at a distant player, but takes longer to
advance to its next phase when the player is close. Both normal and Special
mode use these rules.

**Act 1: fire enemies.** The floating fire enemies move differently around
a nearby Master. The Western versions rise along a curved path, hover for
64 active frames, and descend. Japan uses steeper rise/fall movements without
that added hover. Their projectile selection also differs: more of the
Japanese decision values produce a bouncing flame, while the range selecting
the other flame type stays the same. This is separate from the blue enemies
inside the pyramid, whose checked movement matches between US and Japan.

**Act 2: wall heads.** The US version adds a pause before ordinary wall heads
fire. Heads in the entrance section shoot every 130 active frames, compared
with 99 in Japan. A second variant deeper in the pyramid has longer firing
and idle animations: its intervals are 168 frames in the US and 137 in Japan.
All fourteen placements match between US and Japan.

**Pharaoh fight.** The US boss adds 16 stationary frames to its landing
sequence before releasing a sphere. In US and Japan, that sphere travels
to a wall and turns into a head. The US head fires one arrow, withdraws,
and disappears 30 frames after firing. The Japanese head stays in place
and fires again every 136 frames, allowing heads to accumulate during the
fight. These differences occur in both normal and Special mode.

**Blue spheres.** The small blue enemies that circle and dart diagonally
through the pyramid have matching movement programs and all nine placements
in US and Japan. Their measured movement also matches frame for frame, with a
177-frame cycle in both normal and Special mode. The reported US slowdown
was not observed for this enemy.

**Bird-headed swordsmen.** Their redraw includes collision changes, not just
clothing. In one extended-sword pose, the forward collision extent is 36
pixels in Japan and 28 in the Western releases. Both tested swordsman
variants use it. Their ordinary standing poses also have a one-pixel
vertical difference. These changes do not add animation steps or alter
the checked attack-animation timing.

**Terrain and pickups.** Act 1 upgrades a half apple, adds another whole
apple, and moves a 1UP and its supporting platform up by one 16-pixel tile.
Another half apple is upgraded in Act 2. Near the pyramid's 1UP, the US
version adds solid ground three tiles wide and shortens the adjacent
damaging strip; other nearby floor cells also change.

The Pharaoh's rematch is covered under
[Death Heim](#death-heim).

Evidence: [Antlion trigger and firing](regional-differences-technical.md#antlion-trigger-and-post-volley-decision),
[wall heads, Pharaoh and blue spheres](regional-differences-technical.md#kasandora-act-2-wall-heads-pharaoh-and-blue-spheres),
[swordsman collision sizes](regional-differences-technical.md#kasandora-swordsman-collision-extents),
[fire-enemy movement and spawning](regional-differences-technical.md#kasandora-fire-enemy-motion-and-spawn-decisions),
[terrain and pickups](regional-differences-technical.md#authored-pickup-differences).

### Aitos

**Act 1: humanoid animation.** Two enemy families have four-picture sequences
whose first and last poses play in the opposite order in Japan. The US and
European versions agree. The sequences still take 36 and 52 updates,
respectively, with unchanged movement and collision extents; these are
pose-order changes, not speed changes.

**Act 1: bamboo spike traps.** Japan adds a third falling trap near the
beginning of the stage; the other two placements match. Their movement is
the same in US and Japan: fall 112 pixels, pause, rise, then rest before
checking for the Master again. A complete activation takes 219 active frames.
Different internal animation-state boundaries do not change the motion.

**Act 1: flying-platform skulls.** The US skulls can be destroyed with a sword
hit, earning 200 points. Japanese skulls deflect the same hit. US and Japan
also let them explode when the Master approaches, but the US trigger area is
wider: less than 32 pixels horizontally and 64 vertically, compared with
24 in either direction in Japan. The five skull placements match.

Once triggered, the explosion lasts 12 active frames before the skull
disappears, without awarding points. These rules apply in normal and Special
mode; Special doubles the skull's attack strength in US and Japan.

**Volcano fireballs.** The six rising fireballs in the volcanic shaft have
matching placements and movement. After their randomized starting waits,
they rise at four pixels per active frame in US and Japan. Their measured
rise, return and reset sequences match in normal and Special mode.

The separate molten rocks launched from the earlier lava pits also have
matching placements, launch rules and movement sequences. Their random
starting waits can make footage look different without changing flight
speed. Neither family substantiates the reported US lava slowdown. The
position-triggered room exits and all seven Aitos room video profiles also
match across all five ROMs. The two animated Act 1 backgrounds advance their
page every five updates in each release; the separate tile-upload animation
is disabled in all three Act 1 profiles. A specific scene or object still
needs to be identified before that report can be treated as a confirmed
difference. See the [background-animation checks](regional-differences-technical.md#aitos-background-animation-and-video-profiles).

**Act 1: European dragon.** Beginner omits an additional projectile-producing
attack; Normal and Expert retain it. The measured movement phases of the
dragon itself take the same time on all three difficulties; its starting
health remains 24 HP. Its paired projectiles also remain active for at least
80 movement updates before checking whether they have left the screen.
US and Japan can retire them after the first update. This changes cleanup
and occupied object slots, not their per-update movement speed.

**Act 2: Flaming Wheel.** Its examined attack program and animation data match
between US and Japan. In their Death Heim rematches, the rolling pass covers
the same distance in 40 frames instead of 80, and the
five projectiles move four times as fast. The volley still releases one shot
every 12 frames. These are shared rematch changes, not regional differences.

**Act 2: background distortion.** One room's alternating mosaic bands read
bytes just beyond the intended waveform table. Relocating the neighboring
code changes the pattern:

| Release | Mosaic pattern compared with US |
| --- | --- |
| US | Baseline |
| Japan | Different |
| European English | Different |
| German | Same |
| French | Different |

This is a presentation quirk, not evidence of different lava or enemy speed.

**Pickups.** The US version upgrades the first act's half apple to a whole
apple. The other authored item placements match.

**Tornado animation.** Despite the Western redraw, the three checked tornado
sequences retain the same entries, timing, movement and collision sizes.
The changed artwork does not add an animation step.

Evidence: [humanoid pose order](regional-differences-technical.md#aitos-humanoid-pose-order),
[bamboo traps](regional-differences-technical.md#aitos-bamboo-spike-traps),
[European dragon](regional-differences-technical.md#european-boss-difficulty-branches),
[projectile lifetime](regional-differences-technical.md#aitos-dragon-projectile-lifetime),
[platform skulls and volcano fireballs](regional-differences-technical.md#aitos-act-1-platform-skulls-and-volcano-fireballs),
[molten-rock launches](regional-differences-technical.md#aitos-molten-rock-launches),
[room exits and the lava report](regional-differences-technical.md#aitos-room-exits-and-the-unassigned-lava-claim),
[pickups](regional-differences-technical.md#authored-pickup-differences),
[Flaming Wheel](regional-differences-technical.md#flaming-wheel-original-fight-and-rematch),
[background distortion](regional-differences-technical.md#regional-action-raster-tables).

### Marahna

**Spellcaster wind-up.** The hooded spellcaster begins both examined attacks
28 frames sooner in Japan. Its two wind-ups last 28 and 40 frames, compared
with 56 and 68 in the US; the following recovery still lasts 41 frames.
The attack it chooses depends on the player's position. Both normal and
Special mode use these timings.

**Act 1: boss vulnerability.** The head behaves differently in each region:

| Head behavior | US | Japan | Europe |
| --- | --- | --- | --- |
| Examined attack loop | Remains exposed | Opens, closes and retracts | Opens, closes and retracts |
| Fully open interval | No corresponding retracting cycle | 40 updates | 80 updates |
| Protection while retracted | Not part of the checked loop | Rejects tested sword hits | Rejects the tested ordinary-hit path |
| Starting health | 24 HP | 24 HP | 24 HP |

Japan's other measured phases are 12 frames opening, 12 closing and 90 closed.
Its closed-head protection applies in normal and Special mode. European
observations cover Action Mode on all three difficulties.

The measured main boss position is eight pixels lower in Japan, and its body
uses a different arrangement of sprite parts.

**European plant attacks.** Beginner gives one tendril a slower bobbing
cycle in place of two quicker cycles, without changing when the next
measured attack phase starts. The English, German and French encounters
agree. The plant's projectile-launching body also prepares its upper and lower
shots more slowly: those sequences last 25 updates in Europe instead of
nine in US and Japan. This is separate from the head's vulnerability cycle.

**Act 2: Viper lightning choice.** At each random attack decision, the US
boss selects lightning when the random value is divisible by four. Japan
selects it for every even value: half the possible values rather than a
quarter. This applies in both the original fight and Death Heim, including
Special mode. It does not mean exactly twice as many bolts per minute;
the other attacks and the player's position also affect the time between
decisions.

**European Viper.** The opening lightning movement takes 18 updates instead
of 22 in the original fight, and nine instead of eleven in the Death Heim
rematch. The original-fight durations are both about 0.36 seconds at their
native refresh rates; the rematch durations are about 0.18 seconds. In each
case, the European segment travels less far because movement per update is
unchanged.

In the original arena, the floor attacks' second descent covers 110 pixels
in 15 updates instead of 22—about 0.30 seconds rather than 0.37. This remains
faster after allowing for PAL. All three European languages share these
changes; the checked positioning and linked-part decisions match the US game.

**Terrain and pickups.** Both Act 1 half apples become whole apples in the
US version. Act 2 upgrades another half apple, adds a whole apple near the
end, and replaces a magic scroll with a 1UP. The room with that replacement
also gains solid ground and loses one hovering fireball in the US version.
Most changed damage boxes drop from 24 HP to 1, but a 24-HP strip remains.

**Splitting fireballs.** The removed enemy belongs to a family that remains
elsewhere in US and Japan. It hovers from side to side, checking the player's
distance after each 48-frame cycle. When the Master is less than 80 pixels
away on both axes, it charges for 32 frames and releases four shots: up, down,
left and right. These movement and attack rules match between US and Japan and
between normal and Special mode; the extra Japanese placement is the gameplay
difference here.

**Act 2: trap arrows.** Japanese arrows move 3 pixels per game frame;
US and European arrows move 2. The Western arrows use a shorter drawing
and alternate with a bright flash, while the Japanese arrows keep one
appearance. Native tests verified both firing directions.

**Act 2: retracting heads.** After firing, the retracting head enemy takes
20 active frames to withdraw in Japan and 16 in the Western releases.
The Western animation omits one intermediate pose; the remaining poses keep
their original timing.

Evidence: [Act 1 boss phases and protection](regional-differences-technical.md#marahna-act-1-boss-vulnerability-and-placement),
[spellcaster timing](regional-differences-technical.md#ordinary-enemy-movement-and-attack-recovery),
[Viper attack selection](regional-differences-technical.md#viper-attack-selection-and-rematch),
[European plant behavior](regional-differences-technical.md#european-boss-difficulty-branches),
[plant projectile preparation](regional-differences-technical.md#marahna-head-and-projectile-preparation),
[European Viper](regional-differences-technical.md#european-viper-lightning-and-floor-attacks),
[splitting fireballs](regional-differences-technical.md#marahna-splitting-fireballs),
[trap arrows](regional-differences-technical.md#marahna-trap-arrow-motion-and-artwork),
[head withdrawal](regional-differences-technical.md#remaining-ordinary-enemy-timing-and-motion),
[terrain and pickups](regional-differences-technical.md#authored-pickup-differences).

### Northwall

**Act 1: European boss.** Its throwing animation takes 15 updates instead of
50 in the US. The projectile appears eight pixels farther from the boss
horizontally. On impact, its effect expands
to a wider collision footprint but disappears sooner—20 updates instead of
42. These changes are larger than ordinary PAL timing compensation.

**Act 2: Ice Dragon wind-up.** The examined wind-up lasts 118 frames in the
original US and Japanese Northwall fights. In Death Heim, the US rematch
still takes 118 frames, but the Japanese rematch takes 106. Japan removes
a short pause from the sequence,
bringing the next attack forward by 12 frames.

In US and Japan, the rematch doubles the horizontal speed of its ice balls.
These timing results hold in normal and Special mode.

Tests of the Ice Dragon's wavy background found no visible effect from the
differing leftover scroll-buffer bytes. Changing the waveform itself did
alter the image, confirming that the test could detect a change. This
result applies to the tested scene entries, not every possible approach.

**Terrain and pickups.** The US release widens the first magic pickup's
platform by two tiles and moves the pickup one tile left. In the water
section, the 1UP moves 24 tiles to the right, and a nearby Japanese 24-HP
trap strip is removed. Another platform gains an eight-tile-wide block of
solid ground.
Two half apples in Act 1 and one in Act 2 become whole apples, while an
Act 2 magic scroll becomes a 1UP. Some traps remain lethal in US and Japan.

Evidence: [European throwing and impact sequence](regional-differences-technical.md#remaining-european-boss-program-differences),
[original fight and Death Heim rematch](regional-differences-technical.md#northwall-act-2-boss-original-versus-death-heim),
[background-wave comparison](regional-differences-technical.md#ice-dragon-raster-workspace-residency),
[terrain and pickups](regional-differences-technical.md#authored-pickup-differences).

### Death Heim

The Minotaur's shared rematch changes and regional axe placement are described
under [Fillmore](#fillmore); the Ice Dragon's regional wind-up change is under
[Northwall](#northwall).

**Pharaoh rematch.** In US and Japan, the spheres and arrows move twice as
fast as in Kasandora, and the wall heads emerge in half the time. The regional
firing patterns remain: US heads shoot once and withdraw, while Japanese heads
stay and fire every 136 frames. The US withdrawal is also halved, from 30 to
15 frames; an arrow already fired continues after its head disappears.

The boss's grounded pause changes differently. Its landing-and-bounce sequence
lasts 56 frames in the US rematch, up from 40 in Kasandora. Japan keeps the
24-frame sequence in both encounters. These timings and firing patterns apply
in normal and Special mode.

Evidence: [Pharaoh rematch timing and descendants](regional-differences-technical.md#pharaoh-death-heim-rematch).

**Other rematches.** US and Japan accelerate parts of the Wizard and Viper
fights. The Wizard's initial spread moves twice as fast, while two wind-up
sequences are shortened. The Viper's lightning reaches the ground sooner
and travels along it twice as fast. Their regional pause and attack-selection
rules remain those described under Bloodpool and Marahna.

**Final battle.** The first form's closing sequence lasts 64 frames in the
US game and 36 in Japan. The boss remains vulnerable during this sequence,
giving US players 28 more frames before it disappears.

After the first form is defeated, the US game resumes the stage countdown
from the time remaining; Japan leaves it stopped. A projectile emitted by
the second form also has attack strength 3 in US and 4 in Japan, even in
Special mode. An upper-body turn lasts 37 frames in US and 38 in Japan.

**European final-boss minion.** One of Tanzra's second-form minions has 1 HP
in Europe instead of 2 in the US. Its turn uses two poses rather than four,
taking eight updates instead of 16 before choosing its next direction. These are minion changes,
not a reduction to the final boss's own health.

Evidence: [European minion](regional-differences-technical.md#remaining-european-boss-program-differences),
[Wizard rematch](regional-differences-technical.md#wizard-original-fight-and-rematch),
[Viper rematch](regional-differences-technical.md#viper-attack-selection-and-rematch),
[final-boss mechanics](regional-differences-technical.md#tanzra-forms-timer-and-projectile-strength).

The arrival of Death Heim on the world map is discussed
[below](#death-heims-appearance).

## Simulation mode

### Town development and recovery

Towns develop more slowly in Japan. Routine development work runs less often,
construction cycles have different lengths, and some town-state waits are
longer. Monsters, miracles, menus, and recovery use separate timing rules.

In a controlled starting-town comparison, a construction cycle took 732
active frames in the US game and 2,412 in Japan—about 3.3 times as long in that
situation. Developed towns and event interruptions can change that ratio.

Construction also uses an internal growth reserve to decide how many buildings
can start. The price is 4 units in Japan. In the US and Europe, it rises with
civilization level:

| Civilization level | US / Europe | Japan |
| --- | --- | --- |
| 1 | 4 | 4 |
| 2 | 6 | 4 |
| 3 | 8 | 4 |

Both versions budget at most six starts per construction batch. A lower cost
does not make a Japanese cycle run sooner, and buildings still need suitable
land, sufficient support and a free structure record.
In the town being shown, houses spend those units; support buildings return
their cost when they finish. Off-screen building uses the same starting budget
but does not deduct the reserve. These are separate from construction speed.
[Construction-budget evidence](regional-differences-technical.md#population-switching-established-boundaries)

Recovery follows these schedules; fractional amounts are rounded down:

| Recovery | US | Japan | Europe |
| --- | --- | --- | --- |
| Routine SP refill | Construction cycle queues one tenth of maximum SP | No equivalent periodic refill; relies on other sources | US amount |
| Angel health | Construction cycle queues one quarter of maximum health | Separate timer restores one point at a time, up to maximum | US construction-cycle amount |

Japan's angel recovery therefore arrives in smaller, more regular increments
between construction cycles. The European development scheduler uses the
checked US service counts, but a complete European cycle has not been
measured here in the same starting-town test.

US and Japan require the angel to have health remaining to fire a normal arrow.

Opening the tested town movement menu freezes development, monsters, and
recovery. During a miracle, development can stop while monsters and recovery
continue.

Evidence: [development and recovery](regional-differences-technical.md#scheduler-and-sim-enemies),
[European simulation rules](regional-differences-technical.md#european-simulation-numeric-rules).

### Miracle costs

Lightning costs slightly more in Japan; the other four miracles cost less.

| Miracle | US SP | Japan SP | Europe SP |
| --- | ---: | ---: | ---: |
| Lightning | 10 | 12 | 10 |
| Rain | 20 | 16 | 20 |
| Sunlight | 30 | 18 | 30 |
| Wind | 80 | 24 | 80 |
| Earthquake | 160 | 60 | 160 |

The lower Japanese prices accompany the absence of regular construction-cycle
SP recovery. European costs do not change with difficulty.

### Earthquakes and rebuilding

The US earthquake protects the highest-tier houses while removing the lower
tiers. That makes it useful for clearing space for better housing. Japanese
earthquakes instead use a random destruction test for houses, including
upgraded ones. The number of buildings destroyed varies from cast to cast.
Europe uses the checked US house-tier rule, independently of difficulty.

The difference extends beyond housing. Several structure categories that the
US rules preserve are subject to random destruction in Japan; field treatment
also depends on the field's type. Bridges have a separate rule and survived
the tested combinations in US and Japan.

The effect also depends on its cause. A player's earthquake costs SP and can
directly reduce eligible lairs' remaining monster counts. A Skull Head's
earthquake does neither. Both can still destroy houses,
and those losses have consequences for monsters and town growth.

Each destroyed house contributes four, six, or eight points in the US game,
according to its housing tier. Japan contributes four regardless of tier.
These points replenish unsealed monster lairs; when all lairs are sealed,
they contribute to town growth instead. The game distributes each house's
contribution separately. A player's earthquake can therefore reduce a lair's
count and then replenish it during the same effect.

Evidence: [earthquake behavior](regional-differences-technical.md#earthquake-class-policy).

### Housing and population support

Houses add residents; other structures provide *support*, an allowance used
to decide whether more housing can be built. These are different quantities:

| Structure contribution | US | Japan | Europe |
| --- | ---: | ---: | ---: |
| House residents, low / middle / high tier | 4 / 6 / 8 | 4 / 6 / 8 | 4 / 6 / 8 |
| Ordinary field support | 32 | 16 | 32 |
| Upgraded field support | 48 | 24 | 48 |
| Bridge support | 32 | 16 | 32 |
| Burned or withered field support | 0 | 0 | 0 |

Other support-producing structure categories differ as well. The European
census uses the checked US coefficients regardless of difficulty.

Rain restores a burned field without changing its crop type. Support returns
at the next census using that region's ordinary or upgraded field value.
Repeated Rain does not stack the benefit or award lair or town-growth points.
The repair behavior is shared; the amount of restored support differs.

The game checks the existing population before admitting another house. Once
approved, a house adds all its residents, even if they take the town beyond
the allowance. Recalculating support leaves existing residents unchanged.

Town development follows a shared layout: each 4×4-cell plot reserves five
positions for houses and a 2×2 footprint for a field or other food-producing
building. Changing the road pattern does not move those positions. Empty
ground alone is insufficient; construction also checks whether the cell has
been reached by the town's pathfinding pass. On Fillmore's unchanged base
terrain, that pass reaches only 16 of the 18 candidate food sites and 110
of the 142 candidate house sites. Bridges and subsequent development matter;
these starting-map counts are not limits on a finished town.

This makes population a layout and development problem rather than a fixed
regional cap. Our preliminary Fillmore calculations reproduce the guide
totals below under stated assumptions, but do not yet establish legal
construction sequences or exclude larger alternatives.

### Documented population maxima

Published guides report these attainable maxima for the original game,
not *ActRaiser Renaissance*. The Western figures come from
[The Admiral's Maximum Population Guide](https://gamefaqs.gamespot.com/snes/563502-actraiser/faqs/47431);
the Japanese figures are listed by both
[GCGX](https://gcgx.games/actraiser/tips.html) and
[nJOY](https://i-njoy.net/ar1_14.html).

| Town | Western guide | Japanese guides |
| --- | ---: | ---: |
| Fillmore | 914 | 634 |
| Bloodpool | 874 | 554 |
| Kasandora / Cassandra | 874 | 754 |
| Aitos | 802 | 522 |
| Marahna | 538 | 354 |
| Northwall | 650 | 538 |
| **Total** | **4,652** | **3,356** |

These are guide-reported results, not hard-coded caps or independently
verified maxima from this project's ROM tests. Separate confirmation for
each European release remains outstanding.

The Western guide supplies construction advice and allows Fillmore's 914
with either one or two bridges. It also distinguishes normal play from
cursor-inaccessible building sites. [The Admiral's guide](https://gamefaqs.gamespot.com/snes/563502-actraiser/faqs/47431)
provides practical layouts to test; proving every maximum independently
is a separate research task, not a prerequisite for reporting these figures.

### Population milestones and the Master's level

Early population milestones agree; later requirements diverge:

| Milestone | US | Japan | Europe |
| --- | ---: | ---: | ---: |
| First three thresholds | 80, 200, 400 | 80, 200, 400 | 80, 200, 400 |
| Next threshold | 700 | 550 | 700 |
| Final level requirement | 4,600 | 3,000 | 4,600 |

These are examples from the level table, not maximum town populations.

The maximum-SP progression table is the same across the checked releases.
Earned levels are retained when population falls below an earlier requirement.

The city report uses different rules to classify development in US and Japan.
Its thresholds determine the report shown, rather than imposing a population
cap. Another difference occurs when the game refreshes town status: Western
versions store newly calculated growth warnings, while Japan calculates
those details but leaves them out of the final status write. We have verified
that behavior, but not whether it was intentional or how every naturally
developed town is reported. The reported wording differences are covered under
[Menus](#menus).

Evidence: [support and level tables](regional-differences-technical.md#town-census-and-level-requirements),
[burned-field recovery](regional-differences-technical.md#burned-field-recovery-and-census-refresh),
[construction and population behavior](regional-differences-technical.md#population-switching-established-boundaries),
[building geometry and candidate totals](regional-differences-technical.md#building-geometry-and-conditional-fillmore-calculation),
[city reports](regional-differences-technical.md#census-construction-and-status-are-separate-contracts).
[Status-refresh evidence](regional-differences-technical.md#regional-growth-status-producer)
documents the separate producer difference.

### Population prerequisites for story events

Two town-story prerequisites use lower populations in Japan:

| Event | US | Japan | Europe |
| --- | ---: | ---: | ---: |
| Fillmore: hint about magic beneath the southeastern rock | Above 110 | Above 88 | Above 110 |
| Kasandora: Ancient Tablet discovery | Above 700 | Above 400 | Above 700 |

The comparison is strictly “above”: reaching the listed number is not enough.
The other 28 entries in the population-event tables match across all five
releases. These conditions enable events for later selection; they do not
guarantee an immediate scene, set a town's maximum population or change the
Master's level. The population check itself does not clear an enabled event
when population falls; individual event callbacks can still change eligibility.

Evidence: [population and road prerequisites](regional-differences-technical.md#population-and-road-story-prerequisites).

### Monster lairs and sealing

Japan generally starts with more monsters remaining in each town's lairs.
The totals below sum the four initial lair reserves. Only a portion of those
monsters is active at once, and the reserves can be reduced through combat,
miracles, and other town events.

| Town | US starting total | Japan starting total | Europe starting total |
| --- | ---: | ---: | ---: |
| Fillmore | 500 | 600 | 500 |
| Bloodpool | 330 | 400 | 330 |
| Kasandora | 450 | 900 | 450 |
| Aitos | 320 | 600 | 320 |
| Marahna | 210 | 600 | 210 |
| Northwall | 180 | 500 | 180 |

The lairs' positions and monster types match in the compared starting tables.
US/Japanese spawning delays differ for every lair; Europe uses the checked
US delay table. Countdowns advance only when the town and lair are eligible
to spawn a monster.

Defeating a monster subtracts one from its lair's reserve; qualifying miracle
effects can subtract up to ten. Destroyed houses and action-stage score
processing can change the count too.
An exhausted but unsealed lair can consequently gain monsters again.

The people can seal a lair while it still has monsters remaining. The tested
guidance path requires a population of at least ten. Sealing credits the
remaining count toward town growth, retains the stored number, and marks the
lair as sealed. Monsters already flying can remain alive after their lair is
exhausted or sealed.

In US and Japan, the tested Blue Dragon's returning soul awards one growth
point even if its lair is empty or sealed. Saving or leaving town preserves
that pending reward.

### Action scores and town development

US and Japan convert action-stage scores into a value used by town development:

- The US conversion produces two development points for each complete 100
  displayed score points.
- Japan ignores the first 6,500 displayed points, then produces ten
  development points for each complete block of 320 above that amount.

After the first act, the lair adjustment goes in opposite directions. At a
displayed score of 10,000, the tested US path adds 50 to each lair; Japan subtracts 25
from each, stopping at zero. Once the town has completed its second act, the
score-derived value is directed to growth instead.

Japan processes the score during the stage-clear sequence; the US game does
so when leaving the action stage.

Evidence: [lair counts and delays](regional-differences-technical.md#monster-lairs),
[score and house effects](regional-differences-technical.md#lair-stock-is-not-monotonic),
[sealing and returning souls](regional-differences-technical.md#guidance-sealing-and-delayed-soul-rewards).

### Simulation enemy combat and behavior

Blue Dragons and Red Demons take more hits in the Western games, and several
species deal more contact damage. Europe uses the US combat-stat table.
The hit counts below assume ordinary one-damage arrows:

| Monster | Hits: US / Europe | Hits: Japan | Contact damage: US / Europe | Contact damage: Japan | SP reward, all |
| --- | ---: | ---: | ---: | ---: | ---: |
| Blue Dragon | 3 | 2 | 3 | 2 | 2 |
| Napper Bat | 1 | 1 | 1 | 1 | 1 |
| Red Demon | 4 | 3 | 6 | 3 | 4 |
| Skull Head | 8 | 8 | 8 | 4 | 12 |

Behavior changes are separate from those numbers:

- In its search phase, the Japanese Blue Dragon tries to find a target once
  every eight updates. US and European Dragons try on every update.
- US and European searches examine one rotating block of 16 structure records;
  Japan examines all 128. They also choose candidate coordinates differently.
  Neither version simply picks from a list of every eligible house.
- A US or European Dragon strike runs an extra update of all active simulation
  actors. Other monsters can move and advance their timers again during
  that update. Japan has no corresponding extra pass.
- The Japanese Napper Bat has more accepted random outcomes for switching
  to the Angel when it cannot find a house. Its pre-abduction wait lasts
  60 updates, compared with one in US and Europe.

Some attack details are shared. The Bat checks that its target still exists
before carrying someone away, but the house is removed only if the carrying
Bat leaves the map. Dragon and Bat house losses use the regional
4/6/8-versus-4 feedback rule described above. The Red Demon's checked field
attack applies the same damaged-field flag and appearance change in all
five releases.

The examined Skull Head targeting and earthquake preparation sequence is
shared, including its timers. Its contact damage and the earthquake's
destruction rules differ.

Evidence: [combat and behavior](regional-differences-technical.md#sim-enemy-state-differences),
[targeting and retries](regional-differences-technical.md#monster-target-search-and-retry-gates),
[strike and destruction ordering](regional-differences-technical.md#dragon-recursion-bat-carrying-and-field-damage),
[Skull Head behavior](regional-differences-technical.md#skull-head-target-and-earthquake-state-contract).

### Offerings and the Magic Skull

**Sources of Life and Magic.** Japan puts these offerings into the held-item
inventory. The player activates them later through Use Offering. In the US
and European releases, collecting either one applies its benefit immediately;
it does not remain in inventory.

A Source of Life permanently adds one to the lives available when entering
an action stage. It does not extend the health bar; level-ups handle health.
A Source of Magic increases the persistent magic-scroll allowance by one,
as well as the current working scroll count. The regional difference is
when the player receives these benefits, not a different reward amount.

Evidence: [collection, activation and inventory handling](regional-differences-technical.md#sources-of-life-and-magic-collection-versus-use).

The four Source of Life discoveries are shared across the releases:

| Town | Discovery condition |
| --- | --- |
| Fillmore | Use Bloodpool's Compass in Fillmore, then let the deep-sea fishing event finish. The fishermen offer a jewel. |
| Bloodpool | Use Rain on the lake. Five target locations share this one reward. |
| Kasandora | Reveal the pyramid with Rain, then use Earthquake before the story event asking you to enter the pyramid. |
| Northwall | Use Lightning on your temple. The priest admits keeping a jewel and offers it to you. |

Marahna accepts the Compass too, but its fishermen offer a Source of Magic
instead. Successful use consumes the Compass and teaches only the receiving
town; cancelling or trying it in another town leaves the item intact. The
fishing reward becomes an offering to collect, not an immediate bonus.

Japan's fishermen take longer despite Fillmore's lower completion threshold:

| Fishing rule | Japan | US | European releases |
| --- | --- | --- | --- |
| Fillmore progress updates required | 128 | 255 | 255 |
| Marahna progress updates required | 128 | 128 | 128 |
| Usual interval between updates while the town runs | 40 frames | 8 frames | 8 frames |

Dialogue pauses and construction-cycle work also affect the wait, so these
are not fixed completion times. Europe's eight-frame interval takes longer
in real time at 50 Hz than the US interval at 60 Hz.

Visiting the Sky Palace before a Compass-led fishing expedition finishes
restarts it when you return to town. The town keeps its Compass knowledge, so you do
not need another Compass. This rule is shared: Fillmore's restart was
tested in all five releases, and Marahna uses the same initialization rule.

Evidence: [discovery gates and fishing rewards](regional-differences-technical.md#source-discoveries-and-compass-fishing).

Northwall's separate lake search uses the same progress counter as Marahna,
but does not reset it when starting. In all five releases, controlled tests
carried Marahna's completed value of 128 into Northwall, reducing its search
from 255 updates to 127 without changing the reward. See
[the shared-counter oddity](unused-content.md#two-towns-sharing-fishing-progress)
for the behavior and test limits.

**Inventory limits.** All five releases require a free held-item slot before
opening Take Offering. This includes Western Sources that activate immediately
and would not occupy that slot. With eight held items, collection is refused;
the town keeps its offering and no bonus is applied.

A full town inventory behaves differently. In tests with all eight offering
slots occupied, fishing and the three miracle-based Sources of Life marked
their discovery complete without storing the new reward. Making room later
did not recover it. This behavior matches across the releases; whether normal
town progression can fill all eight slots at these moments remains unverified.

Evidence: [capacity gates and one-time discoveries](regional-differences-technical.md#full-inventories-and-one-time-discoveries).

The Magic Skull seals its designated Bloodpool lair through a different path
from guiding the people. It works at the tested population of two, credits
the remaining lair count toward growth, and consumes the item only on success.
It does not grant the guidance path's technology reward or SP refill. These
rules agree in US and Japan.

The US item sequence adds a 90-frame wait after the effect before consuming
the skull. Japan proceeds without that wait.

All five ROMs also retain a [separate Fillmore Skull event](unused-content.md#fillmores-extra-magic-skull-event)
with translated dialogue, but no normal way of activating it has been identified.

The crop offering uses the same checked replenishment rule in all five
releases. Once Bloodpool's story prerequisite is met, the game supplies
another crop when the town's offering inventory is completely empty. Another kind of offering
blocks replenishment; already carrying a crop does not.

In the US/Japanese menu tests, neither replenishes it while Take Offering is
open. The check resumes when you return to the town, and runs less often
under Japan's slower development schedule. There is no separate Japanese crop cooldown in
this routine, and no need to wait for a full construction cycle.

The Japanese offering is called rice rather than wheat, but its menu icon
is unchanged: both the selected and greyed-out pictures match across all
five releases. This comparison does not cover the fields drawn in towns.

Evidence: [crop replenishment](regional-differences-technical.md#crop-offering-replenishment),
[offering names and artwork](regional-differences-technical.md#crop-offering-menu-artwork).

### Town story events

The examined story-event conditions mostly match across all five releases.
The 15 miracle-triggered entries use the same towns, miracle types and target
squares. The towns' periodic check lists also preserve the same ordering and
local decisions, although their scheduling and translated dialogue differ.

#### Bridges and temple visits

Four successful maintenance flows have been followed through the original
dialogue and, where applicable, the temple visit:

| Event path | Shared observed result |
| --- | --- |
| Fillmore Bridge offering | Requires three particular lairs sealed, not all four; grants the Bridge, visits the temple and returns with the event marked complete |
| Bloodpool bridge request, before bridge technology is available | Visits the temple and returns without marking the request complete |
| Bloodpool lake clearing | Clears 13 terrain squares, adds an offering and completes the event on return |
| Marahna warning | Displays the warning and resumes the town without a temple visit |

Returning from the temple therefore does not always mean that an event is
complete. Bloodpool's bridge request is a verified example, not a regional
difference.

Delivering the Bridge offering later enables bridge technology, consumes the
item and completes Bloodpool's request in all five releases. Those steps
happen in that order, with the acceptance dialogue before consumption.
Cancelling in the item picker or trying to give a bridge that Bloodpool
already knows preserves the item.

Evidence: [maintenance transactions](regional-differences-technical.md#successful-maintenance-transactions),
[discovery and crop-sharing conditions](regional-differences-technical.md#bridges-and-cross-town-crop-sharing),
[bridge delivery](regional-differences-technical.md#bridge-offering-delivery).

#### Crop sharing and Teddy

Bloodpool's early crop and Teddy conditions also match. Above 35 residents,
the crop event becomes eligible; above 90, Teddy's disappearance does.
The latter stops development and supplies Bread. Delivering the Bread records
Teddy's return. Processing that return supplies the Magic Skull, resumes
development and selects the return dialogue. That is separate from
using the skull to seal the southwestern lair and trigger the lake-clearing
sequence. A road connection to Fillmore has its own location check, rather
than being the population condition for discovering the crop.

That connection teaches Fillmore only if Bloodpool already knows the crop.
It records Fillmore's crop knowledge and upgrades its existing fields. If
the connection event is checked before Bloodpool learns the crop, it is
marked complete without teaching; learning the crop later does not retry it.

Manual crop delivery provides another route. Using the offering on an ordinary
field upgrades that field, records the receiving town's crop knowledge and
consumes the offering. Using it on an already-upgraded field still consumes
the offering. Neither method repairs burned fields; Rain handles that separately.
These rules match across all five releases.

Evidence: [crop and Teddy sequence](regional-differences-technical.md#bloodpool-crop-and-teddy-event-joins),
[automatic teaching and manual delivery](regional-differences-technical.md#bridges-and-cross-town-crop-sharing).

#### Bloodpool's disputes, Music and Compass

Bloodpool's disputes become eligible above 300 residents, but the event also
requires Act 2 to be complete. It holds development until Music from Kasandora
has been delivered. Music can be given in advance; the later dispute check
then completes without leaving development on hold. Once the disputes are
resolved, a population above 450 enables the Compass offering.

There is a regional difference in the stored prerequisites. Before Act 2 is
complete, the Western dispute check clears both its own eligibility and the
Compass eligibility; Japan retains the Compass eligibility. However, the
regular town check refreshes population prerequisites and still selects the
unresolved dispute first. This difference alone has not established earlier
Compass access in normal Japanese play.

Evidence: [event priority, Music and Compass](regional-differences-technical.md#bloodpool-disputes-music-and-compass).

#### Miracles and other story conditions

The successful miracle-triggered events also agree across the five releases.
The checked outcomes include Magical Fire from Fillmore's hidden treasure,
the [Source of Life discoveries](#offerings-and-the-magic-skull) and Marahna's
Earthquake terrain change. Location and story gates belong to each event:
Bloodpool's jewel and Northwall's temple confession require their target
locations and an unfound reward; Kasandora's jewel has additional story
conditions. Offerings appear in the town inventory for collection.

Some prerequisites are more specific than a general “enough lairs sealed”
test. One Aitos check requires exactly two sealed lairs, while a Northwall
check requires the second and fourth. Once enabled, these prerequisites
remain set if the original condition changes.

Another Aitos prerequisite checks **Fillmore's population**, requiring at
least 20 alongside an existing story flag. Changing Aitos's own population
does not satisfy that check. This dependency occurs in all five ROMs; whether
it was intentional has not been established.

The checked European monster-state data, shared visual-effect scripts and
structure-update paths also match the US results. These shared systems do not
establish complete campaign equivalence: the successful event tests begin
with controlled prerequisites, rather than following full town-event chains.

Evidence: [miracle story triggers](regional-differences-technical.md#miracle-triggered-story-events),
[periodic town checks](regional-differences-technical.md#periodic-town-event-maintenance),
[miracle outcomes](regional-differences-technical.md#successful-miracle-story-transactions),
[European monster programs](regional-differences-technical.md#european-sim-state-program-comparison),
[shared effects and redraws](regional-differences-technical.md#european-shared-effects-and-structure-redraws),
[structure updates](regional-differences-technical.md#european-nonempty-structure-action-matrix).

## Sky Palace, story, graphics, and music

### Menus

The detailed menu interaction comparison currently covers US and Japan:

| Menu behavior | US | Japan |
| --- | --- | --- |
| Master-status report, press B | Open a second page of act scores and total | Close the report |
| Master-status report, press Y | Close the report | Close the report |
| After closing a town status report | Return to controlling the angel | Keep the command menu open |
| Cancel Progress Log or Message Speed in town | Return to controlling the angel | Keep the command menu open |
| Message Speed choices | 0–9 | 0–7 |

Either B or Y closes the US score page. The different town-menu return
behavior affects simulation timing: development resumes when the US menu
closes, but remains paused while the Japanese menu stays open. Matching
Message Speed numbers do not establish identical reading speeds across
languages.

The city report has separate “slow” and “limit” labels in Japan. Its highest
report code displays **げんかい** (“limit”), corresponding to “Max” in the US
and European English releases. Japan does not simply replace “Max” with “Slow.”
The rules choosing a report code differ, however, so a particular developed
town can receive different classifications. Naturally maximized towns still
need checking alongside their attainable populations.

Evidence: [Master-status navigation](regional-differences-technical.md#master-status-and-score-page),
[town-menu returns and speed choices](regional-differences-technical.md#town-menu-return-behavior-and-message-speed-choices),
[city-report wording and native display](regional-differences-technical.md#city-report-label-selection).

### Death Heim's appearance

After all six towns have completed both acts, the US version cuts to Death
Heim's location on the world map. The screen shakes as the island gradually
appears through a speckled reveal, then fades back to the Sky Palace for the
announcement. The reveal uncovers an island already drawn underneath it;
the island itself does not move upward.

Japan returns to the town after the final act. Death Heim is unlocked and
announced when the player next enters the Sky Palace, without the emergence
animation. It is already present on the next world-map visit. Returning to
the Palace does not repeat the announcement in either version.

During the US reveal, the game keeps the stage-clear music data loaded. It
loads the usual Sky Palace music when returning there.

Evidence: [arrival and music selection](regional-differences-technical.md#death-heim-transition-and-music).

### Story and wording

The Japanese dialogue calls the divine protagonist God and the antagonist
Satan; the Western scripts use the Master and Tanzra. However, the in-game
opening retains the same basic explanation in all five checked releases:
the antagonist sealed away the protagonist's power, and the people's faith
is needed to restore it. A broader claim about a rewritten origin story
should not be applied to this conversation; manuals and other introductory
material are outside this script comparison.

Kasandora's lost man is explicitly dead when the search party finds him in
**all five releases**. Each also describes plans to bury him near the temple
or shrine and offer the scroll found in his hand. This is not a Japanese-only
detail merely implied in the Western dialogue.

The Bloodpool ending does change Teddy's role. In Japan, the angel reveals
that Teddy made the lots used to choose the sacrifice. The Western ending
instead says the angel knew that Teddy was selected. The Japanese wording
suggests more involvement, but does not explicitly state that he rigged the
draw to select himself. European English also corrects a small typo in this
conversation, changing “boy name” to “boy named.”

Evidence: [native narrative comparisons](regional-differences-technical.md#native-narrative-comparisons).

### Music

Fillmore Act 2 keeps the first act's Fillmore theme in Japan. The US and all
three European releases instead load the track used by Kasandora Act 2 and
Marahna Act 1. This was checked against the music actually loaded when the
original game enters the scene.

Of the 17 compared US/Japanese song data sets, 15 match after accounting for
their different locations in the ROM. Two have changed sequence data, and
several scenes select music differently.

All five ROMs contain a silent music entry used by the final world-map
transition before the credits. Western Music Mode exposes it as selection
21; Japan's debug menu stops at 20, although its ending can still request 21.
The Western Palace scene also declares the silent resource under a separate
scene-local selector. A normal route through that extra declaration has not
been established; the silent resource itself is not unused.

Evidence: [Fillmore Act 2 music](regional-differences-technical.md#fillmore-act-2-music-residency),
[song-data survey](regional-differences-technical.md#structured-census),
[silent Palace resource](regional-differences-technical.md#extra-palace-music-resource-silent-upload).

### Artwork

The ordinary action-artwork comparisons confirm the following changes:

| Location | Japanese-to-Western artwork changes |
| --- | --- |
| Fillmore | Revised birds, goblins, leaping beasts, ghouls and skeletal swordsmen; Western goblins carry clubs |
| Bloodpool | Lizard knights gain clothing; water-jumping creatures change shape and color; skulls, fireball statues and the castle's electrical barrier are redrawn |
| Kasandora | The tall flowering plant in the desert is redrawn; bird-headed swordsmen gain clothing and revised sword poses |
| Aitos | Humanoids and tornadoes are redrawn; checked birdman poses change color without changing their silhouettes |
| Marahna | Small spearman shading edit and revised trap arrows |
| Northwall | Two-headed enemies have revised faces across several poses |

The successfully rendered ordinary-enemy comparisons match between the US
and all three European ROMs. They do not cover every boss, animation
controller or shared graphics resource. Native scene checks identify the
larger barrier and plant pictures; their checked animation timing, movement
and collision extents are unchanged across the five releases.

Four additional cave pictures match across regions, but have no identified
animation references. They may be unused; their presence in the ROM is not
evidence that they appear during play.

Image data, palettes, sprite assembly and collision sizes are separate
aspects of a redraw. The gameplay sections describe the confirmed collision
or timing effects; a changed picture alone does not establish either one.
Evidence: [ordinary action artwork](regional-differences-technical.md#ordinary-action-artwork-survey).

#### Town symbols and fields

| Picture | US and Europe | Japan |
| --- | --- | --- |
| Ordinary and improved fields | Same checked field pictures as Japan, including growth frames and burned fields | Same pictures; different support values and animation timing are described in the simulation sections |
| Follower status symbol 1 | Red angry face | Skull |
| Follower death symbol | Skull | Yellow cross |
| Skull Head lair, earlier town graphics | Diamond design | Six-pointed star |
| Kasandora pyramid | Plain upper face | Eye on the upper face |

The rice/wheat naming difference does not extend to the checked offering
icon or field pictures. The follower symbols retain their native positions
and selectors; the regional change is in their pixels.

The lair comparison depends on which town graphics bank is loaded. Before
both acts are completed, Japan uses the star design. Its later bank uses the
same lair pictures as the Western releases. The pyramid's eye differs in
both banks. These are graphics-bank rules, not changes to lair behavior.

#### Title and Death Heim artwork

Japan's title uses a different initial A, a lowercase r and Japanese
lettering beneath the name. The Western logo has a capital R, removes that
lettering, adds a crack to the emblem and includes a TM mark. The US and all
three European logo canvases match.

The copyright footer is drawn separately from the logo:

| Release | Year and company order | Other footer text |
| --- | --- | --- |
| Japan | 1990, Quintet/Enix | “All rights reserved”; no Nintendo licensing line in this record |
| US | 1991, Enix/Quintet | “All rights reserved” and Nintendo licensing line |
| European English and German | 1992, Enix/Quintet | Same wording as US |
| French | 1992, Enix/Quintet | French rights-reserved line; Nintendo licensing line remains English |

In Death Heim's boss-selection room, the leftmost statue has horns in Japan
and lacks them in the Western releases. The background comparison confirms
the redraw; it does not imply a different boss-selection mechanic.

Evidence: [town, title and Death Heim artwork](regional-differences-technical.md#town-title-and-death-heim-artwork).

## What remains unverified

The guide-reported population figures above are documented externally.
Their independent verification and the following questions remain open:

| Topic | Still to establish |
| --- | --- |
| Independent verification of population maxima | Reproduce the published layouts in the pinned ROMs; upper bounds accounting for upgrades, bridges and event changes remain a separate proof task |
| Complete event chains | Natural prerequisite progression and event order across complete campaigns, beyond the individually tested successful paths |
| Other action behavior | Untested enemy and boss branches; a precise scene/object for the reported Aitos lava slowdown |
| Remaining action artwork | Bosses and other shared graphics; native owners for the extra Bloodpool log animation, questionable Japanese Bloodpool references and four unreferenced Fillmore cave pictures. See the [resource-usage checks](regional-differences-technical.md#action-resources-without-a-proven-gameplay-owner) |
| Extra Palace silence declaration | A route carrying scene selector 4 into the Palace loader. The ending already uses the same silent resource directly as music ID 21, independently of this declaration |

Follow-up traces checked the questionable action graphics with their actual
loaded resources and confirmed that the known Death Heim scene selector is
cleared before the Palace loads. They found no new gameplay use for these
entries. The [technical account](regional-differences-technical.md#instruction-level-resource-selection-checks)
records the tested routes and their limits; neither question is treated as
an established regional gameplay difference.

The [unused content and oddities](unused-content.md) article covers debug
facilities, developer text and possible leftovers separately. It
distinguishes working features from unexplained resources; none of its
remaining candidates has confirmed beta or cut-content provenance.

## About the research

Last revised: September 22, 2026. Established findings come from comparing
the retail US, Japanese and European ROM data and code, supplemented by controlled
tests running the original routines and game scenes. Some tests deliberately
set up a particular situation; they are not substitutes for complete playthroughs.

The [technical companion](regional-differences-technical.md) preserves the
exact ROM identities, addresses, measurements, and limitations so readers
can inspect the evidence. Reported leads were paraphrased from the regional
differences section of The Cutting Room Floor's *ActRaiser* article, supplied
as text during the research. They are kept distinct from independently
verified findings.
