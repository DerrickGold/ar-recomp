
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
  if (pendingOp) commitOp();
  pendingOp = { label, parts: {} };
}
/* An operation is a set of per-layer deltas rather than one layer's, because
 * "reset everything" is a single gesture that spans all of them and should
 * undo as one. Ordinary painting simply has one part. */
function partFor(key) {
  if (!pendingOp) return null;
  if (!pendingOp.parts[key]) pendingOp.parts[key] = { before:{ cell:{}, id:{} } };
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
    part.after = { cell:{}, id:{} };
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
  undoStack.push(op);
  if (undoStack.length > kMaxUndo) undoStack.shift();
  redoStack.length = 0;
  refreshHistoryButtons();
}
function setKey(target, k, value) {
  if (value === undefined) delete target[k]; else target[k] = value;
}
function applySide(op, which) {
  const keys = Object.keys(op.parts);
  const current = keyOf(room, bgIndex);
  /* A single-layer op that belongs somewhere else switches there: undoing
   * into a layer you cannot see would look like nothing happened. A
   * multi-layer op stays put and just refreshes what is on screen. */
  if (keys.length === 1 && keys[0] !== current) {
    const [g, m, bg] = keys[0].split(':').map(Number);
    const idx = DATA.rooms.findIndex(r => r.group===g && r.map===m);
    if (idx >= 0) { room = DATA.rooms[idx]; $('#room').value = String(idx); setLayer(bg); }
  }
  for (const key of keys) {
    const side = op.parts[key][which];
    if (!store[key]) store[key] = { byId:{}, byCell:{} };
    const bucket = store[key];
    for (const k in side.cell) setKey(bucket.byCell, k, side.cell[k]);
    for (const k in side.id)   setKey(bucket.byId,   k, side.id[k]);
    if (key !== keyOf(room, bgIndex)) continue;
    for (const k in side.cell) markCellDirty(Number(k));
    for (const k in side.id)   markIdDirty(Number(k));
  }
  glDirty = true; invalidateOther(); invalidateGameComposite();
  configDirty = true; tally(); draw();
  return true;
}
function undo() {
  if (pendingOp) commitOp();
  const op = undoStack.pop();
  if (!op) return;
  redoStack.push(op);
  applySide(op, 'before');
  refreshHistoryButtons();
}
function redo() {
  const op = redoStack.pop();
  if (!op) return;
  undoStack.push(op);
  applySide(op, 'after');
  refreshHistoryButtons();
}
function refreshHistoryButtons() {
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
  if (cx<0 || cy<0 || cx>=L.cellsW || cy>=L.cellsH) return;
  invalidateGameComposite(); configDirty = true;
  const cell = cy*L.cellsW + cx, id = L.cellId[cell];
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
  invalidateGameComposite(); configDirty = true;
  for (let cy=Math.min(y0,y1); cy<=Math.max(y0,y1); cy++)
    for (let cx=Math.min(x0,x1); cx<=Math.max(x0,x1); cx++) {
      const cell = cy*L.cellsW + cx;
      recordCell(cell);
      if (value === null) delete st.byCell[cell]; else st.byCell[cell] = value;
      markCellDirty(cell);
    }
}
const toCell = ev => {
  const r = cvs.getBoundingClientRect();
  return [Math.floor(((ev.clientX-r.left) - view.x) / view.scale / 16),
          Math.floor(((ev.clientY-r.top ) - view.y) / view.scale / 16)];
};

