#include "actraiser/actraiser_ar2_master_assets.h"

#include "actraiser/quintet_lzss.h"
#include "snesrecomp/support/digest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ActRaiser 2 (USA) is a 12 Mbit fast HiROM cart: banks $C0-$D7, mirrored at
 * $80-$97 for $8000-$FFFF. Every address below is a SNES bus address. */
enum {
  kAr2RomBytes = 0x180000,
  kCopierHeaderBytes = 512,
  kHeaderTitle = 0xFFC0,
  kHeaderComplement = 0xFFDC,
  kHeaderChecksum = 0xFFDE,
  kAnimBank = 0x87,
  kAnimTable = 0x8000,
  kTileBankBase = 0x88,
  kTileWindow = 0x8000,
  kMotionPackedBank = 0xCF,
  kMotionPackedAddress = 0x06F2,
  kPaletteBank = 0xD7,
  kPaletteAddress = 0x8139,
  kFrameDmaBytes = 5, /* size word, source word, bank byte before the def */
  kFrameHeaderBytes = 14,
  kPartBytes = 7,
};

static const char kAr2Title[] = "ActRaiser-2 USA";
static const uint8_t kAr2Sha256[32] = {
    0x71, 0xbd, 0xd4, 0x48, 0xa3, 0x0b, 0x88, 0x72, 0x58, 0x64, 0xe5, 0x55, 0x94, 0xeb, 0xb6, 0x7a,
    0x11, 0x8b, 0x1f, 0x19, 0x7a, 0x3f, 0x9e, 0x5d, 0xd3, 0x9d, 0xbf, 0x23, 0x39, 0x9e, 0xfa, 0x15};

static void SetError(char *error, size_t error_size, const char *message) {
  if (error && error_size) snprintf(error, error_size, "%s", message);
}

static bool HiromOffset(size_t rom_size, uint8_t bank, uint16_t address, uint32_t *offset) {
  if (bank < 0x80 || (bank < 0xC0 && address < 0x8000)) return false;
  const uint32_t linear = ((uint32_t)(bank & 0x3F) << 16) | address;
  if (linear >= rom_size) return false;
  *offset = linear;
  return true;
}

static bool ReadByte(const uint8_t *rom, size_t rom_size, uint8_t bank, uint16_t address,
                     uint8_t *value) {
  uint32_t offset;
  if (!HiromOffset(rom_size, bank, address, &offset)) return false;
  *value = rom[offset];
  return true;
}

static bool ReadWord(const uint8_t *rom, size_t rom_size, uint8_t bank, uint16_t address,
                     uint16_t *value) {
  uint8_t lo, hi;
  if (!ReadByte(rom, rom_size, bank, address, &lo) ||
      !ReadByte(rom, rom_size, bank, (uint16_t)(address + 1), &hi))
    return false;
  *value = (uint16_t)(lo | hi << 8);
  return true;
}

bool Ar2MasterAssets_IdentifyRom(const uint8_t *image, size_t image_size, const uint8_t **payload,
                                 size_t *payload_size, char *error, size_t error_size) {
  if (!image || !payload || !payload_size) {
    SetError(error, error_size, "no ActRaiser 2 image");
    return false;
  }
  if (image_size == kAr2RomBytes + kCopierHeaderBytes) {
    image += kCopierHeaderBytes;
    image_size -= kCopierHeaderBytes;
  }
  if (image_size != kAr2RomBytes) {
    SetError(error, error_size, "not a 12 Mbit ActRaiser 2 image");
    return false;
  }
  if (memcmp(image + kHeaderTitle, kAr2Title, sizeof(kAr2Title) - 1) != 0) {
    SetError(error, error_size, "internal title is not ActRaiser-2 USA");
    return false;
  }
  /* The SNES checksum mirrors the final 4 Mbit of a 12 Mbit image once. */
  uint32_t sum = 0;
  for (size_t i = 0; i < 0x100000; i++)
    sum += image[i];
  for (size_t i = 0x100000; i < kAr2RomBytes; i++)
    sum += 2u * image[i];
  const uint16_t checksum = (uint16_t)(image[kHeaderChecksum] | image[kHeaderChecksum + 1] << 8);
  const uint16_t complement =
      (uint16_t)(image[kHeaderComplement] | image[kHeaderComplement + 1] << 8);
  if ((uint16_t)sum != checksum || (uint16_t)(checksum ^ complement) != 0xFFFF) {
    SetError(error, error_size, "ActRaiser 2 header checksum does not match");
    return false;
  }
  uint8_t digest[32];
  if (!sr_support_sha256(image, image_size, digest) ||
      memcmp(digest, kAr2Sha256, sizeof(digest)) != 0) {
    SetError(error, error_size, "unsupported ActRaiser 2 revision (SHA-256 mismatch)");
    return false;
  }
  *payload = image;
  *payload_size = image_size;
  return true;
}

