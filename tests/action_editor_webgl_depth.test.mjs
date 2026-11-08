/* CPU-only contract checks. This is not hardware/GPU acceptance. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import {spawnSync} from 'node:child_process';

const root=path.resolve(import.meta.dirname,'..');
const shaderResult=spawnSync('python3',['-c',
  'import sys,json;sys.path.insert(0,"tools/action_editor");from build_compositor import shader_variants;print(json.dumps(shader_variants()))'],{cwd:root,encoding:'utf8'});
assert.equal(shaderResult.status,0,shaderResult.stderr);
const sources=JSON.parse(shaderResult.stdout);
function mockGl() {
  const state={enabled:new Set(),boundBuffers:new Map(),textures:new Map(),programs:[],draws:[],clears:[],active:1000};
  let next=1;
  const methods={
    TEXTURE0:1000,TEXTURE1:1001,INVALID_INDEX:0xffffffff,NO_ERROR:0,
    createTexture:()=>({id:next++}),createSampler:()=>({id:next++}),createVertexArray:()=>({id:next++}),
    createBuffer:()=>({id:next++,size:0}),createFramebuffer:()=>({id:next++,attachments:new Map()}),
    createShader:type=>({type,source:''}),createProgram:()=>{const p={id:next++,shaders:[],uniforms:{}};state.programs.push(p);return p;},
    shaderSource:(shader,source)=>{shader.source=source;},attachShader:(p,s)=>p.shaders.push(s),
    getShaderParameter:()=>true,getProgramParameter:()=>true,getUniformBlockIndex:()=>0xffffffff,
    getUniformLocation:(p,name)=>({p,name}),useProgram:p=>{state.program=p;},
    uniform1i:(u,v)=>{u.p.uniforms[u.name]=v;},uniform2f:()=>{},
    uniform4fv:(u,v)=>{u.p.uniforms[u.name]=Array.from(v);},
    bindBuffer:(target,buffer)=>state.boundBuffers.set(target,buffer),
    bufferData:(target,size)=>{state.boundBuffers.get(target).size=size;},
    bufferSubData:(target,offset,bytes)=>assert.ok(offset+bytes.byteLength<=state.boundBuffers.get(target).size,'GPU upload stays within allocated buffer'),
    bindFramebuffer:(target,fbo)=>{if(target==='FRAMEBUFFER'||target==='DRAW_FRAMEBUFFER')state.fbo=fbo;},
    framebufferTexture2D:(_target,attachment,_kind,texture)=>{assert.ok(state.fbo);state.fbo.attachments.set(attachment,texture);},
    activeTexture:unit=>{state.active=unit;},bindTexture:(_kind,t)=>state.textures.set(state.active,t),
    checkFramebufferStatus:()=> 'FRAMEBUFFER_COMPLETE',getExtension:()=>({}),getError:()=>0,isContextLost:()=>false,
    enable:cap=>state.enabled.add(cap),disable:cap=>state.enabled.delete(cap),depthMask:v=>{state.depthMask=v;},
    depthFunc:v=>{state.depthFunc=v;},drawBuffers:v=>{state.outputs=v;},
    clear:()=>state.clears.push({fbo:state.fbo,completedDraws:state.draws.length}),
    drawElements:()=>{
      const uniforms=state.program.uniforms;
      const sampled=['u_texture','u_illumination','u_attributes','u_enemy_light','opaque_depth','opaque_order'];
      for(const name of sampled)if(uniforms[name]!==undefined) {
        const texture=state.textures.get(1000+uniforms[name]);
        if(texture)for(const attached of state.fbo?.attachments.values()||[])assert.notEqual(texture,attached,'No framebuffer/texture feedback');
      }
      state.draws.push({fbo:state.fbo,options:uniforms.u_alpha_options?.slice(),native:uniforms.u_depth_native,
        mask:state.depthMask,func:state.depthFunc,blend:state.enabled.has('BLEND')});
    },
  };
  return {state,gl:new Proxy(methods,{get:(target,key)=>key in target?target[key]:/^[A-Z0-9_]+$/.test(key)?key:()=>{}})};
}
const {gl,state}=mockGl();
const context=vm.createContext({console,WebAssembly,Float32Array,Uint32Array,Int32Array,Uint8Array});
vm.runInContext(fs.readFileSync(path.join(root,'tools/action_editor/webgl2_backend.js'),'utf8')+'\nglobalThis.Backend=ActionWebGL2Backend;',context);
const backend=new context.Backend({width:64,height:48,getContext:()=>gl},sources);
backend.memory=new WebAssembly.Memory({initial:2});
const vertices=256,indices=512,depths=576,clipW=608;
new Float32Array(backend.memory.buffer,vertices,24).set([0,0,1,1,1,1,0,0,32,0,1,1,1,1,1,0,0,32,1,1,1,1,0,1]);
new Int32Array(backend.memory.buffer,indices,3).set([0,1,2]);
new Float32Array(backend.memory.buffer,depths,3).set([.2,.3,.4]);
new Float32Array(backend.memory.buffer,clipW,3).set([1,2,3]);
const art=backend.create(16,16,0,0,1);
assert.equal(backend.depth_begin(),1);
assert.equal(backend.target(0),1);assert.equal(state.fbo,backend.depthCanvas.fbo,'Target zero remains the scoped color target');
assert.equal(backend.depth_geometry(art,vertices,3,indices,3,1,0,0,-1,depths,clipW,0),1);
assert.equal(state.draws.length,0,'World geometry waits for the native band boundary');
/* Native actor is a boundary: the opaque pass and blended tail flush first. */
assert.equal(backend.depth_geometry(art,vertices,3,indices,3,1,0,0,-1,depths,0,3),1);
assert.equal(state.draws.length,3);
assert.deepEqual(state.draws.slice(0,2).map(d=>d.options[0]),[0,1]);
assert.equal(state.draws[0].blend,false,'Opaque ordinal MRT never requires floating-point blending');
assert.equal(state.draws[1].blend,true,'Tail blends over the opaque result');
assert.equal(state.draws[1].mask,false,'Tail never writes depth');
assert.equal(state.draws.at(-1).native,1);assert.equal(state.draws.at(-1).func,'ALWAYS');assert.equal(state.draws.at(-1).mask,true);
assert.equal(backend.depth_clear(),1);
assert.equal(backend.depth_geometry(art,vertices,3,indices,3,3,0,0,-1,depths,clipW,1),1);
assert.equal(backend.depth_finish(1),1);
assert.equal(backend.depthActive,false);assert.equal(state.fbo,null);
/* Abort drops borrowed pending draws and leaves ordinary rendering usable. */
assert.equal(backend.depth_begin(),1);
assert.equal(backend.depth_geometry(art,vertices,3,indices,3,1,0,0,-1,depths,clipW,0),1);
const beforeAbort=state.draws.length;assert.equal(backend.depth_finish(0),1);assert.equal(state.draws.length,beforeAbort);
assert.equal(backend.geometry(art,vertices,3,indices,3,1,0,0,-1),1);

