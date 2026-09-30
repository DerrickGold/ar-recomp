
/* ===========================================================================
 * Action Layer Editor
 *
 * WHAT THIS IS FOR
 * Some action rooms put every scenery element on one background plane, so the
 * Diorama 3D presentation has nothing to separate in depth and the room reads
 * flat. This tool tags each background tile with the depth band it should be
 * drawn in, and previews the result with an orbit camera.
 *
 * THE MODEL, AND WHY IT IS SHAPED THIS WAY
 * A band is not a new layer. It is ANCHORED to the BG it came from, so it
 * inherits that layer's camera and its live scroll deltas. Moving a tile to a
 * different BG instead would break exactly that: BG2 has its own scroll
 * registers and its own tilemap, and in a room where the engine never updates
 * BG2 a promoted tile would sit still while the rest of the room moved.
 *
 * Three bands per anchor:
 *   0 far virtual   a new surface whose z/order/alpha are authored here
 *   1 plane         the anchor's ordinary priority-0 surface (default)
 *   2 priority      the anchor's authentic priority-1 surface
 *
 * THE FILE CONTRACT
 * The editor owns bg1/bg1hi/bg2/bg2hi and bgN-virtual records in each base
 * [layers:GG:MM] section of diorama-layers.ini. Geometry is sparse; metatile
 * rules classify every instance and cell rectangles refine them, with the ROM
 * priority bit as fallback. Export replaces those owned records and preserves
 * all other comments, sections and plane settings verbatim.
 *
 * WHY THE PREVIEW IS BUILT THE WAY IT IS
 * The map editor keeps full-level surfaces for authoring. Native frame and
 * Diorama 3D instead sample exact 256x224 camera-local captures through the
 * shared raster model, then route every source pixel into its configured band.
 * That is deliberately the runtime shape: captured planes composited at depth.
 * Flat game order remains authentic and deliberately ignores virtual
 * classification; Diorama uses configured painter order and independent z.
 * ======================================================================== */

const DATA = window.__ACTION_BG__;
const TERRAIN_PROFILES = DATA.terrainProfiles || [{profile:0,label:'US'}];
let terrainProfile = TERRAIN_PROFILES[0].profile;
/* Rendering caches follow the selected terrain; authoring follows the
 * equivalent placed-terrain family for each background. */
function terrainRoom(base, profile = terrainProfile) {
  const variant = (base.terrainVariants || []).find(v => v.profile === profile);
  return variant ? {...base, terrainProfile:profile,
    bg:[variant.bg1,base.bg[1]], nativeGolden:variant.nativeGolden,
    changedCells:variant.changedCells, changedMetatiles:variant.changedMetatiles} : base;
}
const sceneKey = r => `${r.group}:${r.map}:${r.terrainProfile || 0}`;
const terrainLabel = r => (TERRAIN_PROFILES.find(
  p => p.profile === (r.terrainProfile || 0)) || TERRAIN_PROFILES[0]).label;
/* These are diorama.c's real defaults, including its paint slots. INI values
 * refine them per room. `order` and `z` remain independent because the runtime
 * is a painter: z changes projection/focus while order decides overlap. */
const PLANE_DEFAULTS = {
  backdrop:{z:0.00,order:0,alpha:255}, obj0:{z:0.51,order:1,alpha:255},
  obj1:{z:0.51,order:2,alpha:255}, bg2:{z:0.20,order:3,alpha:255},
  bg1:{z:0.50,order:4,alpha:255}, obj2:{z:0.51,order:5,alpha:255},
  bg2hi:{z:0.21,order:6,alpha:255}, bg1hi:{z:0.51,order:7,alpha:255},
  obj3:{z:0.52,order:8,alpha:255}, bg3:{z:0.95,order:9,alpha:255},
};
const PLANE_TOKENS = [
  'backdrop','bg1','bg1hi','bg2','bg2hi','bg3','obj0','obj1','obj2','obj3',
];
const EDITABLE_PLANE_TOKENS = new Set(['bg1','bg1hi','bg2','bg2hi']);
const STRATEGIES = ['flat','rake','bow','thick','stack','voxel'];
const STACK_DIRECTIONS = ['forward','backward','both'];
/* The new far planes default 0.15 behind their anchors and share the anchor's
 * paint key, inserted immediately before it. Authoring z/order/alpha on a
 * bgN-virtual record overrides these defaults. */
