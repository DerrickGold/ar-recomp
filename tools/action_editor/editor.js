
/* ---- reference actor ----------------------------------------------------
 * A stand-in at the loaded OBJ2 z/order makes authentic low/high background
 * priority visible without requiring the room's live sprite stream. The 2D
 * view inserts it at Mode-1 OBJ priority 2; the Diorama view puts it in the
 * same painter list as the configured surfaces.
 *
 * It is deliberately a silhouette rather than real sprite art. The question it
 * answers is one of ordering and footprint, and pulling the room's OBJ
 * characters out of the asset script would be a second reverse-engineering
 * problem for no extra answer. */
const actor = { x: 0, y: 0, w: 24, h: 32, show: true };
const actorZ = () => resolvedPlane('obj2').z;

function resetActor() {
  actor.x = nativeCamera.x + Math.round(DATA.frameWidth * 0.40);
  actor.y = nativeCamera.y + Math.round(DATA.frameHeight * 0.60);
}

/* Inverse of a 4x4, so a screen drag can be put back onto the actor's plane.
 * Plain arithmetic on the same column-major convention as the rest of the
 * camera code. */
function invert(m) {
  const a=m[0],b=m[1],c=m[2],d=m[3],e=m[4],f=m[5],g=m[6],h=m[7],
        i=m[8],j=m[9],k=m[10],l=m[11],n=m[12],o=m[13],q=m[14],r=m[15];
  const s0=a*f-b*e, s1=a*g-c*e, s2=a*h-d*e, s3=b*g-c*f,
        s4=b*h-d*f, s5=c*h-d*g, c5=k*r-l*q, c4=j*r-l*o, c3=j*q-k*o,
        c2=i*r-l*n, c1=i*q-k*n, c0=i*o-j*n;
  const det = s0*c5-s1*c4+s2*c3+s3*c2-s4*c1+s5*c0;
  if (!det) return null;
  const t = 1/det;
  return [ ( f*c5-g*c4+h*c3)*t, (-b*c5+c*c4-d*c3)*t, ( o*s5-q*s4+r*s3)*t, (-j*s5+k*s4-l*s3)*t,
           (-e*c5+g*c2-h*c1)*t, ( a*c5-c*c2+d*c1)*t, (-n*s5+q*s2-r*s1)*t, ( i*s5-k*s2+l*s1)*t,
           ( e*c4-f*c2+h*c0)*t, (-a*c4+b*c2-d*c0)*t, ( n*s4-o*s2+r*s0)*t, (-i*s4+j*s2-l*s0)*t,
           (-e*c3+f*c1-g*c0)*t, ( a*c3-b*c1+c*c0)*t, (-n*s3+o*s1-q*s0)*t, ( i*s3-j*s1+k*s0)*t ];
}
/* Screen point -> the point on plane z where the eye ray crosses it. */
function unprojectToPlane(mvp, sx, sy, planeZ, vw, vh) {
  const inv = invert(mvp);
  if (!inv) return null;
  const ndcX = (sx / vw) * 2 - 1, ndcY = 1 - (sy / vh) * 2;
  const at = ndc => {
    const v = [ndcX, ndcY, ndc, 1], o = [0,0,0,0];
    for (let r=0;r<4;r++){ let acc=0;
      for (let c=0;c<4;c++) acc += inv[c*4+r]*v[c]; o[r]=acc; }
    return [o[0]/o[3], o[1]/o[3], o[2]/o[3]];
  };
  const n = at(-1), f = at(1);
  const dz = f[2] - n[2];
  if (Math.abs(dz) < 1e-9) return null;
  const t = (planeZ - n[2]) / dz;
  return [n[0] + (f[0]-n[0])*t, n[1] + (f[1]-n[1])*t];
}

/* Export the complete INI, not a sidecar dialect. The four ordinary BG plane
 * records and both virtual records are regenerated; every other record stays
 * byte-for-byte in the surrounding document. */
$('#export').onclick = () => {
  try {
    const text = mergeDioramaIni();
    const url = URL.createObjectURL(new Blob([text],{type:'text/plain;charset=utf-8'}));
    const a = document.createElement('a');
    a.href=url; a.download=sourceIniName || 'diorama-layers.ini';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(()=>URL.revokeObjectURL(url),1000);
    captureEditorSavepoint('exported');
    tileActionStatus('INI download started. Use the exported file as the game’s diorama-layers.ini.');
  } catch (error) {
    alert(`Cannot export INI:\n\n${error.message}`);
  }
};
$('#importIni').onchange = async event => {
  const file = event.target.files && event.target.files[0];
  event.target.value = '';
  if (!file) return;
  if (editorHasUnexportedChanges() && !confirm('Loading another INI replaces the unexported edits in this editor. Continue?'))
    return;
  loadIniText(await file.text(),file.name);
  undoStack.length=0; redoStack.length=0; pendingOp=null;
  refreshHistoryButtons();
  surfacesDirty=compositeDirty=glDirty=true; composite=null;
  invalidateOther(); invalidateGameComposite();
  setLayer(bgIndex);
};

