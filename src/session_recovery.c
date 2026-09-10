#include "session_recovery.h"

#include <string.h>

const char *SessionRecovery_Title(ArUiLocale locale, SessionFailureKind kind) {
  return ArUiCatalog_Text(locale, kind == kSessionFailure_Startup
      ? "recovery.startup_title" : "recovery.closed_title", "ActRaiser Recompiled");
}

bool SessionRecovery_Format(char *output, size_t capacity, ArUiLocale locale,
    SessionFailureKind kind, const char *detail, bool settings_failed, bool save_failed) {
  static const char *const reasons[] = {
      "recovery.generic", "recovery.battery_save", "recovery.graphics_reset",
      "recovery.graphics_lost", "recovery.audio_device", "recovery.startup"};
  if (kind < kSessionFailure_Generic || kind > kSessionFailure_Startup)
    kind = kSessionFailure_Generic;
  /* Leave ample room for translated instructions and both warnings even when
   * a platform supplies a very long diagnostic. Never split a UTF-8 scalar. */
  char bounded_detail[1024];
  if (!detail) detail = "";
  size_t bytes = 0;
  while (bytes < sizeof(bounded_detail) - 1 && detail[bytes]) ++bytes;
  while (bytes && ((unsigned char)detail[bytes] & 0xc0u) == 0x80u) --bytes;
  /* The fatal latch/platform may already have truncated the diagnostic. Drop
   * an incomplete final scalar there too, not only at our own capacity edge. */
  if (bytes) {
    size_t last = bytes - 1;
    while (last && ((unsigned char)detail[last] & 0xc0u) == 0x80u) --last;
    const unsigned char lead = (unsigned char)detail[last];
    const size_t scalar_bytes = lead >= 0xf0u ? 4 : lead >= 0xe0u ? 3 : lead >= 0xc0u ? 2 : 1;
    if (bytes - last < scalar_bytes) bytes = last;
  }
  memcpy(bounded_detail, detail, bytes);
  bounded_detail[bytes] = 0;
  const ArUiTextArgument args[] = {
      {"reason", ArUiCatalog_Text(locale, reasons[kind], NULL)},
      {"detail", bounded_detail},
      {"closed", kind == kSessionFailure_Startup ? "" : ArUiCatalog_Text(locale, "recovery.closed", NULL)},
      {"settings", settings_failed ? ArUiCatalog_Text(locale, "recovery.settings_warning", NULL) : ""},
      {"save", save_failed ? ArUiCatalog_Text(locale, "recovery.save_warning", NULL) : ""},
  };
  return ArUiCatalog_Format(output, capacity,
      ArUiCatalog_Text(locale, "recovery.message", NULL), args, sizeof(args)/sizeof(args[0]));
}
