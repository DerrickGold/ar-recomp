#ifndef INI_UPGRADE_APPLY_H
#define INI_UPGRADE_APPLY_H

/* Merge the bundle's shipped defaults (defaults/<leaf>) into the user's live
 * config files, once at startup, before anything reads them.
 *
 * Keeps every value the user changed and adds only keys that are new in this
 * version -- the upgrade path for someone who extracts a new bundle over an
 * existing install. See ini_upgrade.h for the merge rules and why the shipped
 * copies had to stop being the live files.
 *
 * Silent when there is nothing to do, non-fatal on every failure: a game that
 * will not start is far worse than a setting that has not appeared yet. */
void IniUpgrade_ApplyShippedDefaults(void);

#endif /* INI_UPGRADE_APPLY_H */
