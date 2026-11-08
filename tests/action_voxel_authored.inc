#include "diorama/diorama_voxel_cap_clip.h"
static void AuthoredBendContracts(void) {
  for(unsigned axis=kDioramaVoxelBend_X;axis<=kDioramaVoxelBend_Y;++axis)
    for(unsigned reverse=0;reverse<2;++reverse) {
      const DioramaVoxelBend bend={.axis=axis,.anchor=reverse?80:16,.tip=reverse?16:80,
          .dx=48,.dy=-32,.dz=.4f};
      const VoxelAuthoredCell first={.x=16,.y=16,.width=8,.offset_x=-4,.bend=bend};
      const VoxelAuthoredCell second={.x=32,.y=32,.width=24,.offset_x=3,.bend=bend};
      const float positions[]={-10,0,.25f,.5f,1,10};
      const float weights[]={0,0,.15625f,.5f,1,1};
      for(unsigned i=0;i<sizeof(positions)/sizeof(*positions);++i) {
        const float source=bend.anchor+(bend.tip-bend.anchor)*positions[i];
        const float x=axis==kDioramaVoxelBend_X?source:123;
        const float y=axis==kDioramaVoxelBend_Y?source:456;
        float dx,dy,dz,other_x,other_y,other_z;
        ActionVoxel_BendOffset(&bend,x,y,&dx,&dy,&dz);
        assert(fabsf(dx-48*weights[i])<.00001f && fabsf(dy+32*weights[i])<.00001f);
        assert(fabsf(dz-.4f*weights[i])<.000001f);
        ActionVoxel_AuthoredBend(&first,x,y,&other_x,&other_y,&other_z);
        assert(dx==other_x && dy==other_y && dz==other_z);
        ActionVoxel_AuthoredBend(&second,x,y,&other_x,&other_y,&other_z);
        assert(dx==other_x && dy==other_y && dz==other_z);
        /* Applying one offset to both depths cannot alter their thickness. */
        assert(fabsf(((.3f+dz)-(.1f+dz))-.2f)<.000001f);
      }
      /* Tangency at the attachment: motion grows quadratically, so the base
       * neither tears away nor develops a discontinuous first derivative. */
      float dx,dy,dz;
      const float source=bend.anchor+(bend.tip-bend.anchor)*.001f;
      ActionVoxel_BendOffset(&bend,axis==kDioramaVoxelBend_X?source:0,
          axis==kDioramaVoxelBend_Y?source:0,&dx,&dy,&dz);
      assert(dx>0 && dx<.0002f);
    }
  float x=1,y=1,z=1;
  ActionVoxel_BendOffset(NULL,0,0,&x,&y,&z);assert(!x && !y && !z);
  const VoxelAuthoredCell plain={0};
  ActionVoxel_AuthoredBend(&plain,8192,-8192,&x,&y,&z);assert(!x && !y && !z);
}
/* These contracts require no game artwork. A compact reset remains the newest
 * covering record, and carved shape records retain their shared Y domain. */
