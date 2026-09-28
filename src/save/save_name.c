#include "save/save_name.h"

#include "byte_order.h"
#include "localization/unicode_grapheme.h"
#include "snesrecomp/support/utf8_fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

bool SaveName_CopyNative(const uint8_t *image, char *out, size_t capacity) {
  if (!out || !capacity) return false;
  out[0] = 0;
  if (!image) return false;
  size_t length = 0;
  while (length < kActRaiserPlayerNameCharacterLimit) {
    const unsigned byte = image[0x1439 + length];
    if (!byte || byte == 255) break;
    if (byte < 32 || byte > 126 || length + 1 >= capacity) {
      out[0] = 0;
      return false;
    }
    out[length++] = (char)byte;
  }
  out[length] = 0;
  return length != 0;
}

bool SaveName_Valid(const char *name) {
  if (!name || !name[0]) return false;
  const size_t size = strlen(name);
  if (size >= kSaveNameCapacity) return false;
  size_t offset = 0;
  unsigned count = 0;
  while (offset < size) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(name, size, offset, NULL, &next) || next <= offset ||
        count++ >= kActRaiserPlayerNameCharacterLimit) return false;
    offset = next;
  }
  return count != 0;
}

bool SaveName_ReadLegacy(const char *path, const uint8_t *image,
                         char out[kSaveNameCapacity], SaveError *error) {
  char companion[kHostPathCapacity], native[kActRaiserPlayerNameStorageBytes];
  const int n = snprintf(companion, sizeof(companion), "%s.arname", path);
  out[0] = 0;
  if (n < 0 || (size_t)n >= sizeof(companion)) {
    if (error) snprintf(error->message, sizeof(error->message), "campaign name path is too long");
    return false;
  }
  FILE *file = sr_fopen(companion, "rb");
  if (!file) {
    if (errno == ENOENT) return true;
    if (error)
      snprintf(error->message, sizeof(error->message), "cannot read campaign name companion");
    return false;
  }
  uint8_t bytes[23 + kSaveNameCapacity] = {0};
  size_t size = fread(bytes, 1, sizeof(bytes), file);
  bool valid = !ferror(file) && fgetc(file) == EOF && size >= 23;
  if (fclose(file)) valid = false;
  const size_t length = ByteOrder_ReadLe16(bytes + 21);
  char compatibility[kActRaiserPlayerNameStorageBytes];
  memcpy(compatibility, bytes + 12, sizeof(compatibility));
  compatibility[sizeof(compatibility) - 1] = 0;
  valid = valid && !memcmp(bytes, "ARNAME1\0", 8) && length && length < kSaveNameCapacity &&
      size == 23 + length && ByteOrder_ReadLe32(bytes + 8) == Save_ComputeChecksum(image) &&
      SaveName_CopyNative(image, native, sizeof(native)) && !strcmp(native, compatibility);
  if (valid) {
    memcpy(out, bytes + 23, length);
    out[length] = 0;
    valid = strlen(out) == length && SaveName_Valid(out);
  }
  if (valid) return true;
  out[0] = 0;
  if (error)
    snprintf(error->message, sizeof(error->message),
             "campaign name companion is stale or damaged; restore it before saving");
  return false;
}
