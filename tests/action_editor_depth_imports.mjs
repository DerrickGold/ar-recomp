/* Preserve each suite's existing geometry oracle while validating the shared
 * Voxel visibility ABI. Pixel/depth rendering is covered by the browser gate. */
import assert from 'node:assert/strict';
export function attachDepthImports(imports, memory) {
  let active = false;
  imports.depth_begin = () => { assert(!active, 'depth scopes cannot overlap'); active = true; return 1; };
  imports.depth_clear = () => { assert(active, 'depth clear requires a scope'); return 1; };
  imports.depth_finish = success => {
    assert(active && (success === 0 || success === 1)); active = false; return 1;
  };
  imports.depth_geometry = (texture, vertices, count, indices, indexCount, blend, u, v, filter,
      depths, clipW, mode) => {
    assert(active && count > 0 && count <= 131072 && indexCount > 0 && indexCount <= 393216);
    assert(Number.isInteger(mode) && mode >= 0 && mode <= 3);
    assert(depths > 0 && new Float32Array(memory().buffer, depths, count).every(Number.isFinite));
    if (clipW) {
      assert.notEqual(mode, 3, 'native actors keep affine native artwork');
      assert(new Float32Array(memory().buffer, clipW, count).every(w => Number.isFinite(w) && w > 0));
    }
    const previous = imports.depthSubmission;
    imports.depthSubmission = {mode, perspective: !!clipW};
    try { return imports.geometry(texture, vertices, count, indices, indexCount, blend, u, v, filter); }
    finally { imports.depthSubmission = previous; }
  };
  return imports;
}