uint16_t Ar2MasterAssets_MotionWord(const Ar2MasterAssets *assets, uint16_t address) {
  if (!assets || address < kAr2MasterMotionBase) return 0;
  const size_t at = (size_t)(address - kAr2MasterMotionBase);
  if (at + 1 >= kAr2MasterMotionBytes) return 0;
  return (uint16_t)(assets->motion[at] | assets->motion[at + 1] << 8);
}

void Ar2MasterAssets_MotionPrograms(const Ar2MasterAssets *assets, unsigned motion,
                                    uint16_t *x_program, uint16_t *y_program) {
  uint16_t x = 0, y = 0;
  if (assets && motion < kAr2MasterMotionCount) {
    const uint16_t entry = (uint16_t)(kAr2MasterMotionBase + motion * 4u);
    x = Ar2MasterAssets_MotionWord(assets, entry);
    y = Ar2MasterAssets_MotionWord(assets, (uint16_t)(entry + 2));
  }
  if (x_program) *x_program = x;
  if (y_program) *y_program = y;
}

void Ar2MotionAxis_Start(Ar2MotionAxis *axis, uint16_t program) {
  axis->pointer = program;
  axis->counter = 0;
}

int16_t Ar2MotionAxis_Step(const Ar2MasterAssets *assets, Ar2MotionAxis *axis) {
  if (!axis->pointer) return 0;
  /* The pointer always names the current delta. A started program has a
   * zero counter, so its first step immediately advances to the first
   * (count, delta) pair; a negative count word is a jump. */
  if (--axis->counter < 0) {
    axis->pointer = (uint16_t)(axis->pointer + 2);
    uint16_t count = Ar2MasterAssets_MotionWord(assets, axis->pointer);
    if (count & 0x8000) {
      axis->pointer = Ar2MasterAssets_MotionWord(assets, (uint16_t)(axis->pointer + 2));
      count = Ar2MasterAssets_MotionWord(assets, axis->pointer);
      if (count & 0x8000) { /* malformed: never loop on a jump to a jump */
        axis->pointer = 0;
        return 0;
      }
    }
    axis->counter = (int16_t)count;
    axis->pointer = (uint16_t)(axis->pointer + 2);
  }
  return (int16_t)Ar2MasterAssets_MotionWord(assets, axis->pointer);
}

