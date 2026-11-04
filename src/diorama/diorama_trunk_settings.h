#ifndef AR_DIORAMA_TRUNK_SETTINGS_H
#define AR_DIORAMA_TRUNK_SETTINGS_H
#include <stdbool.h>

/* A room-owned enclosure. Ordinary BG fill, skybox and plane-shape policies
 * do not describe this connected surface and cannot override it. */
typedef struct DioramaTrunkSettings {
  bool set, enabled;
  float depth, recess, wrap, gust, frost, bark;
} DioramaTrunkSettings;

static inline bool DioramaTrunk_Room(unsigned group, unsigned room) {
  return group == 6 && (room == 6 || room == 7);
}

static inline DioramaTrunkSettings DioramaTrunk_Default(void) {
  return (DioramaTrunkSettings){.enabled=true, .depth=.38f, .recess=.24f,
      .wrap=1, .gust=1, .frost=1, .bark=1};
}
#endif
