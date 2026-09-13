#ifndef AR_LOCALIZATION_FRAME_H
#define AR_LOCALIZATION_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/enhanced_text_settings.h"
#include "localization/text_rasterizer.h"
#include "localization/text_presentation.h"
#include "localization/text_cell_record.h"
#include "localization/text_boundaries.h"

#define AR_LOCALIZATION_FRAME_ABI_VERSION UINT32_C(27)

enum {
  kArLocalizationFrameTextCapacity = 16 * 1024,
  kArLocalizationFrameFontStackCapacity = 128,
  kArLocalizationFrameLocaleCapacity = 33,
  kArLocalizationFrameNativePreserveCapacity = 8,
  kArLocalizationFrameIndicatorCapacity = 8,
  kArLocalizationFrameInlineObjectCapacity = 32,
  /* Screen-space records are intentionally scarce. Unlike tile-cell claims,
   * they are for positively identified native UI whose placement is already
   * expressed in authentic 256x224 pixels. */
  kArLocalizationFrameScreenTextCapacity = 4,
  kArLocalizationFrameNameCursorExtent = 8,
  kArLocalizationFrameNameCursorPixels = 8 * 8,
  kArLocalizationArtworkPixels = 16 * 8,
  /* Cells in one row of a grid layout, and rows shapes one grid may declare.
   * A grid is derived from the game's own menu geometry, so these bound what
   * the game may publish, not what the renderer can imagine. */
  kArLocalizationGridMaximumCells = 10,
  kArLocalizationGridMaximumRules = 16,
  kArLocalizationFrameGridCapacity = 4,
  /* Row rule wildcards. */
  kArLocalizationGridAnyLine = 255,
  kArLocalizationGridAnyFieldCount = 0,
  kArLocalizationFrameKeySeparatorCapacity = 7,
};

typedef enum ArLocalizationArtworkKind {
  kArLocalizationArtwork_Continue = 0,
  kArLocalizationArtwork_Life,
  kArLocalizationArtwork_Population,
  kArLocalizationArtwork_SpeedDirection,
  kArLocalizationArtwork_LabelFrameLeft,
  kArLocalizationArtwork_LabelFrameRight,
  /* The name-entry keyboard's own action glyphs. Absent when the keyboard is
   * not on screen; the presenter then draws its own shapes instead. */
  kArLocalizationArtwork_NameFinish,
  kArLocalizationArtwork_NameBackspace,
  kArLocalizationArtwork_Count,
} ArLocalizationArtworkKind;

typedef struct ArLocalizationArtwork {
  uint8_t width, height;
  bool valid;
  uint32_t argb[kArLocalizationArtworkPixels];
} ArLocalizationArtwork;

typedef enum ArLocalizationIndicatorKind {
  kArLocalizationIndicator_None = 0,
  kArLocalizationIndicator_DialogueContinue,
} ArLocalizationIndicatorKind;

typedef struct ArLocalizationIndicatorSnapshot {
  uint32_t surface_id;
  ArLocalizationIndicatorKind kind;
  ArTextCellRegion region;
} ArLocalizationIndicatorSnapshot;

typedef enum ArLocalizationInlineObjectKind {
  kArLocalizationInlineObject_None = 0,
  kArLocalizationInlineObject_StatusLife,
  kArLocalizationInlineObject_SelectionPointer,
  kArLocalizationInlineObject_NameBackspace,
  kArLocalizationInlineObject_NameFinish,
  kArLocalizationInlineObject_NameCursor,
  kArLocalizationInlineObject_StatusPopulation,
  kArLocalizationInlineObject_SpeedDirection,
  kArLocalizationInlineObject_NameFieldUnderline,
} ArLocalizationInlineObjectKind;

typedef struct ArLocalizationInlineObjectSnapshot {
  ArLocalizationInlineObjectKind kind;
  /* UTF-8 byte offset immediately after the width-reserving shaped cluster. */
  uint32_t end_utf8_byte;
} ArLocalizationInlineObjectSnapshot;

/* How the renderer presents a surface. These are rendering shapes, not game
 * screens: nothing here names a particular menu, and the renderer cannot tell
 * a status report from a message-speed scale. Cell geometry for Grid arrives
 * with the snapshot as an ArLocalizationTextGrid. */
