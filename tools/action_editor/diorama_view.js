
/* ---- 3D orbit preview --------------------------------------------------
 * PORTABILITY IS THE POINT. The engine composites a diorama by drawing each
 * captured plane as one textured quad at its own depth, so that is exactly
 * what this does -- one exact camera-local raster capture per band, placed at
 * the band's z. Nothing here depends on a WebGL-only feature:
 *
 *   - the matrix is a plain right-handed perspective * lookAt, column-major,
 *     the same convention as scene3d_math.c;
 *   - every BG is sampled through its own live scroll, mosaic and page-cycle
 *     state before it becomes a common 256x224 capture surface;
 *   - configured painter order decides overlap while z independently controls
 *     projected geometry, matching diorama.c;
 *   - the shader is texel * vertex colour, which is the SIM 3D fragment
 *     stage already, so per-vertex tint carries over unchanged.
 *
 * The runtime consumes the same INI values and capture-surface model. */
const V_SRC = `
attribute vec3 aPos; attribute vec2 aUV; attribute float aShade;
uniform mat4 uMVP; uniform vec3 uOrigin;
varying vec2 vUV; varying float vShade;
void main(){ vUV = aUV; vShade = aShade;
  gl_Position = uMVP * vec4(aPos + uOrigin, 1.0); }`;
const F_SRC = `
precision mediump float;
uniform sampler2D uTex; uniform vec4 uTint;
varying vec2 vUV; varying float vShade;
void main(){ vec4 t = texture2D(uTex, vUV);
  if (t.a < 0.02) discard;                 /* colour 0 stays transparent */
  gl_FragColor = vec4(t.rgb * uTint.rgb * vShade, t.a * uTint.a); }`;

let gl = null, prog = null, texes = [null,null,null], quadBuf = null;
const nativeBandTex = [
  [null,null,null], [null,null,null],
];
/* The layer you are NOT editing, rasterised and cached so both backgrounds can
 * be in the scene at once. Without it there is no way to see whether a band
 * you pushed back on BG1 has passed through BG2 -- which is the one thing the
 * depth ladder cannot show you visually. */
let otherTex = [null,null,null], otherKey = null, otherL = null;
function otherLayerTextures() {
  const bg = bgIndex ^ 1;
  if (!room.bg[bg]) return null;
  const key = `${sceneKey(room)}:${bg}`;
  if (otherKey === key && otherTex[0]) return { texes:otherTex, L:otherL };
  const L2 = decodeLayer(room, bg);
  const st2 = bucket(room, bg);
  const rgb = BANDS.map((_, i) => tint ? bandRGB(i) : null);
  const imgs = [0,1,2].map(() => new ImageData(L2.w, L2.h));
  for (let ty=0; ty<L2.tilesH; ty++) for (let tx=0; tx<L2.tilesW; tx++) {
    const b = bandAt(st2, L2, tx, ty);
    blitTile(L2, L2.words[ty*L2.tilesW+tx], imgs[b].data, L2.w, tx*8, ty*8, rgb[b]);
  }
  for (let b=0;b<3;b++){
    if (!otherTex[b]) otherTex[b] = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, otherTex[b]);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE, imgs[b]);
  }
  otherKey = key; otherL = L2;
  return { texes:otherTex, L:otherL };
}
const invalidateOther = () => { otherKey = null; };
let showBoth = true;
/* Where the other background sits relative to this one, in level pixels.
 *
 * The two layers are rarely the same size -- 17 rooms have a BG2 of a single
 * 256x256 page against a playfield up to 4096 long, and 16 more simply differ
 * -- and in play BG2 scrolls at its own rate or repeats. So there is no one
 * true alignment to preview: any part of BG2 can end up behind any part of
 * BG1. Sliding it is how you scan the whole relationship for conflicts rather
 * than checking the single arrangement that happens to be at the origin. */
