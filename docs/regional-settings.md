# Regional settings

Open **Settings → Localization → Regional rules** after starting or continuing
a game. You can choose spell-scroll costs and miracle SP costs independently.
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

These choices belong to the current campaign. **Save with the Progress Log to
keep them.** Closing the overlay is not a save. New Game starts with US costs;
Continue restores the choices saved with that campaign. Keep the matching
`.archeckpoint` companion beside your save when copying it to another Recomp
installation. The `.srm` itself remains compatible with SNES emulators. See
[save companions](save-format.md#regional-campaign-checkpoints) for backup and
recovery details.

Regional changes are locked during recording and replay, including after
taking over from a replay. Start a normal session to edit them.

These are **pricing options, not complete regional presets**. They do not
change spell inventory, controls, recovery, earthquake destruction, population,
enemy behavior, artwork or language. In particular, choosing European costs
does not enable the European Action-mode spell stack. No donor ROM is needed
for these pricing rules. The [regional comparison](regional-differences.md)
describes the wider set of differences separately.