/* ---- chrome ------------------------------------------------------------ */
function hud(cx, cy) {
  const inb = cx>=0 && cy>=0 && cx<L.cellsW && cy<L.cellsH;
  const parts = [
    `<b>${room.group}:${room.map}</b> BG${bgIndex+1} &nbsp; ${L.w}\u00d7${L.h}px`
      + ` &nbsp; ${terrainLabel(room)} terrain`+(compareOriginal?' &nbsp; <b>Original tiles</b>':'')
      + ` &nbsp; ${L.pw}\u00d7${L.ph} pages`
      + ` &nbsp; profile $${Number(room.videoProfile).toString(16).padStart(2,'0')}`
      + (room.raster ? ` &nbsp; raster R${room.raster}` : '')
      + (room.animation
          ? ` &nbsp; anim ${animationPhase(room)+1}/${room.animation.phases}`
          : '')
      + (room.bg2PageCycle
          ? ` &nbsp; BG2 page ${bg2PageIndex(room)+1}/4`
          : ''),
  ];
  const pasted=compareOriginal?null:stampBucket(room,bgIndex).cells[`${cx},${cy}`];
  if(pasted) {
    parts.push(`cell ${cx},${cy} &nbsp; pasted metatile $${pasted.id.toString(16).padStart(2,'0')}`
      +` &nbsp; bands ${pasted.bands.join('/')}`);
  } else if (inb) {
    const cell = cy*L.cellsW+cx, id = L.cellId[cell];
    const e = L.words[(cy*2)*L.tilesW + cx*2];
    const b = compareOriginal?authenticBand(e):bandAt(st, L, cx*2, cy*2);
    parts.push(`cell ${cx},${cy} &nbsp; metatile <b>$${id.toString(16)
      .padStart(2,'0')}</b> &nbsp; char $${(e&0x3ff).toString(16).padStart(3,'0')}`
      + ` &nbsp; pal ${(e>>10)&7} &nbsp; ROM priority ${(e&0x2000)?1:0}`);
    parts.push(`band <b style="color:var(${BANDS[b].css})">${BANDS[b].name}</b>`
      + (!compareOriginal&&cellEdited(st,L,cx,cy) ? ' (band override)' : ' (original band)'));
  } else if (mode === '3d') {
    parts.push(`drag to orbit &nbsp; shift-drag to move native camera &nbsp; wheel to zoom`
      + ` &nbsp; <kbd>r</kbd> resets`
      + ` &nbsp; camera ${nativeCamera.x},${nativeCamera.y}`);
  } else {
    parts.push('Shift-click to select a range &nbsp; shift-drag to pan &nbsp; wheel to zoom'
      + ' &nbsp; <kbd>f</kbd> fits');
  }
  if(!inb&&!pasted)parts.push(`cell ${cx},${cy} &nbsp; empty edge space`);
  $('#hud').innerHTML = parts.join('<br>');
}
/* Counting every tile is O(level) -- 49,152 of them in Fillmore 1:1 -- so it
 * runs when the classification changes, never when the camera moves. */
function tally() {
  const n = [0,0,0];
  for (let ty=0; ty<L.tilesH; ty++) for (let tx=0; tx<L.tilesW; tx++)
    n[bandAt(st, L, tx, ty)]++;
  $('#tB').textContent = n[0]; $('#tP').textContent = n[1]; $('#tA').textContent = n[2];
  const note = $('#engineNote');
  if (n[0] > 0) {
    note.className = 'note';
    note.innerHTML = `<b>${n[0]} tiles use the far virtual plane.</b> Its z, `
      + `paint order and alpha are authored above and export with this room's `
      + `<code>bg${bgIndex+1}-virtual</code> records.`;
  } else {
    note.className = 'note';
    note.innerHTML = '<b>No tiles are assigned to the far plane.</b> The room '
      + 'currently resolves through its ordinary and priority BG planes; the '
      + 'ROM priority bit remains the fallback for every unedited tile.';
  }
}
/* ---- pacing -------------------------------------------------------------
 * Pointer events arrive far faster than the display refreshes -- a fast drag
 * can deliver several hundred a second -- so drawing straight from the event
 * handler renders the same frame many times and the camera appears to jump.
 * Every redraw is therefore a REQUEST that collapses onto the next animation
 * frame, which is both the natural cadence and a hard cap of one draw per
 * refresh no matter how the mouse behaves. */
