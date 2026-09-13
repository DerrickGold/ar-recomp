#include "atomic_replace.h"

#include "snesrecomp/support/utf8_fs.h"

bool AtomicReplaceFile(const char *temporary, const char *path) {
  return sr_replace_file(temporary, path) != 0;
}
