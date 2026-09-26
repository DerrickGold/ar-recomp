#ifndef AR_ACTRAISER_SCENE_MUSIC_H
#define AR_ACTRAISER_SCENE_MUSIC_H
/* ActRaiser scene-music hook: picks each scene's song under the regional music
 * rules.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_scene_music_test.c */
#include "snesrecomp/game/cpu.h"
bool ActRaiser_SceneMusicEntry(CpuState *cpu);
RecompReturn ActRaiser_SceneMusic(CpuState *cpu);
#endif
