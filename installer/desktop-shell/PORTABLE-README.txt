ActRaiser Recomp Builder — portable download

Extract this entire folder somewhere writable. Keep the Builder app and its
.portable file together, then open the app. Choose your own US game ROM in the
Workshop to build the game. BuilderData holds the build workspace; the default
game output is the adjacent ActRaiserRecomp folder.

Updating an existing game: close the game, back up its entire saves/ directory,
then choose that game folder in the new Builder and rebuild. On the game's
first upgraded launch, older save.srm/save.ini files (or actraiser.srm when
neither exists) in saves/ are automatically adopted into Slot 1. Originals are
retained in saves/legacy-layout/. Choose Continue at the title screen to resume.
For another or older utils/ installation, use Import previous installation to
copy its data into the selected game folder before launching the rebuilt game.

The first launch verifies the bundled files. Later launches reuse checks for
unchanged files to open faster. To force a complete check, close the Builder
and launch its executable with --verify-bundle. See the full guide linked below
for platform-specific commands.

Windows: Application Control
The Builder and its tools are unsigned. Windows may let the Builder open but
block an internal tool such as snesbuild.exe with "An Application Control policy
has blocked this file". Open Windows Security > App & browser control > Smart
App Control settings. There is no per-app exception. On your own device, if
you trust this download and accept disabling this protection for all apps,
choose Off after reading Windows' confirmation, then retry Build game.
For a managed device, an unavailable setting, or a continuing block, ask your
administrator to approve the tool; copy the Builder's error details.

macOS: Gatekeeper
The Builder has a local signature, without Developer ID signing or notarization.
For an unidentified-developer or unverified-app warning on a download you trust,
try opening it, then use System Settings > Privacy & Security > Open Anyway.
Helpers may also be checked. Damaged-app or malware warnings need separate
investigation.

Steam Deck: open the AppImage in Desktop Mode. If FUSE is unavailable, run:
APPIMAGE_EXTRACT_AND_RUN=1 ./ActRaiserRecompBuilder-steam-deck.AppImage

Full troubleshooting instructions and official Windows/macOS guidance:
https://github.com/DerrickGold/ar-recomp/blob/main/docs/desktop-packaging.md
