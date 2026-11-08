/* The ROM exporter includes every metatile definition and the loaded CHR
 * banks. Borrow definitions only when they resolve against identical assets;
 * stamps freeze tile words, not graphics from an unrelated room's memory. */
const tilePalette=$('#tilePalette');
tilePalette.hidden=true;
const paletteZoom=$('#paletteZoom');
paletteZoom.hidden=true;
let paletteZoomEntry=null;
let paletteContext='',paletteSourceIndex=0,paletteCache='',paletteQuarter=0;
let paletteWords=[0,0,0,0],paletteChoice=null,paletteButtons=[];
let voxelFacePalette=null;
let paletteStampPick=null;
const paletteBg=()=>voxelFacePalette?.bg??bgIndex;
const paletteFaceContext=()=>`${room.group}:${room.map}:${room.terrainProfile||0}`;
function paletteReadArt(value) {
  if(!value||value==='auto')return null;
  const parts=String(value).split(','),words=parts.slice(1,5).map(v=>parseInt(v,16));
  const bg=Number(parts[0]),[x,y,size]=parts.slice(5).map(Number);
  if(parts.length!==8||![0,1].includes(bg)||words.some((v,i)=>!/^[0-9a-f]{1,4}$/i.test(parts[i+1])||!Number.isInteger(v))||
    ![x,y,size].every(Number.isInteger)||![8,16].includes(size)||x<0||y<0||x+size>16||y+size>16)return null;
  return {bg,words,x,y,size};
}
function paletteArtValue(art) {
  return [art.bg,...art.words.map(word=>paletteHex(word,4)),art.x,art.y,art.size].join(',');
}
function openVoxelFacePalette(choose,options={}) {
  if(voxelFacePalette)closeTilePalette(false);
  const art=paletteReadArt(options.currentArt);
  cvs.classList.remove('palette-eyedropper');$('#sharedGl').classList.remove('palette-eyedropper');
  voxelFacePalette={choose,bg:art?.bg??bgIndex,art,label:options.label||'Selected voxel face',
    context:paletteFaceContext(),returnFocus:document.activeElement,picking:null,
    preview:options.preview,cancel:options.cancel,accept:options.accept,previewValue:art?paletteArtValue(art):null};
  $('#paletteFacePieceMode').value=art?.size===16?'assemble':'piece';
  $('#paletteFaceStatus').textContent=art?'Current face artwork. Adjust the crop or choose another native tile.':'Choose native artwork or pick a tile from the scene.';
  paletteContext='';$('#palettePieceFlipH').checked=$('#palettePieceFlipV').checked=false;
  tilePalette.hidden=false;refreshTilePalette(true);
  if(art)paletteWords=[...art.words];
  refreshPaletteComposer(decodeLayer(paletteRoom(),paletteBg()));refreshVoxelFacePalette();
  $(art?'#paletteFaceSource':'#palettePickFace').focus({preventScroll:true});
}
/* Face artwork never enters the map clipboard. A live callback updates the
 * destination's preview transaction; Done accepts it and Cancel restores it. */