const otherOffset = { x: 0, y: 0, repeat: true };
function initGL() {
  gl = glc.getContext('webgl', { alpha:false, antialias:true, depth:true });
  if (!gl) return false;
  const mk = (t, src) => { const s = gl.createShader(t);
    gl.shaderSource(s, src); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
      console.error(gl.getShaderInfoLog(s));
    return s; };
  prog = gl.createProgram();
  gl.attachShader(prog, mk(gl.VERTEX_SHADER, V_SRC));
  gl.attachShader(prog, mk(gl.FRAGMENT_SHADER, F_SRC));
  gl.linkProgram(prog); gl.useProgram(prog);
  quadBuf = gl.createBuffer();
  for (let i=0;i<3;i++){ texes[i] = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texes[i]);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE); }
  for(let bg=0;bg<2;bg++)for(let b=0;b<3;b++){
    nativeBandTex[bg][b]=gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D,nativeBandTex[bg][b]);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
  }
  return true;
}
/* Column-major, same layout scene3d_math.c uses. */
function perspective(fovy, aspect, n, f) {
  const t = 1/Math.tan(fovy/2);
  return [t/aspect,0,0,0, 0,t,0,0, 0,0,(f+n)/(n-f),-1, 0,0,2*f*n/(n-f),0];
}
function lookAt(ex,ey,ez, cx,cy,cz) {
  let zx=ex-cx, zy=ey-cy, zz=ez-cz;
  let l=Math.hypot(zx,zy,zz); zx/=l; zy/=l; zz/=l;
  let xx=zz, xy=0, xz=-zx; l=Math.hypot(xx,xy,xz)||1; xx/=l; xy/=l; xz/=l;
  const yx=zy*xz-zz*xy, yy=zz*xx-zx*xz, yz=zx*xy-zy*xx;
  return [xx,yx,zx,0, xy,yy,zy,0, xz,yz,zz,0,
          -(xx*ex+xy*ey+xz*ez), -(yx*ex+yy*ey+yz*ez), -(zx*ex+zy*ey+zz*ez), 1];
}
const mul = (a,b) => { const o = new Array(16);
  for (let c=0;c<4;c++) for (let r=0;r<4;r++) { let s=0;
    for (let k=0;k<4;k++) s += a[k*4+r]*b[c*4+k]; o[c*4+r]=s; }
  return o; };

/* The MVP of the last 3D draw, kept so a pointer can be put back onto the
 * actor's plane without recomputing the camera. */
