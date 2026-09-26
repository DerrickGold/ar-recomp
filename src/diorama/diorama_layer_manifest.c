#include "diorama_layer_manifest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snesrecomp/support/utf8_fs.h"
#include "app/user_data_dir.h"
#include "constants.h"
#include "host/atomic_replace.h"

/* Per-room ($18,$19) layer overrides. The editor mutates this; the draw loop
 * reads it. Empty by default, and DioramaLayerOrder_Resolve on an empty table
 * returns the defaults verbatim in built-in order, so an unedited game is
 * bit-identical to before this existed. */
static DioramaLayerOrderTable s_layer_overrides;

const DioramaLayerOrderTable *DioramaLayerManifest_Table(void) { return &s_layer_overrides; }
DioramaLayerOrderTable *DioramaLayerManifest_Edit(void) { return &s_layer_overrides; }

/* ── layer manifest I/O ──────────────────────────────────────────────────
 *
 * `diorama-layers.ini` beside settings.ini. Sections are rooms, bodies are
 * planes:
 *
 *     [layers:01:02]          ; $18=01 $19=02 -- Fillmore act 2
 *     bg2hi = rake:0.29       ; flood the water forward to meet the rock path
 *     bg1   = thick:0.20      ; give the rock path a near face
 *     bg2hi = stack:0.29 copies:4  ; or fill the gap with parallel repeats
 *     bg2   = z:0.30 alpha:200
 *
 * The grammar and every bound live in diorama_layer_order.c, which is pure and
 * tested; this is only the file wrapper. A malformed line is reported and
 * SKIPPED rather than aborting the load, so one typo cannot cost every other
 * authored room. */
static const char kLayerManifestLeaf[] = "diorama-layers.ini";

