#ifndef AR2_SCENES_H
#define AR2_SCENES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Ar2SceneProfile {
  uint8_t scene, entrance;
  bool production;
} Ar2SceneProfile;

/* Shared by the globe and private module; neither needs the other's runtime. */
static inline const Ar2SceneProfile *Ar2Scenes_Find(unsigned scene, bool research) {
  enum { Production = true, Research = false };
  static const Ar2SceneProfile profiles[] = {
#define AR2_SCENE(scene, entrance, availability) {scene, entrance, availability},
#include "ar2_scenes.inc"
#undef AR2_SCENE
  };
  for (size_t i = 0; i < sizeof(profiles) / sizeof(*profiles); ++i)
    if (profiles[i].scene == scene && (research || profiles[i].production)) return &profiles[i];
  return NULL;
}

static inline bool Ar2Scenes_IsEntrance(unsigned scene, bool research) {
  const Ar2SceneProfile *profile = Ar2Scenes_Find(scene, research);
  return profile && profile->entrance == scene;
}
#endif
