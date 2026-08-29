#!/bin/sh
# Guard the canonical constants most likely to drift back into hand-written
# code. This is intentionally targeted: numeric tables, ROM signatures, test
# fixtures, and ordinary arithmetic are data, not "magic number" violations.
set -eu

status=0

check_forbidden() {
  description=$1
  pattern=$2
  shift 2
  if matches=$(rg -n "$pattern" "$@" 2>/dev/null); then
    printf '%s\n' "constant-policy violation: $description" >&2
    printf '%s\n' "$matches" >&2
    status=1
  fi
}

check_forbidden \
  "use ActRaiser WRAM address names instead of indexing g_ram with literals" \
  'g_ram[[:space:]]*\[[[:space:]]*0x[0-9A-Fa-f]+' \
  src --glob '*.[ch]' --glob '!gen/**'

check_forbidden \
  "use kSnesWramSize throughout the bundled runtime" \
  '\b0x20000([uU]([lL]{1,2})?)?\b' \
  snesrecomp-go/runtime/src --glob '*.[ch]' \
  --glob '!**/runtime_constants.h'

check_forbidden \
  "use kActRaiserRuntimeWram_GameFrame in runtime diagnostics" \
  'g_ram[[:space:]]*\[[[:space:]]*0x0*(88|89)\b' \
  snesrecomp-go/runtime/src --glob '*.[ch]'

check_forbidden \
  "use the canonical runtime block-trace ring capacity and derived mask" \
  '\bAR_BLK_RING\b|g_ar_blk_idx[^;]*&[[:space:]]*1023[uU]?' \
  src snesrecomp-go/runtime/src --glob '*.[ch]' --glob '!gen/**'

check_forbidden \
  "use kActRaiserWramSize throughout authored game code" \
  '\b0x20000([uU]([lL]{1,2})?)?\b' \
  src --glob '*.[ch]' --glob '!gen/**' --glob '!**/constants.h'

check_forbidden \
  "do not redeclare the canonical WRAM storage with a literal extent" \
  'extern[[:space:]]+(unsigned char|uint8|uint8_t)[[:space:]]+g_ram[[:space:]]*\[[[:space:]]*0x' \
  src snesrecomp-go/runtime/src --glob '*.[ch]' --glob '!gen/**'

check_forbidden \
  "authentic dimensions belong in src/constants.h" \
  '\bk[A-Za-z0-9_]*(Authentic|Native)[A-Za-z0-9_]*(Width|Height)[[:space:]]*=[[:space:]]*(224|225|256)\b' \
  src --glob '*.[ch]' --glob '!gen/**' --glob '!**/constants.h'

check_forbidden \
  "use kActRaiserSpcMusicSourceMinimum for the shared/music SRCN boundary" \
  '#define[[:space:]]+(MUSIC_MUTE_SRCN_MIN|SFX_SRCN_MAX)\b' \
  src --glob '*.[ch]' --glob '!gen/**'

# The vended default ships to every install. It is a TEMPLATE rather than the
# developer's live config.ini precisely so an afternoon's debugging cannot reach
# a release -- this check is the other half of that: the template itself must
# stay stock. AR_MOONJUMP, AR_NO_KNOCKBACK and AR_RANGED_SWORD were all enabled
# in a shipped default before the two were separated. Comment settings out
# rather than deleting them; only uncommented assignments are flagged.
check_vended_config_is_stock() {
  file=$1
  [ -f "$file" ] || return 0
  violations=$(awk '
    /^[[:space:]]*\[/ { section = $0; sub(/^[[:space:]]*/, "", section); next }
    /^[[:space:]]*[#;]/ { next }
    /^[[:space:]]*$/ { next }
    (section == "[Cheats]" || section == "[Debug]") {
      printf "%s:%d: %s\n", FILENAME, FNR, $0
    }
  ' "$file")
  if [ -n "$violations" ]; then
    printf '%s\n' "vended-config violation: $file must ship with no cheat or debug setting enabled" >&2
    printf '%s\n' "$violations" >&2
    status=1
  fi
}

check_vended_config_is_stock snesrecomp-go/packaging/templates/config.ini

if [ "$status" -eq 0 ]; then
  printf '%s\n' "Canonical constant checks passed."
fi
exit "$status"
