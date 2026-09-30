
/* ---- classification store ---------------------------------------------
 * Two keyed layers, resolved cell-override-first. The same pair is what a
 * runtime lookup would consult: it is handed (tile_x, tile_y), from which the
 * map cell and the metatile id at that cell are both one index away.
 *
 * The metatile brush is the high-leverage one. A level map is literally an
 * array of metatile ids, so reclassifying an id lifts every instance of that
 * art across the whole room in one click. The cell brush exists because the
 * same art is often reused both near and far in one room. */
const pixelStore = {};
const store = {};   /* store[`${g}:${m}:${bg}:${family}`] = { byId:{}, byCell:{} } */
const keyOf = (r, bg) => `${r.group}:${r.map}:${bg}:${editFamily(r,bg).profile}`;
function bucket(r, bg) {
  const k = keyOf(r, bg);
  if (!store[k]) store[k] = { byId:{}, byCell:{} };
  return store[k];
}

/* ---- diorama-layers.ini -------------------------------------------------
 * The standalone editor owns authoring. It embeds the current manifest, can
 * load another one, and emits the complete file with the four action BG plane
 * records plus bgN-virtual records regenerated. Other planes, comments, scoped
 * sections and unrelated sections pass through untouched. */
let sourceIniText = window.__DIORAMA_LAYERS__ || '';
let sourceIniName = window.__DIORAMA_LAYERS_NAME__ || 'diorama-layers.ini';
let configDirty = false;
const configRooms = {};
const kVirtualCellSpanMax = 512;
const planeTokens = new Set(Object.keys(PLANE_DEFAULTS));
const roomKey = r => `${r.group}:${r.map}`;
function emptyPlane() {
  return {
    setOrder:false,order:0,setZ:false,z:0,setAlpha:false,alpha:255,
    setSource:false,source:'captured',setTransparent:false,transparent:'off',
    setRake:false,rake:0,
    setBow:false,bow:0,setThickness:false,thickness:0,
    setStack:false,stack:0,setStackCopies:false,stackCopies:0,
    setStackDensity:false,stackDensity:0,
    setStackDirection:false,stackDirection:'forward',
    setVoxel:false,voxel:0,setVoxelCopies:false,voxelCopies:0,
  };
}
function emptyVirtual() {
  return { setZ:false, z:0, setOrder:false, order:0,
           setAlpha:false, alpha:255 };
}
function roomConfig(r) {
  const key = roomKey(r);
  if (!configRooms[key])
    configRooms[key] = { planes:{}, virtual:[emptyVirtual(),emptyVirtual()] };
  return configRooms[key];
}
function resolvedPlane(token) {
  const base = PLANE_DEFAULTS[token] || {z:0,order:0,alpha:255};
  const edit = typeof room !== 'undefined' && room
    ? roomConfig(room).planes[token] || {} : {};
  let stack=0,copies=0,solid=false;
  if (edit.setVoxel && edit.voxel > 0) {
    stack=edit.voxel;solid=true;
    copies=edit.setVoxelCopies?edit.voxelCopies:12;
    copies=Math.max(2,Math.min(24,copies));
  } else if (edit.setStack && edit.stack > 0) {
    stack=edit.stack;
    if(edit.setStackCopies)copies=edit.stackCopies;
    else if(edit.setStackDensity)
      copies=Math.max(2,Math.min(8,Math.round(stack*edit.stackDensity)+1));
    else copies=3;
    copies=Math.max(1,Math.min(8,copies));
  }
  return {
    z: edit.setZ ? edit.z : base.z,
    order: edit.setOrder ? edit.order : base.order,
    alpha: edit.setAlpha ? edit.alpha : base.alpha,
    setOrder: !!edit.setOrder,
    rake:edit.setRake?edit.rake:0,
    bow:edit.setBow?edit.bow:0,
    thickness:edit.setThickness?edit.thickness:0,
    stack,copies,solid,
    direction:edit.setStackDirection
      ? Math.max(0,STACK_DIRECTIONS.indexOf(edit.stackDirection)):0,
  };
}
function strategyOfPlane(edit) {
  if(edit&&edit.setVoxel&&edit.voxel>0)return 'voxel';
  if(edit&&edit.setStack&&edit.stack>0)return 'stack';
  if(edit&&edit.setThickness&&edit.thickness>0)return 'thick';
  if(edit&&edit.setBow&&edit.bow!==0)return 'bow';
  if(edit&&edit.setRake&&edit.rake!==0)return 'rake';
  return 'flat';
}
function clearPlaneShape(edit) {
  edit.setRake=edit.setBow=edit.setThickness=edit.setStack=false;
  edit.setStackCopies=edit.setStackDensity=edit.setStackDirection=false;
  edit.setVoxel=edit.setVoxelCopies=false;
  edit.rake=edit.bow=edit.thickness=edit.stack=edit.stackDensity=edit.voxel=0;
  edit.stackCopies=edit.voxelCopies=0;edit.stackDirection='forward';
}
function resolvedVirtual(bg) {
  const base = VIRTUAL_DEFAULTS[bg];
  const edit = typeof room !== 'undefined' && room
    ? roomConfig(room).virtual[bg] : emptyVirtual();
  return {
    z: edit.setZ ? edit.z : base.z,
    order: edit.setOrder ? edit.order : base.order,
    alpha: edit.setAlpha ? edit.alpha : base.alpha,
    setOrder: !!edit.setOrder,
  };
}
const stripIniComment = line => {
  const semi = line.indexOf(';'), hash = line.indexOf('#');
  let cut = line.length;
  if (semi >= 0) cut = Math.min(cut, semi);
  if (hash >= 0) cut = Math.min(cut, hash);
  return line.slice(0, cut).trim();
};
function iniWords(rhs) {
  const values = {};
  for (const word of rhs.trim().split(/\s+/)) {
    const at = word.indexOf(':');
    if (at > 0) values[word.slice(0, at)] = word.slice(at + 1);
  }
  return values;
}
function resetLoadedConfig() {
  selectionAnchor=null;
  for (const key of Object.keys(store)) delete store[key];
  for (const key of Object.keys(pixelStore)) delete pixelStore[key];
  for (const key of Object.keys(stampStore)) delete stampStore[key];
  for (const key of Object.keys(configRooms)) delete configRooms[key];
}
function loadIniText(text, name) {
  resetLoadedConfig();
  sourceIniText = String(text || '');
  sourceIniName = name || 'diorama-layers.ini';
  let active = null;
  const warnings = [];
  for (const raw of sourceIniText.split(/\r?\n/)) {
    const line = stripIniComment(raw);
    if (!line) continue;
    if (line[0] === '[') {
      const match = line.match(/^\[layers:([0-9a-fA-F]{1,2}):([0-9a-fA-F]{1,2})\]$/);
      if (!match) { active = null; continue; }
      const group = parseInt(match[1],16), map = parseInt(match[2],16);
      active = DATA.rooms.find(r => r.group === group && r.map === map) || null;
      continue;
    }
    if (!active) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key=regionalKey(line.slice(0,eq).trim());
    if(!key)continue;
    const [token,region]=key;
    const values = iniWords(line.slice(eq+1));
    if(region!==undefined)values.terrain=region;
    if(!terrainMask(values.terrain))continue;
    const sm=token.match(/^bg([12])-(stamp|map)$/);
    if(sm){
      const bg=Number(sm[1])-1;
      for(const variant of regionalRooms(active,bg,values.terrain))loadStampIni(variant,bg,values);
      continue;
    }
    const pm = token.match(/^bg([12])-pixels$/);
    if (pm) {
      const bg=Number(pm[1])-1;
      for(const variant of regionalRooms(active,bg,values.terrain))loadPixelIni(variant,bg,values);
      continue;
    }
    if (planeTokens.has(token)) {
      const p = roomConfig(active).planes[token] || emptyPlane();
      if (values.z !== undefined && Number.isFinite(Number(values.z)))
        { p.setZ = true; p.z = Number(values.z); }
      if (values.order !== undefined && /^\d+$/.test(values.order))
        { p.setOrder = true; p.order = Number(values.order); }
      if (values.alpha !== undefined && /^\d+$/.test(values.alpha))
        { p.setAlpha = true; p.alpha = Number(values.alpha); }
      if (values.source !== undefined)
        { p.setSource = true; p.source = values.source; }
      if (values.transparent !== undefined && /^(off|black|cgram-[0-9a-fA-F]{2})$/.test(values.transparent))
        { p.setTransparent = true; p.transparent = values.transparent; }
      if (values.rake !== undefined && Number.isFinite(Number(values.rake)))
        { p.setRake = true; p.rake = Number(values.rake); }
      if (values.bow !== undefined && Number.isFinite(Number(values.bow)))
        { p.setBow = true; p.bow = Number(values.bow); }
      if (values.thick !== undefined && Number.isFinite(Number(values.thick)))
        { p.setThickness = true; p.thickness = Number(values.thick); }
      if (values.stack !== undefined && Number.isFinite(Number(values.stack)))
        { p.setStack = true; p.stack = Number(values.stack); }
      if (values.copies !== undefined && /^\d+$/.test(values.copies))
        { p.setStackCopies = true; p.stackCopies = Number(values.copies); }
      if (values.density !== undefined && Number.isFinite(Number(values.density)))
        { p.setStackDensity = true; p.stackDensity = Number(values.density); }
      if (values.dir !== undefined && STACK_DIRECTIONS.includes(values.dir))
        { p.setStackDirection = true; p.stackDirection = values.dir; }
      if (values.voxel !== undefined && Number.isFinite(Number(values.voxel)))
        { p.setVoxel = true; p.voxel = Number(values.voxel); }
      if (values.slices !== undefined && /^\d+$/.test(values.slices))
        { p.setVoxelCopies = true; p.voxelCopies = Number(values.slices); }
      roomConfig(active).planes[token] = p;
      continue;
    }
    const vm = token.match(/^bg([12])-virtual$/);
    if (!vm) continue;
    const bg = Number(vm[1]) - 1, v = roomConfig(active).virtual[bg];
    if(region!==undefined&&values.metatile===undefined&&values.cells===undefined)continue;
    if (values.z !== undefined && Number.isFinite(Number(values.z)))
      { v.setZ = true; v.z = Number(values.z); }
    if (values.order !== undefined && /^\d+$/.test(values.order))
      { v.setOrder = true; v.order = Number(values.order); }
    if (values.alpha !== undefined && /^\d+$/.test(values.alpha))
      { v.setAlpha = true; v.alpha = Number(values.alpha); }
    const parsedBand = /^\d+$/.test(values.band || '') ? Number(values.band) : -1;
    if (parsedBand < 0 || parsedBand >= BANDS.length) continue;
    for(const variant of regionalRooms(active,bg,values.terrain)) {
      const target = bucket(variant, bg);
      if (values.metatile !== undefined && /^[0-9a-fA-F]{1,2}$/.test(values.metatile)) {
        target.byId[parseInt(values.metatile, 16)] = parsedBand;
      } else if (values.cells !== undefined) {
        const cm = values.cells.match(/^(\d+),(\d+)-(\d+),(\d+)$/);
        const layer = variant.bg[bg];
        if (!cm || !layer) continue;
        const [x0,y0,x1,y1] = cm.slice(1).map(Number);
        const cellsW = layer.pagesWide * 16, cellsH = layer.pagesHigh * 16;
        if (x0 > x1 || y0 > y1 || x1 >= cellsW || y1 >= cellsH) {
          warnings.push(`${active.group}:${active.map} BG${bg+1} cells:${values.cells}`);
          continue;
        }
        for (let y=y0; y<=y1; y++) for (let x=x0; x<=x1; x++)
          target.byCell[y*cellsW+x] = parsedBand;
      }
    }
  }
  hydrateStampMasks();
  configDirty = false;captureEditorSavepoint();
  $('#iniName').textContent = sourceIniName;
  $('#iniName').title = warnings.length
    ? `${warnings.length} out-of-bounds virtual cell record(s) skipped` : 'Loaded INI';
}

