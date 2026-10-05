/* Context actions keep the existing selection and use the same authored
 * deltas as the toolbar. A paste destination is independent of the selection. */
const tileMenu=$('#tileMenu');
tileMenu.hidden=true;
const tileMenuButtons=['AddEffect','EditEffect','DeleteEffect','CopyEffect','PasteEffect','Preview','Settings','Priority','Far','ResetBand','Pixels','Fill','FlipH','FlipV','MirrorH','MirrorV','Copy','Paste','Stamp','Delete','Reset','Remove','Deselect']
  .map(name=>$(`#tileAction${name}`));
let tileMenuTarget=null;
const macControlClick=ev=>ev.button===0&&ev.ctrlKey&&/Mac/.test(window.navigator?.platform||'');
function tileSelectionPositions() {
  return [...selectedCells].map(cell=>[cell%L.cellsW,Math.floor(cell/L.cellsW)])
    .concat([...selectedStampKeys].map(key=>key.split(',').map(Number)));
}
function closeTileMenu(focus=false) {
  if(!tileMenuTarget)return;
  tileMenu.hidden=true;tileMenuTarget=null;
  if(focus)cvs.focus({preventScroll:true});
}
function tileActionStatus(text) {$('#tileActionStatus').textContent=text;}
function openTileMenu(cx,cy,clientX,clientY) {
  if(mode!=='2d'||!L)return false;
  returnToEditedTiles();commitOp();drag=null;
  const hit=EmitterMapTools.hit({clientX,clientY}),point=EmitterMapTools.point({clientX,clientY});
  const overlaps=EmitterMapTools.hits({clientX,clientY});
  const effect=hit?.entry;
  const tile=displayedCell(room,bgIndex,L,cx,cy),position=`${cx},${cy}`;
  const selected=selectedStampKeys.has(position)||
    (cx>=0&&cy>=0&&cx<L.cellsW&&cy<L.cellsH&&selectedCells.has(cy*L.cellsW+cx));
  if(tile&&!selected&&!effect) {
    selectedCells.clear();selectedStampKeys.clear();selectionRect=null;
    selectCell(cx,cy);setSelectionAnchor(cx,cy);
  }
  if(effect&&!EffectEditor.isMapSelected(effect.id))EffectEditor.selectEmitter(effect.id);
  else if(tile)focusTileAt(cx,cy);
  refreshSelectionControls();draw();
  const positions=tileSelectionPositions(),bands=positions.flatMap(([x,y])=>
    displayedCell(room,bgIndex,L,x,y)?.bands||[]);
  const hasSelection=positions.length>0,anyHigh=bands.some(b=>b===2);
  const allHigh=bands.length>0&&bands.every(b=>b===2);
  const sameClipboard=tileClipboard?.key===keyOf(room,bgIndex);
  tileMenuTarget={cx,cy,key:keyOf(room,bgIndex),priority:allHigh?1:2,effect:effect?.id,x:point.x,y:point.y};
  $('#tileActionAddEffect').disabled=false;
  $('#tileActionAddEffect').title=`Place an effect anchored to this BG${bgIndex+1} map point.`;
  $('#tileActionCopyEffect').hidden=!effect;$('#tileActionPasteEffect').hidden=!EffectEditor.hasClipboard();
  $('#tileActionEditEffect').hidden=$('#tileActionDeleteEffect').hidden=!effect;
  $('#tileActionEditEffect').textContent=effect?`Configure ${effect.kind}…`:'Configure effect…';
  $('#tileActionDeleteEffect').textContent=effect?.native?'Disable default effect':'Delete effect';
  const choices=$('#tileEffectChoice');choices.replaceChildren();
  for(const entry of overlaps) {
    const option=document.createElement('option');option.value=entry.id;
    option.textContent=`${entry.kind}${entry.native?' (default)':''}${entry.enabled?'':' (disabled)'}`;
    choices.append(option);
  }
  choices.value=effect?.id??'';$('#tileEffectChoiceRow').hidden=overlaps.length<2;
  for(const button of tileMenuButtons.slice(7))button.hidden=!!effect;
  $('#tileMenuTitle').textContent=effect?`${effect.kind} · ${Math.round(effect.x)}, ${Math.round(effect.y)}`:`${positions.length} tile${positions.length===1?'':'s'} selected · BG${bgIndex+1}`;
  $('#tileMenuDestination').hidden=!!effect;
  $('#tileMenuDestination').textContent=`Paste destination: ${cx}, ${cy} · Diorama only`;
  $('#tileActionPriority').textContent=allHigh?'Turn priority off':'Turn priority on';
  $('#tileActionPriority').setAttribute('aria-checked',allHigh?'true':anyHigh?'mixed':'false');
  for(const name of ['Priority','Far','ResetBand','Fill','FlipH','FlipV','Copy','Delete','Reset','Deselect'])
    $(`#tileAction${name}`).disabled=!hasSelection;
  $('#tileActionDelete').textContent=positions.length===1?'Delete tile':'Delete selected tiles';
  $('#tileActionReset').textContent=positions.length===1?'Reset tile to default':'Reset selected tiles to default';
  for(const axis of ['H','V'])$(`#tileActionMirror${axis}`).disabled=!selectedTileRectangle(positions);
  $('#tileActionPixels').disabled=!tile;
  $('#tileActionRemove').disabled=$('#removeStamps').disabled;
  $('#tileActionStamp').disabled=!sameClipboard;
  $('#tileActionPaste').disabled=!sameClipboard||cx < -512||cy < -512||
    cx+tileClipboard.w>512||cy+tileClipboard.h>512;
  $('#tileActionPaste').title=sameClipboard?`Paste ${tileClipboard.w}×${tileClipboard.h} tiles at ${cx}, ${cy}`
    :'Copy tiles from this room, background and terrain first.';
  tileMenu.hidden=false;
  const rect=tileMenu.getBoundingClientRect();
  tileMenu.style.left=`${Math.max(8,Math.min(clientX,window.innerWidth-rect.width-8))}px`;
  tileMenu.style.top=`${Math.max(8,Math.min(clientY,window.innerHeight-rect.height-8))}px`;
  (tileMenuButtons.find(button=>!button.disabled&&!button.hidden)||tileMenu).focus();
  return true;
}
$('#tileEffectChoice').onchange=e=>{
  const entry=EffectEditor.mapEmitters().find(effect=>effect.id===e.target.value);
  if(!tileMenuTarget||!entry)return;
  tileMenuTarget.effect=entry.id;EffectEditor.selectEmitter(entry.id);
  $('#tileMenuTitle').textContent=`${entry.kind} · ${Math.round(entry.x)}, ${Math.round(entry.y)}`;
  $('#tileActionEditEffect').textContent=`Configure ${entry.kind}…`;
  $('#tileActionDeleteEffect').textContent=entry.native?'Disable default effect':'Delete effect';
};
function runTileAction(action) {
  const target=tileMenuTarget;
  if(!target)return;
  closeTileMenu(true);
  if(target.key!==keyOf(room,bgIndex)||mode!=='2d')return;
  action(target);
}
$('#tileActionCopyEffect').onclick=()=>runTileAction(()=>EffectEditor.copyEffects());
$('#tileActionPasteEffect').onclick=()=>runTileAction(({x,y})=>EffectEditor.pasteEffects(x,y));
$('#tileActionAddEffect').onclick=()=>runTileAction(({x,y})=>EffectEditor.openModal(null,x,y));
$('#tileActionEditEffect').onclick=()=>runTileAction(({effect})=>EffectEditor.openModal(effect));
$('#tileActionDeleteEffect').onclick=()=>runTileAction(({effect})=>EffectEditor.removeEffect(effect));
$('#tileActionPreview').onclick=()=>runTileAction(({x,y})=>EffectEditor.previewAt(x,y));
$('#tileActionPriority').onclick=()=>runTileAction(({priority})=>{
  applySelectionBand(priority);tileActionStatus(`Priority ${priority===2?'on':'off'} for selected tiles.`);
});
$('#tileActionFar').onclick=()=>runTileAction(()=>{
  applySelectionBand(0);tileActionStatus('Selected tiles moved to the far plane.');
});
$('#tileActionResetBand').onclick=()=>runTileAction(()=>{
  applySelectionBand(null);tileActionStatus('Selected tile band overrides reset.');
});
$('#tileActionPixels').onclick=()=>runTileAction(()=>{
  openPixelInspector();
});
$('#tileActionFill').onclick=()=>runTileAction(()=>{
  fillSelectedTransparency();tileActionStatus($('#selectionPixelInfo').textContent);
});
for(const axis of ['h','v']) {
  $(`#tileActionFlip${axis.toUpperCase()}`).onclick=()=>runTileAction(()=>flipSelectedTiles(axis));
  $(`#tileActionMirror${axis.toUpperCase()}`).onclick=()=>runTileAction(()=>flipSelectedTiles(axis,true));
}
$('#tileActionCopy').onclick=()=>runTileAction(()=>{
  const copied=copyTiles();tileActionStatus(copied?`Copied ${tileClipboard.w}×${tileClipboard.h} tiles.`
    :$('#stampInfo').textContent);
});
$('#tileActionPaste').onclick=()=>runTileAction(({cx,cy})=>{
  const pasted=stampTiles(cx,cy);tileActionStatus(pasted?`Pasted tiles at ${cx}, ${cy}.`
    :$('#stampInfo').textContent);
});
$('#tileActionStamp').onclick=()=>runTileAction(({cx,cy})=>{
  startStamp();stampHover=[cx,cy];draw();tileActionStatus('Click to stamp copied tiles; Esc finishes.');
});
$('#tileActionDelete').onclick=()=>runTileAction(()=>deleteSelectedTiles());
$('#tileActionReset').onclick=()=>runTileAction(()=>resetSelectedTiles());
$('#tileActionRemove').onclick=()=>runTileAction(()=>{
  removeStamps();tileActionStatus('Selected pasted tiles removed.');
});
$('#tileActionDeselect').onclick=()=>runTileAction(()=>{
  deselect();tileActionStatus('Selection cleared.');
});
$('#tileActionSettings').onclick=()=>runTileAction(()=>EditorLayout.openSettings());
cvs.addEventListener('contextmenu',ev=>{
  if(mode!=='2d')return;
  ev.preventDefault();
  openTileMenu(...toCell(ev),ev.clientX,ev.clientY);
});
cvs.addEventListener('keydown',ev=>{
  if(mode!=='2d'||!(ev.key==='ContextMenu'||ev.key==='F10'&&ev.shiftKey))return;
  ev.preventDefault();ev.stopPropagation();
  const position=selectionAnchor||tileSelectionPositions()[0];
  if(!position)return;
  const [cx,cy]=position,rect=cvs.getBoundingClientRect();
  openTileMenu(cx,cy,rect.left+view.x+(cx*16+8)*view.scale,
    rect.top+view.y+(cy*16+8)*view.scale);
});
tileMenu.addEventListener('keydown',ev=>{
  if(ev.key==='Escape') {
    ev.preventDefault();ev.stopPropagation();closeTileMenu(true);return;
  }
  if(ev.key==='Tab'){closeTileMenu(true);return;}
  if(ev.target===$('#tileEffectChoice'))return;
  const enabled=tileMenuButtons.filter(button=>!button.disabled&&!button.hidden);
  const index=enabled.indexOf(document.activeElement);
  let next;
  if(ev.key==='ArrowDown')next=(index+1)%enabled.length;
  else if(ev.key==='ArrowUp')next=(index+enabled.length-1)%enabled.length;
  else if(ev.key==='Home')next=0;
  else if(ev.key==='End')next=enabled.length-1;
  else return;
  ev.preventDefault();ev.stopPropagation();enabled[next]?.focus();
});
window.addEventListener('mousedown',ev=>{
  if(!tileMenu.hidden&&!tileMenu.contains(ev.target))closeTileMenu();
},true);
window.addEventListener('wheel',ev=>{
  if(!tileMenu.contains(ev.target))closeTileMenu();
},true);
window.addEventListener('resize',()=>closeTileMenu());
window.addEventListener('blur',()=>closeTileMenu());
