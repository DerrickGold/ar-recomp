#include "actraiser/actraiser_story_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t wram[0x20000], sram[kActRaiserSramSize];
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  if (bank == 0x70) { assert(address < sizeof(sram)); return sram[address]; }
  assert(bank == 0 || bank == 0x7f);
  return wram[(bank == 0x7f ? 0x10000 : 0) + address];
}
static void Read(const char *path, void *buffer, size_t length) {
  FILE *f = fopen(path, "rb"); assert(f);
  assert(fread(buffer, 1, length, f) == length && fgetc(f) == EOF);
  assert(fclose(f) == 0);
}
static void Checks(void) {
  uint8_t result[kActRaiserSramSize], before[kActRaiserSramSize];
  for (unsigned i = 0; i < sizeof(wram); ++i) wram[i] = (i * 17 + (i >> 8) * 7) & 255;
  for (unsigned i = 0; i < sizeof(sram); ++i) sram[i] = i * 11 + 91;
  memcpy(before, sram, sizeof(before));
  CpuState cpu = {.A=0x1234,.X=0x5678,.Y=0x9abc,.D=0x200,.S=0x1e00,.PB=2,.DB=0x42,.P=0xff};
  const CpuState original = cpu;
  assert(ActRaiserStorySnapshot_Capture(&cpu, result));
  assert(!memcmp(&cpu, &original, sizeof(cpu)) && !memcmp(sram, before, sizeof(sram)));
  assert(Save_ChecksumValid(result));
  assert(!memcmp(result, wram + 0x16800, 0x300));
  assert(!memcmp(result + 0x600, wram + 0x16be7, 0xc00));
  assert(!memcmp(result + 0x1633, wram + 0x197da, 0x720));
  assert(!memcmp(result + 0x1369, wram + 0x16b32, 6));
  assert(!memcmp(result + 0x136f, sram + 0x136f, 6));
  assert(!memcmp(result + 0x1d6b, sram + 0x1d6b, 0x1fec - 0x1d6b));
  assert(!memcmp(result + 0x1ff0, sram + 0x1ff0, 16));
  memcpy(before, result, sizeof(before));
  assert(!ActRaiserStorySnapshot_Capture(NULL, result) && !memcmp(result, before, sizeof(before)));
  assert(!ActRaiserStorySnapshot_Capture(&cpu, NULL));
}
int main(int argc, char **argv) {
  Checks();
  if (argc == 4) {
    uint8_t result[kActRaiserSramSize], expected[kActRaiserSramSize];
    Read(argv[1], wram, sizeof(wram)); Read(argv[2], sram, sizeof(sram));
    Read(argv[3], expected, sizeof(expected));
    CpuState cpu = {0};
    assert(ActRaiserStorySnapshot_Capture(&cpu, result));
    for (unsigned i=0; i<sizeof(result); ++i) if (result[i] != expected[i]) {
      fprintf(stderr,"native snapshot mismatch at %04x: %02x != %02x\n",i,result[i],expected[i]);
      return 1;
    }
  } else assert(argc == 1);
  puts("story snapshot: read-only capture, reserved bytes and checksum passed");
  return 0;
}
