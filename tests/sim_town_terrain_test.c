#include <math.h>
#include <stdio.h>

#include "sim_town_terrain.h"

static int s_failures;
static const float kVisibleEdgeTestEpsilonUnits = 0.004f;
#define CHECK(expression)                                                  \
  do {                                                                     \
    if (!(expression)) {                                                   \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,     \
              #expression);                                                \
      s_failures++;                                                        \
    }                                                                      \
  } while (0)

static void TestBoundsAndSampling(void) {
  CHECK(SimTownTerrain_ScaledHeightPixels(2.0f, 0.0f) == 0.0f);
  CHECK(SimTownTerrain_ScaledHeightPixels(
      2.0f, kSimTownTerrainLandscapeHeightDefaultPct) == 32.0f);
  CHECK(fabsf(SimTownTerrain_ScaledHeightPixels(2.0f, 40.0f) - 12.8f) <
        0.0001f);
  CHECK(SimTownTerrain_CornerQ8(0, 0, 0, 0) == 0);
  CHECK(SimTownTerrain_CornerQ8(7, 0, 0, 0) == 0);
  CHECK(SimTownTerrain_CornerQ8(1, -1, 0, 0) == 0);
  CHECK(SimTownTerrain_CornerQ8(1, 0, 32, 0) == 0);
  CHECK(SimTownTerrain_CornerQ8(1, 0, 0, 4) == 0);
  CHECK(SimTownTerrain_HeightUnitsAt(0, 100.0f, 100.0f) == 0.0f);
  CHECK(SimTownTerrain_HeightUnitsAt(1, NAN, 0.0f) == 0.0f);

  SimTownTerrainSample invalid = {.height_units = 123.0f};
  CHECK(!SimTownTerrain_SampleCell(0, 0, 0, 0.5f, 0.5f, &invalid));
  CHECK(invalid.height_units == 0.0f);
  CHECK(!SimTownTerrain_SamplePoint(1, 0.0f, 0.0f, NULL));

  for (uint8_t town = 1; town <= kSimTownTerrainTownCount; town++) {
    for (int y = 0; y < kSimTownTerrainCells; y++) {
      for (int x = 0; x < kSimTownTerrainCells; x++) {
        float average = 0.0f;
        for (int corner = 0; corner < 4; corner++)
          average += SimTownTerrain_CornerUnits(town, x, y, corner) * 0.25f;
        CHECK(fabsf(average - SimTownTerrain_CellUnits(town, x, y)) < 0.0001f);
        float sampled = SimTownTerrain_HeightUnitsAt(
            town, x * 16.0f + 8.0f, y * 16.0f + 8.0f);
        CHECK(fabsf(average - sampled) < 0.0001f);

        SimTownTerrainSample detail;
        CHECK(SimTownTerrain_SampleCell(town, x, y, 0.5f, 0.5f,
                                        &detail));
        CHECK(fabsf(average - detail.height_units) < 0.0001f);
        /* Height-only runtime queries use a cheaper interpolation path than
         * the slope/normal API. Keep them bit-close at non-symmetric points,
         * not merely at cell centres where several mistakes can cancel. */
        const float u = 0.137f + (x % 3) * 0.211f;
        const float v = 0.193f + (y % 3) * 0.173f;
        CHECK(SimTownTerrain_SampleCell(town, x, y, u, v, &detail));
        const float height_only = SimTownTerrain_HeightUnitsAt(
            town, (x + u) * kSimTownTerrainCellPixels,
            (y + v) * kSimTownTerrainCellPixels);
        CHECK(fabsf(detail.height_units - height_only) < 0.00001f);
        const float normal_length = sqrtf(
            detail.normal_x * detail.normal_x +
            detail.normal_y * detail.normal_y +
            detail.normal_z * detail.normal_z);
        CHECK(fabsf(normal_length - 1.0f) < 0.0001f);
        CHECK(detail.normal_z > 0.0f);
      }
    }
  }
}