static int FindOrParseFrame(const uint8_t *rom, size_t rom_size, Ar2MasterAssets *assets,
                            uint16_t *addresses, uint16_t definition, char *error,
                            size_t error_size) {
  for (unsigned i = 0; i < assets->frame_count; i++)
    if (addresses[i] == definition) return (int)i;
  if (assets->frame_count >= kAr2MasterMaxFrames) {
    SetError(error, error_size, "too many ActRaiser 2 master frames");
    return -1;
  }
  Ar2MasterFrame frame = {0};
  uint16_t size = 0, source = 0;
  uint8_t bank_step = 0;
  uint8_t header[kFrameHeaderBytes + 1];
  bool ok = ReadWord(rom, rom_size, kAnimBank, (uint16_t)(definition - kFrameDmaBytes), &size) &&
            ReadWord(rom, rom_size, kAnimBank, (uint16_t)(definition - 3), &source) &&
            ReadByte(rom, rom_size, kAnimBank, (uint16_t)(definition - 1), &bank_step);
  for (unsigned i = 0; ok && i < sizeof(header); i++)
    ok = ReadByte(rom, rom_size, kAnimBank, (uint16_t)(definition + i), &header[i]);
  if (!ok || !size || size > kAr2MasterTileBufferBytes || (size & 31) || source >= kTileWindow) {
    SetError(error, error_size, "malformed ActRaiser 2 frame definition");
    return -1;
  }
  uint32_t tile_offset;
  if (!HiromOffset(rom_size, (uint8_t)(kTileBankBase + bank_step), (uint16_t)(kTileWindow + source),
                   &tile_offset) ||
      tile_offset + size > rom_size) {
    SetError(error, error_size, "ActRaiser 2 frame tiles are outside the ROM");
    return -1;
  }
  frame.tile_offset = tile_offset;
  frame.tile_bytes = size;
  frame.left = header[0];
  frame.right = header[1];
  frame.top = header[2];
  frame.bottom = header[3];
  frame.hurt_half_width = header[6];
  frame.hurt_height = header[7];
  frame.attack_x = (int16_t)(header[8] | header[9] << 8);
  frame.attack_y = (int16_t)(header[10] | header[11] << 8);
  frame.attack_width = header[12];
  frame.attack_height = header[13];
  frame.has_attack = frame.attack_y != 0;
  frame.part_count = header[kFrameHeaderBytes];
  if (frame.part_count > kAr2MasterMaxParts) {
    SetError(error, error_size, "ActRaiser 2 frame has too many sprite parts");
    return -1;
  }
  for (unsigned i = 0; i < frame.part_count; i++) {
    uint8_t raw[kPartBytes];
    const uint16_t at = (uint16_t)(definition + kFrameHeaderBytes + 1 + i * kPartBytes);
    for (unsigned b = 0; b < kPartBytes; b++)
      if (!ReadByte(rom, rom_size, kAnimBank, (uint16_t)(at + b), &raw[b])) {
        SetError(error, error_size, "ActRaiser 2 sprite part is outside the ROM");
        return -1;
      }
    Ar2MasterPart *part = &frame.parts[i];
    part->large = (raw[0] & 1) != 0;
    part->x = raw[1];
    part->x_flipped = raw[2];
    part->y = raw[3];
    part->y_flipped = raw[4];
    part->attributes = (uint16_t)(raw[5] | raw[6] << 8);
    /* The streamed block holds at most 64 tiles; a large part also reads the
     * row below. Anything else would sample unrelated OBJ VRAM. */
    const unsigned last_tile = (part->attributes & 0xFFu) + (part->large ? 17u : 0u);
    if ((part->attributes & 0x3F00u) || last_tile >= kAr2MasterTileBufferBytes / 32u) {
      SetError(error, error_size, "ActRaiser 2 sprite part uses unexpected tiles");
      return -1;
    }
  }
  addresses[assets->frame_count] = definition;
  assets->frames[assets->frame_count] = frame;
  return assets->frame_count++;
}

