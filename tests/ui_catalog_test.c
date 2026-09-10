#include "localization/ui_catalog.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; \
} } while (0)

int main(void) {
  CHECK(ArUiCatalog_ParseLocale(NULL) == kArUiLocale_English);
  CHECK(ArUiCatalog_ParseLocale("") == kArUiLocale_English);
  CHECK(ArUiCatalog_ParseLocale("f") == kArUiLocale_English);
  CHECK(ArUiCatalog_ParseLocale("en-GB") == kArUiLocale_English);
  CHECK(ArUiCatalog_ParseLocale("FR-ca") == kArUiLocale_French);
  CHECK(ArUiCatalog_ParseLocale("de_DE") == kArUiLocale_German);
  CHECK(ArUiCatalog_ParseLocale("ja-JP") == kArUiLocale_Japanese);
  CHECK(ArUiCatalog_ParseLocale("jargon") == kArUiLocale_English);
  CHECK(!strcmp(ArUiCatalog_LocaleTag((ArUiLocale)99), "en"));
  CHECK(!strcmp(ArUiCatalog_Text(kArUiLocale_French, "common.save", "wrong"), "Enregistrer"));
  CHECK(!strcmp(ArUiCatalog_Text(kArUiLocale_German, "common.save", NULL), "Speichern"));
  CHECK(!strcmp(ArUiCatalog_Text(kArUiLocale_Japanese, "common.save", NULL), "保存"));
  CHECK(!strcmp(ArUiCatalog_Text((ArUiLocale)-1, "common.save", NULL), "Save"));
  const char *custom = "Save — {my_pack} 日本語";
  CHECK(ArUiCatalog_Text(kArUiLocale_Japanese, "unknown.key", custom) == custom);
  CHECK(!strcmp(ArUiCatalog_Text(kArUiLocale_Japanese, NULL, NULL), ""));
  char out[512] = "unchanged";
  ArUiTextArgument args[] = {{"section", "{other} %s 日本語"}};
  CHECK(ArUiCatalog_Format(out, sizeof(out), "{section} / {section}", args, 1));
  CHECK(!strcmp(out, "{other} %s 日本語 / {other} %s 日本語"));
  for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
    CHECK(ArUiCatalog_Format(out, sizeof(out), ArUiCatalog_Text(
        (ArUiLocale)locale, "overlay.reset_section", NULL), args, 1));
    CHECK(strstr(out, args[0].value) != NULL);
  }
  const char *bad[] = {"{missing}", "{section", "section}", "{}", "{{section}}", "{bad-name}"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
    strcpy(out, "unchanged");
    CHECK(!ArUiCatalog_Format(out, sizeof(out), bad[i], args, 1));
    CHECK(!strcmp(out, "unchanged"));
  }
  CHECK(!ArUiCatalog_Format(out, 3, "abcd", NULL, 0));
  CHECK(!strcmp(out, "unchanged"));
  CHECK(ArUiCatalog_Format(out, 5, "abcd", NULL, 0));
  CHECK(!strcmp(out, "abcd"));
  CHECK(ArUiCatalog_Format(out, 1, "", NULL, 0));
  ArUiTextArgument duplicate[] = {{"a", "1"}, {"a", "2"}};
  CHECK(!ArUiCatalog_Format(out, sizeof(out), "{a}", duplicate, 2));
  CHECK(!ArUiCatalog_Format(NULL, 100, "text", NULL, 0));
  CHECK(!ArUiCatalog_Format(out, 0, "text", NULL, 0));
  CHECK(!ArUiCatalog_Format(out, sizeof(out), "text", NULL, 1));
  return failures ? 1 : 0;
}