typedef enum ArLocalizationTextLayoutKind {
  kArLocalizationTextLayout_Flow = 0,
  /* Fixed cells positioned by the accompanying grid description. */
  kArLocalizationTextLayout_Grid,
  kArLocalizationTextLayout_DialogueWindow,
  /* One fitted line, anchored to the leading edge and vertically centered
   * inside its claim. It must never wrap into neighboring native content. */
  kArLocalizationTextLayout_SingleLineLabel,
  /* Fitted one-line card, centered in a safe cell region (not word wrapped). */
  kArLocalizationTextLayout_CenteredLabel,
  /* Physical right edge, independent of Unicode paragraph direction. */
  kArLocalizationTextLayout_RightAlignedLabel,
  /* Physical left edge, independent of Unicode paragraph direction. */
  kArLocalizationTextLayout_LeftAlignedLabel,
  /* Centered fitted label with native artwork bookends, not font brackets. */
  kArLocalizationTextLayout_FramedLabel,
} ArLocalizationTextLayoutKind;

/* One cell of a row. Columns are native cell units relative to the owning
 * region, half-open [start, end); the renderer scales them into output pixels
 * and solves the fitting. Any native blank a right-aligned value keeps before
 * its neighbour is already part of `end`. */
typedef struct ArLocalizationTextCellRule {
  uint8_t start;
  uint8_t end;
  /* An ArTextHorizontalAlignment. Narrowed to a byte so this structure has no
   * padding: grids are compared and hashed whole, and padding a caller never
   * wrote would make two identical grids look different. */
  uint8_t alignment;
  /* Leading becomes trailing when the paragraph runs right to left. A cell
   * whose position is physical (a scale tick, an anchored value) clears this. */
  uint8_t follows_direction;
  /* Reserve a native pixel beside adjacent artwork on that side. */
  uint8_t gutter_leading;
  uint8_t gutter_trailing;
  /* Whole semantic value field, including locale-specific numeral scripts. */
  uint8_t italic;
} ArLocalizationTextCellRule;

/* Rules are searched in order; the first whose line range and row shape match
 * wins, so a specific rule precedes its general fallback. An unmatched row is
 * rejected, exactly as unknown geometry was before. */
typedef struct ArLocalizationTextRowRule {
  uint8_t first_line;
  uint8_t last_line;
  /* Row shape this rule describes, or kArLocalizationGridAnyFieldCount. */
  uint8_t field_count;
  uint8_t cell_count;
  /* The game draws this row itself -- a divider, a rule, native artwork. The
   * renderer claims nothing on it. */
  uint8_t native_reserved;
  /* This row's cells line up with the grid's shared fitted columns. */
  uint8_t shared_columns;
  ArLocalizationTextCellRule cells[kArLocalizationGridMaximumCells];
} ArLocalizationTextRowRule;

/* Renderer-neutral description of a fixed cell layout, published by the game
 * adapter with the snapshot it belongs to. */
/* Every member is a byte, so the whole description packs without padding and
 * can be compared and hashed as bytes. */
typedef struct ArLocalizationTextGrid {
  uint8_t rule_count;
  /* Rows marked shared_columns are measured together and share one set of
   * fitted column widths. Zero disables shared fitting. */
  uint8_t shared_column_count;
  /* Line whose cell rule seeds the preferred shared widths. */
  uint8_t shared_template_line;
  /* Native rows one text row occupies. */
  uint8_t row_height;
  /* Tight single-row cells may discard transparent top/bottom padding. */
  uint8_t crop_rows;
  /* Center each row's ink vertically inside its declared row height. */
  uint8_t center_rows;
  ArLocalizationTextRowRule rules[kArLocalizationGridMaximumRules];
} ArLocalizationTextGrid;

/* Effective content language, independent of the selected presentation font
 * stack. A partial pack can publish several source languages in one frame. */
typedef struct ArLocalizationTextLanguage {
  char locale[kArLocalizationFrameLocaleCapacity];
  ArTextDirection direction;
} ArLocalizationTextLanguage;

bool ArLocalizationTextLanguage_IsValid(const ArLocalizationTextLanguage *language);

