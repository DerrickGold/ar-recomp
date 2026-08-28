#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "hd_replacements.h"
#include "manifest_utils.h"
#include "settings.h"

HdReplacement g_hd_replacements[kHdMaxReplacements];
int g_hd_replacement_count;
static SrRunnerHandle *s_runner;

enum { kHdManifestLineCapacity = 1024 };

void HdReplacements_BindRunner(SrRunnerHandle *runner) {
  s_runner = runner;
}

/* ---- parsing ---------------------------------------------------------- */

static int ParseSourceName(const char *value) {
  if (!strcmp(value, "bg1")) return SR_PPU_OVERLAY_BG1;
  if (!strcmp(value, "bg2")) return SR_PPU_OVERLAY_BG2;
  if (!strcmp(value, "bg3")) return SR_PPU_OVERLAY_BG3;
  if (!strcmp(value, "bg4")) return SR_PPU_OVERLAY_BG4;
  if (!strcmp(value, "obj")) return SR_PPU_OVERLAY_OBJ;
  return -1;
}

static bool ParseRect4(const char *value, int *x0, int *y0, int *x1, int *y1) {
  if (sscanf(value, "%d ,%d ,%d ,%d", x0, y0, x1, y1) != 4)
    return false;
  return *x1 > *x0 && *y1 > *y0;
}

/* One comparison: <operand> ==|!= <value>. Returns false on syntax error.
 * Shared with the music manifest parser (music_replacements.c). */
bool HdManifest_ParseCondition(char *term, HdCondition *cond) {
  memset(cond, 0, sizeof(*cond));
  char *op = strstr(term, "==");
  cond->negate = 0;
  if (!op) { op = strstr(term, "!="); cond->negate = 1; }
  if (!op) return false;
  *op = 0;
  char *lhs = Manifest_Trim(term);
  char *rhs = Manifest_Trim(op + 2);
  if (!lhs[0] || !rhs[0]) return false;

  if (!strncmp(lhs, "wram[", 5)) {
    char *close = strchr(lhs + 5, ']');
    if (!close) return false;
    *close = 0;
    const char *addr = lhs + 5;
    if (addr[0] == '$') addr++;
    unsigned long parsed = strtoul(addr, NULL, 16);
    if (parsed > 0xffff) return false; /* gate operands are low-page WRAM */
    cond->kind = kHdCond_WramByte;
    cond->address = (uint16)parsed;
    cond->value = (uint16)strtoul(rhs, NULL, 0);
    return true;
  }
  if (!strcmp(lhs, "mode")) {
    cond->kind = kHdCond_BgMode;
    cond->value = (uint16)strtoul(rhs, NULL, 0);
    return true;
  }
  if (!strcmp(lhs, "m7")) {
    if (strcmp(rhs, "identity")) return false;
    cond->kind = kHdCond_M7Identity;
    return true;
  }
  if (lhs[0] == 'm' && lhs[1] == '7' && lhs[2] >= 'a' && lhs[2] <= 'd' &&
      !lhs[3]) {
    cond->kind = kHdCond_M7Element;
    cond->address = (uint16)(lhs[2] - 'a');
    cond->value = (uint16)strtoul(rhs, NULL, 0);
    return true;
  }
  return false;
}

bool HdManifest_ParseWhen(char *value, HdCondition *conditions, int max,
                          int *count) {
  char *cursor = value;
  while (cursor && *cursor) {
    char *comma = strchr(cursor, ',');
    if (comma) *comma = 0;
    char *term = Manifest_Trim(cursor);
    if (term[0]) {
      if (*count >= max) return false;
      if (!HdManifest_ParseCondition(term, &conditions[*count]))
        return false;
      (*count)++;
    }
    cursor = comma ? comma + 1 : NULL;
  }
  return *count > 0;
}

static bool EntryComplete(const HdReplacement *entry, const char *path,
                          int line) {
  const char *missing = NULL;
  if (!entry->image[0]) missing = "image";
  else if (entry->plane == kHdPlane_Screen && entry->source < 0)
    missing = "layer";
  else if (entry->plane == kHdPlane_Screen && entry->x1 <= entry->x0)
    missing = "rect";
  else if (entry->plane == kHdPlane_Mode7 &&
           (entry->canvas_x1 <= entry->canvas_x0 || entry->canvas_x0 < 0 ||
            entry->canvas_x1 > (int)SR_PPU_MODE7_CANVAS_EXTENT ||
            entry->canvas_y0 < 0 ||
            entry->canvas_y1 > (int)SR_PPU_MODE7_CANVAS_EXTENT))
    missing = "canvas_rect";
  else if (!entry->condition_count) missing = "when";
  if (missing)
    fprintf(stderr, "[hd-manifest] %s:%d: [replace:%s] missing/invalid '%s'"
            " — entry dropped\n", path, line, entry->name, missing);
  return !missing;
}

