/* Designer framing is independent of the authoring rectangle and gameplay
 * camera. Match BG1 terrain families so different boss layouts can be tuned. */
const roomFraming=r=>stampBucket(r,0).framing||{x:0,y:0};
let showFramingGuide=true;
let framingPixelAspect=window.__ACTION_VIEW__?.pixelAspect||'crt';
let framingProjection=window.__ACTION_VIEW__?'diorama':'native';
let framingDistance=window.__ACTION_VIEW__?.distance||3.25;
const wideFramingGuides=[
  {id:'framing169',label:'16:9',ratio:16/9,color:'#65dce9',show:true},
  {id:'framing1610',label:'16:10',ratio:16/10,color:'#c49aff',show:true},
];
const framingRect=(adjusted=true,ratio=null)=>{
  const f=adjusted?roomFraming(room):{x:0,y:0};
  const par=framingPixelAspect==='crt'?7/6:1;
  if(ratio&&framingProjection==='diorama') {
    // Runtime FOV is 0.4 radians; one world unit is 224 native pixels.
    // This is the untilted BG1 plane, before dynamic vertical edge alignment.
    const depth=Math.max(.01,framingDistance-(resolvedPlane('bg1').z-.5));
    const height=2*depth*Math.tan(.4/2)*DATA.frameHeight;
    const w=Math.ceil(height*ratio/par),h=Math.ceil(height);
    return {x:nativeCamera.x+f.x+(DATA.frameWidth-w)/2,
      y:nativeCamera.y+1+f.y+(DATA.frameHeight-h)/2,w,h};
  }
  // Match HostDisplay_ResolveVideoGeometry: expand equally in whole columns.
  const extra=ratio?Math.max(0,Math.ceil((Math.ceil(DATA.frameHeight*ratio/par)-DATA.frameWidth)/2)):0;
  // Native scanout starts at camera Y + 1, matching nativeBandSurfaces.
  return {x:nativeCamera.x+f.x-extra,y:nativeCamera.y+1+f.y,
    w:DATA.frameWidth+2*extra,h:DATA.frameHeight};
};
function framingCoverageRect(adjusted=true) {
  return wideFramingGuides.filter(g=>g.show).map(g=>framingRect(adjusted,g.ratio))
    .reduce((a,b)=>b.w>a.w?b:a,framingRect(adjusted));
}
function framingIniLines(r) {
  return regionalRecords(r,0,variant=>{
    const f=stampBucket(variant,0).framing;
    return f?[`framing = x:${f.x} y:${f.y}`]:[];
  });
}
function refreshFramingControls() {
  if(!L)return;
  const f=roomFraming(room);
  $('#framingX').value=String(f.x);$('#framingY').value=String(f.y);
  $('#framingReset').disabled=!stampBucket(room,0).framing;
  $('#showFraming').checked=showFramingGuide;
  $('#framingPixelAspect').value=framingPixelAspect;
  $('#framingProjection').value=framingProjection;
  $('#framingDistance').value=String(framingDistance);
  $('#framingDistance').disabled=framingProjection!=='diorama'&&!showCoverageGuide;
  for(const guide of wideFramingGuides)$(`#${guide.id}`).checked=guide.show;
  $('#frameTool').classList.toggle('on',mode==='2d'&&brush==='framing');
  $('#frameTool').setAttribute('aria-pressed',String(mode==='2d'&&brush==='framing'));
  const regions=editFamily(room,0).mask;
  $('#framingInfo').textContent='Shared by '+TERRAIN_TOKENS.filter((_,i)=>regions&(1<<i))
    .map(r=>r.toUpperCase()).join(' + ')+'. Applies to the whole Diorama scene.';
  $('#framingGuideInfo').textContent=`Native ${DATA.frameWidth}×${DATA.frameHeight} at preview scroll `
    +`${nativeCamera.x}, ${nativeCamera.y}. Saved offset ${f.x}, ${f.y} px.`;
  $('#framingViewportInfo').textContent=wideFramingGuides.filter(g=>g.show)
    .map(g=>{const r=framingRect(true,g.ratio);
      return `${g.label}: ${framingProjection==='diorama'?'≈':''}${r.w}×${r.h} map pixels`;}).join(' · ');
  refreshCoverageControls();
}
function writeRoomFraming(next) {
  const target=stampBucket(room,0),part=partFor(keyOf(room,0));
  if(JSON.stringify(target.framing)===JSON.stringify(next))return false;
  if(part&&!('framing' in part.before))part.before.framing=target.framing;
  target.framing=next;nativeBandCache=null;glDirty=true;
  refreshFramingControls();draw();return true;
}
function setRoomFraming(reset=false) {
  finishFramingDrag();
  const x=Number($('#framingX').value),y=Number($('#framingY').value);
  if(!reset&&(!Number.isInteger(x)||!Number.isInteger(y)||Math.abs(x)>64||Math.abs(y)>64)) {
    refreshFramingControls();tileActionStatus('Framing offsets must be between −64 and 64 pixels.');return false;
  }
  const target=stampBucket(room,0),next=reset?undefined:{x,y};
  if(JSON.stringify(target.framing)===JSON.stringify(next))return false;
  beginOp(reset?'reset room framing':'adjust room framing');
  writeRoomFraming(next);commitOp();
  draw();tileActionStatus('Room framing applied. Export level INI saves it for the game.');
  return true;
}
function startFramingTool() {
  finishFramingDrag();setMode('2d');
  if(bgIndex!==0)setLayer(0);
  returnToEditedTiles();showFramingGuide=true;brush='framing';stampHover=null;
  Object.values(brushBtns).forEach(b=>b.classList.remove('on'));
  fitFramingGuides();
  cvs.style.cursor='move';cvs.focus();refreshSelectionControls();draw();
  tileActionStatus('Drag any viewport frame. Arrow keys move 1 px; Shift + arrows move 16 px. Esc returns to Select.');
}
function fitFramingGuides() {
  const r=framingCoverageRect(false),canvas=cvs.getBoundingClientRect();
  view.scale=Math.min(canvas.width/(r.w+160),canvas.height/(r.h+160),4);
  view.x=canvas.width/2-(r.x+r.w/2)*view.scale;
  view.y=canvas.height/2-(r.y+r.h/2)*view.scale;
}
function beginFramingDrag(ev) {
  const canvas=cvs.getBoundingClientRect(),r=framingCoverageRect();
  const x=(ev.clientX-canvas.left-view.x)/view.scale,y=(ev.clientY-canvas.top-view.y)/view.scale;
  const pad=7/view.scale;
  if(x<r.x-pad||y<r.y-pad||x>r.x+r.w+pad||y>r.y+r.h+pad)return;
  beginOp('drag room framing');cvs.focus();ev.preventDefault();
  drag={framing:true,key:keyOf(room,0),x:ev.clientX,y:ev.clientY,scale:view.scale,
    offset:{...roomFraming(room)},before:stampBucket(room,0).framing};
  cvs.style.cursor='grabbing';
}
function moveFramingDrag(ev) {
  const x=Math.round(drag.offset.x+(ev.clientX-drag.x)/drag.scale);
  const y=Math.round(drag.offset.y+(ev.clientY-drag.y)/drag.scale);
  writeRoomFraming({x:Math.max(-64,Math.min(64,x)),y:Math.max(-64,Math.min(64,y))});
  tileActionStatus(Math.abs(x)>64||Math.abs(y)>64
    ?'Framing limit: 64 px from the native view in each direction.'
    :'Release to apply framing. Esc cancels this drag.');
}
function finishFramingDrag(cancel=false) {
  if(!drag?.framing)return false;
  const target=stampStore[drag.key],f=target.framing||{x:0,y:0};
  const changed=f.x!==drag.offset.x||f.y!==drag.offset.y;
  if(cancel||!changed)target.framing=drag.before;
  drag=null;commitOp();cvs.style.cursor=brush==='framing'?'move':'default';
  nativeBandCache=null;glDirty=true;
  refreshFramingControls();draw();
  tileActionStatus(cancel?'Framing drag canceled.':changed
    ?'Framing applied. Export level INI saves it for the game.':'Framing unchanged.');
  return true;
}
function nudgeFraming(dx,dy) {
  finishFramingDrag();
  const f=roomFraming(room);beginOp('nudge room framing');
  writeRoomFraming({x:Math.max(-64,Math.min(64,f.x+dx)),y:Math.max(-64,Math.min(64,f.y+dy))});
  commitOp();
}
function drawFramingGuide() {
  if(!showFramingGuide||bgIndex!==0)return;
  const base=framingRect(false),frame=framingRect(),s=view.scale;
  const rect=r=>[view.x+r.x*s,view.y+r.y*s,r.w*s,r.h*s];
  const [x,y,w,h]=rect(frame),canvas=cvs.getBoundingClientRect();
  ctx.save();
  ctx.font='12px monospace';ctx.textBaseline='middle';
  const labelAt=(text,lx,ly,color)=>{
    const width=ctx.measureText(text).width+16;
    lx=Math.max(4,Math.min(canvas.width-width-4,lx));
    ly=Math.max(4,Math.min(canvas.height-28,ly));
    ctx.fillStyle='#20232b';ctx.fillRect(lx,ly,width,23);
    ctx.fillStyle=color;ctx.fillText(text,lx+8,ly+12);
  };
  for(const guide of wideFramingGuides.filter(g=>g.show)) {
    const [gx,gy,gw,gh]=rect(framingRect(true,guide.ratio));
    ctx.lineWidth=2;ctx.strokeStyle=guide.color;ctx.setLineDash([]);
    ctx.strokeRect(gx,gy,gw,gh);
    if(gx+gw>=0&&gy+gh>=0&&gx<=canvas.width&&gy<=canvas.height)
      labelAt(guide.label,gx+gw-ctx.measureText(guide.label).width-16,
        guide.id==='framing169'?gy-27:gy+gh+4,guide.color);
  }
  ctx.lineWidth=2;ctx.strokeStyle='#e6eaf2';ctx.setLineDash([6,5]);
  ctx.strokeRect(...rect(base));ctx.setLineDash([]);
  ctx.strokeStyle='#ffdc74';ctx.fillStyle='rgba(255,220,116,0.055)';
  ctx.fillRect(x,y,w,h);ctx.strokeRect(x,y,w,h);
  for(const [cx,cy] of [[x,y],[x+w,y],[x,y+h],[x+w,y+h]]) {
    ctx.fillStyle='#ffdc74';ctx.fillRect(cx-3,cy-3,6,6);
  }
  if(x+w<0||y+h<0||x>canvas.width||y>canvas.height){ctx.restore();return;}
  const f=roomFraming(room);
  labelAt(`Native ${frame.w}×${frame.h} · View ${f.x}, ${f.y}`,x,y-53,'#ffdc74');
  ctx.restore();
}
$('#framingX').onchange=$('#framingY').onchange=()=>setRoomFraming();
$('#framingReset').onclick=()=>setRoomFraming(true);
$('#framingAdjust').onclick=startFramingTool;
$('#frameTool').onclick=()=>mode==='2d'&&brush==='framing'?$('#bSelect').onclick():startFramingTool();
$('#showFraming').onchange=()=>{showFramingGuide=$('#showFraming').checked;
  if(!showFramingGuide&&brush==='framing')$('#bSelect').onclick();draw();};
function refreshViewportGuides() {
  finishFramingDrag();refreshFramingControls();
  if(mode==='2d'&&brush==='framing')fitFramingGuides();
  draw();
}
for(const guide of wideFramingGuides)$(`#${guide.id}`).onchange=()=>{
  guide.show=$(`#${guide.id}`).checked;refreshViewportGuides();
};
$('#framingPixelAspect').onchange=()=>{
  framingPixelAspect=$('#framingPixelAspect').value==='square'?'square':'crt';refreshViewportGuides();
};
$('#framingProjection').onchange=()=>{
  framingProjection=$('#framingProjection').value==='diorama'?'diorama':'native';refreshViewportGuides();
};
$('#framingDistance').onchange=()=>{
  const value=Number($('#framingDistance').value);
  if(Number.isFinite(value)&&value>=2&&value<=20)framingDistance=value;
  refreshViewportGuides();
};
window.addEventListener('blur',()=>finishFramingDrag());
