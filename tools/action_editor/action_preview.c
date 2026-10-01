/* Narrow baseline WASM API. All scene decode/raster/color math is production C.
 * Module-owned bounded buffers avoid borrowed JS memory or per-frame malloc. */
#include "action/action_scene_snapshot.h"
#include "deterministic_hash.h"
#include <string.h>

static uint8_t s_input[kActionSceneSnapshotMaxBytes];
static ActionSceneSnapshot s_active, s_candidate;
static ActionRoomSceneFrameState s_frame;
static uint32_t s_pixels[kActionRoomSceneFramePixels];
static uint8_t s_rgba[kActionRoomSceneFramePixels * 4];
static bool s_loaded;
static uint32_t s_hash;

unsigned ActionPreview_Version(void) { return kActionSceneSnapshotVersion; }
uint8_t *ActionPreview_Input(void) { return s_input; }
unsigned ActionPreview_Capacity(void) { return sizeof(s_input); }
uint32_t ActionPreview_Hash(void) { return s_hash; }
void ActionPreview_Reset(void) { s_loaded = false; s_hash = 0; }

int ActionPreview_Load(unsigned bytes) {
  if (!ActionSceneSnapshot_Decode(s_input, bytes, &s_candidate) ||
      !ActionRoomScene_BuildFrameState(&s_candidate.scene, &s_candidate.frame, &s_frame) ||
      !ActionRoomScene_RenderNativeFrame(&s_candidate.scene, &s_frame,
          s_pixels, kActionRoomSceneFramePixels)) return 0;
  s_active = s_candidate;
  s_loaded = true;
  return 1;
}

const uint8_t *ActionPreview_Render(int x, int y, uint32_t frame) {
  if (!s_loaded || x < -32768 || x > 65535 || y < -32768 || y > 65535) return NULL;
  ActionRoomSceneFrameRequest request = s_active.frame;
  request.camera_x = x;
  request.camera_y = y;
  request.game_frame = frame;
  if (!ActionRoomScene_BuildFrameState(&s_active.scene, &request, &s_frame) ||
      !ActionRoomScene_RenderNativeFrame(&s_active.scene, &s_frame,
          s_pixels, kActionRoomSceneFramePixels)) return NULL;
  s_hash = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  for (unsigned i = 0; i < kActionRoomSceneFramePixels; ++i) {
    const uint32_t c = s_pixels[i];
    s_hash = DeterministicHash_Fnv1a32Word(s_hash, c);
    s_rgba[i * 4] = (uint8_t)(c >> 16);
    s_rgba[i * 4 + 1] = (uint8_t)(c >> 8);
    s_rgba[i * 4 + 2] = (uint8_t)c;
    s_rgba[i * 4 + 3] = (uint8_t)(c >> 24);
  }
  return s_rgba;
}
