# Regional settings

Open **Settings → Localization → Regional rules** after starting or continuing
a game. You can choose spell-scroll costs, miracle SP costs, initial room
time limits, checkpoint-retry score handling, development pacing, construction
waits, Fillmore's fishing target, town recovery, earthquake destruction and
the Master report's score page, town-menu return behavior, message-speed
choices and magic controls independently.
The flag and name beside each choice identify its source rules, not the
language of your game.

| Cost | US / Europe | Japan |
| --- | ---: | ---: |
| Fire | 1 scroll | 1 scroll |
| Stardust | 1 scroll | 2 scrolls |
| Aura | 1 scroll | 3 scrolls |
| Light | 1 scroll | 4 scrolls |
| Lightning | 10 SP | 12 SP |
| Rain | 20 SP | 16 SP |
| Sunlight | 30 SP | 18 SP |
| Wind | 80 SP | 24 SP |
| Earthquake | 160 SP | 60 SP |

A change applies to the next cast or miracle. A miracle already being chosen
keeps its original price through confirmation, cancellation and completion.
The description shows the selected prices; **Last activated** identifies the
rules used by the most recent transaction. Insufficient resources prevent a
cast without deducting anything.

The initial-time option changes six action-room profiles. Room numbers here
count sections within each region, not acts.

| Room | US / Europe | Japan |
| --- | ---: | ---: |
| Fillmore 1 | 300 | 200 |
| Fillmore 3 | 200 | 100 |
| Bloodpool 1 | 300 | 200 |
| Marahna boss / its Death Heim rematch | 300 | 200 |
| Northwall 1 | 200 | 100 |
| Northwall 2 | 200 | 100 |

Other room limits are unchanged. The choice takes effect when the game next
initializes an action room, including a retry. Changing it never refills or
shortens a countdown already running. The countdown cadence stays unchanged:
choosing Europe does not switch the game to PAL timing.

**Checkpoint score** chooses whether a retry keeps the current action score
(US/Europe) or clears it (Japan). This applies when the game consumes its
checkpoint-retry marker, not when you change the setting. It does not alter
life allowances, act-clear rewards, returning to the Palace after the last
life, or starting another act/run; those paths keep their own native behavior.

**Magic controls** uses A or X for the US/European standing cast, or Up + Y
for Japan's ground-attack cast. In the Japanese scheme A and X no longer cast;
Y alone still swings the sword. Airborne and crouching attacks retain their
native behavior. These are game buttons: your physical controller and keyboard
bindings stay unchanged. A change waits until Up, Y, A and X are released, so
holding a button while switching does not accidentally cast. Scroll prices,
spell inventory and the ordinary casting restrictions are separate.

**Construction wait** sets the pause between town construction phases: one
town update for US/Europe, or 150 for Japan. These are calls to the town's wait
handler, not seconds. Each town finishes its current wait before using the new
reload. Switching back does not immediately erase an outstanding Japanese wait.
The broader development-cycle pacing, monster updates and recovery rules remain
unchanged; this option alone does not reproduce Japan's complete town pacing.

**Development clock** changes how often the town's development work runs.
US/Europe services it on each eligible master-loop call, with a long cycle of
720 services. Japan services it every fifth call, with a long cycle of 480
services. Japan also updates structure visuals every fifth effect-service call;
those calls share the same divider phase. Monster updates run independently.
The current long cycle finishes before a changed setting takes effect, without
resetting its clocks. Open menus and effects retain their native pause rules;
these counts are not a promise of elapsed seconds. Construction waits, fishing
targets, HP/SP recovery and the host's 60 Hz clock are separate.

**Town recovery** selects the routine HP/SP recovery rules. US/Europe replaces
the pending recovery amounts each long cycle with one tenth of maximum SP and
one quarter of the angel's maximum HP, rounded down. The angel's normal update
drains one queued SP on every fourth frame phase and one HP on every sixteenth.
Japan has no cycle-based SP refill; the angel instead recovers one HP every
60 eligible movement-service calls. Native pauses still apply, including the
Japanese exclusion for angel state 4. These are service counts, not seconds.

The choice applies at the next recovery service. Changing a recovery rule
discards its old pending queue or partial clock without removing HP/SP already
earned. It does not grant an immediate refill. Switching between equal US and
European rules preserves the queue. Level-up, lair-sealing and other rewards
remain independent, as does the selected development clock.

**Earthquake destruction** changes the structure-selection rules for the next
player or monster earthquake. US/Europe protects the highest-tier houses,
improved fields, windmills and factories. Japan uses the native random test for
these categories, so upgraded buildings can also be destroyed. Other native
structure categories retain their respective regional rules; bridges retain
their shared gate-dependent behavior. These rolls use the game's RNG and
normal structure processing, not a separate host random generator.

The whole effect keeps the choices captured when it started. Changing the
setting does not cause an earthquake, change its price, or alter the existing
house-loss contributions and player-versus-monster resource side effects.
It is separate from the planned town-redevelopment reset.

**Master score page** controls the report shared by the Sky Palace and town
menus. US/Europe opens the act-score page when you press B after the Master
report; Y closes it directly. Japan closes the report with either button.
Scores continue to be recorded even when their page is hidden. The choice
applies on the next report opening and never closes an already-open page.
It does not change whether the surrounding command menu remains open.

**Town menu return** controls what happens after Master status, city status,
Progress Log, and Message Speed. US/Europe closes the command menu and resumes
town play; Japan keeps it open at the same selection. The command captures the
choice when accepted, so a change during a report applies to the next command.
Reports still clean up their own text and objects, saving still uses the normal
save transaction, and cancelling Message Speed keeps its previous setting.
Palace menus and other town commands are unchanged. Both Original and Modern
town-menu styles use this rule.

**Message-speed range** offers 0–9 for US/Europe or 0–7 for Japan. The choice
applies the next time the speed selector opens in the Palace or a town; an
open selector keeps its range. If the current speed is 8 or 9, Japan's cursor
starts at 7, but the stored speed is not changed until you confirm. Cancelling
preserves the old value. The numeric row and cursor use the same range with
native or enhanced text and either town-menu style. This changes the offered
choices, not the timing of each speed or the selected language.

**Fillmore fishing** sets the Compass expedition's target to 255 event updates
(US/Europe) or 128 (Japan). These are eligible event updates, not seconds; the
development cycle controls how often they occur. A change takes effect at the
next fishing update without resetting progress. Lowering the target past the
current progress completes the event once through its normal reward path.
It cannot repeat a completed reward. Native Palace visits still restart an
unfinished expedition. Marahna fishing, Northwall's lake search and automatic
versus manual collection of Sources are unchanged.

These choices belong to the current campaign. **Save with the Progress Log to
keep them.** Closing the overlay is not a save. New Game starts with US rules;
Continue restores the choices saved with that campaign. Keep the matching
`.archeckpoint` companion beside your save when copying it to another Recomp
installation. The `.srm` itself remains compatible with SNES emulators. See
[save companions](save-format.md#regional-campaign-checkpoints) for backup and
recovery details.

Regional changes are locked during recording and replay, including after
taking over from a replay. Start a normal session to edit them.

These are **individual options, not complete regional presets**. They do not
change spell inventory, population,
enemy behavior, artwork or language. In particular, choosing European costs
does not enable the European Action-mode spell stack. No donor ROM is needed
for these rules. The [regional comparison](regional-differences.md)
describes the wider set of differences separately.
