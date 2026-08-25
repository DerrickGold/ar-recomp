#ifndef TESTS_FIXTURES_SIM_AITOS_HOUSE_BUILD_GF14690_H
#define TESTS_FIXTURES_SIM_AITOS_HOUSE_BUILD_GF14690_H

#include <stdint.h>

/* Minimal lossless fixture extracted from
 * runs/20260824-201110/snapshots/snap_02_gf14690.wram.bin
 * SHA-256 c0bfbb25a7de4eed75d7885ebffd8ec4d69abe248d68b0923eeb09f4be448c9f.
 *
 * Aitos record slot 82 is visually on the first $30 scaffold frame even
 * though the semantic cell map already says house ($E0). Keeping the exact
 * record, live tile words, atlas definition and step slot prevents a synthetic
 * fixture from merely restating classifier assumptions. */
enum {
  kAitosBuildTown = 4,
  kAitosBuildRecordSlot = 82,
  kAitosBuildCellX = 23,
  kAitosBuildCellY = 20,
  kAitosBuildMetatile = 0x30,
  kAitosBuildCellMarker = 0xE0,
};

static const uint8_t kAitosBuildRecord[4] = {0x17, 0x14, 0xA0, 0xE0};
static const uint8_t kAitosBuildStepSlot[8] = {
  0x03, 0x00, 0xDC, 0xD7, 0xD8, 0xD7, 0xDA, 0xD7,
};
static const uint16_t kAitosBuildDefinition[4] = {
  0x1582, 0x1583, 0x1592, 0x1593,
};
static const uint16_t kAitosBuildLiveCell[4] = {
  0x1582, 0x1583, 0x1592, 0x1593,
};
static const uint8_t kAitosBuildSequenceMetatiles[3] = {0x30, 0x31, 0x32};
static const uint16_t kAitosBuildSequenceDefinitions[3][4] = {
  {0x1582, 0x1583, 0x1592, 0x1593},
  {0x1580, 0x1581, 0x1590, 0x1591},
  {0x154C, 0x154D, 0x155C, 0x155D},
};

#endif  /* TESTS_FIXTURES_SIM_AITOS_HOUSE_BUILD_GF14690_H */
