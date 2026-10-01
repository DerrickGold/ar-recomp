/* Bloodpool 02:08's authored 512x320 envelope is a measured minimum at
 * 16:10 / square pixels / distance 3.25. Keep that calibration independent
 * of later room edits, then scale its inverse-projected footprint for the
 * chosen viewport and plane depth. Empty workspace never counts as scenery. */
const coverageReference={width:512,height:320,aspect:16/10,distance:3.25};
const coverageCalibration=coverageReference.height/
  (2*coverageReference.distance*Math.tan(.4/2)*DATA.frameHeight);
let showCoverageGuide=!!window.__ACTION_VIEW__,coverageBasis='calibrated';
let coverageAspect=['16:10','16:9','4:3'].includes(window.__ACTION_VIEW__?.aspect)
  ?window.__ACTION_VIEW__.aspect:'16:10';
let coverageTiltX=window.__ACTION_VIEW__?.tiltX||0,coverageTiltY=window.__ACTION_VIEW__?.tiltY||0;
let coverageCache=null;
const coverageUnion=(a,b)=>({x0:Math.min(a.x0,b.x0),y0:Math.min(a.y0,b.y0),
  x1:Math.max(a.x1,b.x1),y1:Math.max(a.y1,b.y1)});
const coverageShortage=(have,need)=>({left:Math.max(0,have.x0-need.x0),
  right:Math.max(0,need.x1-have.x1),top:Math.max(0,have.y0-need.y0),bottom:Math.max(0,need.y1-have.y1)});
const coverageFits=missing=>Object.values(missing).every(n=>n===0);
const coverageEditable=b=>b.x0>=-512&&b.y0>=-512&&b.x1<=512&&b.y1<=512&&
  b.x1-b.x0<=512&&b.y1-b.y0<=512;
