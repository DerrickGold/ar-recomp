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
const isEffectAtlas=id=>{const t=textures.get(id);return t?.w===1024&&t?.h===896;};
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
// Editor guides query the production layout; adding openings keeps existing
// anchors/widths, and all unsigned seed bits survive the JS/WASM boundary.
const openings=(seed,count)=>{
  const n=api.RoomPreview_CloudOpenings(seed,count);
  return [...new Float32Array(api.memory.buffer,api.DioramaPreview_Input(),n)];
};
const cluster=openings(512,4);
assert.equal(cluster.length,24);
assert.deepEqual(openings(512,2),cluster.slice(0,12));
assert(cluster[1]>cluster[7]*1.5,'broad and narrow cloud openings');
assert(Math.abs(cluster[6]-cluster[12])<.04,'clustered openings');
assert(Math.abs(cluster[0]-cluster[18])>.3,'quiet gap');
for(let i=0;i<cluster.length;i+=6)assert(cluster[i+5]>cluster[i+1], 'beam spreads beyond opening');
assert.notDeepEqual(openings(0xffffffff,4),openings(4095,4));
assert.deepEqual(openings(512,0),[]);assert.deepEqual(openings(512,6),[]);
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
function maskAlpha(t,u,v) {
  const x=Math.max(0,Math.min(t.w-1,u*t.w-.5)),y=Math.max(0,Math.min(t.h-1,v*t.h-.5));
  const ix=Math.floor(x),iy=Math.floor(y),nx=Math.min(ix+1,t.w-1),ny=Math.min(iy+1,t.h-1);
  const a=x-ix,b=y-iy,p=(xx,yy)=>(t.pixels[yy*t.w+xx]>>>24)/255;
  return (p(ix,iy)*(1-a)+p(nx,iy)*a)*(1-b)+(p(ix,ny)*(1-a)+p(nx,ny)*a)*b;
}
function energy(id) {
  return draws.filter(d=>d.target===id&&(!d.id||isEffectAtlas(d.id))).reduce((sum,d)=>{
    if(!d.id) {
      for(let i=0;i<d.v.length;i+=8)sum+=(d.v[i+2]+d.v[i+3]+d.v[i+4])*d.v[i+5];
      return sum;
    }
    // Textured caustics share a light batch with ordinary glows. Include the
    // uploaded mask, not just vertex alpha or an untextured-draw count.
    const t=textures.get(d.id),weights=[[1/3,1/3,1/3],[.6,.2,.2],[.2,.6,.2],[.2,.2,.6],
      [.8,.1,.1],[.1,.8,.1],[.1,.1,.8]];
    for(let i=0;i<d.indices.length;i+=3)for(const w of weights) {
      let rgb=0,alpha=0,u=0,v=0;
      for(let j=0;j<3;j++) {
        const k=d.indices[i+j]*8;
        rgb+=(d.v[k+2]+d.v[k+3]+d.v[k+4])*w[j];alpha+=d.v[k+5]*w[j];
        u+=d.v[k+6]*w[j];v+=d.v[k+7]*w[j];
      }
      sum+=rgb*alpha*maskAlpha(t,u,v)/weights.length;
    }
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
// The final nebula follows the same environmental master as other rooms.
// Toggling it preserves the player stand-in, projection and original artwork.
assert.equal(api.RoomPreview_FinalBattle,undefined);
const finalRoom=data.rooms.find(r=>r.group===7&&r.map===8);
assert.equal(send(encode(data,blobs,finalRoom),api.RoomPreview_Load),1);
assert.equal(send(Buffer.from('[layers:07:08]\n'),api.RoomPreview_Configure),1);
assert.equal(send(art,api.RoomPreview_LoadPlayer),1);
api.RoomPreview_EnableEffects(0);
assert.equal(api.RoomPreview_ReceiverStyle(0,0),1);
assert.equal(api.RoomPreview_SetReceivers(1,96,200,0,0),1);
camera=[0,0];baselineDistance=3.25;
for (const view of [[800,600,1,0,0,0,0],[960,540,1,0,0,1,0],[960,600,1.4,.3,-.2,2,0]]) {
  api.RoomPreview_EnableEffects(0);render(view);render(view);
  const native=structuredClone(draws);
  api.RoomPreview_EnableEffects(1);render(view);
  assert(draws.some(d=>d.effect===6&&d.params[3]===0),'extract light from native art');
  assert(draws.some(d=>d.effect===6&&d.params[3]===1),'separable glow filter');
  assert(draws.some(d=>d.effect===6&&d.params[3]===2),'original art gets its colour grade');
  const filtered=draws.filter(d=>d.effect===6&&d.params[3]<2);
  for(const d of filtered){const t=textures.get(d.target);assert(t.w<=640&&t.h<=360,'bounded radiance target');}
  const uploadedArt=new Set(native.filter(d=>d.id&&textures.get(d.id)?.usage!==2).map(d=>d.id));
  assert(draws.some(d=>uploadedArt.has(d.id)),'native artwork remains in use');
  const allocated=creates;render(view);
  assert.equal(creates,allocated,'retained nebula preview creates no resources');
  assert.equal(uploads.length,0,'retained nebula preview uploads no textures');
  const pause=structuredClone(draws);render(view);
  assert.deepEqual(draws,pause,'same paused tick reproduces every draw');
  api.RoomPreview_EnableEffects(0);render(view);render(view);
  assert.deepEqual(draws,native,'disabling environmental effects restores identical native geometry and player');
}
console.log('Native nebula: original art, bounded filtering, native fallback, aspect/orbit and resource reuse passed.');
// Cloud density is baked once; only a small shared mesh moves each tick. Its
// pause/seek clock and independent disable must not alter native geometry.
const dustDraw=()=>draws.filter(d=>textures.get(d.id)?.w===576&&textures.get(d.id)?.h===96);
const dustConfig=(dust,speed=1)=>`[layers:07:08]\nnative-bloom = strength:.85 radius:13 threshold:.24 saturation:1.08 contrast:1.05 light:1.3 receivers:6 dust:${dust} dust-scale:1 dust-speed:${speed} dust-blue:467ccf dust-rose:bc438b\n`;
const setDust=(density,speed)=>assert.equal(send(Buffer.from(dustConfig(density,speed)),api.RoomPreview_Configure),1);
api.RoomPreview_EnableEffects(1);setDust(.3);
render(undefined,1200);assert.equal(dustDraw().length,1,'six veils share one draw');
const dustTexture=dustDraw()[0].id,dustMesh=structuredClone(dustDraw());
assert.equal(dustMesh[0].v.length,210*8);
const dustResources=creates;
render(undefined,1500);assert.notDeepEqual(dustDraw(),dustMesh,'wisps drift and shear with game time');
assert.equal(creates,dustResources);assert(!uploads.some(u=>u.id===dustTexture),'animated density stays resident');
render(undefined,1200);assert.deepEqual(dustDraw(),dustMesh,'scrubbing backwards restores exact dust placement');
render(undefined,1200);assert.deepEqual(dustDraw(),dustMesh,'pause freezes all dust motion');
render(undefined,65535);const beforeWrap=dustDraw()[0].v.slice();
render(undefined,65536);const afterWrap=dustDraw()[0].v;
for(let i=0;i<beforeWrap.length;i+=8)for(const axis of [0,1])
  assert(Math.abs(beforeWrap[i+axis]-afterWrap[i+axis])<1,'scene clock wrap remains a small animation step');
setDust(.3,0);render(undefined,1200);const frozenDust=structuredClone(dustDraw());
render(undefined,1500);assert.deepEqual(dustDraw(),frozenDust,'zero speed holds the mesh still');
setDust(0);render();assert.equal(dustDraw().length,0);
assert(draws.some(d=>d.effect===6),'dust disable preserves native bloom');
console.log('Nebula dust: one retained batch, animated/paused/seeking clocks and independent controls passed.');
// The full editor intensity range must keep the enhancement active. Its
// output gain cannot be encoded in normalized texture/vertex colour.
for(const strength of [0,.8,1.5,3]) {
  assert.equal(send(Buffer.from(`[layers:07:08]\nnative-bloom = strength:${strength} radius:13 threshold:0.22 saturation:1.08 contrast:1.05 light:1.3 receivers:6\n`),api.RoomPreview_Configure),1);
  api.RoomPreview_EnableEffects(1);render();
  const glow=draws.find(d=>d.effect===6&&d.params[3]===3);
  assert(glow,`native glow remains active at strength ${strength}`);
  assert(Math.abs(glow.params[6]-strength)<.0001,'gain travels through the shared shader ABI');
  for(let i=0;i<glow.v.length;i+=8) for(let c=2;c<6;c++)
    assert(glow.v[i+c]>=0&&glow.v[i+c]<=1,'texture tint remains normalized');
}
api.RoomPreview_EnableEffects(0);
// Palette-only scenery uses a retained black texel, not another full-screen
// illumination pass. The same grade travels through the real WASM ABI.
const gradeRoom=data.rooms.find(r=>r.group===5&&r.map===1);
assert.equal(send(encode(data,blobs,gradeRoom),api.RoomPreview_Load),1);
configure(noEffects);api.RoomPreview_EnableEffects(1);
assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
camera=[128,128];
assert.equal(send(Buffer.from('[layers:05:01]\nscenery-grade = strength:0.48 saturation:1.25 shadows:34675e highlights:d9d38b\n'),api.RoomPreview_Configure),1);
render([960,540,1,0,0,0,0]);
const gradeDraws=draws.filter(d=>d.effect===4);
assert(gradeDraws.length>0,'palette-only scenery binds the material shader');
for(const draw of gradeDraws) {
 assert.equal(draw.params.length,177);assert(Math.abs(draw.params[16]-.48)<.0001);
 assert.equal(draw.params[17],1.25);
 assert(Math.abs(draw.params[20]-0x34/255)<.0001);
 const map=textures.get(draw.params[176]);assert(map&&map.w===1&&map.h===1,'palette-only map is one black texel');
 assert.equal(map.pixels[0],0xff000000);
}
const gradeAllocations=creates,gradeDrawCount=draws.length;
render([960,540,1,0,0,0,0]);
assert.equal(creates,gradeAllocations);assert.equal(uploads.length,0);assert.equal(draws.length,gradeDrawCount);
api.RoomPreview_EnableEffects(0);render([960,540,1,0,0,0,0]);
assert(!draws.some(d=>d.effect===4),'environment toggle restores ungraded materials');
console.log('Scenery palette: shader uniforms, single-texel reuse, no extra light-map draw, and disable passed.');
// BG1 heat travels through the same material ABI for every scenery priority,
// without adding a full-frame pass or changing actor UVs. Scrubbing the camera
// preserves the region's world coordinates, including an offscreen centre.
assert.equal(send(Buffer.from('[layers:05:01]\n'),api.RoomPreview_Configure),1);
api.RoomPreview_EnableEffects(1);
configure(noEffects+'[emitter:05:01:0:heat-haze:1]\nanchor=bg1\nx=300\ny=260\nwidth=140\nheight=100\namplitude=2\n');
camera=[128,128];render([960,540,1,0,0,0,0]);
const heatDraws=draws.filter(d=>d.effect===4&&d.params[34]>0);
assert(heatDraws.length>0,'heat-only scenery binds its shared material');
const heatDrawCount=draws.length,heatAllocations=creates;
for(const d of heatDraws) {
 assert.deepEqual([...d.params.slice(36,40)],[230,210,370,310]);
 assert.equal(d.params[52],2);
 const map=textures.get(d.params[176]);assert(map.w===1&&map.h===1,'heat-only room reuses one black texel');
}
camera=[160,144];render([960,540,1,0,0,0,0]);
assert.equal(creates,heatAllocations);assert.equal(draws.length,heatDrawCount);
for(const d of draws.filter(d=>d.effect===4&&d.params[34]>0))
 assert.deepEqual([...d.params.slice(36,40)],[230,210,370,310]);
api.RoomPreview_EnableEffects(0);render([960,540,1,0,0,0,0]);
assert(!draws.some(d=>d.effect===4&&d.params[34]>0),'environment switch removes heat');
configure(noEffects);
console.log('Scenery heat: shared WASM material, world-stable regions, disable and no extra pass passed.');
// The real player/enemy stand-ins feed the same bounded body-wake observer.
api.RoomPreview_EnableEffects(1);
configure('[effects]\nversion=1\n[emitter:05:01:0:cloud-bank:1]\nx=128\ny=224\nwidth=512\nheight=128\ntextured=1\nrolling=.8\nwake=.8\nstrands=3\ncolor=b9b0aa\ncolor-end=aaa4af\n');
camera=[0,96];
const mistCount=()=>draws.filter(d=>d.effect===-1&&isEffectAtlas(d.id)&&textures.get(d.id)?.pixels[0]===0xffffffff).reduce((n,d)=>n+d.v.length/8,0);
assert.equal(api.RoomPreview_SetReceivers(3,100,240,180,240),1);render(undefined,2000);
const mistBase=mistCount();assert(mistBase>0,'rolling density masks submitted in the shared renderer');
assert.equal(api.RoomPreview_SetReceivers(3,103,240,177,240),1);render(undefined,2001);
assert.equal(mistCount(),mistBase+8,'moving player and enemy each add a bounded wake');
render(undefined,2001);assert.equal(mistCount(),mistBase+8,'paused frame retains its wakes');
render(undefined,2100);const settledMist=mistCount();
assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);render(undefined,2100);
assert.equal(mistCount(),settledMist,'timeline discontinuity matches fog with no body wakes');
assert.equal(api.RoomPreview_SetReceivers(3,105,100,175,100),1);render(undefined,2101);
assert.equal(api.RoomPreview_SetReceivers(3,108,100,172,100),1);render(undefined,2102);
const highMist=mistCount();
assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);render(undefined,2102);
assert.equal(mistCount(),highMist,'bodies above the fog do not stir it');
console.log('Rolling mist preview: player/enemy movement, pause, scrubbing and vertical exclusion passed.');
configure('[effects]\nversion=1\n[emitter:05:01:0:cloud-bank:1]\nx=128\ny=240\nwidth=512\nheight=64\npoints=-256,0 256,0\nwake=.9\ncanopy-response=.7\ntwinkle=.9\nparticles=0\n');
const powder=()=>draws.filter(d=>d.effect===-1&&(!d.id||
  isEffectAtlas(d.id))).map(d=>[...d.v]);
const powderCount=()=>powder().reduce((n,v)=>n+v.length/8,0);
api.RoomPreview_SetReceivers(3,100,240,180,240);render(undefined,3000);
api.RoomPreview_SetReceivers(3,103,240,177,240);render(undefined,3001);
const movingPowder=powder(),movingPowderCount=powderCount();
render(undefined,3001);assert.deepEqual(powder(),movingPowder,'pausing preserves the powder wake');
api.RoomPreview_SetReceivers(0,0,0,0,0);render(undefined,3001);
assert(movingPowderCount>powderCount(),'both native stand-in observers feed extra ground powder');
api.RoomPreview_SetReceivers(3,100,180,180,180);render(undefined,3010);
api.RoomPreview_SetReceivers(3,103,180,177,180);render(undefined,3011);
const airbornePowder=powder();api.RoomPreview_SetReceivers(0,0,0,0,0);render(undefined,3011);
assert.deepEqual(powder(),airbornePowder,'airborne bodies cannot kick powder off the ground path');
api.RoomPreview_SetReceivers(3,100,240,180,240);render(undefined,3020);
api.RoomPreview_SetReceivers(3,103,240,177,240);render(undefined,3021);
render(undefined,3000);const scrubbedPowder=powder();
api.RoomPreview_SetReceivers(0,0,0,0,0);render(undefined,3000);
assert.deepEqual(powder(),scrubbedPowder,'reverse seeking discards body motion history');
console.log('Snow powder: real player/enemy wakes, pause, airborne exclusion and reverse seeking passed.');
const groundCoverRecipe='[effects]\nversion=1\n[emitter:05:01:0:cloud-bank:1]\nx=128\ny=240\nwidth=512\nheight=64\npoints=-256,0 256,0\nground-depth=4\nintensity=2\ncolor=d8e6f6\ncolor-end=fff5df\nparticles=0\n';
configure(groundCoverRecipe);
api.RoomPreview_SetReceivers(0,0,0,0,0);render(undefined,3100);
const untouchedCover=powder(),coverCreates=creates;
api.RoomPreview_SetReceivers(3,100,240,180,240);render(undefined,3100);
const pressedCover=powder();assert.notDeepEqual(pressedCover,untouchedCover,'stationary feet compress solid cover');
assert(draws.findIndex(d=>d.effect===5)<draws.findLastIndex(d=>d.effect===-1&&d.target===0&&
  (!d.id||isEffectAtlas(d.id))),
  'cover uses the foreground alpha pass after the actor material');
render(undefined,3190);assert.deepEqual(powder(),pressedCover,'solid cover has no gust-clock flicker');
render(undefined,3100);assert.deepEqual(powder(),pressedCover,'seeking preserves static contact');
api.RoomPreview_SetReceivers(3,100,220,180,220);render(undefined,3101);
assert.deepEqual(powder(),untouchedCover,'jumping feet clear the surface');
assert.equal(creates,coverCreates,'ground cover allocates no additional textures');
console.log('Ground snow: standing contact, actor occlusion order, jump, stable clocks and texture reuse passed.');
const coverDraws=()=>draws.filter(d=>d.effect===-1&&d.target===0&&
  (!d.id||isEffectAtlas(d.id)));
api.RoomPreview_SetReceivers(3,100,240,180,240);render(undefined,3200);
const lipOnly=[...coverDraws().at(-1).v],lipPasses=coverDraws().length;
configure(groundCoverRecipe+'ground-width=8\n');render(undefined,3200);
const surfaceDraws=coverDraws(),actorIndex=draws.findIndex(d=>d.effect===5&&d.target===0);
assert.equal(surfaceDraws.length,lipPasses+1,'top uses one existing scenery alpha batch');
assert(actorIndex>=0&&draws.indexOf(surfaceDraws[0])<actorIndex&&
  draws.indexOf(surfaceDraws.at(-1))>actorIndex,'broad top behind actors, shallow lip in front');
assert.deepEqual([...surfaceDraws.at(-1).v],lipOnly,'surface width cannot increase boot occlusion');
const surfaceVertices=surfaceDraws.map(d=>[...d.v]);
render(undefined,3260);assert.deepEqual(coverDraws().map(d=>[...d.v]),surfaceVertices);
render(undefined,3200);assert.deepEqual(coverDraws().map(d=>[...d.v]),surfaceVertices);
api.RoomPreview_SetReceivers(3,100,220,180,220);render(undefined,3201);
const airborneSurface=coverDraws().map(d=>[...d.v]);
api.RoomPreview_SetReceivers(0,0,0,0,0);render(undefined,3201);
assert.deepEqual(coverDraws().map(d=>[...d.v]),airborneSurface,'jump clears both contact surfaces');
assert.equal(creates,coverCreates,'top surface reuses existing textures');
console.log('Ground top face: separate actor order, unchanged boot depth, stable contact/seek and texture reuse passed.');
// Volumetric cloud banks share the existing atlas/batch in the actual WASM
// renderer, including background-only anchoring and deterministic scrubbing.
configure('[effects]\nversion=1\n[emitter:05:01:0:cloud-bank:1]\nx=128\ny=128\nwidth=512\nheight=192\nanchor=bg2-point\nplacement=background\ntextured=1\nrolling=.8\nvolume=1\nstrands=6\ncolor=f4e7d1\ncolor-end=68748c\n');
camera=[0,96];render(undefined,1200);
const volumeDraws=()=>{
  const lightTargets=new Set(draws.flatMap(d=>d.effect===4?[d.params[176]]:
    d.effect===5?[d.params[17],d.params[18]]:[]).filter(id=>id>0));
  return draws.filter(d=>isEffectAtlas(d.id)&&!lightTargets.has(d.target));
};
assert(volumeDraws().length>0,'volume masks reach the shared background renderer');
const volumeVertices=volumeDraws().map(d=>[...d.v]),volumeCreates=creates;
render(undefined,1237);assert.equal(creates,volumeCreates);
assert.notDeepEqual(volumeDraws().map(d=>[...d.v]),volumeVertices,'cloud billows drift');
render(undefined,1200);assert.equal(creates,volumeCreates);
assert.deepEqual(volumeDraws().map(d=>[...d.v]),volumeVertices,'seeking restores cloud shapes and shading');
console.log('Cloud volumes: actual WASM BG2 drawing, motion, seek and texture reuse passed.');
// The shipped native definitions must fit the real WASM budget too. Moving
// through every Marahna room exercises all glow variants and sparse dew cycles.
const shippedEffects=fs.readFileSync('action-effects.ini','utf8');
for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===6&&r.map===1);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);configure(shippedEffects);
  const cells=api.RoomPreview_CollisionGrid(),columns=api.RoomPreview_Width()/16;
  const collision=new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),cells).slice();
  for(const block of shippedEffects.split(/(?=^\[)/m).filter(b=>b.startsWith(`[emitter:06:01:${terrain}:cloud-bank:`))) {
    const r=Object.fromEntries([...block.matchAll(/^([a-z-]+)=([^\n]*)$/gm)].map(m=>[m[1],m[2]]));
    const points=r.points.split(/\s+/).map(p=>p.split(',').map(Number));
    for(let i=1;i<points.length;i++)for(let x=points[i-1][0]+4;x<points[i][0];x+=8) {
      const wx=x+Number(r.x),column=Math.floor(wx/16);
      let row=0;while(row<cells/columns&&!(collision[row*columns+column]&15))row++;
      const mask=collision[row*columns+column]&15,offset=wx%16;
      const floor=row*16+(mask===9?offset:mask===6?16-offset:0);
      const t=(x-points[i-1][0])/(points[i][0]-points[i-1][0]);
      const y=Number(r.y)+points[i-1][1]+t*(points[i][1]-points[i-1][1]);
      assert(Math.abs(y-floor)<=1,`Northwall snow path must touch terrain ${terrain} at ${wx}: ${y} vs ${floor}`);
    }
  }
  for(const [cx,cy,px,py] of [[0,288,64,448],[1152,496,1280,656],[2260,560,2400,688]]) {
    camera=[cx,cy];api.RoomPreview_SetReceivers(3,px,py,px+24,py);render(undefined,3200);
    api.RoomPreview_SetReceivers(3,px+3,py,px+21,py);render(undefined,3201);
    render(undefined,3200);
  }
}
console.log('Northwall snow: complete shipped room across three terrains, motion and scrubbing passed.');
// Act 2 has several vertical routes through one column. Check the authored
// contact against its local collision surface, not the first solid cell above.
for(const terrain of [0,1,2])for(const map of [5,6,8]) {
  const base=data.rooms.find(r=>r.group===6&&r.map===map);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);configure(shippedEffects);
  const cells=api.RoomPreview_CollisionGrid(),columns=api.RoomPreview_Width()/16;
  const collision=new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),cells).slice();
  const header=`[emitter:06:${map.toString(16).padStart(2,'0')}:${terrain}:cloud-bank:`;
  const covers=shippedEffects.split(/(?=^\[)/m).filter(b=>b.startsWith(header))
    .map(b=>Object.fromEntries([...b.matchAll(/^([a-z-]+)=([^\n]*)$/gm)].map(m=>[m[1],m[2]])))
    .filter(r=>Number(r['ground-depth'])>0);
  assert(covers.length>0,'each snowy Act 2 room has contact cover');
  for(const r of covers) {
    const points=r.points.split(/\s+/).map(p=>p.split(',').map(Number));
    for(let i=1;i<points.length;i++)for(let x=points[i-1][0]+4;x<points[i][0];x+=8) {
      const wx=x+Number(r.x),column=Math.floor(wx/16),offset=wx%16;
      const t=(x-points[i-1][0])/(points[i][0]-points[i-1][0]);
      const y=Number(r.y)+points[i-1][1]+t*(points[i][1]-points[i-1][1]);
      const rows=[Math.floor(y/16)-1,Math.floor(y/16)];
      const supported=rows.some(row=>{
        if(row<0||row>=cells/columns)return false;
        const flags=collision[row*columns+column],mask=flags&15;
        if(!(flags&16)&&mask!==6&&mask!==9)return false;
        const floor=row*16+(mask===9?offset:mask===6?16-offset:0);
        return Math.abs(y-floor)<=1;
      });
      assert(supported,`Act 2 snow must touch local terrain ${map}/${terrain} at ${wx},${y}`);
    }
  }
  const positions={5:[448,192,552,320],6:[512,896,704,1056],8:[128,288,272,448]};
  const [cx,cy,px,py]=positions[map];camera=[cx,cy];
  api.RoomPreview_SetReceivers(3,px,py,px+16,py);render(undefined,3200);
  api.RoomPreview_SetReceivers(3,px+3,py,px+13,py);render(undefined,3202);
  render(undefined,3294);render(undefined,3200);
}
console.log('Northwall Act 2 snow: local platform contacts, three terrains, player/enemy movement, settling and scrubbing passed.');
// Aitos Act 2 combines the shared extended lava source with placed atmosphere.
// Validate the complete shipped document in every terrain, through scrolling
// and reverse-time seeking, rather than checking only isolated field defaults.
for(const [id,positions] of [[4,[[440,410],[160,748]]],[5,[[128,64]]],
                            [6,[[240,332],[1230,560]]],[7,[[128,0]]]])
