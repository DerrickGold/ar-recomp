/* global ActionAuditCamera, SharedActionPreview, ActionWebGL2Backend */
/* Read-only UI. Native decoding, voxel policies, effects and projection stay
 * in the same C/WASM renderer shipped with the desktop action editor. */
(() => {
  'use strict';
  const bundle=window.__ACTION_AUDIT__,data=bundle.data,$=id=>document.getElementById(id);
  const decode=text=>Uint8Array.from(atob(text),c=>c.charCodeAt(0));
  const blobs=data.blobs.map(decode),encoder=new TextEncoder(),decoder=new TextDecoder();
  const canvas=$('view'),stage=$('stage'),math=ActionAuditCamera;
  const regions=data.terrainProfiles||[{profile:0,label:'US'}];
  let view={room:0,region:0,x:0,y:0,yaw:0,pitch:.15,zoom:1,frame:320,original:false,effects:true};
  let nativeBounds={minX:0,maxX:0,maxY:0},bounds={minX:0,maxX:0,minY:0,maxY:0};
  let nativeCamera={x:0,y:0},cameraPan={x:0,y:0},visibleHeight=224,flatViewportHeight=224,api=null,backend=null,loaded='',skyboxKey=0;
  let pending=false,lost=false,pinning=false,playing=false,lastTick=0,frameClock=320,animationRequest=0;
  let currentCapture=null,reportBlob=null,reportUrl='',saveTimer=0;
  let startAtFloor=true;
  const points=new Map();let gesture=null,tap=null,lastTap=null,mousePan=false;
  const roomId=r=>`${r.group.toString(16).padStart(2,'0')}-${r.map.toString(16).padStart(2,'0')}${r.section?' '+r.section:''}`;
  function currentRoom() {
    const base=data.rooms[view.room],variant=base.terrainVariants?.find(v=>v.profile===view.region);
    return variant?{...base,terrainProfile:view.region,bg:[variant.bg1,base.bg[1]]}:base;
  }
  function roomName(r=currentRoom()) {
    const names=['','Fillmore','Bloodpool','Kassandora','Aitos','Marahna','Northwall','Death Heim'];
    const secondAct=[0,2,2,3,4,4,5];
    const act=r.group<7?` Act ${r.map>=secondAct[r.group]?2:1}`:'';
    return `${names[r.group]||'Action'}${act} · ${roomId(r)}`;
  }
  function regionName() {return regions.find(r=>r.profile===view.region)?.label||String(view.region);}
  function status(text,error=false) {
    $('status').textContent=text;$('status').classList.toggle('error',error);
    $('settingsStatus').textContent=error?text:'';
    if(error)$('location').textContent=text;
  }
  function send(bytes,call) {
    if(bytes.length>=api.DioramaPreview_Capacity())throw Error('Scene exceeds preview memory.');
    new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),bytes.length).set(bytes);
    if(!call(bytes.length))throw Error('The shared renderer rejected the scene or configuration.');
  }
  function setView() {
    if(!api.RoomPreview_SetView(bundle.view.distance??3.25,0,0,2,0))throw Error('Invalid camera baseline.');
  }
  function ensureRoom() {
    const r=currentRoom(),key=`${view.room}:${view.region}`;
    if(loaded===key)return;
    send(SharedActionPreview.encode(data,blobs,r),api.RoomPreview_Load);
    send(encoder.encode(bundle.scenery),api.RoomPreview_Configure);
    send(encoder.encode(bundle.effects),api.RoomPreview_ConfigureEffects);
    if(!api.RoomPreview_Set3DMode(2))throw Error('Voxel preview is unavailable.');
    api.RoomPreview_SetReceivers(0,0,0,0,0);setView();
    loaded=key;skyboxKey=0;
    nativeBounds={minX:0,maxX:Math.max(0,api.RoomPreview_Width()-256),maxY:Math.max(0,api.RoomPreview_Height()-224)};
    bounds=math.inspectionBounds(nativeBounds);
    if(startAtFloor) {
      const count=api.RoomPreview_CollisionGrid(),width=api.RoomPreview_Width()/16;
      view.y=count?math.initialY(new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),count),width,count/width):0;
      startAtFloor=false;
    }
  }
  function args() {return [Math.round(math.clamp(view.x,Math.max(0,nativeBounds.minX),nativeBounds.maxX)),
    Math.round(math.clamp(view.y,0,nativeBounds.maxY)),view.frame>>>0,120,64,
    canvas.width,canvas.height,view.zoom,view.yaw,view.pitch,1,bundle.view.pixelAspect==='crt'?1:0];}
  function sync() {
    $('previous').disabled=view.room===0;$('next').disabled=view.room===data.rooms.length-1;
    for(const key of ['zoom','yaw','pitch','frame'])if(document.activeElement!==$(key))$(key).value=String(view[key]);
    $('original').setAttribute('aria-pressed',String(view.original));
    gestureHint();
    for(const [axis,max] of [['X',bounds.maxX],['Y',bounds.maxY]]) {
      const value=Math.round(view[axis.toLowerCase()]);
      for(const prefix of ['position','number']) {
        const node=$(prefix+axis);node.min=axis==='X'?bounds.minX:bounds.minY;node.max=max;
        if(document.activeElement!==node)node.value=value;
      }
    }
  }
  function draw() {
    pending=false;if(!api||lost)return false;
    try {
      ensureRoom();
      const rect=stage.getBoundingClientRect(),size=math.outputSize(rect.width,rect.height);
      if(canvas.width!==size.width)canvas.width=size.width;
      if(canvas.height!==size.height)canvas.height=size.height;
      // Flat scanout has native map bounds, independent of the 3D camera pose.
      if(view.original)nativeBounds={minX:0,maxX:Math.max(0,api.RoomPreview_Width()-256),maxY:Math.max(0,api.RoomPreview_Height()-225)};
      bounds=math.inspectionBounds(nativeBounds);
      view.x=math.clamp(view.x,bounds.minX,bounds.maxX);view.y=math.clamp(view.y,bounds.minY,bounds.maxY);
      const values=args();
      nativeCamera={x:values[0],y:values[1]};
      if(!view.original && api.RoomPreview_Measure(...values,0)===16) {
        const a=new Float32Array(api.memory.buffer,api.DioramaPreview_Input(),16).slice();
        nativeCamera={x:a[0],y:a[1]};nativeBounds={minX:a[2],maxX:a[3],maxY:a[4]};visibleHeight=a[11];
        bounds=math.inspectionBounds(nativeBounds);
        view.x=math.clamp(view.x,bounds.minX,bounds.maxX);view.y=math.clamp(view.y,bounds.minY,bounds.maxY);
        values[0]=Math.round(nativeCamera.x);values[1]=Math.round(nativeCamera.y);
      }
      cameraPan={x:nativeCamera.x-view.x,y:nativeCamera.y-view.y};
      if(!api.RoomPreview_SetCameraPan(cameraPan.x,cameraPan.y))throw Error('Invalid inspection pan.');
      api.RoomPreview_EnableEffects(view.effects?1:0);
      api.RoomPreview_ShowBackdrop(bundle.view.showBackdrop?1:0);
      const render=()=>view.original?
        api.RoomPreview_RenderFlat(values[0],values[1],values[2],values[5],values[6],values[11]):
        api.RoomPreview_Render(...values);
      let ok=render();
      if(view.original && ok) {
        const a=new Float32Array(api.memory.buffer,api.DioramaPreview_Input(),16);
        nativeCamera={x:a[0],y:a[1]};visibleHeight=a[11];flatViewportHeight=a[15];
        cameraPan={x:nativeCamera.x-view.x,y:nativeCamera.y-view.y};
      }
      if(!view.original) {
        const source=api.RoomPreview_SkyboxRoom();
        if(source && source!==skyboxKey) {
          const sky=data.rooms.find(r=>r.group===(source>>>16)&&r.map===((source>>>8)&255));
          if(!sky)throw Error('Named skybox is missing from this bundle.');
          send(SharedActionPreview.encode(data,blobs,sky),api.RoomPreview_LoadSkybox);ok=render();
        }
        skyboxKey=source;
      }
      const error=backend.gl.getError();if(!ok||error)throw Error(`Scene rendering failed (${error}). Try another room or reload.`);
      $('location').textContent=`${roomId(currentRoom())} · ${regionName()} · X ${Math.round(view.x)} Y ${Math.round(view.y)} · ${view.original?'Original 2D':'Voxel 3D'}`;
      $('reportButton').disabled=false;status('Ready · '+bundle.build.revision);sync();
      savePosition();return true;
    } catch(error) {setPlaying(false);$('reportButton').disabled=true;status(error.message,true);return false;}
  }
  function requestDraw() {if(!pending){pending=true;requestAnimationFrame(draw);}}
  function cancelGestures() {points.clear();gesture=null;tap=null;lastTap=null;mousePan=false;}
  function gestureHint() {
    $('hint').textContent=pinning?'Tap the issue to pin it · Capture view to report without a pin':
      view.original?'Drag to pan · Original 2D comparison':
        '1 finger: pan · 2 fingers: orbit / pinch · Double-tap: reset';
  }
  function pinMode(enabled) {
    pinning=enabled;$('reportButton').textContent=enabled?'Capture view':'Report';
    gestureHint();
  }
  function resetCamera() {view.yaw=0;view.pitch=.15;view.zoom=1;lastTap=null;requestDraw();}
  function selectRoom(index,reset=true) {
    view.room=math.clamp(index,0,data.rooms.length-1);$('room').value=view.room;
    $('reportButton').disabled=true;status('Loading '+roomName()+'…');
    loaded='';cancelGestures();pinMode(false);
    if(reset){view.x=view.y=0;view.frame=320;frameClock=320;startAtFloor=true;}
    requestDraw();
  }
  function openDialog(id) {cancelGestures();pinMode(false);sync();$(id).showModal();}
  let focusScroll=0;
  function keepReportFieldVisible() {
    focusScroll=0;const field=document.activeElement,dialog=$('reportDialog');
    if(!dialog.open || !dialog.contains(field) || !field.matches('textarea,input,select'))return;
    const content=dialog.querySelector('.content'),box=field.getBoundingClientRect();
    const area=content.getBoundingClientRect(),header=dialog.querySelector('header').getBoundingClientRect();
    const top=Math.max(area.top,header.bottom)+10,bottom=area.bottom-12;
    if(box.bottom>bottom)content.scrollTop+=box.bottom-bottom;
    else if(box.top<top)content.scrollTop-=top-box.top;
  }
  function updateHeight() {
    const viewport=window.visualViewport,style=document.documentElement.style;
    for(const [name,value] of Object.entries({'app-height':viewport?.height||window.innerHeight,
      'visual-width':viewport?.width||window.innerWidth,'visual-top':viewport?.offsetTop||0,'visual-left':viewport?.offsetLeft||0}))
      style.setProperty('--'+name,`${value}px`);
    document.body.classList.toggle('editing-report',$('reportDialog').open&&$('reportDialog').contains(document.activeElement)&&document.activeElement.matches('textarea,input'));
    if(!focusScroll)focusScroll=requestAnimationFrame(keepReportFieldVisible);
    if(!document.querySelector('dialog[open]'))requestDraw();
  }
  function savePosition() {
    clearTimeout(saveTimer);saveTimer=setTimeout(()=>{
      try {localStorage.setItem('actraiser-audit-view-v1',JSON.stringify(view));}catch { /* Private/file browsing may disable storage. */ }
    },200);
  }
  function restorePosition() {
    try {
      const saved=JSON.parse(localStorage.getItem('actraiser-audit-view-v1'));
      if(!saved || !Number.isInteger(saved.room)||saved.room<0||saved.room>=data.rooms.length)return;
      for(const key of ['x','y','yaw','pitch','zoom','frame','region'])if(!Number.isFinite(saved[key]))return;
      if(!regions.some(r=>r.profile===saved.region))return;
      view={...view,...saved,yaw:math.clamp(saved.yaw,-.7,.7),pitch:math.clamp(saved.pitch,-.7,.7),
        zoom:math.clamp(saved.zoom,.5,2.5),frame:math.clamp(Math.round(saved.frame),0,0xffffffff)};
      view.original=!!saved.original;view.effects=!!saved.effects;
      startAtFloor=false;
    }catch { /* Start with defaults if storage is absent or malformed. */ }
  }
  function endPointer(event,cancel=false) {
    if(!points.has(event.pointerId))return;
    const captured=tap,wasSingle=points.size===1;
    points.delete(event.pointerId);gesture=math.sample(points);tap=null;
    if(!cancel&&pinning&&wasSingle&&captured&&!captured.moved) {
      const point=math.canvasPoint(canvas.getBoundingClientRect(),canvas.width,canvas.height,event.clientX,event.clientY);
      if(point)captureReport(point);
    } else if(!cancel&&!pinning&&wasSingle&&captured&&!captured.moved&&event.pointerType==='touch'&&
        performance.now()-captured.time<300) {
      const now=performance.now();
      if(lastTap&&now-lastTap.time<320&&Math.hypot(event.clientX-lastTap.x,event.clientY-lastTap.y)<28)resetCamera();
      else lastTap={time:now,x:event.clientX,y:event.clientY};
    }
    if(cancel)lastTap=null;
  }
  canvas.addEventListener('pointerdown',event=>{
    if(event.button!==0 && !(event.pointerType==='mouse'&&(event.button===1||event.button===2)))return;
    event.preventDefault();canvas.setPointerCapture(event.pointerId);
    mousePan=event.pointerType==='mouse'&&(event.button===1||event.button===2||event.shiftKey);
    points.set(event.pointerId,{x:event.clientX,y:event.clientY});gesture=math.sample(points);
    tap=points.size===1?{x:event.clientX,y:event.clientY,moved:false,time:performance.now()}:null;
    if(points.size>1)lastTap=null;
  });
  canvas.addEventListener('pointermove',event=>{
    if(!points.has(event.pointerId))return;event.preventDefault();
    points.set(event.pointerId,{x:event.clientX,y:event.clientY});
    if(tap && Math.hypot(event.clientX-tap.x,event.clientY-tap.y)>8){tap.moved=true;lastTap=null;}
    const next=math.sample(points);
    if(!pinning) {
      const height=canvas.getBoundingClientRect().height;
      const units=view.original?visibleHeight/Math.max(1,height*flatViewportHeight/canvas.height):
        visibleHeight*view.zoom/Math.max(1,height);
      const mode=view.original||mousePan?'pan':event.pointerType==='mouse'?'orbit':'touch';
      view=math.move(view,gesture,next,units,mode);requestDraw();
    }
    gesture=next;
  });
  canvas.addEventListener('pointerup',event=>endPointer(event));
  for(const name of ['pointercancel','lostpointercapture'])canvas.addEventListener(name,event=>endPointer(event,true));
  canvas.addEventListener('contextmenu',event=>event.preventDefault());
  canvas.addEventListener('wheel',event=>{
    event.preventDefault();view.zoom=math.clamp(view.zoom*Math.exp(event.deltaY*.001),.5,2.5);requestDraw();
  },{passive:false});
  window.addEventListener('blur',cancelGestures);
  document.addEventListener('visibilitychange',()=>{if(document.hidden){setPlaying(false);cancelGestures();}});
  for(const [index,r] of data.rooms.entries())$('room').add(new Option(roomName(r),String(index)));
  for(const r of regions)$('region').add(new Option(r.label,String(r.profile)));
  restorePosition();$('room').value=view.room;$('region').value=view.region;$('effects').checked=view.effects;
  $('room').onchange=()=>selectRoom(Number($('room').value));
  $('region').onchange=()=>{view.region=Number($('region').value);selectRoom(view.room);};
  $('previous').onclick=()=>selectRoom(view.room-1);$('next').onclick=()=>selectRoom(view.room+1);
  $('reset').onclick=()=>{cancelGestures();pinMode(false);resetCamera();};
  $('navigate').onclick=()=>openDialog('navigation');$('options').onclick=()=>openDialog('settings');
  document.querySelectorAll('[data-close]').forEach(button=>button.onclick=()=>$(button.dataset.close).close());
  for(const id of ['navigation','settings','reportDialog'])$(id).addEventListener('close',()=>{cancelGestures();canvas.focus({preventScroll:true});updateHeight();});
  $('reportDialog').addEventListener('focusin',()=>{updateHeight();setTimeout(updateHeight,250);});
  $('reportDialog').addEventListener('focusout',()=>setTimeout(updateHeight,0));
  for(const axis of ['X','Y'])for(const prefix of ['position','number'])$(prefix+axis).oninput=()=>{
    const value=Number($(prefix+axis).value);if(!Number.isFinite(value))return;
    view[axis.toLowerCase()]=value;requestDraw();
  };
  document.querySelectorAll('[data-jump]').forEach(button=>button.onclick=()=>{
    const f={start:0,middle:.5,end:1}[button.dataset.jump];
    view.x=nativeBounds.minX+(nativeBounds.maxX-nativeBounds.minX)*f;view.y=nativeBounds.maxY*f;requestDraw();
  });
  const angles={front:[0,0],left:[-.42,.18],right:[.42,.18],above:[0,.6],below:[0,-.5]};
  document.querySelectorAll('[data-angle]').forEach(button=>button.onclick=()=>{
    [view.yaw,view.pitch]=angles[button.dataset.angle];requestDraw();
  });
  for(const key of ['zoom','yaw','pitch','frame'])$(key).oninput=()=>{
    const value=Number($(key).value);if(!Number.isFinite(value))return;
    const limits={zoom:[.5,2.5],yaw:[-.7,.7],pitch:[-.7,.7],frame:[0,0xffffffff]};
    view[key]=math.clamp(value,...limits[key]);if(key==='frame')view.frame=Math.round(view.frame);
    frameClock=view.frame;requestDraw();
  };
  $('resetCamera').onclick=resetCamera;
  $('original').onclick=()=>{view.original=!view.original;requestDraw();};
  $('effects').onchange=()=>{view.effects=$('effects').checked;requestDraw();};
  function tick(now) {
    if(!playing)return;
    if(now-lastTick>=1000/20) {
      frameClock+=Math.min(250,now-lastTick)*.06;view.frame=Math.floor(frameClock)>>>0;lastTick=now;requestDraw();
    }
    animationRequest=requestAnimationFrame(tick);
  }
  function setPlaying(enabled) {
    cancelAnimationFrame(animationRequest);playing=enabled;$('animate').checked=enabled;
    lastTick=performance.now();frameClock=view.frame;if(playing)animationRequest=requestAnimationFrame(tick);
  }
  $('animate').onchange=()=>setPlaying($('animate').checked);
  function reportDocument() {
    return {...currentCapture,category:$('category').value,notes:$('notes').value.trim()};
  }
  function reportText() {
    if(!currentCapture)return '';
    const r=reportDocument(),c=r.camera;
    return [`ActRaiser level audit · ${r.room.label} · ${r.terrain.label}`,
      `Build ${r.build.revision} / ${r.build.fingerprint}`,`Camera ${c.x}, ${c.y} · frame ${c.frame} · ${r.mode}`,
      `Yaw ${c.yaw.toFixed(3)} · pitch ${c.pitch.toFixed(3)} · zoom ${c.zoom.toFixed(3)} · pan ${c.panX.toFixed(1)}, ${c.panY.toFixed(1)}`,
      r.pin?`Pin screen ${r.pin.x}, ${r.pin.y}${r.pin.tile?.kind==='tile'?` · native cell ${r.pin.tile.x}, ${r.pin.tile.y}`:''}`:'Whole view',
      `Effects ${r.effects?'on':'off'} · ${r.category}`,r.notes||'(No notes)',
      '',JSON.stringify(r,null,2)].join('\n');
  }
  function updateReport() {$('reportText').value=reportText();}
  async function captureReport(point=null) {
    setPlaying(false);cancelGestures();pinMode(false);
    if(!draw())return;
    let tile=null;
    if(point&&!view.original) {
      const length=api.RoomPreview_VoxelPick(point.x,point.y,1);
      if(length)tile=JSON.parse(decoder.decode(new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),length)));
    }
    const room=currentRoom();
    currentCapture={schema:'actraiser-level-audit',version:1,createdAt:new Date().toISOString(),build:bundle.build,
      room:{group:room.group,map:room.map,section:room.section||'',label:roomName(room)},
      terrain:{profile:view.region,label:regionName()},
      camera:{x:Math.round(nativeCamera.x),y:Math.round(nativeCamera.y),panX:cameraPan.x,panY:cameraPan.y,
        focusX:view.x,focusY:view.y,yaw:view.yaw,pitch:view.pitch,zoom:view.zoom,
        frame:view.frame,baselineDistance:bundle.view.distance??3.25},
      mode:view.original?'original-2d':'voxel-3d',effects:!view.original&&!!view.effects,
      presentation:{pixelAspect:bundle.view.pixelAspect,skybox:1,backdrop:!!bundle.view.showBackdrop},
      image:{width:canvas.width,height:canvas.height+92},
      pin:point?{x:Math.round(point.x),y:Math.round(point.y),tile}:null};
    const shot=document.createElement('canvas');shot.width=canvas.width;shot.height=canvas.height+92;
    const ctx=shot.getContext('2d');ctx.drawImage(canvas,0,0);
    if(point) {
      ctx.beginPath();ctx.arc(point.x,point.y,22,0,Math.PI*2);ctx.strokeStyle='#ff694e';ctx.lineWidth=4;ctx.stroke();
      ctx.beginPath();ctx.moveTo(point.x-6,point.y);ctx.lineTo(point.x+6,point.y);ctx.moveTo(point.x,point.y-6);ctx.lineTo(point.x,point.y+6);ctx.lineWidth=2;ctx.stroke();
    }
    ctx.fillStyle='#101824';ctx.fillRect(0,canvas.height,canvas.width,92);ctx.fillStyle='#f0f5ff';
    const font=Math.max(10,Math.min(15,canvas.width/35));ctx.font=`${font}px system-ui`;
    const c=currentCapture.camera;
    for(const [i,line] of [`${roomId(room)} · ${regionName()} · ${currentCapture.mode}`,
      `X ${c.x} Y ${c.y} · pan ${c.panX.toFixed(0)},${c.panY.toFixed(0)} · frame ${c.frame} · yaw ${c.yaw.toFixed(2)} pitch ${c.pitch.toFixed(2)} zoom ${c.zoom.toFixed(2)}`,
      `Build ${bundle.build.revision} · ${bundle.build.fingerprint}`].entries())ctx.fillText(line,10,canvas.height+24+i*24,canvas.width-20);
    reportBlob=null;for(const id of ['share','saveImage','saveReport'])$(id).disabled=true;
    $('notes').value='';$('reportStatus').textContent='Preparing screenshot…';updateReport();
    openDialog('reportDialog');
    try {
      reportBlob=await new Promise((resolve,reject)=>shot.toBlob(b=>b?resolve(b):reject(Error('Screenshot could not be encoded.')),'image/png'));
      if(reportUrl)URL.revokeObjectURL(reportUrl);reportUrl=URL.createObjectURL(reportBlob);$('reportImage').src=reportUrl;
      for(const id of ['share','saveImage','saveReport'])$(id).disabled=false;
      $('reportStatus').textContent='Ready to share. The image includes room and camera details; the report includes your notes and exact tile hit when available.';
    }catch(error){$('reportStatus').textContent=error.message;}
  }
  $('reportButton').onclick=()=>{if(pinning)captureReport();else pinMode(true);};
  $('notes').oninput=$('category').onchange=updateReport;
  const filename=extension=>`actraiser-${roomId(currentCapture.room).replace(/[^\w-]/g,'-')}-${currentCapture.camera.x}-${currentCapture.camera.y}-${currentCapture.camera.frame}.${extension}`;
  function download(blob,name) {
    const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=name;
    document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),60000);
  }
  $('saveImage').onclick=()=>download(reportBlob,filename('png'));
  $('saveReport').onclick=()=>download(new Blob([JSON.stringify(reportDocument(),null,2)],{type:'application/json'}),filename('json'));
  $('copyReport').onclick=async()=>{
    try {await navigator.clipboard.writeText(reportText());$('reportStatus').textContent='Report copied.';}
    catch {$('reportText').closest('details').open=true;$('reportText').focus();$('reportText').select();$('reportStatus').textContent='Select and copy the report text below.';}
  };
  $('share').onclick=async()=>{
    const image=new File([reportBlob],filename('png'),{type:'image/png'});
    const report=new File([JSON.stringify(reportDocument(),null,2)],filename('json'),{type:'application/json'});
    try {
      if(!navigator.share||!navigator.canShare?.({files:[image]})) {
        $('reportStatus').textContent='File sharing is unavailable here. Save image and Save report, or long-press the preview image and copy the report text.';return;
      }
      const files=navigator.canShare({files:[image,report]})?[image,report]:[image];
      await navigator.share({files,title:'ActRaiser level audit',text:reportText()});
      $('reportStatus').textContent='Shared.';
    }catch(error){if(error.name!=='AbortError')$('reportStatus').textContent=`Sharing failed: ${error.message}. Save the image and report instead.`;}
  };
  canvas.addEventListener('webglcontextlost',event=>{
    event.preventDefault();lost=true;setPlaying(false);cancelGestures();
    $('reportButton').disabled=true;status('Graphics context lost. Restoring the current view…',true);
  });
  canvas.addEventListener('webglcontextrestored',()=>{api=null;backend=null;loaded='';skyboxKey=0;lost=false;initialize();});
  async function initialize() {
    try {
      backend=new ActionWebGL2Backend(canvas,bundle.shaders);
      const module=await WebAssembly.compile(decode(bundle.wasm));
      const wasi={fd_close:()=>0,fd_seek:()=>0,fd_write:()=>0,proc_exit:code=>{throw Error(`Renderer exited (${code}).`);}};
      for(const item of WebAssembly.Module.imports(module))
        if(!(item.module==='ar'&&Object.hasOwn(backend.imports,item.name))&&
            !(item.module==='wasi_snapshot_preview1'&&Object.hasOwn(wasi,item.name)))throw Error(`Unsupported renderer import: ${item.name}`);
      const instance=await WebAssembly.instantiate(module,{ar:backend.imports,wasi_snapshot_preview1:wasi});
      api=instance.exports;backend.memory=api.memory;api._initialize?.();
      if(api.DioramaPreview_Version()!==1||!api.DioramaPreview_Init(backend.gl.getParameter(backend.gl.MAX_TEXTURE_SIZE)))throw Error('This device cannot initialize the 3D renderer.');
      $('buildLabel').textContent=`Build ${bundle.build.revision} · ${bundle.build.fingerprint} · ${bundle.build.builtAt}`;
      requestDraw();
    } catch(error) {status(error.message,true);api=null;backend?.dispose();}
  }
  window.addEventListener('resize',updateHeight);window.visualViewport?.addEventListener('resize',updateHeight);
  window.visualViewport?.addEventListener('scroll',updateHeight);
  updateHeight();sync();initialize();
})();
