/* Mirror displayed artwork, including quadrant positions, native CHR flip
 * flags, depth bands and pixel masks. All resulting cells use ordinary stamps. */
const cloneSceneryTile=c=>({...c,words:[...c.words],bands:[...c.bands]});
function flipSceneryTile(tile,axis) {
  const bit=axis==='h'?1:2,flag=axis==='h'?0x4000:0x8000;
  const next=cloneSceneryTile(tile);
  for(let q=0;q<4;q++) {
    next.words[q]=tile.blank?0:tile.words[q^bit]^flag;
    next.bands[q]=tile.bands[q^bit];
  }
  let mask=ZERO_PIXEL_MASK,clear=ZERO_PIXEL_MASK;
  for(let y=0;y<16;y++)for(let x=0;x<16;x++)
    if(pixelIsBlack(tile.black,axis==='h'?15-x:x,axis==='v'?15-y:y))
      mask=pixelMaskSet(mask,x,y,true);
  for(let y=0;y<16;y++)for(let x=0;x<16;x++)
    if(pixelIsBlack(tile.transparent,axis==='h'?15-x:x,axis==='v'?15-y:y))
      clear=pixelMaskSet(clear,x,y,true);
  next.black=mask;
  if(tile.transparent!==undefined||clear!==ZERO_PIXEL_MASK)next.transparent=clear;
  return next;
}
function applyTileUpdates(updates,label) {
  if(!updates.length)return false;
  const key=keyOf(room,bgIndex),target=stampBucket(room,bgIndex),proposed={...target.cells};
  for(const [cell,tile] of updates) {
    if(tile)proposed[cell]=tile;else delete proposed[cell];
  }
  if(regionalPixelCount(room,bgIndex,proposed)>256) {
    sceneryNotice('This operation would exceed 256 pixel edits. No tiles were changed.');return false;
  }
  beginOp(label);
  for(const [cell,tile] of updates) {
    recordStamp(key,cell);
    if(tile)target.cells[cell]=cloneSceneryTile(tile);else delete target.cells[cell];
    const [x,y]=cell.split(',').map(Number);
    if(nativeCell(L,x,y))selectedCells.delete(y*L.cellsW+x);
    selectedStampKeys.add(cell);
  }
  commitOp();
  const focused=activeSelectionPosition();if(focused)focusTileAt(...focused);
  sceneryChanged();tileActionStatus(`${label}: ${updates.length} tiles. Undo reverses this change.`);
  return true;
}
function selectedTileRectangle(positions=tileSelectionPositions()) {
  if(!positions.length)return null;
  let x0=Infinity,y0=Infinity,x1=-Infinity,y1=-Infinity;
  for(const [x,y] of positions) {
    x0=Math.min(x0,x);y0=Math.min(y0,y);x1=Math.max(x1,x);y1=Math.max(y1,y);
  }
  return new Set(positions.map(p=>p.join(','))).size===(x1-x0+1)*(y1-y0+1)
    ?{x0,y0,x1,y1}:null;
}
function flipSelectedTiles(axis,wholeRange=false) {
  const positions=tileSelectionPositions();if(!positions.length||mode!=='2d')return false;
  returnToEditedTiles();
  const rect=wholeRange?selectedTileRectangle(positions):null;
  if(wholeRange&&!rect) {
    sceneryNotice('Select a complete rectangle to mirror the whole range. No tiles were changed.');return false;
  }
  const updates=positions.map(([x,y])=>{
    const sx=wholeRange&&axis==='h'?rect.x0+rect.x1-x:x;
    const sy=wholeRange&&axis==='v'?rect.y0+rect.y1-y:y;
    return [`${x},${y}`,flipSceneryTile(displayedCell(room,bgIndex,L,sx,sy),axis)];
  });
  return applyTileUpdates(updates,`${wholeRange?'Mirror range':'Flip tiles'} ${axis==='h'?'horizontally':'vertically'}`);
}
function flipClipboard(axis) {
  const clip=tileClipboard;if(!clip||clip.key!==keyOf(room,bgIndex))return false;
  const cells=clip.cells.map((_,i)=>{
    const x=i%clip.w,y=Math.floor(i/clip.w);
    const source=clip.cells[(axis==='v'?clip.h-1-y:y)*clip.w+(axis==='h'?clip.w-1-x:x)];
    return source?flipSceneryTile(source,axis):null;
  });
  tileClipboard={...clip,cells,[axis==='h'?'flipH':'flipV']:!clip[axis==='h'?'flipH':'flipV']};
  startStamp();
  tileActionStatus('Stamp source mirrored. Click the map to place it; copied tile flags are preserved.');
  return true;
}
function refreshTileTransformControls() {
  const selected=!!(selectedCells.size||selectedStampKeys.size)&&mode==='2d';
  const rectangle=selected&&!!selectedTileRectangle();
  for(const axis of ['H','V']) {
    $(`#flipSelected${axis}`).disabled=!selected;
    $(`#mirrorSelected${axis}`).disabled=!rectangle;
    const button=$(`#flipClipboard${axis}`),same=tileClipboard?.key===keyOf(room,bgIndex);
    button.disabled=!same;button.classList.toggle('on',same&&!!tileClipboard[`flip${axis}`]);
    button.setAttribute('aria-pressed',String(!!(same&&tileClipboard[`flip${axis}`])));
  }
}
for(const axis of ['h','v']) {
  $(`#flipSelected${axis.toUpperCase()}`).onclick=()=>flipSelectedTiles(axis);
  $(`#mirrorSelected${axis.toUpperCase()}`).onclick=()=>flipSelectedTiles(axis,true);
  $(`#flipClipboard${axis.toUpperCase()}`).onclick=()=>flipClipboard(axis);
}