for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===4&&r.map===id);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  configure(shippedEffects);api.RoomPreview_EnableEffects(1);
  const cellCount=api.RoomPreview_CollisionGrid(),columns=api.RoomPreview_Width()/16;
  const collision=new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),cellCount).slice();
  for(const block of shippedEffects.split(/(?=^\[)/m).filter(b=>b.startsWith(`[emitter:04:0${id}:${terrain}:drips:`)&&/^debris=1$/m.test(b))) {
    const v=Object.fromEntries([...block.matchAll(/^([a-z-]+)=([^\n]*)$/gm)].map(m=>[m[1],Number(m[2])]));
    for(let py=Math.ceil((v.y-v.height/2+8)/16)*16;py<v.y+v.height/2-8;py+=16)
      for(const px of [v.x-v.width/2,v.x+v.width/2])
        assert.equal(collision[(py/16|0)*columns+(px/16|0)]&15,0,'grit path stops before intervening terrain');
  }
  for(const position of positions)for(const height of [360,400]) {
    camera=position;const view=[640,height,1,0,0,1,0];
    render(view,180);const hash=api.RoomPreview_EffectHash(),allocated=creates;
    render(view,211);render(view,180);
    assert.equal(api.RoomPreview_EffectHash(),hash,'lava and atmosphere return to the same saved clock');
    assert.equal(creates,allocated,'Aitos lighting and atmosphere reuse textures during animation');
  }
}
console.log('Aitos Act 2: all four rooms/three terrains, shipped effects, 16:9/16:10, scrolling, reverse seeking and texture reuse passed.');
for(const id of [1,2,3]) for(const terrain of [0,1,2]) {
  const originalRoom=data.rooms.find(r=>r.group===5&&r.map===id);
  const terrainRoom={...originalRoom,terrainProfile:terrain,bg:[originalRoom.terrainVariants.find(v=>v.profile===terrain).bg1,originalRoom.bg[1]]};
  assert.equal(send(encode(data,blobs,terrainRoom),api.RoomPreview_Load),1);
  configure(shippedEffects);api.RoomPreview_EnableEffects(1);
  const cells=api.RoomPreview_CollisionGrid(),columns=api.RoomPreview_Width()/16;
  const collision=new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),cells).slice();
  for(const block of shippedEffects.split(/(?=^\[)/m).filter(b=>b.startsWith(`[emitter:05:0${id}:${terrain}:drips:`))) {
    const values=Object.fromEntries([...block.matchAll(/^([a-z-]+)=([^\n]*)$/gm)].map(m=>[m[1],m[2]]));
    const x=Number(values.x),y=Number(values.y),h=Number(values.height),w=Number(values.width);
    for(let py=Math.ceil((y-h/2)/16)*16;py<y+h/2;py+=16)for(const px of [x-w/2,x+w/2])
      assert.equal(collision[(py/16|0)*columns+(px/16|0)]&15,0,'authored dew never falls through a platform');
    if(values.ripple==='1')assert.equal(collision[((y+h/2)/16|0)*columns+(x/16|0)]&15,0,'ripples only on open water');
  }
  camera=[0,id===3?0:224];render(undefined,1200);
  const created=creates;
  for(const clock of [1201,1500,1900,1200]) {render(undefined,clock);assert.equal(creates,created,'glow/dew animation reuses textures');}
  camera=[128,id===3?0:224];render(undefined,1200);
  assert(draws.some(d=>isEffectAtlas(d.id)));
}
assert.equal(send(encode(data,blobs,data.rooms.find(r=>r.group===5&&r.map===1)),api.RoomPreview_Load),1);
console.log('Marahna swamp life: shipped room budgets, shared render, timeline scrubbing and texture reuse passed.');
// Moving Fillmore's wet-surface field into another room must also move its
// editable catalogue, including contacts added/removed after the first scan.
const wetField=fs.readFileSync('assets/effects/cave-water-field.ini','utf8')
  .replace('[field:01:02:0:', '[field:05:01:0:')
  .replace('counts=3 4 8 4','counts=0 0 2 0')
  .replace('components=15','components=8')
  .replace('dimensions=2048 1280 2048 1280','dimensions=0 0 0 0')
  .replace(/^wet-1=.*$/m,'wet-1=278 100 416 0 0 0')
  .replace(/^wet-2=.*$/m,'wet-2=354 200 416 0 0 0');
