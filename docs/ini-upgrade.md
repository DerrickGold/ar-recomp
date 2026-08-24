# Upgrade-safe INI merging

Release bundles keep shipped templates under `utils/defaults/`; they do not
contain the live `config.ini`, `diorama-layers.ini`, or
`game-assets/manifest.ini`. This separation is required because archive
extraction happens before the game can preserve or merge a user's file.

`IniUpgrade_Merge` performs a conservative two-way merge. Existing user keys,
values, comments, blank lines, ordering, and unknown sections remain unchanged.
New namespace keys and entirely new sections are appended from the shipped
template. Section and key names are matched case-insensitively, with keys
scoped to their section; this is necessary because names such as `Fullscreen`
can have unrelated meanings in different sections.

The caller must distinguish two section models:

- `kIniUpgrade_Namespaces` is for fixed sections such as `[Graphics]` and
  `[KeyMap]`. A missing key usually means an older file, so the shipped key is
  appended.
- `kIniUpgrade_Records` is for authored records such as `[layers:GG:MM]`,
  `[replace:*]`, and `[music:*]`. A missing key may be an intentional deletion,
  so only wholly absent sections are appended.

Appending individual keys to record sections would undo user deletions and
could create duplicate section headers that the record parsers interpret as
new, invalid entries. Record-mode merges therefore converge without restoring
pruned fields. The merge never removes or rewrites user content.

The API is pure and follows `snprintf` sizing semantics: call with a null or
zero-sized output to obtain the required byte count. A null or empty live file
is the first-run case and returns the shipped template verbatim.