let framePending = false;
function updateLegend() {
  $('#legend').innerHTML = mode === '2d'
    ? (brush==='stamp'?'<b>click</b> stamp copied tiles &nbsp; <b>Esc</b> finish &nbsp; ':
      brush==='selectRect'?'<b>drag</b> select rectangle &nbsp; <b>Ctrl/Cmd-C</b> copy &nbsp; ':
      '<b>drag</b> select / paint &nbsp; ')+'<b>shift-drag / middle</b> pan &nbsp; '
      + '<b>arrows</b> move<br><b>Shift-click</b> select range &nbsp; <b>wheel</b> zoom &nbsp; <b>f</b> fit &nbsp; '
      + '<b>right-click</b> actions &nbsp; <b>ctrl/cmd-Z</b> undo'
      + (compareOriginal?'<br>Original tile comparison · highlights hidden':
        '<br><span style="color:#5aa9ff">Blue: selected</span> &nbsp; '
        + `<span style="color:${highlightMode==='modified'?'#f0c55a':`var(${highlightColor()})`}">${highlightLabel()}: highlighted</span>`)
    : mode === 'native'
      ? '<b>native stable frame</b> &nbsp; camera sliders or arrows move the view'
        + '<br>frame slider drives CHR, page-cycle, and persistent raster phases'
    : '<b>drag</b> orbit &nbsp; <b>shift-drag / arrows</b> move native camera'
      + '<br><b>wheel</b> zoom &nbsp; <b>r</b> reset orbit';
}
/* The ladder is the answer to "will these two layers interpenetrate?" -- it
 * is arithmetic on the resolved depths, so it is right even where the camera
 * angle happens to hide the overlap. */