const kindName=kind=>{
  const p=api.RoomPreview_KindName(kind),bytes=new Uint8Array(api.memory.buffer);
  let end=p;while(bytes[end])end++;
  return new TextDecoder().decode(bytes.subarray(p,end));
};
const sheenIndex=()=>Array.from({length:api.RoomPreview_CatalogueCount()},(_,i)=>i)
  .find(i=>kindName(api.RoomPreview_CatalogueValue(i,1))==='cave-sheen');
configure(wetField);
let wetIndex=sheenIndex();
assert.notEqual(wetIndex,undefined,'copied water field adds its family to the destination catalogue');
assert.equal(api.RoomPreview_CatalogueValue(wetIndex,7),2);
assert.equal(api.RoomPreview_CatalogueMemberValue(wetIndex,1,1),354);
assert.equal(api.RoomPreview_CatalogueMemberValue(wetIndex,1,2),416,'sheen belongs to the landing, not the ceiling');
assert.equal(api.RoomPreview_CatalogueMemberValue(wetIndex,1,3),32);
assert.equal(api.RoomPreview_CatalogueMemberValue(wetIndex,1,4),5,'surface depth sets the resize handle');
const wetMember='[member:05:01:0:cave-sheen:00000002]\noffset-x=7\n';
configure(wetField+'\n'+wetMember);
configure(noEffects+wetMember+wetField.slice(wetField.indexOf('[field:')));
assert.equal(send(Buffer.from(wetField+'\n'+wetMember.replace('00000002','00000003')),
  api.RoomPreview_ConfigureEffects),0,'member beyond the copied field count is rejected');