bool DioramaLayerManifest_Load(void) {
  char path[kHostPathCapacity];
  UserDataFile(path, sizeof path, kLayerManifestLeaf);
  FILE *file = sr_fopen(path, "r");
  if (!file) {
    /* Absent is legitimate -- an unauthored install renders stock geometry and
     * that is correct. Say so anyway, at one line: this file is CWD-relative,
     * and when a packaged build failed to ship it the only symptom was the 3D
     * scene quietly losing every authored room, with nothing anywhere to
     * distinguish "nothing authored" from "the manifest is not where the game
     * is looking". The path is printed for exactly that reason. */
    fprintf(stderr,
            "[diorama-layers] %s not found (looked in the working directory) "
            "-- no per-room layer overrides; the diorama renders stock "
            "geometry\n",
            path);
    return false;
  }

  DioramaLayerOrderTable *loaded = calloc(1, sizeof(*loaded));
  if (!loaded) {
    fclose(file);
    fprintf(stderr, "[diorama-layers] out of memory loading %s\n", path);
    return false;
  }
  DioramaRoomOverride *room = NULL;
  char line[512];
  int rooms = 0, planes = 0, bad = 0, line_number = 0;
  while (fgets(line, sizeof line, file)) {
    line_number++;
    char *at = line;
    while (*at == ' ' || *at == '\t')
      at++;
    /* Strip a trailing comment BEFORE trimming, so `bg2hi = rake:0.29  ; why`
     * parses. The grammar in diorama_layer_order.c is whitespace-delimited
     * key:value only and would reject the `;` as a malformed pair -- inline
     * comments are a property of this file format, so they belong here rather
     * than in the pure parser. Section lines never needed this (ParseSection
     * only ever sees what is inside the brackets), which is exactly why the
     * first documented example looked fine and its second line did not. */
    for (char *scan = at; *scan; scan++) {
      if (*scan == ';' || *scan == '#') {
        *scan = '\0';
        break;
      }
    }
    char *end = at + strlen(at);
    while (end > at && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
      *--end = '\0';
    if (!*at) continue;

    if (*at == '[') {
      /* EVERY failure below must clear `room`. A section line that does not
       * resolve cannot leave the PREVIOUS room selected, or the plane lines that
       * follow are silently applied to it -- so a one-character typo in a header
       * changes a different room's rendering while reporting success. That was
       * the behaviour until this comment: the unterminated-'[' arm incremented
       * `bad` and continued without touching `room`. */
      char *close = strchr(at, ']');
      if (!close) {
        fprintf(stderr,
                "[diorama-layers] %s:%d: section missing ']' -- ignored, and "
                "the plane lines under it are skipped\n",
                kLayerManifestLeaf, line_number);
        room = NULL;
        bad++;
        continue;
      }
      *close = '\0';
      uint8_t group = 0, map = 0, section = kDioramaLayerSection_Room;
      if (!DioramaLayerOrder_ParseScopedSection(at + 1, &group, &map, &section)) {
        /* Not one of ours -- a foreign section just ends the current room. That
         * is legitimate (the file may grow other sections), so it is not
         * counted as bad. But it IS worth a line: the grammar is strict
         * ("layers:GG:MM[:token]", hex), so `[layers:04:01 ]` with one
         * stray space lands here and would otherwise drop a whole authored room
         * with no output at all. Reported at a lower volume than an error. */
        if (!strncmp(at + 1, "layers", 6))
          fprintf(stderr,
                  "[diorama-layers] %s:%d: '[%s]' is not a valid room header "
                  "(expected [layers:GG:MM[:token]] in hex) -- its plane lines are "
                  "skipped\n",
                  kLayerManifestLeaf, line_number, at + 1);
        room = NULL;
        continue;
      }
      room = DioramaLayerOrder_FindOrAddSection(loaded, group, map, section);
      if (!room) {
        fprintf(stderr, "[diorama-layers] %s:%d: table full, room dropped\n", kLayerManifestLeaf,
                line_number);
        bad++;
      } else {
        rooms++;
      }
      continue;
    }
    if (!room) continue;
    const char *error = NULL;
    if (DioramaLayerOrder_ParseLine(room, at, &error)) {
      planes++;
    } else {
      bad++;
      fprintf(stderr, "[diorama-layers] %s:%d: %s -- line skipped\n", kLayerManifestLeaf,
              line_number, error ? error : "bad line");
    }
  }
  bool complete = !ferror(file);
  if (fclose(file) != 0) complete = false;
  if (!complete) {
    free(loaded);
    fprintf(stderr, "[diorama-layers] cannot read %s -- previous overrides kept\n", path);
    return false;
  }
  s_layer_overrides = *loaded;
  free(loaded);
  fprintf(stderr, "[diorama-layers] loaded %s: %d room(s), %d plane override(s)%s\n", path, rooms,
          planes, bad ? ", some lines skipped" : "");
  return true;
}

/* The documentation a genuinely NEW manifest is seeded with. An existing file
 * keeps whatever preamble it already has -- the whole point of the merge is that
 * this text is never allowed to overwrite the user's. */
static const char kLayerManifestPreamble[] =
    "# Diorama per-room layer overrides.\n"
    "#\n"
    "# THERE IS AN IN-GAME EDITOR for this file: turn on \"Show developer\n"
    "# settings\" (System > Game) and a \"Layers\" section appears in the settings\n"
    "# menu. Left/Right cycles a plane through the shapes below and the result is\n"
    "# on screen immediately. Every edit is written back here, so the two are\n"
    "# interchangeable -- and your comments and layout are PRESERVED across a save.\n"
    "#\n"
    "# Section is [layers:GG:MM] with $18/$19 in hex. A camera-local refinement\n"
    "# may use [layers:GG:MM:waterfall] and inherits the base room first. Keys are\n"
    "#   order:<slot>  z:<-1..2>  alpha:<0-255>  rake:<-1..1>  bow:<-1..1>"
    "  thick:<0..1>\n"
    "#   stack:<0..1>  copies:<1..8>  density:<per unit>  dir:<forward|"
    "backward|both>\n"
    "#   voxel:<0..1>  slices:<2..24>\n"
    "# Base BG1/BG2 accept transparent:off, transparent:black, or\n"
    "# transparent:cgram-XX. Off suppresses an inherited room fill. A fill\n"
    "# fills the complete low plane before tiles paint, including untiled areas;\n"
    "# mirror/repeat/clamp remain tile policies and the high band stays sparse.\n"
    "# Action BG virtual depth bands share the same room section. Band 0 is the\n"
    "# new far plane, band 1 the ordinary BG plane, and band 2 its priority-1\n"
    "# plane. Cell rectangles override metatile rules; the ROM priority bit is\n"
    "# the fallback. The virtual plane itself accepts z/order/alpha:\n"
    "#   bg1-virtual = z:0.35 order:4 alpha:255\n"
    "#   bg1-virtual = metatile:23 band:0\n"
    "#   bg1-virtual = cells:4,5-12,5 band:2\n"
    "# Backdrop's source key selects the SKYBOX: captured uses current BG2;\n"
    "# rom-GG-MM-bgN (N=1/2) decodes a stock action BG. Backdrop alpha/z/order\n"
    "# control only the residual plane and do not disable that skybox source.\n"
    "# A named ROM BG source follows the same fill-then-paint rule.\n"
    "# rake tilts a plane in depth (top keeps z, bottom sits at z+rake); bow is\n"
    "# the same tilt EASED. thick extrudes the bottom edge forward. stack fills\n"
    "# the gap with PARALLEL repeats (no tilt, one parallax rate); dir picks which\n"
    "# side to fill and density sets slices per unit depth. voxel is a dense\n"
    "# unfaded stack -- one SOLID object that, unlike thick, respects the art's\n"
    "# silhouette. All compose.\n\n";

/* Preserve every byte before merging. A partial read, oversized document or
 * embedded NUL must never turn into a successful but destructive replacement. */
static bool ReadExistingManifest(const char *path, char **out) {
  enum { kManifestReadMax = 1 << 20 };
  *out = NULL;
  FILE *file = sr_fopen(path, "rb");
  if (!file) return errno == ENOENT;
  bool complete = fseek(file, 0, SEEK_END) == 0;
  const long length = complete ? ftell(file) : -1;
  complete = complete && length >= 0 && length <= kManifestReadMax;
  if (complete) complete = fseek(file, 0, SEEK_SET) == 0;
  char *text = complete ? malloc((size_t)length + 1) : NULL;
  if (!text) complete = false;
  if (complete) {
    const size_t got = fread(text, 1, (size_t)length, file);
    complete =
        got == (size_t)length && fgetc(file) == EOF && !ferror(file) && !memchr(text, '\0', got);
    text[got] = '\0';
  }
  if (fclose(file) != 0) complete = false;
  if (!complete) {
    free(text);
    return false;
  }
  *out = text;
  return true;
}

bool DioramaLayerManifest_Save(void) {
  char path[kHostPathCapacity];
  UserDataFile(path, sizeof path, kLayerManifestLeaf);

  char *existing = NULL;
  if (!ReadExistingManifest(path, &existing)) {
    fprintf(stderr, "[diorama-layers] cannot read all of %s -- original kept\n", path);
    return false;
  }

  /* Size the merged output, then render it. Two passes over a pure function is
   * cheaper and safer than guessing a bound -- and the merge preserves the whole
   * input, so its size is roughly the file's size plus a room or two. */
  size_t need = DioramaLayerOrder_MergeManifest(&s_layer_overrides, existing,
                                                kLayerManifestPreamble, NULL, 0);
  char *out = (char *)malloc(need + 1);
  if (!out) {
    free(existing);
    fprintf(stderr, "[diorama-layers] out of memory writing %s\n", path);
    return false;
  }
  size_t wrote = DioramaLayerOrder_MergeManifest(&s_layer_overrides, existing,
                                                 kLayerManifestPreamble, out, need + 1);
  free(existing);
  if (wrote != need) {
    free(out);
    fprintf(stderr, "[diorama-layers] could not merge %s -- original kept\n", path);
    return false;
  }

  /* Write to a temp file and rename, so a crash mid-write cannot leave the
   * user's manifest truncated -- this file may hold hand-authored content that
   * is not reproducible from the table. */
  /* Room for the manifest path plus the ".tmp" suffix. */
  char tmp[sizeof(path) + 64];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *file = sr_fopen(tmp, "wb");
  if (!file) {
    free(out);
    fprintf(stderr, "[diorama-layers] cannot write %s\n", tmp);
    return false;
  }
  size_t put = fwrite(out, 1, wrote, file);
  free(out);
  bool complete = put == wrote && fflush(file) == 0;
  if (fclose(file) != 0) complete = false;
  if (!complete) {
    sr_remove(tmp);
    fprintf(stderr, "[diorama-layers] incomplete write to %s -- original kept\n", tmp);
    return false;
  }
  /* One atomic replace on both platforms. A bare rename() FAILS on Windows when
   * the destination exists (packaging builds windows-x86_64/arm64), so every save
   * after the first would silently stop persisting the user's edits -- the same
   * trap handled by the ActRaiser Builder's ROM storage. See atomic_replace.h;
   * this file may
   * contain hand-authored rooms that cannot be reproduced from the table, so
   * "original kept" below must be TRUE. */
  if (!AtomicReplaceFile(tmp, path)) {
    sr_remove(tmp);
    fprintf(stderr, "[diorama-layers] could not replace %s -- original kept\n", path);
    return false;
  }

  int active = 0;
  for (int i = 0; i < s_layer_overrides.count; i++)
    if (DioramaLayerOrder_RoomIsActive(&s_layer_overrides.rooms[i])) active++;
  fprintf(stderr, "[diorama-layers] wrote %s (%d room(s), comments preserved)\n", path, active);
  return true;
}