typedef struct ArLocalizationTextSnapshot {
  uint32_t surface_id;
  uint32_t utf8_offset;
  /* Zero bytes/clusters is an intentional blank replacement: retain the
   * cell claim and native preserves/indicators without rasterizing text. */
  uint32_t utf8_bytes;
  uint32_t revealed_cluster_count;
  uint32_t cluster_count;
  /* DialogueWindow uses a logical byte boundary so retained hard breaks and
   * shaped ligatures do not consume the wrong number of reveal steps. */
  uint32_t revealed_utf8_bytes;
  uint64_t source_revision;
  uint32_t style_id;
  uint32_t band_rgb;
  uint32_t body_rgb;
  /* The game's third ink, drawn beside every stroke. Published separately
   * from the two band endpoints because it is not part of the gradient: it
   * sits behind the letter rather than in it. Disabled when the surface has
   * no such shade. */
  uint32_t shadow_rgb;
  bool shadow_enabled;
  ArTextShadowShape shadow_shape;
  bool italic;
  bool slant_ascii_numerals;
  uint32_t accent_end_utf8_byte;
  uint32_t accent_rgb;
  /* Logical (8-pixel-cell) physical gutters, scaled with the claim, independent
   * of shaping direction. Masking still covers the complete native field. */
  uint8_t left_inset_pixels;
  uint8_t right_inset_pixels;
  uint8_t top_inset_pixels;
  ArLocalizationTextLanguage language;
  uint16_t bidi_span_offset, bidi_span_count;
  ArLocalizationTextLayoutKind layout;
  /* One-based index into the frame's grid table; zero for a non-grid layout. */
  uint8_t grid_index;
  /* Blank the game reserves between selectable keys, when this surface has
   * any. A selector object is sized to the narrowest run of it, so the arrow
   * cannot cover the previous key. Empty when the surface has no keys; the
   * renderer never assumes a particular spacing character. */
  uint8_t key_separator_bytes;
  char key_separator[kArLocalizationFrameKeySeparatorCapacity];
  /* Uniform key pitch. Non-zero means the last `key_trailing_lines` lines of
   * this text are a row of `key_columns` equal cells spanning the claim, and
   * each key is centred in its own cell rather than placed by the advance the
   * shaper accumulated. Proportional glyphs otherwise make every line a
   * different width, so columns that the native fixed cell grid kept aligned
   * drift apart from line to line. The text is still shaped once, so every
   * key keeps one glyph size. Zero columns leaves the text flowed. */
  uint8_t key_columns;
  uint8_t key_trailing_lines;
  /* Native cells one key occupies, counted in the region's own columns. The
   * retail keyboard gives every key two: a blank for the selector, then the
   * glyph. Stating it keeps the pitch equal to the native one even when the
   * claim is wider than the keyboard, and leaves the selector the same room
   * the game did. */
  uint8_t key_cell_columns;
  uint8_t native_font_pixels;
  uint8_t native_preserve_count;
  uint8_t inline_object_offset;
  uint8_t inline_object_count;
  ArTextCellRegion native_preserves[
      kArLocalizationFrameNativePreserveCapacity];
} ArLocalizationTextSnapshot;

/* A renderer-neutral claim in the game's authentic screen coordinate space.
 * This is separate from ArTextCellRecord because it owns no BG/tilemap cells:
 * callers must not disguise OBJ or host UI as a tilemap destination. */
typedef struct ArLocalizationScreenTextRecord {
  uint32_t surface_id;
  uint16_t x, y;
  uint16_t width, height;
  uint8_t snapshot_slot;
} ArLocalizationScreenTextRecord;

/* Pointer-free game-thread -> presenter contract. Text shares one bounded
 * UTF-8 pool, while cell records bind each snapshot to its exact BG target.
 * Font IDs are borrowed process-local identities, not owning leases. Retained
 * frames can use an already pinned stack; if its registration and backend have
 * both retired, presentation falls back to the captured native pixels. */