assert.equal(api.RoomPreview_CatalogueValue(sheenIndex(),7),2,'invalid edit preserves the active field');
assert.equal(send(Buffer.from(noEffects+wetMember),api.RoomPreview_ConfigureEffects),0,
  'copied members require their destination field');
configure(wetField.replace('counts=0 0 2 0','counts=0 0 1 0'));
assert.equal(api.RoomPreview_CatalogueValue(sheenIndex(),7),1,'contact deletion refreshes the catalogue');
configure(noEffects);
assert.equal(sheenIndex(),undefined,'removing the copied field removes its catalogue entries');
assert.equal(send(encode(data,blobs,data.rooms.find(r=>r.group===1&&r.map===2)),api.RoomPreview_Load),1);
configure(wetField.replace('[field:05:01:0:','[field:01:02:0:').replace('counts=0 0 2 0','counts=0 0 0 0'));
assert.equal(api.RoomPreview_CatalogueValue(sheenIndex(),7),0,'zero contacts never fall back to bundled contacts');
console.log('Wet-surface fields: portable catalogue, landing handles, member saves, deletion and atomic validation passed.');
// Reused window recipes must actually capture on temple materials, not merely
// expose editable handles. Check the shared light maps with real regional art.
const shippedBlocks=shippedEffects.split(/(?=^\[)/m);
const properties=block=>Object.fromEntries([...block.matchAll(/^([a-z0-9-]+)=([^\n]*)$/gm)].map(m=>[m[1],m[2]]));
const sceneryEnergy=()=>draws.filter(d=>d.effect===4).reduce((sum,d)=>sum+energy(d.params[176]),0);
for(const id of [4,5,6,7,8]) for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===5&&r.map===id);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  const castle=shippedBlocks.find(b=>b.startsWith(`[field:05:0${id}:${terrain}:castle-field:`));
  const values=properties(castle),source=values['source-1'].split(/\s+/).map(Number);
  const x=source[5],y=source[14]-4;
  configure(noEffects+castle);api.RoomPreview_EnableEffects(1);
  assert.equal(api.RoomPreview_SetReceivers(3,x-4,y,x+4,y),1);
  camera=[Math.max(0,x-128),Math.max(0,y-160)];baselineDistance=3.25;render(undefined,1200);
  const lit=sceneryEnergy(),actorLight=draws.find(d=>d.effect===5);
  assert(actorLight&&roleEnergy(actorLight,0)>0&&roleEnergy(actorLight,1)>0,'window light reaches both actor classes');
  const index=Array.from({length:api.RoomPreview_CatalogueCount()},(_,i)=>i)
    .find(i=>kindName(api.RoomPreview_CatalogueValue(i,1))==='castle-light');
  assert.notEqual(index,undefined,'temple window catalogue is editable');
  assert.equal(api.RoomPreview_CatalogueValue(index,7),Number(values['source-count']));
  configure(noEffects+castle.replace(/^components=1$/m,'components=0'));render(undefined,1200);
  assert(lit>sceneryEnergy(),`window material capture lights room 05:0${id} terrain ${terrain}`);
  configure(shippedEffects);render(undefined,1200);
  const allocated=creates;
  for(const clock of [1201,1500,1900,1200]) {render(undefined,clock);assert.equal(creates,allocated,'temple animation reuses textures');}
  const cells=api.RoomPreview_CollisionGrid(),columns=api.RoomPreview_Width()/16;
  const collision=new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),cells).slice();
  const solid=(px,py)=>!!(collision[(py/16|0)*columns+(px/16|0)]&(1<<((py/8&1)*2+(px/8&1))));
  const map=blobs[room.bg[0].map],pages=room.bg[0].pagesWide;
  const mistAir=(px,py)=>{
    const cell=map[(Math.floor(py/256)*pages+Math.floor(px/256))*256+(py>>4&15)*16+(px>>4&15)];
    // EU marks the lower spike art solid; the mist still belongs at its base.
    return !solid(px,py)||cell===0x70||cell===0x78;
  };
  for(const block of shippedBlocks.filter(b=>b.startsWith(`[emitter:05:0${id}:${terrain}:drips:`))) {
    const v=properties(block),x=+v.x,y=+v.y,h=+v.height;
    for(let py=y-h/2;py<y+h/2;py+=4)
      assert(!solid(x,py),`temple drip at ${x},${py} crosses collision in 05:0${id} terrain ${terrain}`);
  }
  for(const block of shippedBlocks.filter(b=>b.startsWith(`[emitter:05:0${id}:${terrain}:wet-contour:`))) {
    const v=properties(block),x=+v.x,y=+v.y,w=+v.width;
    for(let px=x-w/2;px<x+w/2;px+=4)
      assert(solid(px,y)&&!solid(px,y-1),`wet ledge at ${px},${y} needs an exposed top in 05:0${id} terrain ${terrain}`);
  }
  for(const block of shippedBlocks.filter(b=>b.startsWith(`[emitter:05:0${id}:${terrain}:cloud-bank:`))) {
    const v=properties(block),x=+v.x,y=+v.y,w=+v.width;
    for(let px=x-w/2;px<x+w/2;px+=8) {
      let supported=false;
      for(let py=y-8;py<=y+16;++py) supported||=solid(px,py)&&mistAir(px,py-1);
      assert(supported,`floor wisp at ${px},${y} floats over a gap in 05:0${id} terrain ${terrain}`);
    }
  }
}
console.log('Marahna temple: regional window capture, both light receivers, editable catalogue, wet ledges, drip paths and texture reuse passed.');
// Grounded lightning must reach the real Aitos BG2 palette in every regional
// profile. An absent terrain selection must remove the active strike, not
// silently fall back to the previous short airborne channel.
for (const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===4&&r.map===1);
  const stormRoom={...base,terrainProfile:terrain,
    bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,stormRoom),api.RoomPreview_Load),1);
  assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
  const storm=noEffects+shippedBlocks.filter(b=>b.startsWith(`[emitter:04:01:${terrain}:storm-lightning:`)).join('');
  camera=[1940,712];baselineDistance=3.25;
  // Storm ribbons/glows are untextured meshes. Fullscreen cache-copy quads
  // legitimately differ between a freshly configured and retained frame.
  const isWeather=d=>d.target>0&&d.id===0&&d.effect===-1&&d.v.length>32&&
    draws.some(sample=>sample.id===d.target&&sample.effect===0);
  const weatherGeometry=()=>draws.filter(isWeather).map(d=>[...d.v]);
  for (const view of [[640,360,1,0,0,1,0],[640,400,1,0,0,1,0]]) {
    configure(storm);render(view,1057);
    const lit=api.RoomPreview_EffectVertices();
    const geometry=weatherGeometry(),allocated=creates;
    assert(geometry.length>0,'storm channels submit visible geometry');
    const weather=draws.filter(isWeather);
    const weatherTarget=weather[0].target;
    assert(weatherTarget>0&&weather.every(d=>d.target===weatherTarget),
      'lightning is composed into one sky source before its blur');
    const lastWeather=draws.findLastIndex(d=>weather.includes(d));
    assert(draws.some((d,i)=>i>lastWeather&&d.id===weatherTarget&&d.effect===0),
      'the existing sky blur samples the combined backdrop and lightning');
    render(view,1063);render(view,1057);
    assert.equal(creates,allocated,'terrain contacts allocate no new textures');
    assert.deepEqual(weatherGeometry(),geometry,'grounded strikes seek deterministically');
    configure(storm.replaceAll('ground-palette=1','ground-palette=7'));render(view,1057);
    assert(api.RoomPreview_EffectVertices()<lit,'the visible bolt requires actual mountain contact');
    assert(!weatherGeometry().length,'absent grounded lightning adds no weather composition');
    configure(storm.replace(/^x=.*$/gm,'x=8000'));render(view,1057);
    assert(!weatherGeometry().length,'offscreen strikes add no sky composition pass');
  }
}
console.log('Aitos lightning: real regional BG2 contacts, 16:9/16:10, missing-ground suppression, reverse seeking and texture reuse passed.');
// Slow aurora folds share the native sky source and its depth of field. The
// reflection is an ordinary editable gradient, never a second sky overlay.
for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===6&&r.map===1);
  const room={...base,terrainProfile:terrain,
    bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
  const aurora=noEffects+shippedBlocks.filter(b=>b.startsWith(`[emitter:06:01:${terrain}:aurora:`)).join('');
  const filtered=(id,start)=>draws.some((d,i)=>i>start&&d.id===id&&
    ((d.effect===0||d.effect===2)||(d.target>0&&filtered(d.target,i))));
  const curtains=()=>draws.filter((d,i)=>d.target>0&&!d.id&&d.effect===-1&&
    d.v.length>32&&filtered(d.target,i));
  camera=[0,288];baselineDistance=3.25;
  for(const mode of [0,1,2]) {
    const view=[640,400,1,0,0,mode,0];
    configure(aurora);render(view,360);
    const folds=curtains(),allocated=creates;
    assert(folds.length>0,`aurora shares BG2 filtering in mode ${mode}, terrain ${terrain}`);
    assert(folds.every(d=>d.target===folds[0].target),'curtains use one background source');
    assert(!draws.some(d=>!d.target&&!d.id&&d.effect===-1&&d.v.length>32),
      'source-composited aurora is not repeated over the foreground');
    const geometry=folds.map(d=>[...d.v]);
    render(view,361);assert.notDeepEqual(curtains().map(d=>[...d.v]),geometry,'folds move continuously');
    render(view,360+Number(aurora.match(/^lifetime=(\d+)/m)[1]));
    assert.deepEqual(curtains().map(d=>[...d.v]),geometry,'aurora cycles without a seam');
    render(view,360);assert.deepEqual(curtains().map(d=>[...d.v]),geometry,'aurora seeks deterministically');
    assert.equal(creates,allocated,'aurora motion reuses source and filtering textures');
    configure(aurora.replace(/^x=.*$/gm,'x=8000'));render(view,360);
    assert(!curtains().length,'offscreen aurora adds no background composition');
  }
  configure(shippedEffects);
  for(const at of [[0,288],[1152,512],[2180,528]]) {
    camera=at;render([640,400,1,0,0,1,0],360);
    assert(api.RoomPreview_EffectVertices()>0,'the full Northwall composition fits its regional room budgets');
  }
}
console.log('Northwall aurora: regional sky composition, source DoF, cycle/seek stability, camera travel and texture reuse passed.');
for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===6&&r.map===1);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  const stars=noEffects+shippedBlocks.filter(b=>b.startsWith(`[emitter:06:01:${terrain}:starfield:`)).join('');
  const filtered=(id,start)=>draws.some((d,i)=>i>start&&d.id===id&&
    ((d.effect===0||d.effect===2)||(d.target>0&&filtered(d.target,i))));
  const points=()=>draws.filter((d,i)=>d.target>0&&!d.id&&d.effect===-1&&d.v.length>32&&filtered(d.target,i));
  const energy=()=>points().reduce((sum,d)=>sum+d.v.reduce((a,v,i)=>a+(i%8===5?v:0),0),0);
  camera=[0,288];baselineDistance=3.25;
  for(const mode of [0,1,2]) {
    const view=[640,400,1,0,0,mode,0];
    configure(stars);render(view,360);
    const starsInGaps=points().map(d=>[...d.v]),maskedEnergy=energy(),allocated=creates;
    assert(starsInGaps.length>0,'the real Northwall sky has visible stars');
    render(view,540);render(view,360);
    assert.deepEqual(points().map(d=>[...d.v]),starsInGaps,'cloud coverage and star shimmer seek deterministically');
    assert.equal(creates,allocated,'star and haze animation reuse the existing sky textures');
    configure(stars.replace(/^sky-colors=.*$/m,'sky-colors=none'));render(view,360);
    assert(energy()>maskedEnergy,'native clouds attenuate stars and haze');
    configure(stars.replace(/^sky-palettes=.*$/m,'sky-palettes=7'));render(view,360);
    assert(!points().length,'no matching clear-sky artwork suppresses the whole starfield');
  }
  // With occlusion disabled, only brightness changes as time advances. Star
  // positions never wander, drift like snow, respawn, or depend on seeking.
  const fixed=stars.replace(/^sky-colors=.*$/m,'sky-colors=none').replace(/^scatter=.*$/m,'scatter=0');
  configure(fixed);render([640,400,1,0,0,1,0],360);
  const positions=()=>points().map(d=>[...d.v].filter((_,i)=>i%8<2));
  const anchored=positions();render([640,400,1,0,0,1,0],480);
  assert.deepEqual(positions(),anchored,'stars are fixed to the distant sky while native clouds move');
}
console.log('Northwall stars: native cloud masking, fixed positions, haze, all regional backgrounds, seeking and texture reuse passed.');
// Boss-room flames are native BG2 artwork. Their added sparks must enter the
// same source texture before filtering, in every background presentation.
for (const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===4&&r.map===7);
  const room={...base,terrainProfile:terrain,
    bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
  const torches=noEffects+shippedBlocks.filter(b=>b.startsWith(`[emitter:04:07:${terrain}:torch:`)).join('');
  camera=[128,0];baselineDistance=3.25;
  const filtered=(id,start)=>draws.some((d,i)=>i>start&&d.id===id&&
    ((d.effect===0||d.effect===2)||(d.target>0&&filtered(d.target,i))));
  const sourceParticles=()=>draws.filter((d,i)=>d.target>0&&!d.id&&d.effect===-1&&
    d.v.length>32&&filtered(d.target,i));
  for (const mode of [0,1,2]) for (const height of [360,400]) {
    const view=[640,height,1,0,0,mode,0];
    configure(torches);render(view,240);
    const particles=sourceParticles(),allocated=creates;
    assert(particles.length>0,`boss sparks share BG2 filtering in mode ${mode}, terrain ${terrain}`);
    assert(particles.every(d=>d.target===particles[0].target),'one source composition per background');
    assert(!draws.some(d=>!d.target&&!d.id&&d.effect===-1&&d.v.length>32),
      'the source-composited sparks are not drawn again as a sharp overlay');
    const geometry=particles.map(d=>[...d.v]);
    render(view,274);render(view,240);
    assert.equal(creates,allocated,'torch animation reuses source and filter textures');
    assert.deepEqual(sourceParticles().map(d=>[...d.v]),geometry,'torch source rendering seeks deterministically');
    configure(torches.replace(/^x=.*$/gm,'x=8000'));render(view,240);
    assert(!sourceParticles().length,'offscreen torches add no source composition');
  }
}
console.log('Aitos boss torches: source DoF in all background modes, regional art, aspect ratios, seeking and texture reuse passed.');
// Textured rock vents share the background source pass as well. Verify the
// mask atlas is composited before every DoF mapping, rather than as an overlay.
for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===4&&r.map===4);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  const vents=noEffects+shippedBlocks.filter(b=>b.startsWith(`[emitter:04:04:${terrain}:cloud-bank:`)&&
    /^plume=1$/m.test(b)&&/^anchor=bg2-point$/m.test(b)).join('');
  const filtered=(id,start)=>draws.some((d,i)=>i>start&&d.id===id&&
    ((d.effect===0||d.effect===2)||(d.target>0&&filtered(d.target,i))));
  const puffs=()=>draws.filter((d,i)=>d.target>0&&isEffectAtlas(d.id)&&d.effect===-1&&filtered(d.target,i));
  camera=[440,410];baselineDistance=3.25;
  for(const mode of [0,1,2])for(const height of [360,400]) {
    const view=[640,height,1,0,0,mode,0];configure(vents);render(view,180);
    const first=puffs().map(d=>[...d.v]),allocated=creates;
    assert(first.length,`vent mask enters source DoF, mode ${mode}, terrain ${terrain}`);
    render(view,211);render(view,180);
    assert.deepEqual(puffs().map(d=>[...d.v]),first,'vent puff clocks seek without state');
    assert.equal(creates,allocated,'vent animation reuses source/filter/atlas textures');
    configure(vents.replace(/^x=.*$/gm,'x=8000'));render(view,180);
    assert(!puffs().length,'offscreen vents do not schedule source composition');
  }
}
console.log('Aitos rock vents: textured source DoF in all background modes/terrains/aspects, seek, culling and texture reuse passed.');