function renderBandHint() {
  const clearsSprites = bandPaintOrder(bgIndex,2) >= resolvedPlane('obj2').order;
  $('#bandHint').innerHTML =
    `Every band is anchored to BG${bgIndex+1}, so all three inherit its scroll. `
    + `<b>Plane</b> is the untouched default. The priority plane `
    + (clearsSprites
        ? `paints <b>after the OBJ2 reference</b>.`
        : `paints <b>before the OBJ2 reference</b>.`)
    + ` Paint order and z are independent, matching the game renderer.`;
}
function renderLadder() {
  const rows = depthLadder();
  const el = $('#ladder');
  el.innerHTML = rows.map(r => {
    const cls = [r.clash ? 'clash' : '', r.kind === 'obj' ? 'obj' : ''].join(' ');
    const mark = r.kind === 'band' && r.bg === bgIndex ? '\u25b8' : '\u00a0';
    const col = r.kind === 'band'
      ? `<span class="swatch" style="width:8px;height:8px;border-radius:2px;background:var(${BANDS[r.band].css})"></span>`
      : '<span style="width:8px"></span>';
    return `<div class="${cls}">${col}<span class="z">${r.z>=0?'+':''}${r.z.toFixed(2)}`
         + `</span><span class="nm">${mark} ${r.label} [order ${r.order}]</span></div>`;
  }).join('');
  const note = $('#clashNote');
  if (rows.clashPairs.length) {
    note.style.display = '';
    note.className = 'note warn';
    note.innerHTML = `<b>Nearly co-planar geometry.</b> `
      + rows.clashPairs.join('; ')
      + `. Their z values are within ${kClashEpsilon}; configured paint order `
      + `still resolves overlap deterministically, but the planes may read as `
      + `one depth.`;
  } else note.style.display = 'none';
}
function draw() {
  if (framePending) return;
  framePending = true;
  requestAnimationFrame(() => { framePending = false; drawNow(); });
}
function drawNow() {
  refreshEditorFeedback();
  if (mode === '2d') draw2d();
  else if (mode === 'native') drawNative2d();
  else draw3d();
  updateLegend(); renderLadder(); renderBandHint();
  const bg = bgIndex ^ 1, lay = room.bg[bg], e = otherExtent();
  $('#otherName').textContent = lay
    ? `BG${bg+1} ${e.w}\u00d7${e.h}` + (e.w < L.w ? ' (shorter)' : '')
    : '(none)';
}
function refreshNativePhaseControls() {
  $('#nativeFrame').value = String(nativeFrame);
  $('#nativeFramev').textContent = String(nativeFrame);
  const parts = [];
  if (room.animation)
    parts.push(`CHR phase ${animationPhase(room)+1}/${room.animation.phases}`
      + ` at ${room.animation.cadence}f cadence`
      + (room.animation.continuation ? ' (native continuation)' : ''));
  if (room.bg2PageCycle)
    parts.push(`BG2 page ${bg2PageIndex(room)+1}/4 at 5f cadence`);
  if (room.raster)
    parts.push(`raster preset R${room.raster} uses this frame clock`);
  const parity=nativeGoldenStatus.get(sceneKey(room));
  if(parity===true)parts.push('C/JS native parity verified');
  else if(parity===false)parts.push('C/JS PARITY MISMATCH — check console');
  $('#nativePhaseInfo').textContent = parts.length
    ? parts.join(' · ') : 'Static background';
}
function refreshNativeCameraControls() {
  const bg1=room.bg[0],maxX=Math.max(0,bg1.pagesWide*256-DATA.frameWidth);
  let maxY=Math.max(0,bg1.pagesHigh*256-(DATA.frameHeight+1)),minX=0,minY=0;
  let previewMaxX=maxX;
  if(mode==='3d') {
    for(let bg=0;bg<2;bg++) {
      const layer=decodeLayer(room,bg);if(!layer)continue;
      const b=mapBounds(room,bg,layer);
      minX=Math.min(minX,b.x0*16);minY=Math.min(minY,b.y0*16);
      if(bg===0){previewMaxX=Math.max(maxX,b.x1*16-DATA.frameWidth);
        maxY=Math.max(maxY,b.y1*16-DATA.frameHeight-1);}
    }
  }
  nativeCamera.x=Math.max(minX,Math.min(previewMaxX,nativeCamera.x));
  nativeCamera.y=Math.max(minY,Math.min(maxY,nativeCamera.y));
  $('#nativeCameraX').min=String(minX);$('#nativeCameraY').min=String(minY);
  $('#nativeCameraX').max=String(previewMaxX);$('#nativeCameraX').value=String(nativeCamera.x);
  $('#nativeCameraY').max=String(maxY);$('#nativeCameraY').value=String(nativeCamera.y);
  $('#nativeCameraXv').textContent=String(nativeCamera.x);
  $('#nativeCameraYv').textContent=String(nativeCamera.y);
}
function setNativeCamera(axis,value) {
  nativeCamera[axis]=Number(value)||0;refreshNativeCameraControls();
  native2dCache=null;nativeBandCache=null;glDirty=true;draw();
}
$('#nativeCameraX').oninput=event=>setNativeCamera('x',event.target.value);
$('#nativeCameraY').oninput=event=>setNativeCamera('y',event.target.value);
function setNativeFrame(value) {
  nativeFrame = Math.max(0, Math.min(255, Number(value) || 0));
  L = decodeLayer(room,bgIndex);
  surfacesDirty = compositeDirty = glDirty = true;
  composite = null;
  invalidateOther();
  invalidateGameComposite();
  refreshNativePhaseControls(); refreshPixelEditor();
  draw();
}
$('#nativeFrame').oninput = event => setNativeFrame(event.target.value);
$('#nativePlay').onclick = () => {
  if (nativePlayTimer) {
    clearInterval(nativePlayTimer); nativePlayTimer = null;
    $('#nativePlay').classList.remove('on');
    $('#nativePlay').textContent = 'Play native phases';
    return;
  }
  nativePlayTimer = setInterval(
    () => setNativeFrame((nativeFrame + 1) & 255), 1000/60);
  $('#nativePlay').classList.add('on');
  $('#nativePlay').textContent = 'Pause native phases';
};
const setBand = i => { band = i;refreshEditorFeedback();
  document.querySelectorAll('.state').forEach((b,j) => b.classList.toggle('on', j===i)); };
document.querySelectorAll('.state').forEach(b =>
  b.onclick = () => setBand(Number(b.dataset.state)));
const brushBtns = { class:$('#bClass'), cell:$('#bCell'), rect:$('#bRect'),
                    select:$('#bSelect'), selectRect:$('#bSelectRect'), stamp:$('#stampTool'), pan:$('#bPan') };