static void AuthoredContracts(void) {
  AuthoredBendContracts();
  /* A shared pivot maps either side of every source-cell join to the same
   * position; scaling around independent tile centers would break this. */
  const DioramaVoxelTransform transform={.px=32,.py=48,.pz=.2f,
      .dx=12,.dy=-8,.dz=-.1f,.sx=.5f,.sy=2,.sz=1.5f};
  assert(ActionVoxel_TransformValid(&transform));
  const VoxelAuthoredCell left={.x=16,.y=32,.transform=transform};
  const VoxelAuthoredCell right={.x=32,.y=32,.transform=transform};
  for(unsigned i=0;i<=16;++i) {
    float ax,ay,az,bx,by,bz;
    ActionVoxel_TransformPoint(&left.transform,left.x+16,left.y+i,.3f,&ax,&ay,&az);
    ActionVoxel_TransformPoint(&right.transform,right.x,right.y+i,.3f,&bx,&by,&bz);
    assert(ax==bx && ay==by && az==bz && ax==44 && fabsf(az-.25f)<.000001f);
  }
  float tx,ty,tz;
  ActionVoxel_TransformPoint(&transform,32,48,.2f,&tx,&ty,&tz);
  assert(tx==44 && ty==40 && fabsf(tz-.1f)<.000001f);
  ActionVoxel_TransformPoint(NULL,2,3,.4f,&tx,&ty,&tz);assert(tx==2 && ty==3 && tz==.4f);
  const DioramaVoxelTransform absent={0};
  ActionVoxel_TransformPoint(&absent,2,3,.4f,&tx,&ty,&tz);assert(tx==2 && ty==3 && tz==.4f);
  const DioramaVoxelTransform identity={.px=7999,.py=-7777,.pz=1,.sx=1,.sy=1,.sz=1};
  ActionVoxel_TransformPoint(&identity,2,3,.4f,&tx,&ty,&tz);
  assert(tx==2 && ty==3 && tz==.4f);
  DioramaVoxelCell native_identity={.fields=kDioramaVoxelCell_Transform,.transform=identity};
  assert(ActionVoxel_CellUsesNativeTransform(&native_identity));
  assert(ActionVoxel_CellKeepsNativeIdentity(&native_identity));
  assert(!ActionVoxel_CellOwnsArtwork(&native_identity));
  native_identity.fields|=kDioramaVoxelCell_Faces;
  native_identity.faces=kDioramaVoxelFaceMask_All;
  assert(ActionVoxel_CellKeepsNativeIdentity(&native_identity) && !ActionVoxel_CellOwnsArtwork(&native_identity));
  native_identity.faces&=~kDioramaVoxelFaceMask_Front;
  assert(ActionVoxel_CellKeepsNativeIdentity(&native_identity) && !ActionVoxel_CellOwnsArtwork(&native_identity));
  native_identity.faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
  assert(!ActionVoxel_CellKeepsNativeIdentity(&native_identity) && ActionVoxel_CellOwnsArtwork(&native_identity));
  native_identity.faces=kDioramaVoxelFaceMask_All;
  native_identity.transform.dx=.000001f;
  assert(!ActionVoxel_CellKeepsNativeIdentity(&native_identity) && ActionVoxel_CellOwnsArtwork(&native_identity));
  native_identity.transform=identity;native_identity.transform.sx=nextafterf(1,2);
  assert(!ActionVoxel_CellKeepsNativeIdentity(&native_identity) && ActionVoxel_CellOwnsArtwork(&native_identity));
  native_identity.transform=identity;native_identity.fields|=kDioramaVoxelCell_TopArt;
  assert(!ActionVoxel_CellKeepsNativeIdentity(&native_identity) && ActionVoxel_CellOwnsArtwork(&native_identity));
  DioramaVoxelTransform invalid=transform;invalid.sx=0;assert(!ActionVoxel_TransformValid(&invalid));
  invalid=transform;invalid.dz=NAN;assert(!ActionVoxel_TransformValid(&invalid));
  static DioramaVoxelEdits edits;memset(&edits,0,sizeof(edits));edits.cell_count=3;
  edits.cells[0]=(DioramaVoxelCell){.x=-1,.y=-1,.w=4,.h=4,.fields=kDioramaVoxelCell_Front};
  edits.cells[1]=(DioramaVoxelCell){.x=0,.y=0,.w=2,.h=2};
  edits.cells[2]=(DioramaVoxelCell){.x=1,.y=1,.fields=kDioramaVoxelCell_Width};
  uint16_t *owners=ActionVoxel_RecordOwners(&edits,-1,-1,4,4);assert(owners);
  assert(owners[0]==1 && owners[5]==2 && owners[10]==3 && owners[15]==1);free(owners);
  assert(!ActionVoxel_RecordOwners(&edits,0,0,512,512));
  VoxelAuthoredCell upper={.shape=kDioramaVoxelShape_Curve,.shape_y=16,.shape_h=64};
  for(unsigned q=0;q<4;++q){upper.front[q]=.02f;upper.back[q]=.3f;
    upper.front_end[q]=.2f;upper.back_end[q]=.4f;}
  VoxelAuthoredCell lower=upper;upper.y=16;lower.y=32;
  for(unsigned q=0;q<4;++q) {
    float a,b,c,d;ActionVoxel_AuthoredDepths(&upper,q,32,&a,&b);ActionVoxel_AuthoredDepths(&lower,q,32,&c,&d);
    assert(a==c && b==d && fabsf(a-(.02f+.18f*.15625f))<.000001f);
    ActionVoxel_AuthoredDepths(&upper,q,16,&a,&b);assert(a==.02f && b==.3f);
    ActionVoxel_AuthoredDepths(&upper,q,80,&a,&b);assert(fabsf(a-.2f)<.000001f && b==.4f);
    upper.shape=kDioramaVoxelShape_Taper;ActionVoxel_AuthoredDepths(&upper,q,32,&a,&b);
    assert(fabsf(a-.065f)<.000001f && fabsf(b-.325f)<.000001f);upper.shape=kDioramaVoxelShape_Curve;
  }
  /* A steep cap crosses this entire texture square between its endpoints.
   * The clipped polygon must retain its area; actor-plane halves must sum to
   * that same area without introducing a hole or overlapping faces. */
  const float near[]={0,.3f},far[]={.02f,.32f};float area[3]={0};
  for(unsigned pass=0;pass<3;++pass) {
    float cuts[8];const unsigned n=AuthoredCapCuts(near,far,.1f,.2f,pass==0,.15f,cuts);
    for(unsigned i=0;i+1<n;++i) {
      float width[2];
      for(unsigned end=0;end<2;++end) {
        const float t=cuts[i+end];float a=near[0]+(near[1]-near[0])*t,b=far[0]+(far[1]-far[0])*t;
        AuthoredCapSpan(&a,&b,pass==0,pass==1,.15f);
        width[end]=fmaxf(.1f,fminf(.2f,b))-fmaxf(.1f,fminf(.2f,a));
      }
      area[pass]+=(width[0]+width[1])*.5f*(cuts[i+1]-cuts[i]);
    }
  }
  assert(fabsf(area[0]-.1f*.02f/.3f)<.000001f);
  assert(fabsf(area[1]+area[2]-area[0])<.000001f && area[1]>0 && area[2]>0);
}
static DioramaVoxelArt AuthoredTestArt(unsigned character) {
  return (DioramaVoxelArt){.words={character,character,character,character},.size=16,.set=true};
}
static void AuthoredVisibilityFixture(const char *path) {
  if(!path)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
  assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
  ActionRoomScene *scene=&snapshot.scene;ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,1);assert(base&&edited);
  assert(ActionVoxel_Build(base,scene,NULL,settings));
  unsigned count;const VoxelPatch *patches=ActionVoxel_Patches(base,&count);const VoxelPatch *stump=NULL;
  for(unsigned i=0;i<count;++i)if(patches[i].kind==kVoxelPatch_SideProfile &&
      patches[i].width==32 && patches[i].height==56){stump=&patches[i];break;}
  assert(stump);const int x=stump->x/16,y=stump->y/16;
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Faces};
  const unsigned masks[]={kDioramaVoxelFaceMask_Front,kDioramaVoxelFaceMask_Back,kDioramaVoxelFaceMask_Top,
      kDioramaVoxelFaceMask_Bottom,kDioramaVoxelFaceMask_Left,kDioramaVoxelFaceMask_Right};
  uint32_t source[32*64],base_rear[32*64],base_front[32*64],rear[32*64],front[32*64];
  for(unsigned i=0;i<32*64;++i)source[i]=0xff203040u+i;
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(base,x*16,y*16,32,64,32,&frame,source,32,base_rear,base_front));
  for(unsigned i=0;i<6;++i) {
    edit->faces=kDioramaVoxelFaceMask_All&~masks[i];assert(ActionVoxel_Build(edited,scene,&terrain,settings));
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,x,y);assert(a && a->faces==edit->faces);
    if(masks[i]==kDioramaVoxelFaceMask_Front) {
      assert(a->native_front_hidden && !a->surface && !a->contour_count);
      assert(ActionVoxel_HasHiddenNativeFronts(edited));SameGeometry(base,edited);
      assert(ActionVoxel_Split(edited,x*16,y*16,32,64,32,&frame,source,32,rear,front));
      assert(!memcmp(rear,base_rear,sizeof(rear)) && !memcmp(front,base_front,sizeof(front)));
    }
    VoxelCellGeometry before,after;
    assert(ActionVoxel_CellGeometry(base,x+1,y,&before) && ActionVoxel_CellGeometry(edited,x+1,y,&after));
    assert(!memcmp(&before,&after,sizeof(before)));
    /* Re-enabling each face must restore every semantic face, curve and
     * patch, not leave behind a replacement prism from the previous hide. */
    edit->faces=kDioramaVoxelFaceMask_All;assert(ActionVoxel_Build(edited,scene,&terrain,settings));
    assert(!ActionVoxel_AuthoredCellAt(edited,x,y));SameGeometry(base,edited);
    assert(!ActionVoxel_HasHiddenNativeFronts(edited));
    assert(!ActionVoxel_AuthoredArtCount(edited));
    assert(ActionVoxel_Split(edited,x*16,y*16,32,64,32,&frame,source,32,rear,front));
    assert(!memcmp(rear,base_rear,sizeof(rear)) && !memcmp(front,base_front,sizeof(front)));
  }
  /* Saved old faces:all records are still whole-record resets; they must not
   * accidentally resurrect a previous compact depth edit underneath. */
  terrain.voxel.cells[0]=(DioramaVoxelCell){.x=x,.y=y,.w=2,.h=4,
      .fields=kDioramaVoxelCell_Front,.front=-.2f};
  terrain.voxel.cells[1]=(DioramaVoxelCell){.x=x,.y=y,.w=2,.h=4,
      .fields=kDioramaVoxelCell_Faces,.faces=kDioramaVoxelFaceMask_All};
  terrain.voxel.cell_count=2;assert(ActionVoxel_Build(edited,scene,&terrain,settings));
  SameGeometry(base,edited);assert(!ActionVoxel_AuthoredArtCount(edited));
  /* Adding all-visible to a backing-only edit cannot promote its front. */
  terrain.voxel.cell_count=1;terrain.voxel.cells[0]=(DioramaVoxelCell){.x=x,.y=y,
      .fields=kDioramaVoxelCell_Backing|kDioramaVoxelCell_Faces,
      .backing=kDioramaVoxelBacking_Fill,.faces=kDioramaVoxelFaceMask_All};
  assert(ActionVoxel_Build(edited,scene,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,x,y);assert(a && !a->surface);
  ActionVoxel_Release(base);ActionVoxel_Release(edited);
  puts("authored visibility: six stump hide/show cycles restore native geometry, splits and artwork; covering reset and backing-only inheritance passed");
}
static void AuthoredBranchFixture(const char *path) {
  if(!path)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
  assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
  ActionRoomScene *scene=&snapshot.scene;assert(scene->group==1 && scene->map==1);
  ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,1);
  assert(ActionVoxel_Build(base,scene,NULL,settings));
  VoxelCellGeometry before[3];
  for(unsigned i=0;i<3;++i)assert(ActionVoxel_CellGeometry(base,126+i,17,&before[i]));
  assert(fabsf(before[0].front[0]-.05f)<.000001f && fabsf(before[0].back[0]-.125f)<.000001f);
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  terrain.voxel.cells[0]=(DioramaVoxelCell){.x=126,.y=17,.w=3,.h=1,.fields=kDioramaVoxelCell_Bend,
      .bend={.axis=kDioramaVoxelBend_X,.anchor=2064,.tip=2016,.dx=12,.dy=-20,.dz=-.2f}};
  assert(ActionVoxel_Build(edited,scene,&terrain,settings));
  unsigned shared=0;
  for(unsigned i=0;i<3;++i) {
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,126+i,17);assert(a);
    assert(!memcmp(a->front,before[i].front,sizeof(a->front)) && !memcmp(a->back,before[i].back,sizeof(a->back)));
    const unsigned mask=(1u<<1)|(1u<<4)|(1u<<5)|(1u<<6);assert((a->automatic_art&mask)==mask);
    if(i==2) {
      float dx,dy,dz;ActionVoxel_AuthoredBend(a,2064,280,&dx,&dy,&dz);assert(dx==0 && dy==0 && dz==0);
    } else {
      const VoxelAuthoredCell *b=ActionVoxel_AuthoredCellAt(edited,127+i,17);assert(b);
      for(int y=0;y<16;++y)if((a->pixels[0][y*16+15]&b->pixels[0][y*16])&0x8000) {
        assert(!ActionVoxel_AuthoredContourExposed(edited,a,15,y,1));++shared;
      }
    }
  }
  assert(shared);
  const VoxelAuthoredCell *tip=ActionVoxel_AuthoredCellAt(edited,126,17);float dx,dy,dz;
  ActionVoxel_AuthoredBend(tip,2016,280,&dx,&dy,&dz);assert(dx==12 && dy==-20 && dz==-.2f);
  ActionVoxel_Release(base);ActionVoxel_Release(edited);
  puts("authored branch: native .05/.125 thickness, opaque Auto cap contract, shared bend joins and fixed anchor passed");
}
static void AuthoredStumpIdentity(const ActionRoomScene *scene,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *edited,DioramaRoomOverride *terrain,const VoxelPatch *stump,
    int x,int y,ActionVoxelSettings settings) {
  memset(terrain,0,sizeof(*terrain));terrain->voxel.cell_count=1;
  DioramaVoxelCell *edit=&terrain->voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Transform,
      .transform={.px=stump->x+16,.py=stump->y+56,.pz=.09f,.sx=1,.sy=1,.sz=1}};
  const uint64_t previous=ActionVoxel_Revision(edited);
  assert(!ActionVoxel_Build(edited,scene,terrain,settings));
  assert(strstr(ActionVoxel_BuildError(edited),"complete classified object"));
  assert(ActionVoxel_Revision(edited)==previous); /* Identity still validates membership. */
  edit->w=2;edit->h=4;
  static uint32_t source[64*96],rear[64*96],front[64*96],base_rear[64*96],base_front[64*96];
  for(unsigned i=0;i<64*96;++i)source[i]=i%7?0xff203040u+i:0;
  for(unsigned mode=0;mode<3;++mode) {
    edit->fields=kDioramaVoxelCell_Transform|(mode?kDioramaVoxelCell_Faces:0);
    edit->faces=mode==2?kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front:kDioramaVoxelFaceMask_All;
    assert(ActionVoxel_Build(edited,scene,terrain,settings));SameGeometry(base,edited);
    assert(!ActionVoxel_HasNativeTransforms(edited) && !ActionVoxel_HasTransformedBacks(edited));
    assert(ActionVoxel_HasHiddenNativeFronts(edited)==(mode==2));
    unsigned caps;ActionVoxel_AuthoredCapArt(edited,&caps);assert(!caps);
    VoxelGroupCell cells[kVoxelSemanticGroupMaxCells];VoxelSemanticGroup group;
    assert(ActionVoxel_SemanticGroup(edited,x,y,&group,cells,kVoxelSemanticGroupMaxCells)==kActionVoxelGroup_Complete);
    assert(group.count==8);
    for(unsigned cy=0;cy<4;++cy)for(unsigned cx=0;cx<2;++cx) {
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,x+cx,y+cy);assert(a);
      assert(a->native_transform_requested && !a->native_transform && !a->surface && !a->contour_count);
      assert(a->native_front_hidden==(mode==2) && !memcmp(&a->transform,&edit->transform,sizeof(a->transform)));
      assert(!ActionVoxel_TransformUnsupportedReason(edited,x+cx,y+cy));
      for(unsigned row=0;row<16;++row)assert(!a->native_rear_rows[row]);
      VoxelCellGeometry info;assert(ActionVoxel_CellGeometry(edited,x+cx,y+cy,&info));
      assert(!memcmp(&info.transform,&edit->transform,sizeof(info.transform)));
    }
    for(unsigned fade=0;fade<2;++fade) {
      const ActionRoomSceneFrameState frame={.brightness=fade?7:15};
      assert(ActionVoxel_Split(base,(x-1)*16,(y-1)*16,64,96,64,&frame,source,64,base_rear,base_front));
      assert(ActionVoxel_Split(edited,(x-1)*16,(y-1)*16,64,96,64,&frame,source,64,rear,front));
      assert(!memcmp(front,base_front,sizeof(front)) && !memcmp(rear,base_rear,sizeof(rear)));
    }
  }
  /* Other face masks retain the owned-face path, so an identity pivot cannot
   * accidentally make a requested cap hide ineffective. */
  edit->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
  assert(ActionVoxel_Build(edited,scene,terrain,settings));
  assert(ActionVoxel_HasNativeTransforms(edited));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,x,y);
  assert(a && a->native_transform && !(a->faces&kDioramaVoxelFaceMask_Top));
  /* The retained group pivot can subsequently move the exact same object. */
  edit->faces=kDioramaVoxelFaceMask_All;edit->transform.dx=16;
  assert(ActionVoxel_Build(edited,scene,terrain,settings));
  a=ActionVoxel_AuthoredCellAt(edited,x,y);
  assert(a && a->native_transform_requested && a->native_transform && a->transform.dx==16);
  assert(ActionVoxel_HasNativeTransforms(edited) && ActionVoxel_HasTransformedBacks(edited));
}
static void AuthoredStumpFixture(const char *path) {
  if(!path)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
  assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
  static ActionRoomScene scene;static DioramaRoomOverride terrain;
  for(unsigned variant=0;variant<2;++variant) {
    scene=snapshot.scene;if(variant)Renumber(&scene);
    scene.have_extra_characters=true;memset(scene.extra_characters,0,32);
    for(unsigned y=0;y<8;++y)scene.extra_characters[y*2]=255;
    scene.palette[2]=31;scene.palette[3]=0;
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,1);
    assert(ActionVoxel_Build(base,&scene,NULL,settings));
    unsigned count;const VoxelPatch *patches=ActionVoxel_Patches(base,&count);VoxelPatch stump={0};
    for(unsigned i=0;i<count;++i)if(patches[i].kind==kVoxelPatch_SideProfile && patches[i].width==32 && patches[i].height==56){stump=patches[i];break;}
    assert(stump.width);const int sx=stump.x+9,sy=stump.y+7;
    if(!variant)printf("authored stump regression: left cell=%d,%d right=%d,%d native donor=%d,%d size4\n",sx/16,sy/16,sx/16+1,sy/16,sx,sy);
    AuthoredStumpIdentity(&scene,base,edited,&terrain,&stump,sx/16,sy/16,settings);
    for(unsigned kind=0;kind<8;++kind) {
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      DioramaVoxelCell *edit=&terrain.voxel.cells[0];edit->x=sx/16;edit->y=sy/16;
      switch(kind) {
        case 0:edit->fields=kDioramaVoxelCell_TopArt;edit->top_art=AuthoredTestArt(512);break;
        case 1:edit->fields=kDioramaVoxelCell_TopArt;break; /* explicit Auto */
        case 2:edit->fields=kDioramaVoxelCell_Front;edit->front=.03f;break;
        case 3:edit->fields=kDioramaVoxelCell_Width;edit->width=8;break;
        case 4:edit->fields=kDioramaVoxelCell_SideArt;edit->side_art=AuthoredTestArt(512);break;
        case 5:edit->fields=kDioramaVoxelCell_FrontArt;edit->front_art=AuthoredTestArt(512);break;
        case 6:edit->fields=kDioramaVoxelCell_Faces;edit->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front;break;
        case 7:edit->fields=kDioramaVoxelCell_Transform;
          edit->transform=(DioramaVoxelTransform){.px=stump.x+16,.py=stump.y+28,.pz=.15f,
              .dx=24,.dy=-8,.dz=-.1f,.sx=.5f,.sy=2,.sz=1.5f};break;
      }
      if(kind==7) {
        const uint64_t previous=ActionVoxel_Revision(edited);
        assert(!ActionVoxel_Build(edited,&scene,&terrain,settings));
        assert(strstr(ActionVoxel_BuildError(edited),"complete classified object"));
        assert(ActionVoxel_Revision(edited)==previous);
        edit->w=2;edit->h=4; /* All eight native stump cells share one pivot. */
      }
      assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
      VoxelCellGeometry right_before,right_after;
      const int neighbour=sx/16+(kind==7?2:1);
      assert(ActionVoxel_CellGeometry(base,neighbour,sy/16,&right_before));
      assert(ActionVoxel_CellGeometry(edited,neighbour,sy/16,&right_after));
      assert(!memcmp(&right_before,&right_after,sizeof(right_before)));
      if(kind==6) {
        /* A visibility-only front hide retains native semantic side/cap
         * geometry and its source texture; no donor rescue is necessary. */
        const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,sx/16,sy/16);
        assert(a && a->native_front_hidden && !a->surface);SameGeometry(base,edited);
        continue;
      }
      if(kind==7) {
        const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,sx/16,sy/16);
        assert(a && a->native_transform && a->native_front_hidden && a->native_front_art && !a->surface && !a->contour_count);
        assert(ActionVoxel_HasNativeTransforms(edited));
        unsigned count,owned=0;const VoxelEdge *edges=ActionVoxel_Edges(edited,&count);
        for(unsigned i=0;i<count;++i)if(edges[i].source_owned_art[0]) {
          assert(edges[i].length<=8 && edges[i].source_owned_art[1]);
          assert(ActionVoxel_AuthoredArt(edited,edges[i].source_owned_art[0]-1));
          assert(ActionVoxel_AuthoredArt(edited,edges[i].source_owned_art[1]-1));++owned;
        }
        assert(owned);
        /* Front artwork remains available to adjoining cap donors. Native
         * rear copies move with the object; independent rear pixels remain. */
        uint32_t source[256],base_rear[256],base_front[256],rear[256],front[256];
        for(unsigned i=0;i<256;++i)source[i]=0xff203040u+i;
        const ActionRoomSceneFrameState frame={.brightness=15};
        assert(ActionVoxel_Split(base,sx/16*16,sy/16*16,16,16,16,&frame,source,16,base_rear,base_front));
        assert(ActionVoxel_Split(edited,sx/16*16,sy/16*16,16,16,16,&frame,source,16,rear,front));
        assert(!memcmp(front,base_front,sizeof(front)));
        for(unsigned i=0;i<256;++i) {
          if(a->native_rear_rows[i/16]&(1u<<(15-i%16)))assert(rear[i]!=source[i]);
          else assert(rear[i]==base_rear[i]);
        }
        /* Local artwork does not remove a transformed member from its
         * source group. A different token or backing-only promotion does. */
        terrain.voxel.cell_count=2;terrain.voxel.cells[1]=*edit;
        terrain.voxel.cells[1].x++;terrain.voxel.cells[1].w=terrain.voxel.cells[1].h=1;
        terrain.voxel.cells[1].fields|=kDioramaVoxelCell_TopArt;
        terrain.voxel.cells[1].top_art=AuthoredTestArt(512);
        assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
        const VoxelAuthoredCell *mixed=ActionVoxel_AuthoredCellAt(edited,sx/16+1,sy/16);
        assert(mixed && mixed->surface && !mixed->native_transform && mixed->native_front_art);
        unsigned mixed_owned=0;
        assert(ActionVoxel_Split(edited,mixed->x,mixed->y,16,16,16,&frame,source,16,rear,front));
        for(unsigned i=0;i<256;++i)if(mixed->native_rear_rows[i/16]&(1u<<(15-i%16))) {
          ++mixed_owned;assert(rear[i]!=source[i]);
        }
        assert(mixed_owned); /* A top-art touchup must not leave its old rear copy. */
        terrain.voxel.cells[1].fields|=kDioramaVoxelCell_Backing;
        terrain.voxel.cells[1].backing=kDioramaVoxelBacking_Replace;
        assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
        mixed=ActionVoxel_AuthoredCellAt(edited,sx/16+1,sy/16);
        for(unsigned y=0;y<16;++y)assert(!mixed->native_rear_rows[y]);
        terrain.voxel.cells[1].fields&=~kDioramaVoxelCell_Backing;
        assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
        const uint64_t previous=ActionVoxel_Revision(edited);
        terrain.voxel.cells[1].transform.dx++;
        assert(!ActionVoxel_Build(edited,&scene,&terrain,settings));
        assert(strstr(ActionVoxel_BuildError(edited),"one shared transform"));
        assert(ActionVoxel_Revision(edited)==previous);
        terrain.voxel.cells[1].transform=edit->transform;
        terrain.voxel.cells[1].fields=kDioramaVoxelCell_Transform|kDioramaVoxelCell_Backing;
        terrain.voxel.cells[1].backing=kDioramaVoxelBacking_Fill;
        assert(!ActionVoxel_Build(edited,&scene,&terrain,settings));
        assert(strstr(ActionVoxel_BuildError(edited),"backing-only"));
        assert(ActionVoxel_Revision(edited)==previous);
        terrain.voxel.cell_count=1;
        continue;
      }
      /* The donor's original flat artwork remains suppressed. */
      uint8_t mask[8]={0};assert(ActionVoxel_CutoutRows(edited,sx/8,sy/8,mask));
      for(unsigned y=0;y<8;++y)assert(mask[y]==255);
      unsigned before_count,after_count,matched=0;
      const VoxelEdge *before=ActionVoxel_Edges(base,&before_count),*after=ActionVoxel_Edges(edited,&after_count);
      for(unsigned i=0;i<before_count;++i) {
        const VoxelEdge *a=&before[i];if(a->top_sample_x!=sx || a->top_sample_y!=sy || !a->top_sample_size)continue;
        for(int x=a->x;x<a->x+a->length;++x)if(x>=stump.x+16) {
          const VoxelEdge *b=NULL;for(unsigned j=0;j<after_count;++j)
            if(after[j].side==a->side && after[j].y==a->y && x>=after[j].x && x<after[j].x+after[j].length){b=&after[j];break;}
          assert(b && b->top_owned_art && b->top_sample_size==4 && b->top_sample_x==sx && b->top_sample_y==sy);
          assert(b->near_rank==a->near_rank && b->far_rank==a->far_rank && b->high_priority==a->high_priority && b->colliding==a->colliding);
          const uint16_t *art=ActionVoxel_AuthoredArt(edited,b->top_owned_art-1);assert(art);
          for(int yy=0;yy<4;++yy)for(int xx=0;xx<4;++xx) {
            uint16_t word;assert(ActionRoomScene_LookupTile(&scene,1,(sx+xx)/8,(sy+yy)/8,&word,NULL));
            const unsigned pixel=FixturePixel(&scene,sx+xx,sy+yy),p=((word>>10)&7)*16+pixel;
            assert(pixel && art[yy*16+xx]==(uint16_t)(0x8000|scene.palette[p*2]|scene.palette[p*2+1]<<8));
            const ActionRoomSceneFrameState dark={.brightness=0};
            assert(ActionVoxel_AuthoredColor(art[yy*16+xx],&dark)==0xff000000);
          }
          ++matched;
        }
      }
      assert(matched);
    }
    terrain.voxel.cells[0].fields=0;assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
    unsigned owned;ActionVoxel_AuthoredCapArt(edited,&owned);assert(!owned);SameGeometry(base,edited);
    /* Shift the same native 2x4-cell profile across both negative axes. The
     * donor straddles Y=-1/0, so truncation toward zero picks the wrong stamp. */
    DioramaTileStamp stamps[24];memset(stamps,0,sizeof(stamps));unsigned stamp_count=0;
    for(int y=-2;y<4;++y)for(int x=-2;x<2;++x) {
      DioramaTileStamp *stamp=&stamps[stamp_count++];stamp->x=x;stamp->y=y;stamp->blank=true;
      if(x>=-1 && x<=0 && y>=-1 && y<=2) {
        stamp->blank=false;
        for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&scene,1,
            (sx/16+x+1)*2+(q&1),(sy/16+y+1)*2+(q>>1),&stamp->words[q],&stamp->metatile));
      }
    }
    memset(&terrain,0,sizeof(terrain));
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-2,.x1=2,.y1=4,
        .cells=stamps,.count=stamp_count};
    terrain.voxel.cell_count=1;terrain.voxel.cells[0]=(DioramaVoxelCell){.x=-1,.y=-1,
        .fields=kDioramaVoxelCell_TopArt,.top_art=AuthoredTestArt(512)};
    assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
    unsigned negative_edges,negative_caps=0;const VoxelEdge *negative=ActionVoxel_Edges(edited,&negative_edges);
    for(unsigned i=0;i<negative_edges;++i)if(negative[i].top_owned_art) {
      assert(negative[i].top_sample_x==-7 && negative[i].top_sample_y==-1);
      const uint16_t *art=ActionVoxel_AuthoredArt(edited,negative[i].top_owned_art-1);
      for(int yy=0;yy<4;++yy)for(int xx=0;xx<4;++xx) {
        uint16_t word;assert(ActionRoomScene_LookupTile(&scene,1,(sx+xx)/8,(sy+yy)/8,&word,NULL));
        const unsigned pixel=FixturePixel(&scene,sx+xx,sy+yy),p=((word>>10)&7)*16+pixel;
        assert(art[yy*16+xx]==(uint16_t)(0x8000|scene.palette[p*2]|scene.palette[p*2+1]<<8));
      }
      ++negative_caps;
    }
    assert(negative_caps);
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
  puts("authored stump: eight edit types preserve neighbouring cap geometry, owned affine sources, native donor, priority, fades and reset (native + renumbered)");
}
/* Optional local ROM-derived fixture; no game artwork is stored in the test. */
static void AuthoredFixture(const char *path) {
  if(!path)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  static ActionSceneSnapshot snapshot;
  FILE *file=fopen(path,"rb");assert(file);
  const size_t size=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));fclose(file);
  assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
  ActionRoomScene *s=&snapshot.scene;
  /* Synthetic donor uses the separate native CHR bank and a cropped square.
   * It is deliberately not placed anywhere in the room or camera capture. */
  s->have_extra_characters=true;memset(s->extra_characters,0,32);
  for(unsigned y=0;y<8;++y)s->extra_characters[y*2]=255;
  s->palette[2]=31;s->palette[3]=0;
  ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
  assert(ActionVoxel_Build(base,s,NULL,settings));
  assert(ActionVoxel_Active(base,s->group,s->map));
  VoxelCellGeometry before,after;assert(ActionVoxel_CellGeometry(base,0,0,&before));
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  terrain.voxel.cell_count=1;
  DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|
      kDioramaVoxelCell_Width|kDioramaVoxelCell_Offset|kDioramaVoxelCell_Backing|
      kDioramaVoxelCell_BackArt|kDioramaVoxelCell_TopArt|kDioramaVoxelCell_SideArt,
      .front=.03f,.back=.3f,.width=8,.offset_x=2,.backing=kDioramaVoxelBacking_Replace};
  edit->top_art=(DioramaVoxelArt){.words={512,512,512,512},.bg=1,.x=8,.y=8,.size=8,.set=true};
  edit->side_art=edit->backing_art=edit->top_art;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_CellGeometry(edited,0,0,&after));
  assert(after.authored && after.width==8 && after.offset_x==2);
  unsigned count;const VoxelAuthoredCell *cells=ActionVoxel_AuthoredCells(edited,&count);
  assert(count==1 && cells[0].surface && ActionVoxel_AuthoredSurfaceAt(edited,15,15));
  assert(!ActionVoxel_AuthoredSurfaceAt(edited,16,15));
  for(unsigned q=0;q<4;++q) {
    uint16_t word;assert(ActionRoomScene_LookupTile(s,1,q&1,q>>1,&word,NULL));
    assert(cells[0].priority[q]==!!(word&0x2000));
    assert(after.front[q]==.03f && after.back[q]==.3f);
  }
  for(unsigned i=0;i<256;++i)assert(cells[0].pixels[1][i]==0x801f &&
      cells[0].pixels[2][i]==0x801f && cells[0].pixels[3][i]==0x801f);
  uint8_t masks[8]={0};assert(ActionVoxel_CutoutRows(edited,0,0,masks));
  for(unsigned i=0;i<8;++i)assert(masks[i]==255);
  uint32_t low[256],rear[256],front[256];for(unsigned i=0;i<256;++i)low[i]=0xffffffff;
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(edited,0,0,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i)assert(!front[i] && rear[i]==0xffff0000);
  /* Backing repair alone must retain every semantic front depth/contour and
   * native source pixel; it does not turn the tile into an authored prism. */
  edit->fields=kDioramaVoxelCell_Backing|kDioramaVoxelCell_BackArt;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_CellGeometry(edited,0,0,&after));
  assert(!ActionVoxel_AuthoredSurfaceAt(edited,0,0));
  assert(!memcmp(before.front,after.front,sizeof(before.front)) &&
      !memcmp(before.back,after.back,sizeof(before.back)) && after.width==16 && after.offset_x==0);
  unsigned a_count,b_count;
  const VoxelEdge *a_edges=ActionVoxel_Edges(base,&a_count),*b_edges=ActionVoxel_Edges(edited,&b_count);
  assert(a_count==b_count && !memcmp(a_edges,b_edges,a_count*sizeof(*a_edges)));
  size_t animation_offset,animation_bytes;
  if(ActionVoxel_AnimationMatches(s,s->characters,&animation_offset,&animation_bytes)) {
    edit->fields|=kDioramaVoxelCell_TopArt;
    for(unsigned q=0;q<4;++q)edit->top_art.words[q]=animation_offset/32;
    assert(!ActionVoxel_AnimationIsPresentationOnly(s,s,&terrain,NULL,0));
  }
  edit->fields=0;assert(ActionVoxel_Build(edited,s,&terrain,settings));
  SameGeometry(base,edited);ActionVoxel_AuthoredCells(edited,&count);assert(!count);
  /* A large compact region uses deduplicated native donors. Adjacent curve
   * cells meet exactly and have no buried caps at their internal boundaries. */
  for(unsigned character=0;character<3;++character) {
    uint8_t *chr=s->extra_characters+character*32;memset(chr,0,32);
    for(unsigned y=0;y<8;++y){chr[y*2]=(character+1)&1?255:0;chr[y*2+1]=(character+1)&2?255:0;}
  }
  s->palette[4]=0xe0;s->palette[5]=3;s->palette[6]=0;s->palette[7]=0x7c;
  *edit=(DioramaVoxelCell){.w=8,.h=8,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|
      kDioramaVoxelCell_FrontArt|kDioramaVoxelCell_TopArt|kDioramaVoxelCell_SideArt|
      kDioramaVoxelCell_LeftArt|kDioramaVoxelCell_RightArt|kDioramaVoxelCell_BottomArt|
      kDioramaVoxelCell_Backing|kDioramaVoxelCell_Shape|kDioramaVoxelCell_FrontEnd|kDioramaVoxelCell_BackEnd,
      .front=.02f,.back=.3f,.front_end=.2f,.back_end=.4f,.shape=kDioramaVoxelShape_Curve,
      .shape_y=0,.shape_h=128,.backing=kDioramaVoxelBacking_None,
      .front_art=AuthoredTestArt(512),.top_art=AuthoredTestArt(513),.side_art=AuthoredTestArt(514),
      .left_art=AuthoredTestArt(513),.bottom_art=AuthoredTestArt(512)};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  cells=ActionVoxel_AuthoredCells(edited,&count);assert(count==64);
  assert(ActionVoxel_AuthoredArtCount(edited)==4); /* RGB donors plus transparent rear. */
  const VoxelAuthoredCell *upper=ActionVoxel_AuthoredCellAt(edited,0,0),*lower=ActionVoxel_AuthoredCellAt(edited,0,1);
  assert(upper && lower && !ActionVoxel_AuthoredCellAt(edited,8,0));
  assert(!upper->native_front_art && !lower->native_front_art); /* Explicit donors retain authored filtering. */
  assert(upper->pixels[0][0]==0x801f && upper->pixels[1][0]==0x83e0 &&
      upper->pixels[4][0]==0x83e0 && upper->pixels[5][0]==0xfc00 && upper->pixels[6][0]==0x801f);
  assert(!(upper->automatic_art&((1u<<1)|(1u<<4)|(1u<<5)|(1u<<6))));
  for(int i=0;i<16;++i) {
    assert(!ActionVoxel_AuthoredContourExposed(edited,upper,i,15,2));
    assert(!ActionVoxel_AuthoredContourExposed(edited,lower,i,0,0));
    assert(!ActionVoxel_AuthoredContourExposed(edited,upper,15,i,1));
    assert(ActionVoxel_AuthoredContourExposed(edited,upper,i,0,0));
    float a,b,c,d;ActionVoxel_AuthoredDepths(upper,i/8+2,16,&a,&b);ActionVoxel_AuthoredDepths(lower,i/8,16,&c,&d);
    assert(a==c && b==d);
  }
  unsigned contours;const VoxelAuthoredContour *outline=ActionVoxel_AuthoredContours(edited,upper,&contours);
  assert(outline && contours==4); /* two 8px top runs and two left runs. */
  /* The donor's transparent pixels become real openings, with side walls at
   * the alpha boundary rather than a rectangular solid around the tile. */
  memset(s->extra_characters+3*32,0,32);
  for(unsigned y=0;y<8;++y)s->extra_characters[3*32+y*2]=0xf0;
  edit->front_art=AuthoredTestArt(515);
  assert(ActionVoxel_Build(edited,s,&terrain,settings));upper=ActionVoxel_AuthoredCellAt(edited,0,0);
  assert(upper->pixels[0][3]==0x801f && upper->pixels[0][4]==0);
  assert(ActionVoxel_AuthoredContourExposed(edited,upper,3,4,1));
  assert(!ActionVoxel_AuthoredContourExposed(edited,upper,4,4,1));
  /* A deliberately selected donor retains its alpha. Auto caps instead use
   * the opaque front contour; directional Auto still inherits shared Sides. */
  const DioramaVoxelArt top=edit->top_art,side=edit->side_art,left=edit->left_art,bottom=edit->bottom_art;
  edit->top_art.set=edit->side_art.set=edit->left_art.set=edit->bottom_art.set=false;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));upper=ActionVoxel_AuthoredCellAt(edited,0,0);
  const unsigned auto_caps=(1u<<1)|(1u<<4)|(1u<<5)|(1u<<6);
  assert((upper->automatic_art&auto_caps)==auto_caps);
  assert(upper->pixels[0][3]==0x801f && upper->pixels[0][4]==0);
  edit->side_art=side;assert(ActionVoxel_Build(edited,s,&terrain,settings));upper=ActionVoxel_AuthoredCellAt(edited,0,0);
  assert((upper->automatic_art&auto_caps)==(1u<<1));
  edit->top_art=top;edit->left_art=left;edit->bottom_art=bottom;
  edit->front_art=AuthoredTestArt(512);
  /* Erasing front artwork is independent of erasing the front face. Hiding
   * that face keeps the volume silhouette and requested rear repair. */
  edit->fields|=kDioramaVoxelCell_Faces|kDioramaVoxelCell_BackArt;
  edit->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front;
  edit->backing=kDioramaVoxelBacking_Replace;edit->backing_art=AuthoredTestArt(513);
  assert(ActionVoxel_Build(edited,s,&terrain,settings));upper=ActionVoxel_AuthoredCellAt(edited,0,0);
  assert(upper->pixels[0][0]==0x801f && upper->pixels[3][0]==0x83e0);
  assert(!(upper->faces&kDioramaVoxelFaceMask_Front) && upper->contour_count==4);
  edit->faces=0;assert(ActionVoxel_Build(edited,s,&terrain,settings));
  upper=ActionVoxel_AuthoredCellAt(edited,0,0);assert(!upper->pixels[3][0] && !upper->faces);
  /* A one-cell reset punctures the region and exposes the adjoining cap;
   * replacing that cell with matching shape closes it again. */
  edit->faces=kDioramaVoxelFaceMask_All;terrain.voxel.cell_count=2;
  terrain.voxel.cells[1]=(DioramaVoxelCell){.x=1,.y=0};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));upper=ActionVoxel_AuthoredCellAt(edited,0,0);
  assert(!ActionVoxel_AuthoredCellAt(edited,1,0) && ActionVoxel_AuthoredContourExposed(edited,upper,15,4,1));
  terrain.voxel.cells[1]=*edit;terrain.voxel.cells[1].x=1;terrain.voxel.cells[1].w=terrain.voxel.cells[1].h=1;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));upper=ActionVoxel_AuthoredCellAt(edited,0,0);
  assert(!ActionVoxel_AuthoredContourExposed(edited,upper,15,4,1));
  terrain.voxel.cells[1].fields|=kDioramaVoxelCell_Width;terrain.voxel.cells[1].width=8;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));upper=ActionVoxel_AuthoredCellAt(edited,0,0);
  assert(ActionVoxel_AuthoredContourExposed(edited,upper,15,4,1));
  /* Failed edits leave the last complete compiled geometry and its revision
   * intact, with a concrete error for Preview/Apply. */
  const uint64_t revision=ActionVoxel_Revision(edited);edit->shape=255;
  assert(!ActionVoxel_Build(edited,s,&terrain,settings));assert(ActionVoxel_Revision(edited)==revision);
  assert(strstr(ActionVoxel_BuildError(edited),"invalid"));
  edit->shape=kDioramaVoxelShape_Curve;
  /* Fill preserves opaque native backing; Replace paints over it. Search a
   * native room region instead of relying on a fragile metatile identifier. */
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.w=32,.h=32,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_Auto};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));cells=ActionVoxel_AuthoredCells(edited,&count);
  int found=-1,texel=-1;uint16_t original=0;
  for(unsigned i=0;i<count && found<0;++i)for(unsigned p=0;p<256;++p)
    if((cells[i].pixels[3][p]&0x8000) && cells[i].pixels[3][p]!=0x801f){found=i;texel=p;original=cells[i].pixels[3][p];break;}
  assert(found>=0);edit->x=cells[found].x/16;edit->y=cells[found].y/16;edit->w=edit->h=1;
  edit->fields|=kDioramaVoxelCell_BackArt;edit->backing_art=AuthoredTestArt(512);edit->backing=kDioramaVoxelBacking_Fill;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));upper=ActionVoxel_AuthoredCellAt(edited,edit->x,edit->y);
  assert(upper->pixels[3][texel]==original);
  edit->backing=kDioramaVoxelBacking_Replace;assert(ActionVoxel_Build(edited,s,&terrain,settings));
  upper=ActionVoxel_AuthoredCellAt(edited,edit->x,edit->y);assert(upper->pixels[3][texel]==0x801f);
  uint16_t original_art[256];uint8_t original_priority[4];
  memcpy(original_art,upper->pixels[0],sizeof(original_art));
  memcpy(original_priority,upper->priority,sizeof(original_priority));
  edit->fields|=kDioramaVoxelCell_Bend;
  edit->bend=(DioramaVoxelBend){.axis=kDioramaVoxelBend_X,.anchor=edit->x*16,
      .tip=(edit->x+1)*16,.dx=24,.dy=-12,.dz=.2f};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  upper=ActionVoxel_AuthoredCellAt(edited,edit->x,edit->y);
  assert(!memcmp(original_art,upper->pixels[0],sizeof(original_art)));
  assert(!memcmp(original_priority,upper->priority,sizeof(original_priority)));
  VoxelCellGeometry bent;assert(ActionVoxel_CellGeometry(edited,edit->x,edit->y,&bent));
  assert(bent.bend.axis==kDioramaVoxelBend_X && bent.bend.anchor==edit->x*16);
  assert(bent.bend.dx==24 && bent.bend.dy==-12 && bent.bend.dz==.2f);
  const uint64_t bent_revision=ActionVoxel_Revision(edited);
  edit->bend.tip=edit->bend.anchor;
  assert(!ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_Revision(edited)==bent_revision);
  ActionVoxel_Release(base);ActionVoxel_Release(edited);
  puts("authored voxel: native donors, independent faces, hidden skins, compact regions, connected curves, deduplication, backing modes and transactional failure passed");
}