let lastMVP = null, lastVW = 1, lastVH = 1;
let glDirty = true;
function draw3d() {
  if (!gl && !initGL()) return;
  const r = glc.getBoundingClientRect();
  const w = Math.round(r.width*devicePixelRatio);
  const h = Math.round(r.height*devicePixelRatio);
  if (glc.width !== w || glc.height !== h) {   /* reallocates the drawing buffer */
    glc.width = w; glc.height = h;
  }
  gl.viewport(0,0,glc.width,glc.height);
  gl.clearColor(0.05,0.06,0.08,1);
  /* diorama.c uses SDL's painter with no depth test. Match it: z changes the
   * projection, while the resolved INI paint order decides overlap. */
  gl.disable(gl.DEPTH_TEST); gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

  const captured = nativeBandSurfaces();
  if (glDirty) {
    for(let bg=0;bg<2;bg++)for(let b=0;b<3;b++){
      gl.bindTexture(gl.TEXTURE_2D,nativeBandTex[bg][b]);
      gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,
                    captured.images[bg][b]);
    }
    glDirty = false;
  }
  /* World units: one authentic screen-height = 1.0, matching the capture
   * geometry the runtime gives its presentation renderer. */
  const unit = 1 / DATA.frameHeight;
  const W = captured.width*unit, H = 1.0;
  const cx = Math.cos(orbit.yaw), sx = Math.sin(orbit.yaw);
  const cp = Math.cos(orbit.pitch), sp = Math.sin(orbit.pitch);
  /* Orbit focus is local to the visible capture. The native camera sliders
   * select which part of the full room was captured before projection. */
  const fx = (orbit.focusX - 0.5) * W, fy = orbit.focusY * H;
  const eye = [fx + sx*cp*orbit.dist, fy - sp*orbit.dist, cx*cp*orbit.dist];
  const mvp = mul(perspective(0.9, glc.width/glc.height, 0.01, 60),
                  lookAt(eye[0],eye[1],eye[2], fx,fy,0));
  const framing=roomFraming(room);
  for(let row=0;row<4;row++)
    mvp[12+row]+=(-framing.x*mvp[row]+framing.y*mvp[4+row])*unit;
  gl.uniformMatrix4fv(gl.getUniformLocation(prog,'uMVP'), false, new Float32Array(mvp));
  lastMVP = mvp; lastVW = glc.clientWidth; lastVH = glc.clientHeight;

  const aPos = gl.getAttribLocation(prog,'aPos'), aUV = gl.getAttribLocation(prog,'aUV');
  const aShade = gl.getAttribLocation(prog,'aShade');
  const uOrigin = gl.getUniformLocation(prog,'uOrigin');
  const uTint = gl.getUniformLocation(prog,'uTint');
  gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
  gl.enableVertexAttribArray(aPos); gl.enableVertexAttribArray(aUV);
  gl.enableVertexAttribArray(aShade);
  gl.vertexAttribPointer(aPos,3,gl.FLOAT,false,24,0);
  gl.vertexAttribPointer(aUV ,2,gl.FLOAT,false,24,12);
  gl.vertexAttribPointer(aShade,1,gl.FLOAT,false,24,20);
  const drawMesh = (vertices, z, tex, alpha, shade=1) => {
    gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(vertices),gl.DYNAMIC_DRAW);
    gl.uniform3f(uOrigin,0,0,z);
    gl.uniform4f(uTint,shade,shade,shade,alpha);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.drawArrays(gl.TRIANGLES,0,vertices.length/6);
  };
  const gridMesh = point => {
    const vertices=[];
    const push=(s,t)=>vertices.push(...point(s,t));
    for(let row=0;row<6;row++)for(let col=0;col<8;col++){
      const s0=col/8,s1=(col+1)/8,t0=row/6,t1=(row+1)/6;
      push(s0,t0);push(s0,t1);push(s1,t0);
      push(s1,t0);push(s0,t1);push(s1,t1);
    }
    return vertices;
  };
  const planeMesh = (rake,bow) => gridMesh((s,t)=>[
    (s-.5)*W,(.5-t)*H,rake*t+bow*t*t,s,t,1]);
  const skirtMesh = (rake,bow,thickness) => gridMesh((s,t)=>[
    (s-.5)*W,-.5*H-t*thickness*.5,rake+bow+t*thickness,
    s,1,1-.45*t]);
  const flatMesh=planeMesh(0,0);
  const drawSurface = (item,tex,alpha) => {
    const token=item.b===0?null:(item.bg===0
      ?(item.b===1?'bg1':'bg1hi'):(item.b===1?'bg2':'bg2hi'));
    const shape=token?resolvedPlane(token):{
      rake:0,bow:0,thickness:0,stack:0,copies:0,direction:0,solid:false};
    const mesh=shape.rake||shape.bow?planeMesh(shape.rake,shape.bow):flatMesh;
    if(shape.stack>0&&shape.copies>1){
      for(let c=shape.copies-1;c>=0;c--){
        if(shape.direction===2
            ?(shape.copies%2===1&&c===(shape.copies-1)/2):c===0)continue;
        const f=c/(shape.copies-1);
        let z=0,distance=f;
        if(shape.direction===1)z=-f*shape.stack;
        else if(shape.direction===2){z=(f-.5)*shape.stack;distance=Math.abs(f-.5)/.5;}
        else z=f*shape.stack;
        const shade=shape.solid?.88:1-.5*distance;
        const copyAlpha=shape.solid?1:1-.65*distance;
        drawMesh(mesh,bandZ(item.bg,item.b)+z,tex,alpha*copyAlpha,shade);
      }
    }
    if(shape.thickness>0)
      drawMesh(skirtMesh(shape.rake,shape.bow,shape.thickness),
               bandZ(item.bg,item.b),tex,alpha);
    drawMesh(mesh,bandZ(item.bg,item.b),tex,alpha);
  };
  /* Resolve the same independent paint keys as DioramaLayerOrder_Resolve.
   * Virtual planes share their anchor's default key and are stably inserted
   * immediately before it. Existing INI `order` values therefore retain their
   * current meaning when the new planes are introduced. */
  const bgState = bg => room.bg[bg] && (showBoth || bg===bgIndex)
    ? {texes:nativeBandTex[bg]} : null;
  const baseSequence = [
    {bg:1,b:0}, {bg:1,b:1}, {bg:0,b:0}, {bg:0,b:1},
    {actor:true}, {bg:1,b:2}, {bg:0,b:2},
  ];
  const items = baseSequence.map((item,sequence) => ({...item,sequence}))
    .filter(item => item.actor ? actor.show : !!bgState(item.bg));
  for (const item of items) item.order = item.actor
    ? resolvedPlane('obj2').order : bandPaintOrder(item.bg,item.b);
  items.sort((a,b) => a.order-b.order || a.sequence-b.sequence);
  for (const item of items) {
    if (item.actor) { drawActor3d(unit,DATA.frameWidth*unit,H); continue; }
    const source = bgState(item.bg);
    const alpha = bandAlpha(item.bg,item.b) / 255;
    drawSurface(item,source.texes[item.b],alpha);
  }
}

/* One untextured quad at the resolved OBJ2 plane. The runtime compositor is a
 * painter, so it participates in the same configured paint ordering as every
 * captured plane; z independently controls its 3D position. */