const VIRTUAL_DEFAULTS = [
  {z:0.35,order:PLANE_DEFAULTS.bg1.order,alpha:255},
  {z:0.05,order:PLANE_DEFAULTS.bg2.order,alpha:255},
];
const BANDS = [
  { key:'far',   name:'Far virtual plane',css:'--behind' },
  { key:'plane', name:'Plane (default)',  css:'--plane' },
  { key:'high',  name:'Priority band',    css:'--ahead' },
];
const bandZ = (bg, b) => b === 0 ? resolvedVirtual(bg).z
  : resolvedPlane(bg === 0 ? (b === 1 ? 'bg1' : 'bg1hi')
                           : (b === 1 ? 'bg2' : 'bg2hi')).z;
const bandAlpha = (bg, b) => b === 0 ? resolvedVirtual(bg).alpha
  : resolvedPlane(bg === 0 ? (b === 1 ? 'bg1' : 'bg1hi')
                           : (b === 1 ? 'bg2' : 'bg2hi')).alpha;
const bandPaintOrder = (bg,b) => b === 0 ? resolvedVirtual(bg).order
  : resolvedPlane(bg === 0 ? (b === 1 ? 'bg1' : 'bg1hi')
                           : (b === 1 ? 'bg2' : 'bg2hi')).order;

/* Every plane in the scene, sorted, with anything that collides flagged. This
 * is the answer to "will my BG1 far plane clip through BG2?" -- two
 * planes closer than this cannot be relied on to order correctly, and at equal
 * depth the winner is undefined. */
const kClashEpsilon = 0.02;
function depthLadder() {
  const rows = [], clashPairs = [];
  for (const bg of [0,1]) {
    if (!room.bg[bg]) continue;
    for (let b=0; b<3; b++)
      rows.push({ z: bandZ(bg, b), label: `BG${bg+1} ${BANDS[b].name}`,
                  order:bandPaintOrder(bg,b), bg, band:b, kind:'band' });
  }
  if (actor.show) rows.push({ z: actorZ(), order:resolvedPlane('obj2').order,
                              label: 'Sprites (OBJ2 reference)', kind:'obj' });
  rows.sort((a,b) => a.z - b.z);
  for (let i=0;i<rows.length;i++) for (let j=i+1;j<rows.length;j++) {
    /* Only BACKGROUND bands can clash with each other. The sprite plane sits
     * 0.01 in front of BG1's, and that hair of separation is deliberate -- it
     * is how "priority-1 tiles draw over sprites" survives becoming a 3D
     * composite. Flagging it would be crying wolf on the one relationship the
     * shipped table is most careful about. */
    if (rows[i].kind !== 'band' || rows[j].kind !== 'band') continue;
    /* The anchor and priority surface of one SNES BG deliberately sit almost
     * together. That is authentic geometry, not a virtual-layer collision. */
    if (rows[i].bg === rows[j].bg) continue;
    if (Math.abs(rows[i].z - rows[j].z) < kClashEpsilon) {
      rows[i].clash = true; rows[j].clash = true;
      clashPairs.push(`${rows[i].label} ↔ ${rows[j].label}`);
    }
  }
  rows.clashPairs = clashPairs;
  return rows;
}

/* ---- ROM asset decode -------------------------------------------------
 * Mirrors diorama_rom_backdrop.c exactly. The metatile word is stored
 * byte-SWAPPED in the ROM ($02:B3CE swaps it while copying to WRAM), then
 * masked with $ECFF and OR'd with the per-BG attribute byte -- that merge is
 * what maps BG1 definitions to characters $000-$0FF and BG2 to $100-$1FF.
 * The shared scene descriptor also supplies the profile's common-priority
 * bits; five rooms force every BG2 tile high through that independent field.
 * Getting any of that wrong renders unrelated art, so it is not re-derived
 * here; it is transcribed. */
