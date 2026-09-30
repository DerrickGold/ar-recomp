/* Frozen displayed tile words keep pasted scenery independent of the source
 * rectangle. Coordinates are signed 16px cells; the original map is immutable. */
const stampStore = {}, selectedStampKeys = new Set();
const kStampMax = 512;
let selectionRect = null, tileClipboard = null, stampHover = null, pixelStamp = null;
function sceneryNotice(message) {
  $('#stampInfo').textContent=message;tileActionStatus(message);
}
function stampBucket(r,bg) {
  return stampStore[keyOf(r,bg)] ??= {cells:{},bounds:undefined};
}
function mapBounds(r,bg,layer) {
  const b=stampBucket(r,bg),base={x0:0,y0:0,x1:layer.cellsW,y1:layer.cellsH};
  const result={...(b.bounds||base)};
  result.x0=Math.min(0,result.x0);result.y0=Math.min(0,result.y0);
  result.x1=Math.max(base.x1,result.x1);result.y1=Math.max(base.y1,result.y1);
  for(const key of Object.keys(b.cells)) {
    const [x,y]=key.split(',').map(Number);
    result.x0=Math.min(result.x0,x);result.y0=Math.min(result.y0,y);
    result.x1=Math.max(result.x1,x+1);result.y1=Math.max(result.y1,y+1);
  }
  return result;
}
function recordStamp(key,cell) {
  const part=partFor(key);
  if(part&&!(cell in part.before.stamp))
    part.before.stamp[cell]=stampStore[key]?.cells[cell];
}
function recordBounds(key) {
  const part=partFor(key);
  if(part&&!('bounds' in part.before))part.before.bounds=stampStore[key]?.bounds;
}
function sceneryChanged() {
  markEditorChanged();surfacesDirty=compositeDirty=glDirty=true;
  invalidateOther();invalidateGameComposite();refreshSelectionControls();draw();
}
function displayedCell(r,bg,layer,x,y) {
  const pasted=stampBucket(r,bg).cells[`${x},${y}`];
  if(pasted)return pasted;
  if(x<0||y<0||x>=layer.cellsW||y>=layer.cellsH)return null;
  const cell=y*layer.cellsW+x,words=[],bands=[];
  for(let q=0;q<4;q++) {
    const tx=x*2+(q&1),ty=y*2+(q>>1);
    words.push(layer.words[ty*layer.tilesW+tx]);
    bands.push(bandAt(bucket(r,bg),layer,tx,ty));
  }
  return {words,bands,id:layer.cellId[cell],black:pixelMaskAt(r,bg,layer,cell)};
}
function stampOriginal(layer,stamp,x,y) {
  const entry=stamp.words[(y>>3)*2+(x>>3)];
  const value=nativeCharacterPixel(layer,entry,x&7,y&7);
  return value?layer.pal[((entry>>10)&7)*16+value]:null;
}
function stampColor(layer,stamp,x,y) {
  return pixelIsBlack(stamp.black,x,y)?0xff000000:stampOriginal(layer,stamp,x,y);
}
function selectRectangle(x0,y0,x1,y1) {
  const bounds=mapBounds(room,bgIndex,L);
  selectionRect={x0:Math.max(bounds.x0,Math.min(x0,x1)),
    y0:Math.max(bounds.y0,Math.min(y0,y1)),
    x1:Math.min(bounds.x1,Math.max(x0,x1)+1),
    y1:Math.min(bounds.y1,Math.max(y0,y1)+1)};
  selectedCells.clear();selectedStampKeys.clear();
  const b=selectionRect;
  if(b.x1<=b.x0||b.y1<=b.y0) {
    selectionRect=null;selectionAnchor=null;focusTileAt(-513,-513);
    refreshSelectionControls();draw();return;
  }
  setSelectionAnchor(Math.max(bounds.x0,Math.min(bounds.x1-1,x0)),
    Math.max(bounds.y0,Math.min(bounds.y1-1,y0)));
  for(let y=b.y0;y<b.y1;y++)for(let x=b.x0;x<b.x1;x++) {
    if(stampBucket(room,bgIndex).cells[`${x},${y}`])selectedStampKeys.add(`${x},${y}`);
    else if(x>=0&&y>=0&&x<L.cellsW&&y<L.cellsH)selectedCells.add(y*L.cellsW+x);
  }
  focusTileAt(Math.max(b.x0,Math.min(b.x1-1,x1)),Math.max(b.y0,Math.min(b.y1-1,y1)));
  refreshSelectionControls();draw();
}
function copyTiles() {
  returnToEditedTiles();
  let b=selectionRect;
  if(!b) {
    const positions=[...selectedCells].map(c=>[c%L.cellsW,Math.floor(c/L.cellsW)])
      .concat([...selectedStampKeys].map(k=>k.split(',').map(Number)));
    if(!positions.length)return false;
    b={x0:Math.min(...positions.map(p=>p[0])),y0:Math.min(...positions.map(p=>p[1])),
      x1:Math.max(...positions.map(p=>p[0]))+1,y1:Math.max(...positions.map(p=>p[1]))+1};
  }
  const w=b.x1-b.x0,h=b.y1-b.y0;
  if(w<=0||h<=0||w*h>kStampMax) {
    sceneryNotice('Select a rectangle of up to 512 tiles.');return false;
  }
  const cells=[];
  for(let y=b.y0;y<b.y1;y++)for(let x=b.x0;x<b.x1;x++) {
    const cell=displayedCell(room,bgIndex,L,x,y);
    cells.push(cell?{...cell,words:[...cell.words],bands:[...cell.bands]}:null);
  }
  if(!cells.some(Boolean)) {
    sceneryNotice('The selection contains no tile art.');return false;
  }
  tileClipboard={key:keyOf(room,bgIndex),w,h,cells};
  $('#pasteX').value=String(b.x1);$('#pasteY').value=String(b.y0);
  refreshStampControls();refreshEditorFeedback();
  tileActionStatus(`Copied ${w}×${h} tiles. Choose Paste over, or Stamp to place copies.`);return true;
}
function stampTiles(x,y) {
  const clip=tileClipboard,key=keyOf(room,bgIndex),target=stampBucket(room,bgIndex);
  if(!clip||clip.key!==key) {
    sceneryNotice('Copy tiles from this room and background first.');return false;
  }
  if(!Number.isInteger(x)||!Number.isInteger(y)||x<-512||y<-512||
      x+clip.w>512||y+clip.h>512) {
    sceneryNotice('Paste coordinates must stay between −512 and 511.');return false;
  }
  const added=clip.cells.reduce((n,c,i)=>n+(!!c&&!target.cells[
    `${x+i%clip.w},${y+Math.floor(i/clip.w)}`]),0);
  if(Object.keys(target.cells).length+added>kStampMax) {
    sceneryNotice('Maximum 512 pasted tiles per background; remove some first.');
    return false;
  }
  /* Check the shared pixel-record budget before mutating anything. */
  const proposed={...target.cells};
  clip.cells.forEach((c,i)=>{if(c)proposed[
    `${x+i%clip.w},${y+Math.floor(i/clip.w)}`]=c;});
  if(regionalPixelCount(room,bgIndex,proposed)>256) {
    sceneryNotice('This paste would exceed 256 pixel edits. No tiles were changed.');
    return false;
  }
  if(regionalStampCount(room,bgIndex,proposed)>kStampMax) {
    sceneryNotice('Maximum 512 saved pasted tiles per BG across terrain versions.');
    return false;
  }
  const nextBounds=mapBounds(room,bgIndex,L);
  nextBounds.x0=Math.min(nextBounds.x0,x);nextBounds.y0=Math.min(nextBounds.y0,y);
  nextBounds.x1=Math.max(nextBounds.x1,x+clip.w);
  nextBounds.y1=Math.max(nextBounds.y1,y+clip.h);
  if(nextBounds.x1-nextBounds.x0>512||nextBounds.y1-nextBounds.y0>512) {
    sceneryNotice('The expanded map can span up to 512 tiles on each axis.');
    return false;
  }
  beginOp('paste tiles');recordBounds(key);target.bounds=nextBounds;
  clip.cells.forEach((c,i)=>{if(c) {
    const cell=`${x+i%clip.w},${y+Math.floor(i/clip.w)}`;
    recordStamp(key,cell);target.cells[cell]=c;
  }});
  commitOp();selectRectangle(x,y,x+clip.w-1,y+clip.h-1);sceneryChanged();
  tileActionStatus(`Pasted ${clip.w}×${clip.h} tiles at ${x}, ${y}. Undo reverses this paste.`);return true;
}
function expandEdge(edge) {
  const n=Number($('#edgeCount').value),key=keyOf(room,bgIndex),b=mapBounds(room,bgIndex,L);
  if(!Number.isInteger(n)||n<1||n>64)return;
  const next={...b};next[edge]+=edge.endsWith('0')?-n:n;
  if(next.x0<-512||next.y0<-512||next.x1>512||next.y1>512||
      next.x1-next.x0>512||next.y1-next.y0>512) {
    sceneryNotice('The expanded map can span up to 512 tiles on each axis.');return;
  }
  beginOp('add edge space');recordBounds(key);stampBucket(room,bgIndex).bounds=next;
  commitOp();sceneryChanged();fitView();draw();
}
function removeStamps(all=false) {
  const key=keyOf(room,bgIndex),target=stampBucket(room,bgIndex);
  const cells=all?Object.keys(target.cells):[...selectedStampKeys];
  beginOp(all?'reset pasted scenery':'remove pasted tiles');
  for(const cell of cells){recordStamp(key,cell);delete target.cells[cell];}
  if(all){recordBounds(key);target.bounds=undefined;}
  commitOp();deselect();sceneryChanged();if(all)fitView();draw();
}
function paintStampBands(cx,cy,value,all=false) {
  const key=keyOf(room,bgIndex),target=stampBucket(room,bgIndex);
  const source=target.cells[`${cx},${cy}`];
  if(!source)return false;
  for(const [cell,c] of Object.entries(target.cells)) {
    if(all?c.id!==source.id:cell!==`${cx},${cy}`)continue;
    recordStamp(key,cell);target.cells[cell]={...c,
      bands:c.words.map(word=>value===null?authenticBand(word):value)};
  }
  sceneryChanged();return true;
}
function loadStampIni(r,bg,values) {
  const target=stampBucket(r,bg);
  if(values.bounds) {
    if(!/^-?\d+,-?\d+,-?\d+,-?\d+$/.test(values.bounds))return;
    const [x0,y0,x1,y1]=values.bounds.split(',').map(Number);
    if(x0>=-512&&y0>=-512&&x1<=512&&y1<=512&&x1>x0&&y1>y0&&
        x1-x0<=512&&y1-y0<=512)target.bounds={x0,y0,x1,y1};
    return;
  }
  if(!/^-?\d+,-?\d+$/.test(values.cell||'')||
      !/^[0-9a-fA-F]{1,4}(,[0-9a-fA-F]{1,4}){3}$/.test(values.words||'')||
      !/^[012](,[012]){3}$/.test(values.bands||'')||
      !/^[0-9a-fA-F]{1,2}$/.test(values.metatile||''))return;
  const [x,y]=values.cell.split(',').map(Number),cell=`${x},${y}`;
  if(x<-512||y<-512||x>=512||y>=512||
      (!target.cells[cell]&&Object.keys(target.cells).length>=kStampMax))return;
  target.cells[cell]={words:values.words.split(',').map(v=>parseInt(v,16)),
    bands:values.bands.split(',').map(Number),id:parseInt(values.metatile,16),
    black:ZERO_PIXEL_MASK};
}
function hydrateStampMasks() {
  for(const base of DATA.rooms)for(let bg=0;bg<2;bg++)
    for(const {room:r} of editFamilies(base,bg)) {
    if(!r.bg[bg])continue;
    const pixels=pixelBucket(r,bg),w=r.bg[bg].pagesWide*16,h=r.bg[bg].pagesHigh*16;
    for(const [cell,c] of Object.entries(stampBucket(r,bg).cells)) {
      const [x,y]=cell.split(',').map(Number);
      c.black=pixels.byCoord?.[cell]??
        (x>=0&&y>=0&&x<w&&y<h?pixels.byCell[y*w+x]:undefined)??
        pixels.byId[c.id]??ZERO_PIXEL_MASK;
      if(pixels.byCoord)delete pixels.byCoord[cell];
      if(x>=0&&y>=0&&x<w&&y<h)delete pixels.byCell[y*w+x];
    }
  }
}
function stampRecords(r,bg,cells=stampBucket(r,bg).cells) {
  const hex=n=>n.toString(16).toUpperCase().padStart(4,'0');
  return Object.entries(cells).sort((a,b)=>{
    const [ax,ay]=a[0].split(',').map(Number),[bx,by]=b[0].split(',').map(Number);
    return ay-by||ax-bx;
  }).map(([cell,c])=>`bg${bg+1}-stamp = cell:${cell} metatile:${hex(c.id).slice(-2)} `
    +`words:${c.words.map(hex).join(',')} bands:${c.bands.join(',')}`);
}
function stampIniLines(r) {
  const lines=[];
  for(let bg=0;bg<2;bg++) {
    const records=regionalRecords(r,bg,variant=>{
      const b=stampBucket(variant,bg).bounds;
      return [...(b?[`bg${bg+1}-map = bounds:${b.x0},${b.y0},${b.x1},${b.y1}`]:[]),
        ...stampRecords(variant,bg)];
    });
    if(records.filter(line=>line.includes('-stamp =')).length>kStampMax)
      throw new Error('Maximum 512 saved pasted tiles per BG across terrain versions');
    lines.push(...records);
  }
  return lines;
}
function refreshStampControls() {
  const same=tileClipboard?.key===keyOf(room,bgIndex);
  $('#copyTiles').disabled=!selectedCells.size&&!selectedStampKeys.size&&!selectionRect;
  $('#copyTilesQuick').disabled=$('#copyTiles').disabled;
  $('#stampTool').disabled=$('#pasteTiles').disabled=!same;
  $('#stampQuick').disabled=!same;
  $('#removeStamps').disabled=!selectedStampKeys.size;
  const b=mapBounds(room,bgIndex,L),n=Object.keys(stampBucket(room,bgIndex).cells).length;
  $('#stampInfo').textContent=(tileClipboard?`Copied ${tileClipboard.w}×${tileClipboard.h}. `:'')
    +`${n}/512 pasted tiles · bounds ${b.x0},${b.y0} to ${b.x1-1},${b.y1-1}.`
    +(tileClipboard&&!same?' Switch back to the source room/BG to paste.':'');
}
function startStamp() {
  if(tileClipboard?.key!==keyOf(room,bgIndex))return;
  setMode('2d');brush='stamp';stampHover=null;
  Object.values(brushBtns).forEach(b=>b.classList.remove('on'));
  $('#stampTool').classList.add('on');draw();
  $('#stampQuick').classList.add('on');$('#selectRectQuick').classList.remove('on');
  cvs.style.cursor='crosshair';
  refreshSelectionControls();
}
function drawStampGuides() {
  const b=mapBounds(room,bgIndex,L);
  const outline=(r,color)=>{ctx.strokeStyle=color;ctx.lineWidth=1.5;
    ctx.strokeRect(view.x+r.x0*16*view.scale,view.y+r.y0*16*view.scale,
      (r.x1-r.x0)*16*view.scale,(r.y1-r.y0)*16*view.scale);};
  outline(b,'#6b7487');
  for(const key of selectedStampKeys) {
    const [x,y]=key.split(',').map(Number);
    outline({x0:x,y0:y,x1:x+1,y1:y+1},'#5aa9ff');
  }
  const rect=drag?.selectRect?{x0:Math.min(drag.x0,drag.x1),y0:Math.min(drag.y0,drag.y1),
    x1:Math.max(drag.x0,drag.x1)+1,y1:Math.max(drag.y0,drag.y1)+1}:selectionRect;
  if(rect)outline(rect,'#5aa9ff');
  if(!compareOriginal&&brush==='stamp'&&stampHover&&tileClipboard?.key===keyOf(room,bgIndex)) {
    const [x,y]=stampHover,clip=tileClipboard;
    ctx.save();ctx.globalAlpha=.55;
    const ghost=document.createElement('canvas');ghost.width=clip.w*16;ghost.height=clip.h*16;
    const image=new ImageData(ghost.width,ghost.height);
    clip.cells.forEach((c,i)=>{if(c)blitStamp(L,c,image,ghost.width,i%clip.w*16,
      Math.floor(i/clip.w)*16);});
    ghost.getContext('2d').putImageData(image,0,0);
    ctx.drawImage(ghost,view.x+x*16*view.scale,view.y+y*16*view.scale,
      ghost.width*view.scale,ghost.height*view.scale);ctx.restore();
    outline({x0:x,y0:y,x1:x+clip.w,y1:y+clip.h},'#ffdc74');
  }
}
function blitStamp(layer,c,image,width,ox,oy,high=null) {
  for(let y=0;y<16;y++)for(let x=0;x<16;x++) {
    const q=(y>>3)*2+(x>>3);
    if(high!==null&&!!(c.words[q]&0x2000)!==!!high)continue;
    const color=stampColor(layer,c,x,y);if(color===null)continue;
    const o=((oy+y)*width+ox+x)*4,data=image.data;
    let red=color&255,green=(color>>>8)&255,blue=(color>>>16)&255;
    if(tint&&!pixelIsBlack(c.black,x,y)) {
      const rgb=bandRGB(c.bands[q]);red=(red*3+rgb[0])>>2;
      green=(green*3+rgb[1])>>2;blue=(blue*3+rgb[2])>>2;
    }
    data[o]=red;data[o+1]=green;data[o+2]=blue;data[o+3]=255;
  }
}
$('#copyTiles').onclick=copyTiles;
$('#copyTilesQuick').onclick=copyTiles;
$('#selectRectQuick').onclick=()=>{setMode('2d');$('#bSelectRect').onclick();};
$('#stampQuick').onclick=startStamp;
$('#stampTool').onclick=startStamp;
$('#pasteTiles').onclick=()=>stampTiles(Number($('#pasteX').value),Number($('#pasteY').value));
$('#edgeLeft').onclick=()=>expandEdge('x0');$('#edgeRight').onclick=()=>expandEdge('x1');
$('#edgeTop').onclick=()=>expandEdge('y0');$('#edgeBottom').onclick=()=>expandEdge('y1');
$('#removeStamps').onclick=()=>removeStamps();$('#resetStamps').onclick=()=>removeStamps(true);
