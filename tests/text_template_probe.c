/* A ROM/backend-free conformance probe. Both language implementations consume
 * tests/fixtures/text_template.json; this process reports the C parser result.
 */
#include "localization/text_template.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct OutputRun {
  ArTextTemplateRun run;
  size_t offset;
} OutputRun;

typedef struct Output {
  char text[kArTextTemplateMaximumBytes + 1];
  size_t bytes;
  OutputRun runs[kArTextTemplateMaximumRuns];
  size_t count;
} Output;

static bool Collect(void *context, const ArTextTemplateRun *run) {
  Output *out = context;
  if (run->bytes > sizeof(out->text) - 1 - out->bytes)
    return false;
  if (run->starts_run) {
    if (out->count == kArTextTemplateMaximumRuns)
      return false;
    out->runs[out->count++] = (OutputRun){.run = *run, .offset = out->bytes};
  } else {
    out->runs[out->count - 1].run.bytes += run->bytes;
  }
  memcpy(out->text + out->bytes, run->text, run->bytes);
  out->bytes += run->bytes;
  return true;
}

static void JsonString(const char *text, size_t bytes) {
  putchar('"');
  for (size_t i = 0; i < bytes; ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (c == '"' || c == '\\')
      printf("\\%c", c);
    else if (c < 32)
      printf("\\u%04x", c);
    else
      putchar(c);
  }
  putchar('"');
}

static void StringProperty(const char *key, const char *value, bool *comma) {
  if (!value[0])
    return;
  printf("%s\"%s\":", *comma ? "," : "", key);
  JsonString(value, strlen(value));
  *comma = true;
}

int main(void) {
  char *input = malloc(kArTextTemplateMaximumBytes + 2);
  Output *out = calloc(1, sizeof(*out));
  if (!input || !out)
    return 2;
  const size_t bytes = fread(input, 1, kArTextTemplateMaximumBytes + 1, stdin);
  if (ferror(stdin))
    return 2;
  ArTextTemplateError error;
  if (!ArTextTemplate_ParseInline(input, bytes, Collect, out, &error)) {
    printf("{\"invalid\":true,\"offset\":%zu,\"error\":", error.offset);
    JsonString(error.message, strlen(error.message));
    puts("}");
  } else {
    fputs("{\"runs\":[", stdout);
    for (size_t i = 0; i < out->count; ++i) {
      const OutputRun *item = &out->runs[i];
      const ArTextTemplateRun *run = &item->run;
      printf("%s{\"%s\":", i ? "," : "", run->is_value ? "value" : "text");
      JsonString(out->text + item->offset, run->bytes);
      if (run->minimum_digits)
        printf(",\"minimum_digits\":%u", run->minimum_digits);
      printf(",\"source_offset\":%zu,\"style\":{", run->source_offset);
      bool comma = false;
      StringProperty("font", run->style.font, &comma);
      StringProperty("style", run->style.treatment, &comma);
      if (run->style.has_color) {
        printf("%s\"color\":\"#%06X\"", comma ? "," : "", run->style.color_rgb);
        comma = true;
      }
      if (run->style.scale_percent) {
        printf("%s\"scale\":%u", comma ? "," : "", run->style.scale_percent);
        comma = true;
      }
      if (run->style.italic)
        printf("%s\"italic\":%u", comma ? "," : "", run->style.italic);
      fputs("}}", stdout);
    }
    puts("]}");
  }
  free(out);
  free(input);
  return 0;
}