const B64 = s => { const b = atob(s), a = new Uint8Array(b.length);
  for (let i=0;i<b.length;i++) a[i]=b.charCodeAt(i); return a; };
const BLOBS = DATA.blobs.map(B64);
const RASTER_WAVEFORM = BLOBS[DATA.rasterWaveform];
const RASTER_R4_WINDOW = BLOBS[DATA.rasterR4Window];
const expand5 = v => ((v&31)<<3)|((v&31)>>2);
let nativeFrame = 0, nativePlayTimer = null;
const nativeCamera = { x:0, y:0 };

function animationPhase(r) {
  if (!r.animation) return 0;
  return (Math.floor(nativeFrame / r.animation.cadence)) &
    (r.animation.phases - 1);
}
function bg2PagePhase(r) {
  if (!r.bg2PageCycle) return 0;
  return Math.floor(nativeFrame / r.bg2PageCycle.cadence) & 3;
}
function bg2PageIndex(r) {
  return r.bg2PageCycle ? r.bg2PageCycle.order[bg2PagePhase(r)] : 0;
}
function charactersAtFrame(r) {
  const source = BLOBS[r.chars];
  if (!r.animation) return source;
  const characters = source.slice();
  const target = r.animation.target * 2;
  const stride = r.animation.stride;
  const from = target + animationPhase(r) * stride;
  characters.set(source.subarray(from, from + stride), target);
  return characters;
}

function decodeLayer(room, bg) {
  const layer = room.bg[bg];
  if (!layer) return null;
  const chars = charactersAtFrame(room);
  const extra = room.extraChars >= 0 ? BLOBS[room.extraChars] : null;
  const praw  = BLOBS[room.palette];
  const mt = BLOBS[layer.metatiles], mp = BLOBS[layer.map];
  const baseAttrs =
    (bg === 0 ? DATA.bg1Attributes : DATA.bg2Attributes) << 8;
  const commonPriority = room.video && room.video.length > 4 &&
    (room.video[4] & (1 << bg)) ? 0x2000 : 0;
  const attrs = baseAttrs | commonPriority;

  const pal = new Uint32Array(128);
  for (let i=0;i<128;i++){ const c = praw[i*2] | (praw[i*2+1]<<8);
    pal[i] = 0xff000000 | (expand5(c>>10)<<16) | (expand5(c>>5)<<8) | expand5(c); }

  const pw = layer.pagesWide, ph = layer.pagesHigh;
  const cellsW = pw*16, cellsH = ph*16;          /* 16x16-pixel metatiles */
  const tilesW = cellsW*2, tilesH = cellsH*2;    /* 8x8 tiles            */

  /* Per 8x8 tile: which character, palette, priority, flips. Priority is the
   * classification the artist already made -- the game uses it to put a tile
   * in front of sprites -- so it is surfaced as a selector, not overwritten. */
  const words = new Uint16Array(tilesW*tilesH);
  const cellId = new Uint8Array(cellsW*cellsH);
  for (let cy=0; cy<cellsH; cy++) for (let cx=0; cx<cellsW; cx++) {
    const page = (cy>>4)*pw + (cx>>4);
    const id = mp[page*256 + (cy&15)*16 + (cx&15)];
    cellId[cy*cellsW+cx] = id;
    for (let q=0;q<4;q++){
      const src = id*8 + q*2;
      const definition = mt[src+1] | (mt[src]<<8);   /* byte-swapped in ROM */
      const entry = (definition & DATA.tileWordMask) | attrs;
      const tx = cx*2 + (q&1), ty = cy*2 + (q>>1);
      words[ty*tilesW+tx] = entry;
    }
  }
  return { pw, ph, cellsW, cellsH, tilesW, tilesH, w:tilesW*8, h:tilesH*8,
           words, cellId, pal, chars, extra, room, bg };
}

