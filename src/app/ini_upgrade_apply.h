#ifndef AR_INI_UPGRADE_APPLY_H
#define AR_INI_UPGRADE_APPLY_H

/* Merge the bundle's shipped defaults (defaults/<leaf>) into the user's live
 * config files, once at startup, before anything reads them.
 *
 * Settings and asset records keep user values and gain missing entries.
 * Diorama layers are release content: changed shipped bytes replace the live
 * file after a numbered .pre-update-N backup. The .installed baseline prevents
 * ordinary launches from overwriting local authoring; missing live content is
 * restored. Source checkouts without defaults/ remain manually authored.
 *
 * Silent when there is nothing to do, non-fatal on every failure: a game that
 * will not start is far worse than a setting that has not appeared yet. */
void IniUpgrade_ApplyShippedDefaults(void);

#endif /* AR_INI_UPGRADE_APPLY_H */
