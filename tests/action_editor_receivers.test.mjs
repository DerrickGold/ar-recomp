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
let api,target=0,effect=-1,creates=0,effectParams=null,viewport=[0,0,0,0];
const textures=new Map(),uploads=[],draws=[];
const ar={
  create(w,h,usage){let id=1;while(textures.has(id))id++;
    textures.set(id,{w,h,usage,pixels:new Uint32Array(w*h)});creates++;return id;},
  destroy(id){assert(textures.delete(id));},
  update(id,x,y,w,h,p,pitch){const t=textures.get(id);assert(t&&x>=0&&y>=0&&x+w<=t.w&&y+h<=t.h);
    for(let row=0;row<h;row++)t.pixels.set(new Uint32Array(api.memory.buffer,p+row*pitch,w),(y+row)*t.w+x);
    uploads.push({id,x,y,w,h});return 1;},
  target(id){target=id;return 1;},viewport(set,x,y,w,h){viewport=set?[x,y,w,h]:[0,0,0,0];return 1;},clip(){return 1;},clear(){return 1;},
  geometry(id,p,n,ip,ni){assert(!id||(textures.has(id)&&id!==target));
    const v=new Float32Array(api.memory.buffer,p,n*8).slice();assert(v.every(Number.isFinite));
    draws.push({id,effect,target,viewport:[...viewport],params:effectParams,v,indices:new Int32Array(api.memory.buffer,ip,ni).slice()});return 1;},
  effect(kind,p,n){if(kind>=0){assert.equal(effect,-1);assert(new Float32Array(api.memory.buffer,p,n).every(Number.isFinite));}
    effect=kind;effectParams=kind>=0?new Float32Array(api.memory.buffer,p,n).slice():null;return 1;},available(){return 1;}
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
// Region handles serialize complete fields. Exercise the actual C validator,
// rather than the editor unit test's permissive configuration stub.
vm.runInContext(fs.readFileSync('tools/action_editor/effect_regions.js','utf8'),context);
const regions=vm.runInContext('EffectRegions',context);
for(const [kind,file,width,height] of [
  ['castle-mist','castle-6',400,50],['castle-sky','castle-6',80,60],
  ['temple-dust','cave-atmosphere',600,300],['cave-light','cave-atmosphere',360,520],
  ['moon-cloud','moon',200,48],
  ['blood-mist','marsh',640,60],['blood-water','marsh',640,24]
]) {
  const text=fs.readFileSync(`assets/effects/${file}-field.ini`,'utf8');
  const record=Object.fromEntries(text.split('\n').filter(l=>l.includes('=')).map(l=>l.split('=')));
  const range=regions.limits(kind,record,0),sizes=[{width,height}];
  if(['castle-mist','castle-sky','moon-cloud','blood-mist','blood-water'].includes(kind))
    sizes.push({width:kind==='castle-mist'?width:range.maxWidth,height:range.maxHeight});
  for(const size of sizes) {
    const updated=regions.patch(kind,record,0,size);
    if(kind==='blood-water')assert(!('surface' in updated),'shoreline width edits preserve the native water band');
    const exported=text.split('\n').map(line=>{
      const key=line.split('=')[0];return key in updated?`${key}=${updated[key]}`:line;
    }).join('\n');
    assert.equal(send(Buffer.from(exported),api.RoomPreview_ConfigureEffects),1,
      `${kind} resized field ${JSON.stringify(size)} must be accepted by native/WASM: line ${api.RoomPreview_RecipeErrorLine()}`);
  }
}
console.log('Region handles: resized complete fields pass the native/WASM configuration contract.');
configure(noEffects);
let camera=[256,160],extra=120,vertical=64,baselineDistance=0;
function render(view=[960,600,1,0,0,1,0],tick=37) {
  assert.equal(api.RoomPreview_SetView(baselineDistance,0,0,2,1),1);
  draws.length=uploads.length=0;
  assert.equal(api.RoomPreview_Render(...camera,tick,extra,vertical,...view),1);
  assert.equal(effect,-1,'shader bindings restored');
}
api.RoomPreview_EnableEffects(0);render();
const prior=new Set(textures.keys());
assert.equal(api.RoomPreview_SetReceivers(1,128,352,192,352),1);
render();
// The first OBJ may also grow a compositor scratch target: BG guard columns
// are conditional, whereas the actor surface always retains its apron.
const added=[...textures.keys()].filter(id=>!prior.has(id)&&textures.get(id).w===640&&textures.get(id).h===352);
assert.equal(added.length,2);
const spriteId=added.find(id=>new Set(textures.get(id).pixels).size>10),texture=textures.get(spriteId);
const attributeId=added.find(id=>id!==spriteId),attributes=textures.get(attributeId);
function receiverDraw() {const d=draws.find(d=>d.effect===5&&d.params[16]===attributeId);assert(d,'projected actor material bound');return d;}
function energy(id) {
  return draws.filter(d=>d.target===id&&!d.id).reduce((sum,d)=>{
    for(let i=0;i<d.v.length;i+=8)sum+=(d.v[i+2]+d.v[i+3]+d.v[i+4])*d.v[i+5];
    return sum;
  },0);
}
function roleEnergy(d,role) {return energy(d.params[17+role]);}
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
assert.deepEqual(texture.pixels,original,'source palette stays intact for projected lighting');
for(let i=0;i<original.length;i++)if(original[i])assert.equal(attributes.pixels[i],0xff10ff00);
assert.equal(receiverDraw().params[16],attributeId,'winning receiver attributes reach the material');
assert.equal(api.RoomPreview_ReceiverStyle(0,1),1);render();
assert(receiverDraw().params[3]>0,'rim is composed after material illumination');
assert.equal(uploads.length,0,'rim toggle reuses captured data');
assert.equal(api.RoomPreview_ReceiverStyle(0,0),1);

// Separate role/ambient data for adjacent identical art, without baking light
// into the shared palette. Native GPU tests verify the final shader pixels.
assert.equal(api.RoomPreview_SetReceivers(3,128,352,192,352),1);render();
for(let i=0;i<original.length;i++)if(original[i]) {
  assert.equal(attributes.pixels[i],0xff10ff00);
  assert.equal(attributes.pixels[i+64],0xff4000ff);
}
configure(exposure('dim-player=0\ndim-enemies=1\n'));render();
for(let i=0;i<original.length;i++)if(original[i]) {
  assert.equal(attributes.pixels[i],0xff40ff00);
  assert.equal(attributes.pixels[i+64],0xff1000ff);
}
assert.equal(api.RoomPreview_SetReceivers(1,128,352,192,352),1);
const torch=targets=>noEffects+'[emitter:03:03:0:torch:00000002]\n'+
  'x=128\ny=320\nwidth=220\nheight=176\ncolor=ff6020\nlight-scenery=0\n'+targets;
configure(torch('light-player=1\nlight-enemies=0\n'));render();
assert.deepEqual(texture.pixels,original);
let d=receiverDraw(),torchEnergy=roleEnergy(d,0);
assert(torchEnergy>0&&roleEnergy(d,1)===0,'torch targets player map only');
configure(torch('light-player=0\nlight-enemies=1\n'));render();
d=receiverDraw();assert.equal(roleEnergy(d,0),0);assert.equal(roleEnergy(d,1),torchEnergy);
assert.equal(uploads.length,0,'changing light targets does not reupload sprite artwork or attributes');
assert.equal(api.RoomPreview_SetReceivers(2,128,352,128,352),1);render();
for(let i=0;i<original.length;i++)if(original[i])assert.equal(attributes.pixels[i],0xff4000ff);

// Moving/intensifying light updates the projected field, not the sprite
// hotpoint. Identical receiver masks share one target instead of two passes.
const fill=(color,intensity=4)=>'[emitter:03:03:0:light-gradient:00000003]\n'+
  `x=128\ny=320\nwidth=1024\nheight=1024\nsoftness=0\nillumination=1\nintensity=${intensity}\n`+
  `color=${color}\ncolor-end=${color}\nlight-scenery=0\nlight-player=1\nlight-enemies=1\n`;
const darkRoom=exposure('dim-player=1\ndim-enemies=1\n').replace('brightness=.25','brightness=.28');
let lastEnergy=0;
for(const intensity of [4,8,16]) {
  configure(darkRoom+fill('ff8040',intensity));render();d=receiverDraw();
  assert.equal(d.params[17],d.params[18],'identical player/enemy fields reuse one target');
  const e=roleEnergy(d,0);assert(e>lastEnergy);lastEnergy=e;
  assert.deepEqual(texture.pixels,original,'all incoming intensities preserve original palette');
  for(let i=0;i<original.length;i++)if(original[i])assert.equal(attributes.pixels[i],0xff1200ff);
  render();assert.equal(uploads.length,0,'animated lighting needs no receiver reuploads');
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
    const sx=mesh.v[i]+mesh.viewport[0],sy=mesh.v[i+1]+mesh.viewport[1],u=mesh.v[i+6],v=mesh.v[i+7];
    // Published mesh includes guard edges; pick within a useful editor range.
    const wx=u*texture.w-extra-64+api.RoomPreview_CameraX(),wy=v*texture.h+camera[1]-vertical;
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
  const offset=[edge.v[0]-center.v[0],edge.v[1]-center.v[1]];
  assert(edge.v.every((v,i)=>Math.abs(v-center.v[i]-(i%8<2?offset[i%8]:0))<.002),
    'camera travel cannot change sprite scale; manual zoom may translate at the world edge');
  if(distance===1&&!tilted)assert(offset.every(v=>Math.abs(v)<.002),'baseline framing stays fixed at the stop');
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
    const sx=edge.v[i]+edge.viewport[0],sy=edge.v[i+1]+edge.viewport[1],u=edge.v[i+6],v=edge.v[i+7];
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
render([960,600,2,0,0,1,1]);
assert(sceneryEdges().top>0,'deliberate zoom-out can reveal capture edges');
assert.equal(send(Buffer.from('[layers:02:08]\nbg1-policy = edge:world\n'),api.RoomPreview_Configure),1);
let shortSpan;baselineDistance=3.25;
for(const y of [0,15,31]) {
  camera=[0,y];render([960,600,1,0,0,1,1]);
  const edge=sceneryEdges();
  assert(edge.top>0&&edge.bottom<600,'fully captured short rooms retain their framing');
  if(shortSpan!==undefined)assert(Math.abs(edge.span-shortSpan)<.002);
  shortSpan=edge.span;
}
baselineDistance=0;
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
  const moonDraw=draws.find(d=>d.effect===5&&d.id);
  assert(moonDraw,'visible native moonlight receivers submitted');
  const bright=roleEnergy(moonDraw,0);assert(bright>0);
  configure(moonOnly+'[source:02:01:0:moonlight:b1000002]\nillumination=0\n');
  render([960,600,1,0,0,mode,0]);
  const unlit=draws.find(d=>d.effect===5&&d.id);assert(unlit);
  assert(bright>roleEnergy(unlit,0),'moon calibration reaches the projected sprite light field');
}
console.log('Native moon: material illumination and sprite receivers work with plane, skybox-only and combined backgrounds.');
// Native forest rays use the same projected field for both receiver roles,
// including the vertical descent into the boss pit and skybox-only mode.
const fillmore=data.rooms.find(r=>r.group===1&&r.map===1);
assert.equal(send(encode(data,blobs,fillmore),api.RoomPreview_Load),1);
assert.equal(send(Buffer.from('[layers:01:01]\n'),api.RoomPreview_Configure),1);
assert.equal(send(art,api.RoomPreview_LoadPlayer),1);
const noForestLight=noEffects+'[source:01:01:0:forest-forward:46000000]\nillumination=0\n';
for(const cam of [[0,0],[3584,384]])for(const mode of [0,1,2]) {
  camera=cam;
  configure(noEffects);
  assert.equal(api.RoomPreview_SetReceivers(3,cam[0]+128,cam[1]+280,cam[0]+192,cam[1]+280),1);
  render([960,600,1,0,0,mode,0]);
  const lit=draws.find(d=>d.effect===5&&d.id);assert(lit);
  assert.equal(lit.params[17],lit.params[18],'forest roles share the same projected illumination');
  const litEnergy=roleEnergy(lit,0);assert(litEnergy>0);
  configure(noForestLight);render([960,600,1,0,0,mode,0]);
  const unlit=draws.find(d=>d.effect===5&&d.id);assert(unlit);
  assert(litEnergy>roleEnergy(unlit,0),'forest brightness reaches the material field, not baked sprite pixels');
}
console.log('Native forest: projected player/enemy illumination follows boss descent in all background modes.');
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
// The optional final backdrop shares the live renderer and must not alter the
// player stand-in, camera projection, or the return to native room rendering.
const finalRoom=data.rooms.find(r=>r.group===7&&r.map===8);
assert.equal(send(encode(data,blobs,finalRoom),api.RoomPreview_Load),1);
assert.equal(send(Buffer.from('[layers:07:08]\n'),api.RoomPreview_Configure),1);
assert.equal(send(art,api.RoomPreview_LoadPlayer),1);
api.RoomPreview_EnableEffects(0);
assert.equal(api.RoomPreview_ReceiverStyle(0,0),1);
assert.equal(api.RoomPreview_SetReceivers(1,96,200,0,0),1);
camera=[0,0];baselineDistance=3.25;
for (const view of [[800,600,1,0,0,0,0],[960,540,1,0,0,1,0],[960,600,1.4,.3,-.2,2,0]]) {
  api.RoomPreview_FinalBattle(0);render(view);render(view);
  const native=structuredClone(draws);
  api.RoomPreview_FinalBattle(1);render(view);
  assert(draws.some(d=>d.v.length>=600*8&&d.indices.length>d.v.length/2),
    'shared preview draws indexed nebula geometry');
  const allocated=creates;render(view);
  assert.equal(creates,allocated,'retained rift preview creates no resources');
  assert.equal(uploads.length,0,'retained rift preview uploads no textures');
  const background=structuredClone(draws.filter(d=>d.indices.length>100));
  const nebula=structuredClone(draws.find(d=>d.indices.length===48*27*6));
  assert(nebula,'main nebula has its own retained surface');
  const uv=v=>v.filter((_,i)=>i%8>=6);
  const nebulaUV=uv(nebula.v);
  const floor=draws.find(d=>d.indices.length===48*10*6);
  assert(floor,'floor is projected with the scene');
  const floorArt=textures.get(floor.id);
  const alphaRow=fraction=>Array.from(floorArt.pixels.subarray(
    Math.floor(floorArt.h*fraction)*floorArt.w,(Math.floor(floorArt.h*fraction)+1)*floorArt.w),p=>p>>>24);
  assert(alphaRow(.68).reduce((a,b)=>a+b,0)/floorArt.w>240,'walking ledge stays opaque');
  assert(alphaRow(.78).filter(a=>a<128).length>floorArt.w/4,
    'dark underside cavities reveal the moving space beneath the rock');
  const position=v=>v.filter((_,i)=>i%8<2);
  const standingSurface=position(floor.v);
  render(view,157);
  const movingNebula=draws.find(d=>d.id===nebula.id);
  assert.deepEqual(position(movingNebula.v),position(nebula.v),
    'nebula currents move inside a fixed screen surface');
  const movedUV=uv(movingNebula.v);
  assert(Math.max(...movedUV.map((v,i)=>Math.abs(v-nebulaUV[i])))>.004,
    'main nebula must visibly flow within two seconds, not only its foreground wisps');
  assert(movedUV.every(v=>v>0&&v<1),'currents retain artwork coverage at every edge');
  render(view,600);
  assert.notDeepEqual(draws.filter(d=>d.indices.length>100),background,'celestial geometry animates');
  assert.deepEqual(position(draws.find(d=>d.id===floor.id).v),standingSurface,
    'floor energy must never move the standing surface');
  assert.equal(creates,allocated,'animation creates no resources');
  const pause=structuredClone(draws);render(view,600);
  assert.deepEqual(draws,pause,'same paused/scrubbed tick reproduces every draw');
  assert.equal(uploads.length,0,'paused animation uploads no textures');
  const environmentIds=new Set(draws.filter(d=>d.indices.length>=100&&d.effect===-1).map(d=>d.id));
  const environment=structuredClone(draws.filter(d=>environmentIds.has(d.id)&&d.effect===-1));
  assert.equal(api.RoomPreview_SetReceivers(1,192,144,0,0),1);render(view,600);
  assert.deepEqual(draws.filter(d=>environmentIds.has(d.id)&&d.effect===-1),environment,
    'ambient spectacle must run independently of character position');
  assert.equal(api.RoomPreview_SetReceivers(1,96,200,0,0),1);render(view,600);
  render(view,65535);
  const endUV=uv(draws.find(d=>d.id===nebula.id).v);
  render(view,0);
  assert(Math.max(...uv(draws.find(d=>d.id===nebula.id).v)
    .map((v,i)=>Math.abs(v-endUV[i])))<.001,'nebula remains continuous when its clock wraps');
  api.RoomPreview_FinalBattle(0);render(view);render(view);
  assert.deepEqual(draws,native,'disabling the rift restores identical native geometry and player');
}
console.log('Astral Rift: native fallback, player, aspect/orbit, and allocation/upload reuse passed.');
assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);api.RoomPreview_Reset();
assert.equal(textures.size,0,'room reset releases all preview textures');
console.log('Shared player preview: native art, receiver masks, dimming/torch/rim, picking, mirroring and upload reuse passed.');
