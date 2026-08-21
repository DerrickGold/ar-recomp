#ifndef AR_PRESENTATION_UPLOAD_MIRROR_H
#define AR_PRESENTATION_UPLOAD_MIRROR_H

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>

/* Exact render-thread-owned mirror for a rectangular ARGB8888 texture region.
 * Texture identity and destination placement are part of the key: matching
 * bytes must never suppress the first upload to a recreated or relocated
 * surface. */
typedef struct PresentationUploadMirror {
  uint8_t *pixels;
  SDL_Texture *texture;
  int width, height;
  int destination_x, destination_y;
  bool valid;
} PresentationUploadMirror;

typedef struct PresentationUploadResult {
  SDL_Rect destination;
  uint64_t uploaded_bytes;
  bool changed;
} PresentationUploadResult;

void PresentationUploadMirror_Reset(PresentationUploadMirror *mirror);

/* Returns whether two equally-sized ARGB8888 regions differ and, when they
 * do, the smallest pixel-aligned bounding rectangle containing every changed
 * byte. Pitches are expressed in bytes. Invalid input fails closed as a full
 * dirty rectangle whenever width/height are positive. */
bool PresentationUploadMirror_FindDirtyRect(
    const uint8_t *current, int current_pitch,
    const uint8_t *previous, int previous_pitch,
    int width, int height, SDL_Rect *dirty);

/* Uploads only the exact dirty rectangle. A mirror allocation failure falls
 * back to a complete upload; an SDL failure invalidates the mirror so the next
 * call retries the complete region. Returning true includes the no-change
 * case, where result.changed is false and no SDL call was made. */
bool PresentationUploadMirror_UploadArgb8888(
    PresentationUploadMirror *mirror, SDL_Texture *texture,
    const uint8_t *source, int width, int height, int source_pitch,
    int destination_x, int destination_y, PresentationUploadResult *result);

#endif /* AR_PRESENTATION_UPLOAD_MIRROR_H */
