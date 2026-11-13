/* Headless diagnostics of the production compiler. No renderer or copied
 * material policy: this audit translation unit includes the compiler to read
 * immutable material interpretation, bounds and flags without a copied policy. */
#include "action/action_scene_snapshot.h"
#include "action/action_voxel_build.c"
#include "action/action_trunk.h"
#include "actraiser_game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool export_surfaces;
static ActionSceneSnapshot snapshot;
static uint8_t bytes[kActionSceneSnapshotMaxBytes + 1];

static void JsonString(const char *s) {
  putchar('"');
  for (; *s; ++s) {
    const unsigned char c=(unsigned char)*s;
    if(c=='"' || c=='\\')printf("\\%c",c);
    else if(c<32)printf("\\u%04x",c);
    else putchar(c);
  }
  putchar('"');
}
static uint64_t AuditHashByte(uint64_t h,unsigned b) {
  return (h^(b&255))*UINT64_C(1099511628211);
}
static unsigned AuditPixel(const ActionRoomScene *s,unsigned word,int x,int y) {
  unsigned tile=word&1023;
  const uint8_t *p=NULL;
  if(tile<512)p=s->characters+tile*32;
  else if(tile<768 && s->have_extra_characters)p=s->extra_characters+(tile-512)*32;
  if(!p)return 0;
  if(word&0x4000)x=7-x;if(word&0x8000)y=7-y;
  const unsigned bit=7-x;
  return ((p[y*2]>>bit)&1)|(((p[y*2+1]>>bit)&1)<<1)|
      (((p[y*2+16]>>bit)&1)<<2)|(((p[y*2+17]>>bit)&1)<<3);
}
static unsigned Collision(const ActionRoomScene *s,unsigned id) {
  unsigned mask=0;
  for(unsigned q=0;q<4;++q)mask|=!!(s->bg[0].metatiles[id*8+q*2]&2)<<q;
  return mask;
}
static bool LoadIni(const char *path,DioramaLayerOrderTable *table) {
  if(!path)return true;
  FILE *f=fopen(path,"r");if(!f){perror(path);return false;}
  char line[4096];unsigned number=0;bool ok=true;DioramaRoomOverride *current=NULL;
  while(ok && fgets(line,sizeof(line),f)) {
    ++number;
    if(!strchr(line,'\n') && !feof(f)){ok=false;break;}
    line[strcspn(line,";#\r\n")]=0;
    char *s=line;while(*s==' ' || *s=='\t')++s;if(!*s)continue;
    if(*s=='[') {
      char *end=strchr(s,']');if(!end){ok=false;break;}*end=0;current=NULL;
      uint8_t group,room,section;
      if(DioramaLayerOrder_ParseScopedSection(s+1,&group,&room,&section)) {
        current=DioramaLayerOrder_FindOrAddSection(table,group,room,section);ok=current!=NULL;
      }
    } else if(current) {
      const char *error=NULL;ok=DioramaLayerOrder_ParseLine(current,s,&error);
      if(!ok)fprintf(stderr,"%s:%u: %s\n",path,number,error?error:"invalid scenery record");
    }
  }
  ok=ok && !ferror(f);fclose(f);
  if(!ok)fprintf(stderr,"Cannot parse scenery INI %s at line %u\n",path,number);
  return ok;
}
static bool Intersects(int x,int y,int px,int py,int w,int h) {
  return x<px+w && x+16>px && y<py+h && y+16>py;
}
static void Flag(bool *comma,const char *name) {
  if(*comma)putchar(',');*comma=true;JsonString(name);
}
static void Cell(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    const DioramaRoomOverride *terrain,const uint16_t *owners,int x,int y) {
  VoxelCellGeometry shape;if(!ActionVoxel_CellGeometry(g,x,y,&shape))return;
  uint16_t words[4]={0},native_word=0;uint8_t native_id=0;
  const bool native=x>=0 && y>=0 && ActionRoomScene_LookupTile(s,1,x*2,y*2,&native_word,&native_id);
  const DioramaTileStamp *stamp=DioramaLayerOrder_StampAt(&terrain->stamp_layers[0],x,y);
  const unsigned id=stamp?stamp->metatile:native_id;
  const bool blank=stamp?stamp->blank:!native;
  for(unsigned q=0;q<4;++q) {
    if(stamp)words[q]=stamp->words[q];
    else if(native)ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&words[q],NULL);
  }
  const DioramaPixelEdit *edit=DioramaLayerOrder_PixelEditAt(terrain,0,x,y,id);
  const VoxelAuthoredCell *authored=ActionVoxel_AuthoredCellAt(g,x,y);
  const size_t at=(size_t)(y-g->origin_y/16)*(g->width/16)+x-g->origin_x/16;
  const unsigned owner=owners?owners[at]:0;
  const uint32_t fields=owner?terrain->voxel.cells[owner-1].fields:0;
  uint64_t art=UINT64_C(14695981039346656037),visual=art;
  unsigned priority=0,roles=0,opaque=0,borders[4]={0};
  bool curved=false,pixel_recess=false,animated=false;
  const bool has_animation=ActionRoomScene_HasCharacterAnimation(s);
  const size_t animation_start=(size_t)ActionRoomScene_CharacterAnimationTarget(s)*2;
  const size_t animation_end=animation_start+ActionRoomScene_CharacterAnimationStride(s);
  for(unsigned q=0;q<4;++q) {
    priority|=!!(words[q]&0x2000)<<q;
    curved|=ActionVoxel_TrunkCurveForCell(g,x*2+(q&1),y*2+(q>>1))!=NULL ||
        ActionVoxel_ColumnCurveForCell(g,x*2+(q&1),y*2+(q>>1))!=NULL;
    pixel_recess|=ActionVoxel_PixelRecessRows(g,x*2+(q&1),y*2+(q>>1))!=NULL;
    if(has_animation) {
      const size_t offset=(words[q]&1023)*32;
      animated|=offset>=animation_start && offset<animation_end;
    }
  }
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
    const unsigned word=words[(py/8)*2+px/8],ink=blank?0:AuditPixel(s,word,px&7,py&7);
    bool visible=ink!=0;unsigned index=((word>>10)&7)*16+ink;
    uint16_t rgb=visible?(uint16_t)(0x8000|s->palette[index*2]|s->palette[index*2+1]<<8):0;
    if(edit && (edit->transparent[py]&(1u<<(15-px))))visible=false;
    if(edit && (edit->black[py]&(1u<<(15-px)))){visible=true;index=128;rgb=0x8000;}
    if(!visible)rgb=0;
    if(authored && authored->surface && authored->pixels[0]) {
      rgb=authored->pixels[0][py*16+px];visible=!!(rgb&0x8000);index=256+(rgb&0x7fff);
    }
    art=AuditHashByte(AuditHashByte(art,visible?index+1:0),(visible?index+1:0)>>8);
    visual=AuditHashByte(AuditHashByte(visual,rgb),rgb>>8);
    const size_t p=(size_t)(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x;
    const unsigned role=g->front[p]&kVoxelKindMask;
    if(role)roles|=1u<<role;
    if(!visible)continue;
    ++opaque;
    /* Native matte-only backing is not a physical connection witness. */
    if(!role || role==kVoxelRearOnly || role==kVoxelFlatParapet)continue;
    if(!py)borders[0]|=1u<<px;if(px==15)borders[1]|=1u<<py;
    if(py==15)borders[2]|=1u<<px;if(!px)borders[3]|=1u<<py;
  }
  printf("{\"x\":%d,\"y\":%d,\"native_id\":%d,\"id\":%u,\"native_collision\":%u,\"collision\":%u,\"priority\":%u,\"words\":[%u,%u,%u,%u],\"art\":\"%016llx\",\"visual_art\":\"%016llx\",\"opaque\":%u,\"borders\":[%u,%u,%u,%u],\"semantic\":%u,\"roles\":%u,\"front\":[%.7g,%.7g,%.7g,%.7g],\"back\":[%.7g,%.7g,%.7g,%.7g],\"width\":%.7g,\"offset_x\":%.7g,\"faces\":%u,\"shape\":%u,\"authored_fields\":%u,\"stamp\":%s,\"stamp_bands\":%u,\"pixel_edit\":%s,\"flags\":[",
      x,y,native?(int)native_id:-1,id,native?Collision(s,native_id):0,blank?0:Collision(s,id),priority,
      words[0],words[1],words[2],words[3],(unsigned long long)art,(unsigned long long)visual,opaque,
      borders[0],borders[1],borders[2],borders[3],g->semantic_cells?g->semantic_cells[at]:0,roles,
      shape.front[0],shape.front[1],shape.front[2],shape.front[3],shape.back[0],shape.back[1],shape.back[2],shape.back[3],
      shape.width,shape.offset_x,shape.faces,shape.shape,fields,stamp?"true":"false",stamp?stamp->bands:0,edit?"true":"false");
  bool comma=false;
  if(!opaque)Flag(&comma,"empty_art");
  if(fields || (authored && (authored->surface || authored->native_transform_requested)))Flag(&comma,"authored_override");
  if(curved)Flag(&comma,"native_curve");
  if(pixel_recess)Flag(&comma,"pixel_recess");
  if(animated)Flag(&comma,"animated_source");
  if(roles && (roles&(roles-1)))Flag(&comma,"mixed_pixel_roles");
  const unsigned semantic=g->semantic_cells?g->semantic_cells[at]:0;
  if(semantic==kVoxelSemantic_Foliage || semantic==kVoxelSemantic_Water ||
      semantic==kVoxelSemantic_Hazard || semantic==kVoxelSemantic_Gate ||
      semantic==kVoxelSemantic_Column || semantic==kVoxelSemantic_Branch)Flag(&comma,"special_semantic");
  bool patch=false;
  for(unsigned i=0;i<g->patch_count;++i)patch|=Intersects(x*16,y*16,g->patches[i].x,g->patches[i].y,g->patches[i].width,g->patches[i].height);
  if(patch)Flag(&comma,"special_patch");
  putchar(']');
  /* Mixed quadrants can contain rear ink and a modeled front. */
  if(export_surfaces) {
    printf(",\"native_front_recess\":[");
    for(unsigned q=0;q<4;++q)
      printf("%s%.7g",q?",":"",ActionVoxel_FrontRecess(g,x*2+(q&1),y*2+(q>>1)));
    putchar(']');
  }
  putchar('}');
}
#include "voxel_surface_export.inc"

