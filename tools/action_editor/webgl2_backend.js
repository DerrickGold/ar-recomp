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
    this.uniforms = new Float32Array(16);
    this.vertexArray = gl.createVertexArray();
    gl.bindVertexArray(this.vertexArray);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vertexBuffer);
    for (const [index, count, offset] of [[0, 2, 0], [1, 4, 8], [2, 2, 24]]) {
      gl.enableVertexAttribArray(index);
      gl.vertexAttribPointer(index, count, gl.FLOAT, false, 32, offset);
    }
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.indexBuffer);
    gl.bindBuffer(gl.UNIFORM_BUFFER, this.uniformBuffer);
    gl.bufferData(gl.UNIFORM_BUFFER, 64, gl.DYNAMIC_DRAW);
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
    this.programs = [this.program(basic)];
    for (const source of shaderSources) {
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
      'clear', 'geometry', 'effect', 'available']) this.imports[name] = this[name].bind(this);
  }

  program(fragmentSource) {
    const gl = this.gl;
    const sources = [[gl.VERTEX_SHADER, `#version 300 es
precision highp float;
layout(location=0) in vec2 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 tex_coord;
uniform vec2 u_view_size;
out vec4 v_color; out vec2 v_uv;
void main() {
 gl_Position = vec4(position.x/u_view_size.x*2.0-1.0,
                    1.0-position.y/u_view_size.y*2.0,0.0,1.0);
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
        texture: gl.getUniformLocation(program, 'u_texture'),
      };
    } catch (error) { gl.deleteProgram(program); throw error; }
    finally { for (const shader of shaders) gl.deleteShader(shader); }
  }

  create(width, height, usage, filter, blend) {
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
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.textures[this.targetId]?.fbo || null);
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
    const entry = this.textures[id];
    if (!entry) return;
    this.gl.deleteTexture(entry.texture);
    if (entry.fbo) this.gl.deleteFramebuffer(entry.fbo);
    this.textures[id] = null;
  }

  update(id, x, y, width, height, pointer, pitch) {
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
    if (id && !this.textures[id]?.fbo) return 0;
    this.targetId = id;
    this.gl.bindFramebuffer(this.gl.FRAMEBUFFER, this.textures[id]?.fbo || null);
    this.applyViewport();
    return 1;
  }
  viewport(set, x, y, w, h) {
    this.viewportBox = set ? [x, y, w, h] : null;
    this.applyViewport();
    return 1;
  }
  clip(set, x, y, w, h) {
    this.clipBox = set ? [x, y, w, h] : null;
    this.applyViewport();
    return 1;
  }
  clear(r, g, b, a) {
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

  geometry(id, vertices, vertexCount, indices, indexCount, blend, u, v) {
    const gl = this.gl;
    if (gl.isContextLost() || (id && id === this.targetId)) return 0;
    const entry = this.textures[id];
    const shader = this.programs[this.effectKind + 1];
    if (!shader || (id && !entry)) return 0;
    gl.useProgram(shader.program);
    const [width, height] = this.extent();
    gl.uniform2f(shader.size, this.viewportBox?.[2] ?? width, this.viewportBox?.[3] ?? height);
    gl.uniform1i(shader.flip, entry?.fbo ? 1 : 0);
    gl.uniform1i(shader.texture, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, entry?.texture || this.white);
    gl.bindSampler(0, this.samplers[(entry?.filter ? 4 : 0) + (u === 2 ? 2 : 0) + (v === 2 ? 1 : 0)]);
    this.blend(blend);
    gl.bindVertexArray(this.vertexArray);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vertexBuffer);
    const vb = vertexCount * 32, ib = indexCount * 4;
    if (vb > this.vertexCapacity) {
      this.vertexCapacity = 2 ** Math.ceil(Math.log2(vb));
      gl.bufferData(gl.ARRAY_BUFFER, this.vertexCapacity, gl.STREAM_DRAW);
    }
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array(this.memory.buffer, vertices, vb));
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.indexBuffer);
    if (ib > this.indexCapacity) {
      this.indexCapacity = 2 ** Math.ceil(Math.log2(ib));
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, this.indexCapacity, gl.STREAM_DRAW);
    }
    gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0, new Uint8Array(this.memory.buffer, indices, ib));
    gl.drawElements(gl.TRIANGLES, indexCount, gl.UNSIGNED_INT, 0);
    this.stats.draws++;
    return 1;
  }

  effect(kind, pointer, count) {
    if (kind < -1 || kind > 3 || (kind >= 0 && !this.programs[kind + 1])) return 0;
    this.effectKind = kind;
    if (kind >= 0) {
      this.uniforms.fill(0);
      this.uniforms.set(new Float32Array(this.memory.buffer, pointer, count));
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
    for (const p of this.programs) if (p) gl.deleteProgram(p.program);
    for (const s of this.samplers) gl.deleteSampler(s);
    for (const b of [this.vertexBuffer, this.indexBuffer, this.uniformBuffer]) gl.deleteBuffer(b);
    gl.deleteVertexArray(this.vertexArray);
    gl.deleteTexture(this.white);
  }
}
