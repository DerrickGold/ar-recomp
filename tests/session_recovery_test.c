#include "session_recovery.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void) {
  const char *detail = "disk 100% {save}; /tmp/Été/日本語/save.srm";
  for (int locale = 0; locale < kArUiLocale_Count; ++locale)
  for (int kind = kSessionFailure_Generic; kind <= kSessionFailure_Startup; ++kind)
  for (int warnings = 0; warnings < 4; ++warnings) {
    char message[kSessionRecoveryCapacity];
    CHECK(SessionRecovery_Format(message, sizeof(message), locale, kind, detail,
                                  warnings & 1, warnings & 2));
    CHECK(strstr(message, detail) != NULL);
    CHECK((strstr(message, "settings.ini") != NULL) == ((warnings & 1) != 0));
    CHECK(SessionRecovery_Title(locale, kind)[0]);
    if (warnings & 2) CHECK(strstr(message, ArUiCatalog_Text(locale, "recovery.save_warning", NULL)));
    if (kind != kSessionFailure_Startup) CHECK(strstr(message, ArUiCatalog_Text(locale, "recovery.closed", NULL)));
    if (locale) {
      char english[kSessionRecoveryCapacity];
      CHECK(SessionRecovery_Format(english, sizeof(english), 0, kind, detail, warnings & 1, warnings & 2));
      CHECK(strcmp(english, message));
    }
    char small[4] = "old";
    CHECK(!SessionRecovery_Format(small, sizeof(small), locale, kind, detail, false, false));
    CHECK(!strcmp(small, "old"));
  }
  char long_detail[2048], message[kSessionRecoveryCapacity];
  memset(long_detail, 'a', sizeof(long_detail));
  memcpy(long_detail + 1022, "日本語", strlen("日本語"));
  long_detail[sizeof(long_detail)-1] = 0;
  CHECK(SessionRecovery_Format(message, sizeof(message), kArUiLocale_Japanese,
      kSessionFailure_BatterySave, long_detail, true, true));
  CHECK(!strstr(message, "日")); /* No partial scalar at the bounded detail end. */
  CHECK(strstr(message, "settings.ini"));
  CHECK(strstr(message, ArUiCatalog_Text(kArUiLocale_Japanese, "recovery.save_warning", NULL)));
  for (size_t cut = 1; cut < strlen("日"); ++cut) {
    char truncated[32] = "detail:";
    memcpy(truncated + 7, "日", cut);
    truncated[7 + cut] = 0;
    CHECK(SessionRecovery_Format(message, sizeof(message), kArUiLocale_English,
        kSessionFailure_Generic, truncated, false, false));
    CHECK(strstr(message, "detail:\n"));
    CHECK(!strstr(message, "\xe6"));
  }
  puts("session_recovery_test: PASS");
  return 0;
}
