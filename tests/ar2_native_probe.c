/* Local-ROM research harness. Script: frame input HEX | write HEX HEX |
 * dump PATH | image PATH | terrain PATH | palette PATH | hud PATH | text PATH |
 * snapshot PREFIX | actors | extra DEC | view 0/1.
 * Writes are explicit test fixtures, not
 * evidence of an unassisted playthrough. No probe exports enter the module. */
#include "actraiser2/imported_game_api.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const ArImportedGameApi *ArImportedGame_GetApi(uint32_t version);

static unsigned word(unsigned at) { return g_ram[at] | g_ram[at + 1] << 8; }

static void actors(void) {
  unsigned actor = word(0x87f);
  for (unsigned count = 0; actor && count < 128; count++) {
    if (actor > 0xffd0) break;
    fprintf(stderr, "actor %04x xy=%u,%u flags=%04x,%04x task=%02x%04x next=%04x\n", actor,
            word(actor), word(actor + 2), word(actor + 4), word(actor + 6), g_ram[0x10006 + actor],
            word(0x10004 + actor), word(actor + 0x2c));
    actor = word(actor + 0x2c);
  }
}

static bool output(const char *path, const ArImportedGameFrame *frame, const char *kind) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  bool ok = true;
  if (!strcmp(kind, "image") || !strcmp(kind, "terrain") ||
      !strcmp(kind, "hud") || !strcmp(kind, "text")) {
    const Ar2VideoFrame *v = &frame->video;
    fprintf(file, "P6\n%d %d\n255\n", v->width, v->height);
    for (int y = 0; y < v->height; y++) {
      for (int x = 0; x < v->width; x++) {
        uint32_t rgb = v->main[y * AR2_CAPTURE_PITCH + x];
        if (!strcmp(kind, "hud") || !strcmp(kind, "text")) {
          rgb = (!strcmp(kind, "hud") ? v->hud : v->screen_objects)[y * AR2_CAPTURE_PITCH + x];
          if (!(rgb >> 24)) rgb = 0xff00ff;
        }
        if (!strcmp(kind, "terrain")) {
          rgb = v->planes[kDioramaPlane_Bg1Hi][y * AR2_CAPTURE_PITCH + x];
          if (!(rgb >> 24)) rgb = v->planes[SR_PPU_OVERLAY_BG1][y * AR2_CAPTURE_PITCH + x];
        }
        const uint8_t bytes[3] = {rgb >> 16, rgb >> 8, rgb};
        ok &= fwrite(bytes, 1, 3, file) == 3;
      }
    }
  } else if (!strcmp(kind, "palette")) {
    const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    SrBorrowedU16Span palette = {.struct_size = SR_BORROWED_U16_SPAN_V2_SIZE};
    ok = api->borrow_u16_memory(RtlGameRunner(), SR_MEMORY_CGRAM, &palette) == SR_RESULT_OK &&
         palette.element_count == 256;
    for (unsigned i = 0; ok && i < palette.element_count; ++i) {
      const uint8_t bytes[2] = {palette.data[i], palette.data[i] >> 8};
      ok = fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    }
  } else {
    ok = fwrite(g_ram, 1, 0x20000, file) == 0x20000;
  }
  return fclose(file) == 0 && ok;
}