let drag = null;
cvs.addEventListener('mousedown', ev => {
  if (mode !== '2d') return;
  /* Middle button, shift, or the explicit Pan tool. Requiring a modifier was
   * the only way to move around and nothing on screen said so. */
  if (ev.button === 1 || ev.shiftKey || brush === 'pan') {
    drag = { pan:true, x:ev.clientX, y:ev.clientY }; return;
  }
  const [cx, cy] = toCell(ev);
  if (actor.show && !ev.altKey) {
    const px = (ev.clientX - cvs.getBoundingClientRect().left - view.x)/view.scale;
    const py = (ev.clientY - cvs.getBoundingClientRect().top  - view.y)/view.scale;
    if (px >= actor.x && px <= actor.x+actor.w && py >= actor.y && py <= actor.y+actor.h) {
      drag = { actor2d:true, ox:px-actor.x, oy:py-actor.y }; return;
    }
  }
  const value = ev.altKey ? null : band;
  if (brush === 'rect') { drag = { rect:true, x0:cx, y0:cy, x1:cx, y1:cy, value }; }
  else {
    beginOp(ev.altKey ? 'revert' : `paint ${BANDS[band].name.toLowerCase()}`);
    drag = { paint:true, value }; paintCell(cx, cy, value);
    glDirty = true; tally(); draw();
  }
  if (cx>=0 && cy>=0 && cx<L.cellsW && cy<L.cellsH)
    lastEntry = L.words[(cy*2)*L.tilesW + cx*2];
});
window.addEventListener('mousemove', ev => {
  if (mode !== '2d') return;            /* the orbit handler owns 3D */
  const [cx, cy] = toCell(ev);
  hud(cx, cy);
  if (!drag) return;
  if (drag.actor2d) {
    const r = cvs.getBoundingClientRect();
    actor.x = Math.round((ev.clientX-r.left-view.x)/view.scale - drag.ox);
    actor.y = Math.round((ev.clientY-r.top -view.y)/view.scale - drag.oy);
    draw(); return;
  }
  if (drag.pan) { view.x += ev.clientX-drag.x; view.y += ev.clientY-drag.y;
                  drag.x=ev.clientX; drag.y=ev.clientY; draw(); }
  else if (drag.paint) {
    /* Paint on every event so a fast drag cannot skip a cell, but let the
     * redraw collapse onto the next frame like everything else. */
    paintCell(cx, cy, drag.value);
    glDirty = true; tally(); draw();
  }
  else if (drag.rect) { drag.x1=cx; drag.y1=cy; draw(); }
});
window.addEventListener('mouseup', () => {
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
  const r = cvs.getBoundingClientRect();
  const mx = ev.clientX-r.left, my = ev.clientY-r.top;
  const k = Math.exp(-ev.deltaY * 0.0015);
  const ns = Math.max(0.05, Math.min(8, view.scale * k));
  view.x = mx - (mx - view.x) * (ns/view.scale);
  view.y = my - (my - view.y) * (ns/view.scale);
  view.scale = ns; draw();
}, { passive:false });

/* ---- select-by --------------------------------------------------------- */
$('#selPrio').onclick = () => {
  configDirty = true;
  beginOp('select by priority');
  /* Every tile the artist already marked priority-1. In a room that uses the
   * bit meaningfully this is the split they intended, and it is usually the
   * best first pass before any hand work. */
  for (let cy=0; cy<L.cellsH; cy++) for (let cx=0; cx<L.cellsW; cx++) {
    let any = false;
    for (let q=0;q<4;q++)
      if (L.words[(cy*2+(q>>1))*L.tilesW + cx*2+(q&1)] & 0x2000) any = true;
    if (any) { recordCell(cy*L.cellsW+cx); st.byCell[cy*L.cellsW+cx] = band; }
  }
  commitOp(); invalidate();
};
$('#selPal').onclick = () => {
  if (lastEntry === null) return;
  configDirty = true;
  beginOp('select by palette');
  const want = (lastEntry>>10) & 7;
  for (let cy=0; cy<L.cellsH; cy++) for (let cx=0; cx<L.cellsW; cx++)
    if ((L.words[(cy*2)*L.tilesW + cx*2] >> 10 & 7) === want) {
      recordCell(cy*L.cellsW+cx); st.byCell[cy*L.cellsW+cx] = band;
    }
  commitOp(); invalidate();
};
/* Clearing every edit in a bucket returns those tiles to the band their own
 * priority bit selects -- which is what "ROM default" means here. The baseline
 * is never stored, so reverting is deletion, not a rewrite. */
function clearKeys(keys, label) {
  const live = keys.filter(k => store[k] &&
    (Object.keys(store[k].byCell).length || Object.keys(store[k].byId).length));
  if (!live.length) return 0;
  configDirty = true;
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
