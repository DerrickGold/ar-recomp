#ifndef AR_TEXT_TEMPLATE_H
#define AR_TEXT_TEMPLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kArTextTemplateMaximumDepth = 16,
  kArTextTemplateMaximumRuns = 4096,
  kArTextTemplateMaximumBytes = 256 * 1024,
  kArTextTemplateRoleCapacity = 97,
  kArTextTemplateMinimumScale = 25,
  kArTextTemplateMaximumScale = 400,
};

/* Authored origin, copied across host/render boundaries. The requested ID
 * may be an alias; resolved_id and source location name the actual body. */
typedef struct ArTextSourceOrigin {
  char source_path[1024];
  char message_id[256];
  char resolved_id[256];
  char package_id[97];
  uint32_t source_line;
} ArTextSourceOrigin;

typedef enum ArLanguagePlaceholderKind {
  kArLanguagePlaceholder_Unknown = 0,
  kArLanguagePlaceholder_LocalizedText,
  kArLanguagePlaceholder_LocalizedTerm,
  kArLanguagePlaceholder_Number,
  kArLanguagePlaceholder_Icon,
} ArLanguagePlaceholderKind;

/* Inline overrides only. Empty/zero fields inherit message defaults. Nested
 * scales replace, rather than multiply, the enclosing inline scale. */
typedef struct ArTextTemplateStyle {
  char font[kArTextTemplateRoleCapacity];
  char treatment[kArTextTemplateRoleCapacity];
  uint32_t color_rgb;
  uint16_t scale_percent;
  uint8_t italic; /* 0=inherited, 1=upright, 2=italic */
  bool has_color;
} ArTextTemplateStyle;

typedef struct ArTextTemplateSpan {
  uint32_t start, end; /* logical UTF-8 offsets after value substitution */
  ArTextTemplateStyle style;
} ArTextTemplateSpan;

typedef enum ArTextInkKind {
  kArTextInk_None = 0,
  kArTextInk_Solid,
  kArTextInk_Binding,
} ArTextInkKind;

/* A binding is an opaque host-provided ink name. The reusable core never
 * samples palettes or substitutes an unrelated surface's colors. */
typedef struct ArTextInk {
  ArTextInkKind kind;
  uint32_t rgb;
  char binding[kArTextTemplateRoleCapacity];
} ArTextInk;

typedef struct ArTextTreatment {
  char name[kArTextTemplateRoleCapacity];
  ArTextInk band, body, shadow;
  bool keyline_shadow;
} ArTextTreatment;

/* Fully resolved appearance, suitable for a frame snapshot or cache key.
 * scale_basis is message percent * inline percent (10000 means 100%). Keep
 * that product until final pixel rounding so nested scopes never compound. */
typedef struct ArTextRunAppearance {
  char font_role[kArTextTemplateRoleCapacity];
  uint32_t scale_basis;
  uint32_t band_rgb, body_rgb, shadow_rgb;
  bool shadow_enabled, keyline_shadow, italic, slant_ascii_numerals;
} ArTextRunAppearance;

typedef struct ArTextAppearanceSpan {
  uint32_t start, end;
  ArTextRunAppearance appearance;
} ArTextAppearanceSpan;

typedef const ArTextTreatment *(*ArTextFindTreatment)(void *context,
                                                      const char *name);
typedef bool (*ArTextResolveInk)(void *context, const char *binding,
                                 uint32_t *rgb);

typedef struct ArTextAppearanceBindings {
  void *context;
  ArTextFindTreatment find_treatment;
  ArTextResolveInk resolve_ink;
} ArTextAppearanceBindings;

bool ArTextTemplate_StyleEqual(const ArTextTemplateStyle *a,
                               const ArTextTemplateStyle *b);

typedef struct ArTextTemplateRun {
  /* A literal or a value name, borrowed only during emit. Literal pieces may
   * be adjacent after decoding escapes. Values are never reparsed as markup. */
  const char *text;
  size_t bytes;
  size_t source_offset;
  bool is_value;
  bool starts_run;
  uint8_t minimum_digits;
  ArTextTemplateStyle style;
} ArTextTemplateRun;

typedef struct ArTextTemplateError {
  size_t offset;
  char message[192];
} ArTextTemplateError;

bool ArTextTreatment_SetProperty(ArTextTreatment *treatment, const char *name,
                                 const char *value, ArTextTemplateError *error);

/* Failure leaves the output unchanged. A color override replaces both main
 * inks and retains the selected treatment's shadow. Empty roles select body;
 * an absent treatment is plain white. Native ink availability is host policy.
 */
bool ArTextTemplate_ResolveAppearance(const ArTextTemplateStyle *defaults,
                                      const ArTextTemplateStyle *span,
                                      bool slant_ascii_numerals,
                                      const ArTextAppearanceBindings *bindings,
                                      ArTextRunAppearance *appearance,
                                      ArTextTemplateError *error);

typedef bool (*ArTextTemplateEmit)(void *context, const ArTextTemplateRun *run);

/* Parse one UTF-8 segment bounded by explicit structure. Tags must balance
 * within it. No game catalog, renderer, filesystem, or font dependency.
 * A failure can follow emitted runs: callers stage results until success.
 * emit may be NULL to validate without producing output. */
bool ArTextTemplate_ParseInline(const char *text, size_t bytes,
                                ArTextTemplateEmit emit, void *context,
                                ArTextTemplateError *error);

/* Shared validation for inline attributes and message presentation defaults.
 * Failure leaves style unchanged. name/value are NUL-terminated ASCII. */
bool ArTextTemplate_SetProperty(ArTextTemplateStyle *style, const char *name,
                                const char *value, ArTextTemplateError *error);

#endif
