
/* ---- undo / redo --------------------------------------------------------
 * Deltas, not snapshots. An operation records the PREVIOUS value of every key
 * it is about to touch and, on commit, the resulting value -- so a stroke that
 * repaints one cell costs one entry, while "all instances of a metatile" costs
 * as many as it genuinely changed.
 *
 * The unit of undo is the GESTURE, not the event. A brush drag fires a paint
 * per mousemove; without coalescing, undoing one stroke would mean pressing
 * undo a few hundred times. beginOp/commitOp bracket the gesture.
 *
 * `undefined` is a real recorded value: it means the key was absent, so the
 * tile fell back to its authentic band. Restoring it deletes the key rather
 * than writing a band, which is why every apply goes through setKey. */
const undoStack = [], redoStack = [];
const kMaxUndo = 200;
let pendingOp = null;

function beginOp(label) {
  returnToEditedTiles();
  if (pendingOp) commitOp();
  pendingOp = { label, parts: {} };
}
/* An operation is a set of per-layer deltas rather than one layer's, because
 * "reset everything" is a single gesture that spans all of them and should
 * undo as one. Ordinary painting simply has one part. */
function partFor(key) {
  if (!pendingOp) return null;
  if (!pendingOp.parts[key]) pendingOp.parts[key] = { before:{ cell:{}, id:{}, pixelCell:{}, pixelId:{}, pixelCoord:{}, transparentCell:{}, transparentId:{}, transparentCoord:{}, stamp:{} } };
  return pendingOp.parts[key];
}
function recordCellIn(key, cell) {
  const part = partFor(key);
  if (part && !(cell in part.before.cell))
    part.before.cell[cell] = (store[key] || {byCell:{}}).byCell[cell];
}
function recordIdIn(key, id) {
  const part = partFor(key);
  if (part && !(id in part.before.id))
    part.before.id[id] = (store[key] || {byId:{}}).byId[id];
}
const recordCell = cell => recordCellIn(keyOf(room, bgIndex), cell);
const recordId   = id   => recordIdIn(keyOf(room, bgIndex), id);
function commitOp() {
  if (!pendingOp) return;
  const op = pendingOp; pendingOp = null;
  let changed = false;
  for (const key in op.parts) {
    const part = op.parts[key], bucket = store[key] || { byCell:{}, byId:{} };
    part.after = { cell:{}, id:{}, pixelCell:{}, pixelId:{}, pixelCoord:{}, transparentCell:{}, transparentId:{}, transparentCoord:{}, stamp:{} };
    const pasted=stampStore[key]||{cells:{},bounds:undefined};
    for(const k in part.before.stamp) {
      part.after.stamp[k]=pasted.cells[k];
      if(part.after.stamp[k]!==part.before.stamp[k])changed=true;
    }
    if('bounds' in part.before) {
      part.after.bounds=pasted.bounds;
      if(part.after.bounds!==part.before.bounds)changed=true;
    }
    if('framing' in part.before) {
      part.after.framing=pasted.framing;
      if(part.after.framing!==part.before.framing)changed=true;
    }
    const pixels = pixelStore[key] || {byCell:{},byId:{}};
    for (const [kind,target] of [['pixelCell',pixels.byCell],['pixelId',pixels.byId],
      ['pixelCoord',pixels.byCoord??{}],['transparentCell',pixels.transparent?.byCell??{}],
      ['transparentId',pixels.transparent?.byId??{}],['transparentCoord',pixels.transparent?.byCoord??{}]])
      for (const k in part.before[kind]) {
        part.after[kind][k] = target[k];
        if (part.after[kind][k] !== part.before[kind][k]) changed = true;
      }
    for (const k in part.before.cell) {
      part.after.cell[k] = bucket.byCell[k];
      if (part.after.cell[k] !== part.before.cell[k]) changed = true;
    }
    for (const k in part.before.id) {
      part.after.id[k] = bucket.byId[k];
      if (part.after.id[k] !== part.before.id[k]) changed = true;
    }
  }
  if (!changed) return;                 /* a gesture that altered nothing */
  markEditorChanged();
  undoStack.push(op);
  if (undoStack.length > kMaxUndo) undoStack.shift();
  redoStack.length = 0;
  refreshHistoryButtons();
}
function setKey(target, k, value) {
  if (value === undefined) delete target[k]; else target[k] = value;
}
function applySide(op, which) {
  if(op.effects){EffectEditor.restore(op.effects[which]);return true;}
  const keys = Object.keys(op.parts);
  const current = keyOf(room, bgIndex);
  /* A single-layer op that belongs somewhere else switches there: undoing
   * into a layer you cannot see would look like nothing happened. A
   * multi-layer op stays put and just refreshes what is on screen. */
  if (keys.length === 1 && keys[0] !== current) {
    const [g, m, bg, profile] = keys[0].split(':').map(Number);
    const idx = DATA.rooms.findIndex(r => r.group===g && r.map===m);
    if (idx >= 0) {
      terrainProfile=profile;room = terrainRoom(DATA.rooms[idx],profile);
      $('#room').value = String(idx);setLayer(bg);
    }
  }
  for (const key of keys) {
    const side = op.parts[key][which];
    const pasted=stampStore[key]??={cells:{},bounds:undefined};
    for(const k in side.stamp)setKey(pasted.cells,k,side.stamp[k]);
    if('bounds' in side)pasted.bounds=side.bounds;
    if('framing' in side)pasted.framing=side.framing;
    if(Object.keys(side.stamp).length||'bounds' in side)surfacesDirty=compositeDirty=true;
    if (!store[key]) store[key] = { byId:{}, byCell:{} };
    const bucket = store[key];
    if (!pixelStore[key]) pixelStore[key] = {byCell:{},byId:{}};
    pixelStore[key].transparent??={byCell:{},byId:{},byCoord:{}};
    pixelStore[key].byCoord??={};
    for (const k in side.pixelCell) setKey(pixelStore[key].byCell,k,side.pixelCell[k]);
    for (const k in side.pixelId) setKey(pixelStore[key].byId,k,side.pixelId[k]);
    for (const k in side.pixelCoord) setKey(pixelStore[key].byCoord,k,side.pixelCoord[k]);
    for (const k in side.transparentCell) setKey(pixelStore[key].transparent.byCell,k,side.transparentCell[k]);
    for (const k in side.transparentId) setKey(pixelStore[key].transparent.byId,k,side.transparentId[k]);
    for (const k in side.transparentCoord) setKey(pixelStore[key].transparent.byCoord,k,side.transparentCoord[k]);
    if (key === keyOf(room,bgIndex) &&
        (Object.keys(side.pixelCell).length || Object.keys(side.pixelId).length||
          Object.keys(side.transparentCell).length||Object.keys(side.transparentId).length))
      surfacesDirty = compositeDirty = true;
    if(Object.keys(side.pixelCoord).length||Object.keys(side.transparentCoord).length)
      surfacesDirty = compositeDirty = true;
    for (const k in side.cell) setKey(bucket.byCell, k, side.cell[k]);
    for (const k in side.id)   setKey(bucket.byId,   k, side.id[k]);
    if (key !== keyOf(room, bgIndex)) continue;
    for (const k in side.cell) markCellDirty(Number(k));
    for (const k in side.id)   markIdDirty(Number(k));
  }
  syncTileSelection();
  refreshNativeCameraControls();
  refreshFramingControls();
  glDirty = true; invalidateOther(); invalidateGameComposite();
  markEditorChanged(); refreshPixelEditor(); tally(); draw();
  return true;
}
function undo() {
  finishFramingDrag();
  if (pendingOp) commitOp();
  const op = undoStack.pop();
  if (!op) return;
  redoStack.push(op);
  applySide(op, 'before');
  refreshHistoryButtons();
}
function redo() {
  finishFramingDrag();
  const op = redoStack.pop();
  if (!op) return;
  undoStack.push(op);
  applySide(op, 'after');
  refreshHistoryButtons();
}
function refreshHistoryButtons() {
  refreshEditorFeedback();
  const u = $('#undoBtn'), r = $('#redoBtn');
  if (!u || !r) return;
  u.disabled = !undoStack.length; r.disabled = !redoStack.length;
  u.title = undoStack.length
    ? `Undo ${undoStack[undoStack.length-1].label} (${undoStack.length})` : 'Nothing to undo';
  r.title = redoStack.length
    ? `Redo ${redoStack[redoStack.length-1].label}` : 'Nothing to redo';
}