/* Top/bottom native guards reuse one compact AA target. A pending world draw
 * must consume the previous strip before that target is cleared and rewritten. */
const guardAA=backend.create(64,64,2,1,1);
assert.ok(guardAA);
assert.equal(backend.depth_begin(),1);
assert.equal(backend.target(0),1);
const beforeGuard=state.draws.length;
assert.equal(backend.depth_geometry(guardAA,vertices,3,indices,3,1,0,0,-1,depths,clipW,0),1);
assert.equal(state.draws.length,beforeGuard,'Guard draw remains queued before target reuse');
assert.equal(backend.target(guardAA),1);
assert.equal(backend.depthQueue.length,0,'Switching to the sampled guard target drains its pending use');
assert.equal(state.draws.length,beforeGuard+2,'Opaque guard draw and blended tail both drain');
for(const draw of state.draws.slice(beforeGuard))
  assert.equal(draw.fbo,backend.depthCanvas.fbo,'Queued guard renders into its original world target');
assert.equal(backend.clear(0,0,0,0),1);
assert.equal(state.clears.at(-1).fbo,backend.textures[guardAA].fbo);
assert.equal(state.clears.at(-1).completedDraws,beforeGuard+2,'Clear follows both consumers of the previous guard strip');
assert.equal(backend.target(0),1);
assert.equal(backend.depth_finish(1),1);

/* Compile and link the actual generated ESSL, including all material wrappers
 * and the production alpha helper, without opening a window or using a GPU. */
if(spawnSync('glslangValidator',['--version'],{encoding:'utf8'}).status===0) {
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'actraiser-depth-shaders-'));
  try {
    for(const [i,program]of state.programs.entries()) {
      const files=[];
      for(const shader of program.shaders) {
        const filename=path.join(directory,`program-${i}.${shader.type==='VERTEX_SHADER'?'vert':'frag'}`);
        fs.writeFileSync(filename,shader.source);files.push(filename);
      }
      const compiled=spawnSync('glslangValidator',['-l',...files],{encoding:'utf8'});
      assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);
    }
  } finally {fs.rmSync(directory,{recursive:true,force:true});}
} else console.log('glslangValidator unavailable: ESSL compilation skipped');
backend.dispose();
console.log('WebGL shared-depth CPU contract: native bands, opaque pass + blended tail, target ownership, no texture feedback, bounded uploads, abort and ESSL linkage passed');
