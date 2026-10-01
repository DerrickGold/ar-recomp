/* Sparse black/transparent masks on displayed 16x16 cells. ROM graphics and the
 * authentic compositor stay unchanged; only the map authoring/Diorama views
 * apply these presentation edits. */
const ZERO_PIXEL_MASK = '0'.repeat(64);
let pixelCell = null, pixelTool = 'black', pixelDrag = false, lastPixel = null;
function pixelBucket(r,bg) {
  const key=keyOf(r,bg);
  if(!pixelStore[key])pixelStore[key]={byCell:{},byId:{}};
  pixelStore[key].transparent??={byCell:{},byId:{},byCoord:{}};
  return pixelStore[key];
}
function pixelMaskAt(r,bg,layer,cell) {
  const edits=pixelBucket(r,bg);
  return edits.byCell[cell] ?? edits.byId[layer.cellId[cell]] ?? ZERO_PIXEL_MASK;
}
function pixelTransparencyAt(r,bg,layer,cell) {
  const edits=pixelBucket(r,bg),masks=edits.transparent;
  return (edits.byCell[cell]!==undefined?masks.byCell[cell]:masks.byId[layer.cellId[cell]])??ZERO_PIXEL_MASK;
}
function pixelIsBlack(mask,x,y) {
  if(!mask)return false;
  return !!(parseInt(mask.slice(y*4,y*4+4),16)&(1<<(15-x)));
}
function pixelMaskSet(mask,x,y,black) {
  const row=parseInt(mask.slice(y*4,y*4+4),16),bit=1<<(15-x);
  const value=black?row|bit:row&~bit;
  return mask.slice(0,y*4)+value.toString(16).padStart(4,'0').toUpperCase()
    +mask.slice(y*4+4);
}
function loadPixelIni(r,bg,values) {
  if(!r.bg[bg]||(!values.black&&!values.transparent)||
      [values.black,values.transparent].some(mask=>mask!==undefined&&!/^[0-9a-fA-F]{64}$/.test(mask)))return;
  const black=(values.black??ZERO_PIXEL_MASK).toUpperCase();
  const transparent=(values.transparent??ZERO_PIXEL_MASK).toUpperCase();
  for(let row=0;row<16;row++)if(parseInt(black.slice(row*4,row*4+4),16)&
      parseInt(transparent.slice(row*4,row*4+4),16))return;
  const target=pixelBucket(r,bg),width=r.bg[bg].pagesWide*16;
  if(/^[0-9a-fA-F]{1,2}$/.test(values.metatile||'')) {
    const id=parseInt(values.metatile,16);target.byId[id]=black;
    target.transparent.byId[id]=transparent;
  } else {
    const match=(values.cell||'').match(/^(-?\d+),(-?\d+)$/);
    if(!match)return;
    const [x,y]=match.slice(1).map(Number);
    if(x < -512||y < -512||x>65535||y>65535)return;
    if(x>=0&&y>=0&&x<width&&y<r.bg[bg].pagesHigh*16) {
      target.byCell[y*width+x]=black;target.transparent.byCell[y*width+x]=transparent;
    } else {
      (target.byCoord??={})[`${x},${y}`]=black;target.transparent.byCoord[`${x},${y}`]=transparent;
    }
  }
}
function pixelRecords(r,bg,stamps=stampBucket(r,bg).cells,target=pixelBucket(r,bg)) {
  const width=r.bg[bg].pagesWide*16,coords={},transparent=target.transparent??{};
  for(const [cell,mask] of Object.entries(target.byCell))
    coords[`${Number(cell)%width},${Math.floor(Number(cell)/width)}`]=[mask,transparent.byCell?.[cell]];
  for(const [cell,mask] of Object.entries(target.byCoord??{}))coords[cell]=[mask,transparent.byCoord?.[cell]];
  for(const [cell,c] of Object.entries(stamps)) {
    /* Zero overrides can suppress inherited black or transparent masks. */
    if(c.black!==ZERO_PIXEL_MASK||(c.transparent??ZERO_PIXEL_MASK)!==ZERO_PIXEL_MASK||
        !c.blank&&target.byId[c.id]!==undefined||coords[cell]!==undefined)
      coords[cell]=[c.black,c.transparent];
  }
  const masks=(black,clear)=>`black:${black}`+(clear&&clear!==ZERO_PIXEL_MASK?` transparent:${clear}`:'');
  return [...Object.entries(target.byId).sort((a,b)=>Number(a[0])-Number(b[0]))
    .map(([id,mask])=>`metatile:${Number(id).toString(16).padStart(2,'0').toUpperCase()} ${masks(mask,transparent.byId?.[id])}`),
    ...Object.entries(coords).sort((a,b)=>a[0].localeCompare(b[0]))
      .map(([cell,[black,clear]])=>`cell:${cell} ${masks(black,clear)}`)];
}
function pixelIniLines(r) {
  const lines=[];
  for(let bg=0;bg<2;bg++) {
    if(!r.bg[bg])continue;
    const records=regionalRecords(r,bg,variant=>pixelRecords(variant,bg)
      .map(record=>`bg${bg+1}-pixels = ${record}`));
    if(records.length>256)throw new Error('Maximum 256 pixel edits per background');
    lines.push(...records);
  }
  return lines;
}
function focusedStamp() {
  return pixelStamp?displayedCell(room,bgIndex,L,...pixelStamp.split(',').map(Number)):null;
}
function focusedMask() {
  const stamp=focusedStamp();if(stamp)return stamp.black;
  const target=pixelBucket(room,bgIndex);
  return $('#pixelScope').value==='id'
    ?target.byId[L.cellId[pixelCell]]??ZERO_PIXEL_MASK
    :pixelMaskAt(room,bgIndex,L,pixelCell);
}
function focusedTransparency() {
  const stamp=focusedStamp();if(stamp)return stamp.transparent??ZERO_PIXEL_MASK;
  const target=pixelBucket(room,bgIndex);
  return $('#pixelScope').value==='id'
    ?target.transparent.byId[L.cellId[pixelCell]]??ZERO_PIXEL_MASK
    :pixelTransparencyAt(room,bgIndex,L,pixelCell);
}
function focusedOriginal(x,y) {
  const stamp=focusedStamp();return stamp?stampOriginal(L,stamp,x,y):pixelOriginal(L,pixelCell,x,y);
}
function pixelOriginal(layer,cell,x,y) {
  const cx=cell%layer.cellsW,cy=Math.floor(cell/layer.cellsW);
  const entry=layer.words[(cy*2+(y>>3))*layer.tilesW+cx*2+(x>>3)];
  const value=nativeCharacterPixel(layer,entry,x&7,y&7);
  return value?layer.pal[((entry>>10)&7)*16+value]:null;
}
function refreshPixelEditor() {
  const pasted=focusedStamp();
  const active=!!L&&(!!pasted||(pixelCell!==null&&pixelCell<L.cellId.length));
  $('#pixelControls').style.display=active?'':'none';
  const canvas=$('#pixelCanvas');canvas.style.display=active?'':'none';
  if(!active){$('#pixelInfo').textContent='Click a map cell with Select cells.';return;}
  const cell=pixelCell,[cx,cy]=pasted?pixelStamp.split(',').map(Number)
    :[cell%L.cellsW,Math.floor(cell/L.cellsW)];
  $('#pixelScope').disabled=!!pasted;
  $('#pixelInfo').textContent=`BG${bgIndex+1} cell ${cx},${cy} · `+(pasted?.blank?'blank tile'
    :`metatile ${(pasted?pasted.id:L.cellId[cell]).toString(16).padStart(2,'0').toUpperCase()}`);
  $('#pixelReset').textContent=!pasted&&$('#pixelScope').value==='id'
    ?'Reset pixel edits for this metatile':'Reset pixel edits for this cell';
  const mask=pasted?pasted.black:pixelMaskAt(room,bgIndex,L,cell);
  const clear=pasted?pasted.transparent:pixelTransparencyAt(room,bgIndex,L,cell),image=new ImageData(224,224);
  const colors=new Uint32Array(256);
  for(let py=0;py<16;py++)for(let px=0;px<16;px++) {
    const color=pixelIsBlack(clear,px,py)?null:pixelIsBlack(mask,px,py)?0xff000000:focusedOriginal(px,py);
    colors[py*16+px]=color??(((px+py)&1)?0xff536078:0xff303a4d);
  }
  for(let y=0;y<224;y++)for(let x=0;x<224;x++) {
    let color=colors[Math.floor(y/14)*16+Math.floor(x/14)];
    if(x%14===0||y%14===0)color=0xff242b37;
    const o=(y*224+x)*4;
    image.data[o]=color&255;image.data[o+1]=(color>>>8)&255;
    image.data[o+2]=(color>>>16)&255;image.data[o+3]=255;
  }
  canvas.getContext('2d').putImageData(image,0,0);
}
function writePixelMask(mask,reset=false,transparent=focusedTransparency()) {
  const pasted=focusedStamp();
  if(pasted) {
    const key=keyOf(room,bgIndex),next={...pasted,black:reset?ZERO_PIXEL_MASK:mask,
      transparent:reset?ZERO_PIXEL_MASK:transparent};
    if(next.black===pasted.black&&next.transparent===(pasted.transparent??ZERO_PIXEL_MASK))return;
    const cells={...stampBucket(room,bgIndex).cells,[pixelStamp]:next};
    if(regionalPixelCount(room,bgIndex,cells)>256) {
      $('#pixelInfo').textContent='Maximum 256 pixel edits per background';return;
    }
    recordStamp(key,pixelStamp);stampBucket(room,bgIndex).cells[pixelStamp]=next;
    sceneryChanged();return;
  }
  if(pixelCell===null)return;
  const key=keyOf(room,bgIndex),target=pixelBucket(room,bgIndex);
  const byId=$('#pixelScope').value==='id';
  const id=byId?L.cellId[pixelCell]:pixelCell;
  const dict=byId?target.byId:target.byCell,kind=byId?'pixelId':'pixelCell';
  const clearDict=byId?target.transparent.byId:target.transparent.byCell;
  const clearKind=byId?'transparentId':'transparentCell';
  const inherited=target.byId[L.cellId[pixelCell]]??ZERO_PIXEL_MASK;
  const inheritedClear=target.transparent.byId[L.cellId[pixelCell]]??ZERO_PIXEL_MASK;
  const value=reset||(mask===ZERO_PIXEL_MASK&&transparent===ZERO_PIXEL_MASK&&
      (byId||inherited===ZERO_PIXEL_MASK&&inheritedClear===ZERO_PIXEL_MASK))
    ?undefined:mask;
  const clearValue=value===undefined||transparent===ZERO_PIXEL_MASK?undefined:transparent;
  if(dict[id]===value&&clearDict[id]===clearValue)return;
  const proposed={...target,byCell:{...target.byCell},byId:{...target.byId},
    transparent:{...target.transparent,byCell:{...target.transparent.byCell},byId:{...target.transparent.byId}}};
  setKey(byId?proposed.byId:proposed.byCell,id,value);
  setKey(byId?proposed.transparent.byId:proposed.transparent.byCell,id,clearValue);
  if(regionalPixelCount(room,bgIndex,stampBucket(room,bgIndex).cells,proposed)>256) {
    $('#pixelInfo').textContent='Maximum 256 pixel edits per background';return;
  }
  const part=partFor(key);
  if(part&&!(id in part.before[kind]))part.before[kind][id]=dict[id];
  if(part&&!(id in part.before[clearKind]))part.before[clearKind][id]=clearDict[id];
  /* A local zero mask intentionally restores original art despite an inherited
   * metatile edit. Reset removes that override and inherits again. */
  setKey(dict,id,value);
  setKey(clearDict,id,clearValue);
  markEditorChanged();surfacesDirty=compositeDirty=glDirty=true;
  invalidateGameComposite();invalidateOther();refreshPixelEditor();draw();
}
function editPixel(x,y,tool=pixelTool) {
  if((pixelCell===null&&!focusedStamp())||x<0||y<0||x>=16||y>=16)return;
  if(typeof tool==='boolean')tool=tool?'black':'restore';
  writePixelMask(pixelMaskSet(focusedMask(),x,y,tool==='black'),false,
    pixelMaskSet(focusedTransparency(),x,y,tool==='transparent'));
}
function pixelBulk(kind) {
  if(pixelCell===null&&!focusedStamp())return;
  beginOp(kind==='reset'?'reset pixel edits':`black ${kind} pixels`);
  let mask=focusedMask(),clear=focusedTransparency();
  for(let y=0;y<16;y++)for(let x=0;x<16;x++)
    if(kind==='whole'||(kind==='transparent'&&
        (focusedOriginal(x,y)===null||pixelIsBlack(clear,x,y)))) {
      mask=pixelMaskSet(mask,x,y,true);
      clear=pixelMaskSet(clear,x,y,false);
    }
  writePixelMask(mask,kind==='reset',clear);commitOp();
}
function fillTransparentMask(mask,original,transparent=ZERO_PIXEL_MASK) {
  let result='';
  for(let y=0;y<16;y++) {
    let row=parseInt(mask.slice(y*4,y*4+4),16);
    for(let x=0;x<16;x++)if(original(x,y)===null||pixelIsBlack(transparent,x,y))row|=1<<(15-x);
    result+=row.toString(16).padStart(4,'0').toUpperCase();
  }
  return result;
}
/* Selection fill always authors placed cells, regardless of the single-tile
 * scope selector. Validate the complete operation before writing any mask. */