bool Ar2MasterAssets_Parse(const uint8_t *rom, size_t rom_size, Ar2MasterAssets *assets,
                           char *error, size_t error_size) {
  if (!rom || !assets) {
    SetError(error, error_size, "no ActRaiser 2 image");
    return false;
  }
  memset(assets, 0, sizeof(*assets));
  uint16_t addresses[kAr2MasterMaxFrames] = {0};
  for (unsigned anim = 0; anim < kAr2MasterAnimCount; anim++) {
    uint16_t list;
    if (!ReadWord(rom, rom_size, kAnimBank, (uint16_t)(kAnimTable + anim * 2), &list)) {
      SetError(error, error_size, "ActRaiser 2 animation table is unreadable");
      return false;
    }
    Ar2MasterAnim *out = &assets->anims[anim];
    for (unsigned step = 0;; step++) {
      uint16_t duration, definition;
      if (!ReadWord(rom, rom_size, kAnimBank, (uint16_t)(list + step * 4), &duration) ||
          !ReadWord(rom, rom_size, kAnimBank, (uint16_t)(list + step * 4 + 2), &definition)) {
        SetError(error, error_size, "ActRaiser 2 animation is unreadable");
        return false;
      }
      if (duration & 0x8000) break;
      if (step >= kAr2MasterMaxSteps) {
        SetError(error, error_size, "ActRaiser 2 animation is too long");
        return false;
      }
      const int frame =
          FindOrParseFrame(rom, rom_size, assets, addresses, definition, error, error_size);
      if (frame < 0) return false;
      out->steps[step].duration = duration;
      out->steps[step].frame = (uint8_t)frame;
      out->step_count = (uint8_t)(step + 1);
    }
    if (!out->step_count) {
      SetError(error, error_size, "ActRaiser 2 animation is empty");
      return false;
    }
  }

  uint32_t packed;
  if (!HiromOffset(rom_size, kMotionPackedBank, kMotionPackedAddress, &packed) ||
      !QuintetLzss_DecompressAsset(rom + packed, rom_size - packed, assets->motion,
                                   kAr2MasterMotionBytes, NULL)) {
    SetError(error, error_size, "ActRaiser 2 motion programs did not unpack");
    return false;
  }
  for (unsigned motion = 0; motion < kAr2MasterMotionCount; motion++) {
    uint16_t x, y;
    Ar2MasterAssets_MotionPrograms(assets, motion, &x, &y);
    if ((x && x < kAr2MasterMotionBase) || (y && y < kAr2MasterMotionBase)) {
      SetError(error, error_size, "ActRaiser 2 motion table is inconsistent");
      return false;
    }
  }

  for (unsigned i = 0; i < 16; i++) {
    uint16_t color;
    if (!ReadWord(rom, rom_size, kPaletteBank, (uint16_t)(kPaletteAddress + i * 2), &color)) {
      SetError(error, error_size, "ActRaiser 2 master palette is unreadable");
      return false;
    }
    assets->palette[i] = (uint16_t)(color & 0x7FFF);
  }
  assets->data = rom;
  assets->data_size = rom_size;
  return true;
}

static bool PoolHas(const uint8_t pool[32], unsigned tile) {
  return tile < 256 && (pool[tile >> 3] >> (tile & 7) & 1);
}

static bool AddCopy(Ar2MasterPacking *packing, unsigned tile, unsigned source) {
  if (packing->copy_count >= kAr2MasterMaxCopies) return false;
  packing->copies[packing->copy_count++] = (Ar2MasterTileCopy){(uint8_t)tile, (uint8_t)source};
  return true;
}

