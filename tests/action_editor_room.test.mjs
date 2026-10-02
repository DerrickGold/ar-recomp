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
const stagePresets=JSON.parse(html.match(/window\.__ACTION_EFFECT_PRESETS__=(.*?);window\.__ACTION_VIEW__/s)[1]);
let scenes=0,frames=0,presetChecks=0;
for(const base of data.rooms)for(const terrain of base.terrainVariants){
 const room={...base,bg:[terrain.bg1,base.bg[1]],terrainProfile:terrain.profile};
 const packet=encode(data,blobs,room);assert.equal(send(packet,api.RoomPreview_Load),1);
 assert.equal(send(Buffer.from(ini),api.RoomPreview_Configure),1,`configuration ${base.group}:${base.map}`);
 const beforeCatalogue=[api.RoomPreview_Hash(),api.RoomPreview_EffectHash(),creates,updates,draws];
 const catalogueCount=api.RoomPreview_CatalogueCount(),catalogueKeys=new Set();
 assert(catalogueCount<1024);
 for(let i=0;i<catalogueCount;i++) {
   const key=[0,1].map(f=>api.RoomPreview_CatalogueValue(i,f)).join(':');
   assert(!catalogueKeys.has(key));catalogueKeys.add(key);
   for(const field of [2,3,4,5,8,9])assert(Number.isFinite(api.RoomPreview_CatalogueValue(i,field)));
   for(const field of [8,9])assert(api.RoomPreview_CatalogueValue(i,field)>0);
   for(let m=0;m<api.RoomPreview_CatalogueValue(i,7);m++) {
     assert.equal(api.RoomPreview_CatalogueMemberValue(i,m,0),m+1);
     for(const field of [1,2,3,4,6,7,8,9,10])assert(Number.isFinite(api.RoomPreview_CatalogueMemberValue(i,m,field)));
   }
 }
 assert.equal(api.RoomPreview_CatalogueCount(),catalogueCount);
 assert.deepEqual([api.RoomPreview_Hash(),api.RoomPreview_EffectHash(),creates,updates,draws],beforeCatalogue,
   'whole-room inventory never renders, allocates textures or mutates the live frame');
 if(base.group===1&&[1,2].includes(base.map))assert(catalogueCount>0);
 if(base.group===2&&base.map===1) {
   const anchored=[];
   for(let i=0;i<catalogueCount;++i)if(api.RoomPreview_CatalogueValue(i,10)===1)
     anchored.push([api.RoomPreview_CatalogueValue(i,2),api.RoomPreview_CatalogueValue(i,3)]);
   assert.deepEqual(anchored,[[112,62],[112,62]],'moon and cloud share a static BG2 source');
   const guides=new Map();
   for(let i=0;i<catalogueCount;++i){
     const id=api.RoomPreview_CatalogueValue(i,0)>>>0;
     const regions=Array.from({length:api.RoomPreview_CatalogueGuideCount(i)},(_,g)=>
       [0,1,2,3].map(f=>api.RoomPreview_CatalogueGuideValue(i,g,f)));
     guides.set(id,regions);
   }
   for(const id of [0xb1000000,0xb1000001])assert.equal(guides.get(id).length,8,'all shoreline spans have guides');
   assert.deepEqual(guides.get(0xb1000000)[0],[528,499.5,704,23]);
   assert.deepEqual(guides.get(0xb1000001)[0],[528,463,704,46]);
   assert.equal(guides.get(0xb1000005).length,16,'insect regions on both banks of each span');
   assert.deepEqual(guides.get(0xb1000005)[0],[200,457,28,22]);
   assert(guides.get(0xb1000004).length>10,'wet timber comes from real terrain contacts throughout the room');
   assert(guides.get(0xb1000004).some(r=>r[0]>3000),'inventory must not depend on the preview camera');
   assert(guides.get(0xb1000004).every(r=>r[1]>100&&r[2]>=4));
   assert.deepEqual(guides.get(0xb1000002)[0],[112,62,16,16]);
   assert.deepEqual(guides.get(0xb1000006)[0],[88,60,170,36]);
 }
 const source=api.RoomPreview_SkyboxRoom();
 if(source){const sky=data.rooms.find(r=>r.group===(source>>>16)&&r.map===((source>>>8)&255));
   assert(sky);assert.equal(send(encode(data,blobs,sky),api.RoomPreview_LoadSkybox),1);}
 const x=room.bg[0].pagesWide*256-256,y=room.bg[0].pagesHigh*256-225;
 const requests=[[0,0,0,0,0],[x>>1,y>>1,37,120,64],[x,y,65535,128,32],
                 [x>>1,y>>1,37,120,64],[0,0,511,128,64],[x,y,0,0,0]];
 const hashes=[],effectHashes=[];
 for(const [i,request] of requests.entries()){
   assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1,`render ${base.group}:${base.map}/${terrain.profile}/${i}`);
   hashes.push(api.RoomPreview_Hash()>>>0);effectHashes.push(api.RoomPreview_EffectHash()>>>0);
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
 const native=execFileSync(replay,[file,iniFile],{input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
 assert.deepEqual(hashes.map((h,i)=>[h,effectHashes[i]]),native,`native surfaces ${base.group}:${base.map}/${terrain.profile}`);

 // Aitos surfaces use the same bounded capture and mesh kernels from complete data.
 if(base.group===4&&[1,2,3,4,6].includes(base.map)){
   const indices=base.map===1?[0]:[2,3].includes(base.map)?[2,3,4]:[1];
   const names=['lava-pit','lava-lake','splash','waterfall','waterfall-mist'];
   let scoped='[effects]\nversion=1\n';
   for(const index of indices){const ptr=api.RoomPreview_SurfaceFieldText(index);assert(ptr);
     let end=ptr;const bytes=new Uint8Array(memory);while(bytes[end])end++;
     scoped+=`[field:04:${base.map.toString(16).padStart(2,'0')}:${terrain.profile}:${names[index]}-field:00000000]\n`+new TextDecoder().decode(bytes.subarray(ptr,end));}
   assert.equal(send(Buffer.from(scoped),api.RoomPreview_ConfigureEffects),1);
   for(const [i,request] of requests.entries()){
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,hashes[i]);
     assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[i],`Aitos surface reconstruction ${base.map}/${terrain.profile}/${i}`);
   }
   const file=path.join(directory,'complete-surfaces.ini');fs.writeFileSync(file,scoped);
   const configured=execFileSync(replay,[path.join(directory,'room.arscene'),iniFile,file],
     {input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(configured,native,'Aitos surface native/WASM round trip');
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 if(base.group===2&&base.map===1){
   const binding=`[effects]\nversion=1\n[emitter:02:01:${terrain.profile}:flame:12345678]\nx=0\ny=-6\nwidth=64\nheight=96\nparticles=12\nlifetime=80\ncolor=ff6628\ncolor-end=ffe6a0\nactor-target=family\nactor-source=B786\nactor-parent=B786\nactor-state=0,1\nactor-animation=5000\nactor-limit=4\n`;
   assert.equal(send(Buffer.from(binding),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[1],'binding without a matching actor adds no render instance');
   assert.equal(api.RoomPreview_PreviewBinding(1,0x12345678,(x>>1)+128,(y>>1)+112,37),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   const boundHash=api.RoomPreview_EffectHash()>>>0;assert.notEqual(boundHash,effectHashes[1]);
   const attached=[];
   for(let i=0;i<api.RoomPreview_EffectCount();++i)if(api.RoomPreview_SourceValue(i,5))attached.push(api.RoomPreview_SourceValue(i,0)>>>0);
   assert.deepEqual(attached,[0x12345678],'actor instances preserve their editable definition identity');
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);assert.equal(api.RoomPreview_EffectHash()>>>0,boundHash);
   assert.equal(api.RoomPreview_PreviewBinding(0,0,0,0,0),1);
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 // A complete forest recipe replaces native capture, rather than overlaying
 // another set of emitters. Config-owned parameters participate in the digest.
 if(base.group===1&&base.map===1) {
   for(const axis of [0,1])assert.equal(api.RoomPreview_RayFieldMapScale(axis),api.RoomPreview_CatalogueValue(0,8+axis));
   const ptr=api.RoomPreview_RayFieldText();assert(ptr);
   let end=ptr;const bytes=new Uint8Array(memory);while(bytes[end])end++;
   const definition=new TextDecoder().decode(bytes.subarray(ptr,end));
   const scoped=`[effects]\nversion=1\n[field:01:01:${terrain.profile}:ray-field:46000000]\n`+definition;
   assert.equal(send(Buffer.from(scoped),api.RoomPreview_ConfigureEffects),1);
   for(const [i,request] of requests.entries()) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,hashes[i]);
     assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[i],`complete forest reconstruction terrain ${terrain.profile}/${i}`);
   }
   const file=path.join(directory,'complete-rays.ini');fs.writeFileSync(file,scoped);
   const configNative=execFileSync(replay,[path.join(directory,'room.arscene'),iniFile,file],
     {input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(configNative,native,'full recipe native/WASM round trip');
   const modified=scoped.replace(/^sway-amplitude=.*$/m,'sway-amplitude=20');
   assert.equal(send(Buffer.from(modified),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   assert.notEqual(api.RoomPreview_EffectHash()>>>0,effectHashes[1],'full parameter edits cannot be hidden by source-only hashes');
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 if(base.group===2||(base.group===5&&base.map>=4&&base.map<=8)||(base.group===7&&base.map===6)) {
   const ptr=api.RoomPreview_GlowFieldText();assert(ptr);
   let end=ptr;const bytes=new Uint8Array(memory);while(bytes[end])end++;
   const definition=new TextDecoder().decode(bytes.subarray(ptr,end));
   const scoped=`[effects]\nversion=1\n[field:${base.group.toString(16).padStart(2,'0')}:${base.map.toString(16).padStart(2,'0')}:${terrain.profile}:glow-field:54000000]\n`+definition;
   assert.equal(send(Buffer.from(scoped),api.RoomPreview_ConfigureEffects),1);
   for(const [i,request] of requests.entries()) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,hashes[i]);
     assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[i],`complete glow reconstruction ${base.map}/${terrain.profile}/${i}`);
   }
   const file=path.join(directory,'complete-glow.ini');fs.writeFileSync(file,scoped);
   const configured=execFileSync(replay,[path.join(directory,'room.arscene'),iniFile,file],
     {input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(configured,native,'glow native/WASM round trip');
   assert.equal(send(Buffer.from(scoped.replace(/^spill-centre=.*$/m,'spill-centre=.1 .2 .3 .4')),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   assert.notEqual(api.RoomPreview_EffectHash()>>>0,effectHashes[1]);
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 if(base.group===2&&base.map>=2&&base.map<=8) {
   const ptr=api.RoomPreview_CastleFieldText();assert(ptr);
   let end=ptr;const bytes=new Uint8Array(memory);while(bytes[end])end++;
   const definition=new TextDecoder().decode(bytes.subarray(ptr,end));
   const scoped=`[effects]\nversion=1\n[field:02:${base.map.toString(16).padStart(2,'0')}:${terrain.profile}:castle-field:ca00000${base.map}]\n`+definition;
   assert.equal(send(Buffer.from(scoped),api.RoomPreview_ConfigureEffects),1);
   for(const [i,request] of requests.entries()) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,hashes[i]);
     assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[i],`complete castle reconstruction ${base.map}/${terrain.profile}/${i}`);
   }
   const file=path.join(directory,'complete-castle.ini');fs.writeFileSync(file,scoped);
   const configured=execFileSync(replay,[path.join(directory,'room.arscene'),iniFile,file],
     {input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(configured,native,'castle native/WASM round trip');
   assert.equal(send(Buffer.from(scoped.replace(/^upper-color=.*$/m,'upper-color=.1 .2 .3')),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   assert.notEqual(api.RoomPreview_EffectHash()>>>0,effectHashes[1]);
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 // The complete linked moon field retains aggregate source and layer order.
 if(base.group===2&&base.map===1) {
   const ptr=api.RoomPreview_MarshFieldText();assert(ptr);
   let end=ptr;const bytes=new Uint8Array(memory);while(bytes[end])end++;
   const definition=new TextDecoder().decode(bytes.subarray(ptr,end));
   const scoped=`[effects]\nversion=1\n[field:02:01:${terrain.profile}:marsh-field:b1000000]\n`+definition;
   assert.equal(send(Buffer.from(scoped),api.RoomPreview_ConfigureEffects),1);
   for(const [i,request] of requests.entries()) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,hashes[i]);
     assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[i],`complete marsh reconstruction ${terrain.profile}/${i}`);
   }
   const file=path.join(directory,'complete-marsh.ini');fs.writeFileSync(file,scoped);
   const configured=execFileSync(replay,[path.join(directory,'room.arscene'),iniFile,file],
     {input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(configured,native,'marsh field native/WASM round trip');
   const modified=scoped.replace(/^water-color=.*$/m,'water-color=.1 .2 .3');
   assert.equal(send(Buffer.from(modified),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   assert.notEqual(api.RoomPreview_EffectHash()>>>0,effectHashes[1]);
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 if(base.group===2&&base.map===1) {
   const ptr=api.RoomPreview_MoonFieldText();assert(ptr);
   let end=ptr;const bytes=new Uint8Array(memory);while(bytes[end])end++;
   const definition=new TextDecoder().decode(bytes.subarray(ptr,end));
   const scoped=`[effects]\nversion=1\n[field:02:01:${terrain.profile}:moon-field:b1000002]\n`+definition;
   assert.equal(send(Buffer.from(scoped),api.RoomPreview_ConfigureEffects),1);
   for(const [i,request] of requests.entries()) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,hashes[i]);
     assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[i],`complete moon reconstruction ${terrain.profile}/${i}`);
   }
   const file=path.join(directory,'complete-moon.ini');fs.writeFileSync(file,scoped);
   const configured=execFileSync(replay,[path.join(directory,'room.arscene'),iniFile,file],
     {input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(configured,native,'moon field native/WASM round trip');
   const modified=scoped.replace(/^ray-color=.*$/m,'ray-color=.1 .2 .3');
   assert.equal(send(Buffer.from(modified),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   assert.notEqual(api.RoomPreview_EffectHash()>>>0,effectHashes[1]);
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 // The complete damp-surface field retains aggregate source and layer order.
 if(base.group===1&&base.map===2) {
   const ptr=api.RoomPreview_WaterFieldText();assert(ptr);
   let end=ptr;const bytes=new Uint8Array(memory);while(bytes[end])end++;
   const definition=new TextDecoder().decode(bytes.subarray(ptr,end));
   const scoped=`[effects]\nversion=1\n[field:01:02:${terrain.profile}:water-field:c2000200]\n`+definition;
   assert.equal(send(Buffer.from(scoped),api.RoomPreview_ConfigureEffects),1);
   for(const [i,request] of requests.entries()) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,hashes[i]);
     assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[i],`complete water reconstruction ${terrain.profile}/${i}`);
   }
   const file=path.join(directory,'complete-water.ini');fs.writeFileSync(file,scoped);
   const configured=execFileSync(replay,[path.join(directory,'room.arscene'),iniFile,file],
     {input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(configured,native,'water field native/WASM round trip');
   const modified=scoped.replace(/^spray-shape=.*$/m,'spray-shape=48 40 2 .24');
   assert.equal(send(Buffer.from(modified),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   assert.notEqual(api.RoomPreview_EffectHash()>>>0,effectHashes[1]);
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 // Three independent atmosphere definitions reproduce native rooms in all terrains.
 if(base.group===1&&base.map>=2&&base.map<=4) {
   const ptr=api.RoomPreview_AtmosphereFieldText();assert(ptr);
   let end=ptr;const bytes=new Uint8Array(memory);while(bytes[end])end++;
   const definition=new TextDecoder().decode(bytes.subarray(ptr,end));
   const scoped=`[effects]\nversion=1\n[field:01:${base.map.toString(16).padStart(2,'0')}:${terrain.profile}:atmosphere-field:c200${base.map.toString(16).padStart(2,'0')}00]\n`+definition;
   assert.equal(send(Buffer.from(scoped),api.RoomPreview_ConfigureEffects),1);
   for(const [i,request] of requests.entries()) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,hashes[i]);
     assert.equal(api.RoomPreview_EffectHash()>>>0,effectHashes[i],`complete atmosphere reconstruction ${terrain.profile}/${i}`);
   }
   const file=path.join(directory,'complete-atmosphere.ini');fs.writeFileSync(file,scoped);
   const configured=execFileSync(replay,[path.join(directory,'room.arscene'),iniFile,file],
     {input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(configured,native,'atmosphere field native/WASM round trip');
   const modified=scoped.replace(/^dust-color=.*$/m,'dust-color=.1 .2 .3');
   assert.equal(send(Buffer.from(modified),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   assert.notEqual(api.RoomPreview_EffectHash()>>>0,effectHashes[1]);
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }
 // Authored emitters round-trip through the identical native resolver.
 const scope=`${base.group.toString(16).padStart(2,'0')}:${base.map.toString(16).padStart(2,'0')}:${terrain.profile}`;
 let recipes='[effects]\nversion=1\n';
 for(const [i,kind] of ['soft-light','motes','free-mist','ground-mist'].entries()) {
   if(kind==='ground-mist'&&base.group===1&&base.map===2)
     recipes+=`[emitter:${scope}:${kind}:${i+1}]\nx=1280\ny=1168\nwidth=480\nheight=160\ncolor=91bedf\n`;
   else recipes+=`[emitter:${scope}:${kind}:${i+1}]\nx=${(x>>1)+128}\ny=${(y>>1)+112}\ncolor=aabbff\n`;
   if(kind==='motes')recipes+='size-min=.65\nsize-max=1.25\ntravel-x=60\ntravel-y=-96\nwander=5\nspread=.6\nseed=4294967295\ncolor-end=91bedf\n';
 }
 assert.equal(api.RoomPreview_CollisionGrid(),api.RoomPreview_Width()*api.RoomPreview_Height()/256);
 for(const [i,kind] of ['particle-area','light-fan','water-surface','drips','waterfall-spray','cloud-bank','exposure','wet-contour'].entries()) {
   const bg2=['light-fan','water-surface','cloud-bank'].includes(kind);
   recipes+=`[emitter:${scope}:${kind}:${i+5}]\nx=${bg2?112:(x>>1)+128}\ny=${bg2?(kind==='water-surface'?192:62):(y>>1)+112}\ncolor=aabbff\n`;
   if(bg2)recipes+=`anchor=${kind==='water-surface'?'bg2-raster':'bg2-point'}\nplacement=background\n`;
   if(kind==='particle-area')recipes+='particles=2\nwidth=2048\nheight=2048\npattern=snow\n';
   if(kind==='light-fan')recipes+='strands=4\nangle=12\nlight-player=1\n';
   if(kind==='cloud-bank')recipes+='strands=4\ndrift=12\namplitude=4\n';
   if(['water-surface','drips','waterfall-spray'].includes(kind))recipes+='particles=8\n';
   if(kind==='exposure')recipes+='dim-player=1\ndim-enemies=0\nintensity=.2\n';
   if(kind==='wet-contour')recipes+='points=-32,12 0,-12 32,12\n';
 }
 // Individual native member IDs stay independent of camera and recipe state.
 let memberCount=0;
 for(let i=0;i<api.RoomPreview_EffectCount();i++) {
   const count=api.RoomPreview_MemberCount(i);memberCount+=count;
   if(!count)continue;
   const kind=api.RoomPreview_SourceValue(i,1),p=api.RoomPreview_KindName(kind),bytes=new Uint8Array(memory);
   let end=p;while(bytes[end])end++;
   const name=new TextDecoder().decode(bytes.subarray(p,end));
   assert.equal(api.RoomPreview_MemberValue(i,0,0),1);
   const anchor=[1,2,3,4].map(f=>api.RoomPreview_MemberValue(i,0,f));assert(anchor.every(Number.isFinite));
   recipes+=`[member:${scope}:${name}:1]\noffset-x=12\noffset-y=4\ncolor=ccddff\n`;
   if(['forest-canopy','forest-forward','castle-light'].includes(name))recipes+='angle=-6\nwidth-scale=1.2\n';
 }
 if(base.group===1&&base.map===1)assert.equal(memberCount,24);
 const effectFile=path.join(directory,'effects.ini');fs.writeFileSync(effectFile,recipes);
 assert.equal(send(Buffer.from(recipes),api.RoomPreview_ConfigureEffects),1);
 assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
 if(base.group===1&&base.map===2) {
   let supports=0;
   for(let i=0;i<api.RoomPreview_EffectCount();i++)supports+=api.RoomPreview_SourceValue(i,6);
   assert.ok(supports>0,`temple floor supports ${scope}`);
 }
 const tuned=[api.RoomPreview_Hash()>>>0,api.RoomPreview_EffectHash()>>>0];
 assert.notEqual(tuned[1],effectHashes[1]);
 const nativeTuned=execFileSync(replay,[file,iniFile,effectFile],{input:requests[1].join(' ')+'\n',encoding:'utf8'}).trim().split(' ').map(v=>parseInt(v,16));
 assert.deepEqual(tuned,nativeTuned,`native effect recipes ${scope}`);
 assert.equal(api.RoomPreview_Render(...requests[4],960,600,1,0,0,1,0),1);
 assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
 assert.deepEqual([api.RoomPreview_Hash()>>>0,api.RoomPreview_EffectHash()>>>0],tuned,'authored reverse time');
 assert.equal(send(Buffer.from(recipes+'particles=999\n'),api.RoomPreview_ConfigureEffects),0);
 assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
 assert.equal(api.RoomPreview_EffectHash()>>>0,tuned[1]);
 if(base.group===1&&base.map===2&&terrain.profile===0) {
   assert.equal(api.RoomPreview_SetReceivers(1,(x>>1)+128,(y>>1)+112,(x>>1)+180,(y>>1)+112),1);
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
   const warm=creates;
   assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);assert.equal(creates,warm);
   assert.equal(api.RoomPreview_SetReceivers(0,0,0,0,0),1);
   let arcText='[effects]\nversion=1\n';
   for(const [index,name] of ['trap','bolt','centaur'].entries()){
     const pointer=api.RoomPreview_ArcFieldText(index);assert(pointer);let end=pointer;const bytes=new Uint8Array(memory);while(bytes[end])end++;
     arcText+=`[field:${scope}:${name}-field:00000000]\n`+new TextDecoder().decode(bytes.subarray(pointer,end));
   }
   assert.equal(send(Buffer.from(recipes+arcText.slice('[effects]\nversion=1\n'.length)),api.RoomPreview_ConfigureEffects),1);
   assert.equal(api.RoomPreview_EventCount(),19);
   for(let i=1;i<=api.RoomPreview_EventCount();i++) {
     assert.equal(api.RoomPreview_SetEvent(i,(x>>1)+128,(y>>1)+112,1,0,30,96,123),1);
     assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1,`event ${i}`);
     const eventHash=api.RoomPreview_EffectHash()>>>0;
     assert.equal(api.RoomPreview_Render(...requests[4],960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Render(...requests[1],960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_EffectHash()>>>0,eventHash);
   }
   assert.equal(api.RoomPreview_SetEvent(0,0,0,0,0,0,96,123),1);
 }
 for(const preset of stagePresets.filter(p=>p.rooms.some(([group,map])=>group===base.group&&map===base.map))) {
   const scope=`${base.group.toString(16).padStart(2,'0')}:${base.map.toString(16).padStart(2,'0')}:${terrain.profile}`;
   const centre=[(x>>1)+128,(y>>1)+112];
   const text='[effects]\nversion=1\n'+preset.members.map((m,i)=>{
     const {kind,x:dx=0,y:dy=0,...properties}=m;
     return `[emitter:${scope}:${kind}:${(0xf000+i).toString(16)}]\nx=${centre[0]+dx}\ny=${centre[1]+dy}\n`+
       Object.entries(properties).map(([key,value])=>`${key}=${value}\n`).join('');
   }).join('');
   assert.equal(send(Buffer.from(text),api.RoomPreview_ConfigureEffects),1,`${preset.id} parse line ${api.RoomPreview_RecipeErrorLine()}`);
   const observed=[];
   for(const request of requests) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1,`${preset.id} render`);
     observed.push([api.RoomPreview_Hash()>>>0,api.RoomPreview_EffectHash()>>>0]);
   }
   assert.deepEqual(observed[1],observed[3],`${preset.id} deterministic scrub`);
   const presetFile=path.join(directory,'stage-preset.ini');fs.writeFileSync(presetFile,text);
   const nativePreset=execFileSync(replay,[file,iniFile,presetFile],{input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'})
     .trim().split('\n').map(row=>row.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(observed,nativePreset,`${preset.id} native/WASM parity`);presetChecks++;
 }
 assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 frames+=requests.length;scenes++;
}
assert.equal(presetChecks,stagePresets.length*3);assert.equal(scenes,147);assert.equal(api.memory.buffer,memory);
api.RoomPreview_Reset();assert.equal(textures.size,0);api.RoomPreview_Reset();
assert.equal(api.RoomPreview_Hash(),0);
console.log(`Whole-room renderer: ${scenes} regional rooms, ${frames} native/WASM surface + source matches, ${scenes} authored round trips, ${presetChecks} stage preset reconstructions, ${draws} valid draws; reverse time, atomic edits/loads, resource reuse and teardown passed.`);
