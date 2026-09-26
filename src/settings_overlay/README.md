# Finding menu behavior

Start with the screen the player sees:

| Screen | UI owner | Rules / host integration |
| --- | --- | --- |
| Save slots, new game, rename, load and delete | `save_slots/save_slot_menu.c` | `save/save_slot_host.c` |
| Regional settings, presets and impact confirmation | `regional/regional_panel.c` | `regional/regional_menu.c` builds rows; `regional/regional_host.c` applies campaign policy |
| Layers, BG Extents and the palette picker | `layers/layer_menu.c`, `layers/layer_palette.c` | `diorama/diorama_layer_editor.c` and `action/action_bg_tuner.c` build/edit rows; `diorama/diorama_host.c` installs live hooks |
| Ordinary setting rows | `settings_overlay.c` | `app/settings.c` owns the descriptor catalog and values |

`settings_overlay.c` owns section/tab navigation, registry-backed editing, and
routing to feature modals. Feature owners receive the selected page/row and a
viewport; they keep their own drafts, commands, help, and rendering together.
`settings_overlay_widgets.c` owns shared drawing and resource lifetime;
`settings_overlay_artwork.c` builds the ROM-derived fonts and atlases.

Localized UI text is keyed through `localization/ui_catalog.c`. Search a visible
caption there, then follow its key into the feature owner. Layer captions use
`layers/layer_localization.c`; regional captions use `regional/regional_ui.c`.

`tests/settings_overlay_test.c` drives the normal keyboard/gamepad paths with
`tests/host_clock_stub.c`, so captures, cursor animation and status expiry use
one controlled clock. The host still supplies the real clock in production.