/* ---- painting ---------------------------------------------------------- */
function paintCell(cx, cy, value) {
  const pasted=stampBucket(room,bgIndex).cells[`${cx},${cy}`];
  if(pasted) {
    paintStampBands(cx,cy,value,brush==='class');selectCell(cx,cy);
    if(brush==='class'&&!pasted.blank) {
      recordId(pasted.id);
      if(value===null)delete st.byId[pasted.id];else st.byId[pasted.id]=value;
      markIdDirty(pasted.id);
    }
    return;
  }
  if (cx<0 || cy<0 || cx>=L.cellsW || cy>=L.cellsH) return;
  selectCell(cx,cy,brush === 'class');
  invalidateGameComposite(); markEditorChanged();
  const cell = cy*L.cellsW + cx, id = L.cellId[cell];
  if(brush==='class') {
    for(const [pos,c] of Object.entries(stampBucket(room,bgIndex).cells))
      if(!c.blank&&c.id===id){const [x,y]=pos.split(',').map(Number);paintStampBands(x,y,value);}
  }
  if (value === null) {                       /* Alt: back to authentic */
    recordCell(cell); delete st.byCell[cell];
    if (brush === 'class') { recordId(id); delete st.byId[id]; markIdDirty(id); }
    markCellDirty(cell);
    return;
  }
  if (brush === 'class') { recordId(id); st.byId[id] = value; markIdDirty(id); }
  else { recordCell(cell); st.byCell[cell] = value; markCellDirty(cell); }
}
/* One metatile id can appear thousands of times; past the incremental cap the
 * full rebuild takes over, which is exactly the right trade at that size. */
