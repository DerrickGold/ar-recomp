/* World-coordinate emitter tools. Drag drafts are UI-only; release submits one
 * atomic recipe edit and one undo entry through the shared C validator. */
const EmitterMapTools=(()=>{
  let gesture=null;
  const colors={'soft-light':'#ffe6a0',motes:'#ffc695','free-mist':'#adbbff','ground-mist':'#85c6ef','particle-area':'#ffc695','light-fan':'#ffe6a0','water-surface':'#85c6ef',drips:'#85c6ef','waterfall-spray':'#85c6ef','cloud-bank':'#adbbff',exposure:'#cc92ff','wet-contour':'#85c6ef'};
  function point(ev) {
    const rect=cvs.getBoundingClientRect();
    return {x:(ev.clientX-rect.left-view.x)/view.scale,
      y:(ev.clientY-rect.top-view.y)/view.scale};
  }
  const snapped=(n,ev)=>Math.round(n/(ev.altKey?16:1))*(ev.altKey?16:1);
  const bounded=n=>Math.max(-2048,Math.min(16384,Math.round(n)));
  function activate(place) {
    finishFramingDrag();cancel();setMode('2d');setLayer(0);$('#bSelect').onclick();
    brush=place?'effectPlace':'effects';cvs.style.cursor=place?'crosshair':'default';
    $('#effectGuides').checked=true;refreshEditorFeedback();draw();
  }
  $('#emitterMapEdit').onclick=()=>activate(false);
  $('#emitterMapPlace').onclick=()=>activate(true);
  $('#effectGuides').onchange=()=>draw();
  function corners(e) {
    return [[-1,-1],[1,-1],[-1,1],[1,1]].map(([sx,sy])=>
      ({x:e.x+sx*e.width/2,y:e.y+sy*e.height/2,sx,sy}));
  }
  function begin(ev) {
    if(bgIndex!==0)return false;
    const p=point(ev);
    if(brush==='effectPlace') {
      EffectEditor.placeEmitter($('#emitterPreset').value,snapped(p.x,ev),snapped(p.y,ev));return true;
    }
    const entries=EffectEditor.mapEmitters(),selected=entries.find(e=>e.id===EffectEditor.selected());
    const radius=8/view.scale;
    const corner=selected&&corners(selected).find(c=>Math.abs(c.x-p.x)<=radius&&Math.abs(c.y-p.y)<=radius);
    const centre=entries.find(e=>Math.hypot(e.x-p.x,e.y-p.y)<=radius);
    const body=entries.filter(e=>Math.abs(e.x-p.x)<=e.width/2&&Math.abs(e.y-p.y)<=e.height/2)
      .sort((a,b)=>a.width*a.height-b.width*b.height)[0];
    const picked=corner?selected:centre||body;
    if(!picked)return true;
    EffectEditor.selectEmitter(picked.id);
    gesture={scope:sceneKey(room),initial:picked,draft:picked,start:p,corner};
    draw();return true;
  }
  function move(ev) {
    if(!gesture)return;
    if(gesture.scope!==sceneKey(room)){cancel();return;}
    const p=point(ev),e=gesture.initial;
    if(gesture.corner) {
      p.x=snapped(p.x,ev);p.y=snapped(p.y,ev);
      const c=gesture.corner, fixed={x:e.x-c.sx*e.width/2,y:e.y-c.sy*e.height/2};
      const limit=['particle-area','exposure'].includes(e.kind)?16384:512;
      const width=Math.max(4,Math.min(limit,2*Math.round(Math.abs(p.x-fixed.x)/2)));
      const height=Math.max(4,Math.min(limit,2*Math.round(Math.abs(p.y-fixed.y)/2)));
      gesture.draft={...e,width,height,x:bounded(fixed.x+(p.x<fixed.x?-1:1)*width/2),
        y:bounded(fixed.y+(p.y<fixed.y?-1:1)*height/2)};
    } else gesture.draft={...e,x:bounded(snapped(e.x+p.x-gesture.start.x,ev)),
      y:bounded(snapped(e.y+p.y-gesture.start.y,ev))};
    draw();
  }
  function finish() {
    if(!gesture)return;
    const {initial,draft,scope}=gesture;gesture=null;
    if(scope!==sceneKey(room))return;
    if(['x','y','width','height'].every(key=>draft[key]===initial[key]))return;
    EffectEditor.editEmitter(initial.id,{x:draft.x,y:draft.y,width:draft.width,height:draft.height},
      'move or resize emitter');
  }
  function cancel(){gesture=null;draw();}
  window.addEventListener('blur',()=>{if(gesture)cancel();});
  function drawGuides(ctx,view,viewport) {
    if(!$('#effectGuides').checked && !['effects','effectPlace'].includes(brush))return;
    if(gesture&&gesture.scope!==sceneKey(room))gesture=null;
    ctx.save();ctx.lineWidth=1.5;
    for(const original of EffectEditor.mapEmitters()) {
      const e=gesture?.initial.id===original.id?gesture.draft:original;
      const x=view.x+(e.x-e.width/2)*view.scale,y=view.y+(e.y-e.height/2)*view.scale;
      const w=e.width*view.scale,h=e.height*view.scale;
      if(x+w<0||y+h<0||x>viewport.width||y>viewport.height)continue;
      const selected=e.id===EffectEditor.selected();
      if(e.points) {
        ctx.beginPath();ctx.strokeStyle='#85c6ef';
        e.points.split(/\s+/).forEach((pair,i)=>{const [px,py]=pair.split(',').map(Number);ctx[i?'lineTo':'moveTo'](view.x+(e.x+px)*view.scale,view.y+(e.y+py)*view.scale);});ctx.stroke();
      }
      ctx.strokeStyle=selected?'#fff19b':colors[e.kind];
      ctx.setLineDash(e.enabled?[]:[4,4]);ctx.strokeRect(x,y,w,h);ctx.setLineDash([]);
      const cx=view.x+e.x*view.scale,cy=view.y+e.y*view.scale;
      ctx.fillStyle=ctx.strokeStyle;ctx.fillRect(cx-4,cy-1,8,2);ctx.fillRect(cx-1,cy-4,2,8);
      if(selected&&brush==='effects') {
        for(const c of corners(e)) {
          const px=view.x+c.x*view.scale,py=view.y+c.y*view.scale;
          ctx.fillStyle='#131722';ctx.fillRect(px-4,py-4,8,8);ctx.strokeRect(px-4,py-4,8,8);
        }
        ctx.fillStyle='#fff19b';ctx.fillText(`${e.kind} · ${e.x}, ${e.y} · ${e.width}×${e.height}`,x,y-8);
      }
    }
    ctx.restore();
  }
  return {begin,move,finish,cancel,draw:drawGuides};
})();