int main(int argc, char **argv) {
  if (argc < 5 || argc > 6) {
    fprintf(stderr, "Usage: %s ROM scene(hex) frames extra [script]\n", argv[0]);
    return 2;
  }
  FILE *rom_file = fopen(argv[1], "rb");
  uint8_t *rom = malloc(0x180000);
  if (!rom_file || !rom || fread(rom, 1, 0x180000, rom_file) != 0x180000) return 2;
  fclose(rom_file);
  FILE *script = argc == 6 ? fopen(argv[5], "r") : NULL;
  if (argc == 6 && !script) return 2;
  const ArImportedGameApi *api = ArImportedGame_GetApi(AR_IMPORTED_GAME_ABI);
  if (!api->open(rom, 0x180000, strtoul(argv[2], NULL, 16))) {
    fprintf(stderr, "%s\n", api->error());
    return 1;
  }
  char line[1024] = "";
  if (script && !fgets(line, sizeof(line), script)) line[0] = 0;
  unsigned input = 0, previous_scene = 0xffff;
  unsigned previous_control = 0xffff;
  int extra = atoi(argv[4]);
  bool diorama = false;
  ArImportedGameFrame frame = {0};
  puts("frame,scene,pending,x,y,camera_x,camera_y,health,lives,level,"
       "map_width,map_height,left_objects,right_objects,audio_energy,event,entry_phase,control");
  for (unsigned tick = 0; tick < strtoul(argv[3], NULL, 10); tick++) {
    while (line[0]) {
      unsigned when, a, b;
      char command[16], path[900];
      if (sscanf(line, "%u %15s", &when, command) != 2 || when < tick) return 2;
      if (when > tick) break;
      if (!strcmp(command, "input") && sscanf(line, "%*u %*s %x", &a) == 1) {
        input = a;
      } else if (!strcmp(command, "extra") && sscanf(line, "%*u %*s %u", &a) == 1 && a <= 128) {
        extra = a;
      } else if (!strcmp(command, "view") && sscanf(line, "%*u %*s %u", &a) == 1 && a <= 1) {
        diorama = a != 0;
      } else if (!strcmp(command, "write") && sscanf(line, "%*u %*s %x %x", &a, &b) == 2 &&
                 a < 0x1ffff && b <= 0xffff) {
        fprintf(stderr, "fixture at %u: [%05x]=%04x\n", tick, a, b);
        g_ram[a] = b;
        g_ram[a + 1] = b >> 8;
      } else if (!strcmp(command, "actors")) {
        actors();
      } else if (!strcmp(command, "snapshot") && sscanf(line, "%*u %*s %899s", path) == 1) {
        if (!api->snapshot(path)) return 2;
      } else if ((!strcmp(command, "dump") || !strcmp(command, "image") ||
                  !strcmp(command, "terrain") ||
                  !strcmp(command, "hud") || !strcmp(command, "text") ||
                  !strcmp(command, "palette")) && tick &&
                 sscanf(line, "%*u %*s %899s", path) == 1) {
        if (!output(path, &frame, command)) return 2;
      } else {
        fprintf(stderr, "Invalid script: %s", line);
        return 2;
      }
      if (!fgets(line, sizeof(line), script)) line[0] = 0;
    }
    if (!api->step(input, extra, diorama, &frame)) {
      fprintf(stderr, "%s at frame %u\n", api->error(), tick);
      return 1;
    }
    const unsigned control = word(0x951);
    if (tick % 30 == 0 || frame.scene != previous_scene || frame.event ||
        control != previous_control) {
      unsigned left = 0, right = 0;
      unsigned audio_energy = 0;
      for (unsigned i = 0; i < frame.pcm_frames * 2; i++) {
        int sample = frame.pcm[i];
        audio_energy += sample < 0 ? -sample : sample;
      }
      const Ar2VideoFrame *v = &frame.video;
      for (int y = 0; y < v->height; y++) {
        for (int x = 0; x < v->width; x++) {
          uint32_t pixel = 0;
          for (unsigned priority = 0; priority < 4; priority++)
            pixel |= v->planes[DioramaPlaneForObjectPriority(priority)][y * AR2_CAPTURE_PITCH + x];
          if (!(pixel >> 24)) continue;
          left += x < v->render_extra;
          right += x >= v->render_extra + 256;
        }
      }
      printf("%u,%02x,%02x,%u,%u,%d,%d,%u,%u,%d,%d,%d,%u,%u,%u,%u,%u,%u\n", tick, frame.scene,
             word(0x518), frame.player_x, frame.player_y, frame.video.camera_x,
             frame.video.camera_y, frame.health, frame.lives, frame.video.level, v->world_width,
             v->world_height, left, right, audio_energy, frame.event, word(0x95b), control);
      fflush(stdout);
    }
    previous_scene = frame.scene;
    previous_control = control;
    if (frame.event) {
      ArImportedGameFrame repeated;
      if (!api->step(input, extra, diorama, &repeated) || repeated.event != frame.event ||
          repeated.frame != frame.frame || repeated.video.main || repeated.pcm_frames)
        return 1;
      /* A terminal coroutine must also be safe to discard and reopen, as
       * selecting another entrance from the host globe does. */
      api->close();
      unsigned stage = strtoul(argv[2], NULL, 16);
      if (!api->open(rom, 0x180000, stage)) return 1;
      bool restored = false;
      for (unsigned tick = 0; tick < 600; tick++) {
        if (!api->step(0, extra, diorama, &repeated)) {
          fprintf(stderr, "Reopen failed at frame %u: %s\n", tick, api->error());
          return 1;
        }
        restored |= repeated.scene == stage && repeated.video.level && repeated.health == 20 &&
                    repeated.lives == 3;
      }
      /* Native enemies can already hit an unattended player by frame 600. */
      if (!restored || repeated.event || repeated.scene != stage || !repeated.video.level ||
          !repeated.health || repeated.lives != 3) {
        fprintf(stderr, "Reopen state: scene=%02x level=%d health=%u lives=%u event=%u\n",
                repeated.scene, repeated.video.level, repeated.health, repeated.lives,
                repeated.event);
        return 1;
      }
      fprintf(stderr, "Reopened entrance %02x after terminal return: 600 native frames\n", stage);
      break;
    }
  }
  if (script) fclose(script);
  api->close();
  free(rom);
  return 0;
}
