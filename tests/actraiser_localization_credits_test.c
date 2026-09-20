#include "actraiser/actraiser_localization_credits.h"
#include <stdio.h>
#include <string.h>

static int failures, calls;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while(0)
static uint8_t ram[0x20000];
static uint16_t vram[0x8000], palette[16] = {0,0x7fff,0,0,0,0x025f};
static ArLocalizationFrame frame;
static ActRaiserLocalizationCredits state;
static const char *body = "- Équipe -\nUn nom\n別の名前";
static bool reject;
static char resolved_id[64];

static bool Resolve(void *context, const char *id,
                    ActRaiserResolvedText *result, char *error,
                    size_t error_capacity) {
  (void)context;
  (void)error;
  (void)error_capacity;
  ++calls;
  snprintf(resolved_id,sizeof(resolved_id),"%s",id);
  if (reject || strlen(body) >= sizeof(result->utf8))
    return false;
  strcpy(result->utf8, body);
  result->utf8_bytes = strlen(body);
  result->cluster_count = 1;
  result->source_revision = 23;
  result->inline_object_count = 0;
  result->language = (ArLocalizationTextLanguage){
      .locale = "fr", .direction = kArTextDirection_LeftToRight};
  result->bidi.count = 0;
  memset(result->structural_boundaries, 0,
         AR_TEXT_BOUNDARY_BYTES(sizeof(result->utf8)));
  for (size_t i = 0; i < result->utf8_bytes; ++i)
    ArTextBoundary_Set(result->structural_boundaries, i,
                       result->utf8[i] == '\n');
  return true;
}

static void Show(unsigned page) {
  for (unsigned i=0; i<1024; ++i) {
    const uint8_t *source=ram+0x4000+page*0x800+i*2;
    vram[0x3800+i]=source[0]|(uint16_t)source[1]<<8;
  }
}
static void Capture(unsigned group, unsigned number, unsigned font) {
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame,"ar","test",1,1,&frame.settings));
  ActRaiserLocalizationCredits_Append(&state,&frame,
      (ArTextCellDestination){3,kArTextCellScreen_Composited,0x3800},
      group,number,font,ram,sizeof(ram),vram,0x8000,
      palette,16,Resolve,NULL);
}

int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1],"--check-pages")) {
    FILE *file=fopen(argv[2],"rb");
    if (!file) return 2;
    const bool ok=fread(ram+0x4000,1,0xa000,file)==0xa000 && fgetc(file)==EOF;
    fclose(file);
    if (!ok) return 2;
  } else if (argc == 1) {
    for (unsigned page=0; page<20; ++page) for (unsigned i=0;i<1024;++i) {
      const unsigned word=i==13*32+10 ? 0x420+page : 0x10;
      ram[0x4000+page*0x800+i*2]=word;
      ram[0x4000+page*0x800+i*2+1]=word>>8;
    }
  } else return 2;
  for (unsigned page=0;page<20;++page) {
    Show(page); Capture(8,1,0x5000);
    CHECK(frame.snapshot_count==(page==15 || page==16 ? 0 : 1));
    if (page<15) {
      char id[32]; snprintf(id,sizeof(id),"credits.page_%02u",page);
      CHECK(!strcmp(id,resolved_id));
    }
    if (page==17) CHECK(!strcmp(resolved_id,"credits.the_end"));
    if (page==18) CHECK(!strcmp(resolved_id,"credits.best_player"));
    if (page==19) CHECK(!strcmp(resolved_id,"credits.game_over"));
  }
  Show(1); Capture(8,1,0x5000);
  CHECK(frame.snapshot_count==1 && frame.cells.count==1);
  CHECK(!strcmp(frame.snapshots[0].language.locale,"fr"));
  CHECK(frame.snapshots[0].language.direction==kArTextDirection_LeftToRight);
  CHECK(frame.cells.records[0].region.row==1 && frame.cells.records[0].region.rows==26);
  CHECK(frame.snapshots[0].accent_end_utf8_byte==11); // seven padded rows, '- ', É
  CHECK(frame.snapshots[0].accent_rgb==0xff9400);
  CHECK(frame.snapshots[0].body_rgb==0xffffff);
  CHECK(!frame.snapshots[0].shadow_enabled && !frame.snapshots[0].slant_ascii_numerals);
  CHECK(frame.grids[0].center_rows && frame.grids[0].row_height==4);
  const int before=calls;
  for (unsigned i=0;i<1000;++i) Capture(8,1,0x5000);
  CHECK(calls==before); // Warm page does not parse/resolve/allocate again.
  Capture(0,7,0x5000); CHECK(!frame.snapshot_count);
  Capture(8,2,0x5000); CHECK(!frame.snapshot_count);
  Capture(8,1,0x1000); CHECK(!frame.snapshot_count);
  Capture(8,1,0x5000); CHECK(frame.snapshot_count==1);
  vram[0x3800]^=1; Capture(8,1,0x5000); CHECK(!frame.snapshot_count);
  Show(1); reject=true; state.resolved=false;
  Capture(8,1,0x5000); CHECK(!frame.snapshot_count);
  const int failed=calls;
  Capture(8,1,0x5000); CHECK(calls==failed); // Failure is cached until pack/scene changes.
  reject=false; state.resolved=false; body="";
  Capture(8,1,0x5000); CHECK(frame.snapshot_count==1 && !frame.snapshots[0].utf8_bytes);
  state.resolved=false; body="one\ntwo\nthree\nfour\nfive\nsix\nseven";
  Capture(8,1,0x5000); CHECK(!frame.snapshot_count);
  // Debug restore or clear cannot keep a cached claim on a blank inventory.
  for (unsigned i=0x4000;i<0xe000;i+=2) { ram[i]=0x10;ram[i+1]=0; }
  Show(1); Capture(8,1,0x5000); CHECK(!frame.snapshot_count);
  return failures ? 1 : 0;
}
