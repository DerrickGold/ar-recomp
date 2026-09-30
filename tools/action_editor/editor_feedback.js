/* Selection, authored data and export state are separate. These controls
 * inspect the same buckets as rendering; choosing a tile never authors a rule. */
let editorRevision=0,feedbackRevision=-1,feedbackText='',exportBaseline='',savepointKind='loaded';
let compareOriginal=false,changeCache=null,highlightMode='modified',highlightCache=null;
const pixelInspector=$('#pixelInspector');
pixelInspector.hidden=true;
const BAND_LABELS=['Far','Normal','Priority'];
function returnToEditedTiles() {
  if(!compareOriginal)return;
  compareOriginal=false;draw();
}
function markEditorChanged() {
  editorRevision++;configDirty=true;changeCache=null;returnToEditedTiles();
}
function currentEditorText() {
  if(feedbackRevision!==editorRevision) {
    feedbackText=mergeDioramaIni();feedbackRevision=editorRevision;
  }
  return feedbackText;
}
function captureEditorSavepoint(kind='loaded') {
  editorRevision++;feedbackRevision=-1;changeCache=null;
  exportBaseline=currentEditorText();savepointKind=kind;configDirty=false;
  if(kind==='exported')refreshEditorFeedback();
}
function editorHasUnexportedChanges() {
  try {configDirty=currentEditorText()!==exportBaseline;}
  catch {configDirty=true;}
  return configDirty;
}
function tileChangeKinds(x,y) {
  const stamp=stampBucket(room,bgIndex).cells[`${x},${y}`];
  if(stamp)return {pasted:true,pixels:stamp.black!==ZERO_PIXEL_MASK,
    band:stamp.bands.some((b,q)=>b!==authenticBand(stamp.words[q]))};
  if(x<0||y<0||x>=L.cellsW||y>=L.cellsH)return {};
  const cell=y*L.cellsW+x,id=L.cellId[cell],pixels=pixelBucket(room,bgIndex);
  return {band:st.byCell[cell]!==undefined||st.byId[id]!==undefined,
    pixels:pixels.byCell[cell]!==undefined||pixels.byId[id]!==undefined};
}
function currentTileChanges() {
  const key=keyOf(room,bgIndex);
  if(changeCache?.key===key&&changeCache.revision===editorRevision)return changeCache;
  const cells=[];let bands=0,pixels=0,pasted=0;
  const add=(x,y)=>{
    const kind=tileChangeKinds(x,y);
    if(!kind.band&&!kind.pixels&&!kind.pasted)return;
    cells.push({x,y,...kind});bands+=!!kind.band;pixels+=!!kind.pixels;pasted+=!!kind.pasted;
  };
  const stamps=stampBucket(room,bgIndex);
  for(let y=0;y<L.cellsH;y++)for(let x=0;x<L.cellsW;x++)
    if(!stamps.cells[`${x},${y}`])add(x,y);
  for(const key of Object.keys(stamps.cells))add(...key.split(',').map(Number));
  cells.sort((a,b)=>a.y-b.y||a.x-b.x);
  return changeCache={key,revision:editorRevision,cells,bands,pixels,pasted,bounds:!!stamps.bounds};
}
function highlightLabel() {
  return highlightMode==='modified'?'Modified tiles':`${BAND_LABELS[Number(highlightMode)]} band`;
}
function highlightColor() {
  return highlightMode==='modified'?'#f0c55a':BANDS[Number(highlightMode)].css;
}
/* Bands resolve per 8px quadrant. A mixed 16px tile matches each of its bands;
 * pasted scenery replaces the underlying tile for filtering, just as in rendering. */