// Periodic waterfall art repeats its page, but the sky light belongs to the
// unwrapped world. Exercise the real WASM material path used by the editor.
for (const map of [2,3]) {
  const waterfall=data.rooms.find(r=>r.group===4&&r.map===map);
  assert.equal(send(encode(data,blobs,waterfall),api.RoomPreview_Load),1);
  const scope=`[emitter:04:0${map}:0:`;
  configure(noEffects+shippedBlocks.filter(b=>b.startsWith(scope)&&
    (b.includes(':cloud-light:')||b.includes(':light-gradient:'))).join(''));
  const skyMaterials=()=>draws.filter(d=>d.effect===4&&d.params?.[72]>0&&
    d.params[70]===256&&d.params[71]===256);
  camera=map===2?[1000,460]:[128,0];baselineDistance=3.25;
  render([640,360,1,0,0,1,0],320);
  const materials=skyMaterials();assert(materials.length>0,'periodic sky receives cloud lighting');
  const field=[...materials[0].params.slice(68,176)],allocated=creates;
  assert.deepEqual(field.slice(0,4),[0,-1,256,256],'native row offset maps back to world coordinates');
  const firstUv=materials[0].v[6];
  camera[0]+=32;
  render([640,360,1,0,0,1,0],320);
  assert.deepEqual([...skyMaterials()[0].params.slice(68,176)],field,
    'scrolling changes sampled positions, not the authored light field');
  assert.notEqual(skyMaterials()[0].v[6],firstUv,'unwrapped waterfall UVs scroll with the camera');
  assert.equal(creates,allocated,'moving the sky light needs no new textures');
  api.RoomPreview_EnableEffects(0);render([640,360,1,0,0,1,0],320);
  assert.equal(skyMaterials().length,0,'environmental setting disables waterfall lighting');
  api.RoomPreview_EnableEffects(1);
}
console.log('Aitos waterfall sky light: periodic material, world anchoring, camera movement, toggle and texture reuse passed.');
// Background exposure travels with the same world region and uniform block in
// the browser, without introducing another draw or render target.
const dimSky=noEffects+shippedBlocks.filter(b=>b.startsWith('[emitter:04:03:0:cloud-light:'))
  .map(b=>b.replace(/^background-brightness=.*\n/gm,'').trimEnd()+'\nbackground-brightness=0.72\n').join('');