static void TestExplicitCellOwnership(void) {
  CHECK(SimTownTerrain_HardEdges(0, 0, 0) == 0);
  CHECK(!SimTownTerrain_IsFaceCell(7, 0, 0));
  CHECK(SimTownTerrain_FaceKind(7, 0, 0) ==
        kSimTownTerrainFace_None);
  for (uint8_t town = 1; town <= kSimTownTerrainTownCount; town++) {
    for (int y = 0; y < kSimTownTerrainCells; y++) {
      for (int x = 0; x < kSimTownTerrainCells; x++) {
        SimTownTerrainSample nw, ne, se, sw;
        CHECK(SimTownTerrain_SampleCell(town, x, y, 0.0f, 0.0f, &nw));
        CHECK(SimTownTerrain_SampleCell(town, x, y, 1.0f, 0.0f, &ne));
        CHECK(SimTownTerrain_SampleCell(town, x, y, 1.0f, 1.0f, &se));
        CHECK(SimTownTerrain_SampleCell(town, x, y, 0.0f, 1.0f, &sw));
        CHECK(fabsf(nw.height_units - SimTownTerrain_CornerUnits(
            town, x, y, kSimTownTerrainCornerNW)) < 0.0001f);
        CHECK(fabsf(ne.height_units - SimTownTerrain_CornerUnits(
            town, x, y, kSimTownTerrainCornerNE)) < 0.0001f);
        CHECK(fabsf(se.height_units - SimTownTerrain_CornerUnits(
            town, x, y, kSimTownTerrainCornerSE)) < 0.0001f);
        CHECK(fabsf(sw.height_units - SimTownTerrain_CornerUnits(
            town, x, y, kSimTownTerrainCornerSW)) < 0.0001f);
        if (x == 0)
          CHECK(nw.hard_edges & kSimTownTerrainEdgeWest);
        if (y == 0)
          CHECK(nw.hard_edges & kSimTownTerrainEdgeNorth);
        if (x == kSimTownTerrainCells - 1)
          CHECK(nw.hard_edges & kSimTownTerrainEdgeEast);
        if (y == kSimTownTerrainCells - 1)
          CHECK(nw.hard_edges & kSimTownTerrainEdgeSouth);
      }
    }
  }
}

static void TestTopologyContract(void) {
  int marahna_faces = 0;
  int marahna_hard_edges = 0;
  for (uint8_t town = 1; town <= kSimTownTerrainTownCount; town++) {
    for (int y = 0; y < kSimTownTerrainCells; y++) {
      for (int x = 0; x < kSimTownTerrainCells; x++) {
        const uint8_t hard = SimTownTerrain_HardEdges(town, x, y);
        const SimTownTerrainFaceKind face_kind =
            SimTownTerrain_FaceKind(town, x, y);
        const int face = SimTownTerrain_IsFaceCell(town, x, y);
        CHECK(face_kind >= kSimTownTerrainFace_None);
        CHECK(face_kind <= kSimTownTerrainFace_Cave);
        CHECK(face == (face_kind != kSimTownTerrainFace_None));
        if (town == 5) {
          marahna_faces += face;
          marahna_hard_edges += !!(hard & kSimTownTerrainEdgeEast);
          marahna_hard_edges += !!(hard & kSimTownTerrainEdgeSouth);
        }
        if (x + 1 < kSimTownTerrainCells) {
          const uint8_t neighbour = SimTownTerrain_HardEdges(town, x + 1, y);
          const int split = (hard & kSimTownTerrainEdgeEast) != 0;
          CHECK(split == ((neighbour & kSimTownTerrainEdgeWest) != 0));
          if (split) {
            CHECK(face != SimTownTerrain_IsFaceCell(town, x + 1, y));
          } else {
            CHECK(SimTownTerrain_CornerQ8(
                town, x, y, kSimTownTerrainCornerNE) ==
                SimTownTerrain_CornerQ8(
                    town, x + 1, y, kSimTownTerrainCornerNW));
            CHECK(SimTownTerrain_CornerQ8(
                town, x, y, kSimTownTerrainCornerSE) ==
                SimTownTerrain_CornerQ8(
                    town, x + 1, y, kSimTownTerrainCornerSW));
          }
        }
        if (y + 1 < kSimTownTerrainCells) {
          const uint8_t neighbour = SimTownTerrain_HardEdges(town, x, y + 1);
          const int split = (hard & kSimTownTerrainEdgeSouth) != 0;
          CHECK(split == ((neighbour & kSimTownTerrainEdgeNorth) != 0));
          if (split) {
            CHECK(face != SimTownTerrain_IsFaceCell(town, x, y + 1));
          } else {
            CHECK(SimTownTerrain_CornerQ8(
                town, x, y, kSimTownTerrainCornerSW) ==
                SimTownTerrain_CornerQ8(
                    town, x, y + 1, kSimTownTerrainCornerNW));
            CHECK(SimTownTerrain_CornerQ8(
                town, x, y, kSimTownTerrainCornerSE) ==
                SimTownTerrain_CornerQ8(
                    town, x, y + 1, kSimTownTerrainCornerNE));
          }
        }
      }
    }
  }
  /* Marahna is the regression town: its grey plateau family must remain an
   * explicit face topology with hard flanks, not inferred numerical seams. */
  CHECK(marahna_faces > 20);
  CHECK(marahna_hard_edges > 10);

  /* Bloodpool has one authored $72 cave mouth. It remains part of the face
   * topology, but is separately identifiable so its black centre cannot be
   * stretched into the hard N/S skirts around the opening. */
  int cave_count = 0;
  for (uint8_t town = 1; town <= kSimTownTerrainTownCount; town++)
    for (int y = 0; y < kSimTownTerrainCells; y++)
      for (int x = 0; x < kSimTownTerrainCells; x++)
        if (SimTownTerrain_IsCaveCell(town, x, y)) {
          cave_count++;
          CHECK(town == 2 && x == 10 && y == 18);
          CHECK(SimTownTerrain_IsFaceCell(town, x, y));
          CHECK(SimTownTerrain_FaceKind(town, x, y) ==
                kSimTownTerrainFace_Cave);
        }
  CHECK(cave_count == 1);
}

