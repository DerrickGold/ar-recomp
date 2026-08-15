#include "sim_background_voxel_palette.h"

#include <string.h>

static uint8_t ClampChannel(int value) {
  return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static uint32_t Argb(uint8_t red, uint8_t green, uint8_t blue) {
  return 0xFF000000u | (uint32_t)red << 16 |
      (uint32_t)green << 8 | blue;
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

static void SetRamp(SimBackgroundVoxelPalette *palette,
                    SimBackgroundVoxelMaterial material,
                    uint32_t shadow, uint32_t mid,
                    uint32_t base, uint32_t highlight) {
  palette->material[material][0] = shadow;
  palette->material[material][1] = mid;
  palette->material[material][2] = base;
  palette->material[material][3] = highlight;
}

static void VaryMaterial(SimBackgroundVoxelPalette *palette,
                         SimBackgroundVoxelMaterial material,
                         int variation) {
  for (int level = 0; level < kSimBackgroundVoxelPaletteRampLevels; level++)
    palette->material[material][level] =
        VaryColour(palette->material[material][level], variation);
}

static void SetEarthenWalls(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Wall,
          Argb(90, 74, 32), Argb(131, 106, 49),
          Argb(180, 139, 74), Argb(222, 180, 90));
  SetRamp(palette, kSimVoxelMaterial_WallLight,
          Argb(131, 106, 49), Argb(180, 139, 74),
          Argb(222, 180, 90), Argb(222, 180, 90));
}

static void SetPurpleRoofs(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(0, 0, 0), Argb(74, 32, 90),
          Argb(74, 32, 90), Argb(115, 57, 148));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(74, 32, 90), Argb(74, 32, 90),
          Argb(115, 57, 148), Argb(115, 57, 148));
}

static void SetBrownRoofs(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(90, 41, 0), Argb(90, 41, 0),
          Argb(131, 65, 0), Argb(164, 82, 0));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(90, 41, 0), Argb(131, 65, 0),
          Argb(164, 82, 0), Argb(164, 82, 0));
}

static void SetCommonPalette(SimBackgroundVoxelPalette *palette) {
  /* These ramps come directly from the SIM background palettes. Keeping the
   * authored SNES steps makes replacement geometry sit beside untouched art
   * without giving up directional lighting or vertex AO. */
  SetEarthenWalls(palette);
  SetPurpleRoofs(palette);
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(90, 74, 32), Argb(131, 106, 49),
          Argb(180, 139, 74), Argb(222, 180, 90));
  SetRamp(palette, kSimVoxelMaterial_Dark,
          Argb(0, 0, 0), Argb(0, 0, 0),
          Argb(32, 32, 24), Argb(65, 74, 57));
  SetRamp(palette, kSimVoxelMaterial_Wood,
          Argb(90, 41, 0), Argb(90, 41, 0),
          Argb(131, 65, 0), Argb(164, 82, 0));
  SetRamp(palette, kSimVoxelMaterial_Metal,
          Argb(32, 57, 82), Argb(74, 115, 164),
          Argb(180, 205, 222), Argb(230, 230, 230));
  SetRamp(palette, kSimVoxelMaterial_Blade,
          Argb(131, 148, 164), Argb(180, 205, 222),
          Argb(230, 230, 230), Argb(230, 230, 230));
  SetRamp(palette, kSimVoxelMaterial_Trunk,
          Argb(74, 57, 24), Argb(90, 49, 8),
          Argb(115, 74, 16), Argb(148, 90, 16));
  SetRamp(palette, kSimVoxelMaterial_Leaves,
          Argb(0, 32, 0), Argb(0, 32, 0),
          Argb(0, 57, 0), Argb(16, 106, 0));
  SetRamp(palette, kSimVoxelMaterial_LeavesLight,
          Argb(0, 57, 0), Argb(0, 57, 0),
          Argb(16, 106, 0), Argb(16, 106, 0));
  SetRamp(palette, kSimVoxelMaterial_LeavesDark,
          Argb(0, 32, 0), Argb(0, 32, 0),
          Argb(0, 57, 0), Argb(16, 106, 0));
  SetRamp(palette, kSimVoxelMaterial_Paving,
          Argb(57, 74, 0), Argb(90, 74, 32),
          Argb(131, 106, 49), Argb(180, 139, 74));
  SetRamp(palette, kSimVoxelMaterial_Gold,
          Argb(115, 106, 57), Argb(164, 148, 82),
          Argb(205, 180, 106), Argb(230, 180, 0));
  SetRamp(palette, kSimVoxelMaterial_Glass,
          Argb(0, 0, 0), Argb(32, 57, 82),
          Argb(74, 115, 164), Argb(131, 148, 164));
  SetRamp(palette, kSimVoxelMaterial_Snow,
          Argb(131, 148, 164), Argb(180, 205, 222),
          Argb(213, 230, 246), Argb(255, 255, 255));
  for (int level = 0; level < kSimBackgroundVoxelPaletteRampLevels; level++)
    palette->material[kSimVoxelMaterial_Contact][level] = 0x380B1014u;
}

