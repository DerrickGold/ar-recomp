#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hd_replacements.h"
#include "snes/ppu.h"
#include "settings.h"

extern uint8 g_ram[0x20000];
extern Ppu *g_ppu;

HdReplacement g_hd_replacements[kHdMaxReplacements];
int g_hd_replacement_count;

/* ---- parsing ---------------------------------------------------------- */

static char *TrimInPlace(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                     end[-1] == '\r' || end[-1] == '\n'))
    *--end = 0;
  return s;
}

static int ParseSourceName(const char *value) {
  if (!strcmp(value, "bg1")) return kPpuOverlaySource_Bg1;
  if (!strcmp(value, "bg2")) return kPpuOverlaySource_Bg2;
  if (!strcmp(value, "bg3")) return kPpuOverlaySource_Bg3;
  if (!strcmp(value, "bg4")) return kPpuOverlaySource_Bg4;
  if (!strcmp(value, "obj")) return kPpuOverlaySource_Obj;
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
  char *lhs = TrimInPlace(term);
  char *rhs = TrimInPlace(op + 2);
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
    char *term = TrimInPlace(cursor);
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

/* Resolve an image path relative to the manifest's directory. */
static void ResolveImagePath(const char *manifest_path, const char *value,
                             char *out, size_t out_size) {
  const char *slash = strrchr(manifest_path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(manifest_path, '\\');
  if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
  if (value[0] == '/' || !slash) {
    snprintf(out, out_size, "%s", value);
  } else {
    snprintf(out, out_size, "%.*s/%s",
             (int)(slash - manifest_path), manifest_path, value);
  }
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
            entry->canvas_x1 > 1024 || entry->canvas_y0 < 0 ||
            entry->canvas_y1 > 1024))
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
  char line[1024];
  int line_number = 0;

  #define COMMIT_PENDING() do { \
    if (in_entry && EntryComplete(&pending, path, entry_line) && \
        g_hd_replacement_count < kHdMaxReplacements) \
      g_hd_replacements[g_hd_replacement_count++] = pending; \
    in_entry = false; \
  } while (0)

  while (fgets(line, sizeof(line), f)) {
    line_number++;
    char *s = TrimInPlace(line);
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
    char *key = TrimInPlace(s);
    char *value = TrimInPlace(equals + 1);
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
      ResolveImagePath(path, value, pending.image, sizeof(pending.image));
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

bool HdManifest_ConditionPasses(const HdCondition *cond) {
  uint16 actual = 0;
  /* PPU-dependent operands never pass without a PPU (headless music gates);
   * WRAM operands stay valid everywhere. */
  if (!g_ppu && cond->kind != kHdCond_WramByte) return false;
  switch (cond->kind) {
    case kHdCond_WramByte: actual = g_ram[cond->address]; break;
    case kHdCond_BgMode: actual = (uint16)(g_ppu->bgmode & 7); break;
    case kHdCond_M7Element:
      actual = (uint16)g_ppu->m7matrix[cond->address & 3];
      break;
    case kHdCond_M7Identity: {
      bool identity = g_ppu->m7matrix[0] == 0x0100 &&
                      g_ppu->m7matrix[1] == 0 && g_ppu->m7matrix[2] == 0 &&
                      g_ppu->m7matrix[3] == 0x0100;
      return cond->negate ? !identity : identity;
    }
  }
  bool equal = actual == cond->value;
  return cond->negate ? !equal : equal;
}

void HdReplacements_EvaluateFrame(void) {
  for (int i = 0; i < g_hd_replacement_count; i++)
    g_hd_replacements[i].active = false;
  if (!g_ppu || !g_settings.hd_replacements)
    return;

  for (int i = 0; i < g_hd_replacement_count; i++) {
    HdReplacement *entry = &g_hd_replacements[i];
    bool has_art = entry->plane == kHdPlane_Screen ? entry->texture != NULL
                                                   : entry->pixels != NULL;
    if (entry->plane == kHdPlane_Tiles || !has_art)
      continue;
    bool pass = true;
    for (int c = 0; c < entry->condition_count && pass; c++)
      pass = HdManifest_ConditionPasses(&entry->conditions[c]);
    if (!pass)
      continue;
    /* One capture rect per source (and one Mode-7 override) per frame is a
     * renderer invariant. An already-set owner (HUD split, magic OAM, or an
     * earlier manifest entry) wins; drop this entry for the frame and say
     * so once. */
    static uint32 warned_mask;
    if (entry->plane == kHdPlane_Mode7) {
      if (g_ppu->m7Override.rgba) {
        if (!(warned_mask & (1u << i))) {
          warned_mask |= 1u << i;
          fprintf(stderr, "[hd-manifest] [replace:%s] Mode-7 override busy "
                  "(another entry owns it this frame); entry skipped\n",
                  entry->name);
        }
        continue;
      }
      if (PpuSetMode7Override(g_ppu, (const uint32 *)entry->pixels,
                              entry->pixels_width, entry->pixels_height,
                              entry->canvas_x0, entry->canvas_y0,
                              entry->canvas_x1, entry->canvas_y1,
                              entry->canvas_wrap))
        entry->active = true;
      continue;
    }
    const PpuOverlayCapture *existing =
        &g_ppu->overlayCaptures[entry->source];
    if (existing->x1 > existing->x0) {
      if (!(warned_mask & (1u << i))) {
        warned_mask |= 1u << i;
        fprintf(stderr, "[hd-manifest] [replace:%s] source busy (another "
                "capture owns it this frame); entry skipped\n", entry->name);
      }
      continue;
    }
    if (PpuSetOverlayCapture(g_ppu, (PpuOverlaySource)entry->source,
                             entry->x0, entry->y0,
                             entry->x1 - entry->x0, entry->y1 - entry->y0,
                             kPpuOverlayFlag_RemoveFromGame))
      entry->active = true;
  }
}