static void TestTownRelief(void) {
  const float expected_min[] = {3.8f, 1.5f, 0.5f, 1.8f, 5.5f, 0.5f};
  const float expected_max[] = {4.5f, 2.2f, 0.9f, 2.2f, 7.0f, 0.9f};
  for (uint8_t town = 1; town <= kSimTownTerrainTownCount; town++) {
    float low = 1000.0f, high = -1000.0f;
    for (int y = 0; y < kSimTownTerrainCells; y++)
      for (int x = 0; x < kSimTownTerrainCells; x++) {
        float height = SimTownTerrain_CellUnits(town, x, y);
        if (height < low) low = height;
        if (height > high) high = height;
      }
    float relief = high - low;
    CHECK(relief >= expected_min[town - 1]);
    CHECK(relief <= expected_max[town - 1]);
  }
}

static void TestTownMaximum(void) {
  CHECK(SimTownTerrain_MaximumUnits(0) == 0.0f);
  CHECK(SimTownTerrain_MaximumUnits(7) == 0.0f);
  for (uint8_t town = 1; town <= kSimTownTerrainTownCount; town++) {
    float expected = 0.0f;
    for (int y = 0; y < kSimTownTerrainCells; y++)
      for (int x = 0; x < kSimTownTerrainCells; x++)
        for (int corner = 0; corner < kSimTownTerrainCornerCount; corner++) {
          float height = SimTownTerrain_CornerUnits(town, x, y, corner);
          if (height > expected) expected = height;
        }
    CHECK(fabsf(SimTownTerrain_MaximumUnits(town) - expected) < 0.0001f);
  }
}

static void TestLevelPair(void) {
  float height = 123.0f;
  CHECK(!SimTownTerrain_LevelPairUnits(
      0, 0, 0, 0.5f, 0.5f, 0, 0, 0.5f, 0.5f, &height));
  CHECK(height == 0.0f);
  CHECK(!SimTownTerrain_LevelPairUnits(
      1, 0, 0, 0.5f, 0.5f, 1, 1, 0.5f, 0.5f, NULL));

  /* Use the town's lowest and highest cell centres as deliberately unequal
   * bridge-bank stand-ins. A level span must choose the higher anchor, never
   * average/interpolate between the two. */
  float low = 1000.0f, high = -1000.0f;
  int low_x = 0, low_y = 0, high_x = 0, high_y = 0;
  for (int y = 0; y < kSimTownTerrainCells; y++)
    for (int x = 0; x < kSimTownTerrainCells; x++) {
      float at = SimTownTerrain_HeightUnitsAt(
          1, x * 16.0f + 8.0f, y * 16.0f + 8.0f);
      if (at < low) { low = at; low_x = x; low_y = y; }
      if (at > high) { high = at; high_x = x; high_y = y; }
    }
  CHECK(high > low);
  CHECK(SimTownTerrain_LevelPairUnits(
      1, low_x, low_y, 0.5f, 0.5f,
      high_x, high_y, 0.5f, 0.5f, &height));
  CHECK(fabsf(height - high) < 0.0001f);
  CHECK(SimTownTerrain_LevelPairUnits(
      1, high_x, high_y, 0.5f, 0.5f,
      low_x, low_y, 0.5f, 0.5f, &height));
  CHECK(fabsf(height - high) < 0.0001f);
}