/* One 8x8 character's pixels, 4bpp planar, honouring both flips. */
function blitTile(L, entry, dst, dw, ox, oy, tintRGB, sourceX=ox, sourceY=oy, maskOverride=undefined) {
  const cell=Math.floor(sourceY/16)*L.cellsW+Math.floor(sourceX/16);
  const mask=maskOverride??pixelMaskAt(L.room,L.bg,L,cell);
  for(let py=0;py<8;py++)for(let px=0;px<8;px++) {
    const black=pixelIsBlack(mask,(sourceX&15)+px,(sourceY&15)+py);
    const value=nativeCharacterPixel(L,entry,px,py);
    if(!black&&!value)continue;
    const color=black?0xff000000:L.pal[((entry>>10)&7)*16+value];
    let red=color&255,green=(color>>>8)&255,blue=(color>>>16)&255;
    if(tintRGB&&!black){red=(red*3+tintRGB[0])>>2;
      green=(green*3+tintRGB[1])>>2;blue=(blue*3+tintRGB[2])>>2;}
    const o=((oy+py)*dw+ox+px)*4;
    dst[o]=red;dst[o+1]=green;dst[o+2]=blue;dst[o+3]=255;
  }
}

/* ---- exact stable-room frame -----------------------------------------
 * This is the JavaScript transcription of ActionRoomScene_BuildFrameState
 * and ActionRoomScene_RenderNativeFrame. The exporter supplies normalized
 * profile bytes and the ROM's $02:96D4 waveform; no gameplay state or save
 * state participates. C tests pin every preset and the native compositor. */