Object.entries(brushBtns).forEach(([k, el]) => el.onclick = () => {
  if(k==='stamp'){startStamp();return;}
  if(k==='selectRect')setMode('2d');
  stampHover=null;brush = k; Object.values(brushBtns).forEach(b => b.classList.remove('on'));
  $('#stampQuick').classList.remove('on');$('#selectRectQuick').classList.toggle('on',k==='selectRect');
  cvs.style.cursor=k==='pan'?'grab':k==='selectRect'?'crosshair':'default';
  el.classList.add('on');$('#selectQuick').classList.toggle('on',k==='select');refreshSelectionControls();draw(); });
$('#bg1').onclick = () => setLayer(0);
$('#bg2').onclick = () => setLayer(1);
$('#mode2d').onclick = () => setMode('2d');
$('#modeNative').onclick = () => setMode('native');
$('#mode3d').onclick = () => setMode('3d');
$('#tint').onclick = () => { tint = !tint; $('#tint').classList.toggle('on', tint);
  invalidate(); };
$('#tint').addEventListener('click', invalidateOther);
$('#editOutlines').onclick = () => {
  setMode('2d');
  showEditOutlines = !showEditOutlines;
  if(showEditOutlines)returnToEditedTiles();
  $('#editOutlines').classList.toggle('on',showEditOutlines);refreshEditorFeedback();draw();
};
let syncingVirtualControls = false;
function refreshVirtualControls() {
  if (!room) return;
  const authored = roomConfig(room).virtual[bgIndex];
  const value = resolvedVirtual(bgIndex);
  syncingVirtualControls = true;
  $('#virtualZ').value = Math.round(value.z*100);
  $('#virtualOrder').value = value.order;
  $('#virtualAlpha').value = value.alpha;
  $('#virtualZv').textContent = `${value.z.toFixed(2)}${authored.setZ?' *':''}`;
  $('#virtualOrderv').textContent = `${value.order}${authored.setOrder?' *':''}`;
  $('#virtualAlphav').textContent = `${value.alpha}${authored.setAlpha?' *':''}`;
  syncingVirtualControls = false;
}
$('#virtualZ').oninput = event => {
  if (syncingVirtualControls) return;
  const v=roomConfig(room).virtual[bgIndex]; v.setZ=true; v.z=Number(event.target.value)/100;
  markEditorChanged(); glDirty=true; refreshVirtualControls(); draw();
};
$('#virtualOrder').oninput = event => {
  if (syncingVirtualControls) return;
  const v=roomConfig(room).virtual[bgIndex]; v.setOrder=true; v.order=Number(event.target.value);
  markEditorChanged(); glDirty=true; refreshVirtualControls(); draw();
};
$('#virtualAlpha').oninput = event => {
  if (syncingVirtualControls) return;
  const v=roomConfig(room).virtual[bgIndex]; v.setAlpha=true; v.alpha=Number(event.target.value);
  markEditorChanged(); glDirty=true; refreshVirtualControls(); draw();
};
$('#virtualReset').onclick = () => {
  const v=roomConfig(room).virtual[bgIndex];
  v.setZ=v.setOrder=v.setAlpha=false;
  markEditorChanged(); glDirty=true; refreshVirtualControls(); draw();
};
let syncingPlaneControls=false;
function selectedPlaneEdit() {
  const planes=roomConfig(room).planes;
  if(!planes[planeToken])planes[planeToken]=emptyPlane();
  return planes[planeToken];
}
function setPlaneStrategy(edit,next) {
  const was=strategyOfPlane(edit);
  const tilt=was==='rake'?edit.rake:was==='bow'?edit.bow:0;
  const fill=was==='stack'?edit.stack:was==='voxel'?edit.voxel:0;
  clearPlaneShape(edit);
  if(next==='rake'){edit.setRake=true;edit.rake=tilt||0.29;}
  else if(next==='bow'){edit.setBow=true;edit.bow=tilt||0.29;}
  else if(next==='thick'){edit.setThickness=true;edit.thickness=0.20;}
  else if(next==='stack'){
    edit.setStack=true;edit.stack=fill||0.29;
    edit.setStackCopies=true;edit.stackCopies=3;
  } else if(next==='voxel'){
    edit.setVoxel=true;edit.voxel=fill||0.18;
    edit.setVoxelCopies=true;edit.voxelCopies=12;
  }
}
function refreshPlaneControls() {
  if(!room)return;
  const edit=selectedPlaneEdit(),value=resolvedPlane(planeToken);
  const strategy=strategyOfPlane(edit);
  syncingPlaneControls=true;
  $('#planeSelect').value=planeToken;
  $('#planeZ').value=Math.round(value.z*100);
  $('#planeOrder').value=value.order;$('#planeAlpha').value=value.alpha;
  $('#planeZv').textContent=`${value.z.toFixed(2)}${edit.setZ?' *':''}`;
  $('#planeOrderv').textContent=`${value.order}${edit.setOrder?' *':''}`;
  $('#planeAlphav').textContent=`${value.alpha}${edit.setAlpha?' *':''}`;
  $('#planeStrategy').value=strategy;$('#planeStrategyv').textContent=strategy;
  const depth=strategy==='rake'?edit.rake:strategy==='bow'?edit.bow:
    strategy==='thick'?edit.thickness:strategy==='stack'?edit.stack:
    strategy==='voxel'?edit.voxel:0;
  const signed=strategy==='rake'||strategy==='bow';
  $('#planeDepth').min=signed?'-100':'0';$('#planeDepth').max='100';
  $('#planeDepth').value=Math.round(depth*100);
  $('#planeDepthv').textContent=Number(depth).toFixed(2);
  $('#planeDepthRow').style.display=strategy==='flat'?'none':'flex';
  const repeated=strategy==='stack'||strategy==='voxel';
  $('#planeCopiesRow').style.display=repeated?'flex':'none';
  $('#planeDirectionRow').style.display=repeated?'flex':'none';
  if(repeated){
    $('#planeCopies').max=strategy==='voxel'?'24':'8';
    $('#planeCopies').value=value.copies;
    $('#planeCopiesv').textContent=`${value.copies}`+
      (strategy==='stack'&&edit.setStackDensity&&!edit.setStackCopies?' density':'');
    $('#planeDirection').value=STACK_DIRECTIONS[value.direction];
  }
  const composed=['setRake','setBow','setThickness','setStack','setVoxel']
    .filter(key=>edit[key]).length>1;
  $('#planeHint').innerHTML=`Edits <code>${planeToken}</code> in this room. `+
    `The 3D view uses the game's rake/bow/skirt/stack formulas immediately.`+
    (composed?' <b>This imported record composes multiple shape keys.</b> Choosing a new shape makes it exclusive.':'');
  syncingPlaneControls=false;
}
function planeChanged() {
  markEditorChanged();glDirty=true;refreshPlaneControls();draw();
}
$('#planeSelect').onchange=event=>{
  planeToken=event.target.value;
  const nextBg=planeToken.startsWith('bg2')?1:0;
  if(nextBg!==bgIndex&&room.bg[nextBg])setLayer(nextBg);
  else {refreshPlaneControls();draw();}
};
$('#planeZ').oninput=event=>{
  if(syncingPlaneControls)return;const p=selectedPlaneEdit();
  p.setZ=true;p.z=Number(event.target.value)/100;planeChanged();
};
$('#planeOrder').oninput=event=>{
  if(syncingPlaneControls)return;const p=selectedPlaneEdit();
  p.setOrder=true;p.order=Number(event.target.value);planeChanged();
};
$('#planeAlpha').oninput=event=>{
  if(syncingPlaneControls)return;const p=selectedPlaneEdit();
  p.setAlpha=true;p.alpha=Number(event.target.value);planeChanged();
};
$('#planeStrategy').onchange=event=>{
  if(syncingPlaneControls)return;setPlaneStrategy(selectedPlaneEdit(),event.target.value);
  planeChanged();
};
$('#planeDepth').oninput=event=>{
  if(syncingPlaneControls)return;
  const p=selectedPlaneEdit(),strategy=strategyOfPlane(p);
  const value=Number(event.target.value)/100;
  if(value===0){clearPlaneShape(p);planeChanged();return;}
  if(strategy==='rake'){p.setRake=true;p.rake=value;}
  else if(strategy==='bow'){p.setBow=true;p.bow=value;}
  else if(strategy==='thick'){p.setThickness=true;p.thickness=value;}
  else if(strategy==='stack'){p.setStack=true;p.stack=value;}
  else if(strategy==='voxel'){p.setVoxel=true;p.voxel=value;}
  planeChanged();
};
$('#planeCopies').oninput=event=>{
  if(syncingPlaneControls)return;
  const p=selectedPlaneEdit(),strategy=strategyOfPlane(p),value=Number(event.target.value);
  if(strategy==='voxel'){p.setVoxelCopies=true;p.voxelCopies=value;}
  else if(strategy==='stack'){
    p.setStackCopies=true;p.stackCopies=value;
    p.setStackDensity=false;p.stackDensity=0;
  }
  planeChanged();
};
$('#planeDirection').onchange=event=>{
  if(syncingPlaneControls)return;const p=selectedPlaneEdit();
  p.setStackDirection=true;p.stackDirection=event.target.value;planeChanged();
};
$('#planeResetLayout').onclick=()=>{
  const p=selectedPlaneEdit();p.setZ=p.setOrder=p.setAlpha=false;planeChanged();
};
$('#planeResetShape').onclick=()=>{clearPlaneShape(selectedPlaneEdit());planeChanged();};
$('#planeResetAll').onclick=()=>{
  roomConfig(room).planes[planeToken]=emptyPlane();planeChanged();
};
$('#undoBtn').onclick = undo;
$('#redoBtn').onclick = redo;
$('#navFit').onclick = () => { if(mode==='2d')fitView(); draw(); };
$('#navOut').onclick = () => zoomBy(1/1.4);
$('#navIn').onclick  = () => zoomBy(1.4);
$('#navReset').onclick = () => { resetCamera(); fitView(); draw(); };
$('#actorBtn').onclick = () => { actor.show = !actor.show;
  $('#actorBtn').classList.toggle('on', actor.show); draw(); };