static void TestMaximumRect(void) {
  float height = 123.0f;
  CHECK(!SimTownTerrain_MaximumUnitsInRect(
      0, 0.0f, 0.0f, 1.0f, 1.0f, &height));
  CHECK(height == 0.0f);
  CHECK(!SimTownTerrain_MaximumUnitsInRect(
      1, 2.0f, 0.0f, 1.0f, 1.0f, &height));
  CHECK(!SimTownTerrain_MaximumUnitsInRect(
      1, 0.0f, 0.0f, 1.0f, 1.0f, NULL));

  CHECK(SimTownTerrain_MaximumUnitsInRect(
      1, 0.0f, 0.0f, 16.0f, 16.0f, &height));
  float expected = -1000.0f;
  for (int y = 0; y <= 1; y++)
    for (int x = 0; x <= 1; x++)
      for (int corner = 0; corner < 4; corner++) {
        float value = SimTownTerrain_CornerUnits(1, x, y, corner);
        if (value > expected) expected = value;
      }
  CHECK(fabsf(height - expected) < 0.0001f);

  /* The user capture's Marahna bridge occupies x=63..81, y=420..430.
   * Its path-centre bank anchors are the visual placement datum. The maximum
   * under the full sloped footprint is retained separately as its conservative
   * depth envelope, keeping terrain from erasing the rear paving without
   * visibly perching the bridge above the approaches. */
  float midpoint_datum;
  CHECK(SimTownTerrain_LevelPairUnits(
      5, 3, 26, 1.0f, 0.5f, 5, 26, 0.0f, 0.5f,
      &midpoint_datum));
  CHECK(fabsf(midpoint_datum - 1.8671875f) < 0.0001f);
  CHECK(SimTownTerrain_MaximumUnitsInRect(
      5, 63.0f, 420.0f, 81.0f, 430.0f, &height));
  CHECK(height > midpoint_datum + 0.3f);
  CHECK(fabsf(height - 2.23480224609375f) < 0.0001f);
}

static void TestHardEdgeClipping(void) {
  float t0 = -1.0f, t1 = -1.0f;
  CHECK(!SimTownTerrain_ClipHigherEdge(
      0.0f, 0.0f, 1.0f, 1.0f, kVisibleEdgeTestEpsilonUnits, &t0, &t1));
  CHECK(t0 == 0.0f && t1 == 0.0f);
  CHECK(SimTownTerrain_ClipHigherEdge(
      2.0f, 3.0f, 1.0f, 1.0f, kVisibleEdgeTestEpsilonUnits, &t0, &t1));
  CHECK(t0 == 0.0f && t1 == 1.0f);

  /* Each owner receives one non-overlapping half of a reversing boundary.
   * Together they meet at the equal-height point instead of each submitting
   * the same self-crossing four-corner bow-tie. */
  CHECK(SimTownTerrain_ClipHigherEdge(
      2.0f, 0.0f, 0.0f, 2.0f, kVisibleEdgeTestEpsilonUnits, &t0, &t1));
  CHECK(fabsf(t0) < 0.0001f && fabsf(t1 - 0.5f) < 0.0001f);
  CHECK(SimTownTerrain_ClipHigherEdge(
      0.0f, 2.0f, 2.0f, 0.0f, kVisibleEdgeTestEpsilonUnits, &t0, &t1));
  CHECK(fabsf(t0 - 0.5f) < 0.0001f && fabsf(t1 - 1.0f) < 0.0001f);
  CHECK(!SimTownTerrain_ClipHigherEdge(
      NAN, 1.0f, 0.0f, 0.0f, kVisibleEdgeTestEpsilonUnits, &t0, &t1));
  CHECK(!SimTownTerrain_ClipHigherEdge(
      1.0f, 1.0f, 0.0f, 0.0f, kVisibleEdgeTestEpsilonUnits, NULL, &t1));

  /* Both color and depth renderers call this policy wrapper. Its exact
   * tolerance stays private so one pass cannot silently diverge. */
  CHECK(!SimTownTerrain_ClipVisibleHigherEdge(
      1.003f, 1.003f, 1.0f, 1.0f, &t0, &t1));
  CHECK(SimTownTerrain_ClipVisibleHigherEdge(
      1.005f, 1.005f, 1.0f, 1.0f, &t0, &t1));
}

