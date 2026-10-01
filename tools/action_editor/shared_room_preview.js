/* UI/transport only. Whole-room sampling, animation, edits and compositing live
 * in C. Asset packets are built from the embedded level catalogue automatically;
 * no user-generated capture is part of this workflow. */
const SharedRoomPreview = (() => {
  const canvas=document.querySelector('#sharedGl'), label=document.querySelector('#sharedRoomStatus');
  let backend=null, api=null, key='', configDirty=true, config='', lost=false, skyboxKey=0, effectsConfig=null, collision=null, eventConfig=null;
  const setting=id=>Number(document.querySelector('#shared'+id).value);
  function send(bytes,call) {
    if(bytes.length>api.DioramaPreview_Capacity())throw Error('Room data exceeds renderer capacity.');
    new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),bytes.length).set(bytes);
    if(!call(bytes.length))throw Error('Shared renderer rejected the room or its edits.');
  }
  function roomIni() {
    // Base is the current in-memory edit. Keep camera-local sections from the
    // original document; they do not replace the edited base section.
    const scoped=[];let keep=false;
    for(const line of sourceIniText.split(/\r?\n/)) {
      if(/^\s*\[/.test(line)) {
        const m=stripIniComment(line).match(/^\[layers:([\da-f]+):([\da-f]+):[^\]]+\]$/i);
        keep=!!m&&parseInt(m[1],16)===room.group&&parseInt(m[2],16)===room.map;
      }
      if(keep)scoped.push(line);
    }
    return roomSectionIni(room)+'\n'+scoped.join('\n');
  }
  function ensureRoom() {
    if(!api||lost)throw Error('Shared renderer is still loading or unavailable.');
      const next=sceneKey(room);
      if(key!==next) {
        send(SharedActionPreview.encode(DATA,BLOBS,room),api.RoomPreview_Load);
        key=next;collision=null;eventConfig=null;config='';configDirty=true;skyboxKey=0;effectsConfig=null;
      }
      if(configDirty) {
        const text=roomIni();
        if(config!==text) {send(new TextEncoder().encode(text),api.RoomPreview_Configure);config=text;}
        configDirty=false;
      }
  }
  function collisionGrid() {
    ensureRoom();
    if(!collision) {
      const count=api.RoomPreview_CollisionGrid();
      if(!count)throw Error('No collision map is available for this room.');
      collision={width:api.RoomPreview_Width()/16,height:api.RoomPreview_Height()/16,
        cells:new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),count).slice()};
    }
    return collision;
  }
  function render() {
    if(!api||lost)return;
    try {
      ensureRoom();
      if(effectsConfig!==EffectEditor.text())validateEffects(EffectEditor.text());
      const eventValues=['previewEvent','previewEventX','previewEventY','previewEventVX','previewEventVY','previewEventStart','previewEventDuration','previewEventSeed'].map(id=>Number($('#'+id).value));
      const nextEvent=eventValues.join(':');
      if(eventConfig!==nextEvent) {
        if(!api.RoomPreview_SetEvent(...eventValues))throw Error('Invalid event preview parameters.');
        eventConfig=nextEvent;
      }
      if(!api.RoomPreview_SetReceivers($('#previewReceivers').checked?1:0,...['previewPlayerX','previewPlayerY','previewEnemyX','previewEnemyY'].map(id=>Number($('#'+id).value))))throw Error('Invalid receiver probe positions.');
      const w=960,h=Math.round(w/setting('Aspect'));
      if(canvas.width!==w)canvas.width=w;if(canvas.height!==h)canvas.height=h;
      backend.stats.creates=backend.stats.draws=0;
      const start=performance.now();
      api.RoomPreview_EnableEffects($('#sharedEffects').checked ? 1 : 0);
      const renderFrame=()=>api.RoomPreview_Render(nativeCamera.x,nativeCamera.y,nativeFrame,
        Math.max(0,Math.min(128,Math.floor(setting('Extra')))),setting('Vertical'),w,h,
        setting('Distance'),setting('Yaw'),setting('Pitch'),setting('Sky'),setting('Pixels'));
      let ok=renderFrame();
      let uploads=api.RoomPreview_Uploads();
      // Resolve after the room frame selects camera-local sections (Aitos).
      // A changed named backdrop needs one redraw; cached surfaces are reused.
      const source=api.RoomPreview_SkyboxRoom();
      if(source&&source!==skyboxKey) {
        const sky=DATA.rooms.find(r=>r.group===(source>>>16)&&r.map===((source>>>8)&255));
        if(!sky)throw Error('Named skybox room is missing from the catalogue.');
        send(SharedActionPreview.encode(DATA,BLOBS,sky),api.RoomPreview_LoadSkybox);
        ok=renderFrame();
        uploads+=api.RoomPreview_Uploads();
      }
      skyboxKey=source;
      const error=backend.gl.getError();
      if(!ok||error)throw Error(`Room rendering failed (${error}).`);
      EffectEditor.updateSources(api);
      label.textContent=`Shared C · ${api.RoomPreview_Width()} × ${api.RoomPreview_Height()} room · `+
        `${api.RoomPreview_EffectCount()} sources · ${api.RoomPreview_EffectVertices()} effect vertices · ${backend.stats.draws} draws · ${uploads} uploads · ${backend.stats.creates} allocations · `+
        `${(performance.now()-start).toFixed(1)} ms CPU`;
      $('#hud').textContent=`Camera ${nativeCamera.x}, ${nativeCamera.y} · frame ${nativeFrame}. `+
        'Drag to pan; arrows move. Shared scenery/effects; optional reference probes and clock-based actor accents.';
    } catch(error) { label.textContent=error.message; }
  }
  $('#previewProbesHere').onclick=()=>{
    $('#previewPlayerX').value=nativeCamera.x+112;$('#previewEnemyX').value=nativeCamera.x+176;
    $('#previewPlayerY').value=$('#previewEnemyY').value=nativeCamera.y+192;
    $('#previewReceivers').checked=true;setMode('shared');draw();
  };
  $('#previewEventHere').onclick=()=>{
    $('#previewEventX').value=nativeCamera.x+128;$('#previewEventY').value=nativeCamera.y+160;
    $('#previewEventStart').value=nativeFrame;setMode('shared');draw();
  };
  document.querySelector('#sharedControls').addEventListener('input',()=>draw());
  for(const event of ['input','change','click'])document.addEventListener(event,e=>{
    const id=e.target.id;
    if(e.target.closest('#sharedControls')||id.startsWith('native')||id==='sharedGl')return;
    configDirty=true;
  });
  document.addEventListener('keydown',e=>{if(e.ctrlKey||e.metaKey)configDirty=true;});
  let drag=null;
  canvas.addEventListener('pointerdown',e=>{
    canvas.focus();canvas.setPointerCapture(e.pointerId);
    drag={x:e.clientX,y:e.clientY,cx:nativeCamera.x,cy:nativeCamera.y};
  });
  canvas.addEventListener('pointermove',e=>{
    if(!drag)return;
    const scale=224/Math.min(canvas.clientHeight,canvas.clientWidth/setting('Aspect'));
    setNativeCamera('x',Math.round(drag.cx-(e.clientX-drag.x)*scale));
    setNativeCamera('y',Math.round(drag.cy-(e.clientY-drag.y)*scale));
  });
  for(const name of ['pointerup','pointercancel','lostpointercapture'])canvas.addEventListener(name,()=>{drag=null;});
  canvas.addEventListener('webglcontextlost',event=>{
    event.preventDefault();lost=true;label.textContent='Graphics context lost. Effects and map edits are retained while the renderer recovers.';
  });
  canvas.addEventListener('webglcontextrestored',()=>{
    // Context loss invalidates every old GL object; recreate the backend and
    // WASM handles together, then replay the current documents and camera.
    api=null;backend=null;key='';config='';effectsConfig=null;configDirty=true;skyboxKey=0;lost=false;
    initialize();
  });
  async function initialize() {
    if(!window.__ROOM_PREVIEW_WASM__) {label.textContent='Shared renderer was not bundled in this build.';return;}
    try {
      backend=new ActionWebGL2Backend(canvas,window.__ROOM_PREVIEW_SHADERS__);
      const module=await WebAssembly.compile(B64(window.__ROOM_PREVIEW_WASM__));
      const wasi={fd_close:()=>0,fd_seek:()=>0,fd_write:()=>0,
        proc_exit:code=>{throw Error(`Renderer exited (${code})`);}};
      for(const item of WebAssembly.Module.imports(module))
        if(!(item.module==='ar'&&Object.hasOwn(backend.imports,item.name))&&
            !(item.module==='wasi_snapshot_preview1'&&Object.hasOwn(wasi,item.name)))
          throw Error(`Unexpected renderer import: ${item.module}.${item.name}`);
      const instance=await WebAssembly.instantiate(module,{ar:backend.imports,wasi_snapshot_preview1:wasi});
      api=instance.exports;backend.memory=api.memory;api._initialize?.();
      const select=$('#previewEvent');select.replaceChildren();
      const none=document.createElement('option');none.value='0';none.textContent='None';select.append(none);
      const bytes=new Uint8Array(api.memory.buffer),decoder=new TextDecoder();
      for(let i=0;i<api.RoomPreview_EventCount();i++) {
        const p=api.RoomPreview_KindName(api.RoomPreview_EventKind(i));let end=p;while(bytes[end])end++;
        const option=document.createElement('option');option.value=String(i+1);option.textContent=decoder.decode(bytes.subarray(p,end));select.append(option);
      }
      if(api.DioramaPreview_Version()!==1||!api.DioramaPreview_Init(backend.gl.getParameter(backend.gl.MAX_TEXTURE_SIZE)))
        throw Error('Unsupported renderer version or device.');
      $('#modeShared').disabled=false;label.textContent='Ready. Select Shared renderer to explore the level.';
      draw();
    }catch(error){label.textContent=error.message;api=null;backend?.dispose();}
  }
  function validateEffects(text) {
    ensureRoom();
    const bytes=new TextEncoder().encode(text);
    if(bytes.length>131072)throw Error('Effect document exceeds 128 KiB.');
    new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),bytes.length).set(bytes);
    if(!api.RoomPreview_ConfigureEffects(bytes.length))
      throw Error(`Effects rejected at line ${api.RoomPreview_RecipeErrorLine()}. Previous effects retained.`);
    effectsConfig=text;
  }
  initialize();
  return {validateEffects,collisionGrid,draw:render,invalidate:()=>{configDirty=true;}};
})();