function markIdDirty(id) {
  if (surfacesDirty) return;
  for (let cell = 0; cell < L.cellId.length; cell++) {
    if (L.cellId[cell] !== id) continue;
    markCellDirty(cell);
    if (surfacesDirty) return;
  }
}
function paintRect(x0, y0, x1, y1, value) {
  invalidateGameComposite(); markEditorChanged();
  const bounds=mapBounds(room,bgIndex,L);
  for(let cy=Math.max(bounds.y0,Math.min(y0,y1));cy<Math.min(bounds.y1,Math.max(y0,y1)+1);cy++)
    for(let cx=Math.max(bounds.x0,Math.min(x0,x1));cx<Math.min(bounds.x1,Math.max(x0,x1)+1);cx++) {
      if(paintStampBands(cx,cy,value)){selectedStampKeys.add(`${cx},${cy}`);continue;}
      if(cx<0||cy<0||cx>=L.cellsW||cy>=L.cellsH)continue;
      const cell=cy*L.cellsW+cx;
      selectedCells.add(cell);recordCell(cell);
      if(value===null)delete st.byCell[cell];else st.byCell[cell]=value;
      markCellDirty(cell);
    }
  refreshSelectionControls();
}
const toCell = ev => {
  const r = cvs.getBoundingClientRect();
  return [Math.floor(((ev.clientX-r.left) - view.x) / view.scale / 16),
          Math.floor(((ev.clientY-r.top ) - view.y) / view.scale / 16)];
};