typedef struct ArLocalizationFrame {
  uint32_t struct_size;
  uint32_t abi_version;
  ArTextCellRecordSet cells;
  ArLocalizationScreenTextRecord
      screen_texts[kArLocalizationFrameScreenTextCapacity];
  uint8_t screen_text_count;
  ArLocalizationTextSnapshot snapshots[kArTextCellRecordCapacity];
  uint8_t snapshot_count;
  ArTextBidiSpans bidi;
  /* Interned grid descriptions; snapshots reference them by index so a report
   * shared by several surfaces is published once. */
  ArLocalizationTextGrid grids[kArLocalizationFrameGridCapacity];
  uint8_t grid_count;
  ArLocalizationIndicatorSnapshot
      indicators[kArLocalizationFrameIndicatorCapacity];
  uint8_t indicator_count;
  ArLocalizationInlineObjectSnapshot
      inline_objects[kArLocalizationFrameInlineObjectCapacity];
  uint8_t inline_object_count;
  /* Original cursor artwork, decoded by the game adapter. Packed ARGB8888,
   * transparent background; no borrowed memory or ROM identity crosses here. */
  bool name_cursor_valid;
  uint32_t name_cursor_argb[kArLocalizationFrameNameCursorPixels];
  ArLocalizationArtwork artwork[kArLocalizationArtwork_Count];
  uint32_t text_bytes;
  char text[kArLocalizationFrameTextCapacity];
  /* Authored grid delimiters, indexed by absolute byte in the shared pool. */
  uint8_t structural_boundaries[AR_TEXT_BOUNDARY_BYTES(kArLocalizationFrameTextCapacity)];
  char locale[kArLocalizationFrameLocaleCapacity];
  char font_stack_id[kArLocalizationFrameFontStackCapacity];
  ArFontResourceId primary_font;
  ArFontResourceId fallback_fonts[kArTextPresentationMaximumFallbackFonts];
  uint8_t fallback_font_count;
  uint64_t font_revision;
  ArEnhancedTextSettings settings;
  /* Zero for observational/fixed text; only scheduled dialogue needs execution
   * feedback when the presentation path falls back to native pixels. */
  uint64_t dialogue_ticket;
  uint32_t dialogue_surface_id;
} ArLocalizationFrame;

void ArLocalizationFrame_Reset(ArLocalizationFrame *frame);
/* Same, for storage the caller has already zeroed -- a freshly cleared frame
 * slot. Stamps the header without clearing tens of kilobytes a second time.
 * Passing anything else leaves stale content behind. */
void ArLocalizationFrame_InitCleared(ArLocalizationFrame *frame);
/* Validate a complete pointer-free frame before a consumer follows any of its
 * bounded pool indices. Cleared frames are valid; populated frames must carry
 * a configured font and mutually consistent snapshots/cell ownership. */
bool ArLocalizationFrame_IsValid(const ArLocalizationFrame *frame);
bool ArLocalizationFrame_AddDialogueWindow(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes, uint32_t revealed_utf8_bytes,
    uint32_t cluster_count, uint64_t source_revision,
    ArTextDirection direction, uint8_t native_font_pixels);
/* Dialogue variant that preserves compiler-authored presentation boundaries.
 * A boundary on an ASCII space is a preferred native row break; the text
 * backend decides whether to keep it after measuring the proportional font. */
bool ArLocalizationFrame_AddStructuredDialogueWindow(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes, uint32_t revealed_utf8_bytes,
    uint32_t cluster_count, uint64_t source_revision,
    ArTextDirection direction, uint8_t native_font_pixels,
    const uint8_t *structural_boundaries);
bool ArLocalizationFrame_SetFont(ArLocalizationFrame *frame,
                                 const char *locale,
                                 const char *font_stack_id,
                                 ArFontResourceId primary_font,
                                 uint64_t font_revision,
                                 const ArEnhancedTextSettings *settings);
/* SetFont clears previous fallbacks; attach this ordered stack afterwards.
 * Resource IDs are copied transactionally into the pointer-free frame. */
bool ArLocalizationFrame_SetFallbackFonts(
    ArLocalizationFrame *frame, const ArFontResourceId *fonts, size_t count);
/* Override the last added snapshot's effective language. AddText otherwise
 * copies SetFont's default locale and the explicit direction argument. Does
 * not change fonts or other snapshots; failure leaves the frame untouched. */
bool ArLocalizationFrame_SetTextLanguage(
    ArLocalizationFrame *frame, const ArLocalizationTextLanguage *language);
