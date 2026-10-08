/* Procedural controller artwork. Render-neutral family/symbol snapshots use
 * the same texture path as other inline objects; no platform input is read here. */
enum { kButtonGlyphExtent = 64 };
static ArRenderTexture s_button_textures[kArButtonGlyphFamily_Count][kArButtonGlyphSymbol_Count];

static float ButtonStroke(float x,float y,float ax,float ay,float bx,float by) {
  return SquaredDistanceToSegment(x,y,ax,ay,bx,by);
}
static const unsigned char *ButtonLetter(char c) {
  static const unsigned char letters[][7] = {
    {14,17,17,31,17,17,17}, /* A */ {30,17,17,30,17,17,30}, /* B */
    {17,17,10,4,10,17,17}, /* X */ {17,17,10,4,4,4,4}, /* Y */
    {16,16,16,16,16,16,31}, /* L */ {30,17,17,30,20,18,17}, /* R */
    {31,1,2,4,8,16,31}, /* Z */ {31,4,4,4,4,4,4}, /* T */
    {4,12,4,4,4,4,14}, /* 1 */ {14,17,1,2,4,8,31}, /* 2 */
  };
  const char *order="ABXYLRZT12";
  const char *at=strchr(order,c);
  return at ? letters[at-order] : NULL;
}
static bool ButtonTextInk(const char *text,float x,float y) {
  const size_t length=strlen(text);
  const float unit=length>1 ? 0.062f : 0.085f;
  const float left=0.5f-(float)(6*length-1)*unit/2.0f, top=0.5f-3.5f*unit;
  for (size_t i=0;i<length;++i) {
    const unsigned char *rows=ButtonLetter(text[i]);
    if (!rows) continue;
    for (unsigned row=0;row<7;++row) for (unsigned col=0;col<5;++col)
      if ((rows[row] & (1u<<(4-col))) &&
          x>=left+(6*i+col)*unit && x<left+(6*i+col+1.0f)*unit &&
          y>=top+row*unit && y<top+(row+1.0f)*unit) return true;
  }
  return false;
}
static uint32_t ButtonPixel(ArButtonGlyph glyph,float x,float y) {
  const unsigned symbol=glyph.symbol;
  const float dx=x-0.5f,dy=y-0.5f;
  const bool face=symbol<=kArButtonGlyphSymbol_Triangle;
  const bool shoulder=symbol>=kArButtonGlyphSymbol_LB && symbol<=kArButtonGlyphSymbol_ZR;
  const float radius=face ? 0.46f : 0.44f;
  bool inside;
  if (shoulder) {
    const float ax=fabsf(dx)-0.35f,ay=fabsf(dy)-0.20f;
    const float cx=fmaxf(ax,0),cy=fmaxf(ay,0);
    inside=cx*cx+cy*cy<0.10f*0.10f;
  } else inside=dx*dx+dy*dy<radius*radius;
  if (!inside) return 0;
  uint32_t colour=0xff30343a;
  if (glyph.family==kArButtonGlyphFamily_Nintendo) colour=0xffdedfe3;
  if ((face && dx*dx+dy*dy>(radius-0.035f)*(radius-0.035f)) ||
      (shoulder && (fabsf(dx)>0.415f || fabsf(dy)>0.265f))) colour=0xff8b9098;
  uint32_t ink=glyph.family==kArButtonGlyphFamily_Nintendo ? 0xff252930 : 0xfff7f7fa;
  if (glyph.family==kArButtonGlyphFamily_Xbox && symbol<=kArButtonGlyphSymbol_Y) {
    static const uint32_t colours[]={0xff83d85a,0xffff655f,0xff6fb5ff,0xffffd95a}; ink=colours[symbol];
  }
  bool mark=false;
  if (symbol==kArButtonGlyphSymbol_Cross) {
    ink=0xff82baff;
    mark=ButtonStroke(x,y,.32f,.32f,.68f,.68f)<.0016f || ButtonStroke(x,y,.68f,.32f,.32f,.68f)<.0016f;
  } else if (symbol==kArButtonGlyphSymbol_Circle) {
    ink=0xffff817d; const float r=sqrtf(dx*dx+dy*dy); mark=fabsf(r-.235f)<.036f;
  } else if (symbol==kArButtonGlyphSymbol_Square) {
    ink=0xffe7a0dc; mark=fabsf(fmaxf(fabsf(dx),fabsf(dy))-.22f)<.032f;
  } else if (symbol==kArButtonGlyphSymbol_Triangle) {
    ink=0xff79debd;
    mark=ButtonStroke(x,y,.5f,.25f,.24f,.70f)<.0013f || ButtonStroke(x,y,.24f,.70f,.76f,.70f)<.0013f || ButtonStroke(x,y,.76f,.70f,.5f,.25f)<.0013f;
  } else if (symbol<=kArButtonGlyphSymbol_Y || shoulder || symbol>=kArButtonGlyphSymbol_LeftStick) {
    static const char *const words[]={"A","B","X","Y","","","","","LB","RB","LT","RT","L1","R1","L2","R2","L","R","ZL","ZR"};
    const char *word=symbol<20 ? words[symbol] : symbol==kArButtonGlyphSymbol_LeftStick ? "L" : "R";
    mark=ButtonTextInk(word,x,y);
    if (symbol>=kArButtonGlyphSymbol_LeftStick) mark=mark || fabsf(sqrtf(dx*dx+dy*dy)-.33f)<.025f;
  } else if (symbol>=kArButtonGlyphSymbol_Up && symbol<=kArButtonGlyphSymbol_Right) {
    if (symbol==kArButtonGlyphSymbol_Down) { x=1-x; y=1-y; }
    else if (symbol==kArButtonGlyphSymbol_Left) { float old=x; x=y; y=old; }
    else if (symbol==kArButtonGlyphSymbol_Right) { float old=x; x=y; y=1-old; }
    mark=ButtonStroke(x,y,.5f,.28f,.5f,.73f)<.0016f || ButtonStroke(x,y,.5f,.28f,.3f,.48f)<.0016f || ButtonStroke(x,y,.5f,.28f,.7f,.48f)<.0016f;
  } else if (symbol==kArButtonGlyphSymbol_Start) {
    mark=x>.29f && x<.71f && (fabsf(y-.34f)<.032f || fabsf(y-.5f)<.032f || fabsf(y-.66f)<.032f);
  } else if (symbol==kArButtonGlyphSymbol_Back) {
    mark=(x>.27f && x<.59f && y>.27f && y<.59f && (x<.30f || y<.30f || x>.56f || y>.56f)) ||
         (x>.42f && x<.74f && y>.42f && y<.74f && (x<.45f || y<.45f || x>.71f || y>.71f));
  } else {
    mark=ButtonStroke(x,y,.25f,.48f,.5f,.27f)<.0013f || ButtonStroke(x,y,.5f,.27f,.75f,.48f)<.0013f ||
         (x>.34f && x<.66f && y>.45f && y<.73f && (x<.38f || x>.62f || y>.69f));
  }
  return mark ? ink : colour;
}
static bool PrepareButtonTexture(ArRenderDevice *device,ArButtonGlyph glyph,ArRenderTexture *texture) {
  if (!ArButtonGlyph_IsValid(glyph)) return false;
  ArRenderTexture *cached=&s_button_textures[glyph.family][glyph.symbol];
  if (!ArRenderTexture_IsValid(*cached)) {
    uint32_t pixels[kButtonGlyphExtent*kButtonGlyphExtent];
    for (unsigned y=0;y<kButtonGlyphExtent;++y) for (unsigned x=0;x<kButtonGlyphExtent;++x) {
      unsigned alpha=0,red=0,green=0,blue=0;
      for (unsigned sy=0;sy<2;++sy) for (unsigned sx=0;sx<2;++sx) {
        const uint32_t p=ButtonPixel(glyph,(x+(sx+.5f)/2)/kButtonGlyphExtent,(y+(sy+.5f)/2)/kButtonGlyphExtent);
        if (p>>24) { ++alpha; red+=(p>>16)&255; green+=(p>>8)&255; blue+=p&255; }
      }
      pixels[y*kButtonGlyphExtent+x]=alpha ? ((alpha*255/4)<<24)|(red/alpha<<16)|(green/alpha<<8)|(blue/alpha) : 0;
    }
    const ArRenderTextureDesc desc={.width=kButtonGlyphExtent,.height=kButtonGlyphExtent,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Static,
      .filter=kArRenderFilter_Linear,.blend=kArRenderBlendMode_Alpha};
    if (!ArRenderDevice_CreateTexture(device,&desc,cached)) return false;
    if (!ArRenderDevice_UpdateTexture(device,*cached,NULL,pixels,kButtonGlyphExtent*sizeof(uint32_t))) {
      ArRenderDevice_DestroyTexture(device,*cached); *cached=ArRenderTexture_Invalid(); return false;
    }
  }
  *texture=*cached; return true;
}
