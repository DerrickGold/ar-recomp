#include "sim_background_voxel_palette.h"

#include <string.h>

static uint8_t ClampChannel(int value) {
  return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static uint32_t Argb(uint8_t red, uint8_t green, uint8_t blue) {
  return 0xFF000000u | (uint32_t)red << 16 |
      (uint32_t)green << 8 | blue;
}

static uint32_t Shade(uint32_t colour, float scale,
                      int red_bias, int green_bias, int blue_bias) {
  uint8_t alpha = (uint8_t)(colour >> 24);
  int red = (int)(((colour >> 16) & 0xFF) * scale) + red_bias;
  int green = (int)(((colour >> 8) & 0xFF) * scale) + green_bias;
  int blue = (int)((colour & 0xFF) * scale) + blue_bias;
  return (uint32_t)alpha << 24 |
      (uint32_t)ClampChannel(red) << 16 |
      (uint32_t)ClampChannel(green) << 8 | ClampChannel(blue);
}

static uint32_t VaryColour(uint32_t colour, int variation) {
  uint8_t alpha = (uint8_t)(colour >> 24);
  int red = (int)((colour >> 16) & 0xFF) + variation;
  int green = (int)((colour >> 8) & 0xFF) + variation;
  int blue = (int)(colour & 0xFF) + variation;
  return (uint32_t)alpha << 24 |
      (uint32_t)ClampChannel(red) << 16 |
      (uint32_t)ClampChannel(green) << 8 | ClampChannel(blue);
}

static uint32_t TintColour(uint32_t colour,
                           int red, int green, int blue) {
  uint8_t alpha = (uint8_t)(colour >> 24);
  red += (int)((colour >> 16) & 0xFF);
  green += (int)((colour >> 8) & 0xFF);
  blue += (int)(colour & 0xFF);
  return (uint32_t)alpha << 24 |
      (uint32_t)ClampChannel(red) << 16 |
      (uint32_t)ClampChannel(green) << 8 | ClampChannel(blue);
}

static void SetMaterial(SimBackgroundVoxelPalette *palette,
                        SimBackgroundVoxelMaterial material,
                        uint32_t base) {
  palette->material[material][0] = Shade(base, 0.48f, -2, 0, 7);
  palette->material[material][1] = Shade(base, 0.70f, 1, 0, 3);
  palette->material[material][2] = base;
  palette->material[material][3] = Shade(base, 1.12f, 8, 6, 3);
}

static void SetCommonPalette(SimBackgroundVoxelPalette *palette) {
  SetMaterial(palette, kSimVoxelMaterial_Wall, Argb(194, 155, 85));
  SetMaterial(palette, kSimVoxelMaterial_WallLight, Argb(230, 204, 139));
  SetMaterial(palette, kSimVoxelMaterial_Roof, Argb(100, 55, 125));
  SetMaterial(palette, kSimVoxelMaterial_RoofLight, Argb(132, 78, 153));
  SetMaterial(palette, kSimVoxelMaterial_Trim, Argb(218, 198, 145));
  SetMaterial(palette, kSimVoxelMaterial_Dark, Argb(39, 28, 34));
  SetMaterial(palette, kSimVoxelMaterial_Wood, Argb(121, 70, 31));
  SetMaterial(palette, kSimVoxelMaterial_Metal, Argb(110, 116, 113));
  SetMaterial(palette, kSimVoxelMaterial_Blade, Argb(239, 237, 220));
  SetMaterial(palette, kSimVoxelMaterial_Trunk, Argb(91, 55, 27));
  SetMaterial(palette, kSimVoxelMaterial_Leaves, Argb(22, 132, 35));
  SetMaterial(palette, kSimVoxelMaterial_LeavesLight, Argb(62, 177, 51));
  SetMaterial(palette, kSimVoxelMaterial_LeavesDark, Argb(7, 79, 24));
  SetMaterial(palette, kSimVoxelMaterial_Paving, Argb(150, 133, 101));
  SetMaterial(palette, kSimVoxelMaterial_Gold, Argb(222, 164, 38));
  SetMaterial(palette, kSimVoxelMaterial_Glass, Argb(45, 72, 91));
  SetMaterial(palette, kSimVoxelMaterial_Snow, Argb(226, 236, 240));
  for (int level = 0; level < kSimBackgroundVoxelPaletteRampLevels; level++)
    palette->material[kSimVoxelMaterial_Contact][level] = 0x380B1014u;
}

static bool IsFoliage(SimBackgroundVoxelMaterial material) {
  return material == kSimVoxelMaterial_Leaves ||
      material == kSimVoxelMaterial_LeavesLight ||
      material == kSimVoxelMaterial_LeavesDark;
}

static void ApplyBiomePalette(SimBackgroundVoxelPalette *palette,
                              SimBackgroundVoxelBiome biome) {
  int red = 0, green = 0, blue = 0;
  switch (biome) {
    case kSimBackgroundVoxelBiome_Temperate: return;
    case kSimBackgroundVoxelBiome_Wetland:
      red = -7; green = -2; blue = 4;
      break;
    case kSimBackgroundVoxelBiome_Desert:
      red = 11; green = 5; blue = -11;
      break;
    case kSimBackgroundVoxelBiome_Volcanic:
      red = -17; green = -15; blue = -10;
      break;
    case kSimBackgroundVoxelBiome_Tropical:
      red = 2; green = 8; blue = -3;
      break;
    case kSimBackgroundVoxelBiome_Snow:
      red = -5; green = 3; blue = 10;
      break;
    case kSimBackgroundVoxelBiome_Count: return;
  }
  for (int material = 0; material < kSimVoxelMaterial_Count; material++) {
    if (material == kSimVoxelMaterial_Contact ||
        material == kSimVoxelMaterial_Snow)
      continue;
    int material_red = red;
    int material_green = green;
    int material_blue = blue;
    if (IsFoliage((SimBackgroundVoxelMaterial)material)) {
      if (biome == kSimBackgroundVoxelBiome_Desert) {
        material_red -= 4;
        material_green -= 6;
        material_blue -= 5;
      } else if (biome == kSimBackgroundVoxelBiome_Tropical) {
        material_green += 8;
        material_blue += 4;
      } else if (biome == kSimBackgroundVoxelBiome_Snow) {
        material_red -= 2;
        material_green -= 8;
      }
    }
    uint32_t base = palette->material[material][2];
    SetMaterial(palette, (SimBackgroundVoxelMaterial)material,
                TintColour(base, material_red, material_green,
                           material_blue));
  }
}

void SimBackgroundVoxelPalette_Build(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelBiome biome,
    SimBackgroundVoxelPalette *palette) {
  if (!palette) return;
  memset(palette, 0, sizeof(*palette));
  SetCommonPalette(palette);
  if (!object) return;
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      SetMaterial(palette, kSimVoxelMaterial_Wall, Argb(202, 158, 81));
      SetMaterial(palette, kSimVoxelMaterial_WallLight,
                  Argb(238, 204, 125));
      SetMaterial(palette, kSimVoxelMaterial_Roof, Argb(123, 64, 34));
      SetMaterial(palette, kSimVoxelMaterial_RoofLight,
                  Argb(163, 87, 42));
      break;
    case kSimBackgroundVoxel_Cathedral:
      SetMaterial(palette, kSimVoxelMaterial_Wall, Argb(194, 200, 190));
      SetMaterial(palette, kSimVoxelMaterial_WallLight,
                  Argb(235, 236, 220));
      SetMaterial(palette, kSimVoxelMaterial_Roof, Argb(172, 181, 178));
      SetMaterial(palette, kSimVoxelMaterial_RoofLight,
                  Argb(239, 239, 224));
      SetMaterial(palette, kSimVoxelMaterial_Trim, Argb(150, 160, 157));
      SetMaterial(palette, kSimVoxelMaterial_Dark, Argb(41, 43, 40));
      SetMaterial(palette, kSimVoxelMaterial_Gold, Argb(235, 181, 43));
      SetMaterial(palette, kSimVoxelMaterial_Glass, Argb(53, 82, 101));
      break;
    case kSimBackgroundVoxel_Windmill:
      SetMaterial(palette, kSimVoxelMaterial_Wall, Argb(196, 154, 82));
      SetMaterial(palette, kSimVoxelMaterial_WallLight,
                  Argb(225, 190, 116));
      SetMaterial(palette, kSimVoxelMaterial_Roof, Argb(88, 47, 118));
      SetMaterial(palette, kSimVoxelMaterial_RoofLight,
                  Argb(127, 74, 151));
      break;
    case kSimBackgroundVoxel_Factory:
      SetMaterial(palette, kSimVoxelMaterial_Wall, Argb(177, 144, 73));
      SetMaterial(palette, kSimVoxelMaterial_WallLight,
                  Argb(220, 190, 112));
      SetMaterial(palette, kSimVoxelMaterial_Roof, Argb(80, 42, 105));
      SetMaterial(palette, kSimVoxelMaterial_RoofLight,
                  Argb(116, 65, 139));
      SetMaterial(palette, kSimVoxelMaterial_Trim, Argb(111, 81, 115));
      break;
    case kSimBackgroundVoxel_Tree: {
      int seed = object->cell_x * 7 + object->cell_y * 5 + object->group;
      int variation = (seed % 3 - 1) * 7;
      static const SimBackgroundVoxelMaterial leaves[] = {
        kSimVoxelMaterial_Leaves,
        kSimVoxelMaterial_LeavesLight,
        kSimVoxelMaterial_LeavesDark,
      };
      for (int material = 0; material < 3; material++) {
        uint32_t base = palette->material[leaves[material]][2];
        SetMaterial(palette, leaves[material],
                    VaryColour(base, variation));
      }
      break;
    }
  }
  if (object->kind != kSimBackgroundVoxel_Tree &&
      object->record_slot != 0xFF) {
    int variation = ((int)object->record_slot % 3 - 1) * 4;
    static const SimBackgroundVoxelMaterial varied[] = {
      kSimVoxelMaterial_Wall,
      kSimVoxelMaterial_Roof,
    };
    for (int material = 0; material < 2; material++) {
      uint32_t base = palette->material[varied[material]][2];
      SetMaterial(palette, varied[material],
                  VaryColour(base, variation));
    }
  }
  ApplyBiomePalette(palette, biome);
}

uint32_t SimBackgroundVoxelPalette_Base(
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelMaterial material) {
  if (!palette || material < 0 || material >= kSimVoxelMaterial_Count)
    return 0xFFFF00FFu;
  return palette->material[material][2];
}

uint32_t SimBackgroundVoxelPalette_Ramp(
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelMaterial material,
    uint8_t brightness) {
  if (!palette || material < 0 || material >= kSimVoxelMaterial_Count)
    return 0xFFFF00FFu;
  int level = brightness < 128 ? 0 : brightness < 178 ? 1
      : brightness < 224 ? 2 : 3;
  return palette->material[material][level];
}
