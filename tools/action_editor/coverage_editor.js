/* Coverage is measured by the shared C compositor, including finite-room
 * stops, capture fitting, saved framing and the selected preview pose. The
 * painting workspace remains freely pannable and never defines camera travel. */
let showCoverageGuide=true;
const coverageUnion=(a,b)=>({x0:Math.min(a.x0,b.x0),y0:Math.min(a.y0,b.y0),
  x1:Math.max(a.x1,b.x1),y1:Math.max(a.y1,b.y1)});
const coverageShortage=(have,need)=>({left:Math.max(0,have.x0-need.x0),
  right:Math.max(0,need.x1-have.x1),top:Math.max(0,have.y0-need.y0),bottom:Math.max(0,need.y1-have.y1)});
const coverageFits=missing=>Object.values(missing).every(n=>n===0);
const coverageEditable=b=>b.x0>=-512&&b.y0>=-512&&b.x1<=512&&b.y1<=512&&
  b.x1-b.x0<=512&&b.y1-b.y0<=512;
function coverageAspectLabel() {
  const value=$('#sharedAspect').value;
  return ({'1.6':'16:10',[String(16/9)]:'16:9',[String(4/3)]:'4:3',
    [String(21/9)]:'21:9',[String(32/9)]:'32:9',auto:'Auto',custom:'Custom',stretch:'Stretch'})[value]||value;
}
function currentCoveragePlan() {
  if(!L)return null;
  const measured=SharedRoomPreview.measure?.(bgIndex);if(!measured)return null;
  const r=measured.rect;
  // Float projection noise at a pixel-aligned edge must not request an extra tile.
  const down=v=>Math.floor((v+.001)/16),up=v=>Math.ceil((v-.001)/16);
  const required={x0:down(r.x),y0:down(r.y),x1:up(r.x+r.w),y1:up(r.y+r.h)};
  const saved=sceneryBounds(room,bgIndex),workspace=mapBounds(room,bgIndex);
  const missing=coverageShortage(saved,required),spaceMissing=coverageShortage(workspace,required);
  return {...measured,required,saved,workspace,missing,spaceMissing,
    fits:coverageFits(missing),nextWorkspace:coverageUnion(workspace,required)};
}
function coverageFitBounds() {
  const b=mapBounds(room,bgIndex);
  const plan=showCoverageGuide&&mode==='2d'?currentCoveragePlan():null;
  return plan&&!plan.fill?coverageUnion(b,plan.required):b;
}
function refreshCoverageControls() {
  $('#showCoverage').checked=showCoverageGuide;
  $('#coverageQuick').classList.toggle('on',showCoverageGuide);
  $('#coverageQuick').setAttribute('aria-pressed',String(showCoverageGuide));
  $('#coverageAspect').value=$('#sharedAspect').value;
  const plan=currentCoveragePlan(),summary=$('#coverageSummary');
  $('#coverageFit').disabled=!plan;
  $('#coverageAddSpace').disabled=!plan||!!plan.fill||coverageFits(plan.spaceMissing)||!coverageEditable(plan.nextWorkspace);
  $('#coverageReference').textContent='Shared game camera · '+(window.__ACTION_VIEW__?.source||'defaults')+
    ` baseline ${framingDistance} · capture margins ${$('#sharedExtra').value} px horizontally / ${$('#sharedVertical').value} px vertically.`;
  summary.classList.remove('short');
  if(!plan) {
    summary.textContent='Camera coverage is available when the shared renderer is ready.';
    $('#coverageMissing').textContent='';$('#coverageWorkspace').textContent='';return;
  }
  const travel=`Camera X ${plan.minX}–${plan.maxX}; Y 0–${plan.maxY}.`;
  if(plan.fill) {
    summary.textContent=plan.fill===2?'BG2 uses the skybox: the engine fills the backdrop.':
      `BG${bgIndex+1} uses a background policy instead of finite scenery bounds.`;
    $('#coverageMissing').textContent=plan.fill===2?'No extra tile columns or rows are needed for skybox coverage.':
      'Check the background policy before adding tiles; mirrored, repeated or capped regions may ignore painted extensions.';
    $('#coverageWorkspace').textContent=travel;return;
  }
  const r=plan.required,m=plan.missing;
  summary.textContent=`BG${bgIndex+1} ${coverageAspectLabel()}: ${(r.x1-r.x0)}×${r.y1-r.y0} tiles intersect the view. `+
    (plan.fits?'Existing scenery covers its outer bounds.':'The view exposes finite room borders.');
  $('#coverageMissing').textContent=plan.fits?'No extra edge tiles needed at this camera pose.':
    `To cover those borders (optional): left ${m.left}, right ${m.right} columns; above ${m.top}, below ${m.bottom} rows.`;
  $('#coverageWorkspace').textContent=travel+' '+(plan.fits?'':coverageFits(plan.spaceMissing)
    ?'Workspace is large enough; paint its empty edge cells if you want to extend the scenery.'
    :'Add edge workspace, then paint it. Empty workspace does not change the game camera.');
  if(!coverageEditable(plan.nextWorkspace))$('#coverageWorkspace').textContent+=
    ' This view exceeds the editable workspace limit.';
}
function addCoverageSpace() {
  if(mode!=='2d')setMode('2d');
  const plan=currentCoveragePlan();
  if(!plan||plan.fill||coverageFits(plan.spaceMissing)||!coverageEditable(plan.nextWorkspace))return false;
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
  const plan=currentCoveragePlan();if(!plan||plan.fill)return;
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
  label(`${coverageAspectLabel()} game view · ${r.x1-r.x0}×${r.y1-r.y0} tiles`,x,y-25);
  if(m.left)label(`← +${m.left} columns`,x,y+h/2);
  if(m.right)label(`+${m.right} columns →`,x+w-110,y+h/2);
  if(m.top)label(`↑ +${m.top} rows`,x+w/2-45,y);
  if(m.bottom)label(`↓ +${m.bottom} rows`,x+w/2-45,y+h-24);
  ctx.restore();
}
function updateCoverageGuide() {
  refreshNativeCameraControls();if(mode==='2d')fitView();draw();
}
function toggleCoverageGuide(value) {
  if(value&&mode!=='2d')setMode('2d');
  showCoverageGuide=value;updateCoverageGuide();
}
$('#showCoverage').onchange=()=>toggleCoverageGuide($('#showCoverage').checked);
$('#coverageQuick').onclick=()=>toggleCoverageGuide(!showCoverageGuide);
$('#coverageAspect').onchange=()=>{
  $('#sharedAspect').value=$('#coverageAspect').value;
  $('#sharedCustomSize').hidden=$('#sharedAspect').value!=='custom';
  updateCoverageGuide();
};
$('#coverageFit').onclick=()=>toggleCoverageGuide(true);
$('#coverageAddSpace').onclick=addCoverageSpace;
$('#coveragePreview').onclick=()=>setMode('shared');