camera=[128,0];
for(const height of [360,400]) {
  configure(dimSky);render([640,height,1,0,0,1,0],320);
  const lit=draws.filter(d=>d.effect===4&&d.params?.[72]>0);
  assert(lit.length>0);
  for(const draw of lit) {
    assert(Math.abs(draw.params[73]-.28)<.0001,'dimming guard is uploaded');
    assert(Math.abs(draw.params[172]-.28)<.0001,'per-region brightness reaches the shader');
  }
  const allocated=creates,drawCount=draws.length;
  configure(dimSky.replace('background-brightness=0.72','background-brightness=1'));
  render([640,height,1,0,0,1,0],320);
  assert.equal(draws.length,drawCount,'dimming stays in the existing sky material');
  assert.equal(creates,allocated,'background brightness allocates no textures');
  for(const draw of draws.filter(d=>d.effect===4&&d.params?.[72]>0)) {
    assert.equal(draw.params[73],0);assert.equal(draw.params[172],0);
  }
}
console.log('Waterfall background brightness: 16:9/16:10, saved controls, neutral reset and pass/texture reuse passed.');
// Rainbow color is part of the existing mist batch, gated by actual sky light
// and density. The saved strength must survive the real C/WASM parser.
const rainbowScene=noEffects+shippedBlocks.filter(b=>b.startsWith('[emitter:04:03:0:')&&
  (b.includes(':cloud-light:')||b.includes(':cloud-bank:0a400331]'))).join('');