let drag = null;
cvs.addEventListener('mousedown', ev => {
  if (mode !== '2d') return;
  if(ev.button===2||macControlClick(ev))return;
  if (ev.button === 1 || brush === 'pan') {
    drag = { pan:true, x:ev.clientX, y:ev.clientY }; return;
  }
  if(ev.button!==0)return;
  if(brush==='framing'){beginFramingDrag(ev);return;}
  if(brush==='select'&&!ev.ctrlKey&&!ev.metaKey&&EmitterMapTools.hit(ev)){if(EmitterMapTools.begin(ev))drag={effects:true};return;}
  if(['effects','effectPlace','effectPaste'].includes(brush)){
    if(EmitterMapTools.begin(ev))drag={effects:true};return;
  }
  const [cx, cy] = toCell(ev);
  /* Defer Shift-click until release. A deliberate drag still pans, while a
   * click selects an inclusive rectangle without painting or stamping. */
  if(ev.shiftKey) {
    drag={shiftSelect:true,x:ev.clientX,y:ev.clientY,cx,cy,anchor:selectionAnchor};
    return;
  }
  if(brush==='contour'){const r=cvs.getBoundingClientRect();drag={contour:true,points:[{x:Math.round((ev.clientX-r.left-view.x)/view.scale),y:Math.round((ev.clientY-r.top-view.y)/view.scale)}]};draw();return;}
  if(brush==='particleArea'){drag={particleArea:true,x0:cx,y0:cy,x1:cx,y1:cy};draw();return;}
  if(brush==='floorMist'||brush==='floorErase'){drag={floorMist:true,erase:brush==='floorErase',x0:cx,y0:cy,x1:cx,y1:cy};draw();return;}
  if(brush==='stamp'){stampTiles(cx,cy);return;}
  if(brush==='selectRect'){drag={selectRect:true,x0:cx,y0:cy,x1:cx,y1:cy};draw();return;}
  if (actor.show && !ev.altKey && brush !== 'select') {
    const px = (ev.clientX - cvs.getBoundingClientRect().left - view.x)/view.scale;
    const py = (ev.clientY - cvs.getBoundingClientRect().top  - view.y)/view.scale;
    if (px >= actor.x && px <= actor.x+actor.w && py >= actor.y && py <= actor.y+actor.h) {
      drag = { actor2d:true, ox:px-actor.x, oy:py-actor.y }; return;
    }
  }
  const value = ev.altKey ? null : band;
  if (brush === 'select') {
    if (!ev.ctrlKey && !ev.metaKey) {selectedCells.clear();selectedStampKeys.clear();}
    selectionRect=null;
    drag = { select:true }; selectCell(cx,cy); draw();
  } else if (brush === 'rect') { drag = { rect:true, x0:cx, y0:cy, x1:cx, y1:cy, value }; }
  else {
    beginOp(ev.altKey ? 'revert' : `paint ${BANDS[band].name.toLowerCase()}`);
    drag = { paint:true, value }; paintCell(cx, cy, value);
    glDirty = true; tally(); draw();
  }
  const picked=displayedCell(room,bgIndex,L,cx,cy);
  if(picked)lastEntry=picked.words[0];
  setSelectionAnchor(cx,cy);
  refreshSelectionControls();
});
window.addEventListener('mousemove', ev => {
  if (mode !== '2d') return;            /* the orbit handler owns 3D */
  const [cx, cy] = toCell(ev);
  hud(cx, cy);
  if(brush==='stamp'){stampHover=[cx,cy];draw();}
  if (!drag) return;
  if(drag.framing){moveFramingDrag(ev);return;}
  if(drag.effects){EmitterMapTools.move(ev);return;}
  if(drag.contour){
    const r=cvs.getBoundingClientRect(),p={x:Math.round((ev.clientX-r.left-view.x)/view.scale),y:Math.round((ev.clientY-r.top-view.y)/view.scale)};
    const last=drag.points.at(-1);
    if(Math.hypot(p.x-last.x,p.y-last.y)>=8) {
      if(drag.points.length===16)drag.points=drag.points.filter((_,i)=>i%2===0||i===15);
      drag.points.push(p);
    }
    draw();return;
  }
  if(drag.shiftSelect) {
    const dx=ev.clientX-drag.x,dy=ev.clientY-drag.y;
    if(dx*dx+dy*dy<16)return;
    view.x+=dx;view.y+=dy;
    drag={pan:true,x:ev.clientX,y:ev.clientY};draw();return;
  }
  if (drag.actor2d) {
    const r = cvs.getBoundingClientRect();
    actor.x = Math.round((ev.clientX-r.left-view.x)/view.scale - drag.ox);
    actor.y = Math.round((ev.clientY-r.top -view.y)/view.scale - drag.oy);
    draw(); return;
  }
  if (drag.pan) { view.x += ev.clientX-drag.x; view.y += ev.clientY-drag.y;
                  drag.x=ev.clientX; drag.y=ev.clientY; draw(); }
  else if (drag.select) { selectCell(cx,cy); draw(); }
  else if (drag.paint) {
    /* Paint on every event so a fast drag cannot skip a cell, but let the
     * redraw collapse onto the next frame like everything else. */
    paintCell(cx, cy, drag.value);
    glDirty = true; tally(); draw();
  }
  else if (drag.rect||drag.selectRect||drag.floorMist||drag.particleArea) { drag.x1=cx; drag.y1=cy; draw(); }
});
window.addEventListener('mouseup', () => {
  if(finishFramingDrag())return;
  if(drag?.effects){drag=null;EmitterMapTools.finish();draw();return;}
  if(drag?.contour){EffectEditor.paintContour(drag.points);drag=null;draw();return;}
  if(drag?.particleArea){
    EffectEditor.paintParticleRect(drag.x0,drag.y0,drag.x1,drag.y1);drag=null;draw();return;
  }
  if(drag?.floorMist){
    EffectEditor.paintFloorRect(drag.x0,drag.y0,drag.x1,drag.y1,drag.erase);drag=null;draw();return;
  }
  if(drag?.shiftSelect) {
    const [x,y]=drag.anchor||[drag.cx,drag.cy];
    selectRectangle(x,y,drag.cx,drag.cy);
    const picked=displayedCell(room,bgIndex,L,drag.cx,drag.cy);
    if(picked) {
      lastEntry=picked.words[0];
      pixelStamp=stampBucket(room,bgIndex).cells[`${drag.cx},${drag.cy}`]
        ?`${drag.cx},${drag.cy}`:null;
      pixelCell=pixelStamp?null:drag.cy*L.cellsW+drag.cx;
    } else {pixelStamp=null;pixelCell=null;}
    refreshSelectionControls();
  }
  if(drag?.selectRect)selectRectangle(drag.x0,drag.y0,drag.x1,drag.y1);
  if (drag && drag.rect) {
    beginOp('rectangle');
    paintRect(drag.x0, drag.y0, drag.x1, drag.y1, drag.value);
  }
  const wasEdit = drag && (drag.rect || drag.paint);
  drag = null;
  if (wasEdit) { commitOp(); glDirty = true; tally(); draw(); }
});
cvs.addEventListener('wheel', ev => {
  ev.preventDefault();
  if (mode !== '2d') return;
  if(drag?.framing)return;
  const r = cvs.getBoundingClientRect();
  const mx = ev.clientX-r.left, my = ev.clientY-r.top;
  const k = Math.exp(-ev.deltaY * 0.0015);
  const ns = Math.max(0.05, Math.min(8, view.scale * k));
  view.x = mx - (mx - view.x) * (ns/view.scale);
  view.y = my - (my - view.y) * (ns/view.scale);
  view.scale = ns; draw();
}, { passive:false });

