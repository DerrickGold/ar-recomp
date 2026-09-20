#include "asset_condition.h"
#include "manifest_utils.h"
#include "actraiser_game.h"
#include <stdlib.h>
#include <string.h>

static SrRunnerHandle *s_runner;
void AssetConditions_BindRunner(SrRunnerHandle *runner) { s_runner = runner; }

static bool QueryPpuState(SrPpuStateSnapshot *state) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  if (!s_runner || !api || api->struct_size < SNES_RUNNER_API_PPU_STATE_SIZE ||
      !(api->capabilities & SR_RUNNER_CAP_PPU_STATE)) return false;
  state->struct_size = sizeof(*state);
  return api->query_ppu_state(s_runner, state) == SR_RESULT_OK;
}

/* One comparison: <operand> ==|!= <value>. Returns false on syntax error.
 * Shared with the music manifest parser (music_replacements.c). */
bool AssetCondition_Parse(char *term, AssetCondition *cond) {
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
    cond->kind = kAssetCondition_WramByte;
    cond->address = (uint16)parsed;
    cond->value = (uint16)strtoul(rhs, NULL, 0);
    return true;
  }
  if (!strcmp(lhs, "mode")) {
    cond->kind = kAssetCondition_BgMode;
    cond->value = (uint16)strtoul(rhs, NULL, 0);
    return true;
  }
  if (!strcmp(lhs, "m7")) {
    if (strcmp(rhs, "identity")) return false;
    cond->kind = kAssetCondition_M7Identity;
    return true;
  }
  if (lhs[0] == 'm' && lhs[1] == '7' && lhs[2] >= 'a' && lhs[2] <= 'd' &&
      !lhs[3]) {
    cond->kind = kAssetCondition_M7Element;
    cond->address = (uint16)(lhs[2] - 'a');
    cond->value = (uint16)strtoul(rhs, NULL, 0);
    return true;
  }
  return false;
}

bool AssetConditions_ParseWhen(char *value, AssetCondition *conditions, int max,
                          int *count) {
  char *cursor = value;
  while (cursor && *cursor) {
    char *comma = strchr(cursor, ',');
    if (comma) *comma = 0;
    char *term = Manifest_Trim(cursor);
    if (term[0]) {
      if (*count >= max) return false;
      if (!AssetCondition_Parse(term, &conditions[*count]))
        return false;
      (*count)++;
    }
    cursor = comma ? comma + 1 : NULL;
  }
  return *count > 0;
}

bool AssetCondition_Matches(
    const AssetCondition *cond, const uint8 *wram,
    const SrPpuStateSnapshot *ppu_state) {
  uint16 actual = 0;
  if (!cond) return false;
  /* PPU-dependent operands never pass without a coherent PPU snapshot;
   * WRAM operands stay valid everywhere. */
  if (!ppu_state && cond->kind != kAssetCondition_WramByte) return false;
  switch (cond->kind) {
    case kAssetCondition_WramByte: actual = wram[cond->address]; break;
    case kAssetCondition_BgMode: actual = ppu_state->bg_mode; break;
    case kAssetCondition_M7Element:
      actual = (uint16)ppu_state->mode7_matrix[cond->address & 3];
      break;
    case kAssetCondition_M7Identity: {
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

bool AssetCondition_Passes(const AssetCondition *cond) {
  SrPpuStateSnapshot ppu_state = {0};
  if (!cond)
    return false;
  if (cond->kind == kAssetCondition_WramByte)
    return AssetCondition_Matches(cond, g_ram, NULL);
  return QueryPpuState(&ppu_state) &&
         AssetCondition_Matches(cond, g_ram, &ppu_state);
}