function previewVoxelFacePalette() {
  const session=voxelFacePalette;if(!session?.art||!session.preview)return true;
  const value=paletteArtValue(session.art);if(value===session.previewValue)return true;
  try {session.preview(value);session.previewValue=value;return true;}
  catch(error){$('#paletteFaceStatus').textContent=error.message;return false;}
}
function chooseVoxelFaceArt(art,label='',keepComposer=false) {
  if(!voxelFacePalette||!room.bg[art.bg])return false;
  if(voxelFacePalette.bg!==art.bg){voxelFacePalette.bg=art.bg;refreshTilePalette(true);}
  voxelFacePalette.art={...art,words:[...art.words]};
  if(!keepComposer)paletteWords=[...art.words];
  if(label)$('#paletteFaceStatus').textContent=label;
  refreshPaletteComposer(decodeLayer(paletteRoom(),paletteBg()));refreshVoxelFacePalette();return previewVoxelFacePalette();
}
function applyVoxelFacePalette() {
  const session=voxelFacePalette;if(!session?.art)return false;
  if(session.context!==paletteFaceContext()){closeTilePalette(false);return false;}
  if(session.picking)endVoxelFacePick();
  try {
    if(session.preview) {if(!previewVoxelFacePalette())return false;session.accept?.();}
    else session.choose(paletteArtValue(session.art));
    closeTilePalette(false,true);session.returnFocus?.focus({preventScroll:true});return true;
  } catch(error){$('#paletteFaceStatus').textContent=error.message;return false;}
}
function refreshVoxelFacePalette() {
  const session=voxelFacePalette,art=session?.art;
  tilePalette.classList.toggle('face-art-palette',!!session);
  $('#paletteFaceControls').hidden=!session;$('#paletteStamp').hidden=!!session;
  $('#palettePickStamp').hidden=!!session;
  $('#paletteStampHint').hidden=!!session;$('#palettePieceFlips').hidden=!session;
  $('#paletteTitle').textContent=session?'Face artwork':'Tile palette';
  $('#paletteUseComposite').textContent=session?'Use assembled face':'Use assembled tile';
  $('#paletteUseComposite').hidden=!!session?.preview;
  $('#paletteFacePieceModeRow').hidden=!session;
  $('#tilePaletteClose').textContent=session?.preview?'Cancel':'Close';
  $('#paletteSource').disabled=!!session;
  if(!session)return;
  $('#paletteFaceTarget').textContent=session.label;
  $('#paletteFaceBg').value=String(session.bg);
  $('#paletteFaceBg2').disabled=!room.bg[1];
  $('#paletteSourceInfo').textContent=`BG${session.bg+1} · Current room native artwork. `+(session.preview?
    'Choices update the selected face immediately. Done keeps the preview; Cancel restores the previous artwork.':
    'Choose, crop or assemble artwork, then apply it to the selected face.');
  $('#paletteFaceApply').textContent=session.preview?'Done':'Apply artwork';
  $('#paletteFaceApply').disabled=!art;
  $('#paletteFaceSamples').hidden=!art;
  for(const id of ['paletteFaceSize','paletteFaceX','paletteFaceY','paletteFaceCenter','paletteFaceFlipH','paletteFaceFlipV'])$('#'+id).disabled=!art;
  $('#palettePickFace').classList.toggle('on',session.picking==='shared');
  $('#palettePickMap').classList.toggle('on',session.picking==='2d');
  for(const [id,mode] of [['palettePickFace','shared'],['palettePickMap','2d']])$('#'+id).setAttribute('aria-pressed',String(session.picking===mode));
  if(!art){$('#paletteFaceCropInfo').textContent='';return;}
  $('#paletteFaceSize').value=String(art.size);
  for(const axis of ['X','Y']) {
    const input=$('#paletteFace'+axis);input.max=String(16-art.size);input.value=String(art[axis.toLowerCase()]);input.disabled=art.size===16;
  }
  const source=$('#paletteFaceSource'),preview=$('#paletteFacePreview'),layer=decodeLayer(paletteRoom(),art.bg);
  source.width=source.height=16;preview.width=preview.height=art.size;
  const tile={words:art.words,black:ZERO_PIXEL_MASK,transparent:ZERO_PIXEL_MASK};
  paintPalettePreview(source,layer,tile);
  const image=new ImageData(art.size,art.size);
  for(let y=0;y<art.size;y++)for(let x=0;x<art.size;x++) {
    const color=stampColor(layer,tile,x+art.x,y+art.y),i=(y*art.size+x)*4;
    const checker=((x>>1)^(y>>1))&1?43:29;
    image.data[i]=color===null?checker:color&255;image.data[i+1]=color===null?checker:(color>>>8)&255;
    image.data[i+2]=color===null?checker:(color>>>16)&255;image.data[i+3]=255;
  }
  preview.getContext('2d').putImageData(image,0,0);
  const crop=$('#paletteFaceCrop');
  crop.style.left=`${art.x/16*100}%`;crop.style.top=`${art.y/16*100}%`;
  crop.style.width=crop.style.height=`${art.size/16*100}%`;
  $('#paletteFaceCropInfo').textContent=`${art.size} × ${art.size} crop at ${art.x}, ${art.y} · BG${art.bg+1}`;
  source.setAttribute('aria-label',`Native 16 by 16 source. ${art.size} by ${art.size} crop at ${art.x}, ${art.y}. Arrow keys move the crop.`);
}
function setVoxelFaceCrop(x,y,size=voxelFacePalette?.art?.size) {
  const art=voxelFacePalette?.art;if(!art||![8,16].includes(size)||!Number.isFinite(x)||!Number.isFinite(y))return false;
  art.size=size;art.x=Math.max(0,Math.min(16-size,Math.round(x)));art.y=Math.max(0,Math.min(16-size,Math.round(y)));
  refreshVoxelFacePalette();return previewVoxelFacePalette();
}
function flipVoxelFaceArt(axis) {
  const art=voxelFacePalette?.art;if(!art)return false;
  const bit=axis==='h'?1:2,flag=axis==='h'?0x4000:0x8000;
  art.words=art.words.map((_,q)=>art.words[q^bit]^flag);
  const coord=axis==='h'?'x':'y';art[coord]=16-art.size-art[coord];
  if(art.size===16)paletteWords=[...art.words];
  refreshPaletteComposer(decodeLayer(paletteRoom(),art.bg));refreshVoxelFacePalette();return previewVoxelFacePalette();
}
const paletteAssetMatch=(a,b)=>a.chars===b.chars&&a.extraChars===b.extraChars&&
  a.palette===b.palette&&JSON.stringify(a.animation||null)===JSON.stringify(b.animation||null);
