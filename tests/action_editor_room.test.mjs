/* Full room navigation + animation through real C PPU/compositor, without a
 * captured scene. Host imports validate all submitted resources/geometry. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
const [htmlFile,wasmFile,replay,directory]=process.argv.slice(2);
const html=fs.readFileSync(htmlFile,'utf8');
const data=JSON.parse(html.match(/window\.__ACTION_BG__=(.*?);<\/script>/s)[1]);
const ini=JSON.parse(html.match(/window\.__DIORAMA_LAYERS__=(.*?);window\.__DIORAMA_LAYERS_NAME__/s)[1]);
const iniFile=path.join(directory,'layers.ini');fs.writeFileSync(iniFile,ini);
const encoder=vm.createContext({window:{},Uint8Array,Uint32Array,DataView});
vm.runInContext(fs.readFileSync('tools/action_editor/shared_preview.js','utf8'),encoder);
const encode=vm.runInContext('SharedActionPreview.encode',encoder);
const blobs=data.blobs.map(s=>Uint8Array.from(Buffer.from(s,'base64')));
const textures=new Map();let api,target=0,creates=0,draws=0,effect=-1,updates=0;
const imports={
 create(w,h,usage){let id=1;while(textures.has(id))id++;assert(id<128);textures.set(id,{w,h,usage});creates++;return id;},
 destroy(id){assert(textures.delete(id));},
 update(id,x,y,w,h,p,pitch){const t=textures.get(id);assert(t&&x>=0&&y>=0&&x+w<=t.w&&y+h<=t.h&&pitch>=w*4);
   assert(p>0&&p+(h-1)*pitch+w*4<=api.memory.buffer.byteLength);updates++;return 1;},
 target(id){assert(!id||textures.get(id)?.usage===2);target=id;return 1;},
 viewport(){return 1;},clip(){return 1;},clear(){return 1;},
 geometry(id,p,n,ip,ni){assert(!id||(textures.has(id)&&id!==target));
   assert(n>0&&ni>0&&new Float32Array(api.memory.buffer,p,n*8).every(Number.isFinite));
   assert(new Int32Array(api.memory.buffer,ip,ni).every(i=>i>=0&&i<n));draws++;return 1;},
 effect(kind,p,n){assert(kind>=-1&&kind<4);if(kind>=0){assert.equal(effect,-1);assert(new Float32Array(api.memory.buffer,p,n).every(Number.isFinite));}effect=kind;return 1;},
 available(){return 1;}
};
const wasi={fd_close:()=>0,fd_seek:()=>0,fd_write:()=>0,proc_exit:n=>{throw Error(`exit ${n}`);}};
const instance=await WebAssembly.instantiate(fs.readFileSync(wasmFile),{ar:imports,wasi_snapshot_preview1:wasi});
api=instance.instance.exports;api._initialize?.();assert.equal(api.DioramaPreview_Init(16384),1);
const memory=api.memory.buffer;
function send(bytes,fn){new Uint8Array(memory,api.DioramaPreview_Input(),bytes.length).set(bytes);return fn(bytes.length);}
let scenes=0,frames=0;
for(const base of data.rooms)for(const terrain of base.terrainVariants){
 const room={...base,bg:[terrain.bg1,base.bg[1]],terrainProfile:terrain.profile};
 const packet=encode(data,blobs,room);assert.equal(send(packet,api.RoomPreview_Load),1);
 assert.equal(send(Buffer.from(ini),api.RoomPreview_Configure),1,`configuration ${base.group}:${base.map}`);
 const source=api.RoomPreview_SkyboxRoom();
 if(source){const sky=data.rooms.find(r=>r.group===(source>>>16)&&r.map===((source>>>8)&255));
   assert(sky);assert.equal(send(encode(data,blobs,sky),api.RoomPreview_LoadSkybox),1);}
 const x=room.bg[0].pagesWide*256-256,y=room.bg[0].pagesHigh*256-225;
 const requests=[[0,0,0,0,0],[x>>1,y>>1,37,120,64],[x,y,65535,128,32],
                 [x>>1,y>>1,37,120,64],[0,0,511,128,64],[x,y,0,0,0]];
 const hashes=[];
 for(const [i,request] of requests.entries()){
   assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1,`render ${base.group}:${base.map}/${terrain.profile}/${i}`);
   hashes.push(api.RoomPreview_Hash()>>>0);
   const allocations=creates,uploadCount=updates;
   assert.equal(api.RoomPreview_Render(...request,960,540,1.1,0.1,-0.1,i%3,i%2),1);
   // Targets can resize with presentation, but the CPU surfaces must not upload again.
   assert.equal(api.RoomPreview_Uploads(),0,'orbit/aspect must reuse source pixels');
   assert.equal(updates,uploadCount);
   const warm=creates;
   assert.equal(api.RoomPreview_Render(...request,960,540,1.1,0.1,-0.1,i%3,i%2),1);
   assert.equal(creates,warm,'repeat draw creates no textures');
   assert.equal(api.RoomPreview_Hash()>>>0,hashes.at(-1));
   assert(creates>=allocations);assert.equal(target,0);assert.equal(effect,-1);
 }
 assert.equal(hashes[1],hashes[3],'reverse time is deterministic');
 const hash=hashes.at(-1), live=textures.size;
 assert.equal(send(Buffer.from('[bad'),api.RoomPreview_Configure),0);
 const broken=Uint8Array.from(packet);broken[0]^=1;
 assert.equal(send(broken,api.RoomPreview_Load),0);
 assert.equal(api.RoomPreview_Hash()>>>0,hash);assert.equal(textures.size,live);
 const file=path.join(directory,'room.arscene');fs.writeFileSync(file,packet);
 const native=execFileSync(replay,[file,iniFile],{input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>parseInt(n,16));
 assert.deepEqual(hashes,native,`native surfaces ${base.group}:${base.map}/${terrain.profile}`);
 frames+=requests.length;scenes++;
}
assert.equal(scenes,147);assert.equal(api.memory.buffer,memory);
api.RoomPreview_Reset();assert.equal(textures.size,0);api.RoomPreview_Reset();
assert.equal(api.RoomPreview_Hash(),0);
console.log(`Whole-room renderer: ${scenes} regional rooms, ${frames} native/WASM surface matches, ${draws} valid draws; reverse time, atomic edits/loads, resource reuse and teardown passed.`);