configure(rainbowScene);camera=[128,0];
render([640,360,1,0,0,1,0],320);
const rainbowCount=api.RoomPreview_EffectVertices(),rainbowAllocations=creates;
configure(rainbowScene.replace(/^rainbow=.*$/gm,'rainbow=0'));render([640,360,1,0,0,1,0],320);
assert(api.RoomPreview_EffectVertices()<rainbowCount,'sunlit spray adds a small spectrum mesh');
configure(rainbowScene);render([640,360,1,0,0,1,0],320);
assert.equal(api.RoomPreview_EffectVertices(),rainbowCount);
assert.equal(creates,rainbowAllocations,'rainbow strength toggles allocate no textures');
render([640,360,1,0,0,1,0],357);render([640,360,1,0,0,1,0],320);
assert.equal(api.RoomPreview_EffectVertices(),rainbowCount,'seeking restores the spray spectrum');
console.log('Waterfall mist rainbow: saved controls, light-dependent geometry, seek and texture reuse passed.');
const floodBlocks=shippedBlocks.filter(b=>b.startsWith('[emitter:04:03:0:')&&
  (b.includes(':cloud-light:')||b.includes(':cloud-bank:0a400103]')))
  .map(b=>b.replace(/^rainbow=.*$/gm,'rainbow=0'));
