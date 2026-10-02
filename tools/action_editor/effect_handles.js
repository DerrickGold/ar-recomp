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
    finishFramingDrag();cancel();setMode('2d');$('#bSelect').onclick();
    brush=place?'effectPlace':'effects';cvs.style.cursor=place?'crosshair':'default';
    $('#effectGuides').checked=true;refreshEditorFeedback();draw();
  }
  $('#emitterMapEdit').onclick=()=>activate(false);
  $('#emitterMapPlace').onclick=()=>activate(true);
  $('#effectGuides').onchange=()=>draw();
  function corners(e) {
    if(e.kind==='light-fan')return [...[-1,1].map(sx=>({...EffectGeometry.rayPoint(e,0,sx),sx,sy:0,ray:true})),{...EffectGeometry.rayPoint(e,1),sx:0,sy:1,ray:true}];
    if(EffectGeometry.isRay(e))return [-1,1].map(sx=>({...EffectGeometry.rayPoint(e,1,sx),sx,sy:1,ray:true}));
    const axes=e.resizeWidth&&e.resizeHeight?[[-1,-1],[1,-1],[-1,1],[1,1]]:
      e.resizeWidth?[[-1,0],[1,0]]:e.resizeHeight?[[0,-1],[0,1]]:[];
    return axes.filter(([,sy])=>e.anchorY!==0||sy>=0).map(([sx,sy])=>
      ({x:e.x+sx*e.width/2,y:e.y+((sy+1)/2-(e.anchorY??.5))*e.height,sx,sy}));
  }
  function hits(ev) {
    if(!$('#effectGuides').checked)return [];
    const p=point(ev),radius=8/view.scale;
    return EffectEditor.mapEmitters().filter(e=>Math.hypot(e.x-p.x,e.y-p.y)<=radius)
      .sort((a,b)=>Math.hypot(a.x-p.x,a.y-p.y)-Math.hypot(b.x-p.x,b.y-p.y)||
        Number(b.id===EffectEditor.selected())-Number(a.id===EffectEditor.selected()));
  }
  function hit(ev,body=false) {
    if(!$('#effectGuides').checked)return null;
    const p=point(ev),entries=EffectEditor.mapEmitters(),radius=8/view.scale;
    const selected=entries.find(e=>e.id===EffectEditor.selected());
    const rotation=selected&&EffectGeometry.rotation(selected,view.scale);
    if(rotation&&Math.hypot(rotation.x-p.x,rotation.y-p.y)<=radius)return {entry:selected,corner:rotation};
    const corner=selected&&corners(selected).find(c=>Math.abs(c.x-p.x)<=radius&&Math.abs(c.y-p.y)<=radius);
    const centre=hits(ev)[0];
    if(centre&&Math.hypot(centre.x-p.x,centre.y-p.y)<=6/view.scale)
      return {entry:centre,corner:null};
    const area=body&&entries.filter(e=>EffectGeometry.contains(e,p)).sort((a,b)=>a.width*a.height-b.width*b.height)[0];
    return corner?{entry:selected,corner}:centre||area?{entry:centre||area,corner:null}:null;
  }
  function begin(ev) {
    const p=point(ev);
    if(brush==='effectPaste'){EffectEditor.pasteEffects(snapped(p.x,ev),snapped(p.y,ev));return true;}
    if(brush==='effectPlace') {
      EffectEditor.placeEmitter($('#emitterPreset').value,snapped(p.x,ev),snapped(p.y,ev));return true;
    }
    const found=hit(ev,brush==='effects');
    if(!found)return brush==='effects';
    const picked=found.entry,corner=found.corner;
    if(ev.ctrlKey||ev.metaKey){EffectEditor.selectEmitter(picked.id,true);draw();return true;}
    if(ev.shiftKey){if(picked.movable!==false)EffectEditor.resetHandle(picked,corner?.rotate?'rotate':corner?'size':'position');draw();return true;}
    if(picked.movable===false){EffectEditor.openModal(picked.id);return true;}
    EffectEditor.selectEmitter(picked.id);
    gesture={scope:sceneKey(room),layer:bgIndex,initial:picked,draft:picked,start:p,corner};
    draw();return true;
  }
  function move(ev) {
    if(!gesture)return;
    if(gesture.scope!==sceneKey(room)||gesture.layer!==bgIndex){cancel();return;}
    const p=point(ev),e=gesture.initial;
    if(gesture.corner?.rotate) {
      const angle=EffectGeometry.angleAt(e,p),limit=e.native?30:180;
      gesture.draft={...e,angle:Math.max(-limit,Math.min(limit,Math.round(angle/(ev.altKey?15:1))*(ev.altKey?15:1)))};
    } else if(gesture.corner?.ray&&e.kind==='light-fan') {
      const o=EffectGeometry.origin(e),a=EffectGeometry.direction(e);
      gesture.draft={...e};
      if(gesture.corner.sx)gesture.draft.width=Math.max(4,Math.min(512,snapped(Math.abs(p.x-o.x)*2,ev)));
      else gesture.draft.height=Math.max(4,Math.min(512,snapped((p.x-o.x)*Math.sin(a)+(p.y-o.y)*Math.cos(a),ev)));
    } else if(gesture.corner?.ray) {
      const o=EffectGeometry.origin(e),limit=e.native?32768:512;
      let height=e.native?p.y-o.y:(p.x-o.x-gesture.corner.sx*e.width/2)*Math.sin(EffectGeometry.direction(e))+(p.y-o.y)*Math.cos(EffectGeometry.direction(e));
      height=Math.max(4,Math.min(limit,snapped(height,ev)));
      const centre=EffectGeometry.rayPoint({...e,height},1),factor=e.guide?.[3]||1;
      const width=Math.max(4,Math.min(limit,snapped(2*Math.abs(p.x-centre.x)/factor,ev)));
      gesture.draft={...e,width,height};
    } else if(gesture.corner) {
      p.x=snapped(p.x,ev);p.y=snapped(p.y,ev);
      const c=gesture.corner,anchorY=e.anchorY??.5;
      const fixed={x:e.x-c.sx*e.width/2,y:e.y+((1-c.sy)/2-anchorY)*e.height};
      const limit=e.native?32768:['particle-area','exposure'].includes(e.kind)?16384:512;
      let width=e.resizeWidth?Math.max(4,Math.min(limit,2*Math.round(Math.abs(p.x-fixed.x)/2))):e.width;
      let height=e.resizeHeight?Math.max(4,Math.min(limit,2*Math.round(Math.abs(p.y-fixed.y)/2))):e.height;
      if(e.native) {
        width=e.resizeWidth?Math.max(e.baseWidth*.25,Math.min(e.baseWidth*2,width)):e.width;
        height=e.resizeHeight?Math.max(e.baseHeight*.25,Math.min(e.baseHeight*2,height)):e.height;
      }
      gesture.draft={...e,width,height,x:e.resizeWidth?bounded(fixed.x+(p.x<fixed.x?-1:1)*width/2):e.x,
        y:e.resizeHeight?bounded(fixed.y-(p.y<fixed.y?height:0)+anchorY*height):e.y};
    } else gesture.draft={...e,x:bounded(snapped(e.x+p.x-gesture.start.x,ev)),
      y:bounded(snapped(e.y+p.y-gesture.start.y,ev))};
    if(e.native) {
      const d=gesture.draft;
      d.x=Math.max(e.baseX-(e.offsetLimitX||512),Math.min(e.baseX+(e.offsetLimitX||512),d.x));d.y=Math.max(e.baseY-(e.offsetLimitY||512),Math.min(e.baseY+(e.offsetLimitY||512),d.y));
      d.width=e.resizeWidth?Math.max(e.baseWidth*.25,Math.min(e.baseWidth*2,d.width)):e.width;
      d.height=e.resizeHeight?Math.max(e.baseHeight*.25,Math.min(e.baseHeight*2,d.height)):e.height;
      d.lengthScale=d.height/e.baseHeight;
    }
    draw();
  }
  function finish() {
    if(!gesture)return;
    const {initial,draft,scope,layer}=gesture;gesture=null;
    if(scope!==sceneKey(room)||layer!==bgIndex)return;
    if(['x','y','width','height','angle'].every(key=>draft[key]===initial[key]))return;
    if(initial.angle!==draft.angle){EffectEditor.editEmitter(initial.id,{angle:draft.angle},'rotate effect');return;}
    EffectEditor.editEmitter(initial.id,{x:draft.x-(initial.actorX||0),y:draft.y-(initial.actorY||0),width:draft.width,height:draft.height},
      'move or resize emitter');
  }
  function cancel(){gesture=null;draw();}
  window.addEventListener('blur',()=>{if(gesture)cancel();});
  function drawGuides(ctx,view,viewport) {
    if(!$('#effectGuides').checked && !['effects','effectPlace','effectPaste'].includes(brush))return;
    if(gesture&&(gesture.scope!==sceneKey(room)||gesture.layer!==bgIndex))gesture=null;
    ctx.save();ctx.lineWidth=1.5;
    for(const original of EffectEditor.mapEmitters()) {
      const e=gesture?.initial.id===original.id?gesture.draft:original;
      const x=view.x+(e.x-e.width/2)*view.scale,y=view.y+(e.y-(e.anchorY??.5)*e.height)*view.scale;
      const w=e.width*view.scale,h=e.height*view.scale;
      const path=EffectGeometry.outline(e),xs=path.map(p=>view.x+p.x*view.scale),ys=path.map(p=>view.y+p.y*view.scale);
      if(Math.max(...xs)<0||Math.max(...ys)<0||Math.min(...xs)>viewport.width||Math.min(...ys)>viewport.height)continue;
      const selected=e.id===EffectEditor.selected()||EffectEditor.isMapSelected(e.id);
      if(e.points) {
        ctx.beginPath();ctx.strokeStyle='#85c6ef';
        e.points.split(/\s+/).forEach((pair,i)=>{const [px,py]=pair.split(',').map(Number);ctx[i?'lineTo':'moveTo'](view.x+(e.x+px)*view.scale,view.y+(e.y+py)*view.scale);});ctx.stroke();
      }
      ctx.strokeStyle=selected?'#fff19b':colors[e.kind]||'#ffe6a0';
      const line=(points,closed=false)=>{
        ctx.beginPath();points.forEach((p,i)=>ctx[i?'lineTo':'moveTo'](view.x+p.x*view.scale,view.y+p.y*view.scale));
        if(closed)ctx.closePath();ctx.stroke();
      };
      if(selected||!e.family) {
        ctx.setLineDash(e.enabled?(selected?[]:[3,5]):[4,4]);line(path,true);ctx.setLineDash([]);
        if(EffectGeometry.isRay(e)) {
          const sides=e.kind==='light-fan'?[-.5,0,.5]:[0];
          for(const side of sides)line([EffectGeometry.rayPoint(e,0,side),EffectGeometry.rayPoint(e,1,side)]);
        }
      }
      const cx=view.x+e.x*view.scale,cy=view.y+e.y*view.scale;
      ctx.fillStyle=e.enabled?'#151b29':'#454951';ctx.fillRect(cx-6,cy-6,12,12);
      ctx.strokeRect(cx-6,cy-6,12,12);ctx.fillStyle=ctx.strokeStyle;
      ctx.fillRect(cx-3,cy-1,6,2);ctx.fillRect(cx-1,cy-3,2,6);
      if(e.family&&selected)ctx.fillText(e.label||'Room '+e.kind,cx+10,cy+4);
      if(selected&&!e.family) {
        for(const c of corners(e)) {
          if(!e.resizeWidth&&!e.resizeHeight)continue;
          const px=view.x+c.x*view.scale,py=view.y+c.y*view.scale;
          ctx.fillStyle='#131722';ctx.fillRect(px-4,py-4,8,8);ctx.strokeRect(px-4,py-4,8,8);
        }
        const rotation=EffectGeometry.rotation(e,view.scale);
        if(rotation) {
          line([EffectGeometry.origin(e),rotation]);
          const rx=view.x+rotation.x*view.scale,ry=view.y+rotation.y*view.scale;
          ctx.beginPath();ctx.arc(rx,ry,6,0,Math.PI*2);ctx.fillStyle='#131722';ctx.fill();ctx.stroke();
          ctx.fillText(`${Math.round(e.angle||0)}°`,rx+10,ry+4);
        }
        ctx.fillStyle='#fff19b';ctx.fillText(`${e.kind} · BG${bgIndex+1} · ${Math.round(e.x)}, ${Math.round(e.y)} · ${Math.round(e.width)}×${Math.round(e.height)}`,x,y-8);
      }
    }
    ctx.restore();
  }
  cvs.addEventListener('dblclick',ev=>{
    if(mode!=='2d')return;
    const found=hit(ev);if(found){ev.preventDefault();cancel();drag=null;EffectEditor.openModal(found.entry.id);}
  });
  return {point,hit,hits,begin,move,finish,cancel,draw:drawGuides};
})();