bool ArLocalizationFrame_SetTextBidiSpans(
    ArLocalizationFrame *frame, const ArTextBidiSpans *spans);
bool ArLocalizationFrame_AddText(ArLocalizationFrame *frame,
                                 uint32_t surface_id,
                                 ArTextCellDestination destination,
                                 ArTextCellRegion region,
                                 const char *utf8, size_t utf8_bytes,
                                 uint32_t revealed_cluster_count,
                                 uint32_t cluster_count,
                                 uint64_t source_revision,
                                 ArTextDirection direction,
                                 uint8_t native_font_pixels,
                                 const ArTextCellRegion *native_preserves,
                                 uint8_t native_preserve_count);
bool ArLocalizationFrame_AddTextWithObjects(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels,
    const ArTextCellRegion *native_preserves,
    uint8_t native_preserve_count,
    const ArLocalizationInlineObjectSnapshot *inline_objects,
    uint8_t inline_object_count);
/* Publishes fixed text whose native owner is a screen-space OBJ/host surface,
 * not a BG tilemap. Coordinates are authentic game pixels and are projected
 * by the owning presentation path. Native pixels remain the fallback until
 * the renderer successfully prepares this exact record. */
bool ArLocalizationFrame_AddScreenText(
    ArLocalizationFrame *frame, uint32_t surface_id,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels, ArLocalizationTextLayoutKind layout);
/* Grid layouts publish their own cell geometry: the renderer positions cells
 * from `grid` and never needs to know which menu it is drawing. The grid is
 * copied and interned; identical grids share one table entry. Boundaries are
 * copied from a bitmap relative to utf8. NULL explicitly means literal text
 * (all pipes/newlines are authored); resolved values must supply their map. */
bool ArLocalizationFrame_AddTextWithGrid(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels, const ArLocalizationTextGrid *grid,
    const uint8_t *structural_boundaries,
    const ArTextCellRegion *native_preserves,
    uint8_t native_preserve_count,
    const ArLocalizationInlineObjectSnapshot *inline_objects,
    uint8_t inline_object_count);
/* Non-grid layouts only; a Grid layout without geometry is rejected. */
bool ArLocalizationFrame_AddTextWithObjectsAndLayout(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels, ArLocalizationTextLayoutKind layout,
    const ArTextCellRegion *native_preserves,
    uint8_t native_preserve_count,
    const ArLocalizationInlineObjectSnapshot *inline_objects,
    uint8_t inline_object_count);
/* Indicators are semantic, fixed-cell UI objects owned by an existing text
 * surface. They carry no backend handle or ROM tile identity. */
/* Declares the blank between selectable keys on the most recently added
 * surface. Call it straight after publishing a keyboard. */
bool ArLocalizationFrame_SetKeySeparator(ArLocalizationFrame *frame,
                                         const char *utf8, size_t utf8_bytes);
/* Declares a uniform key pitch on the most recently added surface: its last
 * `trailing_lines` lines each hold `columns` keys, and every key occupies
 * `cell_columns` of the region's native columns. Call it straight after
 * publishing a keyboard whose native rows were a fixed grid. */
bool ArLocalizationFrame_SetKeyGrid(ArLocalizationFrame *frame,
                                    uint8_t columns, uint8_t trailing_lines,
                                    uint8_t cell_columns);
bool ArLocalizationFrame_AddIndicator(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArLocalizationIndicatorKind kind, ArTextCellRegion region);
/* Cell rule for one parsed row, or NULL when the grid does not describe it. */
const ArLocalizationTextRowRule *ArLocalizationGrid_FindRow(
    const ArLocalizationTextGrid *grid, unsigned line, unsigned field_count);
const ArLocalizationTextGrid *ArLocalizationFrame_GetGrid(
    const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot);
const ArLocalizationScreenTextRecord *ArLocalizationFrame_FindScreenText(
    const ArLocalizationFrame *frame, uint32_t surface_id);

const char *ArLocalizationFrame_GetText(
    const ArLocalizationFrame *frame, uint8_t snapshot_index,
    size_t *utf8_bytes);

#endif /* AR_LOCALIZATION_FRAME_H */
