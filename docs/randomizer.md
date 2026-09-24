# Seeded campaigns

The randomizer is currently an experimental feature under the overlay's debug
settings. It has automated tests, but a complete randomized campaign has not
yet been play-tested. Enable **Show debug settings**, then open **Randomizer**
at the title screen.

## Starting a run

Turn on **Randomizer**, enter a seed or choose **New seed**, and select the
options you want. Starting **New Game** captures that setup for the campaign.
The seed and every randomizer option are saved when you save your game.
They cannot be changed during that run; return to the title screen to prepare
another one. This does not replace an existing save until you actually save
the new campaign.

**Continue** restores the saved seed and options, regardless of the current
title-screen setup or `settings.ini`. It restores the saved regional choices
instead of rolling them again. An older save with no recorded randomizer
recipe uses unrandomized content and retains its existing regional rules:
there is no reliable way to recover a historical seed from SRAM. Starting a
new seeded game is the supported way to obtain a reproducible campaign.

## Regional rolls

Two independent options choose among US, Japanese and European sources:

- **Regional action rules** rolls stage design and placements, enemy/boss
  behavior and stats, magic, lives and scoring rules. The HP and damage
  percentages scale the selected regional base values afterward.
- **Regional town rules** rolls construction, population support, lairs,
  monsters, miracles and resources. Reduced population support is paired with
  reachable level and story goals. It never redevelops an existing town.

Each underlying feature has its own deterministic roll, rather than one
region for an entire tab. Enabling Town rolls does not change the Action
results. Some regional choices have identical behavior; choosing JP does not
necessarily make a feature harder.

Difficulty, title-mode access, controls, menu behavior, artwork, music and the
Death Heim arrival sequence remain as selected manually. Language and fonts
are also unaffected. Unselected randomizer groups retain your starting rules.
You can inspect the resulting choices in **Regions** after starting the game;
manual regional edits still use their usual confirmation and activation rules.
Those edits are saved, but are not new seed rolls.

To reproduce a setup, share the seed, enabled options, starting regional rules
and game version. The seed chooses content and regional rules; it does not
replace the game's moment-to-moment RNG or reproduce player input.

## Moving or backing up a save

Keep `save.srm.archeckpoint` alongside `save.srm` (or the corresponding
companion for an INI save). The checkpoint binds the recipe and regional state
to the exact native save image. Do not copy a companion from an unrelated
save. The 8 KiB SRAM file remains compatible with SNES emulators; an emulator
does not apply the host randomizer or regional enhancements.

The game's import and recovery-copy operations preserve this metadata. A
plain native-SRAM export is for emulator interchange, not a complete backup
of enhanced campaign state. For the binary details, see [Save format](save-format.md).
