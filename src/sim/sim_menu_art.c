#include "sim/sim_menu_art.h"

#include <string.h>

static unsigned Word(const uint8_t *r, unsigned at) {
  return r[at] | r[at+1] << 8;
}

bool SimMenuArt_MeasurePrompt(const uint8_t *rom, size_t bytes, unsigned source,
                             unsigned *width, unsigned *height) {
  static const unsigned sources[]={0xfd15,0xfdb9,0xff57,0xfe2a,0xfec7,
                                    0xf99b,0xf9ba,0xfa7b};
  bool audited=false;
  for (unsigned i=0;i<sizeof(sources)/sizeof(sources[0]);++i)
    if (source==sources[i]) audited=true;
  if (!audited || !rom || bytes<0x26000 || !width || !height) return false;
  unsigned column=0,row=0,columns=0,rows=0;
  for (unsigned token=0;token<256 && source<bytes;++token) {
    const uint8_t ch=rom[source++];
    if (ch==1) {
      if (!columns || !rows) return false;
      *width=columns*8+1; *height=rows*8+1;
      return true;
    }
    if (ch==5) { column=row=columns=rows=0; continue; }
    if (ch==0x0d) { column=0; if (++row>=6) return false; continue; }
    if (ch==6) {
      /* Native salutation: "Sir " and at most eight name cells. All audited
       * prompts have a longer question row; no live name or text is copied. */
      column+=12;
      if (column>columns) columns=column;
      rows=row+1;
      continue;
    }
    if (ch<0x20) return false; /* Unknown control: retain captured-ink fallback. */
    const unsigned at=ch>=0x80?0x258f3+(ch-0x80)*12:0;
    for (unsigned i=0;i<(at?12u:1u);++i) {
      const uint8_t letter=at?rom[at+i]:ch;
      if (letter<0x20 || letter>=0x80) return false;
      ++column;
      if (column>24) return false;
      if (letter!=' ') {
        if (column>columns) columns=column;
        rows=row+1;
      }
      if (at && letter==' ') break;
    }
  }
  return false;
}

static void Label(const uint8_t *rom, unsigned at, char out[64]) {
  unsigned n=0;
  for (unsigned tokens=0;tokens<256 && n<63;++tokens) {
    const uint8_t ch=rom[at++];
    if (!ch) break;
    if (ch == 0x0b) { ++at; continue; } /* Native centering spaces. */
    if (ch == 0x0d || ch == ' ') {
      if (n && out[n-1] != ' ') out[n++]=' ';
    } else if (ch >= 0x80) {
      const unsigned dictionary=0x258f3 + (ch-0x80)*12;
      for (unsigned i=0;i<12 && n<63;++i) {
        const uint8_t letter=rom[dictionary+i];
        if (letter < 0x20 || letter >= 0x80) break;
        out[n++]=(char)letter;
        if (letter == ' ') break;
      }
    } else if (ch >= 0x20) out[n++]=(char)ch;
    else break; /* Fixed-label format only; never interpret event text. */
  }
  while(n && out[n-1]==' ') --n;
  out[n]=0;
}

bool SimMenuArt_Capture(SimMenuFrame *f, const SnesRunnerApi *api,
                        SrRunnerHandle *runner) {
  f->valid=false;
  if (!api || !runner || api->struct_size < SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE ||
      !api->rasterize_ppu_obj_parts) return false;
  SrBorrowedSpan rom={.struct_size=sizeof(rom)};
  SrGenerationSnapshot generation={.struct_size=sizeof(generation)};
  if (api->borrow_memory(runner,SR_MEMORY_ROM,&rom) != SR_RESULT_OK ||
      rom.byte_size < 0x100000 ||
      api->query_generations(runner,&generation) != SR_RESULT_OK) return false;
  const uint8_t *r=rom.data;
  unsigned prompt_width=0,prompt_height=0;
  SimMenuArt_MeasurePrompt(r,rom.byte_size,f->model.dialogue_source,
                          &prompt_width,&prompt_height);
  f->prompt_width=prompt_width; f->prompt_height=prompt_height;
  static const uint8_t categories[6]={9,31,25,22,16,19};
  static const uint8_t actions[15]={12,10,32,33,26,27,28,30,29,23,24,17,18,20,21};
  memset(f->argb,0,sizeof(f->argb));
  for (unsigned i=0;i<kSimMenuArtCount;++i) {
    unsigned family=0,label=0;
    if (i<6) { family=categories[i]; label=Word(r,0xf36a+i*2); }
    else if (i<21) { family=actions[i-6]; label=Word(r,0xf34c+(i-6)*2); }
    else if (i<41) { label=Word(r,0xf08e + (i-21)*2); family=r[label++]; }
    /* Observe the People's small angel: family $0B, selected $01:D134.
     * Resolve its genuine grey variant through the same ROM table. */
    else family=i==kSimMenuArtDescribeAngel?0x0b:i==41?35:36;
    if (label) Label(r,label,f->labels[i]);
    for (unsigned variant=0;variant<2;++variant) {
      const unsigned table=Word(r,0xa227+family*2);
      const unsigned script=Word(r,table+variant*2);
      if (r[script]>=0xfd) return false;
      const unsigned composition=Word(r,script+1);
      const unsigned count=r[composition];
      if (!count || count>4) return false;
      SrPpuObjPart parts[4];
      for (unsigned p=0;p<count;++p) {
        const uint8_t *part=r+composition+1+p*5;
        parts[p]=(SrPpuObjPart){
          .x=part[1]>128?part[1]-256:part[1],
          .y=part[2]>128?part[2]-256:part[2],
          .tile_attr=Word(part,3), .size=part[0]&1?16:8};
      }
      uint32_t pixels[256]={0};
      const SrPpuObjPartsRasterRequest request={
        .struct_size=sizeof(request), .lifetime_generation=generation.lifetime_generation,
        .parts=parts,.part_count=count,.x0=0,.y0=0,.x1=16,.y1=16,
        .pixel_format=SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
        .pixels=pixels,.pixel_byte_size=sizeof(pixels),.pitch_bytes=16*4};
      SrPpuObjRasterResult result={.struct_size=sizeof(result)};
      if (api->rasterize_ppu_obj_parts(runner,&request,&result)!=SR_RESULT_OK)
        return false;
      for (unsigned y=0;y<16;++y)
        memcpy(f->argb+(i*16+y)*32+variant*16,pixels+y*16,16*4);
    }
  }
  /* Content identity invalidates texture uploads even across ROM/state resets. */
  uint32_t hash=2166136261u;
  for (unsigned i=0;i<sizeof(f->argb)/sizeof(f->argb[0]);++i)
    hash=(hash^f->argb[i])*16777619u;
  f->art_revision=hash;
  f->valid=true;
  return true;
}
