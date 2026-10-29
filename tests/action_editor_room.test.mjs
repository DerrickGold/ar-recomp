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
const iniFile=path.join(directory,'layers.ini');
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
let shaftMotes=null;
const imports={
 create(w,h,usage){let id=1;while(textures.has(id))id++;assert(id<128);textures.set(id,{w,h,usage});creates++;return id;},
 destroy(id){assert(textures.delete(id));},
 update(id,x,y,w,h,p,pitch){const t=textures.get(id);assert(t&&x>=0&&y>=0&&x+w<=t.w&&y+h<=t.h&&pitch>=w*4);
   assert(p>0&&p+(h-1)*pitch+w*4<=api.memory.buffer.byteLength);updates++;return 1;},
 target(id){assert(!id||textures.get(id)?.usage===2);target=id;return 1;},
 viewport(){return 1;},clip(){return 1;},clear(){return 1;},
 geometry(id,p,n,ip,ni){assert(!id||(textures.has(id)&&id!==target));
   assert(n>0&&ni>0&&new Float32Array(api.memory.buffer,p,n*8).every(Number.isFinite));
   assert(new Int32Array(api.memory.buffer,ip,ni).every(i=>i>=0&&i<n));draws++;
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
   assert(kind>=-1&&kind<=5);
   if(kind>=0){
     assert.equal(effect,-1);assert.equal(n,[3,3,9,4,173,19][kind]);
     const params=new Float32Array(api.memory.buffer,p,n);assert(params.every(Number.isFinite));
     if(kind===4)assert(textures.has(params[172])&&params[172]!==target,'illumination map is a valid separate texture');
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
const stagePresets=JSON.parse(html.match(/window\.__ACTION_EFFECT_PRESETS__=(.*?);window\.__ACTION_VIEW__/s)[1]);
let scenes=0,frames=0,presetChecks=0;
for(const base of data.rooms)for(const terrain of base.terrainVariants){
 const roomIni=roomConfiguration(base);
 fs.writeFileSync(iniFile,roomIni);
 const room={...base,bg:[terrain.bg1,base.bg[1]],terrainProfile:terrain.profile};
 const packet=encode(data,blobs,room);assert.equal(send(packet,api.RoomPreview_Load),1);
 assert.equal(send(Buffer.from(roomIni),api.RoomPreview_Configure),1,`configuration ${base.group}:${base.map}`);
 // Measurement must be cheap and independent of render history. Cover real
 // room policies/edits at native, wide and capped ultrawide monitor shapes.
 const untouched=[api.RoomPreview_Hash(),creates,updates,draws];
 const nx=room.bg[0].pagesWide*256-256,ny=room.bg[0].pagesHigh*256-225;
 for(const [w,h,mode] of [[960,720,0],[960,600,1],[960,540,1],[960,411,2],[960,270,2],[540,960,2]])
 for(const pixels of [0,1]) {
   assert.equal(api.RoomPreview_SetView(3.25,0,0,mode,1),1);
   const request=[nx+256,ny+256,37,120,64,w,h,1,0,0,1,pixels];
   assert.equal(api.RoomPreview_Measure(...request,0),16,`camera footprint ${base.group}:${base.map}`);
   const a=Array.from(new Float32Array(memory,api.DioramaPreview_Input(),16));
   assert(a.every(Number.isFinite));assert(a[0]>=0&&a[0]<=nx&&a[0]===a[3]);
   assert.equal(a[1],ny);assert.equal(a[4],ny);assert(a[7]>a[5]&&a[8]>a[6]);
   assert(a[14]<=w&&a[15]<=h,'viewport respects capped content and letterboxing');
   request[7]=1.4;request[8]=.2;request[9]=-.1;
   assert.equal(api.RoomPreview_Measure(...request,0),16);
   const inspect=Array.from(new Float32Array(memory,api.DioramaPreview_Input(),16));
   assert.deepEqual(inspect.slice(0,5),a.slice(0,5),'inspection pose cannot change gameplay travel');
   assert.equal(api.RoomPreview_Measure(...request,1),16);
   assert.equal(new Float32Array(memory,api.DioramaPreview_Input(),16)[9],2,'skybox fills without painted extension');
 }
 assert.deepEqual([api.RoomPreview_Hash(),creates,updates,draws],untouched,'coverage queries never scan out, upload or draw');
 assert.equal(api.RoomPreview_SetView(3.25,0,0,1,1),1);
 const beforeCatalogue=[api.RoomPreview_Hash(),api.RoomPreview_EffectHash(),creates,updates,draws];
 const catalogueCount=api.RoomPreview_CatalogueCount(),catalogueKeys=new Set();
 assert(catalogueCount<1024);
 for(let i=0;i<catalogueCount;i++) {
   const key=[0,1].map(f=>api.RoomPreview_CatalogueValue(i,f)).join(':');
   assert(!catalogueKeys.has(key));catalogueKeys.add(key);
   for(const field of [2,3,4,5,8,9])assert(Number.isFinite(api.RoomPreview_CatalogueValue(i,field)));
   for(const field of [8,9])assert(api.RoomPreview_CatalogueValue(i,field)>0);
   const name=cstring(api.RoomPreview_KindName(api.RoomPreview_CatalogueValue(i,1)));
   const guides=Array.from({length:api.RoomPreview_CatalogueGuideCount(i)},(_,g)=>
     [0,1,2,3].map(f=>api.RoomPreview_CatalogueGuideValue(i,g,f)));
   assert(guides.flat().every(Number.isFinite));
   if(!api.RoomPreview_CatalogueValue(i,6)&&!api.RoomPreview_CatalogueValue(i,7))
     assert(guides.some(g=>g[2]>0&&g[3]>0),`${base.group}:${base.map} ${name} must have a real spatial guide`);
   if(name==='temple-dust')assert(guides.every(([x,y,w,h])=>x-w/2>=0&&y-h/2>=0&&
     x+w/2<=room.bg[0].pagesWide*256&&y+h/2<=room.bg[0].pagesHigh*256),'dust sentinel bounds are clipped to the room');
   if(name==='castle-sky')assert.equal(api.RoomPreview_CatalogueValue(i,10),2,'castle sky follows the BG2 raster');
   if(name==='lava-pit'||name==='lava-lake') {
     const [x,y,w,h]=guides[0];
     assert(w>0&&h>0);assert(Math.abs(x-api.RoomPreview_CatalogueValue(i,2))<=w/2);
     assert(Math.abs(y-api.RoomPreview_CatalogueValue(i,3))<=h/2);
   }
   for(let m=0;m<api.RoomPreview_CatalogueValue(i,7);m++) {
     assert.equal(api.RoomPreview_CatalogueMemberValue(i,m,0),m+1);
     for(const field of [1,2,3,4,6,7,8,9,10])assert(Number.isFinite(api.RoomPreview_CatalogueMemberValue(i,m,field)));
   }
 }
 assert.equal(api.RoomPreview_CatalogueCount(),catalogueCount);
 assert.deepEqual([api.RoomPreview_Hash(),api.RoomPreview_EffectHash(),creates,updates,draws],beforeCatalogue,
   'whole-room inventory never renders, allocates textures or mutates the live frame');
 if(base.group===1&&[1,2].includes(base.map))assert(catalogueCount>0);
 if(base.group===1&&[2,3].includes(base.map)) {
   const floorGuides=()=>{
     const spans=[];
     for(let i=0;i<api.RoomPreview_CatalogueCount();++i) {
       if(cstring(api.RoomPreview_KindName(api.RoomPreview_CatalogueValue(i,1)))!=='floor-mist')continue;
       assert.equal(api.RoomPreview_CatalogueGuideCount(i),1);
       const [x,y,w,h]=[0,1,2,3].map(f=>api.RoomPreview_CatalogueGuideValue(i,0,f));
       assert.equal(x-w/2,api.RoomPreview_CatalogueValue(i,2),'guide starts at the native mist span');
       assert.equal(y+h/2,api.RoomPreview_CatalogueValue(i,3),'guide bottom rests on the supporting floor');
       assert.equal(w,api.RoomPreview_CatalogueValue(i,4));
       assert.equal(h,api.RoomPreview_CatalogueValue(i,5));
       spans.push([x,y,w,h]);
     }
     return spans;
   };
   const original=floorGuides();assert(original.length>0,'lower palace has supported mist spans');
   const definition=cstring(api.RoomPreview_AtmosphereFieldText());
   const area=definition.match(/^floor-area=(.*)$/m)[1].split(/\s+/).map(Number);area[0]+=16;
   const modified=definition.replace(/^floor-style=.*$/m,'floor-style=40 0.2 12')
     .replace(/^floor-area=.*$/m,'floor-area='+area.join(' '));
   const header=`[effects]\nversion=1\n[field:01:0${base.map}:${terrain.profile}:atmosphere-field:c2000${base.map}00]\n`;
   assert.equal(send(Buffer.from(header+modified),api.RoomPreview_ConfigureEffects),1);
   const changed=floorGuides();assert(changed.length>0);
   assert(changed.every(([x,y,w,h])=>h===40&&x-w/2>=area[0]),'guide height and floor coverage follow the active recipe');
   assert.equal(send(Buffer.from(header+modified.replace('floor-style=40','floor-style=nan')),api.RoomPreview_ConfigureEffects),0);
   assert.deepEqual(floorGuides(),changed,'invalid recipe leaves guide inventory intact');
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
   assert.deepEqual(floorGuides(),original,'removing the override restores native floor guides');
 }
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
   const allocations=creates;
   assert.equal(api.RoomPreview_Render(...request,960,540,1.1,0.1,-0.1,i%3,i%2),1);
   // Aspect/orbit can change guard capture or skybox placement even without
   // moving the camera. Repeating that resolved policy must reuse all pixels.
   const warm=creates,viewHash=api.RoomPreview_Hash(),viewUploads=updates;
   assert.equal(api.RoomPreview_Render(...request,960,540,1.1,0.1,-0.1,i%3,i%2),1);
   assert.equal(creates,warm,'repeat draw creates no textures');
   assert.equal(api.RoomPreview_Uploads(),0,'repeat draw reuses source pixels');
   assert.equal(updates,viewUploads);assert.equal(api.RoomPreview_Hash(),viewHash);
   assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
   assert.equal(api.RoomPreview_Hash()>>>0,hashes.at(-1),'returning to the original view restores the capture');
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


 // The distributed room compositions must use exactly the same recipes in
 // native rendering and the editor, across regional terrain and camera edges.
 if(base.group===3) {
   const shipped=JSON.parse(html.match(/window\.__ACTION_EFFECTS__=(.*?);window\.__ACTION_ACTORS__/s)[1]);
   assert.equal(shipped,fs.readFileSync('action-effects.ini','utf8'),'editor embeds the shipped effects document');
   const effectsFile=path.join(directory,'shipped-effects.ini');fs.writeFileSync(effectsFile,shipped);
   assert.equal(send(Buffer.from(shipped),api.RoomPreview_ConfigureEffects),1,'shipped recipes fit their instance/geometry budgets');
   const enhanced=[];
   for(const request of requests) {
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     enhanced.push([api.RoomPreview_Hash()>>>0,api.RoomPreview_EffectHash()>>>0]);
     for(const sky of [0,2])assert.equal(api.RoomPreview_Render(...request,720,450,1,0,0,sky,0),1);
   }
   const nativeEffects=execFileSync(replay,[file,iniFile,effectsFile],{
     input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'
   }).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
   assert.deepEqual(enhanced,nativeEffects,`shipped Kassandora effects ${base.map}/${terrain.profile}`);
   assert(enhanced.some((h,i)=>h[1]!==effectHashes[i]),'every Kassandora room receives its composition');
   assert.deepEqual(enhanced[1],enhanced[3],'authored animation is deterministic when scrubbing backwards');
   if(base.map===5) {
     const original=[...shipped.matchAll(new RegExp(`\\[emitter:03:05:${terrain.profile}:soft-light:[^\\]]+\\]([^\\[]+)`,'g'))]
       .find(match=>/^y=1424$/m.test(match[1]))[1];
     const pasted=shipped+`\n[emitter:03:05:${terrain.profile}:soft-light:f0000000]\n`+
       original.replace(/^x=.*$/m,'x=752').replace(/^y=.*$/m,'y=1392');
     assert.equal(send(Buffer.from(pasted),api.RoomPreview_ConfigureEffects),1,
       `Kassandora room 5 accepts copied soft lights beyond the old 16-instance limit: ${cstring(api.RoomPreview_RecipeError())}`);
     fs.writeFileSync(effectsFile,pasted);
     const pastedHashes=[];
     for(const request of requests) {
       assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
       pastedHashes.push([api.RoomPreview_Hash()>>>0,api.RoomPreview_EffectHash()>>>0]);
     }
     const nativePasted=execFileSync(replay,[file,iniFile,effectsFile],{
       input:requests.map(r=>r.join(' ')).join('\n')+'\n',encoding:'utf8'
     }).trim().split('\n').map(n=>n.split(' ').map(v=>parseInt(v,16)));
     assert.deepEqual(pastedHashes,nativePasted,'pasted soft lights render identically in native and WASM');
     // Budget validation must not depend on the current artistic composition
     // happening to be one copied light away from the geometry limit.
     const oversized='[effects]\nversion=1\n'+Array.from({length:96},(_,i)=>
       `[emitter:03:05:${terrain.profile}:soft-light:${i.toString(16)}]\n`).join('');
     assert.equal(send(Buffer.from(oversized),api.RoomPreview_ConfigureEffects),0);
     assert.match(cstring(api.RoomPreview_RecipeError()),/reserved effect vertices; the limit is 4096/);
     assert.equal(api.RoomPreview_Render(...requests.at(-1),960,600,1,0,0,1,0),1);
     assert.deepEqual([api.RoomPreview_Hash()>>>0,api.RoomPreview_EffectHash()>>>0],pastedHashes.at(-1),
       'geometry-budget failure preserves the last accepted room');
     const dense=count=>'[effects]\nversion=1\n'+Array.from({length:count},(_,i)=>
       `[emitter:03:05:${terrain.profile}:motes:${i.toString(16)}]\nparticles=1\n`).join('');
     assert.equal(send(Buffer.from(dense(96)),api.RoomPreview_ConfigureEffects),1);
     assert.equal(api.RoomPreview_Render(0,0,37,128,64,960,600,1,0,0,1,0),1);
     const authored=Array.from({length:api.RoomPreview_EffectCount()},(_,i)=>i)
       .filter(i=>api.RoomPreview_SourceValue(i,5));
     assert.equal(authored.length,96,'all 96 authored slots reach the shared renderer');
     assert.equal(api.RoomPreview_SourceValue(authored.at(-1),0),95);
     assert.equal(send(Buffer.from(dense(97)),api.RoomPreview_ConfigureEffects),0);
     assert.match(cstring(api.RoomPreview_RecipeError()),/97 active effects; the limit is 96/);
   }
   assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
 }

 // Faces and completion select separate authored scopes and native policies.
 if(base.group===7&&base.map===1) {
   const completion=base.section==='completion';
   assert.equal(api.RoomPreview_Policy(0,0,37,1),98);
   const defaults=Array.from(new Int32Array(memory,api.DioramaPreview_Input(),98));
   assert.equal(defaults[49+3],completion?3:2);
   assert.equal(defaults[49+11],completion?0:1);
   const independent='[layers:07:01]\nbg2-policy = edge:repeat horizontal:fixed left:7 right:9 bands:0\n'+
     '[layers:07:01:completion]\nbg2-policy = edge:transparent horizontal:fixed left:23 right:41 bands:0\n'+
     'bg1-stamp = cell:2,2 metatile:00 words:blank bands:1,1,1,1\n';
   assert.equal(send(Buffer.from(independent),api.RoomPreview_Configure),1);
   assert.equal(api.RoomPreview_Policy(0,0,37,0),98);
   const policy=Array.from(new Int32Array(memory,api.DioramaPreview_Input(),98));
   assert.equal(policy[49+3],completion?0:4);
   assert.deepEqual(policy.slice(49+6,49+8),completion?[23,41]:[7,9]);
   const request=[0,0,37,128,64];
   assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
   const phaseIni=path.join(directory,'death-heim-scenes.ini');fs.writeFileSync(phaseIni,independent);
   const observed=[api.RoomPreview_Hash()>>>0,api.RoomPreview_EffectHash()>>>0];
   const native=execFileSync(replay,[file,phaseIni],{input:request.join(' ')+'\n',encoding:'utf8'}).trim().split(' ').map(v=>parseInt(v,16));
   assert.deepEqual(observed,native,'A/B saved scene policy and tile edits match native replay');
   if(completion) {
     assert.equal(send(Buffer.from('[layers:07:01]\nbg2-policy = edge:repeat\n'),api.RoomPreview_Configure),1);
     assert.equal(api.RoomPreview_Policy(0,0,37,0),98);
     assert.deepEqual(Array.from(new Int32Array(memory,api.DioramaPreview_Input(),98)),defaults,
       'A edits do not become B defaults when B has no authored section');
   }
   assert.equal(send(Buffer.from(roomIni),api.RoomPreview_Configure),1);
 }

 // Saved policies use the planner, affect pixels, and survive native replay.
 if(base.group===7&&base.map===3) {
   const policy=(defaults=false)=>{
     assert.equal(api.RoomPreview_Policy(0,0,37,defaults?1:0),98);
     return Array.from(new Int32Array(memory,api.DioramaPreview_Input(),98));
   };
   const canonical=policy(true);
   assert.equal(canonical[3],3,'Death Heim room 3 BG1 defaults to mirror');
   assert.deepEqual(canonical.slice(5,8),[2,80,80]);
   const header='[layers:07:03]\n',request=[0,0,37,128,64];
   assert.equal(send(Buffer.from(header),api.RoomPreview_Configure),1);
   assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
   const mirrored=api.RoomPreview_Hash()>>>0;
   const authored=header+'bg1-policy = edge:world motion:normal horizontal:available vertical:available bands:0\n';
   assert.equal(send(Buffer.from(authored),api.RoomPreview_Configure),1);
   const effective=policy();assert.equal(effective[3],1);
   assert.deepEqual(effective.slice(5,8),[1,0,0]);assert.equal(effective[11],0);
   assert.deepEqual(effective.slice(49),canonical.slice(49),'BG2 remains canonical');
   assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
   const painted=api.RoomPreview_Hash()>>>0;
   assert.notEqual(painted,mirrored,'removing mirroring changes actual scanout');
   const policyIni=path.join(directory,'policy.ini');fs.writeFileSync(policyIni,authored);
   const actual=[painted,api.RoomPreview_EffectHash()>>>0];
   const expected=execFileSync(replay,[file,policyIni],{input:request.join(' ')+'\n',encoding:'utf8'}).trim().split(' ').map(v=>parseInt(v,16));
   assert.deepEqual(actual,expected,'saved policy produces the same native/WASM pixels');
   const crossing=header+'bg1-policy = bands:2\n'+
     'bg1-policy-band = index:0 rows:0,100 anchor:screen edge:mirror motion:normal horizontal:inherit\n'+
     'bg1-policy-band = index:1 rows:100,224 anchor:world edge:repeat motion:normal horizontal:inherit\n';
   for(const invalid of [crossing,header+'bg1-policy = horizontal:fixed left:129 right:0\n',
       header+'bg1-policy = bands:1\n',header+'bg1-policy = bands:5\n']) {
     assert.equal(send(Buffer.from(invalid),api.RoomPreview_Configure),0);
     assert.deepEqual(policy(),effective,'failed edits retain the complete previous policy');
     assert.equal(api.RoomPreview_Render(...request,960,600,1,0,0,1,0),1);
     assert.equal(api.RoomPreview_Hash()>>>0,painted);
   }
   assert.deepEqual(policy(true),canonical,'defaults remain available after authoring');
   assert.equal(send(Buffer.from(roomIni),api.RoomPreview_Configure),1);
 }

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
   if(base.map===1) {
     const custom=scoped.replace(/^mode=0$/m,'mode=1').replace(/^source-count=0$/m,'source-count=1')
       .replace(/^source-1=.*$/m,'source-1=480 320 -32 -8 64 16 1');
     assert.equal(send(Buffer.from(custom),api.RoomPreview_ConfigureEffects),1);
     const lava=[];
     for(let i=0;i<api.RoomPreview_CatalogueCount();++i)
       if(cstring(api.RoomPreview_KindName(api.RoomPreview_CatalogueValue(i,1)))==='lava-pit')
         lava.push([0,1,2,3].map(f=>api.RoomPreview_CatalogueGuideValue(i,0,f)));
     assert.deepEqual(lava,[[496,324,96,24]],'surface recipe changes refresh source capture and local bounds');
   }
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
   if(base.map===3) {
     const moved=scoped.replace(/^source-count=.*$/m,'source-count=1').replace(/^source-1=(.*)$/m,(_,values)=>{
       const v=values.split(/\s+/);v[6]='530';return 'source-1='+v.join(' ');
     }).replace(/^sky-anchor=.*$/m,'sky-anchor=140 80');
     assert.equal(send(Buffer.from(moved),api.RoomPreview_ConfigureEffects),1);
     let lights=0,skies=0;
     for(let i=0;i<api.RoomPreview_CatalogueCount();++i) {
       const name=cstring(api.RoomPreview_KindName(api.RoomPreview_CatalogueValue(i,1)));
       if(name==='castle-light') {
         lights++;assert.equal(api.RoomPreview_CatalogueValue(i,7),1);
         assert.deepEqual([1,2].map(f=>api.RoomPreview_CatalogueMemberValue(i,0,f)),[168,530],
           'window member bases use the edited definition');
       }
       if(name==='castle-sky') {
         skies++;assert.deepEqual([0,1].map(f=>api.RoomPreview_CatalogueGuideValue(i,0,f)),[140,80]);
       }
     }
     assert.equal(lights,1);assert.equal(skies,1);
   }
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
   if(kind==='exposure')recipes+='dim-player=1\ndim-enemies=0\nbrightness=.8\nfade-left=16\nfade-top=24\n';
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
assert.equal(presetChecks,data.rooms.reduce((n,r)=>n+stagePresets.filter(p=>p.rooms.some(([g,m])=>g===r.group&&m===r.map)).length,0)*3);assert.equal(scenes,150);assert.equal(api.memory.buffer,memory);
// Freeze animation during the last part of Fillmore's boss-pit descent. Once
// the world camera stops, shaft dust must stop too, in every backdrop mode.
const forest=data.rooms.find(r=>r.group===1&&r.map===1);
assert.equal(send(encode(data,blobs,forest),api.RoomPreview_Load),1);
// Aspect changes the displayed footprint independently of captured guard
// pixels. Cached and freshly configured views must agree.
assert.equal(send(Buffer.from(''),api.RoomPreview_Configure),1);
assert.equal(api.RoomPreview_Render(1920,300,128,120,64,960,600,1,0,0,1,0),1);
assert.equal(api.RoomPreview_Measure(1920,300,128,120,64,960,600,1,0,0,1,0,0),16);
const narrowFootprint=Array.from(new Float32Array(memory,api.DioramaPreview_Input(),16));
assert.equal(api.RoomPreview_Render(1920,300,128,120,64,960,540,1,0,0,1,0),1);
assert.equal(api.RoomPreview_CameraX(),1920);
const wideHash=api.RoomPreview_Hash();
assert.equal(api.RoomPreview_Measure(1920,300,128,120,64,960,540,1,0,0,1,0,0),16);
const wideFootprint=Array.from(new Float32Array(memory,api.DioramaPreview_Input(),16));
assert(wideFootprint[7]-wideFootprint[5]>narrowFootprint[7]-narrowFootprint[5],
  'wide monitor shows more world without changing the capture budget');
assert.equal(send(Buffer.from(''),api.RoomPreview_Configure),1);
assert.equal(api.RoomPreview_Render(1920,300,128,120,64,960,540,1,0,0,1,0),1);
assert.equal(api.RoomPreview_Hash(),wideHash,'view changes invalidate stale capture policy');
assert.equal(api.RoomPreview_Uploads(),0,'rebuilding an identical policy reuses uploaded pixels');
assert.equal(send(Buffer.from(roomConfiguration(forest)),api.RoomPreview_Configure),1);
assert.equal(send(Buffer.from('[effects]\nversion=1\n'),api.RoomPreview_ConfigureEffects),1);
for(const [width,height] of [[960,600],[960,540],[800,600]])for(const vertical of [32,64])for(const sky of [0,1,2]) {
 const positions=[];
 for(const y of [536,543,536]) {
   shaftMotes=new Map();
   assert.equal(api.RoomPreview_Render(3720,y,128,120,vertical,width,height,1,0,0,sky,0),1);
   positions.push(shaftMotes);
 }
 let compared=0;
 for(const [key,a] of positions[0]) {
   const b=positions[1].get(key),c=positions[2].get(key);if(!b||!c)continue;
   assert(Math.hypot(b[0]-a[0],b[1]-a[1])<.002,`shaft dust slid at camera stop: ${width}x${height}, rows=${vertical}, sky=${sky}`);
   assert(Math.hypot(c[0]-a[0],c[1]-a[1])<.002,'reverse scrub restores the same frozen mote');compared++;
 }
 assert(compared>=8,'real fine shaft particles remain visible throughout the stop');
}
shaftMotes=null;
api.RoomPreview_Reset();assert.equal(textures.size,0);api.RoomPreview_Reset();
assert.equal(api.RoomPreview_Hash(),0);
console.log(`Whole-room renderer: ${scenes} regional rooms, ${frames} native/WASM surface + source matches, ${scenes} authored round trips, ${presetChecks} stage preset reconstructions, ${draws} valid draws; reverse time, atomic edits/loads, resource reuse and teardown passed.`);