static void ApplyBiomePalette(SimBackgroundVoxelPalette *palette,
                              SimBackgroundVoxelBiome biome) {
  /* Five towns share the same structure and foliage CGRAM ramps. Northwall
   * changes only the forest family to its subdued snow-town greens. */
  if (biome != kSimBackgroundVoxelBiome_Snow) return;
  SetRamp(palette, kSimVoxelMaterial_Leaves,
          Argb(32, 32, 32), Argb(32, 32, 32),
          Argb(65, 74, 65), Argb(115, 131, 115));
  SetRamp(palette, kSimVoxelMaterial_LeavesLight,
          Argb(65, 74, 65), Argb(65, 74, 65),
          Argb(115, 131, 115), Argb(115, 131, 115));
  SetRamp(palette, kSimVoxelMaterial_LeavesDark,
          Argb(32, 32, 32), Argb(32, 32, 32),
          Argb(65, 74, 65), Argb(115, 131, 115));
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
      SetBrownRoofs(palette);
      break;
    case kSimBackgroundVoxel_Cathedral:
      SetRamp(palette, kSimVoxelMaterial_Wall,
              Argb(32, 32, 24), Argb(65, 74, 57),
              Argb(106, 106, 98), Argb(139, 148, 131));
      SetRamp(palette, kSimVoxelMaterial_WallLight,
              Argb(65, 74, 57), Argb(106, 106, 98),
              Argb(180, 180, 172), Argb(213, 222, 205));
      SetRamp(palette, kSimVoxelMaterial_Roof,
              Argb(65, 74, 57), Argb(106, 106, 98),
              Argb(139, 148, 131), Argb(180, 180, 172));
      SetRamp(palette, kSimVoxelMaterial_RoofLight,
              Argb(106, 106, 98), Argb(139, 148, 131),
              Argb(180, 180, 172), Argb(213, 222, 205));
      SetRamp(palette, kSimVoxelMaterial_Trim,
              Argb(32, 32, 24), Argb(65, 74, 57),
              Argb(139, 148, 131), Argb(180, 180, 172));
      SetRamp(palette, kSimVoxelMaterial_Dark,
              Argb(0, 0, 0), Argb(32, 32, 24),
              Argb(32, 32, 24), Argb(65, 74, 57));
      SetRamp(palette, kSimVoxelMaterial_Gold,
              Argb(115, 106, 57), Argb(131, 82, 0),
              Argb(164, 98, 0), Argb(205, 180, 106));
      break;
    case kSimBackgroundVoxel_Windmill:
      SetEarthenWalls(palette);
      SetPurpleRoofs(palette);
      break;
    case kSimBackgroundVoxel_Factory:
      SetEarthenWalls(palette);
      SetPurpleRoofs(palette);
      SetRamp(palette, kSimVoxelMaterial_Trim,
              Argb(0, 0, 0), Argb(74, 32, 90),
              Argb(115, 57, 148), Argb(115, 57, 148));
      break;
    case kSimBackgroundVoxel_Tree: {
      int seed = object->cell_x * 7 + object->cell_y * 5 + object->group;
      int variation = (seed % 3 - 1) * 3;
      static const SimBackgroundVoxelMaterial leaves[] = {
        kSimVoxelMaterial_Leaves,
        kSimVoxelMaterial_LeavesLight,
        kSimVoxelMaterial_LeavesDark,
      };
      for (int material = 0; material < 3; material++)
        VaryMaterial(palette, leaves[material], variation);
      break;
    }
  }
  if (object->kind != kSimBackgroundVoxel_Tree &&
      object->record_slot != 0xFF) {
    int variation = ((int)object->record_slot % 3 - 1) * 2;
    static const SimBackgroundVoxelMaterial varied[] = {
      kSimVoxelMaterial_Wall,
      kSimVoxelMaterial_Roof,
    };
    for (int material = 0; material < 2; material++)
      VaryMaterial(palette, varied[material], variation);
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
