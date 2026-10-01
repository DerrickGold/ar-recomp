/* Real serialized room surfaces through the shared compositor's browser ABI.
 * Pixel agreement is a separate WebGL2/native image comparison. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
const [wasmPath, ...scenes] = process.argv.slice(2);
assert(wasmPath && scenes.length, 'usage: wasm test.wasm scene.ardi [...]');
const textures = new Map();
let api, target = 0, creates = 0, draws = 0, effect = -1, shaderSupport = true;
const imports = {
  create(w, h, usage, filter, blend) {
    assert(w > 0 && h > 0 && w <= 16384 && h <= 16384);
    let id = 1;
    while (textures.has(id)) id++;
    assert(id < 128);
    textures.set(id, {w,h,usage,filter,blend}); creates++;
    return id;
  },
  destroy(id) { assert(textures.delete(id)); },
  update(id, x, y, w, h, pointer, pitch) {
    const t = textures.get(id); assert(t);
    assert(x >= 0 && y >= 0 && x+w <= t.w && y+h <= t.h && pitch >= w*4);
    assert(pointer > 0 && pointer+(h-1)*pitch+w*4 <= api.memory.buffer.byteLength);
    return 1;
  },
  target(id) { assert(!id || textures.get(id)?.usage === 2); target=id; return 1; },
  viewport() { return 1; }, clip() { return 1; }, clear() { return 1; },
  geometry(id, pointer, count, ip, ni, blend, u, v) {
    assert(!id || (textures.has(id) && id !== target));
    assert(count > 0 && ni > 0 && blend >= 0 && blend <= 9 && u <= 2 && v <= 2);
    const vertices = new Float32Array(api.memory.buffer,pointer,count*8);
    assert(vertices.every(Number.isFinite));
    const indices = new Int32Array(api.memory.buffer,ip,ni);
    assert(indices.every(i=>i >= 0 && i < count));
    draws++;
    return 1;
  },
  effect(kind, pointer, count) {
    assert(kind >= -1 && kind < 4);
    if (kind >= 0) {
      assert(shaderSupport && effect === -1);
      assert.equal(count, [3,3,9,4][kind]);
      assert(new Float32Array(api.memory.buffer,pointer,count).every(Number.isFinite));
    }
    effect=kind; return 1;
  },
  available() { return shaderSupport ? 1 : 0; },
};
const wasi = {fd_close:()=>0,fd_seek:()=>0,fd_write:()=>0,
  proc_exit:code=>{throw new Error(`Unexpected WASI exit ${code}`);}};
const module = await WebAssembly.compile(fs.readFileSync(wasmPath));
for (const item of WebAssembly.Module.imports(module))
  assert((item.module === 'ar' && Object.hasOwn(imports,item.name)) ||
    (item.module === 'wasi_snapshot_preview1' && Object.hasOwn(wasi,item.name)), JSON.stringify(item));
api=(await WebAssembly.instantiate(module,{ar:imports,wasi_snapshot_preview1:wasi})).exports;
api._initialize?.();
assert.equal(api.DioramaPreview_Version(),1);
assert.equal(api.DioramaPreview_Init(16384),1);
for (const file of scenes) {
  const packet=fs.readFileSync(file);
  assert(packet.length <= api.DioramaPreview_Capacity());
  const input=new Uint8Array(api.memory.buffer,api.DioramaPreview_Input(),packet.length);
  input.set(packet);
  assert.equal(api.DioramaPreview_Load(packet.length),1,file);
  for (const shaders of [true,false]) {
    shaderSupport=shaders;
    for (const [w,h,zoom,yaw,pitch,sky] of [[720,448,1,0,0,-1],[960,540,0.75,0.2,0.1,2],
      [800,600,1.5,-0.2,-0.1,0],[960,600,1,0,0,1]]) {
      assert.equal(api.DioramaPreview_Render(w,h,zoom,yaw,pitch,sky),1);
      assert.equal(effect,-1); assert.equal(target,0);
      const before=creates;
      assert.equal(api.DioramaPreview_Render(w,h,zoom,yaw,pitch,sky),1);
      assert.equal(creates,before,'unchanged frame must reuse its targets');
    }
  }
  const live = textures.size;
  input[0] ^= 1;
  assert.equal(api.DioramaPreview_Load(packet.length),0);
  assert.equal(textures.size,live,'failed load preserves active textures');
  assert.equal(api.DioramaPreview_Render(720,448,1,0,0,-1),1,'previous capture still usable');
  assert.equal(api.DioramaPreview_Render(720,448,NaN,0,0,-1),0);
}
api.DioramaPreview_Reset(); assert.equal(textures.size,0);
api.DioramaPreview_Reset(); assert.equal(textures.size,0);
console.log(`Captured compositor WASM: ${scenes.length} scenes, ${draws} draws; camera/aspect/skybox, shader fallback, atomic loads and resource reuse passed`);