function cellRuns(r, bg) {
  const layer = r.bg[bg], target = bucket(r,bg);
  if (!layer) return [];
  const cellsW = layer.pagesWide * 16;
  const entries = Object.entries(target.byCell)
    .map(([cell,band]) => ({cell:Number(cell),band:Number(band)}))
    .sort((a,b) => a.cell-b.cell);
  const runs = [];
  for (const e of entries) {
    const x = e.cell % cellsW, y = Math.floor(e.cell / cellsW);
    const last = runs[runs.length-1];
    if (last && last.band === e.band && last.y0 === y && last.y1 === y &&
        last.x1 + 1 === x) last.x1 = x;
    else runs.push({x0:x,y0:y,x1:x,y1:y,band:e.band});
  }
  if (runs.length > kVirtualCellSpanMax)
    throw new Error(`${r.group}:${r.map} BG${bg+1} needs ${runs.length} cell spans; maximum is ${kVirtualCellSpanMax}`);
  return runs;
}
const fmtIniNumber = value => Number(value).toFixed(4)
  .replace(/0+$/,'').replace(/\.$/,'').replace(/^-0$/,'0');
function planeIsAuthored(p) {
  return !!p && (p.setOrder||p.setZ||p.setAlpha||p.setSource||p.setTransparent||p.setRake||
    p.setBow||p.setThickness||p.setStack||p.setStackCopies||
    p.setStackDensity||p.setStackDirection||p.setVoxel||p.setVoxelCopies);
}
function planeIniLines(r) {
  const lines=[];
  for(const token of PLANE_TOKENS) {
    if(!EDITABLE_PLANE_TOKENS.has(token))continue;
    const p=roomConfig(r).planes[token];
    if(!planeIsAuthored(p))continue;
    const fields=[];
    if(p.setOrder)fields.push(`order:${p.order}`);
    if(p.setZ)fields.push(`z:${fmtIniNumber(p.z)}`);
    if(p.setAlpha)fields.push(`alpha:${p.alpha}`);
    if(p.setSource)fields.push(`source:${p.source}`);
    if(p.setTransparent)fields.push(`transparent:${p.transparent}`);
    if(p.setRake)fields.push(`rake:${fmtIniNumber(p.rake)}`);
    if(p.setBow)fields.push(`bow:${fmtIniNumber(p.bow)}`);
    if(p.setThickness)fields.push(`thick:${fmtIniNumber(p.thickness)}`);
    if(p.setStack)fields.push(`stack:${fmtIniNumber(p.stack)}`);
    if(p.setStackCopies)fields.push(`copies:${p.stackCopies}`);
    if(p.setVoxel)fields.push(`voxel:${fmtIniNumber(p.voxel)}`);
    if(p.setVoxelCopies)fields.push(`slices:${p.voxelCopies}`);
    if(p.setStackDensity)fields.push(`density:${fmtIniNumber(p.stackDensity)}`);
    if(p.setStackDirection)fields.push(`dir:${p.stackDirection}`);
    lines.push(`${token} = ${fields.join(' ')}`);
  }
  return lines;
}
function virtualIniLines(r) {
  const lines = [];
  for (let bg=0; bg<2; bg++) {
    if (!r.bg[bg]) continue;
    const token = `bg${bg+1}-virtual`, v = roomConfig(r).virtual[bg];
    const geometry = [];
    if (v.setZ) geometry.push(`z:${Number(v.z).toFixed(4).replace(/0+$/,'').replace(/\.$/,'')}`);
    if (v.setOrder) geometry.push(`order:${v.order}`);
    if (v.setAlpha) geometry.push(`alpha:${v.alpha}`);
    if (geometry.length) lines.push(`${token} = ${geometry.join(' ')}`);
    const records=regionalRecords(r,bg,variant=>{
      const result=[],target = bucket(variant,bg);
      for (const [id,b] of Object.entries(target.byId).sort((a,b)=>Number(a[0])-Number(b[0])))
        result.push(`${token} = metatile:${Number(id).toString(16).toUpperCase().padStart(2,'0')} band:${b}`);
      for (const s of cellRuns(variant,bg))
        result.push(`${token} = cells:${s.x0},${s.y0}-${s.x1},${s.y1} band:${s.band}`);
      return result;
    });
    if(records.filter(line=>line.includes(" cells:")).length>kVirtualCellSpanMax)
      throw new Error(`Maximum ${kVirtualCellSpanMax} saved cell spans per BG across terrain versions`);
    lines.push(...records);
  }
  return lines;
}
const roomIniLines = r => [...planeIniLines(r),...virtualIniLines(r),...pixelIniLines(r),...stampIniLines(r)];
function ownedIniLine(line) {
  const match=stripIniComment(line).match(/^([^=\s]+)\s*=/);
  if(!match)return false;
  const key=regionalKey(match[1]);
  if(!key)return false;
  const [token,region]=key;
  return EDITABLE_PLANE_TOKENS.has(token)&&region===undefined||
    /^bg[12]-(virtual|pixels|stamp|map)$/.test(token)&&!!terrainMask(region);
}
function mergeDioramaIni() {
  const hadFinalNewline = /\r?\n$/.test(sourceIniText);
  const lines = sourceIniText.split(/\r?\n/);
  if (hadFinalNewline) lines.pop();
  const out = [];
  const written = new Set();
  let i = 0;
  while (i < lines.length) {
    if (!/^\s*\[/.test(lines[i])) { out.push(lines[i++]); continue; }
    const start = i, header = stripIniComment(lines[i++]);
    while (i < lines.length && !/^\s*\[/.test(lines[i])) i++;
    const block = lines.slice(start, i);
    const match = header.match(/^\[layers:([0-9a-fA-F]{1,2}):([0-9a-fA-F]{1,2})\]$/);
    const r = match && DATA.rooms.find(x => x.group===parseInt(match[1],16) && x.map===parseInt(match[2],16));
    if (!r) { out.push(...block); continue; }
    written.add(roomKey(r));
    const canonical = roomIniLines(r);
    const body = block.slice(1);
    const first = body.findIndex(ownedIniLine);
    const kept = body.filter(line => !ownedIniLine(line));
    out.push(block[0]);
    if (first >= 0) {
      const beforeCount = body.slice(0,first)
        .filter(line => !ownedIniLine(line)).length;
      out.push(...kept.slice(0,beforeCount), ...canonical, ...kept.slice(beforeCount));
    } else {
      let at = kept.length;
      while (at > 0 && !kept[at-1].trim()) at--;
      out.push(...kept.slice(0,at), ...canonical, ...kept.slice(at));
    }
  }
  for (const r of DATA.rooms) {
    if (written.has(roomKey(r)) || !roomIniLines(r).length) continue;
    if (out.length && out[out.length-1].trim()) out.push('');
    out.push(`[layers:${r.group.toString(16).toUpperCase().padStart(2,'0')}:${r.map.toString(16).toUpperCase().padStart(2,'0')}]`);
    out.push(...roomIniLines(r));
  }
  return out.join('\n') + (hadFinalNewline || out.length ? '\n' : '');
}
/* The AUTHENTIC band of one 8x8 tile, before any edit.
 *
 * A room is not flat to begin with: the priority bit already splits each BG
 * into two depths, and the game uses it to put a tile in front of sprites.
 * Defaulting everything to "behind" would misrepresent every priority-1 tile
 * in the room and make the editor's first render disagree with the game. So
 * the baseline is the bit the artist already set, and an edit is a DELTA from
 * authentic -- which is also what makes "reset to default" meaningful. */
const authenticBand = entry => (entry & 0x2000) ? 2 : 1;

/* The resolved band of one 8x8 tile. Explicit assignment wins, cell before
 * metatile id; otherwise the tile keeps its authentic band. Both keys are one
 * index away from what a runtime lookup is handed (tile_x, tile_y), so this
 * resolution order is expressible there unchanged. */
function bandAt(st, L, tx, ty) {
  const cx = tx >> 1, cy = ty >> 1;
  const cell = cy*L.cellsW + cx;
  if (st.byCell[cell] !== undefined) return st.byCell[cell];
  const id = L.cellId[cell];
  if (st.byId[id] !== undefined) return st.byId[id];
  return authenticBand(L.words[ty*L.tilesW + tx]);
}
/* True when a cell has been moved off what the ROM says. Drives the outline
 * overlay: the interesting thing to see is the delta, not the baseline. */
function cellEdited(st, L, cx, cy) {
  const cell = cy*L.cellsW + cx;
  return st.byCell[cell] !== undefined || st.byId[L.cellId[cell]] !== undefined;
}

/* ---- state ------------------------------------------------------------ */
let room = terrainRoom(DATA.rooms[0]), bgIndex = 0, L = null, st = null;
let band = 2, brush = 'select', mode = '2d', tint = false;
let planeToken = 'bg1';
let lastEntry = null;
const selectedCells = new Set();
let selectionAnchor = null;
let selectionLayer = '', showEditOutlines = true;
function syncSelectionLayer() {
  const key = keyOf(room,bgIndex);
  if (selectionLayer !== key) {
    selectedCells.clear(); selectedStampKeys.clear(); selectionRect=null;selectionAnchor=null;
    lastEntry = null; pixelCell = null; pixelStamp=null;
    if(brush==='stamp'&&tileClipboard?.key!==key)$('#bSelect').onclick();
    selectionLayer = key;
  }
  refreshSelectionControls();
}
function setSelectionAnchor(cx,cy) {
  const bounds=mapBounds(room,bgIndex,L);
  selectionAnchor=cx>=bounds.x0&&cy>=bounds.y0&&cx<bounds.x1&&cy<bounds.y1
    ?[cx,cy]:null;
}
function focusTileAt(cx,cy) {
  const tile=displayedCell(room,bgIndex,L,cx,cy);
  pixelStamp=tile&&stampBucket(room,bgIndex).cells[`${cx},${cy}`]?`${cx},${cy}`:null;
  pixelCell=tile&&!pixelStamp?cy*L.cellsW+cx:null;
  lastEntry=tile?tile.words[0]:null;
}
function refreshSelectionControls() {
  $('#selectionInfo').textContent = `${selectedCells.size+selectedStampKeys.size} cells selected`
    +(selectionAnchor?` · start ${selectionAnchor[0]},${selectionAnchor[1]}`:'');
  $('#deselect').disabled = selectedCells.size === 0 && selectedStampKeys.size === 0 && !selectionRect && brush!=='stamp' && lastEntry === null;
  $('#applyBand').disabled = selectedCells.size === 0 && selectedStampKeys.size === 0;
  $('#pixelSelectionBlack').disabled=$('#pixelSelectionBlackQuick').disabled=$('#applyBand').disabled;
  refreshPixelEditor();refreshStampControls();refreshEditorFeedback();
}
function selectCell(cx,cy,allInstances = false) {
  selectionRect=null;
  const pasted=stampBucket(room,bgIndex).cells[`${cx},${cy}`];
  if(pasted) {
    pixelCell=null;pixelStamp=`${cx},${cy}`;selectedStampKeys.add(pixelStamp);
    refreshSelectionControls();return;
  }
  pixelStamp=null;
  if (cx<0 || cy<0 || cx>=L.cellsW || cy>=L.cellsH) return;
  const cell = cy*L.cellsW+cx;
  pixelCell = cell;
  if (allInstances) {
    const id = L.cellId[cell];
    for (let i=0; i<L.cellId.length; i++)
      if (L.cellId[i] === id) selectedCells.add(i);
  } else selectedCells.add(cell);
  refreshSelectionControls();
}
let view = { x:0, y:0, scale:1 };           /* 2D pan/zoom */
/* The camera frames a WINDOW of the level, not the whole thing. A room is up
 * to 4096px long and the game only ever shows 256 of them, so orbiting a
 * single 5:1 quad tells you nothing about what the player sees -- the far end
 * swings out of frame and the parallax is unreadable. focusX walks that window
 * along the level the way the camera does in play. */
let orbit = { yaw:0.0, pitch:-0.34, dist:1.25, focusX:0.5, focusY:0.0 };

const $ = s => document.querySelector(s);
const cvs = $('#map2d'), ctx = cvs.getContext('2d', { alpha:false });
const glc = $('#gl');

function bandRGB(i) {
  const css = getComputedStyle(document.documentElement)
      .getPropertyValue(BANDS[i].css).trim();
  const n = parseInt(css.slice(1), 16);
  return [(n>>16)&255, (n>>8)&255, n&255];
}

/* ---- band surfaces -----------------------------------------------------
 * One full-level RGBA surface per band -- the same thing the engine builds:
 * a captured plane per destination, composited at its own depth. Tiles not
 * in a band are simply absent from its surface, which is the mask the
 * provider expresses by returning false. */
function buildSurfaces() {
  const out = [];
  const rgb = BANDS.map((_, i) => tint ? bandRGB(i) : null);
  for (let b=0; b<3; b++) {
    const img = new ImageData(L.w, L.h);
    out.push(img);
  }
  for (let ty=0; ty<L.tilesH; ty++) for (let tx=0; tx<L.tilesW; tx++) {
    const b = bandAt(st, L, tx, ty);
    blitTile(L, L.words[ty*L.tilesW+tx], out[b].data, L.w, tx*8, ty*8, rgb[b]);
  }
  return out;
}
let surfaces = null, surfacesDirty = true;
/* Cells whose band changed since the last raster. A brush stroke touches a
 * handful of them, and repainting the whole level for each one costs ~18ms --
 * enough to make dragging a brush feel heavy on a long room. Past a threshold
 * a full rebuild is cheaper than bookkeeping, so this degrades into one. */
const dirtyCells = new Set();
const kMaxIncrementalCells = 2048;
function markCellDirty(cell) {
  if (surfacesDirty) return;
  if (dirtyCells.size >= kMaxIncrementalCells) { surfacesDirty = true; dirtyCells.clear(); }
  else dirtyCells.add(cell);
}
/* A tile belongs to exactly one band, so a cell's contribution to the
 * flattened composite is just its own four tiles -- clear the cell's rect
 * everywhere, then re-blit it into whichever band now owns it. */
function repaintCell(cell) {
  const cx = cell % L.cellsW, cy = (cell / L.cellsW) | 0;
  const rgb = BANDS.map((_, i) => tint ? bandRGB(i) : null);
  for (let b = 0; b < 3; b++) {
    const d = surfaces[b].data;
    for (let py = 0; py < 16; py++) {
      let o = ((cy*16 + py) * L.w + cx*16) * 4;
      for (let px = 0; px < 16; px++, o += 4)
        { d[o]=0; d[o+1]=0; d[o+2]=0; d[o+3]=0; }
    }
  }
  for (let q = 0; q < 4; q++) {
    const tx = cx*2 + (q&1), ty = cy*2 + (q>>1);
    const b = bandAt(st, L, tx, ty);
    blitTile(L, L.words[ty*L.tilesW+tx], surfaces[b].data, L.w, tx*8, ty*8, rgb[b]);
  }
  if (composite) {
    const cc = composite.getContext('2d');
    cc.clearRect(cx*16, cy*16, 16, 16);
    const one = new ImageData(16, 16);
    for (let b = 0; b < 3; b++) {
      const src = surfaces[b].data;
      for (let py = 0; py < 16; py++) {
        let so = ((cy*16+py)*L.w + cx*16)*4, dof = (py*16)*4;
        for (let px = 0; px < 16; px++, so += 4, dof += 4) {
          if (!src[so+3]) continue;
          one.data[dof]=src[so]; one.data[dof+1]=src[so+1];
          one.data[dof+2]=src[so+2]; one.data[dof+3]=255;
        }
      }
    }
    const sc = document.createElement('canvas');
    sc.width = 16; sc.height = 16;
    sc.getContext('2d').putImageData(one, 0, 0);
    cc.drawImage(sc, cx*16, cy*16);
  }
}
const invalidate = () => {
  surfacesDirty = true; compositeDirty = true; glDirty = true;
  invalidateOther(); invalidateGameComposite();
  tally(); draw();
};
function surfacesNow() {
  if (surfacesDirty || !surfaces) {
    surfaces = buildSurfaces(); surfacesDirty = false;
    dirtyCells.clear(); compositeDirty = true;
  } else if (dirtyCells.size) {
    for (const cell of dirtyCells) repaintCell(cell);
    dirtyCells.clear();
  }
  return surfaces;
}

/* ---- 2D view ----------------------------------------------------------- */
/* Returns false when the element has no layout yet. Fitting against a 0x0
 * rect yields scale 0, which draws nothing and never recovers on its own --
 * that is a black 2D view for the rest of the session. An initial load can
 * land here while fonts are still resolving, so the caller re-fits once real
 * layout arrives. */
let fitted = false;
function fitView() {
  const r = cvs.getBoundingClientRect();
  if (r.width < 8 || r.height < 8) return false;
  const b=mapBounds(room,bgIndex,L),w=(b.x1-b.x0)*16,h=(b.y1-b.y0)*16;
  view.scale = Math.min(r.width / w, r.height / h);
  view.x = (r.width - w*view.scale)/2 - b.x0*16*view.scale;
  view.y = (r.height - h*view.scale)/2 - b.y0*16*view.scale;
  fitted = true;
  return true;
}
function zoomBy(k) {
  const r = cvs.getBoundingClientRect();
  const mx = r.width/2, my = r.height/2;
  const ns = Math.max(0.05, Math.min(8, view.scale * k));
  view.x = mx - (mx - view.x) * (ns/view.scale);
  view.y = my - (my - view.y) * (ns/view.scale);
  view.scale = ns; draw();
}
let composite = null, compositeDirty = true;
/* The three band surfaces flattened into one image. Rebuilding this allocated
 * three canvases and ran three putImageData EVERY draw, which is what made
 * panning and painting feel heavy; it only actually changes when a band
 * assignment changes. */
function compositeNow() {
  if (!composite || composite.width !== L.w || composite.height !== L.h) {
    composite = document.createElement('canvas');
    composite.width = L.w; composite.height = L.h;
    compositeDirty = true;
  }
  if (!compositeDirty) return composite;
  const surf = surfacesNow();
  const cc = composite.getContext('2d');
  cc.clearRect(0, 0, L.w, L.h);
  const scratch = document.createElement('canvas');
  scratch.width = L.w; scratch.height = L.h;
  const sc = scratch.getContext('2d');
  for (const b of [0,1,2]) {            /* furthest band first */
    sc.putImageData(surf[b], 0, 0);
    cc.drawImage(scratch, 0, 0);
  }
  compositeDirty = false;
  return composite;
}
let game2dCache = new Map();
function invalidateGameComposite() {
  game2dCache.clear(); native2dCache=null; nativeBandCache=null;
}
function gameLayerSurfaces(r, bg, original=false) {
  const key = `${sceneKey(r)}:${bg}:${tint ? 1 : 0}:${animationPhase(r)}`
    + `:${bg === 1 ? bg2PagePhase(r) : 0}:${original?1:0}`;
  if (game2dCache.has(key)) return game2dCache.get(key);
  const layer = decodeLayer(r,bg), state = bucket(r,bg);
  if (!layer) return null;
  const bounds=original?{x0:0,y0:0,x1:layer.cellsW,y1:layer.cellsH}:mapBounds(r,bg,layer),w=(bounds.x1-bounds.x0)*16,h=(bounds.y1-bounds.y0)*16;
  const ox=-bounds.x0*16,oy=-bounds.y0*16;
  const images = [new ImageData(w,h), new ImageData(w,h)];
  for (let ty=0; ty<layer.tilesH; ty++) for (let tx=0; tx<layer.tilesW; tx++) {
    const entry = layer.words[ty*layer.tilesW+tx];
    const nativeHigh = (entry & 0x2000) ? 1 : 0;
    const virtualBand = bandAt(state,layer,tx,ty);
    blitTile(layer,entry,images[nativeHigh].data,w,ox+tx*8,oy+ty*8,
             tint&&!original ? bandRGB(virtualBand) : null,tx*8,ty*8,original?ZERO_PIXEL_MASK:undefined);
  }
  for(const [key,c] of Object.entries(original?{}:stampBucket(r,bg).cells)) {
    const [x,y]=key.split(',').map(Number),px=ox+x*16,py=oy+y*16;
    for(const image of images)for(let row=0;row<16;row++)
      image.data.fill(0,((py+row)*w+px)*4,((py+row)*w+px+16)*4);
    images.forEach((image,high)=>blitStamp(layer,c,image,w,px,py,high));
  }
  const canvases = images.map(image => {
    const canvas = document.createElement('canvas');
    canvas.width=w; canvas.height=h;
    canvas.getContext('2d').putImageData(image,0,0);
    return canvas;
  });
  let built = { L:{...layer,w,h,originX:bounds.x0*16,originY:bounds.y0*16}, low:canvases[0], high:canvases[1] };
  if (bg === 1 && r.bg2PageCycle && (original||!Object.keys(stampBucket(r,bg).cells).length && !stampBucket(r,bg).bounds)) {
    const page = bg2PageIndex(r), sx = (page & 1) * 256;
    const sy = (page >> 1) * 256;
    const cropped = canvases.map(source => {
      const canvas = document.createElement('canvas');
      canvas.width = canvas.height = 256;
      canvas.getContext('2d').drawImage(
        source, sx, sy, 256, 256, 0, 0, 256, 256);
      return canvas;
    });
    built = { L:{...layer,w:256,h:256}, low:cropped[0], high:cropped[1] };
  }
  game2dCache.set(key,built);
  return built;
}
function layerCopies2d(bg, surface) {
  if (bg === bgIndex) return [{x:surface.L.originX||0,y:surface.L.originY||0}];
  if (!showBoth) return [];
  const span = otherOffset.repeat
    ? Math.ceil((L.w + surface.L.w) / surface.L.w) + 1 : 1;
  const first = otherOffset.repeat
    ? Math.floor((-L.w/2 - surface.L.w) / surface.L.w) : 0;
  const copies = [];
  for (let i=0;i<span;i++) copies.push({
    x: otherOffset.x + (first+i)*surface.L.w + (surface.L.w-L.w)/2,
    y: otherOffset.y + (L.h-surface.L.h)/2,
  });
  return copies;
}
function drawGameLayer(bg, high) {
  if (!room.bg[bg] || (bg !== bgIndex && !showBoth)) return;
  const surface = gameLayerSurfaces(room,bg,compareOriginal);
  const image = high ? surface.high : surface.low;
  for (const copy of layerCopies2d(bg,surface))
    ctx.drawImage(image, view.x+copy.x*view.scale, view.y+copy.y*view.scale,
                  surface.L.w*view.scale, surface.L.h*view.scale);
}
function drawActor2d() {
  if (!actor.show) return;
  ctx.save();
  ctx.strokeStyle = 'rgba(255,86,120,.95)'; ctx.lineWidth = 2;
  ctx.fillStyle = 'rgba(255,86,120,.78)';
  const ax = view.x + actor.x*view.scale, ay = view.y + actor.y*view.scale;
  ctx.fillRect(ax, ay, actor.w*view.scale, actor.h*view.scale);
  ctx.strokeRect(ax, ay, actor.w*view.scale, actor.h*view.scale);
  ctx.restore();
}
function drawNative2d() {
  const r=cvs.getBoundingClientRect(),w=Math.round(r.width*devicePixelRatio);
  const h=Math.round(r.height*devicePixelRatio);
  if(cvs.width!==w||cvs.height!==h){cvs.width=w;cvs.height=h;}
  ctx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);
  ctx.fillStyle='#0d0f14';ctx.fillRect(0,0,r.width,r.height);
  ctx.imageSmoothingEnabled=false;
  const scale=Math.min(r.width/DATA.frameWidth,r.height/DATA.frameHeight);
  const x=(r.width-DATA.frameWidth*scale)/2,y=(r.height-DATA.frameHeight*scale)/2;
  ctx.drawImage(nativeFrameCanvas(),x,y,DATA.frameWidth*scale,DATA.frameHeight*scale);
  const state=nativeFrameState(room);
  $('#hud').innerHTML=`<b>${room.group}:${room.map}</b> native 256×224 stable frame`
    +` &nbsp; ${terrainLabel(room)} terrain`
    +` &nbsp; camera ${nativeCamera.x},${nativeCamera.y}`
    +` &nbsp; profile $${Number(room.videoProfile).toString(16).padStart(2,'0')}`
    +(room.raster?` &nbsp; raster R${room.raster}`:'')
    +`<br>BG1 ${state.h[0][0]},${state.v[0][0]}`
    +` &nbsp; BG2 ${state.h[1][0]},${state.v[1][0]}`
    +` &nbsp; TM $${room.video[0].toString(16).padStart(2,'0')}`
    +` TS $${room.video[1].toString(16).padStart(2,'0')}`
    +` CGWSEL $${room.video[2].toString(16).padStart(2,'0')}`
    +` CGADSUB $${room.video[3].toString(16).padStart(2,'0')}`;
}
function draw2d() {
  const r = cvs.getBoundingClientRect();
  const w = Math.round(r.width * devicePixelRatio);
  const h = Math.round(r.height * devicePixelRatio);
  if (cvs.width !== w || cvs.height !== h) { cvs.width = w; cvs.height = h; }
  ctx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);
  ctx.fillStyle = '#0d0f14'; ctx.fillRect(0,0,r.width,r.height);
  ctx.imageSmoothingEnabled = false;
  /* Authentic Mode-1 background ranks. Virtual classification is
   * presentation-only, so it may tint these pixels but never changes this
   * flat-game ordering. OBJ2 sits between the low and high BG ranks. */
  drawGameLayer(1,false);  /* BG2 priority 0, hardware rank 7  */
  drawGameLayer(0,false);  /* BG1 priority 0, hardware rank 8  */
  drawActor2d();            /* reference for OBJ priority 2, rank 10 */
  drawGameLayer(1,true);   /* BG2 priority 1, hardware rank 11 */
  drawGameLayer(0,true);   /* BG1 priority 1, hardware rank 12 */

  drawStampGuides();
  /* Authored edits and the current selection have separate overlays. */
  if (showEditOutlines && !compareOriginal) {
    const color=highlightColor();
    ctx.lineWidth=1.5;ctx.strokeStyle=color.startsWith('--')
      ?getComputedStyle(document.documentElement).getPropertyValue(color).trim():color;
    for(const {x:cx,y:cy} of currentHighlightedTiles()) {
      const size=16*view.scale,x=view.x+cx*size,y=view.y+cy*size;
      if(x+size<0||y+size<0||x>r.width||y>r.height)continue;
      const shown=Math.max(3,size-4);
      ctx.strokeRect(x+(size-shown)/2,y+(size-shown)/2,shown,shown);
    }
  }
  {
    ctx.strokeStyle = '#5aa9ff';
    ctx.fillStyle = 'rgba(90,169,255,0.12)';
    ctx.lineWidth = 1.5;
    for (const cell of selectedCells) {
      const cx=cell%L.cellsW,cy=Math.floor(cell/L.cellsW);
      const x=view.x+cx*16*view.scale,y=view.y+cy*16*view.scale,size=16*view.scale;
      const shown=Math.max(4,size-1);
      ctx.fillRect(x,y,size,size);ctx.strokeRect(x+(size-shown)/2,y+(size-shown)/2,shown,shown);
    }
  }
}
