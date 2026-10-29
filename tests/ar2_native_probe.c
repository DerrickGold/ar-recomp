/* Local-ROM research harness. Script: frame input HEX | write HEX HEX |
 * dump PATH | image PATH | terrain PATH | background PATH | palette PATH | hud PATH | text PATH |
 * snapshot PREFIX | actors | extra DEC | rows DEC | view 0/1 | refresh |
 * watch PC(hex) | watch-block PC(hex) | require PC(hex) | timing PATH (per-step CSV) |
 * observe-block PC(hex) (first executed block hit: CPU and lifecycle state on stderr) |
 * artifacts PREFIX (snapshot and last completed image on exit, including failure).
 * Watches count actual native dispatches or executed blocks.
 * Writes are explicit test fixtures, not
 * evidence of an unassisted playthrough. No probe exports enter the module. */
#include "actraiser2/imported_game_api.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const ArImportedGameApi *ArImportedGame_GetApi(uint32_t version);

static unsigned word(unsigned at) { return g_ram[at] | g_ram[at + 1] << 8; }
static struct { uint32_t pc; unsigned hits; bool blocks, observe; } watches[32];
static unsigned watch_count, probe_tick;

static void observe(uint32_t pc) {
  /* Read at the native block boundary, before its instructions execute.
   * No snapshot/runner calls inside the callback: those would be reentrant. */
  fprintf(stderr, "native observation {\"pc\":\"%06x\",\"frame\":%u,"
          "\"a\":%u,\"x\":%u,\"y\":%u,\"s\":%u,\"d\":%u,\"db\":%u,\"pb\":%u,"
          "\"m\":%u,\"x_flag\":%u,\"scene\":%u,\"pending\":%u,\"control\":%u,"
          "\"transition\":%u,\"record\":%u,\"record_offset\":%u,\"source_request\":%u,\"boss\":%u,"
          "\"progress\":[%u,%u,%u,%u,%u],\"score\":[%u,%u],\"time\":%u}\n",
          pc, probe_tick, g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S, g_cpu.D, g_cpu.DB, g_cpu.PB,
          g_cpu.m_flag, g_cpu.x_flag, word(0x51a), word(0x518), word(0x951), word(0x95b),
          word(0x971), word(0x975), word(0x97b), word(0x659), g_ram[0x90c], g_ram[0x90d],
          g_ram[0x90e], g_ram[0x90f], g_ram[0x910], word(0x929), word(0x92b), word(0x94d));
}

static double milliseconds(void) {
  struct timespec now;
  if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0;
  return (double)now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}

static void dispatch(void *unused, SrRunnerHandle *runner, const SrRunnerEvent *event) {
  (void)unused;
  (void)runner;
  for (unsigned i = 0; i < watch_count; ++i)
    if (watches[i].pc == event->pc24 &&
        (watches[i].blocks ? event->type == SR_EVENT_EXECUTION_BLOCK :
         event->type == SR_EVENT_DYNAMIC_DISPATCH && (event->flags & SR_EVENT_DISPATCH_FOUND))) {
      if (++watches[i].hits == 1 && watches[i].observe) observe(event->pc24);
    }
}

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

static uint64_t picture_hash(const Ar2VideoFrame *video) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (int y = 0; y < video->height; ++y)
    for (int x = 0; x < video->width; ++x)
      hash = (hash ^ video->main[y * AR2_CAPTURE_PITCH + x]) * UINT64_C(1099511628211);
  return hash;
}

static bool refresh(const ArImportedGameApi *api, ArImportedGameView original,
                    ArImportedGameFrame *frame) {
  static uint8_t before[0x20000];
  memcpy(before, g_ram, sizeof(before));
  const CpuState cpu = g_cpu;
  const uint64_t apu = RtlApuCycleCount(), pixels = picture_hash(&frame->video);
  const uint64_t main_cycles = g_main_cpu_cycles_estimate;
  const uint64_t pace_cycles = g_apu_pace_cycles_estimate, sync_cycles = g_apu_last_sync_cycles;
  const unsigned number = frame->frame;
  const ArImportedGameView views[] = {{0, 0, false}, {128, 64, false}, {51, 64, true}, original};
  for (unsigned i = 0; i < sizeof(views) / sizeof(*views); ++i) {
    if (!api->refresh(views[i], frame) || frame->frame != number || frame->pcm_frames ||
        memcmp(before, g_ram, sizeof(before)) || memcmp(&cpu, &g_cpu, sizeof(cpu)) ||
        RtlApuCycleCount() != apu || g_main_cpu_cycles_estimate != main_cycles ||
        g_apu_pace_cycles_estimate != pace_cycles || g_apu_last_sync_cycles != sync_cycles) return false;
  }
  return pixels == picture_hash(&frame->video);
}