function fillSelectedTransparency() {
  const positions=new Set([...selectedCells].map(cell=>
    `${cell%L.cellsW},${Math.floor(cell/L.cellsW)}`).concat([...selectedStampKeys]));
  const info=$('#selectionPixelInfo');
  if(!positions.size){info.textContent='Select tiles to fill their transparency.';return false;}
  const key=keyOf(room,bgIndex),pixels=pixelBucket(room,bgIndex),stamps=stampBucket(room,bgIndex);
  const proposedPixels={...pixels,byCell:{...pixels.byCell},
    transparent:{...pixels.transparent,byCell:{...pixels.transparent.byCell}}};
  const proposedStamps={...stamps.cells},changes=[];
  for(const position of positions) {
    const [x,y]=position.split(',').map(Number);
    const pasted=stamps.cells[position]||(!nativeCell(L,x,y)?displayedCell(room,bgIndex,L,x,y):null);
    if(pasted) {
      const mask=fillTransparentMask(pasted.black,(px,py)=>stampOriginal(L,pasted,px,py),pasted.transparent);
      if(mask===pasted.black&&(pasted.transparent??ZERO_PIXEL_MASK)===ZERO_PIXEL_MASK)continue;
      proposedStamps[position]={...pasted,black:mask,transparent:ZERO_PIXEL_MASK};
      changes.push({position,pasted:proposedStamps[position]});
    } else {
      if(x<0||y<0||x>=L.cellsW||y>=L.cellsH)continue;
      const cell=y*L.cellsW+x,previous=pixelMaskAt(room,bgIndex,L,cell);
      const clear=pixelTransparencyAt(room,bgIndex,L,cell);
      const mask=fillTransparentMask(previous,(px,py)=>pixelOriginal(L,cell,px,py),clear);
      if(mask===previous&&clear===ZERO_PIXEL_MASK)continue;
      delete proposedPixels.transparent.byCell[cell];
      proposedPixels.byCell[cell]=mask;changes.push({cell,mask});
    }
  }
  if(!changes.length){info.textContent='Selected transparency is already filled.';return false;}
  const records=regionalPixelCount(room,bgIndex,proposedStamps,proposedPixels);
  if(records>256) {
    info.textContent=`This selection needs ${records} pixel edits; the limit is 256 per BG. `
      +'Select a smaller range. No pixels were changed.';
    return false;
  }
  beginOp('fill selected transparency black');
  for(const change of changes) {
    if(change.pasted) {
      recordStamp(key,change.position);stamps.cells[change.position]=change.pasted;
    } else {
      const part=partFor(key);
      part.before.pixelCell[change.cell]=pixels.byCell[change.cell];
      part.before.transparentCell[change.cell]=pixels.transparent.byCell[change.cell];
      pixels.byCell[change.cell]=change.mask;
      delete pixels.transparent.byCell[change.cell];
    }
  }
  commitOp();sceneryChanged();
  info.textContent=`Filled transparency black on ${changes.length} selected tile${changes.length===1?'':'s'}.`;
  return true;
}
$('#pixelPick').onclick=()=>openPixelInspector();
$('#pixelScope').onchange=refreshPixelEditor;
for(const [id,tool] of [['pixelBlack','black'],['pixelClear','transparent'],['pixelRestore','restore']])
  $(`#${id}`).onclick=()=>{
    pixelTool=tool;
    for(const name of ['pixelBlack','pixelClear','pixelRestore']) {
      $(`#${name}`).classList.toggle('on',name===id);
      $(`#${name}`).setAttribute('aria-pressed',name===id?'true':'false');
    }
  };