/* ---- select-by --------------------------------------------------------- */
function selectMatchingCells(predicate) {
  selectedCells.clear();selectedStampKeys.clear();selectionRect=null;selectionAnchor=null;
  const pasted=stampBucket(room,bgIndex).cells;
  for(let cy=0;cy<L.cellsH;cy++)for(let cx=0;cx<L.cellsW;cx++) {
    const key=`${cx},${cy}`;
    if(pasted[key])continue;
    if(predicate(displayedCell(room,bgIndex,L,cx,cy)))selectedCells.add(cy*L.cellsW+cx);
  }
  for(const [key,c] of Object.entries(pasted))if(predicate(c))selectedStampKeys.add(key);
  refreshSelectionControls();draw();
}
$('#selPrio').onclick=()=>selectMatchingCells(c=>c.words.some(word=>word&0x2000));
$('#selPal').onclick=()=>{
  if(lastEntry===null)return;
  const palette=(lastEntry>>10)&7;
  selectMatchingCells(c=>((c.words[0]>>10)&7)===palette);
};
function deselect() {
  commitOp(); if(drag?.effects)EmitterMapTools.cancel();drag = null;
  selectedCells.clear();selectedStampKeys.clear();selectionRect=null;stampHover=null;
  selectionAnchor=null;
  if(['stamp','floorMist','floorErase','effects','effectPlace','effectPaste','particleArea'].includes(brush)){$('#bSelect').onclick();}
  lastEntry = null; pixelCell = null;pixelStamp=null;
  refreshSelectionControls(); draw();
}
$('#deselect').onclick = deselect;
function applySelectionBand(value) {
  if (!selectedCells.size && !selectedStampKeys.size) return false;
  beginOp(value===null?'reset selected tile bands':`apply ${BANDS[value].name.toLowerCase()} to selection`);
  const target=stampBucket(room,bgIndex),proposed={...target.cells};
  for(const cell of selectedStampKeys) {
    const tile=displayedCell(room,bgIndex,L,...cell.split(',').map(Number));
    if(!tile)continue;
    const bands=tile.words.map(word=>value===null?authenticBand(word):value);
    if(bands.some((b,i)=>b!==tile.bands[i]))proposed[cell]={...tile,bands};
  }
  let changed=false;
  for(const cell of selectedStampKeys) {
    if(proposed[cell]===target.cells[cell])continue;
    recordStamp(keyOf(room,bgIndex),cell);target.cells[cell]=proposed[cell];changed=true;
  }
  for (const cell of selectedCells) {
    const next=value===null?undefined:value;
    if(st.byCell[cell]===next)continue;
    recordCell(cell);setKey(st.byCell,cell,next);changed=true;
  }
  commitOp();
  if(changed){invalidate();sceneryChanged();}
  return changed;
}
$('#applyBand').onclick = () => applySelectionBand(band);
/* Clearing every edit in a bucket returns those tiles to the band their own
 * priority bit selects -- which is what "ROM default" means here. The baseline
 * is never stored, so reverting is deletion, not a rewrite. */
