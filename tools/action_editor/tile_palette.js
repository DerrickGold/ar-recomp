/* The ROM exporter includes every metatile definition and the loaded CHR
 * banks. Borrow definitions only when they resolve against identical assets;
 * stamps freeze tile words, not graphics from an unrelated room's memory. */
const tilePalette=$('#tilePalette');
tilePalette.hidden=true;
let paletteContext='',paletteSourceIndex=0,paletteCache='',paletteQuarter=0;
let paletteWords=[0,0,0,0],paletteChoice=null,paletteButtons=[];
const paletteAssetMatch=(a,b)=>a.chars===b.chars&&a.extraChars===b.extraChars&&
  a.palette===b.palette&&JSON.stringify(a.animation||null)===JSON.stringify(b.animation||null);
const paletteRoom=()=>terrainRoom(DATA.rooms[paletteSourceIndex],room.terrainProfile||0);
const paletteHex=(id,width=2)=>id.toString(16).toUpperCase().padStart(width,'0');
function paletteTile(layer,id) {
  if(!Number.isInteger(id)||id<0||id*4+4>layer.metatileWords.length)return null;
  const words=Array.from(layer.metatileWords.slice(id*4,id*4+4));
  return {id,words,bands:words.map(authenticBand),black:ZERO_PIXEL_MASK};
}
function loadedCharacterIds(layer) {
  const ids=[];
  for(let id=0;id<0x400;id++)
    if((id+1)*32<=layer.chars.length||layer.extra&&id>=0x200&&id<0x300&&
      (id-0x200+1)*32<=layer.extra.length)ids.push(id);
  return ids;
}
function paletteEntries(layer,kind) {
  const used=new Set(kind==='character'?Array.from(layer.words,w=>w&0x3ff):layer.cellId);
  const ids=kind==='character'?loadedCharacterIds(layer):
    Array.from({length:Math.floor(layer.metatileWords.length/4)},(_,i)=>i);
  return ids.map(id=>({id,used:used.has(id)}));
}
/* Keep original colors even while Band tint is enabled. Checkerboard is
 * drawn into the thumbnail so transparent and black pixels stay distinct. */
