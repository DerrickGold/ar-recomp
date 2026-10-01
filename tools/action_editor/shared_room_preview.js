/* UI/transport only. Whole-room sampling, animation, edits and compositing live
 * in C. Asset packets are built from the embedded level catalogue automatically;
 * no user-generated capture is part of this workflow. */
const SharedRoomPreview = (() => {
  const canvas=document.querySelector('#sharedGl'), label=document.querySelector('#sharedRoomStatus');
  let backend=null, api=null, key='', configDirty=true, config='', lost=false, skyboxKey=0;
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
  function render() {
    if(!api||lost)return;
    try {
      const next=sceneKey(room);
      if(key!==next) {
        send(SharedActionPreview.encode(DATA,BLOBS,room),api.RoomPreview_Load);
        key=next;config='';configDirty=true;skyboxKey=0;
      }
      if(configDirty) {
        const text=roomIni();
        if(config!==text) {send(new TextEncoder().encode(text),api.RoomPreview_Configure);config=text;}
        const source=api.RoomPreview_SkyboxRoom();
        if(source&&source!==skyboxKey) {
          const sky=DATA.rooms.find(r=>r.group===(source>>>16)&&r.map===((source>>>8)&255));
          if(!sky)throw Error('Named skybox room is missing from the catalogue.');
          send(SharedActionPreview.encode(DATA,BLOBS,sky),api.RoomPreview_LoadSkybox);
        }
        skyboxKey=source;configDirty=false;
      }
      const w=960,h=Math.round(w/setting('Aspect'));
      if(canvas.width!==w)canvas.width=w;if(canvas.height!==h)canvas.height=h;
      backend.stats.creates=backend.stats.draws=0;
      const start=performance.now();
      const ok=api.RoomPreview_Render(nativeCamera.x,nativeCamera.y,nativeFrame,
        Math.max(0,Math.min(128,Math.floor(setting('Extra')))),setting('Vertical'),w,h,
        setting('Distance'),setting('Yaw'),setting('Pitch'),setting('Sky'),setting('Pixels'));
      const error=backend.gl.getError();
      if(!ok||error)throw Error(`Room rendering failed (${error}).`);
      label.textContent=`Shared C · ${api.RoomPreview_Width()} × ${api.RoomPreview_Height()} room · `+
        `${backend.stats.draws} draws · ${api.RoomPreview_Uploads()} uploads · ${backend.stats.creates} allocations · `+
        `${(performance.now()-start).toFixed(1)} ms CPU`;
      $('#hud').textContent=`Camera ${nativeCamera.x}, ${nativeCamera.y} · frame ${nativeFrame}. `+
        'Drag to pan; arrows move. Live scenery preview; environmental effects and actors are still pending.';
    } catch(error) { label.textContent=error.message; }
  }
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
    event.preventDefault();lost=true;label.textContent='Graphics context lost. Reload to restore the renderer.';
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
      if(api.DioramaPreview_Version()!==1||!api.DioramaPreview_Init(backend.gl.getParameter(backend.gl.MAX_TEXTURE_SIZE)))
        throw Error('Unsupported renderer version or device.');
      $('#modeShared').disabled=false;label.textContent='Ready. Select Shared renderer to explore the level.';
    }catch(error){label.textContent=error.message;api=null;backend?.dispose();}
  }
  initialize();
  return {draw:render,invalidate:()=>{configDirty=true;}};
})();
