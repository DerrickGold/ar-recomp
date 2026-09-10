#ifndef AR_LOCALIZATION_FRAME_H
#define AR_LOCALIZATION_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/enhanced_text_settings.h"
#include "localization/text_rasterizer.h"
#include "render/text_cell_record.h"

#define AR_LOCALIZATION_FRAME_ABI_VERSION UINT32_C(1)

enum {
  kArLocalizationFrameTextCapacity = 16 * 1024,
  kArLocalizationFrameFontStackCapacity = 128,
  kArLocalizationFrameFontPathCapacity = 512,
  kArLocalizationFrameLocaleCapacity = 33,
  kArLocalizationFrameNativePreserveCapacity = 8,
};

typedef struct ArLocalizationTextSnapshot {
  uint32_t surface_id;
  uint32_t utf8_offset;
  uint32_t utf8_bytes;
  uint32_t revealed_cluster_count;
  uint32_t cluster_count;
  uint64_t source_revision;
  ArTextDirection direction;
  uint8_t native_font_pixels;
  uint8_t native_preserve_count;
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
  uint32_t text_bytes;
  char text[kArLocalizationFrameTextCapacity];
  char locale[kArLocalizationFrameLocaleCapacity];
  char font_stack_id[kArLocalizationFrameFontStackCapacity];
  char primary_font_path[kArLocalizationFrameFontPathCapacity];
  uint64_t font_revision;
  ArEnhancedTextSettings settings;
} ArLocalizationFrame;

void ArLocalizationFrame_Reset(ArLocalizationFrame *frame);
bool ArLocalizationFrame_SetFont(ArLocalizationFrame *frame,
                                 const char *locale,
                                 const char *font_stack_id,
                                 const char *primary_font_path,
                                 uint64_t font_revision,
                                 const ArEnhancedTextSettings *settings);
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
const char *ArLocalizationFrame_GetText(
    const ArLocalizationFrame *frame, uint8_t snapshot_index,
    size_t *utf8_bytes);

#endif /* AR_LOCALIZATION_FRAME_H */
