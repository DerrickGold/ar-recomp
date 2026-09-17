#ifndef AR_UPLOAD_RECT_RUN_H
#define AR_UPLOAD_RECT_RUN_H

#include <stdbool.h>

#include "render_types.h"

/* Coalesces a stream of dirty rectangles for ONE texture into as few texture
 * updates as possible.
 *
 * Every update call carries a fixed backend cost on top of its bytes, and on
 * SDL's GPU renderer that cost is not small: each texture update creates, maps
 * and releases a fresh transfer buffer (GPU_UpdateTextureInternal in SDL
 * 3.4.12 and 3.4.16), which the Direct3D 12 backend allocates as its own
 * CreateCommittedResource. Row-banded dirty trackers hand out one
 * rectangle per distinct row span, so a Town 3D frame issued 12 canvas updates
 * covering 84% of the texture every frame and 95-155 one-row ground updates
 * every 8th frame (Aitos eruption replay, 2026-09-16) — while World 3D issued
 * four updates per present in total.
 *
 * A run absorbs the next rectangle whenever that re-sends at most
 * kUploadRectRunMaximumWastePixels already-clean pixels. Each merge removes
 * exactly one update, so the total re-sent area stays bounded by that allowance
 * times the updates saved. The allowance is a 128x128 region: deckbench-gpu
 * (2026-09-11) put the break-even at roughly 7.6k pixels per avoided call on
 * Mac/Metal (4.6 us/call, 6.6 GB/s) and 27k on Steam Deck/Vulkan (37.3 us/call,
 * 2.9 GB/s); Direct3D 12 per-call cost has not been measured.
 *
 * Merging re-sends pixels the texture already holds, so it is only valid when
 * the caller uploads from a buffer holding the COMPLETE current image, never
 * from a buffer that is valid only inside the dirty rectangles.
 *
 * Rectangles are expected in top-to-bottom order, as row-banded trackers emit
 * them; out-of-order or overlapping input stays correct (the run is a bounding
 * box) and only merges less well. */

enum { kUploadRectRunMaximumWastePixels = 128 * 128 };

typedef struct UploadRectRun {
  ArRenderRectI bounds;
  bool active;
} UploadRectRun;

/* Adds `next` to the run. Returns true when `next` could not join, in which
 * case *flush receives the finished rectangle to upload now and `next` starts
 * the new run. Empty rectangles are ignored. */
bool UploadRectRun_Add(UploadRectRun *run, ArRenderRectI next,
                       ArRenderRectI *flush);

/* Ends the stream. Returns true with the final rectangle to upload, or false
 * when nothing was added. The run is empty afterwards. */
bool UploadRectRun_Finish(UploadRectRun *run, ArRenderRectI *flush);

#endif /* AR_UPLOAD_RECT_RUN_H */