$('#bothBtn').onclick = () => { showBoth = !showBoth;
  $('#bothBtn').classList.toggle('on', showBoth); draw(); };
/* Sliders are fractions of the other layer's own width/height, so the same
 * throw covers a 256px background and a 4096px one. */
function otherExtent() {
  const bg = bgIndex ^ 1;
  const lay = room.bg[bg];
  return lay ? { w: lay.pagesWide*256, h: lay.pagesHigh*256 } : { w:256, h:256 };
}
function applyOffsets() {
  const e = otherExtent();
  otherOffset.x = Math.round(($('#offX').value / 100) * e.w);
  otherOffset.y = Math.round(($('#offY').value / 100) * e.h);
  $('#offXv').textContent = `${otherOffset.x >= 0 ? '+' : ''}${otherOffset.x}px`;
  $('#offYv').textContent = `${otherOffset.y >= 0 ? '+' : ''}${otherOffset.y}px`;
  draw();
}
$('#offX').oninput = applyOffsets;
$('#offY').oninput = applyOffsets;
$('#offRepeat').onclick = () => { otherOffset.repeat = !otherOffset.repeat;
  $('#offRepeat').classList.toggle('on', otherOffset.repeat); draw(); };
$('#offZero').onclick = () => { $('#offX').value = 0; $('#offY').value = 0;
  applyOffsets(); };