function coveragePlaneDepths() {
  const bands=new Set(),stamps=stampBucket(room,bgIndex).cells;
  for(let cy=0;cy<L.cellsH;cy++)for(let cx=0;cx<L.cellsW;cx++) {
    const pasted=stamps[`${cx},${cy}`];
    if(pasted)continue;
    for(let q=0;q<4;q++)bands.add(bandAt(st,L,cx*2+(q&1),cy*2+(q>>1)));
  }
  for(const tile of Object.values(stamps))tile.bands.forEach(b=>bands.add(b));
  const depths=[];
  for(const band of bands) {
    if(!bandAlpha(bgIndex,band))continue;
    const plane=band===0?resolvedVirtual(bgIndex):resolvedPlane(`bg${bgIndex+1}${band===2?'hi':''}`);
    // Runtime row depth is z + rake*t + bow*t*t. Its endpoint/critical
    // values also bound the mesh's linearly interpolated rows.
    const rake=plane.rake||0,bow=plane.bow||0,rows=[0,1];
    const critical=bow?-rake/(2*bow):-1;
    if(critical>0&&critical<1)rows.push(critical);
    const stack=plane.stack||0,offsets=plane.direction===1?[-stack,0]
      :plane.direction===2?[-stack/2,stack/2]:[0,stack];
    for(const t of rows)for(const offset of offsets) {
      depths.push(plane.z-.5+rake*t+bow*t*t+offset);
    }
  }
  return depths.length?[Math.min(...depths),Math.max(...depths)]:[bandZ(bgIndex,1)-.5];
}
function coverageFootprint() {
  const [rw,rh]=coverageAspect.split(':').map(Number),ratio=rw/rh;
  const cx=Math.cos(coverageTiltX),sx=Math.sin(coverageTiltX);
  const cy=Math.cos(coverageTiltY),sy=Math.sin(coverageTiltY);
  const rotateX=[1,0,0,0,0,cx,sx,0,0,-sx,cx,0,0,0,0,1];
  const rotateY=[cy,0,sy,0,0,1,0,0,-sy,0,cy,0,0,0,0,1];
  const translate=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,-framingDistance,1];
  const matrix=mul(perspective(.4,ratio,.1,100),mul(translate,mul(rotateX,rotateY)));
  const par=framingPixelAspect==='crt'?7/6:1,scale=coverageBasis==='calibrated'?coverageCalibration:1;
  let x0=Infinity,y0=Infinity,x1=-Infinity,y1=-Infinity;
  for(const depth of coveragePlaneDepths())for(const x of [0,1])for(const y of [0,1]) {
    const point=unprojectToPlane(matrix,x,y,depth,1,1);
    // A plane behind the eye cannot supply a finite coverage recommendation.
    if(!point)return null;
    const w=matrix[3]*point[0]+matrix[7]*point[1]+matrix[11]*depth+matrix[15];
    if(w<=.0001)return null;
    const px=point[0]*DATA.frameHeight/par*scale,py=-point[1]*DATA.frameHeight*scale;
    x0=Math.min(x0,px);y0=Math.min(y0,py);x1=Math.max(x1,px);y1=Math.max(y1,py);
  }
  return {x0,y0,x1,y1};
}
function currentCoveragePlan() {
  if(!L)return null;
  const f=roomFraming(room),signature=[keyOf(room,bgIndex),editorRevision,nativeFrame,
    nativeCamera.x,nativeCamera.y,f.x,f.y,framingPixelAspect,framingDistance,
    coverageAspect,coverageBasis,coverageTiltX,coverageTiltY].join(':');
  if(coverageCache?.signature===signature)return coverageCache.plan;
  const footprint=coverageFootprint();if(!footprint)return null;
  const scrollX=bgIndex===0?nativeCamera.x:resolveParallax(nativeCamera.x,room.video[9],L.w,0x100);
  const scrollY=bgIndex===0?nativeCamera.y:resolveParallax(nativeCamera.y,room.video[10],L.h,0xe0);
  const centerX=scrollX+DATA.frameWidth/2+f.x;
  // Small rooms expose the whole arena. Keep its native center as extensions
  // grow/shrink; larger finite rooms anchor at their top/bottom scroll stops.
  const lower=-footprint.y0,upper=L.h-footprint.y1;
  const centerY=(lower<=upper?Math.max(lower,Math.min(upper,scrollY+1+DATA.frameHeight/2)):L.h/2)+f.y;
  const snapDown=v=>Math.floor((v+1e-7)/16),snapUp=v=>Math.ceil((v-1e-7)/16);
  const required={x0:snapDown(centerX+footprint.x0),y0:snapDown(centerY+footprint.y0),
    x1:snapUp(centerX+footprint.x1),y1:snapUp(centerY+footprint.y1)};
  const saved=sceneryBounds(room,bgIndex),workspace=mapBounds(room,bgIndex);
  const missing=coverageShortage(saved,required),spaceMissing=coverageShortage(workspace,required);
  const plan={required,saved,workspace,missing,spaceMissing,
    fits:coverageFits(missing),nextWorkspace:coverageUnion(workspace,required)};
  coverageCache={signature,plan};return plan;
}
function coverageFitBounds() {
  const b=mapBounds(room,bgIndex);
  const plan=showCoverageGuide&&mode==='2d'?currentCoveragePlan():null;
  return plan?coverageUnion(b,plan.required):b;
}
function refreshCoverageControls() {
  coverageCache=null;
  $('#showCoverage').checked=showCoverageGuide;
  $('#coverageQuick').classList.toggle('on',showCoverageGuide);
  $('#coverageQuick').setAttribute('aria-pressed',String(showCoverageGuide));
  $('#coverageAspect').value=coverageAspect;$('#coverageBasis').value=coverageBasis;
  $('#coverageTiltX').value=(coverageTiltX*180/Math.PI).toFixed(2);
  $('#coverageTiltY').value=(coverageTiltY*180/Math.PI).toFixed(2);
  $('#coverageReference').textContent='Tested reference: Bloodpool 2:8, 32×20 tiles / 512×320 px, '
    +'16:10 · Square pixels · distance 3.25. View defaults: '+(window.__ACTION_VIEW__?.source||'editor defaults')+'.';
  const plan=currentCoveragePlan(),summary=$('#coverageSummary');
  $('#coverageFit').disabled=!plan;
  $('#coverageAddSpace').disabled=!plan||coverageFits(plan.spaceMissing)||!coverageEditable(plan.nextWorkspace);
  if(!plan) {
    summary.textContent='This camera pose has no finite footprint on the selected plane.';
    summary.classList.add('short');$('#coverageMissing').textContent='Adjust depth, distance or tilt.';
    $('#coverageWorkspace').textContent='';return;
  }
  const r=plan.required,w=r.x1-r.x0,h=r.y1-r.y0;
  summary.classList.toggle('short',!plan.fits);
  summary.textContent=`BG${bgIndex+1} ${coverageAspect}: needs ${w}×${h} tiles (${w*16}×${h*16} px). `
    +(plan.fits?'Saved extents cover this baseline.':'Saved extents are too small.');
  const m=plan.missing;
  $('#coverageMissing').textContent=`Add columns: left ${m.left}, right ${m.right}. Rows: above ${m.top}, below ${m.bottom}.`;
  $('#coverageWorkspace').textContent=coverageFits(plan.spaceMissing)
    ?(plan.fits?'':'Workspace is large enough; stamp or paint its empty edge cells.')
    :'Add required edge space exposes the missing cells; then stamp or paint them.';
  if(!coverageEditable(plan.nextWorkspace))$('#coverageWorkspace').textContent=
    'Required workspace exceeds the editor’s signed coordinates or 512-tile span. Reduce distance or change framing.';
}
function addCoverageSpace() {
  if(mode!=='2d')setMode('2d');
  const plan=currentCoveragePlan();
  if(!plan||coverageFits(plan.spaceMissing)||!coverageEditable(plan.nextWorkspace))return false;
  const key=keyOf(room,bgIndex);
  beginOp('add coverage edge space');recordBounds(key);
  stampBucket(room,bgIndex).bounds={...plan.nextWorkspace};commitOp();sceneryChanged();fitView();draw();
  tileActionStatus('Coverage workspace added. Stamp or paint the new cells to create saved scenery; Undo removes the space.');
  return true;
}
function coverageUncoveredRects(required,saved) {
  const x0=Math.max(required.x0,saved.x0),x1=Math.min(required.x1,saved.x1);
  const y0=Math.max(required.y0,saved.y0),y1=Math.min(required.y1,saved.y1);
  if(x0>=x1||y0>=y1)return [required];
  return [{x0:required.x0,x1:x0,y0:required.y0,y1:required.y1},
    {x0:x1,x1:required.x1,y0:required.y0,y1:required.y1},
    {x0,x1,y0:required.y0,y1:y0},{x0,x1,y0:y1,y1:required.y1}]
    .filter(r=>r.x1>r.x0&&r.y1>r.y0);
}
function drawCoverageGuide() {
  if(!showCoverageGuide)return;
  const plan=currentCoveragePlan();if(!plan)return;
  const scale=16*view.scale,toCanvas=r=>[view.x+r.x0*scale,view.y+r.y0*scale,
    (r.x1-r.x0)*scale,(r.y1-r.y0)*scale];
  const r=plan.required,m=plan.missing,canvas=cvs.getBoundingClientRect();
  const [x,y,w,h]=toCanvas(r),color=plan.fits?'#76d19c':'#f48c7f';
  ctx.save();ctx.fillStyle='rgba(244,140,127,.16)';
  for(const missing of coverageUncoveredRects(r,plan.saved))ctx.fillRect(...toCanvas(missing));
  ctx.lineWidth=2;ctx.strokeStyle=color;ctx.setLineDash([3,3]);ctx.strokeRect(x,y,w,h);
  ctx.setLineDash([]);ctx.font='12px monospace';ctx.textBaseline='middle';
  const label=(text,lx,ly)=>{
    const width=ctx.measureText(text).width+12;
    lx=Math.max(4,Math.min(canvas.width-width-4,lx));ly=Math.max(4,Math.min(canvas.height-24,ly));
    ctx.fillStyle='#20232b';ctx.fillRect(lx,ly,width,22);
    ctx.fillStyle=color;ctx.fillText(text,lx+6,ly+11);
  };
  label(`${coverageAspect} coverage · ${r.x1-r.x0}×${r.y1-r.y0} tiles`,x,y-25);
  if(m.left)label(`← +${m.left} columns`,x,y+h/2);
  if(m.right)label(`+${m.right} columns →`,x+w-110,y+h/2);
  if(m.top)label(`↑ +${m.top} rows`,x+w/2-45,y);
  if(m.bottom)label(`↓ +${m.bottom} rows`,x+w/2-45,y+h-24);
  ctx.restore();
}
function updateCoverageGuide() {
  refreshFramingControls();if(mode==='2d')fitView();draw();
}
function toggleCoverageGuide(value) {
  if(value&&mode!=='2d')setMode('2d');
  showCoverageGuide=value;updateCoverageGuide();
}
$('#showCoverage').onchange=()=>toggleCoverageGuide($('#showCoverage').checked);
$('#coverageQuick').onclick=()=>toggleCoverageGuide(!showCoverageGuide);
$('#coverageAspect').onchange=()=>{coverageAspect=$('#coverageAspect').value;updateCoverageGuide();};
$('#coverageBasis').onchange=()=>{coverageBasis=$('#coverageBasis').value;updateCoverageGuide();};
for(const axis of ['X','Y'])$(`#coverageTilt${axis}`).onchange=()=>{
  const value=Number($(`#coverageTilt${axis}`).value);
  if(Number.isFinite(value)&&Math.abs(value)<=45) {
    if(axis==='X')coverageTiltX=value*Math.PI/180;else coverageTiltY=value*Math.PI/180;
  }
  updateCoverageGuide();
};
$('#coverageFit').onclick=()=>toggleCoverageGuide(true);
$('#coverageAddSpace').onclick=addCoverageSpace;