for(const height of [360,400]) {
  configure(noEffects+floodBlocks.join(''));render([640,height,1,0,0,1,0],320);
  const lit=volumeDraws().map(d=>[...d.v]),allocated=creates;
  assert(lit.length>0,'lower mist reaches the shared renderer');
  configure(noEffects+floodBlocks.map(b=>b.includes(':cloud-light:')?
    b.replace(/^intensity=.*$/gm,'intensity=0'):b).join(''));
  render([640,height,1,0,0,1,0],320);
  const dark=volumeDraws().map(d=>[...d.v]);
  assert.equal(lit.length,dark.length);
  let catchesLight=false;
  lit.forEach((vertices,draw)=>vertices.forEach((value,i)=>{
    if(i%8>=2&&i%8<=4)catchesLight ||= value>dark[draw][i]+.001;
    else assert.equal(value,dark[draw][i],'lighting preserves mist shape, opacity and UVs');
  }));
  assert(catchesLight,'lower BG1 mist catches the BG2 flood independently of rainbow colour');
  assert.equal(creates,allocated,'mist lighting adds no texture');
}
console.log('Boss flood mist: shared sky-light response, 16:9/16:10, unchanged geometry/alpha and texture reuse passed.');
for(const map of [2,3]) {
  const room=data.rooms.find(r=>r.group===4&&r.map===map);
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
  const blocks=shippedBlocks.filter(b=>b.startsWith(`[emitter:04:0${map}:0:`));
  const sun=blocks.find(b=>b.includes(':cloud-light:'));
  const water=noEffects+sun+blocks.filter(b=>b.includes('pattern=water-glints')).join('');
  const mist=noEffects+sun+blocks.filter(b=>b.includes(':cloud-bank:')&&/^sparkle=/m.test(b))
    .map(b=>b.replace(/^rainbow=.*$/gm,'rainbow=0')).join('');
  camera=map===2?[1000,460]:[128,0];baselineDistance=3.25;
  for(const height of [360,400])for(const mode of [0,1,2]) {
    const view=[640,height,1,0,0,mode,0];
    configure(water);render(view,320);
    const wetCount=api.RoomPreview_EffectVertices();
    configure(water.replace(sun,sun.replace(/^intensity=.*$/m,'intensity=0')));render(view,320);
    assert(api.RoomPreview_EffectVertices()<wetCount,'water glints need direct sky light');
    configure(mist);render(view,320);
    const sprayCount=api.RoomPreview_EffectVertices(),allocated=creates;
    configure(mist.replace(/^sparkle=.*$/gm,'sparkle=0'));render(view,320);
    assert(api.RoomPreview_EffectVertices()<sprayCount,'mist sparkle control adds density-bound droplets');
    configure(mist);render(view,343);render(view,320);
    assert.equal(api.RoomPreview_EffectVertices(),sprayCount,'mist sparkle seeking is deterministic');
    assert.equal(creates,allocated,'sparkle toggles and animation reuse existing textures');
  }
}
console.log('Water/mist sparkles: both rooms, 16:9/16:10, all background modes, light gating, seek and texture reuse passed.');
// Ice windows share one anchored footprint for visible diffusion and receiver
// lighting. Changing the halo must never change material or actor illumination.
for(const map of [2,3,4])for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===6&&r.map===map);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  const lights=shippedBlocks.filter(b=>b.startsWith(`[emitter:06:0${map}:${terrain}:soft-light:`));
  assert(lights.length>0&&lights.length<=42,'bounded authored ice-window lights and camera-facing glints');
  const p=properties(lights[0]),x=Number(p.x),y=Number(p.y);
  const recipe=noEffects+lights.join('');
  camera=[Math.max(0,x-128),Math.max(0,y-112)];baselineDistance=3.25;
  assert.equal(api.RoomPreview_SetReceivers(3,x-8,y+24,x+8,y+24),1);
  for(const height of [360,400])for(const mode of [0,1,2]) {
    const view=[640,height,1,0,0,mode,0];
    configure(recipe);render(view,180);
    const a=draws.find(d=>d.effect===5);assert(a);
    const received=[sceneryEnergy(),roleEnergy(a,0),roleEnergy(a,1)];
    assert(received.every(n=>n>0),'ice lights reach scenery, player and enemy');
    const visible=api.RoomPreview_EffectVertices();
    configure(recipe.replace(/^scatter=.*$/gm,'scatter=0'));render(view,180);
    const b=draws.find(d=>d.effect===5);assert(b);
    assert.deepEqual([sceneryEnergy(),roleEnergy(b,0),roleEnergy(b,1)],received,
      'diffusion control leaves receiver energy unchanged');
    assert(api.RoomPreview_EffectVertices()<visible,'diffusion adds a visible light pool');
    configure(recipe.replace(/^light-(scenery|player|enemies)=1$/gm,'light-$1=0'));render(view,180);
    assert.equal(sceneryEnergy(),0,'receiver toggles leave a visual-only halo');
    assert(api.RoomPreview_EffectVertices()>0);
    configure(recipe);render(view,180);const allocated=creates;
    render(view,227);render(view,180);
    assert.equal(api.RoomPreview_EffectVertices(),visible,'static ice glow survives seeking');
    assert.equal(creates,allocated,'ice diffusion reuses existing render targets');
  }
}
console.log('Northwall ice windows: three caves/terrains, receiver-independent diffusion, aspects, background modes, seeking and texture reuse passed.');
// Foreground water must retain its own depth when distant BG2 switches to a
// skybox. Exercise the shipped patches through the real parser and compositor.
for(const map of [2,4])for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===6&&r.map===map);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
  const blocks=shippedBlocks.filter(b=>b.startsWith(`[emitter:06:0${map}:${terrain}:`));
  const water=blocks.filter(b=>b.includes(':water-surface:'));
  const mist=blocks.filter(b=>b.includes(':cloud-bank:'));
  assert(water.length&&mist.length,'water caves have both surface shimmer and hanging haze');
  camera=map===2?[512,448]:[128,224];baselineDistance=3.25;
  const surfaceGeometry=()=>draws.filter(d=>d.effect===-1&&d.target===0&&!d.id).map(d=>[...d.v]);
  for(const height of [360,400])for(const mode of [0,1,2]) {
    const view=[640,height,1,0,0,mode,0];
    configure(noEffects+water.join(''));render(view,180);
    assert(api.RoomPreview_EffectVertices()>0,'water survives every background mode');
    const still=surfaceGeometry();assert(still.length);
    render(view,227);assert.notDeepEqual(surfaceGeometry(),still,'surface reflections move');
    render(view,180);assert.deepEqual(surfaceGeometry(),still,'seeking restores the exact water surface');
    configure(noEffects+mist.join(''));render(view,180);
    const banks=volumeDraws().map(d=>[...d.v]);assert(banks.length);
    const allocated=creates;
    render(view,227);assert.notDeepEqual(volumeDraws().map(d=>[...d.v]),banks,'hanging haze drifts');
    render(view,180);assert.deepEqual(volumeDraws().map(d=>[...d.v]),banks,'mist is deterministic');
    assert.equal(creates,allocated,'water mist reuses its mask atlas');
    api.RoomPreview_EnableEffects(0);render(view,180);
    assert.equal(api.RoomPreview_EffectVertices(),0,'environment switch disables water and mist');
    api.RoomPreview_EnableEffects(1);
  }
}
console.log('Northwall water: both rooms/all terrains, high water in all background modes, 16:9/16:10, motion, seeking, toggle and texture reuse passed.');
// Airborne ice and haze receive the blue window light in the alpha pass.
// Optical glare and visible diffusion must not masquerade as incident light.
for(const map of [2,4])for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===6&&r.map===map);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  const blocks=shippedBlocks.filter(b=>b.startsWith(`[emitter:06:0${map}:${terrain}:`));
  const lights=blocks.filter(b=>b.includes(':soft-light:')).join('');
  const air=blocks.filter(b=>b.includes('pattern=ice-crystals')||/:cloud-bank:0602a/i.test(b)).join('');
  assert(air.includes('pattern=ice-crystals')&&air.includes(':cloud-bank:'),'water caves include both air layers');
  const recipe=noEffects+lights+air;
  camera=map===2?[512,448]:[128,224];baselineDistance=3.25;
  for(const height of [360,400])for(const mode of [0,1,2]) {
    const view=[640,height,1,0,0,mode,0];
    configure(recipe);render(view,180);
    const lit=volumeDraws().map(d=>[...d.v]);assert(lit.length);
    configure(noEffects+lights.replace(/^illumination=.*$/gm,'illumination=0')+air);render(view,180);
    const dark=volumeDraws().map(d=>[...d.v]);
    assert.notDeepEqual(lit,dark,'haze and crystals catch ice-window illumination');
    configure(noEffects+lights.replace(/^scatter=.*$/gm,'scatter=0')+air);render(view,180);
    assert.deepEqual(volumeDraws().map(d=>[...d.v]),lit,'diffusion does not alter airborne lighting');
    configure(noEffects+lights.replace(/^light-scenery=1$/gm,'light-scenery=0')+air);render(view,180);
    assert.deepEqual(volumeDraws().map(d=>[...d.v]),dark,'scenery receiver gates airborne lighting');
    configure(recipe);render(view,180);const allocated=creates;
    render(view,227);assert.notDeepEqual(volumeDraws().map(d=>[...d.v]),lit,'haze and ice drift slowly');
    render(view,180);assert.deepEqual(volumeDraws().map(d=>[...d.v]),lit,'seeking restores ice and haze exactly');
    assert.equal(creates,allocated,'air layers reuse the existing atlas and render targets');
    api.RoomPreview_EnableEffects(0);render(view,180);assert.equal(api.RoomPreview_EffectVertices(),0);
    api.RoomPreview_EnableEffects(1);
  }
}
console.log('Northwall cave air: both rooms/all terrains, ice-window illumination, diffusion independence, aspects/modes, motion, seeking, toggle and texture reuse passed.');
const dripTimes=new Map();
for(const map of [2,4])for(const terrain of [0,1,2]) {
  const base=data.rooms.find(r=>r.group===6&&r.map===map);
  const room={...base,terrainProfile:terrain,bg:[base.terrainVariants.find(v=>v.profile===terrain).bg1,base.bg[1]]};
  assert.equal(send(encode(data,blobs,room),api.RoomPreview_Load),1);
  const blocks=shippedBlocks.filter(b=>b.startsWith(`[emitter:06:0${map}:${terrain}:`));
  const reflected=blocks.filter(b=>b.includes(':soft-light:')&&/^ripple=1$/m.test(b)).join('');
  const drips=blocks.filter(b=>/:drips:0602d/i.test(b));
  assert(reflected,'both water rooms retain reflected light');
  assert.equal(drips.length,map===2?4:0,'visible drops belong to foreground cave icicles; distant boss-room ice has no drips');
  // Use one source's footprint for actors; the full recipe still illuminates scenery.
  const first=properties(blocks.find(b=>b.includes(':soft-light:')&&/^ripple=1$/m.test(b)));
  const lx=Number(first.x),ly=Number(first.y);
  camera=[Math.max(0,lx-128),Math.max(0,ly-112)];baselineDistance=3.25;
  assert.equal(api.RoomPreview_SetReceivers(3,lx-8,ly+Number(first.height)*.12+8,lx+8,ly+Number(first.height)*.12+8),1);
  const lightEnergy=()=>{const actor=draws.find(d=>d.effect===5);assert(actor);return [sceneryEnergy(),roleEnergy(actor,0),roleEnergy(actor,1)];};
  for(const height of [360,400])for(const mode of [0,1,2]) {
    const view=[640,height,1,0,0,mode,0];
    configure(noEffects+reflected);render(view,180);const a=lightEnergy(),allocated=creates;
    assert(a.every(n=>n>0),'reflected light reaches scenery, player and enemy');
    render(view,227);assert.notDeepEqual(lightEnergy(),a,'reflected illumination wavers with the water cycle');
    render(view,180);assert.deepEqual(lightEnergy(),a,'reflection seeking is exact');
    configure(noEffects+reflected.replace(/^ripple=1$/gm,'ripple=0'));render(view,180);const fixed=lightEnergy();
    render(view,227);assert.deepEqual(lightEnergy(),fixed,'motion off restores static soft lights');
    assert.equal(creates,allocated,'ripple motion needs no new textures');
  }
  assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
  if(!drips.length)continue;
  const drop=drips[0],d=properties(drop),bottom=Number(d.y)+Number(d.height)/2;
  camera=[Math.max(0,Number(d.x)-128),Math.max(0,bottom-190)];
  configure(noEffects+drop);
  if(!dripTimes.has(map)) {
    const times={};
    for(let tick=0;tick<Number(d.lifetime);tick+=4) {
      render([640,400,1,0,0,1,0],tick);
      const state=api.RoomPreview_EffectVertices()===0?'quiet':volumeDraws().length?'ripple':'fall';
      times[state]??=tick;
      if(Object.keys(times).length===3)break;
    }
    assert.equal(Object.keys(times).length,3,'a sparse drip has separate quiet, falling and water-ring phases');
    dripTimes.set(map,times);
  }
  const times=dripTimes.get(map);
  for(const height of [360,400])for(const mode of [0,1,2]) {
    const view=[640,height,1,0,0,mode,0];
    render(view,times.quiet);assert.equal(api.RoomPreview_EffectVertices(),0);
    render(view,times.fall);assert(api.RoomPreview_EffectVertices()>0);assert.equal(volumeDraws().length,0);
    render(view,times.ripple);const ring=volumeDraws().map(d=>[...d.v]);assert(ring.length);
    const allocated=creates;
    render(view,times.quiet);render(view,times.ripple);
    assert.deepEqual(volumeDraws().map(d=>[...d.v]),ring,'water landing survives all background modes and seeking');
    assert.equal(creates,allocated,'water rings reuse the existing mask atlas');
  }
  api.RoomPreview_EnableEffects(0);render();assert.equal(api.RoomPreview_EffectVertices(),0);api.RoomPreview_EnableEffects(1);
}
console.log('Northwall reflected light/drips: all terrains/aspects/modes, moving illumination, foreground-only drip placement, static reset, quiet/fall/ripple phases, seek and texture reuse passed.',Object.fromEntries(dripTimes));
assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);api.RoomPreview_Reset();
assert.equal(textures.size,0,'room reset releases all preview textures');
console.log('Shared player preview: native art, receiver masks, dimming/torch/rim, picking, mirroring and upload reuse passed.');
