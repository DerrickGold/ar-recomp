# Next release — draft

Changes since **v0800** (October 5, 2026).

A minor update focused on controls, dialogue and translations.

## Controls

- Add analog angel movement in towns, enabled by default. Stick deflection
  controls speed; hold fire to keep your facing while moving. Adjust or disable
  it under **Controls → Devices**.
- Fix duplicate dialogue confirmations and menu movement on Steam Deck.
  **Auto** now uses the controller when connected, with keyboard fallback
  when disconnected.

## Dialogue and translations

- Add controller button artwork and binding-aware text prompts, including
  support for remapped controls. Choose **Text** or **Button glyphs** under
  **Localization → Game text → Button prompts**; Text remains the default.
- Improve enhanced dialogue scrolling and pacing to match the original message
  speed, removing extra confirmations and duplicate or delayed dialogue blips.
- Keep spell and miracle names consistent with translated menu labels.
- Expand translation coverage for town-menu Help and fix word spacing and
  missing translations in town-event dialogue.

## Builder

- Improve Linux and Steam Deck Builder compatibility and startup reliability.
- Fix interrupted Linux release builds leaving stale locks on macOS.

Rebuild your game with the updated Builder to receive these changes. See
[Updating an installation](https://github.com/DerrickGold/ar-recomp#4-update-or-import-an-existing-installation).