static bool output(const char *path, const ArImportedGameFrame *frame, const char *kind) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  bool ok = true;
  if (!strcmp(kind, "image") || !strcmp(kind, "terrain") || !strcmp(kind, "background") ||
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
        if (!strcmp(kind, "background")) {
          rgb = v->planes[kDioramaPlane_Bg2Hi][y * AR2_CAPTURE_PITCH + x];
          if (!(rgb >> 24)) rgb = v->planes[SR_PPU_OVERLAY_BG2][y * AR2_CAPTURE_PITCH + x];
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

static bool artifacts(const ArImportedGameApi *api, const char *prefix,
                      const ArImportedGameFrame *frame) {
  if (!prefix[0]) return true;
  bool ok = api->snapshot(prefix);
  char path[1024];
  if (snprintf(path, sizeof(path), "%s.last.ppm", prefix) >= (int)sizeof(path)) return false;
  if (frame->video.main) ok &= output(path, frame, "image");
  return ok;
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
  FILE *timing = NULL;
  if (argc == 6 && !script) return 2;
  const ArImportedGameApi *api = ArImportedGame_GetApi(AR_IMPORTED_GAME_ABI);
  if (!api->open(rom, 0x180000, strtoul(argv[2], NULL, 16))) {
    fprintf(stderr, "%s\n", api->error());
    return 1;
  }
  const SnesRunnerApi *runner_api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  uint64_t subscription = 0;
  uint64_t block_subscription = 0;
  char line[1024] = "";
  char artifact_prefix[900] = "";
  if (script && !fgets(line, sizeof(line), script)) line[0] = 0;
  unsigned input = 0, previous_scene = 0xffff;
  unsigned previous_control = 0xffff;
  unsigned previous_boss = 0xffff;
  int extra = atoi(argv[4]);
  bool diorama = false;
  int rows = 0;
  ArImportedGameFrame frame = {0};
  puts("frame,scene,pending,x,y,camera_x,camera_y,health,lives,level,"
       "map_width,map_height,left_objects,right_objects,audio_energy,event,entry_phase,control,"
       "extra_top,extra_bottom,height,above_objects,below_objects,"
       "boss_actor,boss_health,camera_min_x,camera_max_x,native_camera_x,native_camera_y,"
       "bg2_camera_x,bg2_camera_y,view_offset_x,native_bg2_x,native_bg2_y");
  for (unsigned tick = 0; tick < strtoul(argv[3], NULL, 10); tick++) {
    probe_tick = tick;
    while (line[0]) {
      unsigned when, a, b;
      char command[16], path[900];
      if (sscanf(line, "%u %15s", &when, command) != 2 || when < tick) return 2;
      if (when > tick) break;
      if (!strcmp(command, "input") && sscanf(line, "%*u %*s %x", &a) == 1) {
        input = a;
      } else if (!strcmp(command, "extra") && sscanf(line, "%*u %*s %u", &a) == 1 && a <= 128) {
        extra = a;
      } else if (!strcmp(command, "rows") && sscanf(line, "%*u %*s %u", &a) == 1 && a <= 64) {
        rows = a;
      } else if (!strcmp(command, "view") && sscanf(line, "%*u %*s %u", &a) == 1 && a <= 1) {
        diorama = a != 0;
      } else if (!strcmp(command, "refresh") && tick) {
        if (!refresh(api, (ArImportedGameView){extra, rows, diorama}, &frame)) {
          fprintf(stderr, "Frozen capture changed native state or pixels at frame %u: %s\n", tick, api->error());
          return 1;
        }
      } else if ((!strcmp(command, "watch") || !strcmp(command, "watch-block") ||
                  !strcmp(command, "observe-block")) &&
                 sscanf(line, "%*u %*s %x", &a) == 1 &&
                 a <= 0xffffff && watch_count < 32) {
        const bool blocks = strcmp(command, "watch") != 0;
        watches[watch_count].pc = a;
        watches[watch_count].observe = !strcmp(command, "observe-block");
        watches[watch_count++].blocks = blocks;
        uint64_t *token = blocks ? &block_subscription : &subscription;
        if (!*token) {
          const SrEventSubscription request = {.struct_size = sizeof(request),
              .event_mask = blocks ? SR_EVENT_MASK_EXECUTION_BLOCK : SR_EVENT_MASK_DYNAMIC_DISPATCH,
              .callback = dispatch};
          if (runner_api->subscribe_events(RtlGameRunner(), &request, token) !=
              SR_RESULT_OK)
            return 2;
        }
      } else if (!strcmp(command, "require") && sscanf(line, "%*u %*s %x", &a) == 1) {
        unsigned hits = 0;
        for (unsigned i = 0; i < watch_count; ++i)
          if (watches[i].pc == a) hits += watches[i].hits;
        fprintf(stderr, "native coverage %06x: %u hits\n", a, hits);
        if (!hits) return 1;
      } else if (!strcmp(command, "write") && sscanf(line, "%*u %*s %x %x", &a, &b) == 2 &&
                 a < 0x1ffff && b <= 0xffff) {
        fprintf(stderr, "fixture at %u: [%05x]=%04x\n", tick, a, b);
        g_ram[a] = b;
        g_ram[a + 1] = b >> 8;
      } else if (!strcmp(command, "actors")) {
        actors();
      } else if (!strcmp(command, "snapshot") && sscanf(line, "%*u %*s %899s", path) == 1) {
        if (!api->snapshot(path)) return 2;
      } else if (!strcmp(command, "artifacts") &&
                 sscanf(line, "%*u %*s %899s", artifact_prefix) == 1) {
        /* Written after the failing step while the native runtime still
         * owns its diagnostic memory. The image is the last completed frame. */
      } else if (!strcmp(command, "timing") && !timing &&
                 sscanf(line, "%*u %*s %899s", path) == 1) {
        timing = fopen(path, "w");
        if (!timing) return 2;
        fputs("frame,step_ms,apu_cycles,scene,graphics_before,graphics_after,music_state,boss_actor,"
              "port_cycles,port_sync_calls,port_reads,port_writes,last_port_function\n",
              timing);
      } else if ((!strcmp(command, "dump") || !strcmp(command, "image") ||
                  !strcmp(command, "terrain") || !strcmp(command, "background") ||
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
    const unsigned graphics_before = timing ? word(0x881) : 0;
    const uint64_t apu_before = timing ? RtlApuCycleCount() : 0;
    if (timing) RtlApuProfileReset();
    const double begin = timing ? milliseconds() : 0;
    if (!api->step(input, (ArImportedGameView){extra, rows, diorama}, &frame)) {
      fprintf(stderr, "%s at frame %u\n", api->error(), tick);
      if (!artifacts(api, artifact_prefix, &frame))
        fprintf(stderr, "Could not write probe artifacts\n");
      if (timing) fclose(timing);
      if (script) fclose(script);
      api->close();
      free(rom);
      return 1;
    }
    if (timing) {
      RtlApuProfile profile = {.struct_size = RTL_APU_PROFILE_V2_SIZE};
      RtlApuProfileRead(&profile);
      fprintf(timing, "%u,%.3f,%llu,%02x,%u,%u,%u,%u,%llu,%u,%u,%u,%s\n", tick, milliseconds() - begin,
          (unsigned long long)(RtlApuCycleCount() - apu_before), frame.scene,
          graphics_before, word(0x881), word(0x98d), word(0x659),
          (unsigned long long)profile.apu_cycles_port_sync, profile.port_sync_calls,
          profile.port_reads, profile.port_writes,
          profile.last_port_function ? profile.last_port_function : "");
      fflush(timing);
    }
    const unsigned control = word(0x951);
    const unsigned boss = word(0x659);
    if (tick % 30 == 0 || tick + 1 == strtoul(argv[3], NULL, 10) ||
        frame.scene != previous_scene || frame.event ||
        control != previous_control || boss != previous_boss) {
      unsigned left = 0, right = 0, above = 0, below = 0;
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
          above += y < v->extra_top;
          below += y >= v->extra_top + 224;
        }
      }
      printf("%u,%02x,%02x,%u,%u,%d,%d,%u,%u,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%d,%d,%u,%u,"
             "%u,%u,%u,%u,%u,%u,%d,%d,%d,%u,%u\n",
             tick, frame.scene,
             word(0x518), frame.player_x, frame.player_y, frame.video.camera_x,
             frame.video.camera_y, frame.health, frame.lives, frame.video.level, v->world_width,
             v->world_height, left, right, audio_energy, frame.event, word(0x95b), control,
             v->extra_top, v->extra_bottom, v->height, above, below,
             boss, word(0x953), word(0x588), word(0x58c), word(0x540), word(0x544),
             v->bg2_camera_x, v->bg2_camera_y, v->view_offset_x, word(0x53a), word(0x53e));
      fflush(stdout);
    }
    previous_scene = frame.scene;
    previous_control = control;
    previous_boss = boss;
    if (frame.event) {
      if (!artifacts(api, artifact_prefix, &frame)) return 2;
      ArImportedGameFrame repeated;
      if (!api->step(input, (ArImportedGameView){extra, rows, diorama}, &repeated) ||
          repeated.event != frame.event ||
          repeated.frame != frame.frame || repeated.video.main || repeated.pcm_frames)
        return 1;
      /* A terminal coroutine must also be safe to discard and reopen, as
       * selecting another entrance from the host globe does. */
      if (subscription) runner_api->unsubscribe_events(RtlGameRunner(), subscription);
      subscription = 0;
      if (block_subscription) runner_api->unsubscribe_events(RtlGameRunner(), block_subscription);
      block_subscription = 0;
      api->close();
      unsigned stage = strtoul(argv[2], NULL, 16);
      if (!api->open(rom, 0x180000, stage)) return 1;
      bool restored = false;
      for (unsigned tick = 0; tick < 600; tick++) {
        if (!api->step(0, (ArImportedGameView){extra, rows, diorama}, &repeated)) {
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
  if (!frame.event && !artifacts(api, artifact_prefix, &frame)) return 2;
  for (unsigned i = 0; i < watch_count; ++i)
    if (watches[i].observe)
      fprintf(stderr, "native observation count %06x: %u\n", watches[i].pc, watches[i].hits);
  if (script) fclose(script);
  if (timing && fclose(timing)) return 2;
  if (subscription) runner_api->unsubscribe_events(RtlGameRunner(), subscription);
  if (block_subscription) runner_api->unsubscribe_events(RtlGameRunner(), block_subscription);
  api->close();
  free(rom);
  return 0;
}