const u8 = value => value & 0xff;
const u16 = value => value & 0xffff;
const u10 = value => value & 0x3ff;
const swap16 = value => u16((value << 8) | (value >>> 8));
const mode2Bytes = (first,second) => u10(u8(first) | (u8(second)<<8));
function resolveParallax(camera, ratio, extent, viewport,wrap=true) {
  const numerator = ratio >>> 4, denominator = ratio & 15;
  let result = denominator ? Math.floor(camera * numerator / denominator) : 0;
  if (extent >= 0x300 && result + viewport >= extent)
    result = extent - viewport;
  return wrap?u10(result):result;
}
function rasterWriter(values) {
  return {
    values, line:0, held:values[0],
    write(count,value) {
      this.held=u10(value);
      while (count-- > 0 && this.line < DATA.frameHeight)
        this.values[this.line++]=this.held;
    },
    finish() {
      while (this.line < DATA.frameHeight)
        this.values[this.line++]=this.held;
    },
  };
}
function nativeFrameState(r) {
  const h=[new Uint16Array(DATA.frameHeight),new Uint16Array(DATA.frameHeight)];
  const v=[new Uint16Array(DATA.frameHeight),new Uint16Array(DATA.frameHeight)];
  const mosaic=new Uint8Array(DATA.frameHeight);
  /* Visible frame N scans the persistent raster table built at game tick N-1. */
  const bg2=r.bg[1], frame=u16(nativeFrame-1), cameraX=u16(nativeCamera.x);
  const rasterWorkspace=BLOBS[r.rasterWorkspace];
  const inheritedMode2=(offset,first)=>mode2Bytes(
    first,rasterWorkspace&&offset<rasterWorkspace.length?rasterWorkspace[offset]:0);
  const baseH=[u10(nativeCamera.x),resolveParallax(nativeCamera.x,r.video[9],
    bg2.pagesWide*256,0x100)];
  const baseV=[u10(nativeCamera.y),resolveParallax(nativeCamera.y,r.video[10],
    bg2.pagesHigh*256,0xe0)];
  for(let bg=0;bg<2;bg++){h[bg].fill(baseH[bg]);v[bg].fill(baseV[bg]);}
  const bg1w=rasterWriter(h[0]),bg2w=rasterWriter(h[1]),bg2vw=rasterWriter(v[1]);
  switch(r.raster) {
    case 1: { let phase=u8(frame>>>1);
      for(let i=0;i<0x6f;i++) bg2w.write(2,inheritedMode2(
        i*3+2,u8(RASTER_WAVEFORM[phase++]+frame)));
      bg2w.finish(); break; }
    case 2: { bg2w.write(0x7f,0); const step=u16((frame+cameraX)<<1); let value=step;
      for(let i=0;i<96;i++){bg2w.write(1,inheritedMode2(
        3+i*3+2,value>>>8));value=u16(value+step);}
      bg2w.finish(); break; }
    case 3: { bg2vw.write(0x7f,inheritedMode2(2,0)); let value=u8(frame);
      if ((frame>>>8)&1) value^=0xff; value=(value>>>4)&15;
      for(let i=0;i<32;i++){bg2vw.write(1,inheritedMode2(
        3+i*3+2,value));value=u8(value-5);}
      bg2vw.write(1,inheritedMode2(3+32*3+2,0));bg2vw.finish();break; }
    case 4: { let phase=u8(frame)>>>2,line=0,held=0;
      /* Native `$02:9382` leaves the high byte of its 16-bit source index at
       * one, so R4 samples the ROM window immediately after the waveform. */
      for(let i=0;i<0x70;i++){held=((RASTER_R4_WINDOW[phase++]<<4)&0x10)|2;
        for(let n=0;n<2;n++)if(line<DATA.frameHeight)mosaic[line++]=held;}
      while(line<DATA.frameHeight)mosaic[line++]=held;break; }
    case 5: { let value=u16((frame<<1)+(cameraX>>>1));
      bg2w.write(0x3f,value);value>>>=1;bg2w.write(0x10,value);
      value>>>=1;bg2w.write(8,value);value>>>=1;bg2w.write(8,value);
      bg2w.write(0x10,0);bg2w.write(8,cameraX>>>4);
      bg2w.write(0x10,cameraX>>>3);bg2w.write(0x28,cameraX>>>2);
      bg2w.write(0x50,cameraX>>>1);bg2w.finish();break; }
    case 6: { let value=u16((frame<<2)+(cameraX>>>1));
      bg2w.write(0x1e,value);value>>>=1;bg2w.write(0x10,value);
      value>>>=1;bg2w.write(0x10,value);value>>>=1;bg2w.write(8,value);
      value>>>=1;bg2w.write(8,value);bg2w.write(0x70,cameraX>>>1);
      const step=u16((frame+cameraX)<<2);value=step;
      for(let i=0;i<0x20;i++){bg2w.write(1,swap16(value));value=u16(value+step);value=u16(value+step);}
      bg2w.finish();break; }
    case 7: { let phase=u8(frame>>>1),step=1;
      for(let i=0;i<0x6f;i++){const source=phase;phase=u8(phase+step++);
        bg2w.write(2,inheritedMode2(0x800+i*3+2,
          RASTER_WAVEFORM[source]));}bg2w.finish();break; }
    case 8: { const reverse=u16(-frame);bg2w.write(0x4f,0);bg2w.write(0x40,0);
      bg2w.write(0x10,reverse>>>2);bg2w.write(0x10,reverse>>>1);
      bg2w.write(0x10,reverse);bg2w.write(8,u16(reverse<<1));
      bg2w.write(4,u16((reverse<<1)+reverse));bg2w.write(8,u16(reverse<<2));
      bg2w.write(4,u16(frame<<1));bg2w.write(8,frame);bg2w.write(0x10,frame>>>1);
      bg2w.finish();break; }
    case 9: bg2w.write(0x4f,inheritedMode2(0x1002,0));
      bg2w.write(0x40,cameraX>>>2);
      bg2w.write(0x60,cameraX>>>1);bg2w.finish();break;
    case 10: { let phase=u8(frame>>>1);
      for(let i=0;i<0x6f;i++){const wave=RASTER_WAVEFORM[phase++];
        bg2w.write(2,inheritedMode2(i*3+2,u8(wave+frame)));
        bg1w.write(2,inheritedMode2(0x800+i*3+2,u8(-wave+0x40)));}
      bg1w.finish();bg2w.finish();break; }
  }
  return {h,v,mosaic,tm:r.video[0],ts:r.video[1],cgwsel:r.video[2],
    cgadsub:r.video[3],bgmode:r.video[6],fixedColor:0,brightness:15,
    page:bg2PageIndex(r)};
}
function nativeTileAt(r,state,layers,bg,pixelX,pixelY) {
  const layer=layers[bg]; let x=pixelX,y=pixelY;
  if(bg===1&&r.bg2PageCycle){x=(x&255)+(state.page&1)*256;
    y=(y&255)+(state.page>>>1)*256;}
  else{x=((x%layer.w)+layer.w)%layer.w;y=((y%layer.h)+layer.h)%layer.h;}
  return {entry:layer.words[(y>>>3)*layer.tilesW+(x>>>3)],
          x:x&7,y:y&7,tx:x>>>3,ty:y>>>3};
}
function nativeCharacterPixel(layer,entry,x,y) {
  let tile=entry&0x3ff,source=layer.chars;
  if(tile*32+32>source.length){if(!layer.extra||tile<0x200||tile>=0x300)return 0;
    source=layer.extra;tile-=0x200;}
  if(entry&0x4000)x=7-x;if(entry&0x8000)y=7-y;
  const a=tile*32+y*2,b=7-x;
  return ((source[a]>>>b)&1)|(((source[a+1]>>>b)&1)<<1)|
    (((source[a+16]>>>b)&1)<<2)|(((source[a+17]>>>b)&1)<<3);
}
function nativeLayerPixel(r,state,layers,bg,x,y) {
  const mosaic=state.mosaic[y],size=(mosaic>>>4)+1;
  let sx=x,line=y+1;
  if(size>1&&(mosaic&(1<<bg))){sx-=((sx%size)+size)%size;line-=line%size;}
  const tile=nativeTileAt(r,state,layers,bg,u10(state.h[bg][y]+sx),
    u10(state.v[bg][y]+line));
  const pixel=nativeCharacterPixel(layers[bg],tile.entry,tile.x,tile.y);
  if(!pixel)return 5<<8;
  const palette=((tile.entry>>>10)&7)*16+pixel;
  const high=(tile.entry&0x2000)!==0;
  const rank=bg===0?(high?12:8):(high?11:7);
  const classified=bandAt(bucket(r,bg),layers[bg],tile.tx,tile.ty);
  return palette|(bg<<8)|(rank<<12)|(classified<<16);
}
function nativeScreenPixel(mask,bg1,bg2){let result=5<<8;
  if((mask&1)&&((bg1>>>12)&15)>((result>>>12)&15))result=bg1;
  if((mask&2)&&((bg2>>>12)&15)>((result>>>12)&15))result=bg2;
  return result;}
