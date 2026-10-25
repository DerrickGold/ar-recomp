#ifndef AR_ACTRAISER_AR2_MASTER_ASSETS_H
#define AR_ACTRAISER_AR2_MASTER_ASSETS_H
/* ActRaiser 2 master assets, extracted from the player's ActRaiser 2 (USA)
 * cartridge dump and loaded from a compact Workshop pack at runtime: the master's animation
 * programs ($87:8000), frame definitions (draw extents, body and attack boxes, sprite parts), the
 * tile blocks the game streams into OBJ VRAM per frame ($88-$8A), the motion programs (LZSS blob
 * $CF:06F2, unpacked to WRAM $7F:6000 by the game) and the master palette. Nothing derived from
 * either ROM is committed. Phase: pure (no CPU state). Tests: tests/actraiser_ar2_master_test.c */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kAr2MasterAnimCount = 37,
  kAr2MasterMaxFrames = 96,
  kAr2MasterMaxSteps = 12,
  kAr2MasterMaxParts = 24,
  /* Four 16-tile OBJ rows: the largest frame block is 56 tiles (1792 bytes). */
  kAr2MasterTileBufferBytes = 2048,
  kAr2MasterMotionCount = 32,
  kAr2MasterMotionBytes = 0x109C,
  /* The motion programs' own pointers address WRAM $7F:6000. */
  kAr2MasterMotionBase = 0x6000,
  kAr2MasterPackMaximumBytes = 256 * 1024,
};

/* AR2's sprite part is byte-for-byte the format AR1's emitter consumes:
 * offsets are unsigned and relative to the extents box's top-left corner. */
typedef struct Ar2MasterPart {
  uint8_t x, x_flipped, y, y_flipped;
  uint16_t attributes; /* tile number and H/V flip only */
  bool large;          /* 16x16, using tiles n, n+1, n+16, n+17 */
} Ar2MasterPart;

typedef struct Ar2MasterFrame {
  /* Draw extents around AR2's object origin, which is the master's feet. */
  uint8_t left, right, top, bottom;
  /* Body box above the feet, used by AR2's enemy-contact test. */
  uint8_t hurt_half_width, hurt_height;
  /* Attack box for a right-facing master: x/y offsets from the feet and its
   * size. AR2's resolver treats a zero y offset as "no attack box". */
  int16_t attack_x, attack_y;
  uint8_t attack_width, attack_height;
  bool has_attack;
  /* Raw 4bpp tile block, laid out 16 tiles per row as the game DMAs it. */
  uint32_t tile_offset;
  uint16_t tile_bytes;
  uint8_t part_count;
  Ar2MasterPart parts[kAr2MasterMaxParts];
} Ar2MasterFrame;

/* One animation step. The game shows the frame for duration + 1 game frames;
 * the script attached to the animation runs again when that timer expires. */
typedef struct Ar2MasterStep {
  uint16_t duration;
  uint8_t frame;
} Ar2MasterStep;

typedef struct Ar2MasterAnim {
  uint8_t step_count;
  Ar2MasterStep steps[kAr2MasterMaxSteps];
} Ar2MasterAnim;

typedef struct Ar2MasterAssets {
  Ar2MasterAnim anims[kAr2MasterAnimCount];
  Ar2MasterFrame frames[kAr2MasterMaxFrames];
  uint16_t frame_count;
  uint8_t motion[kAr2MasterMotionBytes];
  uint16_t palette[16];
  const uint8_t *data; /* Tile offsets are relative to this borrowed storage. */
  size_t data_size;
} Ar2MasterAssets;

/* Workshop pack; bytes must outlive the parsed view. No AR2 ROM is needed by
 * the game. All counts, references, tile spans and the digest are checked. */
bool Ar2MasterAssets_ParsePack(const uint8_t *data, size_t size, Ar2MasterAssets *assets,
                               char *error, size_t error_size);

/* Parses a validated image. On failure writes a one-line reason and leaves
 * *assets unusable. The image must outlive the assets (tiles are read from it). */
bool Ar2MasterAssets_Parse(const uint8_t *rom, size_t rom_size, Ar2MasterAssets *assets,
                           char *error, size_t error_size);

/* Identity gate for the supported dump: ActRaiser 2 (USA), 12 Mbit, by size,
 * internal title, header checksum and SHA-256. A 512-byte copier header is
 * skipped and reported through *payload. */
bool Ar2MasterAssets_IdentifyRom(const uint8_t *image, size_t image_size, const uint8_t **payload,
                                 size_t *payload_size, char *error, size_t error_size);

/* Little-endian word of the unpacked motion table at WRAM address $7F:addr.
 * Out-of-range addresses read as zero, which ends a program. */
uint16_t Ar2MasterAssets_MotionWord(const Ar2MasterAssets *assets, uint16_t address);

/* One axis of an AR2 motion program ($80:A46F): a run-length list of
 * (frames - 1, delta) pairs with negative counts as jumps. */
typedef struct Ar2MotionAxis {
  uint16_t pointer; /* zero: no motion on this axis */
  int16_t counter;
} Ar2MotionAxis;

void Ar2MotionAxis_Start(Ar2MotionAxis *axis, uint16_t program);
/* Advances one game frame and returns this frame's signed displacement. */
int16_t Ar2MotionAxis_Step(const Ar2MasterAssets *assets, Ar2MotionAxis *axis);
/* Program pointers for motion id; both zero for an unknown id. */
void Ar2MasterAssets_MotionPrograms(const Ar2MasterAssets *assets, unsigned motion,
                                    uint16_t *x_program, uint16_t *y_program);

/* A frame's parts moved onto host-chosen OBJ tiles (table 0), so the frame
 * never has to occupy tiles other sprites draw from. */
enum { kAr2MasterMaxCopies = 64 };
typedef struct Ar2MasterTileCopy {
  uint8_t tile;   /* destination OBJ tile */
  uint8_t source; /* tile index in the frame's streamed block */
} Ar2MasterTileCopy;
typedef struct Ar2MasterPacking {
  uint8_t part_tiles[kAr2MasterMaxParts]; /* replaces each part's tile number */
  uint8_t copy_count;
  Ar2MasterTileCopy copies[kAr2MasterMaxCopies];
} Ar2MasterPacking;

/* Places the frame's large parts on 2x2 blocks and its small parts on single
 * tiles drawn from pool (a 256-bit set, bit t = tile t). A 2x2 block never
 * wraps a 16-tile row or the table, so tile n's block is n, n+1, n+16, n+17
 * exactly as in the source. Parts that share source tiles share their copy.
 * False if the pool is too small or a part reads outside the frame's block. */
bool Ar2MasterAssets_PackFrame(const Ar2MasterFrame *frame, const uint8_t pool[32],
                               Ar2MasterPacking *packing);

/* Re-expresses one step of a native palette fade for the AR2 palette. The
 * native fade runs between stone and final_colors (ActRaiser's statue and
 * master palettes). A step that is a blend of the two becomes the same blend
 * of a stone AR2 palette and the AR2 colours; any other step (a glow or a
 * flash) gives each AR2 colour the step's colour at the native index of
 * nearest brightness. Index 0 is transparent and passes through. */
void Ar2MasterAssets_TranslateFade(const uint16_t ar2[16], const uint16_t stone[16],
                                   const uint16_t final_colors[16], const uint16_t step[16],
                                   uint16_t out[16]);

#endif
