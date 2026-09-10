#ifndef AR_LOCALIZATION_FRAME_H
#define AR_LOCALIZATION_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/enhanced_text_settings.h"
#include "localization/text_rasterizer.h"
#include "localization/text_presentation.h"
#include "render/text_cell_record.h"

#define AR_LOCALIZATION_FRAME_ABI_VERSION UINT32_C(13)

enum {
  kArLocalizationFrameTextCapacity = 16 * 1024,
  kArLocalizationFrameFontStackCapacity = 128,
  kArLocalizationFrameFontPathCapacity = 512,
  kArLocalizationFrameLocaleCapacity = 33,
  kArLocalizationFrameNativePreserveCapacity = 8,
  kArLocalizationFrameIndicatorCapacity = 8,
  kArLocalizationFrameInlineObjectCapacity = 32,
  kArLocalizationFrameNameCursorExtent = 8,
  kArLocalizationFrameNameCursorPixels = 8 * 8,
  kArLocalizationArtworkPixels = 16 * 8,
};

typedef enum ArLocalizationArtworkKind {
  kArLocalizationArtwork_Continue = 0,
  kArLocalizationArtwork_Life,
  kArLocalizationArtwork_Population,
  kArLocalizationArtwork_SpeedDirection,
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

typedef enum ArLocalizationTextLayoutKind {
  kArLocalizationTextLayout_Flow = 0,
  kArLocalizationTextLayout_StatusCities,
  kArLocalizationTextLayout_StatusScore,
  kArLocalizationTextLayout_StatusMaster,
  kArLocalizationTextLayout_FixedRows,
  kArLocalizationTextLayout_MessageSpeed,
  kArLocalizationTextLayout_DialogueWindow,
  /* One fitted line, anchored to the leading edge and vertically centered
   * inside its claim. It must never wrap into neighboring native content. */
  kArLocalizationTextLayout_SingleLineLabel,
} ArLocalizationTextLayoutKind;

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
  ArTextDirection direction;
  ArLocalizationTextLayoutKind layout;
  uint8_t native_font_pixels;
  uint8_t native_preserve_count;
  uint8_t inline_object_offset;
  uint8_t inline_object_count;
  ArTextCellRegion native_preserves[
      kArLocalizationFrameNativePreserveCapacity];
} ArLocalizationTextSnapshot;

/* Pointer-free game-thread -> presenter contract. Text shares one bounded
 * UTF-8 pool, while cell records bind each snapshot to its exact BG target. */
typedef struct ArLocalizationFrame {
  uint32_t struct_size;
  uint32_t abi_version;
  ArTextCellRecordSet cells;
  ArLocalizationTextSnapshot snapshots[kArTextCellRecordCapacity];
  uint8_t snapshot_count;
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
  char locale[kArLocalizationFrameLocaleCapacity];
  char font_stack_id[kArLocalizationFrameFontStackCapacity];
  char primary_font_path[kArLocalizationFrameFontPathCapacity];
  char fallback_font_paths[kArTextPresentationMaximumFallbackFonts]
                          [kArLocalizationFrameFontPathCapacity];
  uint8_t fallback_font_count;
  uint64_t font_revision;
  ArEnhancedTextSettings settings;
  /* Zero for observational/fixed text; only scheduled dialogue needs execution
   * feedback when the presentation path falls back to native pixels. */
  uint64_t dialogue_ticket;
  uint32_t dialogue_surface_id;
} ArLocalizationFrame;

void ArLocalizationFrame_Reset(ArLocalizationFrame *frame);
bool ArLocalizationFrame_AddDialogueWindow(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes, uint32_t revealed_utf8_bytes,
    uint32_t cluster_count, uint64_t source_revision,
    ArTextDirection direction, uint8_t native_font_pixels);
bool ArLocalizationFrame_SetFont(ArLocalizationFrame *frame,
                                 const char *locale,
                                 const char *font_stack_id,
                                 const char *primary_font_path,
                                 uint64_t font_revision,
                                 const ArEnhancedTextSettings *settings);
/* SetFont clears previous fallbacks; attach this ordered stack afterwards.
 * Paths are copied transactionally into the pointer-free frame. */
bool ArLocalizationFrame_SetFallbackFonts(
    ArLocalizationFrame *frame, const char *const *paths, size_t count);
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
bool ArLocalizationFrame_AddIndicator(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArLocalizationIndicatorKind kind, ArTextCellRegion region);
const char *ArLocalizationFrame_GetText(
    const ArLocalizationFrame *frame, uint8_t snapshot_index,
    size_t *utf8_bytes);

#endif /* AR_LOCALIZATION_FRAME_H */