static void TestBakedHardEdgeReversals(void) {
  int aitos_reversals = 0;
  for (uint8_t town = 1; town <= kSimTownTerrainTownCount; town++)
    for (int y = 0; y < kSimTownTerrainCells; y++)
      for (int x = 0; x < kSimTownTerrainCells; x++) {
        const uint8_t hard = SimTownTerrain_HardEdges(town, x, y);
        float current[2], neighbour[2];
        for (int direction = 0; direction < 2; direction++) {
          if (direction == 0) {
            if (x + 1 >= kSimTownTerrainCells ||
                !(hard & kSimTownTerrainEdgeEast))
              continue;
            current[0] = SimTownTerrain_CornerUnits(
                town, x, y, kSimTownTerrainCornerNE);
            current[1] = SimTownTerrain_CornerUnits(
                town, x, y, kSimTownTerrainCornerSE);
            neighbour[0] = SimTownTerrain_CornerUnits(
                town, x + 1, y, kSimTownTerrainCornerNW);
            neighbour[1] = SimTownTerrain_CornerUnits(
                town, x + 1, y, kSimTownTerrainCornerSW);
          } else {
            if (y + 1 >= kSimTownTerrainCells ||
                !(hard & kSimTownTerrainEdgeSouth))
              continue;
            current[0] = SimTownTerrain_CornerUnits(
                town, x, y, kSimTownTerrainCornerSW);
            current[1] = SimTownTerrain_CornerUnits(
                town, x, y, kSimTownTerrainCornerSE);
            neighbour[0] = SimTownTerrain_CornerUnits(
                town, x, y + 1, kSimTownTerrainCornerNW);
            neighbour[1] = SimTownTerrain_CornerUnits(
                town, x, y + 1, kSimTownTerrainCornerNE);
          }
          const float difference_0 = current[0] - neighbour[0];
          const float difference_1 = current[1] - neighbour[1];
          if (difference_0 * difference_1 >= 0.0f) continue;
          if (town == 4) aitos_reversals++;
          float a0, a1, b0, b1;
          CHECK(SimTownTerrain_ClipHigherEdge(
              current[0], current[1], neighbour[0], neighbour[1],
              kVisibleEdgeTestEpsilonUnits, &a0, &a1));
          CHECK(SimTownTerrain_ClipHigherEdge(
              neighbour[0], neighbour[1], current[0], current[1],
              kVisibleEdgeTestEpsilonUnits, &b0, &b1));
          CHECK(fabsf((a1 - a0) + (b1 - b0) - 1.0f) < 0.0001f);
          CHECK(a1 <= b0 + 0.0001f || b1 <= a0 + 0.0001f);
          CHECK(fabsf(fminf(a0, b0)) < 0.0001f);
          CHECK(fabsf(fmaxf(a1, b1) - 1.0f) < 0.0001f);
        }
      }
  /* The three reported Aitos crops come from this exact topology family.
   * Keep the audited map rich enough to exercise the real fix, not just the
   * synthetic half-and-half edge above. */
  CHECK(aitos_reversals >= 10);
}

int main(void) {
  TestBoundsAndSampling();
  TestExplicitCellOwnership();
  TestTopologyContract();
  TestTownRelief();
  TestTownMaximum();
  TestLevelPair();
  TestMaximumRect();
  TestHardEdgeClipping();
  TestBakedHardEdgeReversals();
  if (s_failures) {
    fprintf(stderr, "%d failure(s)\n", s_failures);
    return 1;
  }
  puts("sim town terrain tests passed");
  return 0;
}
