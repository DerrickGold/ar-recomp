#ifndef SIM_TOWN_TERRAIN_H
#define SIM_TOWN_TERRAIN_H

#include <stdint.h>

#include "constants.h"
#include "sim_world_map.h"

/* Audited, deterministic terrain derived from the six stock SIM maps.  Towns
 * use the game's raw 1..6 numbering.  Heights are relative to sea datum; one
 * elevation unit is one 16-pixel town cell. */
enum {
  kSimTownTerrainTownCount = kSimTownCount,
  kSimTownTerrainCells = kSimTownCells,
  kSimTownTerrainCellPixels = kSimTownCellPixels,
  kSimTownTerrainCornerCount = 4,
  kSimTownTerrainQ8Scale = 256,
  kSimTownTerrainLandscapeHeightMinimumPct = 0,
  kSimTownTerrainLandscapeHeightDefaultPct = kPercentScale,
  kSimTownTerrainLandscapeHeightMaximumPct = 150,
  kSimTownTerrainLandscapeHeightStepPct = 5,
};

/* Converts audited elevation units to the source-pixel height consumed by
 * both the SDL terrain mesh and the GPU depth/model renderer. Keeping this
 * scale at the terrain boundary prevents one path from silently using a
 * different percentage convention. */
static inline float SimTownTerrain_ScaledHeightPixels(
    float height_units, float landscape_height_pct) {
  return height_units * (float)kSimTownTerrainCellPixels *
      landscape_height_pct / (float)kPercentScale;
}

typedef enum SimTownTerrainCorner {
  kSimTownTerrainCornerNW = 0,
  kSimTownTerrainCornerNE = 1,
  kSimTownTerrainCornerSE = 2,
  kSimTownTerrainCornerSW = 3,
} SimTownTerrainCorner;

typedef enum SimTownTerrainEdge {
  kSimTownTerrainEdgeNorth = 1 << 0,
  kSimTownTerrainEdgeEast = 1 << 1,
  kSimTownTerrainEdgeSouth = 1 << 2,
  kSimTownTerrainEdgeWest = 1 << 3,
} SimTownTerrainEdge;

/* Authored vertical-face subtypes baked by the terrain classifier. */
typedef enum SimTownTerrainFaceKind {
  kSimTownTerrainFace_None,
  kSimTownTerrainFace_Cliff,
  kSimTownTerrainFace_Cave,
} SimTownTerrainFaceKind;

/* A cell-owned sample.  slope_x/y are elevation units gained over one town
 * cell.  hard_edges identifies borders whose two cells intentionally do not
 * share a height, allowing callers to avoid averaging through a cliff. */
typedef struct SimTownTerrainSample {
  float height_units;
  float slope_x;
  float slope_y;
  float normal_x;
  float normal_y;
  float normal_z;
  uint8_t hard_edges;
} SimTownTerrainSample;

/* Invalid towns, cells, or corners return zero (the datum), which keeps this
 * optional presentation layer fail-closed. */
int16_t SimTownTerrain_CornerQ8(uint8_t town, int cell_x, int cell_y,
                                int corner);
float SimTownTerrain_CornerUnits(uint8_t town, int cell_x, int cell_y,
                                 int corner);
float SimTownTerrain_CellUnits(uint8_t town, int cell_x, int cell_y);

/* Returns the baked classifier-owned hard-edge mask for a cell.  Internal
 * edges are symmetric with their neighbour; map borders are hard. */
uint8_t SimTownTerrain_HardEdges(uint8_t town, int cell_x, int cell_y);

/* Returns the exact authored face subtype, or None for invalid/ordinary
 * cells. Prefer this when a caller needs more than a yes/no face test. */
SimTownTerrainFaceKind SimTownTerrain_FaceKind(
    uint8_t town, int cell_x, int cell_y);

/* True when the source terrain classifier says this cell is authored cliff
 * face art.  Hard-boundary skirts borrow this side's material, never grass. */
int SimTownTerrain_IsFaceCell(uint8_t town, int cell_x, int cell_y);

/* The cave mouth is face topology, but its centre is the dark aperture rather
 * than reusable rock. Renderers use this subtype to keep that texel from
 * becoming a horizontal black skirt above or below the entrance. */
int SimTownTerrain_IsCaveCell(uint8_t town, int cell_x, int cell_y);

/* Highest audited corner in a town.  Flying actors use this as a stable
 * world-space flight datum instead of climbing and descending with every
 * local hill beneath them. */
float SimTownTerrain_MaximumUnits(uint8_t town);

/* Samples one explicitly selected cell.  u/v are clamped to [0, 1].  This is
 * the preferred API for semantic anchors such as the bank side of a bridge or
 * the landward side of a cliff.  Returns zero for invalid inputs. */
int SimTownTerrain_SampleCell(uint8_t town, int cell_x, int cell_y,
                              float u, float v,
                              SimTownTerrainSample *out_sample);

/* Resolves one horizontal engineered datum across two semantic anchors. The
 * higher endpoint wins, keeping a bridge/platform level without burying it in
 * the higher bank. Returns false and writes zero when either anchor is
 * invalid. */
int SimTownTerrain_LevelPairUnits(
    uint8_t town,
    int cell_a_x, int cell_a_y, float a_u, float a_v,
    int cell_b_x, int cell_b_y, float b_u, float b_v,
    float *out_height_units);

/* Maximum audited height touched by an inclusive town-pixel rectangle. Each
 * overlapped cell keeps its own side of a hard edge. A level rigid footprint
 * can use this to clear every point beneath it instead of checking only its
 * centre or two midpoint anchors. */
int SimTownTerrain_MaximumUnitsInRect(
    uint8_t town, float pixel_x0, float pixel_y0,
    float pixel_x1, float pixel_y1, float *out_height_units);

/* Clips one hard boundary to the interval on which the current cell is above
 * its neighbour.  A cliff edge can change height ownership between its two
 * endpoints; emitting the uncut four-corner skirt in that case makes a
 * self-crossing bow-tie whose triangles escape across the terrain cap.
 * Returns false when no visible interval is higher than epsilon. */
int SimTownTerrain_ClipHigherEdge(
    float current_0, float current_1,
    float neighbour_0, float neighbour_1,
    float epsilon, float *out_t0, float *out_t1);

/* Renderer policy wrapper for the visible/depth cliff boundary. Keeping its
 * tolerance here prevents the color and D32 meshes from drifting apart. */
int SimTownTerrain_ClipVisibleHigherEdge(
    float current_0, float current_1,
    float neighbour_0, float neighbour_1,
    float *out_t0, float *out_t1);

/* Selects the owning cell for a town-pixel point and returns its complete
 * height/slope/normal sample.  Inputs are clamped to the finite 512x512 map. */
int SimTownTerrain_SamplePoint(uint8_t town, float town_pixel_x,
                               float town_pixel_y,
                               SimTownTerrainSample *out_sample);

/* Bilinear sampling within the cell owning the supplied town-pixel position.
 * Inputs are clamped to the finite 512x512 town.  Hard cliff edges deliberately
 * retain the owning cell's side instead of being averaged across the wall. */
float SimTownTerrain_HeightUnitsAt(uint8_t town, float town_pixel_x,
                                   float town_pixel_y);

#endif  /* SIM_TOWN_TERRAIN_H */