function nativeCompositePixel(r,state,main,sub) {
  const raw=BLOBS[r.palette],palette=i=>raw[i*2]|(raw[i*2+1]<<8);
  const clip=(state.cgwsel>>>6)&3,visible=clip===0||clip===2;
  let color=palette(main&255),red=visible?color&31:0;
  let green=visible?(color>>>5)&31:0,blue=visible?(color>>>10)&31:0;
  const prevent=(state.cgwsel>>>4)&3,mathWindow=prevent===0||prevent===2;
  const layer=(main>>>8)&15,math=mathWindow&&(state.cgadsub&(1<<layer));
  let half=false;
  if(math){const useSub=(state.cgwsel&2)!==0,haveSub=useSub&&(sub&255)!==0;
    const second=haveSub?palette(sub&255):state.fixedColor;
    const r2=second&31,g2=(second>>>5)&31,b2=(second>>>10)&31;
    if(state.cgadsub&0x80){red=Math.max(0,red-r2);green=Math.max(0,green-g2);blue=Math.max(0,blue-b2);}
    else{red+=r2;green+=g2;blue+=b2;}
    half=(state.cgadsub&0x40)!==0&&(!useSub||haveSub);}
  const component=value=>{if(half)value>>>=1;value=Math.min(value,31);
    return Math.floor(expand5(value)*state.brightness/15);};
  let rr=component(red),gg=component(green),bb=component(blue);
  if(tint&&layer<2){const c=bandRGB((main>>>16)&3);
    rr=(rr*3+c[0])>>2;gg=(gg*3+c[1])>>2;bb=(bb*3+c[2])>>2;}
  return rr|(gg<<8)|(bb<<16);
}
let native2dCache=null,nativeBandCache=null;
let nativeDecodedKey='',nativeDecoded=null;
function nativeDecodedLayers() {
  const decodedKey=`${sceneKey(room)}:${animationPhase(room)}`;
  if(nativeDecodedKey!==decodedKey){nativeDecodedKey=decodedKey;
    nativeDecoded=[decodeLayer(room,0),decodeLayer(room,1)];}
  return {decodedKey,layers:nativeDecoded};
}
function nativeFrameCanvas() {
  const decoded=nativeDecodedLayers(),decodedKey=decoded.decodedKey;
  const key=`${decodedKey}:${nativeFrame}:${nativeCamera.x}:${nativeCamera.y}:${tint}`;
  if(native2dCache&&native2dCache.key===key)return native2dCache.canvas;
  const state=nativeFrameState(room),image=new ImageData(DATA.frameWidth,DATA.frameHeight);
  for(let y=0;y<DATA.frameHeight;y++)for(let x=0;x<DATA.frameWidth;x++){
    const bg1=nativeLayerPixel(room,state,decoded.layers,0,x,y);
    const bg2=nativeLayerPixel(room,state,decoded.layers,1,x,y);
    const main=nativeScreenPixel(state.tm,bg1,bg2),sub=nativeScreenPixel(state.ts,bg1,bg2);
    const color=nativeCompositePixel(room,state,main,sub),o=(y*DATA.frameWidth+x)*4;
    image.data[o]=color&255;image.data[o+1]=(color>>>8)&255;
    image.data[o+2]=(color>>>16)&255;image.data[o+3]=255;
  }
  const canvas=document.createElement('canvas');canvas.width=DATA.frameWidth;
  canvas.height=DATA.frameHeight;canvas.getContext('2d').putImageData(image,0,0);
  native2dCache={key,canvas};return canvas;
}

