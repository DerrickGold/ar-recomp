#include "actraiser/actraiser_ar2_master_assets.h"
#include "byte_order.h"
#include "snesrecomp/support/digest.h"

#include <stdio.h>
#include <string.h>

/* AR2MASTR v1: 48-byte header (magic, version, payload size, SHA-256),
 * palette, motion table, frame count, 37 length-prefixed animations, then
 * frame records (native 15-byte header, seven-byte parts, tile size, tiles).
 * This matches installer/internal/ar2master; no cartridge code is retained. */
bool Ar2MasterAssets_ParsePack(const uint8_t *data, size_t size, Ar2MasterAssets *assets,
                               char *error, size_t error_size) {
  if (!data || !assets || size < 48 + 32 + kAr2MasterMotionBytes + 2 ||
      size > kAr2MasterPackMaximumBytes || memcmp(data, "AR2MASTR", 8) ||
      ByteOrder_ReadLe32(data + 8) != 1 || ByteOrder_ReadLe32(data + 12) != size - 48)
    goto bad;
  uint8_t digest[32];
  if (!sr_support_sha256(data + 48, size - 48, digest) || memcmp(digest, data + 16, 32)) goto bad;
  memset(assets, 0, sizeof(*assets));
  for (unsigned i = 0; i < 16; i++)
    assets->palette[i] = ByteOrder_ReadLe16(data + 48 + i * 2);
  memcpy(assets->motion, data + 80, kAr2MasterMotionBytes);
  size_t at = 80 + kAr2MasterMotionBytes;
  assets->frame_count = ByteOrder_ReadLe16(data + at);
  at += 2;
  if (!assets->frame_count || assets->frame_count > kAr2MasterMaxFrames) goto bad;
  for (unsigned i = 0; i < kAr2MasterAnimCount; i++) {
    if (at >= size) goto bad;
    Ar2MasterAnim *anim = &assets->anims[i];
    anim->step_count = data[at++];
    if (!anim->step_count || anim->step_count > kAr2MasterMaxSteps ||
        size - at < anim->step_count * 3u)
      goto bad;
    for (unsigned j = 0; j < anim->step_count; j++, at += 3) {
      anim->steps[j].duration = ByteOrder_ReadLe16(data + at);
      anim->steps[j].frame = data[at + 2];
      if (anim->steps[j].duration & 0x8000 || anim->steps[j].frame >= assets->frame_count) goto bad;
    }
  }
  for (unsigned i = 0; i < assets->frame_count; i++) {
    if (size - at < 15) goto bad;
    Ar2MasterFrame *f = &assets->frames[i];
    f->left = data[at];
    f->right = data[at + 1];
    f->top = data[at + 2];
    f->bottom = data[at + 3];
    f->hurt_half_width = data[at + 6];
    f->hurt_height = data[at + 7];
    f->attack_x = (int16_t)ByteOrder_ReadLe16(data + at + 8);
    f->attack_y = (int16_t)ByteOrder_ReadLe16(data + at + 10);
    f->attack_width = data[at + 12];
    f->attack_height = data[at + 13];
    f->has_attack = f->attack_y != 0;
    f->part_count = data[at + 14];
    at += 15;
    if (f->part_count > kAr2MasterMaxParts || size - at < f->part_count * 7u + 2) goto bad;
    for (unsigned j = 0; j < f->part_count; j++, at += 7) {
      f->parts[j] = (Ar2MasterPart){.large = (data[at] & 1) != 0,
                                    .x = data[at + 1],
                                    .x_flipped = data[at + 2],
                                    .y = data[at + 3],
                                    .y_flipped = data[at + 4],
                                    .attributes = ByteOrder_ReadLe16(data + at + 5)};
    }
    f->tile_bytes = ByteOrder_ReadLe16(data + at);
    at += 2;
    if (!f->tile_bytes || f->tile_bytes > kAr2MasterTileBufferBytes || f->tile_bytes % 32 ||
        size - at < f->tile_bytes)
      goto bad;
    f->tile_offset = (uint32_t)at;
    at += f->tile_bytes;
    for (unsigned j = 0; j < f->part_count; j++) {
      const Ar2MasterPart *p = &f->parts[j];
      const unsigned last = (p->attributes & 0xFF) + (p->large ? 17u : 0);
      if (p->attributes & 0x3F00 || last >= f->tile_bytes / 32u) goto bad;
    }
  }
  if (at != size) goto bad;
  assets->data = data;
  assets->data_size = size;
  return true;
bad:
  if (assets) memset(assets, 0, sizeof(*assets));
  if (error && error_size)
    snprintf(error, error_size, "character assets are incomplete, modified or unsupported");
  return false;
}