const paletteRoom=()=>terrainRoom(DATA.rooms[paletteSourceIndex],room.terrainProfile||0);
const paletteHex=(id,width=2)=>id.toString(16).toUpperCase().padStart(width,'0');
function paletteTile(layer,id) {
  if(!Number.isInteger(id)||id<0||id*4+4>layer.metatileWords.length)return null;
  const words=Array.from(layer.metatileWords.slice(id*4,id*4+4));
  return {id,words,bands:words.map(authenticBand),black:ZERO_PIXEL_MASK,transparent:ZERO_PIXEL_MASK};
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
function hidePaletteZoom(entry=null) {
  if(entry&&paletteZoomEntry?.button!==entry.button)return;
  paletteZoom.hidden=true;paletteZoomEntry=null;
}
function showPaletteZoom(entry,layer=null) {
  if(tilePalette.hidden)return;
  layer??=decodeLayer(paletteRoom(),paletteBg());
  const {id,kind,used,button}=entry,size=kind==='character'?8:16;
  const canvas=$('#paletteZoomCanvas');canvas.width=canvas.height=size;
  // Scale the source pixels in CSS so artwork and checkerboard stay crisp.
  paintPalettePreview(canvas,layer,kind==='metatile'?paletteTile(layer,id):null,
    size,paletteCharacterWord(id));
  $('#paletteZoomTitle').textContent=`${kind==='character'?'Piece':'Tile'} ${paletteHex(id,size===8?3:2)} · ${size}×${size}`;
  $('#paletteZoomInfo').textContent=`${used?'Used':'Unused'} in source map`;
  paletteZoomEntry={...entry,context:paletteCache.slice(0,paletteCache.lastIndexOf(':'))};
  paletteZoom.hidden=false;
  const box=paletteZoom.getBoundingClientRect(),panel=tilePalette.getBoundingClientRect();
  const tile=button.getBoundingClientRect(),margin=8,gap=12;
  // Keep the preview beside the palette, outside its scrolling/clipped grid.
  let left=panel.left-box.width-gap;
  if(left<margin)left=panel.left+panel.width+gap;
  paletteZoom.style.left=Math.max(margin,Math.min(window.innerWidth-box.width-margin,left))+'px';
  paletteZoom.style.top=Math.max(margin,Math.min(window.innerHeight-box.height-margin,
    tile.top+(tile.height-box.height)/2))+'px';
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
  const source=paletteRoom(),bg=paletteBg();
  if(!source.bg[bg]||!paletteAssetMatch(room,source))return false;
  const layer=decodeLayer(source,bg),tile=paletteTile(layer,id);if(!tile)return false;
  paletteWords=[...tile.words];paletteChoice=id;
  refreshPaletteComposer(layer);
  for(const {button,id:other} of paletteButtons) {
    const active=other===id;button.classList.toggle('on',active);
    button.setAttribute('aria-pressed',String(active));
  }
  if(voxelFacePalette)return chooseVoxelFaceArt({bg,words:tile.words,x:0,y:0,size:16},`Tile ${paletteHex(id)} selected. Use the full tile, or crop an 8 × 8 patch.`);
  tileClipboard={key:keyOf(room,bgIndex),w:1,h:1,cells:[tile],label:`Tile ${paletteHex(id)}`};
  startStamp();tileActionStatus(`Tile ${paletteHex(id)} ready. Click the map to place it; Esc finishes.`);
  return true;
}
function paletteCharacterWord(id) {
  return id|(Number($('#paletteRow').value)<<10)|(voxelFacePalette&&$('#palettePieceFlipH').checked?0x4000:0)|
    (voxelFacePalette&&$('#palettePieceFlipV').checked?0x8000:0);
}
function choosePaletteCharacter(id) {
  const source=paletteRoom(),bg=paletteBg();
  if(!source.bg[bg]||!paletteAssetMatch(room,source))return false;
  const layer=decodeLayer(source,bg);if(!loadedCharacterIds(layer).includes(id))return false;
  const word=paletteCharacterWord(id);paletteWords[paletteQuarter]=word;
  paletteQuarter=(paletteQuarter+1)%4;
  refreshPaletteComposer(layer);
  if(voxelFacePalette) {
    const assemble=$('#paletteFacePieceMode').value==='assemble';
    return chooseVoxelFaceArt({bg,words:assemble?paletteWords:Array(4).fill(word),x:0,y:0,size:assemble?16:8},
      assemble?'Assembled artwork updated. Choose the next piece or a quarter to replace.':`Piece ${paletteHex(id,3)} selected. Repeats across the chosen face.`,true);
  }
  tileActionStatus('Piece added to the assembled tile. Choose more pieces, then Use assembled tile.');
  return true;
}
function usePaletteComposite() {
  if(!paletteAssetMatch(room,paletteRoom()))return false;
  if(voxelFacePalette)return chooseVoxelFaceArt({bg:paletteBg(),words:paletteWords,x:0,y:0,size:16},
    'Four native pieces assembled.',true);
  const tile={id:paletteChoice??0,words:[...paletteWords],bands:paletteWords.map(authenticBand),black:ZERO_PIXEL_MASK,transparent:ZERO_PIXEL_MASK};
  tileClipboard={key:keyOf(room,bgIndex),w:1,h:1,cells:[tile],label:'Assembled tile'};
  startStamp();tileActionStatus('Assembled tile ready. Click the map to place it; Esc finishes.');return true;
}
function refreshTilePalette(force=false) {
  if(!L){hidePaletteZoom();return;}
  if(voxelFacePalette&&voxelFacePalette.context!==paletteFaceContext()){closeTilePalette(false);return;}
  const bg=paletteBg(),context=`${room.group}:${room.map}:${room.terrainProfile||0}:${bg}`;
  if(context!==paletteContext) {
    paletteContext=context;paletteCache='';paletteChoice=null;paletteQuarter=0;
    paletteSourceIndex=DATA.rooms.findIndex(r=>sceneIdentity(r)===sceneIdentity(room));
    const activeLayer=decodeLayer(room,bg);
    paletteWords=Array.from(activeLayer.metatileWords.slice(0,4));
    const select=$('#paletteSource');select.replaceChildren();
    let count=0;
    DATA.rooms.forEach((base,index)=>{
      const source=terrainRoom(base,room.terrainProfile||0);
      if(!source.bg[bg]||!paletteAssetMatch(room,source)||voxelFacePalette&&index!==paletteSourceIndex)return;
      const option=document.createElement('option');option.value=String(index);
      const label=$('#room').children[index]?.textContent?.split('  —')[0]||`${base.group}:${base.map}`;
      option.textContent=index===paletteSourceIndex?`Current map · ${label}`:label;
      select.appendChild(option);count++;
    });
    select.value=String(paletteSourceIndex);
    $('#paletteSourceInfo').textContent=`BG${bg+1} · ${count} compatible map${count===1?'':'s'}. `
      +'Other maps appear when graphics, colors and animation match this room.';
    $('#paletteRow').value=String((activeLayer.words[0]>>10)&7);
  }
  if(tilePalette.hidden){hidePaletteZoom();return;}
  const kind=$('#paletteKind').value||'metatile',usage=$('#paletteUsage').value||'all';
  const row=Number($('#paletteRow').value),phase=animationPhase(paletteRoom());
  const cache=`${context}:${paletteSourceIndex}:${kind}:${usage}:${row}:${paletteCharacterWord(0)}:${phase}`;
  if(!force&&cache===paletteCache){refreshPaletteStamp();refreshVoxelFacePalette();return;}
  const zoomEntry=paletteZoomEntry;hidePaletteZoom();
  paletteCache=cache;
  const layer=decodeLayer(paletteRoom(),bg),entries=paletteEntries(layer,kind);
  const filtered=entries.filter(e=>usage==='all'||e.used===(usage==='used'));
  $('#paletteComposer').hidden=kind!=='character';
  $('#paletteCount').textContent=`${filtered.length} of ${entries.length} ${kind==='character'?'pieces':'tiles'} · `
    +`${entries.filter(e=>!e.used).length} unused in source map. Gold dots = unused.`;
  const grid=$('#paletteGrid');grid.replaceChildren();paletteButtons=[];
  for(const {id,used} of filtered) {
    const button=document.createElement('button'),canvas=document.createElement('canvas');
    const size=kind==='character'?8:16;canvas.width=canvas.height=size;
    const label=paletteHex(id,kind==='character'?3:2);
    button.setAttribute('aria-label',`${size}×${size} ${label} · ${used?'Used':'Unused'} in original source map`);
    button.setAttribute('aria-pressed',String(kind==='metatile'&&id===paletteChoice));
    button.classList.toggle('unused',!used);button.classList.toggle('on',kind==='metatile'&&id===paletteChoice);
    paintPalettePreview(canvas,layer,kind==='metatile'?paletteTile(layer,id):null,size,paletteCharacterWord(id));
    button.appendChild(canvas);
    const caption=document.createElement('span');caption.textContent=label;button.appendChild(caption);
    button.onclick=()=>kind==='character'?choosePaletteCharacter(id):choosePaletteTile(id);
    grid.appendChild(button);paletteButtons.push({button,id});
    const entry={button,id,kind,used};
    button.onpointerenter=()=>showPaletteZoom(entry);
    button.onfocus=()=>showPaletteZoom(entry);
    button.onpointerleave=button.onblur=()=>hidePaletteZoom(entry);
    // Animated thumbnails rebuild the grid; keep the hovered preview current.
    if(zoomEntry?.id===id&&zoomEntry.kind===kind&&
      zoomEntry.context===cache.slice(0,cache.lastIndexOf(':')))showPaletteZoom(entry,layer);
  }
  refreshPaletteComposer(layer);refreshPaletteStamp();refreshVoxelFacePalette();
}
function closeTilePalette(focus=true,accepted=false) {
  const session=voxelFacePalette;voxelFacePalette=null;paletteContext='';$('#paletteSource').disabled=false;
  paletteStampPick=null;$('#palettePicking').hidden=true;
  cvs.classList.remove('palette-eyedropper');$('#sharedGl').classList.remove('palette-eyedropper');
  refreshVoxelFacePalette();
  hidePaletteZoom();
  tilePalette.hidden=true;$('#tilePaletteOpen').classList.remove('on');
  $('#tilePaletteOpen').setAttribute('aria-pressed','false');
  if(session?.preview&&!accepted)session.cancel?.();
  if(focus)(session?.returnFocus||cvs).focus({preventScroll:true});
}
function openTilePalette() {
  if(voxelFacePalette)closeTilePalette(false);
  paletteContext='';refreshVoxelFacePalette();
  setMode('2d');returnToEditedTiles();pixelInspector.hidden=true;
  tilePalette.hidden=false;$('#tilePaletteOpen').classList.add('on');
  $('#tilePaletteOpen').setAttribute('aria-pressed','true');refreshTilePalette();
}
/* Keep the selected destination and its draft intact while temporarily looking
 * at the 2D source map. setMode normally closes palettes when entering 3D. */
function paletteFaceView(next) {
  const session=voxelFacePalette,words=[...paletteWords];
  voxelFacePalette=null; // setMode's ordinary close hook must not cancel this transaction.
  if(mode!==next)setMode(next);
  voxelFacePalette=session;paletteWords=words;
  if(session){tilePalette.hidden=false;refreshTilePalette(true);paletteWords=words;refreshPaletteComposer(decodeLayer(paletteRoom(),paletteBg()));}
}
function beginVoxelFacePick(next) {
  const session=voxelFacePalette;if(!session)return false;
  if(session.picking===next){endVoxelFacePick();return true;}
  session.returnMode??=mode;session.picking=next;
  paletteFaceView(next);
  cvs.classList.toggle('palette-eyedropper',next==='2d');$('#sharedGl').classList.toggle('palette-eyedropper',next==='shared');
  $('#paletteFaceStatus').textContent=next==='2d'?'Click a native map tile. Escape cancels the eyedropper.':'Click a visible tile face. Escape cancels the eyedropper.';
  $('#palettePickingText').textContent=next==='2d'?'Click a map tile to use its artwork.':'Click a visible face to use its artwork.';
  hidePaletteZoom();tilePalette.hidden=true;$('#palettePicking').hidden=false;
  $('#palettePickCancel').focus({preventScroll:true});refreshVoxelFacePalette();return true;
}
function endVoxelFacePick() {
  const session=voxelFacePalette;if(!session)return;
  const restore=session.returnMode;session.picking=null;delete session.returnMode;
  cvs.classList.remove('palette-eyedropper');$('#sharedGl').classList.remove('palette-eyedropper');
  if(restore)paletteFaceView(restore);
  tilePalette.hidden=false;$('#palettePicking').hidden=true;refreshVoxelFacePalette();
  $('#palettePickFace').focus({preventScroll:true});
}
function finishVoxelFacePaletteForHistory() {
  if(!voxelFacePalette)return true;
  if(voxelFacePalette.preview && voxelFacePalette.art)return applyVoxelFacePalette();
  closeTilePalette(false);return true;
}
function beginPaletteStampPick() {
  if(voxelFacePalette)return false;
  setMode('2d');paletteStampPick={context:keyOf(room,bgIndex),returnFocus:document.activeElement};
  hidePaletteZoom();tilePalette.hidden=true;cvs.classList.add('palette-eyedropper');
  $('#palettePickingText').textContent='Click a map tile to use as your paint brush. The current selection stays unchanged.';
  $('#palettePicking').hidden=false;$('#palettePickCancel').focus({preventScroll:true});return true;
}
function endPaletteStampPick() {
  const session=paletteStampPick;paletteStampPick=null;cvs.classList.remove('palette-eyedropper');
  $('#palettePicking').hidden=true;tilePalette.hidden=false;refreshTilePalette(true);
  session?.returnFocus?.focus({preventScroll:true});
}
/* Automatic cap geometry can name its exact native donor. Resolve the same
 * displayed words as the map, including pasted cells, rather than infer an
 * artwork tile from projected face bounds. An 8px crop may cross metatiles. */
function paletteSourceArt(source) {
  if(!source||![8,16].includes(source.size)||![source.x,source.y].every(Number.isInteger))return null;
  const bg=source.bg??0;if(![0,1].includes(bg)||!room.bg[bg])return null;
  const x0=Math.floor(source.x/8)*8,y0=Math.floor(source.y/8)*8;
  const x=source.x-x0,y=source.y-y0,size=source.size;
  if(x+size>16||y+size>16)return null;
  const layer=decodeLayer(room,bg),words=[];
  for(let q=0;q<4;q++) {
    const wx=x0+(q&1)*8,wy=y0+(q>>1)*8,cx=Math.floor(wx/16),cy=Math.floor(wy/16);
    const tile=displayedCell(room,bg,layer,cx,cy);
    const needed=(q&1)*8<x+size&&(q&1)*8+8>x&&(q>>1)*8<y+size&&(q>>1)*8+8>y;
    if((!tile||tile.blank)&&needed)return null;
    words.push(tile&&!tile.blank?tile.words[((wy-cy*16)>>3)*2+((wx-cx*16)>>3)]:0);
  }
  return {bg,words,x,y,size};
}
function paletteSampleFaceArt(hit) {
  if(!hit||hit.kind!=='tile')return null;
  const face=hit.face||'front';
  const own=hit[face+'Art'],inheritsSide=['left','right','bottom'].includes(face);
  // A deliberately empty face must not turn into native artwork through the
  // fallback path, even when inspecting a future/imported empty-art marker.
  if(own==='none'||(!paletteReadArt(own)&&inheritsSide&&hit.sideArt==='none'))return null;
  const art=paletteReadArt(own)||(inheritsSide?paletteReadArt(hit.sideArt):null);
  if(art)return {art,label:`Picked ${face} face artwork from cell ${hit.x}, ${hit.y}.`};
  // Coordinate selections can span different native material patches. A scene
  // eyedropper resolves the actual clicked patch; do not guess one here.
  if(hit.sampleSourceAmbiguous)return null;
  const donor=paletteSourceArt(hit.sampleSource);
  if(donor)return {art:donor,label:`Picked the ${face} face's native ${donor.size} × ${donor.size} donor at ${hit.sampleSource.x}, ${hit.sampleSource.y}.`};
  const bg=hit.bg??0;if(![0,1].includes(bg)||!room.bg[bg])return null;
  const layer=decodeLayer(room,bg),tile=displayedCell(room,bg,layer,hit.x,hit.y);
  if(!tile||tile.blank)return null;
  return {art:{bg,words:[...tile.words],x:0,y:0,size:16},label:`Picked native BG${bg+1} artwork from cell ${hit.x}, ${hit.y}.`};
}
function pickupVoxelFaceArt(hit) {
  if(!voxelFacePalette)return false;
  const sampled=paletteSampleFaceArt(hit);
  return sampled?chooseVoxelFaceArt(sampled.art,sampled.label):false;
}
function paletteConsume(event) {event.preventDefault();event.stopImmediatePropagation();}
cvs.addEventListener('mousedown',event=>{
  if(paletteStampPick&&mode==='2d'&&event.button===0&&!event.altKey&&!macControlClick(event)) {
    paletteConsume(event);
    if(paletteStampPick.context!==keyOf(room,bgIndex)){endPaletteStampPick();return;}
    if(sampleStampAt(...toCell(event)))endPaletteStampPick();
    else $('#palettePickingText').textContent='This position has no tile artwork. Choose another tile or cancel.';
    return;
  }
  if(!voxelFacePalette||voxelFacePalette.picking!=='2d'||mode!=='2d'||event.button!==0||event.altKey)return;
  paletteConsume(event);
  if(macControlClick(event))return;
  const [x,y]=toCell(event);
  if(pickupVoxelFaceArt({kind:'tile',x,y,bg:bgIndex})){endVoxelFacePick();$('#paletteFaceSource').focus({preventScroll:true});}
  else $('#palettePickingText').textContent='This cell has no native artwork. Pick another tile, or cancel.';
},true);
$('#sharedGl').addEventListener('pointerdown',event=>{
  if(!voxelFacePalette||voxelFacePalette.picking!=='shared'||mode!=='shared'||event.button!==0||event.altKey)return;
  paletteConsume(event);
  if(macControlClick(event))return;
  try {
    const point=sharedPreviewCanvasPoint($('#sharedGl'),event);
    // Objects block the eyedropper just as they block visible map faces. Never
    // silently sample through an actor and report it as the clicked artwork.
    const hit=point&&SharedRoomPreview.voxelPick(point.x,point.y,3);
    if(pickupVoxelFaceArt(hit)){endVoxelFacePick();$('#paletteFaceSource').focus({preventScroll:true});}
    else $('#palettePickingText').textContent='Pick a visible map face; sprite artwork is edited with its native object.';
  } catch(error){$('#palettePickingText').textContent=error.message;}
},true);
window.addEventListener('keydown',event=>{
  if((!voxelFacePalette&&!paletteStampPick)||event.key!=='Escape')return;
  paletteConsume(event);
  if(paletteStampPick){endPaletteStampPick();return;}
  if(voxelFacePalette.picking){endVoxelFacePick();$('#paletteFaceStatus').textContent='Eyedropper cancelled. The chosen artwork is unchanged.';}
  else closeTilePalette();
},true);
window.addEventListener('pointerdown',event=>{
  const session=voxelFacePalette;if(!session?.preview)return;
  if(tilePalette.contains(event.target)||$('#palettePicking').contains(event.target))return;
  if(session.picking && (cvs.contains(event.target)||$('#sharedGl').contains(event.target)))return;
  // Finish the artwork transaction before another control starts its own
  // gesture. A later Cancel must never roll back unrelated property edits.
  if(!finishVoxelFacePaletteForHistory())paletteConsume(event);
},true);
$('#tilePaletteOpen').onclick=()=>{
  if(tilePalette.hidden)openTilePalette();
  else {if(voxelFacePalette?.picking)endVoxelFacePick();closeTilePalette();}
};
$('#tilePaletteClose').onclick=()=>{if(voxelFacePalette?.picking)endVoxelFacePick();closeTilePalette();};
$('#paletteGrid').addEventListener('pointerleave',()=>hidePaletteZoom());
$('#paletteGrid').addEventListener('scroll',()=>hidePaletteZoom());
tilePalette.addEventListener('scroll',()=>hidePaletteZoom());
window.addEventListener('resize',()=>hidePaletteZoom());
window.addEventListener('blur',()=>hidePaletteZoom());
window.addEventListener('keydown',event=>{if(event.key==='Escape')hidePaletteZoom();});
$('#paletteSource').onchange=()=>{
  if(voxelFacePalette)return;
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
  paletteQuarter=q;refreshPaletteComposer(decodeLayer(paletteRoom(),paletteBg()));
};
$('#paletteUseComposite').onclick=usePaletteComposite;
$('#paletteFacePieceMode').onchange=()=>{
  if(voxelFacePalette&&$('#paletteFacePieceMode').value==='assemble')usePaletteComposite();
};
$('#paletteFaceApply').onclick=applyVoxelFacePalette;
$('#palettePickStamp').onclick=beginPaletteStampPick;
$('#palettePickCancel').onclick=()=>{
  if(paletteStampPick)endPaletteStampPick();else if(voxelFacePalette?.picking)endVoxelFacePick();
};
$('#palettePickMap').onclick=()=>beginVoxelFacePick('2d');
$('#palettePickFace').onclick=()=>beginVoxelFacePick('shared');
$('#paletteFaceFlipH').onclick=()=>flipVoxelFaceArt('h');
$('#paletteFaceFlipV').onclick=()=>flipVoxelFaceArt('v');
$('#paletteFaceCenter').onclick=()=>setVoxelFaceCrop(4,4,8);
$('#paletteFaceSize').onchange=()=>setVoxelFaceCrop(0,0,Number($('#paletteFaceSize').value));
for(const axis of ['X','Y'])$('#paletteFace'+axis).oninput=()=>{
  const x=Number($('#paletteFaceX').value),y=Number($('#paletteFaceY').value);
  if(Number.isFinite(x)&&Number.isFinite(y))setVoxelFaceCrop(x,y);
};
$('#paletteFaceBg').onchange=()=>{
  const bg=Number($('#paletteFaceBg').value);if(!voxelFacePalette||!room.bg[bg])return;
  voxelFacePalette.bg=bg;voxelFacePalette.art=null;
  $('#paletteFaceStatus').textContent='Choose artwork from this background.';refreshTilePalette(true);
};
for(const id of ['palettePieceFlipH','palettePieceFlipV'])$('#'+id).onchange=()=>refreshTilePalette(true);
$('#paletteFaceSource').onclick=event=>{
  if(voxelFacePalette?.art?.size!==8)return;
  const box=event.currentTarget.getBoundingClientRect();
  setVoxelFaceCrop((event.clientX-box.left)*16/box.width-4,(event.clientY-box.top)*16/box.height-4);
};
$('#paletteFaceSource').onkeydown=event=>{
  const art=voxelFacePalette?.art;if(!art)return;
  const step=event.shiftKey?4:1,moves={ArrowLeft:[-step,0],ArrowRight:[step,0],ArrowUp:[0,-step],ArrowDown:[0,step]};
  if(moves[event.key]){paletteConsume(event);setVoxelFaceCrop(art.x+moves[event.key][0],art.y+moves[event.key][1]);}
  if(event.key==='Enter'){paletteConsume(event);applyVoxelFacePalette();}
};