$('#pixelTransparent').onclick=()=>pixelBulk('transparent');
$('#pixelFill').onclick=()=>pixelBulk('whole');
$('#pixelReset').onclick=()=>pixelBulk('reset');
$('#pixelSelectionBlack').onclick=$('#pixelSelectionBlackQuick').onclick=()=>{
  const changed=fillSelectedTransparency();tileActionStatus($('#selectionPixelInfo').textContent);return changed;
};
const pixelPosition=ev=>{const rect=$('#pixelCanvas').getBoundingClientRect();
  return [Math.floor((ev.clientX-rect.left)*16/rect.width),
    Math.floor((ev.clientY-rect.top)*16/rect.height)];};
$('#pixelCanvas').addEventListener('pointerdown',ev=>{
  if(ev.button!==0||(pixelCell===null&&!focusedStamp()))return;
  ev.preventDefault();$('#pixelCanvas').setPointerCapture(ev.pointerId);
  beginOp(pixelTool==='restore'?'restore pixels':`paint ${pixelTool} pixels`);
  pixelDrag=true;lastPixel=pixelPosition(ev);editPixel(...lastPixel);
});
$('#pixelCanvas').addEventListener('pointermove',ev=>{
  if(!pixelDrag)return;
  const current=pixelPosition(ev),dx=current[0]-lastPixel[0],dy=current[1]-lastPixel[1];
  const steps=Math.max(Math.abs(dx),Math.abs(dy));
  for(let i=1;i<=steps;i++)editPixel(Math.round(lastPixel[0]+dx*i/steps),
    Math.round(lastPixel[1]+dy*i/steps));
  lastPixel=current;
});
const finishPixelStroke=()=>{if(pixelDrag){pixelDrag=false;commitOp();}};
$('#pixelCanvas').addEventListener('pointerup',finishPixelStroke);
$('#pixelCanvas').addEventListener('pointercancel',finishPixelStroke);
$('#pixelCanvas').addEventListener('lostpointercapture',finishPixelStroke);
