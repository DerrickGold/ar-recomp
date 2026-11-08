/* Batch adapter for ArRenderDevice. Projection, order, coverage, filtering
 * policy and render-target reuse stay in the shared production C compositor. */
class ActionWebGL2Backend {
  constructor(canvas, shaderSources) {
    const gl = canvas.getContext('webgl2', {
      alpha: false, antialias: false, depth: false, stencil: false,
      premultipliedAlpha: false, preserveDrawingBuffer: true,
    });
    if (!gl) throw new Error('WebGL2 is unavailable in this browser.');
    this.gl = gl;
    this.canvas = canvas;
    this.textures = [null];
    this.targetId = 0;
    this.viewportBox = null;
    this.clipBox = null;
    this.effectKind = -1;
    this.memory = null;
    this.uploadBytes = new Uint8Array(0);
    this.stats = { draws: 0, creates: 0, updates: 0 };
    this.warnings = [];
    this.vertexBuffer = gl.createBuffer();
    this.indexBuffer = gl.createBuffer();
    this.uniformBuffer = gl.createBuffer();
    this.vertexCapacity = 0;
    this.indexCapacity = 0;
    this.uniforms = new Float32Array(176);
    this.vertexArray = gl.createVertexArray();
    gl.bindVertexArray(this.vertexArray);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vertexBuffer);
    for (const [index, count, offset] of [[0, 2, 0], [1, 4, 8], [2, 2, 24]]) {
      gl.enableVertexAttribArray(index);
      gl.vertexAttribPointer(index, count, gl.FLOAT, false, 32, offset);
    }
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.indexBuffer);
    gl.bindBuffer(gl.UNIFORM_BUFFER, this.uniformBuffer);
    gl.bufferData(gl.UNIFORM_BUFFER, this.uniforms.byteLength, gl.DYNAMIC_DRAW);
    gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, this.uniformBuffer);
    this.samplers = [];
    for (let linear = 0; linear < 2; linear++) {
      for (let u = 0; u < 2; u++) for (let v = 0; v < 2; v++) {
        const sampler = gl.createSampler();
        gl.samplerParameteri(sampler, gl.TEXTURE_MIN_FILTER, linear ? gl.LINEAR : gl.NEAREST);
        gl.samplerParameteri(sampler, gl.TEXTURE_MAG_FILTER, linear ? gl.LINEAR : gl.NEAREST);
        gl.samplerParameteri(sampler, gl.TEXTURE_WRAP_S, u ? gl.REPEAT : gl.CLAMP_TO_EDGE);
        gl.samplerParameteri(sampler, gl.TEXTURE_WRAP_T, v ? gl.REPEAT : gl.CLAMP_TO_EDGE);
        this.samplers.push(sampler);
      }
    }
    const basic = `#version 300 es
precision highp float;
in vec4 v_color; in vec2 v_uv; out vec4 o_color;
uniform sampler2D u_texture; uniform bool u_flip_source;
void main() {
  vec2 uv = vec2(v_uv.x, u_flip_source ? 1.0-v_uv.y : v_uv.y);
  o_color = texture(u_texture, uv) * v_color;
}`;
    this.materialSources = [basic, ...shaderSources.slice(0, 7)];
    this.alphaSource = shaderSources[7] || null;
    this.depthPrograms = null;
    this.depthQueue = [];
    this.depthActive = false;
    this.depthSerial = 0;
    this.programs = [this.program(basic)];
    for (const source of shaderSources.slice(0, 7)) {
      try { this.programs.push(this.program(source)); }
      catch (error) { this.programs.push(null); this.warnings.push(error.message); }
    }
    this.white = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.white);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
      new Uint8Array([255, 255, 255, 255]));
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);
    gl.disable(gl.DITHER);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.NONE);
    this.blendFactors = [null,
      [gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA],
      [gl.ONE, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA],
      [gl.SRC_ALPHA, gl.ONE, gl.ZERO, gl.ONE],
      [gl.ONE, gl.ONE, gl.ZERO, gl.ONE],
      [gl.ZERO, gl.ONE, gl.ONE, gl.ONE],
      [gl.ZERO, gl.ONE, gl.ZERO, gl.SRC_ALPHA],
      [gl.DST_COLOR, gl.ONE, gl.ZERO, gl.ONE],
      [gl.ZERO, gl.SRC_COLOR, gl.ZERO, gl.ONE],
      [gl.DST_COLOR, gl.ONE_MINUS_SRC_ALPHA, gl.ZERO, gl.ONE],
    ];
    this.imports = {};
    for (const name of ['create', 'destroy', 'update', 'target', 'viewport', 'clip',
      'clear', 'geometry', 'effect', 'available', 'depth_begin', 'depth_clear', 'depth_finish', 'depth_geometry']) this.imports[name] = this[name].bind(this);
  }

  program(fragmentSource, depth = false) {
    if (depth) {
      const alpha = this.alphaSource.replace(/layout\(location=2\) flat in/, 'flat in').replaceAll('alpha.options', 'u_alpha_options');
      fragmentSource = fragmentSource.replace(/(?:layout\(location\s*=\s*0\)\s*)?out vec4 o_color;/, 'layout(location=0) out vec4 o_color;').replace(/void\s+main\s*\(/, 'void material_main(');
      fragmentSource += `\nuniform vec4 u_alpha_options;
uniform bool u_depth_native;
uniform sampler2D opaque_depth, opaque_order;
${alpha}
void main() {
  if (u_depth_native) { material_main(); if (o_color.a <= .5/255.0) discard; return; }
  alpha_filter(); material_main(); alpha_output();
}`;
    }
    const gl = this.gl;
    const sources = [[gl.VERTEX_SHADER, `#version 300 es
precision highp float;
layout(location=0) in vec2 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 tex_coord;
${depth ? "layout(location=3) in vec3 depth_info; flat out float v_ordinal;" : ""}
uniform vec2 u_view_size;
out vec4 v_color; out vec2 v_uv;
void main() {
 gl_Position = vec4(position.x/u_view_size.x*2.0-1.0,
                    1.0-position.y/u_view_size.y*2.0,0.0,1.0);
${depth ? " gl_Position.z=depth_info.x*2.0-1.0; gl_Position*=depth_info.y; v_ordinal=depth_info.z;" : ""}
 v_color=color; v_uv=tex_coord;
}`], [gl.FRAGMENT_SHADER, fragmentSource]];
    const shaders = [];
    const program = gl.createProgram();
    try {
      for (const [type, source] of sources) {
        const shader = gl.createShader(type);
        shaders.push(shader);
        gl.shaderSource(shader, source);
        gl.compileShader(shader);
        if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS))
          throw new Error(gl.getShaderInfoLog(shader));
        gl.attachShader(program, shader);
      }
      gl.linkProgram(program);
      if (!gl.getProgramParameter(program, gl.LINK_STATUS))
        throw new Error(gl.getProgramInfoLog(program));
      const block = gl.getUniformBlockIndex(program, 'Context');
      if (block !== gl.INVALID_INDEX) gl.uniformBlockBinding(program, block, 0);
      return {
        program,
        size: gl.getUniformLocation(program, 'u_view_size'),
        flip: gl.getUniformLocation(program, 'u_flip_source'),
        alphaOptions: gl.getUniformLocation(program, 'u_alpha_options'),
        depthNative: gl.getUniformLocation(program, 'u_depth_native'),
        opaqueDepth: gl.getUniformLocation(program, 'opaque_depth'),
        opaqueOrder: gl.getUniformLocation(program, 'opaque_order'),
        texture: gl.getUniformLocation(program, 'u_texture'),
        illumination: gl.getUniformLocation(program, 'u_illumination'),
      attributes: gl.getUniformLocation(program, 'u_attributes'),
      enemyLight: gl.getUniformLocation(program, 'u_enemy_light'),
      };
    } catch (error) { gl.deleteProgram(program); throw error; }
    finally { for (const shader of shaders) gl.deleteShader(shader); }
  }

  create(width, height, usage, filter, blend) {
    if (!this.flushDepth()) return 0;
    const gl = this.gl;
    let id = this.textures.findIndex((t, i) => i > 0 && !t);
    if (id < 0) id = this.textures.length;
    if (id >= 128) return 0;
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, width, height);
    const fbo = usage === 2 ? gl.createFramebuffer() : null;
    if (fbo) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
      const ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.currentFramebuffer());
      if (!ok) { gl.deleteFramebuffer(fbo); gl.deleteTexture(texture); return 0; }
    }
    if (gl.getError() !== gl.NO_ERROR) {
      if (fbo) gl.deleteFramebuffer(fbo);
      gl.deleteTexture(texture);
      return 0;
    }
    this.textures[id] = { texture, fbo, width, height, filter, blend };
    this.stats.creates++;
    return id;
  }

  destroy(id) {
    this.flushDepth();
    const entry = this.textures[id];
    if (!entry) return;
    this.gl.deleteTexture(entry.texture);
    if (entry.fbo) this.gl.deleteFramebuffer(entry.fbo);
    if (entry.worldDepth) this.gl.deleteTexture(entry.worldDepth);
    this.textures[id] = null;
  }

  update(id, x, y, width, height, pointer, pitch) {
    if (!this.flushDepth()) return 0;
    const gl = this.gl, entry = this.textures[id];
    if (!entry || entry.fbo) return 0;
    const size = width * height * 4;
    if (this.uploadBytes.length < size) this.uploadBytes = new Uint8Array(size);
    const input = new Uint8Array(this.memory.buffer);
    let output = 0;
    for (let row = 0; row < height; row++) {
      let at = pointer + row * pitch;
      for (let col = 0; col < width; col++, at += 4) {
        this.uploadBytes[output++] = input[at + 2];
        this.uploadBytes[output++] = input[at + 1];
        this.uploadBytes[output++] = input[at];
        this.uploadBytes[output++] = input[at + 3];
      }
    }
    gl.bindTexture(gl.TEXTURE_2D, entry.texture);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, width, height, gl.RGBA,
      gl.UNSIGNED_BYTE, this.uploadBytes.subarray(0, size));
    this.stats.updates++;
    return gl.getError() === gl.NO_ERROR ? 1 : 0;
  }

  extent() {
    const t = this.textures[this.targetId];
    return t ? [t.width, t.height] : [this.canvas.width, this.canvas.height];
  }

  applyViewport() {
    const gl = this.gl, [width, height] = this.extent();
    const [x, y, w, h] = this.viewportBox || [0, 0, width, height];
    gl.viewport(x, height - y - h, w, h);
    let box = [x, y, w, h];
    if (this.clipBox) {
      const [cx, cy, cw, ch] = this.clipBox;
      const x0 = Math.max(x, x + cx), y0 = Math.max(y, y + cy);
      box = [x0, y0, Math.max(0, Math.min(x + w, x + cx + cw) - x0),
        Math.max(0, Math.min(y + h, y + cy + ch) - y0)];
    }
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(box[0], height - box[1] - box[3], box[2], box[3]);
  }

  target(id) {
    if (!this.flushDepth()) return 0;
    if (id && !this.textures[id]?.fbo) return 0;
    this.targetId = id;
    this.gl.bindFramebuffer(this.gl.FRAMEBUFFER, this.currentFramebuffer());
    this.applyViewport();
    return 1;
  }
  viewport(set, x, y, w, h) {
    if (!this.flushDepth()) return 0;
    this.viewportBox = set ? [x, y, w, h] : null;
    this.applyViewport();
    return 1;
  }
  clip(set, x, y, w, h) {
    if (!this.flushDepth()) return 0;
    this.clipBox = set ? [x, y, w, h] : null;
    this.applyViewport();
    return 1;
  }
  clear(r, g, b, a) {
    if (!this.flushDepth()) return 0;
    const gl = this.gl;
    gl.disable(gl.SCISSOR_TEST);
    gl.clearColor(r, g, b, a);
    gl.clear(gl.COLOR_BUFFER_BIT);
    this.applyViewport();
    return 1;
  }

  blend(mode) {
    const gl = this.gl;
    if (!mode) { gl.disable(gl.BLEND); return; }
    gl.enable(gl.BLEND);
    gl.blendEquationSeparate(gl.FUNC_ADD, gl.FUNC_ADD);
    gl.blendFuncSeparate(...this.blendFactors[mode]);
  }

  geometry(id, vertices, vertexCount, indices, indexCount, blend, u, v, filter = -1, depth = null) {
    if (!depth && !this.flushDepth()) return 0;
    const gl = this.gl;
    if (gl.isContextLost() || (id && id === this.targetId)) return 0;
    const entry = this.textures[id];
    const shader = (depth ? this.depthPrograms : this.programs)[this.effectKind + 1];
    if (!shader || (id && !entry)) return 0;
    gl.useProgram(shader.program);
    if (depth) {
      gl.uniform4fv(shader.alphaOptions, depth.options);
      gl.uniform1i(shader.depthNative, depth.native ? 1 : 0);
      for (const [slot, uniform, texture] of [[5, shader.opaqueDepth, depth.opaque], [6, shader.opaqueOrder, depth.order]]) {
        gl.uniform1i(uniform, slot); gl.activeTexture(gl.TEXTURE0+slot);
        gl.bindTexture(gl.TEXTURE_2D, texture || this.white); gl.bindSampler(slot, this.samplers[0]);
      }
    } else { gl.disable(gl.DEPTH_TEST); gl.depthMask(false); }

    const [width, height] = this.extent();
    gl.uniform2f(shader.size, this.viewportBox?.[2] ?? width, this.viewportBox?.[3] ?? height);
    gl.uniform1i(shader.flip, entry?.fbo ? 1 : 0);
    gl.uniform1i(shader.texture, 0);
    if (this.effectKind === 4) {
      const light = this.textures[this.illuminationId];
      if (!light || this.illuminationId === this.targetId) return 0;
      gl.uniform1i(shader.illumination, 1);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, light.texture);
      gl.bindSampler(1, this.samplers[4]);
    }
    if(this.effectKind === 5) {
      for(let i=0;i<3;i++) {
        const id=this.actorTextureIds[i], texture=this.textures[id];
        if(!texture || id===this.targetId)return 0;
        gl.uniform1i(shader[['attributes','illumination','enemyLight'][i]],i+1);
        gl.activeTexture(gl.TEXTURE0+i+1);
        gl.bindTexture(gl.TEXTURE_2D,texture.texture);
        gl.bindSampler(i+1,this.samplers[4]);
      }
    }
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, entry?.texture || this.white);
    // A filter of -1 keeps the texture's own; 0/1 override it for this draw.
    const linear = filter >= 0 ? filter === 1 : !!entry?.filter;
    gl.bindSampler(0, this.samplers[(linear ? 4 : 0) + (u === 2 ? 2 : 0) + (v === 2 ? 1 : 0)]);
    this.blend(depth?.opaquePass ? 0 : blend);
    gl.bindVertexArray(this.vertexArray);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vertexBuffer);
    const stride = depth ? 44 : 32;
    for (const [index, count, offset] of [[0, 2, 0], [1, 4, 8], [2, 2, 24]]) gl.vertexAttribPointer(index, count, gl.FLOAT, false, stride, offset);
    if (depth) { gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 3, gl.FLOAT, false, stride, 32); }
    else gl.disableVertexAttribArray(3);
    const vb = vertexCount * stride, ib = indexCount * 4;
    if (vb > this.vertexCapacity) {
      this.vertexCapacity = 2 ** Math.ceil(Math.log2(vb));
      gl.bufferData(gl.ARRAY_BUFFER, this.vertexCapacity, gl.STREAM_DRAW);
    }
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, depth ? vertices : new Uint8Array(this.memory.buffer, vertices, vb));
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.indexBuffer);
    if (ib > this.indexCapacity) {
      this.indexCapacity = 2 ** Math.ceil(Math.log2(ib));
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, this.indexCapacity, gl.STREAM_DRAW);
    }
    gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0, depth ? indices : new Uint8Array(this.memory.buffer, indices, ib));
    gl.drawElements(gl.TRIANGLES, indexCount, gl.UNSIGNED_INT, 0);
    this.stats.draws++;
    return 1;
  }

  currentFramebuffer() {
    return this.textures[this.targetId]?.fbo || (this.depthActive ? this.depthCanvas?.fbo : null) || null;
  }
  depthTexture(width, height, format) {
    const gl=this.gl, texture=gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D,texture);gl.texStorage2D(gl.TEXTURE_2D,1,format,width,height);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
    return texture;
  }
  releaseDepthTarget(entry) {
    const gl=this.gl;
    if(entry.worldDepth)gl.deleteTexture(entry.worldDepth);
    if(entry.texture)gl.deleteTexture(entry.texture);
    if(entry.fbo)gl.deleteFramebuffer(entry.fbo);
  }
  releaseDepthScratch() {
    const gl=this.gl,s=this.depthScratch;if(!s)return;
    gl.deleteTexture(s.order);this.depthScratch=null;
  }
  ensureDepthTarget() {
    const gl=this.gl,entry=this.textures[this.targetId]||this.depthCanvas;
    if(!entry?.fbo)return null;
    if(!entry.worldDepth)entry.worldDepth=this.depthTexture(entry.width,entry.height,gl.DEPTH_COMPONENT32F);
    gl.bindFramebuffer(gl.FRAMEBUFFER,entry.fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.DEPTH_ATTACHMENT,gl.TEXTURE_2D,entry.worldDepth,0);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    if(entry.depthSerial!==this.depthSerial) {
      gl.disable(gl.SCISSOR_TEST);gl.depthMask(true);gl.clearDepth(1);gl.clear(gl.DEPTH_BUFFER_BIT);
      entry.depthSerial=this.depthSerial;this.applyViewport();
    }
    return gl.checkFramebufferStatus(gl.FRAMEBUFFER)===gl.FRAMEBUFFER_COMPLETE?entry:null;
  }
  ensureDepthScratch(width,height) {
    const gl=this.gl;
    if(this.depthScratch?.width===width && this.depthScratch?.height===height)return this.depthScratch;
    this.releaseDepthScratch();
    this.depthScratch={width,height,order:this.depthTexture(width,height,gl.R32F)};
    return this.depthScratch;
  }
  depth_begin() {
    if(this.depthActive)return 0;
    const gl=this.gl;
    if(!this.alphaSource || !gl.getExtension('EXT_color_buffer_float')) {
      this.warnings.push('Voxel preview needs WebGL2 floating-point color targets for shared depth and transparency.');return 0;
    }
    try {
      if(!this.depthPrograms) {
        const programs=[];
        try {for(const source of this.materialSources)programs.push(this.program(source,true));}
        catch(error){for(const p of programs)gl.deleteProgram(p.program);throw error;}
        this.depthPrograms=programs;
      }
      if(this.depthCanvas?.width!==this.canvas.width || this.depthCanvas?.height!==this.canvas.height) {
        if(this.depthCanvas)this.releaseDepthTarget(this.depthCanvas);
        this.depthCanvas={width:this.canvas.width,height:this.canvas.height,fbo:gl.createFramebuffer(),
          texture:this.depthTexture(this.canvas.width,this.canvas.height,gl.RGBA8)};
        gl.bindFramebuffer(gl.FRAMEBUFFER,this.depthCanvas.fbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,this.depthCanvas.texture,0);
      }
      /* The main target is a texture during the scope, permitting the same
       * opaque/ordinal MRT pass as every intermediate compositor target. */
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER,null);gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER,this.depthCanvas.fbo);
      gl.disable(gl.SCISSOR_TEST);
      gl.blitFramebuffer(0,0,this.canvas.width,this.canvas.height,0,0,this.canvas.width,this.canvas.height,gl.COLOR_BUFFER_BIT,gl.NEAREST);
      this.depthActive=true;this.depthSerial++;this.depthQueue=[];this.depthVertices=0;this.depthIndices=0;
      gl.bindFramebuffer(gl.FRAMEBUFFER,this.currentFramebuffer());this.applyViewport();return 1;
    } catch(error) { this.warnings.push(`Voxel shared-depth initialization failed: ${error.message}`);return 0; }
  }
  depth_clear() {
    if(!this.depthActive || !this.flushDepth())return 0;
    const entry=this.ensureDepthTarget();if(!entry)return 0;
    const gl=this.gl;gl.disable(gl.SCISSOR_TEST);gl.depthMask(true);gl.clearDepth(1);gl.clear(gl.DEPTH_BUFFER_BIT);
    this.applyViewport();return 1;
  }
  depth_finish(success) {
    if(!this.depthActive)return 1;
    const ok=success?this.flushDepth():true;this.depthQueue=[];
    const gl=this.gl;
    if(success&&ok) {
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER,this.depthCanvas.fbo);gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER,null);
      gl.disable(gl.SCISSOR_TEST);
      gl.blitFramebuffer(0,0,this.canvas.width,this.canvas.height,0,0,this.canvas.width,this.canvas.height,gl.COLOR_BUFFER_BIT,gl.NEAREST);
    }
    this.depthActive=false;gl.disable(gl.DEPTH_TEST);gl.depthMask(false);
    gl.bindFramebuffer(gl.FRAMEBUFFER,this.currentFramebuffer());this.applyViewport();return ok?1:0;
  }
  depth_geometry(id,vertices,nv,indices,ni,blend,u,v,filter,depths,clipW,mode) {
    if(!this.depthActive || nv<=0 || ni<=0 || ni%3 || nv>131072 || ni>393216 || mode<0 || mode>3)return 0;
    if(this.depthQueue.length===256 || this.depthVertices+nv>131072 || this.depthIndices+ni>393216)
      if(!this.flushDepth())return 0;
    const source=new Float32Array(this.memory.buffer,vertices,nv*8),z=new Float32Array(this.memory.buffer,depths,nv);
    const w=clipW?new Float32Array(this.memory.buffer,clipW,nv):null;
    const index=new Int32Array(this.memory.buffer,indices,ni),expanded=new Float32Array(ni*11);
    const sequential=new Uint32Array(ni),ordinal=this.depthIndices/3;
    for(let i=0;i<ni;i++) {
      const at=index[i];if(at<0||at>=nv||!Number.isFinite(z[at])||(w&&!(w[at]>0)))return 0;
      expanded.set(source.subarray(at*8,at*8+8),i*11);
      expanded[i*11+8]=mode===3?1:z[at];expanded[i*11+9]=w?w[at]:1;
      expanded[i*11+10]=ordinal+Math.floor(i/3);sequential[i]=i;
    }
    const draw={id,vertices:expanded,indices:sequential,nv:ni,ni,blend,u,v,filter,mode,
      effectKind:this.effectKind,uniforms:this.uniforms.slice(),illuminationId:this.illuminationId,
      actorTextureIds:this.actorTextureIds?.slice(),viewport:this.viewportBox?.slice(),clip:this.clipBox?.slice()};
    if(mode===3 || mode===2) {
      if(!this.flushDepth())return 0;
      const target=this.ensureDepthTarget();if(!target)return 0;
      const gl=this.gl;gl.enable(gl.DEPTH_TEST);gl.depthFunc(mode===3?gl.ALWAYS:gl.LEQUAL);gl.depthMask(mode===3);
      const ok=this.replayDepth(draw,{native:true,options:[0,0,blend,0]});
      gl.disable(gl.DEPTH_TEST);gl.depthMask(false);return ok;
    }
    this.depthQueue.push(draw);this.depthVertices+=nv;this.depthIndices+=ni;return 1;
  }
  replayDepth(draw,depth) {
    const gl=this.gl;
    this.effectKind=draw.effectKind;this.uniforms.set(draw.uniforms);
    this.illuminationId=draw.illuminationId;this.actorTextureIds=draw.actorTextureIds;
    this.viewportBox=draw.viewport;this.clipBox=draw.clip;this.applyViewport();
    gl.bindBuffer(gl.UNIFORM_BUFFER,this.uniformBuffer);gl.bufferSubData(gl.UNIFORM_BUFFER,0,this.uniforms);
    return this.geometry(draw.id,draw.vertices,draw.nv,draw.indices,draw.ni,draw.blend,draw.u,draw.v,draw.filter,depth);
  }
  flushDepth() {
    if(!this.depthQueue?.length)return true;
    const gl=this.gl,draws=this.depthQueue;this.depthQueue=[];this.depthVertices=this.depthIndices=0;
    const state={effectKind:this.effectKind,uniforms:this.uniforms.slice(),illuminationId:this.illuminationId,
      actorTextureIds:this.actorTextureIds,viewport:this.viewportBox,clip:this.clipBox};
    let target=null,ok=false;
    try {
      target=this.ensureDepthTarget();if(!target)throw new Error('Depth target is incomplete');
      const scratch=this.ensureDepthScratch(target.width,target.height);
      gl.bindFramebuffer(gl.FRAMEBUFFER,target.fbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT1,gl.TEXTURE_2D,scratch.order,0);
      gl.drawBuffers([gl.COLOR_ATTACHMENT0,gl.COLOR_ATTACHMENT1]);
      gl.disable(gl.SCISSOR_TEST);gl.clearBufferfv(gl.COLOR,1,new Float32Array([-1,0,0,0]));
      gl.enable(gl.DEPTH_TEST);gl.depthFunc(gl.LEQUAL);gl.depthMask(true);
      for(const draw of draws)if(!this.replayDepth(draw,{opaquePass:true,options:[0,0,draw.blend===3?2:draw.blend?1:0,draw.mode===0?1:0]}))throw new Error('Opaque draw failed');
      gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT1,gl.TEXTURE_2D,null,0);
      gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
      /* Everything the opaque pass rejected (coverage edges, fades, additive
       * light) blends in band draw order in front of the opaque depth. */
      gl.disable(gl.DEPTH_TEST);gl.depthMask(false);
      gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.DEPTH_ATTACHMENT,gl.TEXTURE_2D,null,0);
      for(const draw of draws)if(!this.replayDepth(draw,{options:[1,0,draw.blend===3?2:draw.blend?1:0,draw.mode===0?1:0],
        opaque:target.worldDepth,order:scratch.order}))throw new Error('Fractional tail failed');
      ok=true;
    } catch(error) {this.warnings.push(`Voxel shared-depth draw failed: ${error.message}`);}
    finally {
      if(target){gl.bindFramebuffer(gl.FRAMEBUFFER,target.fbo);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.DEPTH_ATTACHMENT,gl.TEXTURE_2D,target.worldDepth,0);gl.drawBuffers([gl.COLOR_ATTACHMENT0]);}
      this.effectKind=state.effectKind;this.uniforms.set(state.uniforms);this.illuminationId=state.illuminationId;
      this.actorTextureIds=state.actorTextureIds;this.viewportBox=state.viewport;this.clipBox=state.clip;
      gl.bindBuffer(gl.UNIFORM_BUFFER,this.uniformBuffer);gl.bufferSubData(gl.UNIFORM_BUFFER,0,this.uniforms);
      gl.disable(gl.DEPTH_TEST);gl.depthMask(false);this.applyViewport();
    }
    return ok;
  }

  effect(kind, pointer, count) {
    if (kind < -1 || kind > 6 || (kind >= 0 && !this.programs[kind + 1])) return 0;
    this.effectKind = kind;
    if (kind >= 0) {
      this.uniforms.fill(0);
      const values = new Float32Array(this.memory.buffer, pointer, count);
      this.uniforms.set(values.subarray(0,kind === 4 ? 176 : 16));
      if (kind === 4) this.illuminationId = values[176];
      if (kind === 5) this.actorTextureIds = Array.from(values.subarray(16,19));
      const gl = this.gl;
      gl.bindBuffer(gl.UNIFORM_BUFFER, this.uniformBuffer);
      gl.bufferSubData(gl.UNIFORM_BUFFER, 0, this.uniforms);
    }
    return 1;
  }
  available(kind) { return this.programs[kind + 1] ? 1 : 0; }
  dispose() {
    const gl = this.gl;
    for (let id = 1; id < this.textures.length; id++) this.destroy(id);
    this.depth_finish(0);
    this.releaseDepthScratch();
    if (this.depthCanvas) this.releaseDepthTarget(this.depthCanvas);
    for (const p of [...this.programs, ...(this.depthPrograms || [])]) if (p) gl.deleteProgram(p.program);
    for (const s of this.samplers) gl.deleteSampler(s);
    for (const b of [this.vertexBuffer, this.indexBuffer, this.uniformBuffer]) gl.deleteBuffer(b);
    gl.deleteVertexArray(this.vertexArray);
    gl.deleteTexture(this.white);
  }
}