function paintPalettePreview(canvas,layer,tile,size=16,word=0) {
  const image=new ImageData(canvas.width,canvas.height),data=image.data;
  for(let y=0;y<canvas.height;y++)for(let x=0;x<canvas.width;x++) {
    const px=Math.floor(x*size/canvas.width),py=Math.floor(y*size/canvas.height);
    let color;
    if(size===8) {
      const value=nativeCharacterPixel(layer,word,px,py);
      color=value?layer.pal[((word>>10)&7)*16+value]:null;
    } else color=stampColor(layer,tile,px,py);
    const o=(y*canvas.width+x)*4,checker=((x>>2)^(y>>2))&1?43:29;
    data[o]=color===null?checker:color&255;
    data[o+1]=color===null?checker:(color>>>8)&255;
    data[o+2]=color===null?checker:(color>>>16)&255;data[o+3]=255;
  }
  canvas.getContext('2d').putImageData(image,0,0);
}
function refreshPaletteComposer(layer) {
  for(let q=0;q<4;q++) {
    const button=$(`#paletteQuarter${q}`);
    button.classList.toggle('on',q===paletteQuarter);
    button.setAttribute('aria-pressed',String(q===paletteQuarter));
    button.title=`${q<2?'Top':'Bottom'} ${q&1?'right':'left'} · word ${paletteHex(paletteWords[q],4)}`;
    paintPalettePreview($(`#paletteQuarterCanvas${q}`),layer,null,8,paletteWords[q]);
  }
}
function refreshPaletteStamp() {
  if(!L||tilePalette.hidden)return;
  const clip=tileClipboard,same=clip?.key===keyOf(room,bgIndex);
  $('#paletteStampInfo').textContent=same?`${clip.label||'Copied range'} · ${clip.w}×${clip.h}`:'Choose a tile';
  const canvas=$('#paletteStampPreview');
  if(!same){canvas.getContext('2d').clearRect(0,0,canvas.width,canvas.height);return;}
  /* Preserve the aspect ratio without allocating a full copy for a large range. */
  const scale=64/Math.max(clip.w,clip.h),w=Math.max(1,Math.round(clip.w*scale)),h=Math.max(1,Math.round(clip.h*scale));
  canvas.width=w;canvas.height=h;
  const image=new ImageData(w,h),data=image.data;
  for(let y=0;y<h;y++)for(let x=0;x<w;x++) {
    const px=Math.min(clip.w*16-1,Math.floor(x*clip.w*16/w));
    const py=Math.min(clip.h*16-1,Math.floor(y*clip.h*16/h));
    const tile=clip.cells[(py>>4)*clip.w+(px>>4)];
    const color=tile?stampColor(L,tile,px&15,py&15):null,o=(y*w+x)*4;
    const checker=((x>>2)^(y>>2))&1?43:29;
    data[o]=color===null?checker:color&255;
    data[o+1]=color===null?checker:(color>>>8)&255;
    data[o+2]=color===null?checker:(color>>>16)&255;data[o+3]=255;
  }
  canvas.getContext('2d').putImageData(image,0,0);
}
function choosePaletteTile(id) {
  const source=paletteRoom();
  if(!source.bg[bgIndex]||!paletteAssetMatch(room,source))return false;
  const layer=decodeLayer(source,bgIndex),tile=paletteTile(layer,id);if(!tile)return false;
  paletteWords=[...tile.words];paletteChoice=id;
  tileClipboard={key:keyOf(room,bgIndex),w:1,h:1,cells:[tile],label:`Tile ${paletteHex(id)}`};
  refreshPaletteComposer(layer);
  for(const {button,id:other} of paletteButtons) {
    const active=other===id;button.classList.toggle('on',active);
    button.setAttribute('aria-pressed',String(active));
  }
  startStamp();tileActionStatus(`Tile ${paletteHex(id)} ready. Click the map to place it; Esc finishes.`);
  return true;
}
function choosePaletteCharacter(id) {
  const source=paletteRoom();
  if(!source.bg[bgIndex]||!paletteAssetMatch(room,source))return false;
  const layer=decodeLayer(source,bgIndex);if(!loadedCharacterIds(layer).includes(id))return false;
  paletteWords[paletteQuarter]=id|(Number($('#paletteRow').value)<<10);
  paletteQuarter=(paletteQuarter+1)%4;
  refreshPaletteComposer(layer);
  tileActionStatus('Piece added to the assembled tile. Choose more pieces, then Use assembled tile.');
  return true;
}
function usePaletteComposite() {
  if(!paletteAssetMatch(room,paletteRoom()))return false;
  const tile={id:paletteChoice??0,words:[...paletteWords],bands:paletteWords.map(authenticBand),black:ZERO_PIXEL_MASK};
  tileClipboard={key:keyOf(room,bgIndex),w:1,h:1,cells:[tile],label:'Assembled tile'};
  startStamp();tileActionStatus('Assembled tile ready. Click the map to place it; Esc finishes.');return true;
}
function refreshTilePalette(force=false) {
  if(!L)return;
  const context=`${room.group}:${room.map}:${room.terrainProfile||0}:${bgIndex}`;
  if(context!==paletteContext) {
    paletteContext=context;paletteCache='';paletteChoice=null;paletteQuarter=0;
    paletteSourceIndex=DATA.rooms.findIndex(r=>r.group===room.group&&r.map===room.map);
    paletteWords=Array.from(L.metatileWords.slice(0,4));
    const select=$('#paletteSource');select.replaceChildren();
    let count=0;
    DATA.rooms.forEach((base,index)=>{
      const source=terrainRoom(base,room.terrainProfile||0);
      if(!source.bg[bgIndex]||!paletteAssetMatch(room,source))return;
      const option=document.createElement('option');option.value=String(index);
      const label=$('#room').children[index]?.textContent?.split('  —')[0]||`${base.group}:${base.map}`;
      option.textContent=index===paletteSourceIndex?`Current map · ${label}`:label;
      select.appendChild(option);count++;
    });
    select.value=String(paletteSourceIndex);
    $('#paletteSourceInfo').textContent=`BG${bgIndex+1} · ${count} compatible map${count===1?'':'s'}. `
      +'Other maps appear when graphics, colors and animation match this room.';
    $('#paletteRow').value=String((L.words[0]>>10)&7);
  }
  if(tilePalette.hidden)return;
  const kind=$('#paletteKind').value||'metatile',usage=$('#paletteUsage').value||'all';
  const row=Number($('#paletteRow').value),phase=animationPhase(paletteRoom());
  const cache=`${context}:${paletteSourceIndex}:${kind}:${usage}:${row}:${phase}`;
  if(!force&&cache===paletteCache){refreshPaletteStamp();return;}
  paletteCache=cache;
  const layer=decodeLayer(paletteRoom(),bgIndex),entries=paletteEntries(layer,kind);
  const filtered=entries.filter(e=>usage==='all'||e.used===(usage==='used'));
  $('#paletteComposer').hidden=kind!=='character';
  $('#paletteCount').textContent=`${filtered.length} of ${entries.length} ${kind==='character'?'pieces':'tiles'} · `
    +`${entries.filter(e=>!e.used).length} unused in source map. Gold dots = unused.`;
  const grid=$('#paletteGrid');grid.replaceChildren();paletteButtons=[];
  for(const {id,used} of filtered) {
    const button=document.createElement('button'),canvas=document.createElement('canvas');
    const size=kind==='character'?8:16;canvas.width=canvas.height=size;
    const label=paletteHex(id,kind==='character'?3:2);
    button.title=`${size}×${size} ${label} · ${used?'Used':'Unused'} in original source map`;
    button.setAttribute('aria-label',button.title);button.setAttribute('aria-pressed',String(kind==='metatile'&&id===paletteChoice));
    button.classList.toggle('unused',!used);button.classList.toggle('on',kind==='metatile'&&id===paletteChoice);
    paintPalettePreview(canvas,layer,kind==='metatile'?paletteTile(layer,id):null,size,id|(row<<10));
    button.appendChild(canvas);
    const caption=document.createElement('span');caption.textContent=label;button.appendChild(caption);
    button.onclick=()=>kind==='character'?choosePaletteCharacter(id):choosePaletteTile(id);
    grid.appendChild(button);paletteButtons.push({button,id});
  }
  refreshPaletteComposer(layer);refreshPaletteStamp();
}
function closeTilePalette(focus=true) {
  tilePalette.hidden=true;$('#tilePaletteOpen').classList.remove('on');
  $('#tilePaletteOpen').setAttribute('aria-pressed','false');
  if(focus)cvs.focus({preventScroll:true});
}
function openTilePalette() {
  setMode('2d');returnToEditedTiles();pixelInspector.hidden=true;
  tilePalette.hidden=false;$('#tilePaletteOpen').classList.add('on');
  $('#tilePaletteOpen').setAttribute('aria-pressed','true');refreshTilePalette();
}
$('#tilePaletteOpen').onclick=()=>tilePalette.hidden?openTilePalette():closeTilePalette();
$('#tilePaletteClose').onclick=()=>closeTilePalette();
$('#paletteSource').onchange=()=>{
  const index=Number($('#paletteSource').value),source=DATA.rooms[index];
  if(!source||!source.bg[bgIndex]||!paletteAssetMatch(room,source))return;
  paletteSourceIndex=index;paletteChoice=null;refreshTilePalette();
};
for(const name of ['paletteKind','paletteUsage','paletteRow'])$(`#${name}`).onchange=()=>refreshTilePalette();
for(let row=0;row<8;row++) {
  const option=document.createElement('option');option.value=String(row);option.textContent=`Palette ${row}`;
  $('#paletteRow').appendChild(option);
}
for(let q=0;q<4;q++)$(`#paletteQuarter${q}`).onclick=()=>{
  paletteQuarter=q;refreshPaletteComposer(decodeLayer(paletteRoom(),bgIndex));
};
$('#paletteUseComposite').onclick=usePaletteComposite;