/* This is the surface contract consumed by Diorama in game: each hardware BG
 * is sampled with the visible line's live scroll/mosaic state, and every real
 * pixel is routed to exactly one of its three destination planes. */
function nativeBandSurfaces() {
  const decoded=nativeDecodedLayers();
  const guard=roomFraming(room).x?64:0,width=DATA.frameWidth+2*guard;
  const key=`${decoded.decodedKey}:${nativeFrame}:${nativeCamera.x}`
    +`:${nativeCamera.y}:${tint}:${guard}`;
  if(nativeBandCache&&nativeBandCache.key===key)return nativeBandCache;
  const state=nativeFrameState(room);
  const images=[0,1].map(()=>[0,1,2].map(
    ()=>new ImageData(width,DATA.frameHeight)));
  const tintRgb=BANDS.map((_,b)=>tint?bandRGB(b):null);
  for(let y=0;y<DATA.frameHeight;y++)for(let x=-guard;x<DATA.frameWidth+guard;x++){
    const o=(y*width+x+guard)*4;
    for(let bg=0;bg<2;bg++){
      if(!((state.tm|state.ts)&(1<<bg)))continue;
      const layer=decoded.layers[bg];
      const mosaic=state.mosaic[y],size=(mosaic>>>4)+1;
      let sx=x,line=y+1;
      if(size>1&&(mosaic&(1<<bg))){sx-=((sx%size)+size)%size;line-=line%size;}
      const baseX=bg===0?nativeCamera.x:resolveParallax(nativeCamera.x,room.video[9],
        layer.w,DATA.frameWidth,false);
      const baseY=bg===0?nativeCamera.y:resolveParallax(nativeCamera.y,room.video[10],
        layer.h,DATA.frameHeight,false);
      const xWorld=baseX+sx+((state.h[bg][y]-u10(baseX)+512)&1023)-512;
      const yWorld=baseY+line+((state.v[bg][y]-u10(baseY)+512)&1023)-512;
      const pasted=stampBucket(room,bg).cells[`${Math.floor(xWorld/16)},${Math.floor(yWorld/16)}`];
      if(pasted) {
        const px=xWorld&15,py=yWorld&15,band=pasted.bands[(py>>3)*2+(px>>3)];
        const color=stampColor(layer,pasted,px,py);
        if(color!==null) {
          const data=images[bg][band].data,c=tintRgb[band];
          let red=color&255,green=(color>>>8)&255,blue=(color>>>16)&255;
          if(c&&!pixelIsBlack(pasted.black,px,py)) {
            red=(red*3+c[0])>>2;green=(green*3+c[1])>>2;blue=(blue*3+c[2])>>2;
          }
          data[o]=red;data[o+1]=green;data[o+2]=blue;data[o+3]=255;
        }
        continue;
      }
      if(guard&&bg===0&&(xWorld<0||xWorld>=layer.w||yWorld<0||yWorld>=layer.h))continue;
      const tile=nativeTileAt(room,state,decoded.layers,bg,u10(state.h[bg][y]+sx),
        u10(state.v[bg][y]+line));
      const cell=(tile.ty>>1)*layer.cellsW+(tile.tx>>1);
      const black=pixelIsBlack(pixelMaskAt(room,bg,layer,cell),
        (tile.tx&1)*8+tile.x,(tile.ty&1)*8+tile.y);
      const pixel=nativeLayerPixel(room,state,decoded.layers,bg,x,y);
      if(!black&&((pixel>>>8)&15)!==bg)continue;
      const band=bandAt(bucket(room,bg),layer,tile.tx,tile.ty);
      const color=black?0xff000000:layer.pal[pixel&255];
      let red=color&255,green=(color>>>8)&255,blue=(color>>>16)&255;
      const c=tintRgb[band];
      if(c&&!black){red=(red*3+c[0])>>2;green=(green*3+c[1])>>2;
        blue=(blue*3+c[2])>>2;}
      const data=images[bg][band].data;
      data[o]=red;data[o+1]=green;data[o+2]=blue;data[o+3]=255;
    }
  }
  nativeBandCache={key,state,images,width};return nativeBandCache;
}

