#ifndef AR_IMPORTED_GAME_API_H
#define AR_IMPORTED_GAME_API_H
/* Private native-game module ABI. The host lends immutable ROM bytes until
 * close; step lends surfaces/PCM until the next step. All calls are synchronous
 * on one owner thread. No SDL objects or runner globals cross this boundary. */
#include "diorama/diorama.h"

#define AR_IMPORTED_GAME_ABI 4u
#ifdef _WIN32
#define AR_IMPORTED_GAME_EXPORT __declspec(dllexport)
#else
#define AR_IMPORTED_GAME_EXPORT __attribute__((visibility("default")))
#endif
enum { AR2_CAPTURE_PITCH = 640, AR2_CAPTURE_HEIGHT = 352 };
typedef struct Ar2VideoFrame {
  const uint32_t *main;
  const uint32_t *planes[kDioramaPlane_Count];
  const uint32_t *hud;
  const uint32_t *screen_objects;
  int width, height, render_extra, display_extra;
  int camera_x, camera_y, world_width, world_height;
  int bg2_camera_x, bg2_camera_y, bg2_world_height;
  bool level;
} Ar2VideoFrame;

typedef struct ArImportedGameFrame {
  Ar2VideoFrame video;
  const int16_t *pcm;
  unsigned pcm_frames, frame;
  uint16_t scene, player_x, player_y, health, lives;
  /* Terminal events have no borrowed video/audio. Close or reopen the module;
   * further steps repeat the event without advancing the native game. */
  uint32_t event;
} ArImportedGameFrame;

enum {
  kArImportedGameEvent_None,
  kArImportedGameEvent_ReturnToWorld,
};

typedef struct ArImportedGameApi {
  uint32_t version, size;
  bool (*open)(const uint8_t *rom, size_t size, uint8_t stage);
  bool (*step)(uint32_t input, int extra_columns, bool diorama, ArImportedGameFrame *out);
  void (*close)(void);
  const char *(*error)(void);
  /* Synchronous diagnostic files in the shared snapshot format. No runner
   * pointers cross the boundary; valid even after a failed native step. */
  bool (*snapshot)(const char *prefix);
} ArImportedGameApi;

typedef const ArImportedGameApi *(*ArImportedGameGetApi)(uint32_t version);
#endif
