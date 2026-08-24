#include "manifest_utils.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "text_parse_utils.h"

char *Manifest_Trim(char *text) {
  text = TextParse_TrimLeft(text);
  TextParse_TrimRight(text);
  return text;
}

static bool IsAsciiLetter(char value) {
  return (value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z');
}

static bool IsAbsolutePath(const char *path) {
  if (path[0] == '/' || path[0] == '\\') return true;
  return IsAsciiLetter(path[0]) && path[1] == ':' &&
      (path[2] == '/' || path[2] == '\\');
}

void Manifest_ResolvePath(const char *manifest_path, const char *value,
                          char *out, size_t out_size) {
  const char *slash = strrchr(manifest_path, '/');
  const char *backslash = strrchr(manifest_path, '\\');
  if (backslash && (!slash || backslash > slash)) slash = backslash;

  if (IsAbsolutePath(value) || !slash) {
    snprintf(out, out_size, "%s", value);
    return;
  }
  snprintf(out, out_size, "%.*s/%s",
           (int)(slash - manifest_path), manifest_path, value);
}
