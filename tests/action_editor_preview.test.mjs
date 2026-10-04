/* Compare the compiled production renderer with native replay. Local ROM assets
 * are optional; the same boundary checks run with the synthetic native fixture. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const [wasm,replay,fixture,directory,roomsFile]=process.argv.slice(2);
const context=vm.createContext({window:{},WebAssembly,Uint8Array,Uint32Array,DataView});
vm.runInContext(fs.readFileSync(path.join(root,'tools/action_editor/shared_preview.js'),'utf8'),context);
const preview=vm.runInContext('SharedActionPreview',context);
const api=await preview.initialize(fs.readFileSync(wasm));
const memory=api.memory.buffer;
assert.equal(api.ActionPreview_Version(),1);
assert.equal(api.ActionPreview_Render(0,0,37),0,'render before load');
const original=fs.readFileSync(fixture);
preview.load(original);
const originalPointer=api.ActionPreview_Render(0,0,37);
assert.notEqual(originalPointer,0);
const originalHash=api.ActionPreview_Hash()>>>0;
assert.equal(originalHash,parseInt(execFileSync(replay,[fixture],{encoding:'utf8'}).trim(),16));
assert.equal(new Uint8Array(memory,originalPointer+3,1)[0],255,'ARGB to RGBA alpha');

// Loading malformed or structurally valid-but-unrenderable scenes is atomic.
const broken=Uint8Array.from(original);broken[broken.length-1]^=1;
assert.throws(()=>preview.load(broken),/rejected/);
assert.throws(()=>preview.load(original.subarray(0,111)),/rejected/);
assert.throws(()=>preview.load(new Uint8Array(api.ActionPreview_Capacity()+1)),/too large/);
function seal(bytes){
  let h=2166136261;for(let i=0;i<bytes.length;i++)h=Math.imul(h^(i>=104&&i<108?0:bytes[i]),16777619)>>>0;
  new DataView(bytes.buffer,bytes.byteOffset,bytes.length).setUint32(104,h,true);
}
const incomplete=Uint8Array.from(original),iv=new DataView(incomplete.buffer);
iv.setUint32(24,iv.getUint32(24,true)&~8,true);seal(incomplete);
assert.throws(()=>preview.load(incomplete),/rejected/,'missing palette');
assert.notEqual(api.ActionPreview_Render(0,0,37),0);
assert.equal(api.ActionPreview_Hash()>>>0,originalHash);
assert.equal(api.ActionPreview_Render(-1,0,37),0,'unsupported negative native camera');
assert.equal(api.ActionPreview_Render(65536,0,37),0,'camera bounds');

let scenes=0,frames=0;
if(roomsFile){
  const data=JSON.parse(fs.readFileSync(roomsFile,'utf8'));
  const blobs=data.blobs.map(text=>Uint8Array.from(Buffer.from(text,'base64')));
  for(const base of data.rooms)for(const terrain of base.terrainVariants){
    const room={...base,bg:[terrain.bg1,base.bg[1]],terrainProfile:terrain.profile};
    const maxX=room.bg[0].pagesWide*256-256,maxY=room.bg[0].pagesHigh*256-224;
    const requests=[{frame:37},{frame:0},{frame:1,cameraX:Math.min(maxX,123),cameraY:Math.min(maxY,64)},
      {frame:511,cameraX:maxX,cameraY:maxY},{frame:65535,cameraX:maxX>>1,cameraY:maxY>>1},
      {frame:65536,animationPhase:0,pagePhase:2},
      {frame:0xffffffff,haveRasterCamera:true,rasterCameraX:99,entryFrame:true},
      {frame:92,bgscMask:2,bgsc:0x7000}];
    const files=[],hashes=[];
    for(const [index,request] of requests.entries()){
      const packet=preview.encode(data,blobs,room,request);
      preview.load(packet);
      const pointer=api.ActionPreview_Render(request.cameraX??0,request.cameraY??0,request.frame);
      assert.notEqual(pointer,0,`render ${base.group}:${base.map}/${terrain.profile}/${index}`);
      const hash=api.ActionPreview_Hash()>>>0;hashes.push(hash);
      // Rehash exported RGBA bytes too: a correct internal hash must not mask a broken upload.
      const rgba=new Uint8Array(memory,pointer,256*224*4);
      let outputHash=2166136261;
      for(let i=0;i<rgba.length;i+=4)
        outputHash=Math.imul(outputHash^(rgba[i+3]<<24|rgba[i]<<16|rgba[i+1]<<8|rgba[i+2]),16777619)>>>0;
      assert.equal(outputHash,hash);
      const filename=path.join(directory,`frame-${index}.arscene`);
      fs.writeFileSync(filename,packet);files.push(filename);
    }
    assert.equal(hashes[0],terrain.nativeGolden.hash,
      `original C asset golden ${base.group}:${base.map}/${terrain.profile}`);
    const native=execFileSync(replay,files,{encoding:'utf8'}).trim().split('\n').map(h=>parseInt(h,16));
    assert.deepEqual(hashes,native,`native replay ${base.group}:${base.map}/${terrain.profile}`);
    assert.throws(()=>preview.encode(data,blobs,room,{cameraX:NaN}),/Invalid/);
    assert.throws(()=>preview.encode(data,blobs,room,{frame:1.5}),/Invalid/);
    assert.throws(()=>preview.encode(data,blobs,room,{bgscMask:4}),/Invalid/);
    frames+=requests.length;scenes++;
  }
  assert.equal(scenes,150);
}
assert.equal(api.memory.buffer,memory,'fixed resident memory across every room');
api.ActionPreview_Reset();
assert.equal(api.ActionPreview_Render(0,0,37),0);
assert.equal(api.ActionPreview_Hash(),0);
preview.load(original);
assert.notEqual(api.ActionPreview_Render(0,0,37),0);
assert.equal(api.ActionPreview_Hash()>>>0,originalHash,'reload after reset');
await assert.rejects(preview.initialize(new Uint8Array(8)));
assert.equal(preview.ready,false);
assert.match(preview.status,/unavailable/);
console.log(`Shared C/WASM: ${scenes} regional scenes, ${frames} frames match native exactly; atomic load/reset passed`);