static bool Export(const char *path,const DioramaLayerOrderTable *table) {
  FILE *f=fopen(path,"rb");if(!f){perror(path);return false;}
  const size_t size=fread(bytes,1,sizeof(bytes),f);const bool ok=!ferror(f);fclose(f);
  if(!ok || !ActionSceneSnapshot_Decode(bytes,size,&snapshot)){fprintf(stderr,"Invalid scene: %s\n",path);return false;}
  const ActionRoomScene *s=&snapshot.scene;ActionRoomSceneFrameState frame;
  if(!ActionRoomScene_BuildFrameState(s,&snapshot.frame,&frame))return false;
  const bool completion=ActRaiser_IsDeathHeimCompletionScene(s->group,s->map,7,frame.bgsc[0],frame.bgsc[1]);
  const unsigned section=completion?kDioramaLayerSection_DeathHeimCompletion:kDioramaLayerSection_Room;
  DioramaRoomOverride *terrain=calloc(1,sizeof(*terrain));ActionVoxelGeometry *g=ActionVoxel_Create();
  if(!terrain || !g){free(terrain);ActionVoxel_Release(g);return false;}
  const DioramaRoomOverride *base=DioramaLayerOrder_FindSection(table,s->group,s->map,section);
  bool built=!base || DioramaLayerOrder_ForTerrain(base,snapshot.terrain_profile,terrain);
  if(built && !completion)built=ActionVoxel_Build(g,s,terrain,ActionVoxel_SettingsForRoom(s->group,s->map));
  if(!built){fprintf(stderr,"Cannot compile %s: %s\n",path,ActionVoxel_BuildError(g));DioramaLayerOrder_ClearRoom(terrain);free(terrain);ActionVoxel_Release(g);return false;}
  printf("{\"schema\":1,\"source\":");JsonString(path);
  printf(",\"room\":\"%02x%02x\",\"terrain\":%u,\"section\":",s->group,s->map,snapshot.terrain_profile);
  JsonString(completion?"completion":"room");
  const char *status=completion?"completion_separate":!ActionVoxel_Room(s->group,s->map)?
      (s->group==6 && (s->map==6 || s->map==7)?"specialized_curved_geometry":"no_voxel_profile"):
      !g->ready || !g->front?"no_compiled_geometry":"ok";
  printf(",\"status\":");JsonString(status);
  printf(",\"frame\":{\"game_frame\":%u,\"resolved_animation_phase\":%u,\"bgsc\":[%u,%u],\"character_source\":\"snapshot_bytes\"}",
      frame.game_frame,frame.animation_phase,frame.bgsc[0],frame.bgsc[1]);
  printf(",\"profile\":%u,\"depth\":%.7g,\"bounds\":[%d,%d,%d,%d],\"cells\":[",
      ActionVoxel_Profile(g),ActionVoxel_Depth(g),g->origin_x/16,g->origin_y/16,
      (g->origin_x+g->width)/16,(g->origin_y+g->height)/16);
  if(!strcmp(status,"ok")) {
    uint16_t *owners=ActionVoxel_RecordOwners(&terrain->voxel,g->origin_x/16,g->origin_y/16,g->width/16,g->height/16);
    if(!owners){DioramaLayerOrder_ClearRoom(terrain);free(terrain);ActionVoxel_Release(g);return false;}
    bool comma=false;
    for(int y=g->origin_y/16;y<(g->origin_y+g->height)/16;++y)
      for(int x=g->origin_x/16;x<(g->origin_x+g->width)/16;++x) {
        if(comma)putchar(',');comma=true;Cell(s,g,terrain,owners,x,y);
      }
    free(owners);
  }
  putchar(']');
  if(export_surfaces)AuditSurfaces(s,g,terrain,completion);
  printf("}\n");fflush(stdout);
  DioramaLayerOrder_ClearRoom(terrain);free(terrain);ActionVoxel_Release(g);return !ferror(stdout);
}
int main(int argc,char **argv) {
  DioramaLayerOrderTable *table=calloc(1,sizeof(*table));if(!table)return 1;
  int first=1;
  while(first<argc && argv[first][0]=='-') {
    if(!strcmp(argv[first],"--surfaces")){export_surfaces=true;++first;}
    else if(!strcmp(argv[first],"--ini") && first+1<argc) {
      if(!LoadIni(argv[first+1],table)){DioramaLayerOrder_ClearTable(table);free(table);return 1;}first+=2;
    } else {fprintf(stderr,"Unknown audit option: %s\n",argv[first]);free(table);return 2;}
  }
  if(first==argc){fprintf(stderr,"usage: %s [--surfaces] [--ini scenery.ini] scene.arscene [...]\n",argv[0]);free(table);return 2;}
  bool ok=true;for(int i=first;ok && i<argc;++i)ok=Export(argv[i],table);
  DioramaLayerOrder_ClearTable(table);free(table);return ok?0:1;
}