$('#docs').onclick = () => $('#docsDlg').showModal();
$('#docsX').onclick = () => $('#docsDlg').close();
const resetCamera = () => {
  orbit.yaw = 0; orbit.pitch = -0.34; orbit.dist = 1.25;
  orbit.focusX = 0.5; orbit.focusY = 0;
  draw();
};
window.addEventListener('keydown', e => {
  if(!tileMenu.hidden)return;
  if (e.target.tagName === 'SELECT' || e.target.tagName === 'INPUT') return;
  const accel = e.metaKey || e.ctrlKey;
  if (e.key === 'Escape' || (accel && e.key.toLowerCase() === 'd')) {
    e.preventDefault(); deselect(); return;
  }
  if(accel&&e.key.toLowerCase()==='c'&&mode==='2d'){e.preventDefault();copyTiles();return;}
  if(accel&&e.key.toLowerCase()==='v'){e.preventDefault();startStamp();return;}
  if (accel && (e.key === 'z' || e.key === 'Z')) {
    e.preventDefault(); e.shiftKey ? redo() : undo(); return;
  }
  if (accel && (e.key === 'y' || e.key === 'Y')) { e.preventDefault(); redo(); return; }
  if (e.key>='1' && e.key<='3') setBand(Number(e.key)-1);
  if (e.key === 'f') { fitView(); draw(); }
  /* It is easy to orbit until nothing is on screen, and without this there is
   * no way back except reloading. */
  if (e.key === 'r') { resetCamera(); fitView(); draw(); }
  /* Arrow keys move in whichever view is up, so navigation never depends on
   * finding the right drag. */
  const step = e.shiftKey ? 4 : 1;
  const arrows = { ArrowLeft:[-1,0], ArrowRight:[1,0], ArrowUp:[0,-1], ArrowDown:[0,1] };
  const a = arrows[e.key];
  if (!a) return;
  e.preventDefault();
  if (mode === '2d') { view.x -= a[0]*48*step; view.y -= a[1]*48*step; }
  else if (mode === 'native' || mode === '3d') {
    setNativeCamera('x',nativeCamera.x+a[0]*8*step);
    setNativeCamera('y',nativeCamera.y+a[1]*8*step);
  }
  draw();
});
function setMode(m) {
  closeTileMenu();
  tileActionStatus('');
  if(m!=='2d'){compareOriginal=false;pixelInspector.hidden=true;}
  drag = null;                          /* a drag never crosses a mode change */
  mode = m; $('#mode2d').classList.toggle('on', m==='2d');
  $('#modeNative').classList.toggle('on', m==='native');
  $('#mode3d').classList.toggle('on', m==='3d');
  cvs.style.display = m!=='3d' ? 'block' : 'none';
  glc.style.display = m==='3d' ? 'block' : 'none';
  refreshNativeCameraControls();invalidateGameComposite();
  refreshEditorFeedback();glDirty = true; draw();
}
function setLayer(i) {
  closeTileMenu();
  tileActionStatus('');
  if (!room.bg[i]) return;
  bgIndex = i;changeCache=null;
  if(!planeToken.startsWith(`bg${i+1}`))planeToken=`bg${i+1}`;
  $('#bg1').classList.toggle('on', i===0); $('#bg2').classList.toggle('on', i===1);
  L = decodeLayer(room, i); st = bucket(room, i);
  syncSelectionLayer();
  surfacesDirty = compositeDirty = glDirty = true; invalidateOther();
  invalidateGameComposite();
  composite = null;
  resetActor();
  verifyRoomNativeGolden(room);
  refreshTerrainControls();
  refreshVirtualControls(); refreshPlaneControls(); refreshNativePhaseControls();
  refreshNativeCameraControls();
  fitView(); tally(); draw();
}
const sel = $('#room');
DATA.rooms.forEach((r, i) => {
  const o = document.createElement('option');
  const a = r.bg[0], b = r.bg[1];
  o.value = i;
  const names=['','Fillmore','Bloodpool','Kassandora','Aitos','Marahna','Northwall','Death Heim'];
  const act2=[0,2,2,3,4,4,5];
  const act=r.group<7?`Act ${r.map>=act2[r.group]?2:1} · `:'';
  o.textContent = `${names[r.group]||r.group} · ${act}Room ${r.map} (${r.group}:${r.map})`
    + `  \u2014  BG1 ${a?a.pagesWide*256+'\u00d7'+a.pagesHigh*256:'\u2013'}`
    + `, BG2 ${b?b.pagesWide*256+'\u00d7'+b.pagesHigh*256:'\u2013'}`
    + `  [P${Number(r.videoProfile).toString(16).padStart(2,'0')}`
    + `${r.raster ? `/R${r.raster}` : ''}]`;
  sel.appendChild(o);
});
sel.onchange = () => { room = terrainRoom(DATA.rooms[Number(sel.value)]);
  nativeCamera.x=nativeCamera.y=0;nativeDecodedKey='';native2dCache=null;setLayer(0); };
