/* UI/transport only. Whole-room sampling, animation, edits and compositing live
 * in C. Scenery comes from the embedded level catalogue; optional finite native
 * object captures supply exact sprite winners, priority and modeled props. */
function sharedPreviewOutputSize(choice,customWidth,customHeight,areaWidth,areaHeight) {
  let aspect;
  if(choice==='auto'||choice==='stretch') {
    aspect=areaWidth>0&&areaHeight>0?areaWidth/areaHeight:16/10;
  } else if(choice==='custom') {
    if(![customWidth,customHeight].every(n=>Number.isInteger(n)&&n>=64&&n<=16384))
      throw Error('Enter a monitor width and height between 64 and 16384 pixels.');
    aspect=customWidth/customHeight;
  } else aspect=Number(choice);
  if(!Number.isFinite(aspect)||aspect<=0)throw Error('Choose a valid preview aspect ratio.');
  // Monitor dimensions describe the shape, not a full-resolution render target.
  // Bound both axes so portrait/Auto cannot exceed the shared renderer's budget.
  return aspect>=1?{width:960,height:Math.max(1,Math.round(960/aspect))}:
    {width:Math.max(1,Math.round(960*aspect)),height:960};
}
function sharedPreviewCanvasPoint(canvas,event) {
  // The canvas uses object-fit:contain. Its DOM box includes letterboxing;
  // clicks must address the fitted image, not stretch across those margins.
  const rect=canvas.getBoundingClientRect();
  const scale=Math.min(rect.width/canvas.width,rect.height/canvas.height);
  if(!(scale>0))return null;
  const x=(event.clientX-rect.left-(rect.width-canvas.width*scale)/2)/scale;
  const y=(event.clientY-rect.top-(rect.height-canvas.height*scale)/2)/scale;
  return x>=0&&y>=0&&x<canvas.width&&y<canvas.height?{x,y}:null;
}
const SharedRoomPreview = (() => {
  const canvas=document.querySelector('#sharedGl'), label=document.querySelector('#sharedRoomStatus');
  // Routine renderer metrics are optional; failures must stay visible even
  // when the scene-details display is closed.
  function previewStatus(message,error=false) {
    label.textContent=message;label.classList.toggle('error',error);
    label.setAttribute('role',error?'alert':'status');
  }
  let backend=null, api=null, key='', configDirty=true, config='', lost=false, skyboxKey=0, effectsConfig=null, collision=null, eventConfig=null;
  let placingPlayer=false;
  let nativeObjects=null;
  let inspection=0;
  function objectControls(info=null) {
    nativeObjects=info;
    $('#sharedResetObjects').disabled=!info;
    $('#sharedClearObjects').disabled=!info;
    $('#sharedPlacePlayer').disabled=!!info;
    $('#sharedPlayerSettings').disabled=!!info;
    $('#sharedObjectStatus').textContent=info
      ? `Native frame ${info[3]} · ${info[11]} actors · ${info[10]} modeled pieces · finite capture`
      : '';
  }
  const measures=new Map();
  let configVersion=0,configuredRevision=-1,configuredMode=-1;
  const setting=id=>Number(document.querySelector('#shared'+id).value);
  const savedView=window.__ACTION_VIEW__||{};
  $('#sharedAspect').value=({'4:3':String(4/3),'16:9':String(16/9),'16:10':'1.6',Auto:'auto',Stretch:'stretch'})[savedView.aspect]||'1.6';
  $('#sharedPixels').value=savedView.pixelAspect==='crt'?'1':'0';
  $('#sharedVertical').value=String(savedView.vertical??64);
  $('#sharedSky').value=String(savedView.skybox??1);
  $('#sharedBackdrop').checked=savedView.showBackdrop??false;
  function updateBackdropControl() {
    $('#sharedBackdrop').disabled=setting('Sky')===1;
  }
  updateBackdropControl();
  $('#sharedSky').addEventListener('input',updateBackdropControl);
  $('#sharedEffects').checked=savedView.environmentalEffects??true;
  function viewArguments(choice=$('#sharedAspect').value) {
    const auto=choice==='auto'||choice==='stretch';
    // Use the editor window, not the remaining canvas area: opening tools or
    // switching modes must not change the simulated monitor/camera limits.
    const {width,height}=sharedPreviewOutputSize(choice,setting('MonitorWidth'),setting('MonitorHeight'),
      auto?window.innerWidth:0,auto?window.innerHeight:0);
    const aspectMode=choice===String(4/3)?0:choice==='stretch'?3:
      choice==='auto'||choice==='custom'||Number(choice)>16/9?2:1;
    if(!api.RoomPreview_SetView(framingDistance,savedView.tiltY||0,savedView.tiltX||0,aspectMode,
        savedView.cameraMode==='free'?0:1))
      throw Error('Invalid game camera baseline.');
    return [nativeCamera.x,nativeCamera.y,nativeFrame,
      Math.max(0,Math.min(128,Math.floor(setting('Extra')))),setting('Vertical'),width,height,
      setting('Distance'),setting('Yaw'),setting('Pitch'),setting('Sky'),setting('Pixels')];
  }
  function measure(bg=0,choice) {
    if(!api||lost)return null;
    try {
      ensureRoom();
      const args=viewArguments(choice),signature=[key,configVersion,framingDistance,...args,bg].join('|');
      if(measures.has(signature))return measures.get(signature);
      const count=api.RoomPreview_Measure(...args,bg);
      if(count!==16)return null;
      const a=new Float32Array(api.memory.buffer,api.DioramaPreview_Input(),count);
      const result={x:a[0],y:a[1],minX:a[2],maxX:a[3],maxY:a[4],
        rect:{x:a[5],y:a[6],w:a[7]-a[5],h:a[8]-a[6]},fill:a[9],
        visibleWidth:a[10],visibleHeight:a[11],viewport:{x:a[12],y:a[13],w:a[14],h:a[15]}};
      if(measures.size>=16)measures.clear();
      measures.set(signature,result);return result;
    } catch(error) {previewStatus(error.message,true);return null;}
  }
  function sceneryError() {
    const address=api?.RoomPreview_SceneryError?.();
    if(!address)return '';
    const bytes=new Uint8Array(api.memory.buffer);let end=address;
    while(end<bytes.length&&end-address<2048&&bytes[end])++end;
    return new TextDecoder().decode(bytes.subarray(address,end));
  }
  function send(bytes,call) {
    if(bytes.length>api.DioramaPreview_Capacity())throw Error('Room data exceeds renderer capacity.');
    new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),bytes.length).set(bytes);
    if(!call(bytes.length))throw Error(call===api.RoomPreview_Configure&&sceneryError()||'Shared renderer rejected the room or its edits.');
  }
  function roomIni() {
    // Base is the current in-memory edit. Keep camera-local sections from the
    // original document; they do not replace the edited base section.
    const scoped=[];let keep=false;
    for(const line of sourceIniText.split(/\r?\n/)) {
      if(/^\s*\[/.test(line)) {
        const m=stripIniComment(line).match(/^\[layers:([\da-f]+):([\da-f]+):[^\]]+\]$/i);
        keep=!!m&&!roomFromSection(stripIniComment(line))&&!room.section&&
          parseInt(m[1],16)===room.group&&parseInt(m[2],16)===room.map;
      }
      if(keep)scoped.push(line);
    }
    const section=roomSectionIni(room);
    return (typeof VoxelEditor==='undefined'?section:VoxelEditor.previewSection(room,section))+'\n'+scoped.join('\n');
  }
  function ensureRoom() {
    if(!api||lost)throw Error('Shared renderer is still loading or unavailable.');
      const revision=typeof editorRevision==='number'?editorRevision:0;
      if(revision!==configuredRevision){configDirty=true;configuredRevision=revision;}
      const next=sceneKey(room);
      if(key!==next) {
        objectControls();
        send(SharedActionPreview.encode(DATA,BLOBS,room),api.RoomPreview_Load);
        key=next;collision=null;eventConfig=null;config='';configDirty=true;skyboxKey=0;effectsConfig=null;configuredMode=-1;
      }
      configureMode();
      if(configDirty) {
        const text=roomIni();
        if(config!==text) {
          send(new TextEncoder().encode(text),api.RoomPreview_Configure);config=text;
          ++configVersion;measures.clear();
        }
        configDirty=false;
      }
  }
  function configureMode() {
    const selected=typeof VoxelEditor==='undefined'?2:VoxelEditor.mode();
    if(api.RoomPreview_Set3DMode&&!api.RoomPreview_Set3DMode(selected))throw Error('Invalid shared preview 3D mode.');
    if(api.RoomPreview_SetInspection&&!api.RoomPreview_SetInspection(selected===2?inspection:0))throw Error('Invalid preview inspection mode.');
    $('#sharedInspection').disabled=selected!==2;
    if(configuredMode!==selected){configuredMode=selected;++configVersion;measures.clear();}
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
      const receiverMask=nativeObjects?0:(($('#previewReceivers').checked?1:0)|($('#previewEnemyEnabled').checked?2:0));
      if(!api.RoomPreview_SetReceivers(receiverMask,...['previewPlayerX','previewPlayerY','previewEnemyX','previewEnemyY'].map(id=>Number($('#'+id).value)))||
          !api.RoomPreview_ReceiverStyle(Number($('#previewFacing').value),$('#previewRim').checked?1:0))throw Error('Invalid receiver preview settings.');
      const args=viewArguments(),w=args[5],h=args[6];
      if(canvas.width!==w)canvas.width=w;if(canvas.height!==h)canvas.height=h;
      const outputLabel=`Preview ${w} × ${h}`;
      if($('#sharedOutputSize').textContent!==outputLabel)$('#sharedOutputSize').textContent=outputLabel;
      backend.stats.creates=backend.stats.draws=0;
      const start=performance.now();
      api.RoomPreview_EnableEffects($('#sharedEffects').checked ? 1 : 0);
      api.RoomPreview_ShowBackdrop($('#sharedBackdrop').checked ? 1 : 0);
      const renderFrame=()=>api.RoomPreview_Render(...args);
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
      if(!ok&&nativeObjects&&!error)throw Error('Native object view exceeds its captured area or does not match this room section. Choose Captured camera, reduce the orbit, or clear native objects.');
      if(!ok||error)throw Error(`Room rendering failed (${error}). ${backend.warnings.join(' ')}`.trim());
      // Use the capture's stopped camera for UI, picking and subsequent drags.
      // Keeping the requested position would accumulate hidden overscroll.
      refreshNativeCameraControls();
      EffectEditor.updateCatalogue(api);EffectEditor.updateSources(api);
      previewStatus(`Shared C · ${api.RoomPreview_Width()} × ${api.RoomPreview_Height()} room · `+
        `${api.RoomPreview_EffectCount()} sources · ${api.RoomPreview_EffectVertices()} effect vertices · ${backend.stats.draws} draws · ${uploads} uploads · ${backend.stats.creates} allocations · `+
        `${(performance.now()-start).toFixed(1)} ms CPU`);
      $('#hud').textContent=`Camera ${nativeCamera.x}, ${nativeCamera.y} · frame ${nativeFrame}. `+
        (nativeObjects?'Captured native object poses. Pan within the captured area; orbit to inspect priority and joins.':
          placingPlayer?'Click to place the player’s feet. Esc cancels.':
          'Drag to pan; arrows move. Shift-click/drag places the player; Alt+Shift places the comparison sprite.');
      if(typeof VoxelEditor!=='undefined') {
        VoxelEditor.afterRender();
        if(VoxelEditor.editing())$('#hud').textContent='Voxel touch-up · Click a face; Shift-click adds selection. Alt-drag pans. Preview → Apply → Save project. Native collision and sprite priority are preserved.';
      }
      return true;
    } catch(error) { previewStatus(error.message,true);return false; }
  }
  $('#previewProbesHere').onclick=()=>{
    $('#previewPlayerX').value=nativeCamera.x+112;$('#previewEnemyX').value=nativeCamera.x+176;
    $('#previewPlayerY').value=$('#previewEnemyY').value=nativeCamera.y+192;
    $('#previewReceivers').checked=true;setMode('shared');draw();
  };
  for(const id of ['previewReceivers','previewEnemyEnabled','previewPlayerX','previewPlayerY',
      'previewEnemyX','previewEnemyY','previewFacing','previewRim'])$('#'+id).addEventListener('input',()=>draw());
  $('#previewRim').checked=!!window.__ACTION_VIEW__?.rimLight;
  $('#sharedPlayerSettings').onclick=()=>EditorLayout.openSettings('receivers');
  $('#sharedLoadObjects').onclick=()=>$('#sharedObjectFile').click();
  $('#sharedObjectFile').onchange=async event=>{
    const file=event.target.files[0];event.target.value='';if(!file)return;
    if(typeof VoxelEditor!=='undefined'&&!VoxelEditor.guard('loading another object capture'))return;
    try {
      ensureRoom();
      if(file.size>api.DioramaPreview_Capacity())throw Error('Native object capture is too large.');
      const roomAtLoad=sceneKey(room),bytes=new Uint8Array(await file.arrayBuffer());
      if(sceneKey(room)!==roomAtLoad)throw Error('The room changed while loading. Select the file again.');
      if(typeof VoxelEditor!=='undefined'&&!VoxelEditor.guard('loading another object capture'))return;
      api.RoomPreview_SetReceivers(0,0,0,0,0);
      send(bytes,api.RoomPreview_LoadObjects);
      if(api.RoomPreview_ObjectInfo()!==14)throw Error('Native object capture metadata is unavailable.');
      const info=Array.from(new Int32Array(api.memory.buffer,api.DioramaPreview_Input(),14));
      if(typeof VoxelEditor!=='undefined')VoxelEditor.clearLoaded();
      objectControls(info);placementMode(false);
      $('#previewReceivers').checked=false;$('#previewEnemyEnabled').checked=false;
      $('#sharedExtra').value=String(info[12]);
      const vertical=Math.max(0,Math.min(64,Math.floor((info[9]-224)/2)));
      $('#sharedVertical').value=String(vertical>=64?64:vertical>=32?32:0);
      setNativeCamera('x',info[4]);setNativeCamera('y',info[5]);setNativeFrame(info[3]);
      setMode('shared');draw();
    } catch(error) {previewStatus(`Cannot load native objects: ${error.message} Check the selected room and region.`,true);}
  };
  $('#sharedClearObjects').onclick=()=>{
    if(typeof VoxelEditor!=='undefined'&&!VoxelEditor.guard('clearing native objects'))return;
    if(typeof VoxelEditor!=='undefined')VoxelEditor.clearLoaded();
    api?.RoomPreview_ClearObjects();objectControls();draw();
  };
  $('#sharedResetObjects').onclick=()=>{
    if(!nativeObjects)return;
    setNativeCamera('x',nativeObjects[4]);setNativeCamera('y',nativeObjects[5]);
    setNativeFrame(nativeObjects[3]);draw();
  };
  function placementMode(enabled) {
    if(enabled&&typeof VoxelEditor!=='undefined')VoxelEditor.deactivate();
    placingPlayer=enabled;canvas.style.cursor=enabled?'crosshair':'';
    $('#sharedPlacePlayer').setAttribute('aria-pressed',String(enabled));
    $('#sharedPlacePlayer').textContent=enabled?'Click scene…':'Place player';
  }
  $('#sharedPlacePlayer').onclick=()=>{placementMode(!placingPlayer);canvas.focus();draw();};
  function placePlayerAt(x,y,center=false,enemy=false) {
    try {
      if(nativeObjects)throw Error('Clear the native object capture before placing a comparison sprite.');
      ensureRoom();
      const prefix=enemy?'previewEnemy':'previewPlayer';
      $('#'+prefix+'X').value=Math.max(-2048,Math.min(16384,Math.round(x)));
      $('#'+prefix+'Y').value=Math.max(-2048,Math.min(16384,Math.round(y)));
      $(enemy?'#previewEnemyEnabled':'#previewReceivers').checked=true;
      if(center) {
        setNativeCamera('x',Math.max(0,x-128));setNativeCamera('y',Math.max(0,y-160));
        setMode('shared');
      }
      draw();
    }catch(error){previewStatus(error.message,true);}
  }
  function placeAtPointer(event,enemy) {
    const point=sharedPreviewCanvasPoint(canvas,event);
    if(!point||!api||lost||api.RoomPreview_PickReceiver(point.x,point.y)!==2)return false;
    const [x,y]=new Int32Array(api.memory.buffer,api.DioramaPreview_Input(),2);
    if(placingPlayer)placementMode(false);
    placePlayerAt(x,y,false,enemy);
    return true;
  }
  $('#previewEventHere').onclick=()=>{
    $('#previewEventX').value=nativeCamera.x+128;$('#previewEventY').value=nativeCamera.y+160;
    $('#previewEventStart').value=nativeFrame;setMode('shared');draw();
  };
  $('#sharedAspect').addEventListener('input',()=>{
    $('#sharedCustomSize').hidden=$('#sharedAspect').value!=='custom';
  });
  document.querySelector('#sharedControls').addEventListener('input',()=>{
    framingPixelAspect=setting('Pixels')?'crt':'square';
    refreshNativeCameraControls();draw();
  });
  $('#sharedResetView').onclick=()=>{
    $('#sharedDistance').value='1';$('#sharedYaw').value=$('#sharedPitch').value='0';
    refreshNativeCameraControls();draw();
  };
  function setInspection(value) {
    value=Number(value);
    if(!Number.isInteger(value)||value<0||value>2)throw Error('Choose a valid Voxel inspection mode.');
    inspection=value;$('#sharedInspection').value=String(value);draw();
  }
  $('#sharedInspection').onchange=event=>setInspection(event.target.value);
  function setCameraPreset(preset) {
    const yaw={front:0,left:-.3,right:.3}[preset];
    if(yaw===undefined)throw Error('Unknown camera preset.');
    $('#sharedYaw').value=String(Math.max(-.7,Math.min(.7,yaw-(savedView.tiltY||0))));
    $('#sharedPitch').value=String(Math.max(-.7,Math.min(.7,-(savedView.tiltX||0))));
    refreshNativeCameraControls();draw();
  }
  for(const preset of ['front','left','right'])$('#sharedPreset'+preset).onclick=()=>setCameraPreset(preset);
  function focusVoxelSelection(bounds) {
    if(!bounds||![bounds.x,bounds.y,bounds.width,bounds.height].every(Number.isFinite)||bounds.width<0||bounds.height<0)return false;
    const previous={...nativeCamera};
    nativeCamera.x=Math.round(bounds.x+bounds.width/2-128);
    nativeCamera.y=Math.round(bounds.y+bounds.height/2-112);
    refreshNativeCameraControls();
    if(render())return true;
    // A finite sprite capture may not contain the newly centered view. Keep
    // the valid captured pose rather than leave the preview in an error state.
    nativeCamera.x=previous.x;nativeCamera.y=previous.y;refreshNativeCameraControls();render();
    previewStatus('Selection is outside the available captured view. Clear native objects to explore the whole room.');
    return false;
  }
  for(const event of ['input','change','click'])document.addEventListener(event,e=>{
    const id=e.target.id;
    if(e.target.closest('#sharedControls')||id.startsWith('native')||id.startsWith('preview')||id==='sharedGl')return;
    configDirty=true;
  });
  document.addEventListener('keydown',e=>{if(e.ctrlKey||e.metaKey)configDirty=true;});
  let drag=null,capturedPointer=null;
  function endPointerGesture(release=false) {
    const pointer=capturedPointer;capturedPointer=null;drag=null;
    if(typeof VoxelEditor!=='undefined')VoxelEditor.endGesture();
    if(release&&pointer!==null&&canvas.hasPointerCapture?.(pointer))canvas.releasePointerCapture(pointer);
  }
  function capturePointer(event) {
    capturedPointer=event.pointerId;canvas.focus();canvas.setPointerCapture(event.pointerId);
  }
  canvas.addEventListener('pointerdown',e=>{
    if(e.button===2||macControlClick(e))return;
    if(e.button===1&&e.shiftKey) {
      endPointerGesture(true);capturePointer(e);e.preventDefault();
      drag={orbit:true,x:e.clientX,y:e.clientY,yaw:setting('Yaw')||0,pitch:setting('Pitch')||0};return;
    }
    if(typeof VoxelEditor!=='undefined'&&VoxelEditor.pointerDown(e)){capturedPointer=e.pointerId;return;}
    if(e.button!==0&&e.button!==1)return;
    capturePointer(e);e.preventDefault();
    if(e.button===0&&(placingPlayer||e.shiftKey)) {
      drag={receiver:true,enemy:e.altKey};e.preventDefault();
      placeAtPointer(e,drag.enemy);
      return;
    }
    drag={x:e.clientX,y:e.clientY,cx:nativeCamera.x,cy:nativeCamera.y};
  });
  canvas.addEventListener('pointermove',e=>{
    if(drag?.orbit) {
      const clamp=(n,limit)=>Math.max(-limit,Math.min(limit,n));
      drag.yaw=clamp(drag.yaw+clamp(e.clientX-drag.x,60)*.0035,.7);
      drag.pitch=clamp(drag.pitch+clamp(e.clientY-drag.y,60)*.003,.7);
      $('#sharedYaw').value=String(drag.yaw);$('#sharedPitch').value=String(drag.pitch);
      drag.x=e.clientX;drag.y=e.clientY;e.preventDefault();refreshNativeCameraControls();draw();return;
    }
    if(typeof VoxelEditor!=='undefined'&&VoxelEditor.pointerMove(e))return;
    if(!drag)return;
    if(drag.receiver){placeAtPointer(e,drag.enemy);return;}
    const scale=224/Math.min(canvas.clientHeight,canvas.clientWidth/(canvas.width/canvas.height));
    const requestedX=Math.round(drag.cx-(e.clientX-drag.x)*scale);
    setNativeCamera('x',requestedX);
    if(nativeCamera.x!==requestedX){drag.cx=nativeCamera.x;drag.x=e.clientX;}
    setNativeCamera('y',Math.round(drag.cy-(e.clientY-drag.y)*scale));
  });
  for(const name of ['pointerup','pointercancel','lostpointercapture'])canvas.addEventListener(name,event=>{
    if(capturedPointer!==null&&event.pointerId!==undefined&&event.pointerId!==capturedPointer)return;
    endPointerGesture(name!=='lostpointercapture');
  });
  window.addEventListener('blur',()=>endPointerGesture(true));
  canvas.addEventListener('wheel',event=>{
    event.preventDefault();endPointerGesture(true);
    const unit=event.deltaMode===1?16:event.deltaMode===2?canvas.clientHeight||640:1;
    const delta=Math.max(-1000,Math.min(1000,event.deltaY*unit));
    $('#sharedDistance').value=String(Math.max(.5,Math.min(2.5,(setting('Distance')||1)*Math.exp(delta*.0012))));
    refreshNativeCameraControls();draw();
  },{passive:false});
  canvas.addEventListener('auxclick',event=>{if(event.button===1)event.preventDefault();});
  canvas.addEventListener('keydown',e=>{
    if(typeof VoxelEditor!=='undefined'&&VoxelEditor.keyDown(e))return;
    if(e.key==='Escape'&&placingPlayer){placementMode(false);draw();}
  });
  canvas.addEventListener('webglcontextlost',event=>{
    event.preventDefault();endPointerGesture(true);lost=true;previewStatus('Graphics context lost. Effects and map edits are retained while the renderer recovers.',true);
  });
  canvas.addEventListener('webglcontextrestored',()=>{
    // Context loss invalidates every old GL object; recreate the backend and
    // WASM handles together, then replay the current documents and camera.
    api=null;backend=null;key='';config='';effectsConfig=null;configDirty=true;skyboxKey=0;lost=false;
    objectControls();
    initialize();
  });
  async function initialize() {
    if(!window.__ROOM_PREVIEW_WASM__) {previewStatus('Shared renderer was not bundled in this build.',true);return;}
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
      const art=DATA.player;
      if(!art)throw Error('Player artwork is missing. Rebuild the editor with tools/action_editor/build.sh.');
      const parts=[art.characters,art.palette,art.picture].map(index=>BLOBS[index]);
      const player=new Uint8Array(parts.reduce((size,part)=>size+part.length,0));
      let offset=0;for(const part of parts){player.set(part,offset);offset+=part.length;}
      send(player,api.RoomPreview_LoadPlayer);
      $('#modeShared').disabled=false;previewStatus('Ready. Select 3D preview to explore the level.');
      refreshNativeCameraControls();draw();
    }catch(error){previewStatus(error.message,true);api=null;backend?.dispose();}
  }
  function validateScenery(text) {
    ensureRoom();
    send(new TextEncoder().encode(text),api.RoomPreview_Configure);
    config=text;configDirty=true;
  }
  function validatePolicyDocument(text) {
    const policies=parseBgPolicyDocument(text);
    if(!Object.keys(policies).length)return;
    ensureRoom();
    try {
      for(const [identity,pair] of Object.entries(policies)) {
        const base=DATA.rooms.find(r=>roomKey(r)===identity);
        for(let terrain=0;terrain<3;terrain++) {
          const variant=terrainRoom(base,terrain);
          send(SharedActionPreview.encode(DATA,BLOBS,variant),api.RoomPreview_Load);
          configureMode();
          try {send(new TextEncoder().encode(roomSectionHeader(base)+'\n'+bgPolicyPairLines(pair).join('\n')),api.RoomPreview_Configure);}
          catch {throw Error(`Background policy rejected for room ${base.group}:${base.map}, ${terrainLabel(variant)} terrain. Check band order, overlap and world row bounds.`);}
        }
      }
    } finally {
      key='';config='';configDirty=true;effectsConfig=null;collision=null;eventConfig=null;skyboxKey=0;
      ensureRoom();
    }
  }
  function policyPlan(defaults=false) {
    ensureRoom();
    if(api.RoomPreview_Policy(nativeCamera.x,nativeCamera.y,nativeFrame,defaults?1:0)!==98)
      throw Error('Unable to resolve background policy at this camera.');
    const words=new Int32Array(api.memory.buffer,api.DioramaPreview_Input(),98).slice();
    return [0,1].map(bg=>{
      const f=words.subarray(bg*49,(bg+1)*49),bands=[];
      for(let i=0;i<f[11];i++) {
        const b=f.subarray(17+i*8,25+i*8);
        bands.push({y0:b[0],y1:b[1],edge:BG_POLICY_EDGES[b[2]],motion:BG_POLICY_MOTIONS[b[3]],
          anchor:['screen','world'][b[4]],horizontal:{mode:BG_POLICY_EXTENTS[b[5]],left:b[6],right:b[7]}});
      }
      return {valid:!!f[0],role:f[1],source:f[2],edge:BG_POLICY_EDGES[f[3]],motion:BG_POLICY_MOTIONS[f[4]],
        horizontal:{mode:BG_POLICY_EXTENTS[f[5]],left:f[6],right:f[7]},
        vertical:{mode:BG_POLICY_EXTENTS[f[8]],top:f[9],bottom:f[10]},bands,
        cameraX:f[12],cameraY:f[13],width:f[14],height:f[15],wrap:!!f[16]};
    });
  }
  function validateEffects(text) {
    ensureRoom();
    const bytes=new TextEncoder().encode(text);
    if(bytes.length>ActionProjectArchive.limits['action-effects.ini'])throw Error('Effect document exceeds 1 MiB.');
    new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),bytes.length).set(bytes);
    if(!api.RoomPreview_ConfigureEffects(bytes.length)) {
      const start=api.RoomPreview_RecipeError(),memory=new Uint8Array(api.memory.buffer);
      let end=start;while(memory[end])end++;
      const reason=new TextDecoder().decode(memory.subarray(start,end));
      throw Error(`${reason||`Effects rejected at line ${api.RoomPreview_RecipeErrorLine()}.`} Previous effects retained.`);
    }
    effectsConfig=text;
  }
  function syncSources() {
    if(!api||lost)return;
    try {ensureRoom();if(effectsConfig!==EffectEditor.text())validateEffects(EffectEditor.text());EffectEditor.updateCatalogue(api);}
    catch(error){previewStatus(error.message,true);}
  }
  function previewActorBinding(source,x,y,start) {
    ensureRoom();validateEffects(EffectEditor.text());
    if(!api.RoomPreview_PreviewBinding(source===null?0:1,source??0,Math.round(x),Math.round(y),start))throw Error('Invalid actor preview anchor.');
    render();
  }
  function surfaceFieldDefinition(kind) {
    ensureRoom();const pointer=api.RoomPreview_SurfaceFieldText(kind);
    if(!pointer)throw Error('No complete lava / waterfall definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function projectileFieldDefinition(kind) {
    ensureRoom();const pointer=api.RoomPreview_ProjectileFieldText(kind);
    if(!pointer)throw Error('No complete projectile response definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function arcFieldDefinition(kind) {
    ensureRoom();const pointer=api.RoomPreview_ArcFieldText(kind);
    if(!pointer)throw Error('No complete arc response definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function glowFieldDefinition() {
    ensureRoom();const pointer=api.RoomPreview_GlowFieldText();
    if(!pointer)throw Error('No complete glow-field definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function castleFieldDefinition() {
    ensureRoom();const pointer=api.RoomPreview_CastleFieldText();
    if(!pointer)throw Error('No complete castle-field definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function marshFieldDefinition() {
    ensureRoom();const pointer=api.RoomPreview_MarshFieldText();
    if(!pointer)throw Error('No complete marsh-field definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function moonFieldDefinition() {
    ensureRoom();const pointer=api.RoomPreview_MoonFieldText();
    if(!pointer)throw Error('No complete moon-field definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function waterFieldDefinition() {
    ensureRoom();const pointer=api.RoomPreview_WaterFieldText();
    if(!pointer)throw Error('No complete water-field definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function atmosphereFieldDefinition() {
    ensureRoom();const pointer=api.RoomPreview_AtmosphereFieldText();
    if(!pointer)throw Error('No complete atmosphere-field definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function waterFieldMapScale() {
    ensureRoom();return [api.RoomPreview_WaterFieldMapScale(0),api.RoomPreview_WaterFieldMapScale(1)];
  }
  function rayFieldDefinition() {
    ensureRoom();
    const pointer=api.RoomPreview_RayFieldText();
    if(!pointer)throw Error('No complete ray-field definition is available.');
    const bytes=new Uint8Array(api.memory.buffer);let end=pointer;
    while(bytes[end])end++;
    return new TextDecoder().decode(bytes.subarray(pointer,end));
  }
  function rayFieldMapScale() {
    ensureRoom();
    return [api.RoomPreview_RayFieldMapScale(0),api.RoomPreview_RayFieldMapScale(1)];
  }
  function cloudOpenings(seed,count) {
    if(!api)return [];
    const length=api.RoomPreview_CloudOpenings(seed,count);
    const values=new Float32Array(api.memory.buffer,api.DioramaPreview_Input(),length),openings=[];
    for(let i=0;i<length;i+=6)openings.push({centre:values[i],width:values[i+1],power:values[i+2],offset:values[i+3],reach:values[i+4],endWidth:values[i+5]});
    return openings;
  }
  function voxelQuery(name,...args) {
    ensureRoom();
    if(typeof api[name]!=='function')throw Error('Rebuild the editor renderer to enable voxel face picking.');
    const length=api[name](...args);
    if(!length)return null;
    return JSON.parse(new TextDecoder().decode(new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),length)));
  }
  const voxelPick=(x,y,mask=3)=>voxelQuery('RoomPreview_VoxelPick',x,y,mask);
  const voxelTile=(x,y,face=0)=>voxelQuery('RoomPreview_VoxelTile',x,y,face);
  const voxelGroup=(x,y)=>voxelQuery('RoomPreview_VoxelGroup',x,y);
  const voxelTransformHandles=(x,y,width,height,px,py,pz,dx,dy,dz,sx,sy,sz)=>
    voxelQuery('RoomPreview_VoxelTransformHandles',x,y,width,height,px,py,pz,dx,dy,dz,sx,sy,sz);
  const voxelBendHandles=(x,y,width,height,axis,anchor,tip,dx,dy,dz)=>
    voxelQuery('RoomPreview_VoxelBendHandles',x,y,width,height,axis,anchor,tip,dx,dy,dz);
  const voxelObjects=()=>voxelQuery('RoomPreview_VoxelObjects')||[];
  initialize();
  return {measure,cloudOpenings,voxelPick,voxelTile,voxelGroup,voxelTransformHandles,voxelBendHandles,voxelObjects,setInspection,inspection:()=>inspection,setCameraPreset,focusVoxelSelection,cancelPlacement:()=>placementMode(false),placePlayerAt,syncSources,validateScenery,validatePolicyDocument,policyPlan,validateEffects,previewActorBinding,surfaceFieldDefinition,projectileFieldDefinition,arcFieldDefinition,glowFieldDefinition,castleFieldDefinition,marshFieldDefinition,atmosphereFieldDefinition,rayFieldDefinition,rayFieldMapScale,moonFieldDefinition,waterFieldDefinition,waterFieldMapScale,collisionGrid,draw:render,invalidate:()=>{configDirty=true;measures.clear();}};
})();
