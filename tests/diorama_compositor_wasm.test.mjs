// Compare commands and projection values, not driver-dependent raster output.
import fs from 'node:fs';
import assert from 'node:assert/strict';

const [wasmPath, tracePath] = process.argv.slice(2);
const bytes = fs.readFileSync(tracePath);
const module = new WebAssembly.Module(fs.readFileSync(wasmPath));
let instance;
let diagnostic = '';
// The C recorder has no filesystem. WASI is only used for failure diagnostics.
const wasi = {
  fd_close() { return 8; },
  fd_seek() { return 8; },
  fd_write(fd, ptr, count, written) {
    assert.ok(fd === 1 || fd === 2);
    const mem = instance.exports.memory.buffer, view = new DataView(mem);
    let size = 0;
    for (let i = 0; i < count; i++) {
      const p = view.getUint32(ptr + i * 8, true), n = view.getUint32(ptr + i * 8 + 4, true);
      diagnostic += Buffer.from(mem, p, n).toString();
      size += n;
    }
    view.setUint32(written, size, true);
    return 0;
  }
};
for (const item of WebAssembly.Module.imports(module)) {
  assert.equal(item.module, 'wasi_snapshot_preview1');
  assert.ok(Object.hasOwn(wasi, item.name), `Unexpected host dependency: ${item.name}`);
}
instance = new WebAssembly.Instance(module, {wasi_snapshot_preview1: wasi});
instance.exports._initialize?.();
const memorySize = instance.exports.memory.buffer.byteLength;
let offset = 0, maxError = 0, total = 0;
const cases = instance.exports.DioramaFixture_Count();
assert.equal(cases, 120);
for (let test = 0; test < cases; test++) {
  let count;
  try { count = instance.exports.DioramaFixture_Run(test); }
  catch (error) { throw new Error(`Diorama case ${test}: ${diagnostic}`, {cause: error}); }
  const expectedCount = bytes.readUInt32LE(offset);
  offset += 4;
  assert.equal(count, expectedCount, `Case ${test}: command stream length`);
  const trace = new Float32Array(instance.exports.memory.buffer,
    instance.exports.DioramaFixture_Trace(), count);
  for (let i = 0; i < count; i++) {
    const expected = bytes.readFloatLE(offset), actual = trace[i];
    offset += 4;
    const delta = Math.abs(expected - actual);
    maxError = Math.max(maxError, delta);
    // Native/libm and WASM can round transcendental operations differently.
    // One thousandth of an output pixel bounds geometry; integers must match.
    assert.ok(delta <= 0.001,
      `Case ${test}, value ${i}: ${expected} != ${actual} (delta ${delta})`);
  }
  assert.equal(instance.exports.memory.buffer.byteLength, memorySize);
  total += count;
}
assert.equal(offset, bytes.length);
assert.equal(diagnostic, '');
console.log(`Diorama native/WASM: ${cases} cases, ${total} values; max delta ${maxError}`);
