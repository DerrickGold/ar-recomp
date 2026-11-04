/* Full room navigation + animation through real C PPU/compositor, without a
 * captured scene. Host imports validate all submitted resources/geometry. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import {createHash} from 'node:crypto';
const [htmlFile,wasmFile,directory]=process.argv.slice(2);
const html=fs.readFileSync(htmlFile,'utf8');
const data=JSON.parse(html.match(/window\.__ACTION_BG__=(.*?);<\/script>/s)[1]);
const ini=JSON.parse(html.match(/window\.__DIORAMA_LAYERS__=(.*?);window\.__DIORAMA_LAYERS_NAME__/s)[1]);
const iniFile=directory?path.join(directory,'layers.ini'):null;
// The UI sends only the selected room to the bounded WASM configuration API.
// A complete project's unrelated rooms can exceed its 1 MiB transport limit.
// Keep named sections too, and send the same room document to the native oracle.
function roomConfiguration(base) {
 let keep=false;
 return ini.split(/\r?\n/).filter(line=>{
   if(/^\s*\[/.test(line)) {
     const section=line.match(/^\s*\[layers:([\da-f]+):([\da-f]+)(?::[^\]]+)?\]/i);
     keep=!!section&&parseInt(section[1],16)===base.group&&parseInt(section[2],16)===base.map;
   }
   return keep;
 }).join('\n');
}
const encoder=vm.createContext({window:{},Uint8Array,Uint32Array,DataView});
vm.runInContext(fs.readFileSync('tools/action_editor/shared_preview.js','utf8'),encoder);
const encode=vm.runInContext('SharedActionPreview.encode',encoder);
const blobs=data.blobs.map(s=>Uint8Array.from(Buffer.from(s,'base64')));
const textures=new Map();let api,target=0,creates=0,draws=0,effect=-1,updates=0;
let shaftMotes=null, geometryHash=createHash('sha256');
const imports={
 create(w,h,usage){let id=1;while(textures.has(id))id++;assert(id<128);textures.set(id,{w,h,usage});creates++;return id;},
 destroy(id){assert(textures.delete(id));},
 update(id,x,y,w,h,p,pitch){const t=textures.get(id);assert(t&&x>=0&&y>=0&&x+w<=t.w&&y+h<=t.h&&pitch>=w*4);
   assert(p>0&&p+(h-1)*pitch+w*4<=api.memory.buffer.byteLength);updates++;return 1;},
 target(id){assert(!id||textures.get(id)?.usage===2);target=id;return 1;},
 viewport(){return 1;},clip(){return 1;},clear(){return 1;},
 geometry(id,p,n,ip,ni){assert(!id||(textures.has(id)&&id!==target));
   assert(n>0&&ni>0&&new Float32Array(api.memory.buffer,p,n*8).every(Number.isFinite));
   assert(new Int32Array(api.memory.buffer,ip,ni).every(i=>i>=0&&i<n));draws++;geometryHash.update(new Uint8Array(api.memory.buffer,p,n*32));geometryHash.update(new Uint8Array(api.memory.buffer,ip,ni*4));
   if(shaftMotes&&id===0&&target===0) {
     const vertices=new Float32Array(api.memory.buffer,p,n*8);
     for(let i=0;i+31<vertices.length;i+=8) {
       if(Math.abs(vertices[i+2]-1)>1e-5||Math.abs(vertices[i+3]-.95)>1e-5||Math.abs(vertices[i+4]-.72)>1e-5)continue;
       shaftMotes.set(vertices[i+5].toFixed(7),[(vertices[i]+vertices[i+16])/2,(vertices[i+1]+vertices[i+17])/2]);
       i+=24;
     }
   }
   return 1;},
 effect(kind,p,n){
   assert(kind>=-1&&kind<=6);
   if(kind>=0){
     assert.equal(effect,-1);assert.equal(n,[3,3,9,4,177,19,8][kind]);
     const params=new Float32Array(api.memory.buffer,p,n);assert(params.every(Number.isFinite));
     if(kind===4)assert(textures.has(params[176])&&params[176]!==target,'illumination map is a valid separate texture');
   }
   effect=kind;return 1;
 },
 available(){return 1;}
};
const wasi={fd_close:()=>0,fd_seek:()=>0,fd_write:()=>0,proc_exit:n=>{throw Error(`exit ${n}`);}};
const instance=await WebAssembly.instantiate(fs.readFileSync(wasmFile),{ar:imports,wasi_snapshot_preview1:wasi});
api=instance.instance.exports;api._initialize?.();assert.equal(api.DioramaPreview_Init(16384),1);
const memory=api.memory.buffer;
function send(bytes,fn){new Uint8Array(memory,api.DioramaPreview_Input(),bytes.length).set(bytes);return fn(bytes.length);}
assert.equal(send(Buffer.concat([data.player.characters,data.player.palette,data.player.picture]
  .map(index=>Buffer.from(blobs[index]))),api.RoomPreview_LoadPlayer),1);
function cstring(pointer) {
 const bytes=new Uint8Array(memory);let end=pointer;while(bytes[end])end++;
 return new TextDecoder().decode(bytes.subarray(pointer,end));
}

const effects=Buffer.from(JSON.parse(html.match(/window\.__ACTION_EFFECTS__=(.*?);window\.__ACTION_ACTORS__/s)[1]));
const conflicts=[
 '',
 'bg1-policy = edge:clamp horizontal:fixed left:0 right:0 vertical:fixed top:0 bottom:0\nbg2-policy = edge:mirror motion:fill\n',
 'bg1 = z:0.1 order:0 alpha:50 rake:0.2 bow:0.2 voxel:0.1 slices:8 dir:backward transparent:black\nbg2 = z:0.9 order:8 alpha:20\nbackdrop = source:rom-04-01-bg2\n',
 'bg1-virtual:us+jp+eu = cells:0,0-63,127 band:0\nbg2-virtual:us+jp+eu = cells:0,0-63,127 band:2\n',
];
let renders=0;
for(const map of [6,7])for(const terrainProfile of [0,1,2]) {
 const base=data.rooms.find(r=>r.group===6&&r.map===map),terrain=base.terrainVariants.find(t=>t.profile===terrainProfile);
 const packet=encode(data,blobs,{...base,bg:[terrain.bg1,base.bg[1]],terrainProfile});
 if(directory)fs.writeFileSync(path.join(directory,`06${map.toString(16).padStart(2,'0')}-${terrainProfile}.arscene`),packet);
 const baseline=new Map();
 for(let variant=0;variant<conflicts.length;++variant) {
  api.RoomPreview_Reset();assert.equal(send(packet,api.RoomPreview_Load),1);
  const config=Buffer.from(`[layers:06:0${map}]\ntrunk = enabled:1 depth:0.38 recess:0.24 wrap:1 gust:1 frost:1 bark:1\n${conflicts[variant]}`);
  assert.equal(send(config,api.RoomPreview_Configure),1,`variant ${variant} parses`);
  assert.equal(send(effects,api.RoomPreview_ConfigureEffects),1);
  assert.equal(api.RoomPreview_SetView(3.25,0,0,1,1),1);
  assert.equal(api.RoomPreview_SetReceivers(3,440,592,568,352),1);
  for(const sky of [0,1,2])for(const [x,y] of [[384,map===6?448:208],[256,0],[512,map===6?1823:1311]])
  for(const [yaw,pitch] of [[0,0],[-.7,.7],[.7,-.7]]) {
   const args=[x,y,420,120,64,960,600,1,yaw,pitch,sky,0],key=[x,y,yaw,pitch].join(',');
   geometryHash=createHash('sha256');assert.equal(api.RoomPreview_Render(...args),1);
   const digest={surfaces:api.RoomPreview_Hash(),geometry:geometryHash.copy().digest('hex')};
   if(variant===0&&sky===0)baseline.set(key,digest);
   else assert.deepEqual(digest,baseline.get(key),`own policy map=${map} terrain=${terrainProfile} variant=${variant} sky=${sky} pose=${key}`);
   const allocations=creates;
   assert.equal(api.RoomPreview_Render(...args),1);
   assert.equal(api.RoomPreview_Hash(),digest.surfaces);
   assert.equal(api.RoomPreview_Uploads(),0,'paused redraw reuses split sources');
   assert.equal(creates,allocations,'paused redraw reuses material');renders+=2;
  }
 }
 const disabled=[];
 for(const conflict of ['',conflicts[1]]) {
  assert.equal(send(Buffer.from(`[layers:06:0${map}]\ntrunk = enabled:0 depth:0.38 recess:0.24 wrap:1 gust:1 frost:1 bark:1\n${conflict}`),api.RoomPreview_Configure),1);
  assert.equal(api.RoomPreview_Render(256,448,420,120,64,960,600,1,0,0,1,0),1);
  disabled.push(api.RoomPreview_Hash());
 }
 assert.notEqual(...disabled,'disabled enclosure restores saved BG fill');
}
api.RoomPreview_Reset();assert.equal(textures.size,0,'all textures released');
console.log(JSON.stringify({rooms:2,terrains:3,variants:conflicts.length,skyModes:3,renders,draws,
 checks:'identical surfaces and geometry across conflicting BG fill/shape/source/band/sky policies; disabled fallback; orbit, pause and resource reuse'}));