let actorTex = null;
function actorTexture() {
  if (actorTex) return actorTex;
  const c = document.createElement('canvas');
  c.width = 24; c.height = 32;
  const g = c.getContext('2d');
  g.fillStyle = 'rgba(255,86,120,0.92)';          /* stands out against art */
  g.fillRect(5, 0, 14, 10);                        /* head                  */
  g.fillRect(2, 10, 20, 14);                       /* body                  */
  g.fillRect(4, 24, 6, 8); g.fillRect(14, 24, 6, 8);
  g.strokeStyle = 'rgba(255,255,255,0.85)'; g.lineWidth = 1;
  g.strokeRect(2.5, 0.5, 19, 31);
  actorTex = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, actorTex);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, c);
  return actorTex;
}
function drawActor3d(unit, W, H) {
  const aw = actor.w * unit, ah = actor.h * unit;
  /* Level pixels -> camera-local capture space. */
  const cxw = (actor.x-nativeCamera.x + actor.w/2) * unit - W/2;
  const cyw = H/2 - (actor.y-nativeCamera.y + actor.h/2) * unit;
  const verts = new Float32Array([
    cxw-aw/2,cyw+ah/2,0, 0,0,1,  cxw-aw/2,cyw-ah/2,0, 0,1,1,
    cxw+aw/2,cyw+ah/2,0, 1,0,1,  cxw+aw/2,cyw+ah/2,0, 1,0,1,
    cxw-aw/2,cyw-ah/2,0, 0,1,1,  cxw+aw/2,cyw-ah/2,0, 1,1,1 ]);
  gl.bufferData(gl.ARRAY_BUFFER, verts, gl.DYNAMIC_DRAW);
  gl.uniform3f(gl.getUniformLocation(prog,'uOrigin'), 0, 0, actorZ());
  gl.uniform4f(gl.getUniformLocation(prog,'uTint'), 1,1,1,1);
  gl.bindTexture(gl.TEXTURE_2D, actorTexture());
  gl.drawArrays(gl.TRIANGLES,0,6);
}
/* Level-space point under a 3D pointer, on the actor's own plane. */
function actorPointFromEvent(ev) {
  if (!lastMVP || !L) return null;
  const r = glc.getBoundingClientRect();
  const unit = 1 / DATA.frameHeight;
  const W = DATA.frameWidth*unit, H = 1.0;
  const hit = unprojectToPlane(lastMVP, ev.clientX-r.left, ev.clientY-r.top,
                               actorZ(), r.width, r.height);
  if (!hit) return null;
  return [ nativeCamera.x+(hit[0]+W/2)/unit,
           nativeCamera.y+(H/2-hit[1])/unit ];
}
function actorHit3d(ev) {
  const p = actorPointFromEvent(ev);
  if (!p) return false;
  return p[0] >= actor.x - 6 && p[0] <= actor.x + actor.w + 6 &&
         p[1] >= actor.y - 6 && p[1] <= actor.y + actor.h + 6;
}
glc.addEventListener('mousedown', ev => {
  /* Capture the pointer so a drag that leaves the canvas keeps tracking
   * instead of stopping and resuming somewhere else. */
  if (glc.setPointerCapture && ev.pointerId !== undefined)
    try { glc.setPointerCapture(ev.pointerId); } catch (_) {}
  if (actor.show && !ev.shiftKey && actorHit3d(ev)) {
    const p = actorPointFromEvent(ev);
    drag = { actor3d:true, ox: p[0]-actor.x, oy: p[1]-actor.y };
    return;
  }
  drag = { orbit:true, pan:ev.shiftKey, x:ev.clientX, y:ev.clientY }; });
window.addEventListener('mousemove', ev => {
  if (mode !== '3d' || !drag) return;
  if (drag.actor3d) {
    const p = actorPointFromEvent(ev);
    if (p) { actor.x = Math.round(p[0]-drag.ox); actor.y = Math.round(p[1]-drag.oy); draw(); }
    return;
  }
  if (!drag.orbit) return;
  if (drag.pan) {          /* shift-drag moves the authentic room camera */
    const dx=ev.clientX-drag.x,dy=ev.clientY-drag.y;
    nativeCamera.x-=Math.round(dx);nativeCamera.y-=Math.round(dy);
    refreshNativeCameraControls();invalidateGameComposite();glDirty=true;
    drag.x=ev.clientX; drag.y=ev.clientY; draw(); return;
  }
  /* Clamp the per-event delta. A pointer that leaves the window and comes
   * back, or a coalesced burst after a stall, arrives as one enormous jump
   * and teleports the camera; capping it turns that into a fast sweep. */
  const clamp = (v, m) => Math.max(-m, Math.min(m, v));
  orbit.yaw   += clamp(ev.clientX-drag.x, 60) * 0.0035;
  orbit.pitch += clamp(ev.clientY-drag.y, 60) * 0.0030;
  orbit.pitch = Math.max(-1.2, Math.min(0.5, orbit.pitch));
  drag.x=ev.clientX; drag.y=ev.clientY; draw();
});
glc.addEventListener('wheel', ev => { ev.preventDefault();
  orbit.dist = Math.max(0.4, Math.min(9, orbit.dist * Math.exp(ev.deltaY*0.0012)));
  draw(); }, { passive:false });