bool Ar2MasterAssets_PackFrame(const Ar2MasterFrame *frame, const uint8_t pool[32],
                               Ar2MasterPacking *packing) {
  if (!frame || !pool || !packing) return false;
  memset(packing, 0, sizeof(*packing));
  const unsigned block_tiles = frame->tile_bytes / 32u;
  uint8_t used[32] = {0};
  /* Destination of each source tile once copied, plus one: 0 is "none". */
  uint16_t placed[64] = {0};
  uint16_t placed_block[64] = {0};
  for (unsigned i = 0; i < frame->part_count; i++) {
    const Ar2MasterPart *part = &frame->parts[i];
    if (!part->large) continue;
    const unsigned source = part->attributes & 0xFFu;
    if (source + 17 >= block_tiles) return false;
    if (placed_block[source]) {
      packing->part_tiles[i] = (uint8_t)(placed_block[source] - 1);
      continue;
    }
    unsigned tile = 0;
    for (; tile < 0xF0; tile++) {
      if ((tile & 15) == 15) continue;
      const unsigned block[4] = {tile, tile + 1, tile + 16, tile + 17};
      bool free = true;
      for (unsigned b = 0; b < 4 && free; b++)
        free = PoolHas(pool, block[b]) && !PoolHas(used, block[b]);
      if (free) break;
    }
    if (tile >= 0xF0) return false;
    static const unsigned kOffsets[4] = {0, 1, 16, 17};
    for (unsigned b = 0; b < 4; b++) {
      const unsigned at = tile + kOffsets[b], from = source + kOffsets[b];
      used[at >> 3] |= (uint8_t)(1u << (at & 7));
      if (!AddCopy(packing, at, from)) return false;
      if (!placed[from]) placed[from] = (uint16_t)(at + 1);
    }
    placed_block[source] = (uint16_t)(tile + 1);
    packing->part_tiles[i] = (uint8_t)tile;
  }
  for (unsigned i = 0; i < frame->part_count; i++) {
    const Ar2MasterPart *part = &frame->parts[i];
    if (part->large) continue;
    const unsigned source = part->attributes & 0xFFu;
    if (source >= block_tiles) return false;
    if (!placed[source]) {
      unsigned tile = 0;
      while (tile < 256 && (!PoolHas(pool, tile) || PoolHas(used, tile)))
        tile++;
      if (tile >= 256 || !AddCopy(packing, tile, source)) return false;
      used[tile >> 3] |= (uint8_t)(1u << (tile & 7));
      placed[source] = (uint16_t)(tile + 1);
    }
    packing->part_tiles[i] = (uint8_t)(placed[source] - 1);
  }
  return true;
}

static int Channel(uint16_t color, unsigned channel) { return (color >> (5 * channel)) & 31; }

/* Rec. 601 luma on 5-bit channels, scaled by 256. */
static int Luma(uint16_t color) {
  return 77 * Channel(color, 0) + 150 * Channel(color, 1) + 29 * Channel(color, 2);
}

void Ar2MasterAssets_TranslateFade(const uint16_t ar2[16], const uint16_t stone[16],
                                   const uint16_t final_colors[16], const uint16_t step[16],
                                   uint16_t out[16]) {
  /* Least-squares position of the step on the stone -> final line, and how
   * far the step lies off that line relative to the line's own length. */
  double along = 0, length = 0;
  for (unsigned i = 1; i < 16; i++)
    for (unsigned c = 0; c < 3; c++) {
      const int span = Channel(final_colors[i], c) - Channel(stone[i], c);
      along += (double)(Channel(step[i], c) - Channel(stone[i], c)) * span;
      length += (double)span * span;
    }
  double t = length > 0 ? along / length : 1.0;
  double off_line = 0;
  for (unsigned i = 1; i < 16; i++)
    for (unsigned c = 0; c < 3; c++) {
      const double expected =
          Channel(stone[i], c) + t * (Channel(final_colors[i], c) - Channel(stone[i], c));
      const double miss = Channel(step[i], c) - expected;
      off_line += miss * miss;
    }
  /* The native blends sit within 3% of the line; its glows are far off it. */
  const bool blend = length > 0 && off_line <= 0.1 * length;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  out[0] = ar2[0];
  for (unsigned i = 1; i < 16; i++) {
    unsigned match = 1;
    for (unsigned j = 2; j < 16; j++) {
      const int best = abs(Luma(final_colors[match]) - Luma(ar2[i]));
      if (abs(Luma(final_colors[j]) - Luma(ar2[i])) < best) match = j;
    }
    if (!blend) {
      out[i] = step[match] & 0x7FFF;
      continue;
    }
    uint16_t color = 0;
    for (unsigned c = 0; c < 3; c++) {
      const double value =
          Channel(stone[match], c) + t * (Channel(ar2[i], c) - Channel(stone[match], c));
      color |= (uint16_t)((unsigned)(value + 0.5) << (5 * c));
    }
    out[i] = color;
  }
}