/* Every exported room carries one C-rendered golden frame. Verify it through
 * the JavaScript compositor the first time that room is opened; this catches
 * transcription drift without requiring the game or a save state. */
const nativeGoldenStatus=new Map();
function verifyRoomNativeGolden(r) {
  const key=sceneKey(r);
  if(nativeGoldenStatus.has(key))return nativeGoldenStatus.get(key);
  const golden=r.nativeGolden;
  if(!golden){nativeGoldenStatus.set(key,false);return false;}
  const savedFrame=nativeFrame,savedX=nativeCamera.x,savedY=nativeCamera.y;
  const savedTint=tint;
  let hash=2166136261>>>0;
  try {
    nativeFrame=golden.frame;nativeCamera.x=golden.cameraX;
    nativeCamera.y=golden.cameraY;tint=false;
    const layers=[decodeLayer(r,0),decodeLayer(r,1)],state=nativeFrameState(r);
    for(let y=0;y<DATA.frameHeight;y++)for(let x=0;x<DATA.frameWidth;x++){
      const bg1=nativeLayerPixel(r,state,layers,0,x,y);
      const bg2=nativeLayerPixel(r,state,layers,1,x,y);
      const main=nativeScreenPixel(state.tm,bg1,bg2);
      const sub=nativeScreenPixel(state.ts,bg1,bg2);
      const color=nativeCompositePixel(r,state,main,sub);
      const argb=(0xff000000|((color&255)<<16)|(color&0xff00)|
                  ((color>>>16)&255))>>>0;
      hash=Math.imul((hash^argb)>>>0,16777619)>>>0;
    }
  } finally {
    nativeFrame=savedFrame;nativeCamera.x=savedX;nativeCamera.y=savedY;
    tint=savedTint;
  }
  const matches=hash===(golden.hash>>>0);
  nativeGoldenStatus.set(key,matches);
  if(!matches)console.error(`Native C/JS parity mismatch for ${key}: `
    +`${hash.toString(16)} != ${(golden.hash>>>0).toString(16)}`);
  return matches;
}
