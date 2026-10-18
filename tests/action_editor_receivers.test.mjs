/* Real WASM, real ROM artwork, production receiver sampling and OBJ compositor.
 * Keep uploaded pixels to verify the complete transport, not just sampler math. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const [htmlFile,wasmFile]=process.argv.slice(2);
const html=fs.readFileSync(htmlFile,'utf8');
const data=JSON.parse(html.match(/window\.__ACTION_BG__=(.*?);<\/script>/s)[1]);
const blobs=data.blobs.map(s=>Uint8Array.from(Buffer.from(s,'base64')));
const context=vm.createContext({window:{},Uint8Array,Uint32Array,DataView});
vm.runInContext(fs.readFileSync('tools/action_editor/shared_preview.js','utf8'),context);
const encode=vm.runInContext('SharedActionPreview.encode',context);
let api,target=0,effect=-1,creates=0;
const textures=new Map(),uploads=[],draws=[];
const ar={
  create(w,h,usage){let id=1;while(textures.has(id))id++;
    textures.set(id,{w,h,usage,pixels:new Uint32Array(w*h)});creates++;return id;},
  destroy(id){assert(textures.delete(id));},
  update(id,x,y,w,h,p,pitch){const t=textures.get(id);assert(t&&x>=0&&y>=0&&x+w<=t.w&&y+h<=t.h);
    for(let row=0;row<h;row++)t.pixels.set(new Uint32Array(api.memory.buffer,p+row*pitch,w),(y+row)*t.w+x);
    uploads.push({id,x,y,w,h});return 1;},
  target(id){target=id;return 1;},viewport(){return 1;},clip(){return 1;},clear(){return 1;},
  geometry(id,p,n,ip,ni){assert(!id||(textures.has(id)&&id!==target));
    const v=new Float32Array(api.memory.buffer,p,n*8).slice();assert(v.every(Number.isFinite));
    draws.push({id,effect,target,v,indices:new Int32Array(api.memory.buffer,ip,ni).slice()});return 1;},
  effect(kind,p,n){if(kind>=0){assert.equal(effect,-1);assert(new Float32Array(api.memory.buffer,p,n).every(Number.isFinite));}
    effect=kind;return 1;},available(){return 1;}
};
const wasi={fd_close:()=>0,fd_seek:()=>0,fd_write:()=>0,proc_exit:n=>{throw Error(`exit ${n}`);}};
({instance:{exports:api}}=await WebAssembly.instantiate(fs.readFileSync(wasmFile),{ar,wasi_snapshot_preview1:wasi}));
api._initialize?.();assert.equal(api.DioramaPreview_Init(16384),1);
const send=(bytes,fn)=>{new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),bytes.length).set(bytes);return fn(bytes.length);};
const art=Buffer.concat([data.player.characters,data.player.palette,data.player.picture].map(i=>Buffer.from(blobs[i])));
assert.equal(send(art,api.RoomPreview_LoadPlayer),1);
assert.equal(send(art.subarray(0,art.length-1),api.RoomPreview_LoadPlayer),0,'reject truncated picture atomically');
const bad=Buffer.from(art);bad[8192+96+5]=2;
assert.equal(send(bad,api.RoomPreview_LoadPlayer),0,'reject unsupported part size without losing valid art');
const room=data.rooms.find(r=>r.group===3&&r.map===3);
assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
const noEffects='[effects]\nversion=1\n';
const configure=text=>assert.equal(send(Buffer.from(text),api.RoomPreview_ConfigureEffects),1);
configure(noEffects);
let camera=[256,160],extra=120,vertical=64;
function render(view=[960,600,1,0,0,1,0]) {
  draws.length=uploads.length=0;
  assert.equal(api.RoomPreview_Render(...camera,37,extra,vertical,...view),1);
  assert.equal(effect,-1,'shader bindings restored');
}
api.RoomPreview_EnableEffects(0);render();
const prior=new Set(textures.keys());
assert.equal(api.RoomPreview_SetReceivers(1,128,352,192,352),1);
render();
// The first OBJ may also grow a compositor scratch target: BG guard columns
// are conditional, whereas the actor surface always retains its apron.
const added=[...textures.keys()].filter(id=>!prior.has(id)&&textures.get(id).w===640&&textures.get(id).h===352);
assert.equal(added.length,1);
const spriteId=added[0],texture=textures.get(spriteId);
const original=texture.pixels.slice();
assert(original.filter(Boolean).length>500,'complete multi-part player, not a geometric placeholder');
assert(new Set(original).size>10,'native palette is retained');
assert(original.every(p=>!p||p>>>24===255),'transparent index zero is never filled');
const warm=creates;
render();assert.equal(uploads.length,0);assert.equal(creates,warm);
render([960,540,1.2,.1,-.15,1,1]);
assert.equal(uploads.filter(u=>u.id===spriteId).length,0,'orbit/aspect/pixel-aspect changes do not upload receiver pixels');

const exposure=targets=>noEffects+'[emitter:03:03:0:exposure:00000001]\n'+
  'x=256\ny=256\nwidth=1024\nheight=1024\nbrightness=.25\ndim-scenery=0\n'+targets;
api.RoomPreview_EnableEffects(1);configure(exposure('dim-player=1\ndim-enemies=0\n'));render();
const dimmed=texture.pixels.slice();
for(let i=0;i<original.length;i++)if(original[i]) {
  assert.equal(dimmed[i]>>>24,255);
  for(const shift of [0,8,16])assert.equal(dimmed[i]>>>shift&255,(original[i]>>>shift&255)>>2);
}
assert.equal(api.RoomPreview_ReceiverStyle(0,1),1);render();
assert(draws.some(d=>d.id===spriteId&&d.effect===1),'production rim shader binds to the lit sprite plane');
assert.equal(uploads.length,0,'rim toggle reuses already-lit pixels');
assert.deepEqual(texture.pixels,dimmed,'rim does not overwrite lighting on CPU');
assert.equal(api.RoomPreview_ReceiverStyle(0,0),1);

// Both role masks on identical art at adjacent world positions.
assert.equal(api.RoomPreview_SetReceivers(3,128,352,192,352),1);render();
for(let i=0;i<original.length;i++)if(original[i])assert.equal(texture.pixels[i+64],original[i],
  'enemy comparison ignores player-only dimming');
configure(exposure('dim-player=0\ndim-enemies=1\n'));render();
for(let i=0;i<original.length;i++)if(original[i]) {
  assert.equal(texture.pixels[i],original[i]);assert.equal(texture.pixels[i+64],dimmed[i]);
}
assert.equal(api.RoomPreview_SetReceivers(1,128,352,192,352),1);
const torch=targets=>noEffects+'[emitter:03:03:0:torch:00000002]\n'+
  'x=128\ny=320\nwidth=220\nheight=176\ncolor=ff6020\nlight-scenery=0\n'+targets;
configure(torch('light-player=1\nlight-enemies=0\n'));render();
const lit=texture.pixels.slice();
assert(lit.some((p,i)=>original[i]&&((p>>>16&255)>(original[i]>>>16&255))), 'torch adds warm illumination to artwork');
configure(torch('light-player=0\nlight-enemies=1\n'));render();
assert.deepEqual(texture.pixels,original,'player ignores a torch configured for enemies');
assert.equal(api.RoomPreview_SetReceivers(2,128,352,128,352),1);render();
assert.deepEqual(texture.pixels,lit,'enemy receiver gets exactly the same torch lighting');
api.RoomPreview_EnableEffects(0);render();assert.deepEqual(texture.pixels,original);

// Room dimming reduces ambient illumination. Local lights restore the native
// palette, instead of adding a flat tint over dimmed pixels or multiplying
// the incoming light by the room's darkness a second time.
const fill=(color,intensity=4)=>'[emitter:03:03:0:light-gradient:00000003]\n'+
  `x=128\ny=320\nwidth=1024\nheight=1024\nsoftness=0\nillumination=1\nintensity=${intensity}\n`+
  `color=${color}\ncolor-end=${color}\nlight-scenery=0\nlight-player=1\nlight-enemies=1\n`;
const darkRoom=exposure('dim-player=1\ndim-enemies=1\n').replace('brightness=.25','brightness=.28');
api.RoomPreview_EnableEffects(1);configure(darkRoom+fill('ffffff'));
for(const role of [1,2]) {
  assert.equal(api.RoomPreview_SetReceivers(role,128,352,128,352),1);render();
  assert.deepEqual(texture.pixels,original,'28% ambient + 72% white light restores every palette entry');
}
configure(darkRoom+fill('ff8040'));render();
for(let i=0;i<original.length;i++)if(original[i]) {
  const result=texture.pixels[i];
  assert.equal(result>>>24,255);
  assert.equal(result>>>16&255,original[i]>>>16&255,'warm light restores the red channel');
  for(const shift of [0,8,16]) {
    const source=original[i]>>>shift&255,value=result>>>shift&255;
    assert(value>=Math.floor(source*.28),'colored light also reveals dimmed detail');
    if(!source)assert.equal(value,0,'lighting cannot wash color into black palette channels');
  }
}
const recolored=texture.pixels.slice();render();
assert.equal(uploads.length,0,'stable combined lighting reuses sprite pixels');
assert.deepEqual(texture.pixels,recolored);
for(const intensity of [8,16])for(const role of [1,2]) {
  configure(darkRoom+fill('ff8040',intensity));
  assert.equal(api.RoomPreview_SetReceivers(role,128,352,128,352),1);render();
  const gains=[1,128/255,64/255].map(t=>Math.round(Math.min(4,.28+.18*intensity*t)*256));
  for(let i=0;i<original.length;i++)if(original[i])for(const [c,shift] of [16,8,0].entries())
    assert.equal(texture.pixels[i]>>>shift&255,Math.min(255,Math.floor((original[i]>>>shift&255)*gains[c]/256)),
      'high intensity preserves the palette, source tint, receiver roles and saturation');
  render();assert.equal(uploads.length,0,'high intensity does not add recurring uploads');
}
api.RoomPreview_EnableEffects(0);render();assert.deepEqual(texture.pixels,original);

// Click inversion is checked against the compositor's actual submitted OBJ
// vertices across aspect, zoom, orbit and capture budgets (no second camera).
assert.equal(api.RoomPreview_SetReceivers(1,384,352,448,352),1);
assert.equal(api.RoomPreview_ReceiverStyle(0,1),1);
for(const [newExtra,newVertical] of [[0,0],[120,32],[128,64]]) {
 extra=newExtra;vertical=newVertical;
 for(const view of [[960,600,1,0,0,1,0],[960,540,1.4,.3,-.2,0,1],[800,600,.8,-.25,.2,2,0],
     [960,411,1,.1,-.1,1,0],[960,270,1,0,0,1,1],[960,402,1.2,-.2,.15,2,0],
     [540,960,1,.1,0,1,0]]) {
  render(view);
  const mesh=draws.find(d=>d.id===spriteId&&d.effect===1);assert(mesh);
  let checks=0;
  for(let i=0;i<mesh.v.length;i+=8) {
    const [sx,sy]=mesh.v.subarray(i,i+2),u=mesh.v[i+6],v=mesh.v[i+7];
    // Published mesh includes guard edges; pick within a useful editor range.
    const wx=u*texture.w-extra-64+camera[0],wy=v*texture.h+camera[1]-vertical;
    if(wx<0||wx>400||wy<160||wy>500)continue;
    assert.equal(api.RoomPreview_PickReceiver(sx,sy),2);
    const picked=[...new Int32Array(api.memory.buffer,api.DioramaPreview_Input(),2)];
    assert(Math.abs(picked[0]-wx)<1.1&&Math.abs(picked[1]-wy)<1.1,`pick ${picked} vs ${wx},${wy}`);
    checks++;
  }
  assert(checks>2,'multiple projected positions verified');
 }
}
extra=120;vertical=64;
assert.equal(api.RoomPreview_PickReceiver(NaN,0),0);
assert.equal(api.RoomPreview_ReceiverStyle(2,0),0);
assert.equal(api.RoomPreview_SetReceivers(4,0,0,0,0),0);
assert.equal(api.RoomPreview_ReceiverStyle(0,0),1);
assert.equal(api.RoomPreview_SetReceivers(1,128,352,192,352),1);render();
assert.equal(api.RoomPreview_SetReceivers(1,144,352,192,352),1);render();
assert(uploads.filter(u=>u.id===spriteId).every(u=>u.w<100&&u.h<=64),'movement uploads just old/new sprite bounds');
assert.equal(api.RoomPreview_SetReceivers(1,128,352,192,352),1);
assert.equal(api.RoomPreview_ReceiverStyle(1,0),1);render();
const mirrored=texture.pixels.slice();
// Mirror about the same captured native hot point, independent of camera.
const mirrorX=128+extra+64-camera[0];
for(let y=0;y<texture.h;y++)for(let x=mirrorX-32;x<mirrorX+32;x++)
  assert.equal(mirrored[y*texture.w+x],original[y*texture.w+2*mirrorX-1-x]);
assert.equal(api.RoomPreview_SetReceivers(1,5000,5000,0,0),1);render();
assert(!draws.some(d=>d.id===spriteId),'offscreen reference emits no OBJ draw');
// A finite edge stops scanout and projection together. Real world pixels,
// sprite picking and upload caching must all use that same camera position.
assert.equal(send(Buffer.from('[layers:03:03]\n'),api.RoomPreview_Configure),1);
assert.equal(api.RoomPreview_ReceiverStyle(0,1),1);
let edgeViews=0,fittedViews=0;
for(const h of [720,600,540,402,270])for(const distance of [.8,1,1.25])
for(const tilted of [false,true])for(const margin of [0,120]) {
  const view=[960,h,distance,tilted?.12:0,tilted?-.1:0,1,0];
  extra=margin;vertical=tilted?32:64;
  camera=[1024,160];
  assert.equal(api.RoomPreview_SetReceivers(1,1152,352,0,0),1);render(view);
  const center=draws.find(d=>d.id===spriteId&&d.effect===1);assert(center);
  camera[0]=1792;
  assert.equal(api.RoomPreview_SetReceivers(1,1920,352,0,0),1);render(view);
  const stopped=api.RoomPreview_CameraX();
  assert(stopped<=1792&&stopped>1024,'right limit stays within native travel');
  if(stopped<1792)fittedViews++;
  const edge=draws.find(d=>d.id===spriteId&&d.effect===1);assert(edge);
  assert.equal(edge.v.length,center.v.length);
  assert(edge.v.every((v,i)=>Math.abs(v-center.v[i])<.002),'camera travel cannot change sprite scale or projection');
  const hash=api.RoomPreview_Hash(),allocations=creates;
  camera[0]=stopped+1;render(view);
  assert.equal(api.RoomPreview_CameraX(),stopped);
  assert.equal(api.RoomPreview_Hash(),hash,'overscroll reuses the exact same capture');
  assert.equal(uploads.length,0,'overscroll does not upload unchanged surfaces');
  assert.equal(creates,allocations);
  assert.deepEqual(draws.find(d=>d.id===spriteId&&d.effect===1).v,edge.v);
  // Pick a mesh point using the STOPPED origin, not the requested overscroll.
  let pickedPoint=false;
  for(let i=0;i<edge.v.length;i+=8) {
    const [sx,sy]=edge.v.subarray(i,i+2),u=edge.v[i+6],v=edge.v[i+7];
    const wx=u*texture.w-extra-64+stopped,wy=v*texture.h+camera[1]-vertical;
    if(wx<stopped||wx>stopped+256||wy<180||wy>350)continue;
    assert.equal(api.RoomPreview_PickReceiver(sx,sy),2);
    const picked=[...new Int32Array(api.memory.buffer,api.DioramaPreview_Input(),2)];
    assert(Math.abs(picked[0]-wx)<1.1&&Math.abs(picked[1]-wy)<1.1);
    pickedPoint=true;break;
  }
  assert(pickedPoint,'stopped-view picking checked');
  camera[0]=0;assert.equal(api.RoomPreview_SetReceivers(1,128,352,0,0),1);render(view);
  const leftStop=api.RoomPreview_CameraX();assert(leftStop>=0);
  const leftHash=api.RoomPreview_Hash();camera[0]=Math.max(0,leftStop-1);render(view);
  assert.equal(api.RoomPreview_CameraX(),leftStop);assert.equal(api.RoomPreview_Hash(),leftHash);
  assert.equal(uploads.length,0);
  edgeViews++;
}
assert(fittedViews>0,'wide projected views stop before the native edge');
// Added scenery supplies the margin; empty editor workspace alone does not.
extra=120;vertical=64;camera=[1792,160];
const edgeConfig=stamps=>'[layers:03:03]\nbg1-map:us+jp+eu = bounds:-20,-6,148,36\n'+stamps;
assert.equal(send(Buffer.from(edgeConfig('')),api.RoomPreview_Configure),1);
render();assert(api.RoomPreview_CameraX()<1792,'empty bounds do not enlarge scenery');
const stamp=x=>`bg1-stamp:us+jp+eu = cell:${x},0 metatile:0A words:144C,144D,145C,145D bands:1,1,1,1\n`;
assert.equal(send(Buffer.from(edgeConfig(stamp(-20)+stamp(147))),api.RoomPreview_Configure),1);
render();assert.equal(api.RoomPreview_CameraX(),1792,'authored right-side scenery extends travel');
camera[0]=0;render();assert.equal(api.RoomPreview_CameraX(),0,'authored left-side scenery extends travel');
console.log(`Shared camera: ${edgeViews} aspect/zoom/tilt/capture combinations stop without zoom, drift or uploads.`);
// Bloodpool's painted boss room extends above and below the native map, but
// only 352 rows fit in the capture. Width/CRT fitting must not expose that
// buffer boundary while the authored world continues beyond it.
const bossRoom=data.rooms.find(r=>r.group===2&&r.map===8);
assert.equal(send(encode(data,blobs,bossRoom),api.RoomPreview_Load),1);
const bossConfig='[layers:02:08]\nbg1-policy = edge:world\n'+
  'bg1-map:us+jp+eu = bounds:-10,-6,26,20\n'+
  [[-10,-6],[25,-6],[-10,19],[25,19]].map(([x,y])=>
    `bg1-stamp:us+jp+eu = cell:${x},${y} metatile:08 words:1037,1038,1047,1048 bands:1,1,1,1\n`).join('');
assert.equal(send(Buffer.from(bossConfig),api.RoomPreview_Configure),1);
api.RoomPreview_EnableEffects(0);
assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
function sceneryEdges() {
  // Inspect the production plane mesh, after its supersampling pass. The
  // skybox is a quad; the actual playfield is the subdivided texture mesh.
  const d=draws.find(d=>!d.target&&d.v.length>32&&textures.get(d.id)?.h>352);
  assert(d,'playfield face submitted');
  let top=-Infinity,bottom=Infinity;
  for(let i=0;i<d.v.length;i+=8) {
    if(d.v[i+7]===0)top=Math.max(top,d.v[i+1]);
    if(d.v[i+7]===1)bottom=Math.min(bottom,d.v[i+1]);
  }
  assert(Number.isFinite(top)&&Number.isFinite(bottom));
  return {top,bottom,span:bottom-top};
}
extra=120;
let captureViews=0;
for(const h of [720,600,540,402,270])for(const pixel of [0,1])for(const rows of [32,64]) {
  vertical=rows;
  let span;
  for(const y of [0,1,15,31]) {
    camera=[0,y];render([960,h,1,0,0,1,pixel]);
    const edge=sceneryEdges();
    assert(edge.top<=.001&&edge.bottom>=h-.001,
      `capture covers ${960}x${h}, CRT ${pixel}, rows ${rows}, camera ${y}: ${JSON.stringify(edge)}`);
    if(span!==undefined)assert(Math.abs(edge.span-span)<.002,'vertical travel must not change scale');
    span=edge.span;
  }
  const allocated=creates;
  render([960,h,1,0,0,1,pixel]);
  assert.equal(uploads.length,0);assert.equal(creates,allocated);
  captureViews++;
}
camera=[0,0];vertical=64;
render([960,600,1.25,0,0,1,1]);
assert(sceneryEdges().top>0,'deliberate zoom-out can reveal capture edges');
assert.equal(send(Buffer.from('[layers:02:08]\nbg1-policy = edge:world\n'),api.RoomPreview_Configure),1);
let shortSpan;
for(const y of [0,15,31]) {
  camera=[0,y];render([960,600,1,0,0,1,1]);
  const edge=sceneryEdges();
  assert(edge.top>0&&edge.bottom<600,'fully captured short rooms retain their framing');
  if(shortSpan!==undefined)assert(Math.abs(edge.span-shortSpan)<.002);
  shortSpan=edge.span;
}
console.log(`Shared camera: ${captureViews} aspect/pixel/row combinations cover the capture without changing scale during travel.`);
// The production moon must light materials in plane, skybox-only and combined
// presentations. Isolate it from the existing wet-water/timber light passes.
const bloodpool=data.rooms.find(r=>r.group===2&&r.map===1);
assert.equal(send(encode(data,blobs,bloodpool),api.RoomPreview_Load),1);
assert.equal(send(Buffer.from('[layers:02:01]\n'),api.RoomPreview_Configure),1);
assert.equal(send(art,api.RoomPreview_LoadPlayer),1);
const moonOnly=noEffects+'[source:02:01:0:blood-water:b1000000]\nenabled=0\n'+
  '[source:02:01:0:wet-timber:b1000004]\nenabled=0\n';
camera=[512,256];extra=120;vertical=64;api.RoomPreview_EnableEffects(1);
assert.equal(api.RoomPreview_ReceiverStyle(0,1),1);
for(const mode of [0,1,2]) {
  configure(moonOnly);
  assert.equal(api.RoomPreview_SetReceivers(3,640,400,720,400),1);
  render([960,600,1,0,0,mode,0]);
  assert(draws.some(d=>d.target&&d.id===0),'native moon produces a material light map even without a BG2 plane');
  const receiverDraw=draws.find(d=>d.effect===1&&d.id);
  assert(receiverDraw,'visible native moonlight receivers submitted');
  const t=textures.get(receiverDraw.id),bright=t.pixels.slice();
  configure(moonOnly+'[source:02:01:0:moonlight:b1000002]\nillumination=0\n');
  render([960,600,1,0,0,mode,0]);
  assert(bright.some((p,i)=>p!==t.pixels[i]),'moon calibration reaches actual sprite palettes');
}
console.log('Native moon: material illumination and sprite receivers work with plane, skybox-only and combined backgrounds.');
// The lower soft light in Kassandora 2/5 previously vanished until its centre
// entered the capture at camera Y=1137, despite already lighting visible rows.
const softRoom=data.rooms.find(r=>r.group===3&&r.map===5);
assert.equal(send(encode(data,blobs,softRoom),api.RoomPreview_Load),1);
assert.equal(send(Buffer.from('[layers:03:05]\nbg1-policy = edge:world\n'),api.RoomPreview_Configure),1);
configure(noEffects+'[emitter:03:05:0:soft-light:1]\nx=520\ny=1424\nwidth=512\nheight=224\n'+
  'color=00faab\nintensity=1.3\nplacement=playfield\nlight-scenery=1\n');
api.RoomPreview_EnableEffects(1);extra=120;vertical=64;
for(const view of [[960,600,1,0,0,1,0],[960,540,1,0,0,0,0],[800,600,1.2,.15,-.1,2,0]])
 for(const y of [1080,1100,1136,1140,1190]) {
  camera=[392,y];render(view);
  const visible=draws.some(d=>d.id===0&&d.target&&Array.from({length:d.v.length/8},(_,i)=>i*8)
    .some(i=>d.v[i+2]===0&&d.v[i+3]>0&&Math.abs(d.v[i+3]/d.v[i+4]-250/171)<.001&&d.v[i+5]>.001));
  assert(visible,`soft light must cover the visible floor before its centre enters: ${view} y=${y}`);
 }
console.log('Kassandora 2/5 soft light: continuous partial visibility across camera-edge entry, three aspects, tilt and background modes.');
assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);api.RoomPreview_Reset();
assert.equal(textures.size,0,'room reset releases all preview textures');
console.log('Shared player preview: native art, receiver masks, dimming/torch/rim, picking, mirroring and upload reuse passed.');
