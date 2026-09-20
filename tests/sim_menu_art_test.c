#include "sim/sim_menu_art.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  static uint8_t rom[0x26000];
  unsigned width=0,height=0;
  /* Synthetic ROM data: a dictionary word, native break and trailing paper.
   * Extents describe the entire prompt before any glyph has been painted. */
  memcpy(rom+0x258f3,"Example     ",12);
  const uint8_t text[]={5,0x80,'?',0x0d,'O','K',' ',' ',1};
  memcpy(rom+0xff57,text,sizeof(text));
  assert(SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xff57,&width,&height));
  assert(width==73 && height==17);
  const uint8_t named[]={5,6,0x0d,'T','e','x','t',1};
  memcpy(rom+0xf99b,named,sizeof(named));
  assert(SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xf99b,&width,&height));
  assert(width==97 && height==17);
  /* Unknown sources/controls, short ROMs and unbounded lines fail closed. */
  assert(!SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xf000,&width,&height));
  assert(!SimMenuArt_MeasurePrompt(rom,0x10000,0xff57,&width,&height));
  rom[0xff58]=2;
  assert(!SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xff57,&width,&height));
  memset(rom+0xff57,'A',256);
  assert(!SimMenuArt_MeasurePrompt(rom,sizeof(rom),0xff57,&width,&height));
  puts("sim_menu_art: OK");
}