int HdReplacements_Load(const char *path) {
  g_hd_replacement_count = 0;
  memset(g_hd_replacements, 0, sizeof(g_hd_replacements));
  FILE *f = fopen(path, "r");
  if (!f) return 0;

  HdReplacement pending;
  bool in_entry = false;
  int entry_line = 0;
  char line[kHdManifestLineCapacity];
  int line_number = 0;

  #define COMMIT_PENDING() do { \
    if (in_entry && EntryComplete(&pending, path, entry_line) && \
        g_hd_replacement_count < kHdMaxReplacements) \
      g_hd_replacements[g_hd_replacement_count++] = pending; \
    in_entry = false; \
  } while (0)

  while (fgets(line, sizeof(line), f)) {
    line_number++;
    char *s = Manifest_Trim(line);
    if (!s[0] || s[0] == '#' || s[0] == ';') continue;

    if (s[0] == '[') {
      COMMIT_PENDING();
      char *close = strchr(s, ']');
      if (close) *close = 0;
      if (!strncmp(s + 1, "replace:", 8)) {
        memset(&pending, 0, sizeof(pending));
        snprintf(pending.name, sizeof(pending.name), "%s", s + 9);
        pending.plane = kHdPlane_Screen;
        pending.source = -1;
        pending.brightness_mod = true;
        in_entry = true;
        entry_line = line_number;
      } else if (!strncmp(s + 1, "music:", 6)) {
        /* Another module's sections in the shared manifest — not ours. */
      } else {
        fprintf(stderr, "[hd-manifest] %s:%d: unknown section '%s]' ignored\n",
                path, line_number, s);
      }
      continue;
    }
    if (!in_entry) continue;

    char *equals = strchr(s, '=');
    if (!equals) continue;
    *equals = 0;
    char *key = Manifest_Trim(s);
    char *value = Manifest_Trim(equals + 1);
    bool ok = true;
    if (!strcmp(key, "plane")) {
      if (!strcmp(value, "screen")) pending.plane = kHdPlane_Screen;
      else if (!strcmp(value, "mode7")) pending.plane = kHdPlane_Mode7;
      else if (!strcmp(value, "tiles")) pending.plane = kHdPlane_Tiles;
      else ok = false;
    } else if (!strcmp(key, "layer")) {
      pending.source = ParseSourceName(value);
      ok = pending.source >= 0;
    } else if (!strcmp(key, "rect")) {
      ok = ParseRect4(value, &pending.x0, &pending.y0,
                      &pending.x1, &pending.y1);
    } else if (!strcmp(key, "canvas_rect")) {
      ok = ParseRect4(value, &pending.canvas_x0, &pending.canvas_y0,
                      &pending.canvas_x1, &pending.canvas_y1);
    } else if (!strcmp(key, "wrap")) {
      pending.canvas_wrap = strtoul(value, NULL, 0) != 0;
    } else if (!strcmp(key, "image")) {
      Manifest_ResolvePath(
          path, value, pending.image, sizeof(pending.image));
    } else if (!strcmp(key, "when")) {
      ok = HdManifest_ParseWhen(value, pending.conditions, kHdMaxConditions,
                                &pending.condition_count);
    } else if (!strcmp(key, "brightness")) {
      pending.brightness_mod = strtoul(value, NULL, 0) != 0;
    } else {
      fprintf(stderr, "[hd-manifest] %s:%d: unknown key '%s' ignored\n",
              path, line_number, key);
    }
    if (!ok) {
      fprintf(stderr, "[hd-manifest] %s:%d: bad value for '%s' — "
              "[replace:%s] dropped\n", path, line_number, key, pending.name);
      in_entry = false;
    }
  }
  COMMIT_PENDING();
  #undef COMMIT_PENDING
  fclose(f);

  for (int i = 0; i < g_hd_replacement_count; i++) {
    HdReplacement *entry = &g_hd_replacements[i];
    if (entry->plane == kHdPlane_Tiles)
      fprintf(stderr, "[hd-manifest] [replace:%s] plane 'tiles' is reserved "
              "and not implemented yet; entry inert\n", entry->name);
  }
  return g_hd_replacement_count;
}

/* ---- per-frame policy -------------------------------------------------- */

static bool QueryPpuState(const SnesRunnerApi **out_api,
                          SrPpuStateSnapshot *out_state,
                          bool require_capture_control) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  const uint64_t required_caps = SR_RUNNER_CAP_PPU_STATE |
      (require_capture_control ? SR_RUNNER_CAP_PPU_CAPTURE_CONTROL : 0u);
  const uint32_t required_size = require_capture_control
      ? SNES_RUNNER_API_PPU_CAPTURE_CONTROL_SIZE
      : SNES_RUNNER_API_PPU_STATE_SIZE;
  if (!s_runner || !api || api->struct_size < required_size ||
      (api->capabilities & required_caps) != required_caps)
    return false;
  out_state->struct_size = sizeof(*out_state);
  if (api->query_ppu_state(s_runner, out_state) != SR_RESULT_OK)
    return false;
  if (out_api) *out_api = api;
  return true;
}