const terrainSelect = $('#terrain');
TERRAIN_PROFILES.forEach(profile => {
  const option = document.createElement('option');
  option.value = profile.profile;
  option.textContent = profile.label;
  terrainSelect.appendChild(option);
});
function refreshTerrainControls() {
  terrainSelect.value = String(terrainProfile);
  const cells = room.changedCells || 0, definitions = room.changedMetatiles || 0;
  const family=editFamily(room,bgIndex),labels=TERRAIN_PROFILES
    .filter(p=>family.mask&(1<<p.profile)).map(p=>p.profile===2?`${p.label} (German too)`:p.label).join(' / ');
  $('#terrainInfo').textContent = `BG${bgIndex+1} tile edits: ${labels}`
    +(editFamilies(room,bgIndex).length===1?' (shared). ':' (matching terrain shares edits). ')
    +(cells || definitions
    ? `${cells} map cells and ${definitions} metatile definitions differ from US.`
    : 'This layout and its metatile definitions match US.');
}
terrainSelect.onchange = () => {
  commitOp();
  terrainProfile = Number(terrainSelect.value);
  room = terrainRoom(DATA.rooms[Number(sel.value)]);
  nativeDecodedKey = '';
  setLayer(bgIndex);
};
new ResizeObserver(() => {
  if (!L) return;
  /* First real layout: if the initial fit happened against a zero-size
   * element, this is what rescues it. */
  if (!fitted) fitView();
  draw();
}).observe($('.view'));
loadIniText(sourceIniText,sourceIniName);
setLayer(0);
setBand(2);
refreshHistoryButtons();