function clearKeys(keys, label) {
  const live = keys.filter(k => store[k] &&
    (Object.keys(store[k].byCell).length || Object.keys(store[k].byId).length));
  if (!live.length) return 0;
  markEditorChanged();
  beginOp(label);
  let n = 0;
  for (const key of live) {
    const b = store[key];
    for (const k of Object.keys(b.byCell)) { recordCellIn(key, Number(k)); delete b.byCell[k]; n++; }
    for (const k of Object.keys(b.byId))   { recordIdIn(key, Number(k));   delete b.byId[k];   n++; }
  }
  commitOp();
  surfacesDirty = true; invalidate();
  return n;
}
$('#clearAll').onclick = () =>
  clearKeys([keyOf(room, bgIndex)], 'reset layer');
$('#clearRoom').onclick = () =>
  clearKeys([keyOf(room, 0), keyOf(room, 1)], 'reset room');
$('#clearEverything').onclick = () => {
  const keys = Object.keys(store);
  const edited = keys.filter(k => store[k] &&
    (Object.keys(store[k].byCell).length || Object.keys(store[k].byId).length));
  if (!edited.length) { alert('Nothing is edited — everything is already at the ROM default.'); return; }
  const rooms = new Set(edited.map(k => k.split(':').slice(0,2).join(':')));
  if (!confirm(`Reset every edit back to the ROM's own priority bits?\n\n`
             + `${edited.length} layer(s) across ${rooms.size} room(s).\n`
             + `This is undoable with ctrl/cmd-Z.`)) return;
  clearKeys(edited, 'reset all rooms');
};