function currentHighlightedTiles() {
  if(highlightMode==='modified')return currentTileChanges().cells;
  const key=keyOf(room,bgIndex);
  if(highlightCache?.key===key&&highlightCache.revision===editorRevision&&highlightCache.mode===highlightMode)
    return highlightCache.cells;
  const cells=[],target=Number(highlightMode),stamps=stampBucket(room,bgIndex).cells;
  for(let y=0;y<L.cellsH;y++)for(let x=0;x<L.cellsW;x++) {
    if(stamps[`${x},${y}`])continue;
    for(let q=0;q<4;q++)if(bandAt(st,L,x*2+(q&1),y*2+(q>>1))===target) {
      cells.push({x,y,pasted:false});break;
    }
  }
  for(const [coord,tile] of Object.entries(stamps))if(tile.bands.includes(target)) {
    const [x,y]=coord.split(',').map(Number);cells.push({x,y,pasted:true});
  }
  cells.sort((a,b)=>a.y-b.y||a.x-b.x);
  highlightCache={key,revision:editorRevision,mode:highlightMode,cells};return cells;
}
function focusedTilePosition() {
  return pixelStamp?pixelStamp.split(',').map(Number):pixelCell!==null
    ?[pixelCell%L.cellsW,Math.floor(pixelCell/L.cellsW)]:selectionAnchor;
}
function activeSelectionPosition() {
  const point=focusedTilePosition();
  if(point&&(selectedStampKeys.has(point.join(','))||point[0]>=0&&point[1]>=0&&point[0]<L.cellsW
    &&point[1]<L.cellsH&&selectedCells.has(point[1]*L.cellsW+point[0])))
    return point;
  return tileSelectionPositions()[0];
}
function selectOnlyTile(x,y) {
  selectedCells.clear();selectedStampKeys.clear();selectionRect=null;
  selectCell(x,y);setSelectionAnchor(x,y);
  const tile=displayedCell(room,bgIndex,L,x,y);if(tile)lastEntry=tile.words[0];
  refreshSelectionControls();draw();
}
function reviewChange(direction) {
  if(!L)return;
  const cells=currentHighlightedTiles();if(!cells.length)return;
  const current=focusedTilePosition();
  const index=current?cells.findIndex(c=>c.x===current[0]&&c.y===current[1]):-1;
  const next=index<0?(direction>0?0:cells.length-1):(index+direction+cells.length)%cells.length;
  const {x,y}=cells[next];setMode('2d');returnToEditedTiles();selectOnlyTile(x,y);
  const rect=cvs.getBoundingClientRect();view.scale=Math.max(view.scale,2);
  view.x=rect.width/2-(x+.5)*16*view.scale;view.y=rect.height/2-(y+.5)*16*view.scale;
  const label=highlightMode==='modified'?'Edit':`${BAND_LABELS[Number(highlightMode)]} match`;
  tileActionStatus(`${label} ${next+1} of ${cells.length} · tile ${x}, ${y}.`);draw();
}
function openPixelInspector() {
  setMode('2d');returnToEditedTiles();$('#bSelect').onclick();
  const point=activeSelectionPosition();if(point)focusTileAt(...point);
  pixelInspector.hidden=false;refreshPixelEditor();$('#pixelBlack').focus({preventScroll:true});
  tileActionStatus('Pixel edits apply immediately to the preview. Undo reverses a stroke.');
}
function previewSelection() {
  const point=activeSelectionPosition();if(!point)return;
  setMode('3d');setNativeCamera('x',point[0]*16-120);setNativeCamera('y',point[1]*16-104);
  tileActionStatus('Diorama preview centered on the selection.');
}
function refreshEditorFeedback() {
  if(!L)return;
  const changes=currentTileChanges(),positions=tileSelectionPositions();
  const bands=positions.flatMap(([x,y])=>displayedCell(room,bgIndex,L,x,y)?.bands||[]);
  const family=editFamily(room,bgIndex),regions=TERRAIN_PROFILES
    .filter(p=>family.mask&(1<<p.profile)).map(p=>p.profile===2?'EU/GE':TERRAIN_TOKENS[p.profile].toUpperCase()).join(' + ');
  const unique=[...new Set(bands)].sort();
  $('#selectionSummary').textContent=positions.length?`${positions.length} tile${positions.length===1?'':'s'} selected`
    :'No tiles selected';
  $('#selectionApplied').textContent=positions.length
    ?`Applied band: ${unique.length===1?BAND_LABELS[unique[0]]:'Mixed ('+unique.map(b=>BAND_LABELS[b]).join(', ')+')'}`
    :'Select tiles, then choose an action below.';
  $('#selectionScope').textContent=`BG${bgIndex+1} · ${regions} · Diorama only`;
  for(let i=0;i<3;i++) {
    const button=$(`#selectionBand${i}`);button.disabled=!positions.length||mode!=='2d';
    button.classList.toggle('on',unique.length===1&&unique[0]===i);
    button.setAttribute('aria-pressed',String(unique.length===1&&unique[0]===i));
  }
  const local=positions.reduce((n,[x,y])=>{
    const cell=y*L.cellsW+x;
    return n+!!(stampBucket(room,bgIndex).cells[`${x},${y}`]||st.byCell[cell]!==undefined);
  },0);
  $('#selectionSource').textContent=positions.length
    ?`${local} local band override${local===1?'':'s'} · ${positions.filter(([x,y])=>tileChangeKinds(x,y).pixels).length} with pixel edits`
    :`Blue = selected · Highlight filter: ${highlightLabel()}.`;
  const selected=positions.length>0;
  $('#selectionPixels').disabled=!selected||mode!=='2d';
  $('#selectionPreview').disabled=!selected;
  $('#selectionPaste').disabled=!selected||mode!=='2d'||tileClipboard?.key!==keyOf(room,bgIndex);
  $('#selectionTools').hidden=mode!=='2d';
  $('#pixelSelectionBlackQuick').disabled=!selected||mode!=='2d';
  $('#copyTilesQuick').disabled=!selected||mode!=='2d';
  const toolNames={select:'Select tiles',selectRect:'Select a rectangle',pan:'Pan',
    cell:'Paint one tile',class:'Paint matching tile types',rect:'Paint a rectangle',stamp:'Stamp copied tiles'};
  $('#selectQuick').classList.toggle('on',brush==='select');
  $('#selectRectQuick').classList.toggle('on',brush==='selectRect');
  $('#stampQuick').classList.toggle('on',brush==='stamp');
  let hint;
  if(compareOriginal)hint='Showing original tiles. Editing returns to your edited preview.';
  else if(brush==='select'||brush==='selectRect')hint='Selection makes no edits. Click or drag; Shift-click selects a range. Actions apply immediately.';
  else if(brush==='stamp')hint='Click places the copied rectangle. Keep clicking to repeat; Esc finishes.';
  else if(brush==='pan')hint='Drag to move the map; wheel zooms. Choose Select to pick tiles.';
  else hint=`Click/drag immediately paints ${BAND_LABELS[band]}`
    +(brush==='class'?' on every matching tile in this terrain family.':'.')+' Alt restores inherited bands.';
  $('#activeTool').textContent=`Tool: ${toolNames[brush]||brush}`;
  $('#toolFeedback').textContent=hint;
  $('#changeSummary').textContent=`${changes.cells.length} edited tile${changes.cells.length===1?'':'s'}`
    +` · ${changes.pixels} with pixel edits · ${changes.pasted} pasted`+(changes.bounds?' · edge space added':'');
  const matches=currentHighlightedTiles(),modified=highlightMode==='modified';
  $('#highlightBy').value=highlightMode;
  $('#highlightSummary').textContent=`${matches.length} matching tile${matches.length===1?'':'s'}`
    +(compareOriginal?' · hidden during comparison':!showEditOutlines?' · highlights hidden':'');
  $('#previousChange').textContent=modified?'Previous edit':'Previous match';
  $('#nextChange').textContent=modified?'Next edit':'Next match';
  $('#selectChanges').textContent=modified?'Select edited tiles':'Select highlighted tiles';
  for(const name of ['previousChange','nextChange','selectChanges'])$(`#${name}`).disabled=!matches.length;
  $('#compareOriginal').disabled=mode!=='2d';
  $('#compareOriginal').textContent=compareOriginal?'Return to edited tiles':'Compare original tiles';
  $('#compareOriginal').classList.toggle('on',compareOriginal);
  $('#editOutlines').classList.toggle('on',showEditOutlines);
  $('#editOutlines').setAttribute('aria-pressed',String(showEditOutlines));
  $('#editOutlines').title=`Show ${highlightLabel().toLowerCase()} on the map. Blue selection remains separate.`;
  const dirty=editorHasUnexportedChanges();
  $('#saveState').textContent=dirty?'Unexported changes':savepointKind==='exported'?'Matches last export':'Loaded INI';
  $('#saveState').classList.toggle('dirty',dirty);
  $('#saveState').title=dirty?'Edits are applied in this preview. Export INI to keep them.'
    :'The game uses its own INI file. Downloading an export does not replace that file automatically.';
  $('#export').textContent=dirty?'Export changes':'Export INI';
}
for(let value=0;value<3;value++)$(`#selectionBand${value}`).onclick=()=>{
  const count=tileSelectionPositions().length;
  const changed=applySelectionBand(value);
  tileActionStatus(changed?`Applied ${BAND_LABELS[value]} to ${count} selected tile${count===1?'':'s'}. View in 3D to check depth.`
    :`Selected tiles already use ${BAND_LABELS[value]}.`);refreshEditorFeedback();
};
$('#selectionPixels').onclick=openPixelInspector;
$('#pixelInspectorClose').onclick=()=>{pixelInspector.hidden=true;cvs.focus({preventScroll:true});};
$('#selectionPreview').onclick=previewSelection;
$('#selectionPaste').onclick=()=>{
  const point=selectionRect?[selectionRect.x0,selectionRect.y0]
    :activeSelectionPosition();if(!point)return;
  returnToEditedTiles();const pasted=stampTiles(...point);
  tileActionStatus(pasted?`Pasted over the selection at ${point.join(', ')}.`:$('#stampInfo').textContent);
};
$('#previousChange').onclick=()=>reviewChange(-1);
$('#nextChange').onclick=()=>reviewChange(1);
$('#highlightBy').onchange=()=>{
  highlightMode=$('#highlightBy').value;showEditOutlines=true;setMode('2d');returnToEditedTiles();
  refreshEditorFeedback();draw();
};
$('#selectChanges').onclick=()=>{
  setMode('2d');selectedCells.clear();selectedStampKeys.clear();selectionRect=null;selectionAnchor=null;
  for(const {x,y,pasted} of currentHighlightedTiles())
    if(pasted)selectedStampKeys.add(`${x},${y}`);else selectedCells.add(y*L.cellsW+x);
  const point=tileSelectionPositions()[0];if(point)focusTileAt(...point);
  refreshSelectionControls();draw();
};
$('#compareOriginal').onclick=()=>{
  compareOriginal=!compareOriginal;
  tileActionStatus(compareOriginal?'Showing original tile art without band tint, black masks or pasted scenery.'
    :`Showing your edited tiles. Highlight filter: ${highlightLabel()}.`);
  refreshEditorFeedback();draw();
};
cvs.addEventListener('dblclick',ev=>{
  if(mode!=='2d'||!['select','selectRect'].includes(brush))return;
  const [x,y]=toCell(ev);if(!displayedCell(room,bgIndex,L,x,y))return;
  selectOnlyTile(x,y);openPixelInspector();
});

$('#selectQuick').onclick=()=>{$('#bSelect').onclick();setMode('2d');};
