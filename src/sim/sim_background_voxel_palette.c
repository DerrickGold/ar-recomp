#include "sim_background_voxel_palette.h"

#include <string.h>

#include "sim_background_voxel_region.h"

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

static void SetCanvasHouse(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Wall,
          Argb(90, 74, 32), Argb(148, 115, 57),
          Argb(205, 164, 90), Argb(230, 205, 139));
  SetRamp(palette, kSimVoxelMaterial_WallLight,
          Argb(148, 115, 57), Argb(180, 139, 74),
          Argb(222, 180, 90), Argb(246, 213, 148));
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(74, 57, 24), Argb(115, 82, 32),
          Argb(164, 123, 57), Argb(205, 164, 90));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(115, 82, 32), Argb(164, 123, 57),
          Argb(205, 164, 90), Argb(230, 205, 139));
}

static void SetYurtHouse(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Wall,
          Argb(82, 49, 16), Argb(123, 74, 24),
          Argb(164, 106, 49), Argb(205, 148, 82));
  SetRamp(palette, kSimVoxelMaterial_WallLight,
          Argb(123, 74, 24), Argb(164, 106, 49),
          Argb(205, 148, 82), Argb(230, 180, 106));
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(65, 32, 8), Argb(98, 49, 8),
          Argb(139, 74, 16), Argb(180, 106, 32));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(98, 49, 8), Argb(139, 74, 16),
          Argb(180, 106, 32), Argb(213, 148, 65));
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(49, 32, 8), Argb(82, 49, 8),
          Argb(115, 65, 16), Argb(164, 98, 32));
}

static void SetWhiteCanvasHouse(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Wall,
          Argb(98, 98, 90), Argb(156, 156, 139),
          Argb(213, 213, 189), Argb(246, 238, 213));
  SetRamp(palette, kSimVoxelMaterial_WallLight,
          Argb(148, 148, 131), Argb(197, 197, 180),
          Argb(238, 230, 205), Argb(255, 255, 238));
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(90, 98, 90), Argb(139, 148, 139),
          Argb(205, 205, 189), Argb(246, 246, 230));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(139, 148, 139), Argb(189, 197, 189),
          Argb(230, 230, 213), Argb(255, 255, 246));
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(74, 49, 16), Argb(106, 74, 24),
          Argb(148, 106, 49), Argb(197, 156, 82));
}

static void SetAdobeHouse(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Wall,
          Argb(106, 82, 49), Argb(156, 123, 74),
          Argb(205, 172, 115), Argb(238, 213, 164));
  SetRamp(palette, kSimVoxelMaterial_WallLight,
          Argb(148, 115, 65), Argb(197, 156, 98),
          Argb(230, 197, 139), Argb(255, 230, 180));
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(98, 57, 16), Argb(139, 82, 24),
          Argb(180, 115, 49), Argb(222, 164, 82));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(139, 82, 24), Argb(180, 115, 49),
          Argb(222, 164, 82), Argb(246, 197, 115));
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(74, 57, 32), Argb(115, 90, 49),
          Argb(164, 131, 82), Argb(213, 180, 123));
}

static void SetTimberHouse(SimBackgroundVoxelPalette *palette) {
  SetEarthenWalls(palette);
  SetBrownRoofs(palette);
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(74, 41, 8), Argb(115, 65, 8),
          Argb(148, 90, 16), Argb(180, 123, 49));
}

static void SetStoneHouse(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Wall,
          Argb(65, 74, 65), Argb(106, 106, 98),
          Argb(164, 164, 148), Argb(213, 213, 197));
  SetRamp(palette, kSimVoxelMaterial_WallLight,
          Argb(106, 106, 98), Argb(164, 164, 148),
          Argb(205, 205, 189), Argb(238, 238, 222));
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(32, 57, 82), Argb(65, 82, 98),
          Argb(106, 115, 115), Argb(164, 172, 164));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(65, 82, 98), Argb(106, 115, 115),
          Argb(164, 172, 164), Argb(205, 213, 205));
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(32, 32, 24), Argb(65, 74, 65),
          Argb(106, 106, 98), Argb(180, 180, 164));
}

