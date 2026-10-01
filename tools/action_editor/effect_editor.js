/* Sparse environmental overrides. The shared C parser validates every edit;
 * this module owns only controls, undo and document transport. */
const EffectEditor=(()=>{
  const empty='[effects]\nversion=1\n';
  let documentText=window.__ACTION_EFFECTS__||empty, saved=documentText;
  let selected='', sources=[], sourceKey='', sourceScope='', records=new Map();
  const status=message=>{$('#effectStatus').textContent=message;$('#effectDocumentStatus').textContent=message;};
  function decode(text) {
    const map=new Map();let record=null;
    for(const raw of text.split(/\r?\n/)) {
      const line=raw.split(/[;#]/,1)[0].trim();if(!line)continue;
      if(line.startsWith('[')) {
        const match=line.match(/^\[(source|emitter):([\da-f]+):([\da-f]+):(\d+):([a-z-]+):([\da-f]+)\]$/i);
        record=null;
        if(match) {
          const id=[match[1],parseInt(match[2],16).toString(16).padStart(2,'0'),
            parseInt(match[3],16).toString(16).padStart(2,'0'),Number(match[4]),match[5],
            parseInt(match[6],16).toString(16).padStart(8,'0')].join(':');
          record={};map.set(id,record);
        }
      }else if(record){const at=line.indexOf('=');record[line.slice(0,at).trim()]=line.slice(at+1).trim();}
    }
    return map;
  }
  function encode(map) {
    return empty+[...map].sort(([a],[b])=>a.localeCompare(b)).map(([id,r])=>
      `\n[${id}]\n`+Object.entries(r).map(([k,v])=>`${k}=${v}\n`).join('')).join('');
  }
  records=decode(documentText);
  function restore(text) {
    if(drag?.effects){EmitterMapTools.cancel();drag=null;}
    const previous=records;
    documentText=text;records=decode(text);
    const added=[...records.keys()].find(id=>!previous.has(id));
    if(added)selected=added;
    rebuildSources(sources.filter(s=>!s.emitter));
    refreshInspector();
    SharedRoomPreview.invalidate();draw();
    status(text===saved?'Effects match the loaded/exported document.':'Unsaved effect changes. Export action-effects.ini to use them in game.');
  }
  function change(text,label) {
    try {
      SharedRoomPreview.validateEffects(text);
      if(text===documentText)return true;
      if(pendingOp)commitOp();
      undoStack.push({label,effects:{before:documentText,after:text}});
      if(undoStack.length>kMaxUndo)undoStack.shift();redoStack.length=0;
      restore(text);refreshHistoryButtons();return true;
    }catch(error){status(error.message);refreshInspector();return false;}
  }
  const moteFields=[['emitterSizeMin','size-min','0.35'],['emitterSizeMax','size-max','0.75'],
    ['emitterTravelX','travel-x','0'],['emitterTravelY','travel-y',''],['emitterWander','wander','2'],
    ['emitterSpread','spread','1'],['emitterSeed','seed','']];
  const particleNames=['motes','particle-area','water-surface','drips','waterfall-spray'];
  const lightNames=['soft-light','light-fan','wall-torch','forest-canopy','forest-forward','cave-sheen','cave-light','tower-window','moonlight','blood-water','wet-timber','castle-light','castle-sky',
    'lava-pit','lava-lake','water-splash','waterfall','waterfall-mist','enemy-fireball','statue-orb','lightning-trap','sword-beam','jungle-fireball','lightning-link','viper-lightning','lava-fireball',
    'statue-fire','molten-rock','minotaur-axe','flaming-wheel','wheel-projectile','ice-ball','tanzara-projectile','centaur-lightning','boss-lightning','northwall-magic'];
  const dimNames=['exposure','cave-light','castle-light'];
  const fieldNames=['particle-area','light-fan','water-surface','drips','waterfall-spray','cloud-bank','wet-contour'];
  const fieldFields=[['emitterPlacement','placement','foreground'],['emitterPattern','pattern','motes'],
    ['emitterAngle','angle','0'],['emitterFan','fan','36'],['emitterStrands','strands','8'],
    ['emitterSoftness','softness','0.35'],['emitterDrift','drift','0'],['emitterAmplitude','amplitude','2']];
  function refreshInspector() {
    const source=sources.find(s=>s.id===selected), record=records.get(selected)||{};
    const disabled=!source;
    for(const id of ['effectEnabled','effectIntensity','effectColor','effectReset'])$('#'+id).disabled=disabled;
    $('#effectReach').disabled=disabled||!source.reach;
    $('#emitterControls').hidden=!source?.emitter;
    $('#effectReset').textContent=source?.emitter?'Remove emitter':'Restore source defaults';
    for(const key of ['x','y','width','height','particles','lifetime','mist-height'])
      $('#emitter'+key).value=record[key]??({width:96,height:64,particles:24,lifetime:240,'mist-height':26}[key]??0);
    const name=source?.name,area=name==='particle-area',large=area||name==='exposure';
    $('#effectColor').disabled=disabled||name==='exposure';
    $('#emitterlifetime').disabled=name==='exposure'||name==='soft-light';
    $('#receiverControls').hidden=!lightNames.includes(name)&&!dimNames.includes(name);
    for(const [mode,names] of [['Light',lightNames],['Dim',dimNames]]) {
      const available=names.includes(name),prefix=mode.toLowerCase();
      const active=['scenery','player','enemies'].some(target=>record[`${prefix}-${target}`]!==undefined);
      $('#effect'+mode+'Targets').checked=active||name==='exposure'&&mode==='Dim';
      $('#effect'+mode+'Targets').disabled=!available||name==='exposure'&&mode==='Dim';
      for(const target of ['Scenery','Player','Enemies']) {
        const element=$('#effect'+mode+target);element.disabled=!available||!$('#effect'+mode+'Targets').checked;
        element.checked=record[`${prefix}-${target.toLowerCase()}`]!==undefined?record[`${prefix}-${target.toLowerCase()}`]==='1':target==='Scenery';
      }
    }
    $('#effectIntensity').max=name==='exposure'?1:4;
    $('#contourControls').hidden=name!=='wet-contour';
    $('#emitterPoints').value=record.points??'-32,0 32,0';
    $('#emitterparticles').disabled=!particleNames.includes(name);
    $('#emitterparticles').max=area?16:128;
    document.querySelector('label[for="emitterparticles"]').textContent=area?'Density / 256px cell':'Particle count';
    for(const key of ['width','height'])$('#emitter'+key).max=large?16384:512;
    $('#fieldControls').hidden=!fieldNames.includes(name);
    for(const [id,key,fallback] of fieldFields) {
      const element=$('#'+id);element.value=record[key]??fallback;
      element.disabled=key==='pattern'? !area : ['angle','fan'].includes(key)?name!=='light-fan':
        ['strands','softness'].includes(key)?!['light-fan','cloud-bank'].includes(name):
        ['drift','amplitude'].includes(key)?!['water-surface','cloud-bank'].includes(name):false;
    }
    $('#emitterStrands').max=name==='cloud-bank'?24:32;
    document.querySelector('label[for="emitterAmplitude"]').textContent=name==='cloud-bank'?'Vertical billow':'Wave height';
    $('#fieldHint').textContent=area?'One region covers the room. Density is particles per 256px world cell; only nearby cells are generated. Seeds stay fixed as you scroll.':
      name==='light-fan'?'X/Y is the opening centre. Width spreads origins across the sill or arch; height is ray length. 0° points down. Rays are clipped, never bent at screen edges.':
      name==='water-surface'?'X/Y is the area centre; its top edge is the water surface. Ripples and glints share the wave clock. This adds surface geometry, not refraction.':
      name==='drips'?'Drops fall from the top edge toward the bottom edge. Place this on a verified wet ceiling.':
      name==='waterfall-spray'?'Spray rises from the bottom edge. Anchor this edge to the native waterfall splash.':
      name==='cloud-bank'?'Layered cloud lobes drift over one area; use a blue tint for damp mist or grey for sky clouds.':'';
    $('#emittermist-height').disabled=source?.name!=='ground-mist';
    $('#moteControls').hidden=!particleNames.includes(name);
    for(const [id,key,fallback] of moteFields) {
      $('#'+id).value=record[key]??fallback;
      $('#'+id).disabled=['water-surface','drips','waterfall-spray'].includes(name)&&['travel-x','travel-y','wander','spread'].includes(key);
    }
    $('#emitterAutoRise').disabled=!['motes','particle-area'].includes(name);
    $('#emitterColorEndEnabled').disabled=false;
    $('#emitterColorEndEnabled').checked=record['color-end']!==undefined;
    $('#emitterColorEnd').disabled=record['color-end']===undefined;
    $('#emitterColorEnd').value='#'+(record['color-end']??record.color??'ffffff');
    $('#effectEnabled').checked=record.enabled!=='0';
    $('#effectIntensity').value=record.intensity??'1';
    $('#effectReach').value=record.reach??'1';
    $('#effectColor').value='#'+(record.color??'ffffff');
    $('#effectSourceInfo').textContent=source?
      `${source.name} at ${source.x}, ${source.y}. ID ${source.id}. `+
      (source.name==='ground-mist'?(source.support===undefined?'Preview to inspect supported spans.':source.support?`${source.support} supported spans. No surfaces outside the painted area are used.`:'No supported floor inside this area. Move or expand it to include a cyan floor edge.'):
      source.actor?'Actor family override. Applies to all recognized actors of this kind in this room and terrain.':source.emitter?'Placed emitter in playfield coordinates.':source.reach?'Reach changes the spill radius without brightening its centre.':'Compound source: tint/intensity affect the whole group.'):
      'Select Shared renderer and move the camera to discover native environmental sources.';
  }
  $('#effectSource').onchange=e=>{selected=e.target.value;refreshInspector();};
  for(const [id,key] of [['effectEnabled','enabled'],['effectIntensity','intensity'],['effectReach','reach'],['effectColor','color']])
    $('#'+id).onchange=e=>{
      if(!selected)return;
      const next=new Map(records),value=id==='effectEnabled'?(e.target.checked?'1':'0'):
        id==='effectColor'?e.target.value.slice(1):e.target.value;
      next.set(selected,{...next.get(selected),[key]:value});
      change(encode(next),'edit environmental effect');
    };
  function receivers(mode) {
    if(!selected)return;
    const next=new Map(records),r={...next.get(selected)},prefix=mode.toLowerCase();
    for(const target of ['Scenery','Player','Enemies']) {
      const key=`${prefix}-${target.toLowerCase()}`;
      if($('#effect'+mode+'Targets').checked)r[key]=$('#effect'+mode+target).checked?'1':'0';else delete r[key];
    }
    next.set(selected,r);change(encode(next),'edit effect receivers');
  }
  for(const mode of ['Light','Dim']) {
    $('#effect'+mode+'Targets').onchange=()=>receivers(mode);
    for(const target of ['Scenery','Player','Enemies'])$('#effect'+mode+target).onchange=()=>receivers(mode);
  }
  for(const key of ['x','y','width','height','particles','lifetime','mist-height'])
    $('#emitter'+key).onchange=e=>editEmitter(selected,{[key]:e.target.value});
  function newEmitterId(name) {
    let id;
    do{id=['emitter',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),
      terrainProfile,name,crypto.getRandomValues(new Uint32Array(1))[0].toString(16).padStart(8,'0')].join(':');}
    while(records.has(id));
    return id;
  }
  function placeEmitter(name,x,y) {
    const preset=name.startsWith('preset:')?name.slice(7):null;
    if(preset)name=preset==='halo'?'soft-light':'particle-area';
    const presets={halo:{width:'256',height:'96',color:'bacbff',intensity:'.65'},
      dust:{color:'d5b990','travel-y':'-32',wander:'12','size-min':'.7','size-max':'1.8',lifetime:'480'},
      leaves:{color:'739644','travel-y':'160',wander:'24','size-min':'1','size-max':'2.5',lifetime:'360'},
      snow:{color:'d5e8ff','travel-x':'32','travel-y':'224',wander:'12',lifetime:'480'},
      sand:{color:'d9af72','travel-x':'224','travel-y':'24',wander:'4',lifetime:'240'},
      insects:{color:'ffd780','travel-y':'0',wander:'24',lifetime:'360'},
      scarabs:{color:'916c3f','travel-y':'0','travel-x':'96',wander:'16','size-min':'1','size-max':'2'},
      sparks:{color:'ffbd55','color-end':'c12d15','travel-y':'-160',wander:'8',lifetime:'180'}};
    const next=new Map(records),id=newEmitterId(name);
    next.set(id,{x:String(Math.round(x)),y:String(Math.round(y)),width:'96',height:'64',
      color:name.endsWith('mist')||['drips','waterfall-spray','water-surface'].includes(name)?'91bedf':
        ['motes','particle-area'].includes(name)?'ffe6bb':'dde6ff',
      ...(name==='particle-area'?{particles:'4',width:'512',height:'512','travel-y':'-96'}:name==='exposure'?{intensity:'.35',width:'512',height:'512'}:{}),
      ...(preset?{...(preset==='halo'?{}:{pattern:preset}),...presets[preset]}:{})});
    return change(encode(next),'add emitter');
  }
  $('#emitterAdd').onclick=()=>placeEmitter($('#emitterPreset').value,nativeCamera.x+128,nativeCamera.y+112);
  function editEmitter(id,patch,label='edit emitter') {
    if(!records.has(id)||!id.startsWith(emitterPrefix()))return false;
    const next=new Map(records),record={...next.get(id)};
    for(const [key,value] of Object.entries(patch)) {
      if(value===null)delete record[key];else record[key]=String(value);
    }
    next.set(id,record);return change(encode(next),label);
  }
  $('#emitterPoints').onchange=e=>editEmitter(selected,{points:e.target.value});
  for(const [id,key] of fieldFields)$('#'+id).onchange=e=>editEmitter(selected,{[key]:e.target.value});
  for(const [id,key] of moteFields)$('#'+id).onchange=e=>
    editEmitter(selected,{[key]:e.target.value===''?null:e.target.value});
  $('#emitterAutoRise').onclick=()=>editEmitter(selected,{'travel-y':null});
  $('#emitterSeedShuffle').onclick=()=>editEmitter(selected,{seed:crypto.getRandomValues(new Uint32Array(1))[0]},'change mote pattern');
  $('#emitterColorEndEnabled').onchange=e=>editEmitter(selected,{'color-end':e.target.checked?$('#effectColor').value.slice(1):null});
  $('#emitterColorEnd').onchange=e=>editEmitter(selected,{'color-end':e.target.value.slice(1)});
  function emitterPrefix() {
    return ['emitter',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),terrainProfile].join(':')+':';
  }
  function mapEmitters() {
    return [...records].filter(([id])=>id.startsWith(emitterPrefix())).map(([id,r])=>({id,
      kind:id.split(':')[4],x:Number(r.x??0),y:Number(r.y??0),points:r.points??(id.split(':')[4]==='wet-contour'?'-32,0 32,0':''),width:Number(r.width??96),
      height:Number(r.height??64),enabled:r.enabled!=='0'}));
  }
  function selectEmitter(id) {
    if(!records.has(id)||!id.startsWith(emitterPrefix()))return false;
    selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();draw();return true;
  }
  $('#emitterDuplicate').onclick=()=>{
    const original=records.get(selected);if(!original||!selected.startsWith('emitter:'))return;
    const next=new Map(records),id=newEmitterId(selected.split(':')[4]);
    next.set(id,{...original,x:String(Math.min(16384,Number(original.x??0)+16))});
    change(encode(next),'duplicate emitter');
  };
  $('#effectReset').onclick=()=>{const next=new Map(records);next.delete(selected);change(encode(next),'reset environmental effect');};
  $('#effectLoad').onchange=async e=>{
    const file=e.target.files[0];if(!file)return;
    try{
      if(file.size>131072)throw Error('Effect document exceeds 128 KiB.');
      change(await file.text(),'load effect document');
    }catch(error){status(error.message);}finally{e.target.value='';}
  };
  $('#effectExport').onclick=()=>{
    try {
      SharedRoomPreview.validateEffects(documentText);
      const url=URL.createObjectURL(new Blob([documentText],{type:'text/plain'}));
      const link=document.createElement('a');link.href=url;link.download='action-effects.ini';link.click();
      setTimeout(()=>URL.revokeObjectURL(url),1000);saved=documentText;
      status('Download requested. Place action-effects.ini beside settings.ini and restart the game.');
    }catch(error){status(error.message);}
  };
  $('#effectDocumentOpen').onclick=()=>{
    $('#effectDocumentText').value=documentText;$('#effectDocument').showModal();
  };
  $('#effectDocumentClose').onclick=()=>$('#effectDocument').close();
  $('#effectDocumentApply').onclick=()=>change($('#effectDocumentText').value,'apply effects document');
  $('#effectDocumentCopy').onclick=async()=>{
    const field=$('#effectDocumentText');field.focus();field.select();
    try{await navigator.clipboard.writeText(field.value);saved=field.value;status('Copied effects INI. Save it as action-effects.ini beside settings.ini.');}
    catch{status('Text selected. Press Ctrl/Cmd-C to copy.');}
  };
  function updateSources(api) {
    const bytes=new Uint8Array(api.memory.buffer), decoder=new TextDecoder();
    const found=[];
    for(let i=0;i<api.RoomPreview_EffectCount();i++) {
      const kind=api.RoomPreview_SourceValue(i,1), p=api.RoomPreview_KindName(kind);if(!p)continue;
      let end=p;while(bytes[end])end++;
      const name=decoder.decode(bytes.subarray(p,end)),generation=api.RoomPreview_SourceValue(i,0)>>>0;
      const id=[api.RoomPreview_SourceValue(i,5)?'emitter':'source',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),
        terrainProfile,name,generation.toString(16).padStart(8,'0')].join(':');
      found.push({id,name,emitter:!!api.RoomPreview_SourceValue(i,5),reach:!!api.RoomPreview_ReachSupported(kind),
        actor:!!api.RoomPreview_SourceValue(i,7),x:api.RoomPreview_SourceValue(i,2)|0,y:api.RoomPreview_SourceValue(i,3)|0,support:api.RoomPreview_SourceValue(i,6)});
    }
    rebuildSources(found);
  }
  function rebuildSources(found) {
    sourceScope=sceneKey(room);
    const prefix=['emitter',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),terrainProfile].join(':')+':';
    for(const [id,record] of records)if(id.startsWith(prefix)&&!found.some(s=>s.id===id))
      found.push({id,name:id.split(':')[4],emitter:true,reach:false,x:Number(record.x??0),y:Number(record.y??0)});
    const nativePrefix=prefix.replace(/^emitter:/,'source:');
    for(const [id] of records)if(id.startsWith(nativePrefix)&&!found.some(s=>s.id===id))
      found.push({id,name:id.split(':')[4],emitter:false,actor:id.endsWith(':00000000'),reach:id.split(':')[4]==='wall-torch',x:0,y:0});
    sources=found;
    const key=found.map(s=>`${s.id}:${s.x}:${s.y}:${s.support}`).join('|');
    if(key===sourceKey){$('#effectSource').value=selected;return;}sourceKey=key;
    const select=$('#effectSource');select.replaceChildren();
    for(const source of sources){const option=document.createElement('option');option.value=source.id;
      option.textContent=`${source.name} · ${source.x}, ${source.y}`;select.append(option);}
    if(!found.some(s=>s.id===selected))selected=found[0]?.id??'';
    select.value=selected;refreshInspector();
  }
  function paintFloorRect(x0,y0,x1,y1,erase) {
    const left=Math.min(x0,x1)*16, top=Math.min(y0,y1)*16;
    const width=(Math.abs(x1-x0)+1)*16,height=(Math.abs(y1-y0)+1)*16;
    if(!erase&&(width>512||height>512)){status('Mist areas are limited to 512 × 512 pixels. Paint several smaller areas.');return;}
    const next=new Map(records);
    if(erase) {
      const prefix=['emitter',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),terrainProfile,'ground-mist'].join(':')+':';
      for(const [id,r] of next)if(id.startsWith(prefix)) {
        const x=Number(r.x??0),y=Number(r.y??0),w=Number(r.width??96),h=Number(r.height??64);
        if(x+w/2>left&&x-w/2<left+width&&y+h/2>top&&y-h/2<top+height)next.delete(id);
      }
    } else {
      const id=newEmitterId('ground-mist');
      next.set(id,{x:String(left+width/2),y:String(top+height/2),width:String(width),height:String(height),
        'mist-height':'26',color:'91bedf'});
    }
    change(encode(next),erase?'erase ground mist areas':'paint ground mist');
  }
  function paintParticleRect(x0,y0,x1,y1) {
    const left=Math.min(x0,x1)*16,top=Math.min(y0,y1)*16;
    const width=(Math.abs(x1-x0)+1)*16,height=(Math.abs(y1-y0)+1)*16;
    if(width>16384||height>16384){status('Particle regions are limited to 16384 × 16384 pixels.');return false;}
    const next=new Map(records),id=newEmitterId('particle-area');
    next.set(id,{x:String(left+width/2),y:String(top+height/2),width:String(width),height:String(height),
      particles:'4',pattern:'motes',color:'ffe6bb','travel-y':'-96'});
    return change(encode(next),'draw particle region');
  }
  function paintContour(points) {
    if(points.length<2)return false;
    const xs=points.map(p=>p.x),ys=points.map(p=>p.y),left=Math.min(...xs),top=Math.min(...ys);
    const width=Math.max(4,Math.max(...xs)-left+4),height=Math.max(4,Math.max(...ys)-top+4);
    if(width>512||height>512){status('Split a long wet contour into strokes of at most 512px.');return false;}
    const x=Math.round(left+(width-4)/2),y=Math.round(top+(height-4)/2),next=new Map(records),id=newEmitterId('wet-contour');
    next.set(id,{x:String(x),y:String(y),width:String(width),height:String(height),color:'91bedf',placement:'playfield',
      points:points.map(p=>`${Math.round(p.x-x)},${Math.round(p.y-y)}`).join(' ')});
    return change(encode(next),'trace wet contour');
  }
  $('#contourDraw').onclick=()=>{
    finishFramingDrag();setMode('2d');setLayer(0);$('#bSelect').onclick();
    brush='contour';cvs.style.cursor='crosshair';$('#effectGuides').checked=true;
    refreshEditorFeedback();status('Trace a visible edge. One stroke creates one contour; Esc cancels.');draw();
  };
  $('#particleAreaDraw').onclick=()=>{
    finishFramingDrag();setMode('2d');setLayer(0);$('#bSelect').onclick();
    brush='particleArea';cvs.style.cursor='crosshair';$('#effectGuides').checked=true;
    refreshEditorFeedback();status('Drag a large region. One record emits a stable particle field across it.');draw();
  };
  for(const [id,tool] of [['floorMistDraw','floorMist'],['floorMistErase','floorErase']])$('#'+id).onclick=()=>{
    try {SharedRoomPreview.collisionGrid();}catch(error){status(error.message);return;}
    finishFramingDrag();setMode('2d');setLayer(0);$('#bSelect').onclick();
    brush=tool;cvs.style.cursor='crosshair';$('#effectCollision').checked=true;
    refreshEditorFeedback();status(tool==='floorMist'?'Drag an area around the floors to cover with mist.':'Drag across authored mist areas to remove them.');draw();
  };
  $('#effectCollision').onchange=()=>{if($('#effectCollision').checked)setMode('2d');draw();};
  $('#emitterPreview').onclick=()=>{
    const r=records.get(selected);if(!r)return;
    setNativeCamera('x',Math.max(0,Number(r.x??0)-128));setNativeCamera('y',Math.max(0,Number(r.y??0)-112));setMode('shared');
  };
  function drawMapOverlay(ctx,view,viewport) {
    if(sourceScope!==sceneKey(room))rebuildSources([]);
    if(bgIndex!==0)return;
    const size=16*view.scale;
    if($('#effectCollision').checked) {
      try {
        const grid=SharedRoomPreview.collisionGrid();
        const left=Math.max(0,Math.floor(-view.x/size)),right=Math.min(grid.width,Math.ceil((viewport.width-view.x)/size));
        const top=Math.max(0,Math.floor(-view.y/size)),bottom=Math.min(grid.height,Math.ceil((viewport.height-view.y)/size));
        for(let y=top;y<bottom;y++)for(let x=left;x<right;x++) {
          const cell=grid.cells[y*grid.width+x];if(!cell)continue;
          ctx.fillStyle=(cell&15)===15?'rgba(64,200,100,.22)':'rgba(255,174,55,.32)';
          ctx.fillRect(view.x+x*size,view.y+y*size,size,size);
          if(cell&16){ctx.fillStyle='#63efff';ctx.fillRect(view.x+x*size,view.y+y*size,size,Math.max(1,view.scale));}
        }
      }catch(error){status(error.message);}
    }
    EmitterMapTools.draw(ctx,view,viewport);
    if(drag?.contour) {
      ctx.strokeStyle='#85c6ef';ctx.beginPath();
      drag.points.forEach((p,i)=>ctx[i?'lineTo':'moveTo'](view.x+p.x*view.scale,view.y+p.y*view.scale));ctx.stroke();
    }
    if(drag?.floorMist||drag?.particleArea) {
      ctx.strokeStyle=drag.erase?'#ff857e':'#fff19b';
      ctx.strokeRect(view.x+Math.min(drag.x0,drag.x1)*size,view.y+Math.min(drag.y0,drag.y1)*size,
        (Math.abs(drag.x1-drag.x0)+1)*size,(Math.abs(drag.y1-drag.y0)+1)*size);
    }
  }
  refreshInspector();
  return {placeEmitter,editEmitter,mapEmitters,paintContour,selectEmitter,selected:()=>selected,paintFloorRect,paintParticleRect,drawMapOverlay,text:()=>documentText,restore,updateSources,dirty:()=>documentText!==saved};
})();
