#ifndef INI_UPGRADE_H
#define INI_UPGRADE_H

#include <stdbool.h>
#include <stddef.h>

/* Conservative, pure merging of shipped defaults into user-owned INI files.
 * Existing content is preserved; namespace sections receive missing keys,
 * while record sections receive only wholly absent records. This keeps bundle
 * upgrades from overwriting user-authored settings or asset records. */

typedef enum {
  kIniUpgrade_Namespaces = 0, /* config.ini: sections are key namespaces */
  kIniUpgrade_Records,        /* layers/manifest: sections are records */
} IniUpgradeSectionKind;

/* snprintf-style merge; out_added optionally receives the number of appended
 * keys and sections. A null/empty live file yields shipped verbatim. */
size_t IniUpgrade_Merge(const char *live, const char *shipped,
                        IniUpgradeSectionKind kind,
                        char *buffer, size_t size, int *out_added);

/* True when merging would change the live file. */
bool IniUpgrade_NeedsMerge(const char *live, const char *shipped,
                           IniUpgradeSectionKind kind);

#endif /* INI_UPGRADE_H */