static void SetBloodpoolHouse(SimBackgroundVoxelPalette *palette) {
  SetEarthenWalls(palette);
  SetPurpleRoofs(palette);
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(32, 32, 24), Argb(74, 32, 90),
          Argb(115, 57, 148), Argb(180, 139, 197));
}

static void SetAitosHouse(SimBackgroundVoxelPalette *palette) {
  SetStoneHouse(palette);
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(90, 41, 0), Argb(131, 65, 0),
          Argb(180, 90, 16), Argb(222, 139, 49));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(131, 65, 0), Argb(180, 90, 16),
          Argb(222, 139, 49), Argb(246, 180, 82));
}

static void SetMarahnaStiltHouse(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Wall,
          Argb(74, 57, 24), Argb(106, 82, 32),
          Argb(148, 115, 49), Argb(197, 156, 82));
  SetRamp(palette, kSimVoxelMaterial_WallLight,
          Argb(106, 82, 32), Argb(148, 115, 49),
          Argb(197, 156, 82), Argb(222, 197, 131));
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(57, 49, 16), Argb(90, 74, 24),
          Argb(131, 106, 49), Argb(180, 148, 74));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(90, 74, 24), Argb(131, 106, 49),
          Argb(180, 148, 74), Argb(213, 180, 106));
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(49, 32, 8), Argb(74, 49, 8),
          Argb(106, 65, 16), Argb(148, 98, 32));
}

static void SetMarahnaLogCabin(SimBackgroundVoxelPalette *palette) {
  SetRamp(palette, kSimVoxelMaterial_Wall,
          Argb(57, 32, 8), Argb(90, 49, 8),
          Argb(131, 74, 16), Argb(180, 115, 49));
  SetRamp(palette, kSimVoxelMaterial_WallLight,
          Argb(90, 49, 8), Argb(131, 74, 16),
          Argb(180, 115, 49), Argb(213, 156, 82));
  SetRamp(palette, kSimVoxelMaterial_Roof,
          Argb(49, 41, 8), Argb(74, 57, 16),
          Argb(106, 82, 24), Argb(148, 115, 49));
  SetRamp(palette, kSimVoxelMaterial_RoofLight,
          Argb(74, 57, 16), Argb(106, 82, 24),
          Argb(148, 115, 49), Argb(197, 156, 82));
  SetRamp(palette, kSimVoxelMaterial_Trim,
          Argb(32, 24, 8), Argb(65, 41, 8),
          Argb(98, 57, 16), Argb(148, 90, 32));
  SetRamp(palette, kSimVoxelMaterial_Wood,
          Argb(65, 32, 8), Argb(98, 49, 8),
          Argb(148, 82, 16), Argb(197, 123, 49));
}

