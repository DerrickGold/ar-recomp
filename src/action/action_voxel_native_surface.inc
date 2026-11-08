/* Compile-only native inward surface pixels. The caller owns semantic source
 * qualification and actual exposed-edge coordinates; this routine owns only
 * atlas orientation and bounded padding. No geometry or palette synthesis. */
typedef struct VoxelNativeSurfaceColumn {
  uint16_t colors[16];
  uint8_t length;
} VoxelNativeSurfaceColumn;
static bool NativeSurfacePack(const VoxelNativeSurfaceColumn columns[16],
    unsigned first,unsigned count,unsigned side,unsigned extent,bool hold,uint16_t pixels[256]) {
  if(!columns || !pixels || side>3 || first>=16 || !count || count>16-first ||
      !extent || extent>16)return false;
  for(unsigned p=first;p<first+count;++p) {
    if(!columns[p].length || columns[p].length>extent || (!hold && columns[p].length!=extent))return false;
    for(unsigned d=0;d<columns[p].length;++d)
      if(!(columns[p].colors[d]&0x8000))return false;
  }
  for(unsigned p=0;p<16;++p) {
    /* Unused along-axis columns are filtering padding, never receivers.
     * Their nearest original boundary trace prevents interpolation with a
     * transparent unrelated column at an atlas fragment edge. */
    const unsigned source=p<first?first:p>=first+count?first+count-1:p;
    const VoxelNativeSurfaceColumn *column=&columns[source];
    for(unsigned d=0;d<16;++d) {
      const unsigned requested=(d*(extent-1)+7)/15;
      const unsigned inward=requested<column->length?requested:column->length-1;
      const unsigned across=(side==1 || side==2)?15-d:d;
      pixels[(side&1)?p*16+across:across*16+p]=column->colors[inward];
    }
  }
  return true;
}

/* A reviewed real native prefix may be traversed once over existing depth.
 * Its caller must positively qualify every short endpoint; an edit, unknown
 * source or missing data must never be mistaken for native termination. */
static bool NativeSurfacePackPrefix(const VoxelNativeSurfaceColumn columns[16],
    const bool terminal[16],unsigned first,unsigned count,unsigned side,
    unsigned extent,uint16_t pixels[256]) {
  if(!columns || !terminal || !extent || extent>16 || first>=16 || !count || count>16-first)return false;
  VoxelNativeSurfaceColumn expanded[16]={0};
  for(unsigned p=first;p<first+count;++p) {
    const unsigned length=columns[p].length;
    if(!length || length>extent || (length<extent && !terminal[p]))return false;
    expanded[p].length=16;
    for(unsigned d=0;d<16;++d) {
      const unsigned at=(d*(length-1)+7)/15;
      if(!(columns[p].colors[at]&0x8000))return false;
      expanded[p].colors[d]=columns[p].colors[at];
    }
  }
  return NativeSurfacePack(expanded,first,count,side,16,false,pixels);
}
/* Keep Ice qualification and its eight-row contract unchanged. */
static bool NativeSurfacePackIceTip(const VoxelNativeSurfaceColumn columns[16],
    const bool terminal[16],unsigned first,unsigned count,unsigned side,uint16_t pixels[256]) {
  return NativeSurfacePackPrefix(columns,terminal,first,count,side,8,pixels);
}