static bool ConditionPassesWithPpuState(
    const HdCondition *cond, const SrPpuStateSnapshot *ppu_state) {
  uint16 actual = 0;
  if (!cond) return false;
  /* PPU-dependent operands never pass without a coherent PPU snapshot;
   * WRAM operands stay valid everywhere. */
  if (!ppu_state && cond->kind != kHdCond_WramByte) return false;
  switch (cond->kind) {
    case kHdCond_WramByte: actual = g_ram[cond->address]; break;
    case kHdCond_BgMode: actual = ppu_state->bg_mode; break;
    case kHdCond_M7Element:
      actual = (uint16)ppu_state->mode7_matrix[cond->address & 3];
      break;
    case kHdCond_M7Identity: {
      bool identity = ppu_state->mode7_matrix[0] == 0x0100 &&
                      ppu_state->mode7_matrix[1] == 0 &&
                      ppu_state->mode7_matrix[2] == 0 &&
                      ppu_state->mode7_matrix[3] == 0x0100;
      return cond->negate ? !identity : identity;
    }
    default: return false;
  }
  bool equal = actual == cond->value;
  return cond->negate ? !equal : equal;
}

bool HdManifest_ConditionPasses(const HdCondition *cond) {
  SrPpuStateSnapshot ppu_state = {0};
  if (!cond)
    return false;
  if (cond->kind == kHdCond_WramByte)
    return ConditionPassesWithPpuState(cond, NULL);
  return QueryPpuState(NULL, &ppu_state, false) &&
         ConditionPassesWithPpuState(cond, &ppu_state);
}

static bool EntryHasLoadedArt(const HdReplacement *entry) {
  if (entry->plane == kHdPlane_Screen)
    return ArRenderTexture_IsValid(entry->texture);
  if (entry->plane == kHdPlane_Mode7)
    return entry->pixels != NULL;
  return false;
}

void HdReplacements_EvaluateFrame(void) {
  const SnesRunnerApi *api;
  SrPpuStateSnapshot ppu_state = {0};
  bool has_any_art = false;
  for (int i = 0; i < g_hd_replacement_count; i++) {
    g_hd_replacements[i].active = false;
    const HdReplacement *entry = &g_hd_replacements[i];
    if (EntryHasLoadedArt(entry))
      has_any_art = true;
  }
  if (!g_settings.hd_replacements || !has_any_art ||
      !QueryPpuState(&api, &ppu_state, true))
    return;

  for (int i = 0; i < g_hd_replacement_count; i++) {
    HdReplacement *entry = &g_hd_replacements[i];
    if (!EntryHasLoadedArt(entry))
      continue;
    bool pass = true;
    for (int c = 0; c < entry->condition_count && pass; c++)
      pass = ConditionPassesWithPpuState(
          &entry->conditions[c], &ppu_state);
    if (!pass)
      continue;
    /* One capture rect per source (and one Mode-7 override) per frame is a
     * renderer invariant. An already-set owner (HUD split, magic OAM, or an
     * earlier manifest entry) wins; drop this entry for the frame and say
     * so once. */
    static uint32 warned_mask;
    if (entry->plane == kHdPlane_Mode7) {
      const SrPpuMode7OverrideRequest request = {
          .struct_size = sizeof(request),
          .lifetime_generation = ppu_state.lifetime_generation,
          .pixels = (const uint32_t *)entry->pixels,
          .pixel_byte_size = entry->pixels_width > 0 &&
                  entry->pixels_height > 0
              ? (uint64_t)(unsigned)entry->pixels_width *
                    (unsigned)entry->pixels_height * sizeof(uint32_t)
              : 0u,
          .width_pixels = entry->pixels_width > 0
              ? (uint32_t)entry->pixels_width : 0u,
          .height_pixels = entry->pixels_height > 0
              ? (uint32_t)entry->pixels_height : 0u,
          .canvas_x0 = entry->canvas_x0,
          .canvas_y0 = entry->canvas_y0,
          .canvas_x1 = entry->canvas_x1,
          .canvas_y1 = entry->canvas_y1,
          .wrap = entry->canvas_wrap ? 1u : 0u,
      };
      const SrResult result = api->claim_ppu_mode7_override(
          s_runner, &request);
      if (result == SR_RESULT_BUSY) {
        if (!(warned_mask & (1u << i))) {
          warned_mask |= 1u << i;
          fprintf(stderr, "[hd-manifest] [replace:%s] Mode-7 override busy "
                  "(another entry owns it this frame); entry skipped\n",
                  entry->name);
        }
        continue;
      }
      entry->active = result == SR_RESULT_OK;
      continue;
    }
    const SrPpuOverlayCaptureRequest request = {
        .struct_size = sizeof(request),
        .flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME,
        .lifetime_generation = ppu_state.lifetime_generation,
        .source = (uint32_t)entry->source,
        .x = entry->x0,
        .y = entry->y0,
        .width = entry->x1 - entry->x0,
        .height = entry->y1 - entry->y0,
    };
    const SrResult result = api->claim_ppu_overlay_capture(
        s_runner, &request);
    if (result == SR_RESULT_BUSY) {
      if (!(warned_mask & (1u << i))) {
        warned_mask |= 1u << i;
        fprintf(stderr, "[hd-manifest] [replace:%s] source busy (another "
                "capture owns it this frame); entry skipped\n", entry->name);
      }
      continue;
    }
    entry->active = result == SR_RESULT_OK;
  }
}