static void ApplyRegionalHousePalette(
    SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelPaletteStyle style) {
  switch (style) {
    case kSimBackgroundPaletteStyle_Canvas:
      SetCanvasHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_Timber:
      SetTimberHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_Fillmore:
      SetEarthenWalls(palette);
      SetBrownRoofs(palette);
      break;
    case kSimBackgroundPaletteStyle_Bloodpool:
      SetBloodpoolHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_Yurt:
      SetYurtHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_WhiteCanvas:
      SetWhiteCanvasHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_Adobe:
      SetAdobeHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_Stone:
      SetStoneHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_Aitos:
      SetAitosHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_MarahnaStilt:
      SetMarahnaStiltHouse(palette);
      break;
    case kSimBackgroundPaletteStyle_MarahnaLogCabin:
      SetMarahnaLogCabin(palette);
      break;
    case kSimBackgroundPaletteStyle_Common:
      break;
  }
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
                              const SimBackgroundVoxelObject *object,
                              SimBackgroundVoxelBiome biome) {
  /* Five towns share the same structure and foliage CGRAM ramps. Northwall
   * changes only the forest family to its subdued snow-town greens. Landmarks
   * are sampled from their own art and must keep it. */
  if (biome != kSimBackgroundVoxelBiome_Snow ||
      object->kind == kSimBackgroundVoxel_StoryTree)
    return;
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
      ApplyRegionalHousePalette(
          palette, SimBackgroundVoxelRegion_PaletteStyle(object));
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
    case kSimBackgroundVoxel_Tree:
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_BroadTree:
    case kSimBackgroundVoxel_Shrub: {
      /* All four families are drawn from one CGRAM foliage ramp; which part of
       * it they occupy is what separates them on screen. Measured from the
       * source art: the evergreen ($0B) is 64% near-black, the broad canopy
       * ($0E) centres on mid green, and the bush ($01) centres on bright
       * green. Sharing one ramp would make a lone bush read as one more dark
       * evergreen. */
      if (object->kind == kSimBackgroundVoxel_Shrub) {
        SetRamp(palette, kSimVoxelMaterial_Leaves,
                Argb(0, 57, 0), Argb(16, 106, 0),
                Argb(32, 148, 0), Argb(57, 189, 0));
        SetRamp(palette, kSimVoxelMaterial_LeavesLight,
                Argb(16, 106, 0), Argb(32, 148, 0),
                Argb(57, 189, 0), Argb(57, 189, 0));
        SetRamp(palette, kSimVoxelMaterial_LeavesDark,
                Argb(0, 32, 0), Argb(0, 57, 0),
                Argb(16, 106, 0), Argb(32, 148, 0));
      } else if (object->kind == kSimBackgroundVoxel_BroadTree) {
        SetRamp(palette, kSimVoxelMaterial_Leaves,
                Argb(0, 32, 0), Argb(0, 57, 0),
                Argb(16, 106, 0), Argb(32, 148, 0));
        SetRamp(palette, kSimVoxelMaterial_LeavesLight,
                Argb(0, 57, 0), Argb(16, 106, 0),
                Argb(32, 148, 0), Argb(32, 148, 0));
        SetRamp(palette, kSimVoxelMaterial_LeavesDark,
                Argb(0, 32, 0), Argb(0, 32, 0),
                Argb(0, 57, 0), Argb(16, 106, 0));
        /* The mangrove's exposed trunk and roots are a warm grey-brown, not
         * the evergreen's red-brown. */
        SetRamp(palette, kSimVoxelMaterial_Trunk,
                Argb(41, 32, 16), Argb(74, 57, 24),
                Argb(106, 90, 32), Argb(131, 115, 41));
      }
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
    case kSimBackgroundVoxel_StoryTree:
      /* Sampled from the $EB plot: pale blue-white snow over a small olive
       * trunk, with the shaded underside a deeper slate blue. */
      SetRamp(palette, kSimVoxelMaterial_Trunk,
              Argb(74, 57, 24), Argb(106, 82, 32),
              Argb(131, 115, 32), Argb(164, 148, 74));
      SetRamp(palette, kSimVoxelMaterial_Wood,
              Argb(57, 41, 16), Argb(90, 74, 24),
              Argb(115, 98, 32), Argb(148, 131, 65));
      /* The crown's three bands have to stay apart after the face-brightness
       * ramp, which lands most faces at the light end. Snow reads white, the
       * shaded middle stays a clear blue-grey and the underside is the deep
       * blue the source art uses; giving the middle band near-white steps
       * flattened the whole canopy into one pale blob. */
      SetRamp(palette, kSimVoxelMaterial_Snow,
              Argb(164, 180, 205), Argb(197, 213, 230),
              Argb(213, 222, 230), Argb(238, 246, 255));
      SetRamp(palette, kSimVoxelMaterial_LeavesLight,
              Argb(90, 131, 172), Argb(115, 156, 189),
              Argb(139, 172, 197), Argb(164, 180, 205));
      SetRamp(palette, kSimVoxelMaterial_LeavesDark,
              Argb(49, 82, 148), Argb(74, 115, 180),
              Argb(90, 131, 172), Argb(115, 156, 189));
      break;
    case kSimBackgroundVoxel_BloodpoolCastle:
      /* Sampled from the $EC plot: the castle is pale stone with tan-gold
       * spire caps and dome. The earlier purple keep matched the town's house
       * roofs, not its own art. */
      SetRamp(palette, kSimVoxelMaterial_Wall,
              Argb(32, 32, 24), Argb(65, 74, 57),
              Argb(106, 106, 98), Argb(139, 148, 131));
      SetRamp(palette, kSimVoxelMaterial_WallLight,
              Argb(65, 74, 57), Argb(106, 106, 98),
              Argb(180, 180, 172), Argb(213, 222, 205));
      SetRamp(palette, kSimVoxelMaterial_Roof,
              Argb(74, 65, 32), Argb(115, 106, 57),
              Argb(164, 148, 82), Argb(189, 172, 98));
      SetRamp(palette, kSimVoxelMaterial_RoofLight,
              Argb(115, 106, 57), Argb(164, 148, 82),
              Argb(189, 172, 98), Argb(205, 180, 106));
      SetRamp(palette, kSimVoxelMaterial_Trim,
              Argb(32, 32, 24), Argb(65, 74, 57),
              Argb(139, 148, 131), Argb(180, 180, 172));
      SetRamp(palette, kSimVoxelMaterial_Gold,
              Argb(115, 106, 57), Argb(131, 82, 0),
              Argb(164, 98, 0), Argb(205, 180, 106));
      break;
    case kSimBackgroundVoxel_Pyramid:
      /* Sampled from the $EE plot: four sandstone steps from the shaded east
       * face to the sunlit casing. */
      SetRamp(palette, kSimVoxelMaterial_Wall,
              Argb(74, 57, 24), Argb(106, 90, 32),
              Argb(148, 115, 41), Argb(189, 164, 98));
      SetRamp(palette, kSimVoxelMaterial_WallLight,
              Argb(106, 90, 32), Argb(148, 115, 41),
              Argb(189, 164, 98), Argb(205, 180, 106));
      SetRamp(palette, kSimVoxelMaterial_Trim,
              Argb(74, 57, 24), Argb(106, 90, 32),
              Argb(148, 115, 41), Argb(189, 164, 98));
      SetRamp(palette, kSimVoxelMaterial_Dark,
              Argb(41, 32, 16), Argb(57, 41, 16),
              Argb(74, 57, 24), Argb(106, 90, 32));
      break;
    case kSimBackgroundVoxel_MarahnaTemple:
      SetRamp(palette, kSimVoxelMaterial_Wall,
              Argb(65, 65, 49), Argb(98, 106, 74),
              Argb(148, 156, 106), Argb(205, 205, 156));
      SetRamp(palette, kSimVoxelMaterial_WallLight,
              Argb(98, 106, 74), Argb(148, 156, 106),
              Argb(197, 205, 156), Argb(238, 238, 197));
      SetRamp(palette, kSimVoxelMaterial_Roof,
              Argb(49, 49, 24), Argb(82, 82, 32),
              Argb(123, 115, 49), Argb(172, 156, 74));
      SetRamp(palette, kSimVoxelMaterial_RoofLight,
              Argb(74, 74, 32), Argb(115, 106, 49),
              Argb(164, 148, 74), Argb(213, 197, 115));
      SetRamp(palette, kSimVoxelMaterial_Paving,
              Argb(57, 65, 49), Argb(90, 98, 65),
              Argb(131, 148, 90), Argb(189, 197, 139));
      SetRamp(palette, kSimVoxelMaterial_Gold,
              Argb(106, 74, 8), Argb(164, 115, 16),
              Argb(222, 172, 49), Argb(255, 230, 115));
      break;
  }
  int vary_by_record = object->kind == kSimBackgroundVoxel_House ||
      object->kind == kSimBackgroundVoxel_Windmill ||
      object->kind == kSimBackgroundVoxel_Factory;
  if (vary_by_record &&
      object->record_slot != 0xFF) {
    int variation = ((int)object->record_slot % 3 - 1) * 2;
    static const SimBackgroundVoxelMaterial varied[] = {
      kSimVoxelMaterial_Wall,
      kSimVoxelMaterial_Roof,
    };
    for (int material = 0; material < 2; material++)
      VaryMaterial(palette, varied[material], variation);
  }
  ApplyBiomePalette(palette, object, biome);
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
