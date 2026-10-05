/* Background policy values are authored here; defaults and semantic validation
 * come from the production C planner through SharedRoomPreview. */
const BG_POLICY_EDGES=['transparent','world','clamp','mirror','repeat','raw'];
const BG_POLICY_EDGE_LABELS=['Transparent','Live world / painted scenery','Clamp','Mirror','Repeat','Raw tilemap wrap'];
const BG_POLICY_MOTIONS=['fill','normal'];
const BG_POLICY_EXTENTS=['inherit','available','fixed'];
const cloneBgPolicy=value=>JSON.parse(JSON.stringify(value));
function emptyBgPolicy(){return {};}
function bgPolicyIniLines(r) {return bgPolicyPairLines(roomConfig(r).policy);}
function bgPolicyPairLines(pair) {
  const lines=[];
  pair.forEach((p,bg)=>{
    const words=[];
    for(const key of ['edge','motion'])if(p[key]!==undefined)words.push(`${key}:${p[key]}`);
    for(const [key,sides] of [['horizontal',['left','right']],['vertical',['top','bottom']]]) {
      const extent=p[key];if(!extent)continue;
      words.push(`${key}:${extent.mode}`);
      if(extent.mode==='fixed')for(const side of sides)words.push(`${side}:${extent[side]}`);
    }
    if(p.bands!==undefined)words.push(`bands:${p.bands.length}`);
    if(words.length)lines.push(`bg${bg+1}-policy = ${words.join(' ')}`);
    p.bands?.forEach((b,index)=>{
      const words=[`index:${index}`,`anchor:${b.anchor}`,`rows:${b.y0},${b.y1}`,
        `edge:${b.edge}`,`motion:${b.motion}`,`horizontal:${b.horizontal.mode}`];
      if(b.horizontal.mode==='fixed')words.push(`left:${b.horizontal.left}`,`right:${b.horizontal.right}`);
      lines.push(`bg${bg+1}-policy-band = ${words.join(' ')}`);
    });
  });
  return lines;
}
function parseBgPolicyDocument(text) {
  const policies={};let key=null,named=false,lineNumber=0;
  const number=(value,max)=>{
    if(!/^\d+$/.test(value??'')||Number(value)>max)throw Error('Invalid policy number.');
    return Number(value);
  };
  const choice=(value,choices)=>{
    if(!choices.includes(value))throw Error('Invalid policy mode.');return value;
  };
  for(const raw of String(text||'').split(/\r?\n/)) {
    lineNumber++;const line=stripIniComment(raw);
    if(line.startsWith('[')) {
      const match=line.match(/^\[layers:([\da-f]{1,2}):([\da-f]{1,2})(:[^\]]+)?\]$/i);
      named=!!match?.[3];
      const target=roomFromSection(line);
      key=target?roomKey(target):match?`${parseInt(match[1],16)}:${parseInt(match[2],16)}`:null;
      if(key&&!DATA.rooms.some(r=>roomKey(r)===key))key=null;
      if(target)named=false;
      continue;
    }
    const match=line.match(/^bg([12])-policy(-band)?(:[^=\s]+)?\s*=\s*(.*)$/);
    if(!match||!key)continue;
    try {
      if(named||match[3])throw Error('Policies belong in the base room section, shared across terrains.');
      const bg=Number(match[1])-1,isBand=!!match[2],v={};
      for(const word of match[4].split(/\s+/)) {
        const m=word.match(/^([^:]+):(.+)$/);
        if(!m||Object.hasOwn(v,m[1]))throw Error('Invalid or duplicate policy field.');
        v[m[1]]=m[2];
      }
      const allowed=isBand?['index','anchor','rows','edge','motion','horizontal','left','right']
        :['edge','motion','horizontal','left','right','vertical','top','bottom','bands'];
      if(Object.keys(v).some(k=>!allowed.includes(k)))throw Error('Unknown policy field.');
      const p=(policies[key]??=[{},{}])[bg],target=isBand?{}:p;
      if(v.edge!==undefined)target.edge=choice(v.edge,BG_POLICY_EDGES);
      if(v.motion!==undefined)target.motion=choice(v.motion,BG_POLICY_MOTIONS);
      for(const [axis,sides] of [['horizontal',['left','right']],['vertical',['top','bottom']]]) {
        if(v[axis]!==undefined) {
          const mode=choice(v[axis],isBand?BG_POLICY_EXTENTS:BG_POLICY_EXTENTS.slice(1));
          target[axis]={mode,left:0,right:0};
          if(axis==='vertical')target[axis]={mode,top:0,bottom:0};
          if(mode==='fixed')for(const side of sides)target[axis][side]=number(v[side],128);
          else if(sides.some(side=>v[side]!==undefined))throw Error('Caps require Fixed mode.');
        } else if(sides.some(side=>v[side]!==undefined))throw Error('Caps require an extent mode.');
      }
      if(isBand) {
        if(!p.bands)throw Error('Declare the replacement band count before its rows.');
        const index=number(v.index,3),rows=(v.rows||'').split(',');
        if(index>=p.bands.length||rows.length!==2)throw Error('Invalid band index or rows.');
        target.anchor=choice(v.anchor,['screen','world']);
        target.y0=number(rows[0],65535);target.y1=number(rows[1],target.anchor==='screen'?224:65535);
        if(target.y0>=target.y1||!target.edge||!target.motion||!target.horizontal)
          throw Error('Band needs its complete policy and increasing rows.');
        if(p.bands[index])throw Error('Duplicate policy band.');
        p.bands[index]=target;
      } else if(v.bands!==undefined)p.bands=new Array(number(v.bands,4));
    } catch(error){throw Error(`Background policy at INI line ${lineNumber}: ${error.message}`);}
  }
  for(const pair of Object.values(policies))for(const p of pair)
    if(p.bands&&Array.from(p.bands).some(b=>!b))throw Error('Background policy is missing a declared row band.');
  return policies;
}

const BackgroundPolicyEditor=(()=>{
  let scope='',saved='',draft={},plan=null,defaults=null,editing=false;
  const drafts=new Map();
  const fields=['Edge','Motion','Horizontal','Left','Right','Vertical','Top','Bottom','Bands'];
  const elements=fields.map(name=>$('#bgPolicy'+name));
  const status=message=>{$('#bgPolicyStatus').textContent=message;};
  function pendingTargets() {
    const keys=new Set(drafts.keys());if(editing)keys.add(scope);
    return [...keys].map(key=>{
      const parts=key.split(':'),bg=Number(parts.pop()),target=DATA.rooms.find(r=>roomKey(r)===parts.join(':'));
      const address=parts.slice(0,2).map(v=>Number(v).toString(16).padStart(2,'0')).join(':');
      const names=['','Fillmore','Bloodpool','Kassandora','Aitos','Marahna','Northwall','Death Heim'];
      return {key,target,bg,label:target
        ? `${names[target.group]||target.group} room ${target.sceneLabel||target.map} (${address}) BG${bg+1}`
        : `${address} BG${bg+1}`};
    });
  }
  function reviewPending() {
    const targets=pendingTargets(),next=targets.find(item=>item.key===scope)||targets[0];
    if(next?.target&&(roomKey(room)!==roomKey(next.target)||bgIndex!==next.bg)) {
      room=terrainRoom(next.target,terrainProfile);$('#room').value=String(DATA.rooms.indexOf(next.target));setLayer(next.bg);
    }
    EditorLayout.openSettings('policy');
  }
  function options(select,values,labels,value) {
    select.replaceChildren();values.forEach((v,i)=>{
      const o=document.createElement('option');o.value=v;o.textContent=labels[i];select.append(o);
    });select.value=value;
  }
  const extentLabel=e=>e.mode==='fixed'?`Fixed (${e.left??e.top} / ${e.right??e.bottom} px)`:'Available';
  function draftFeedback() {
    $('#bgPolicyApply').disabled=$('#bgPolicyDiscard').disabled=!editing;
    const targets=pendingTargets();
    $('#bgPolicyDrafts').hidden=!targets.length;
    $('#bgPolicyDrafts').textContent=targets.length?'Unapplied drafts: '+targets.map(item=>item.label)
      .join(', ')+'. Apply or discard each draft before saving the project.':'';
    refreshEditorFeedback();
  }
  function controls() {
    options($('#bgPolicyEdge'),['default',...BG_POLICY_EDGES],
      [`Room default (${BG_POLICY_EDGE_LABELS[BG_POLICY_EDGES.indexOf(defaults.edge)]})`,...BG_POLICY_EDGE_LABELS],draft.edge??'default');
    options($('#bgPolicyMotion'),['default','fill','normal'],
      [`Room default (${defaults.motion})`,'Follow fill direction','Normal scroll'],draft.motion??'default');
    for(const axis of ['Horizontal','Vertical']) {
      const key=axis.toLowerCase(),sides=axis==='Horizontal'?['Left','Right']:['Top','Bottom'];
      options($('#bgPolicy'+axis),['default','available','fixed'],
        [`Room default (${extentLabel(defaults[key])})`,'Available','Fixed cap'],draft[key]?.mode??'default');
      for(const side of sides) {
        const input=$('#bgPolicy'+side);input.value=String((draft[key]??plan[key])[side.toLowerCase()]);
        input.disabled=draft[key]?.mode!=='fixed';
      }
    }
    options($('#bgPolicyBands'),['default','none','custom'],
      [`Room default (${defaults.bands.length} bands)`,'No row bands','Custom row bands'],
      draft.bands===undefined?'default':draft.bands.length?'custom':'none');
    bandControls();
    draftFeedback();
  }
  function bandControls() {
    const container=$('#bgPolicyBandList');container.replaceChildren();
    const bands=draft.bands??defaults.bands,editable=draft.bands!==undefined;
    bands.forEach((b,index)=>{
      const row=document.createElement('fieldset');row.className='bg-policy-band';
      const title=document.createElement('legend');title.textContent=`Row band ${index+1}`;row.append(title);
      function field(label,key,values,labels,min,max) {
        const wrap=document.createElement('label');wrap.textContent=label;
        const input=document.createElement(values?'select':'input');input.disabled=!editable;
        if(values)options(input,values,labels??values,b[key]);
        else {input.type='number';input.min=min;input.max=max;input.step='1';input.value=String(b[key]);}
        input.onchange=()=>{b[key]=values?input.value:Number(input.value);editing=true;draftFeedback();};
        wrap.append(input);row.append(wrap);return input;
      }
      field('Anchor','anchor',['screen','world'],['Screen rows','World rows']);
      field('First row','y0',null,null,0,65534);field('End row (exclusive)','y1',null,null,1,65535);
      field('Edge fill','edge',BG_POLICY_EDGES,BG_POLICY_EDGE_LABELS);
      field('Motion','motion',BG_POLICY_MOTIONS,['Follow fill','Normal scroll']);
      const extent=document.createElement('label');extent.textContent='Horizontal extent';
      const select=document.createElement('select');select.disabled=!editable;
      options(select,BG_POLICY_EXTENTS,['Inherit layer','Available','Fixed cap'],b.horizontal.mode);
      select.onchange=()=>{b.horizontal={mode:select.value,left:0,right:0};editing=true;controls();};
      extent.append(select);row.append(extent);
      for(const side of ['left','right']) {
        const wrap=document.createElement('label');wrap.textContent=side+' cap';
        const input=document.createElement('input');input.type='number';input.min=0;input.max=128;input.step=1;
        input.value=String(b.horizontal[side]);input.disabled=!editable||b.horizontal.mode!=='fixed';
        input.onchange=()=>{b.horizontal[side]=Number(input.value);editing=true;draftFeedback();};
        wrap.append(input);row.append(wrap);
      }
      const remove=document.createElement('button');remove.textContent='Remove band';remove.disabled=!editable;
      remove.onclick=()=>{draft.bands.splice(index,1);editing=true;controls();};row.append(remove);container.append(row);
    });
    $('#bgPolicyAddBand').disabled=bands.length>=4;
  }
  function readControls() {
    for(const key of ['edge','motion']) {
      const value=$('#bgPolicy'+key[0].toUpperCase()+key.slice(1)).value;
      if(value==='default')delete draft[key];else draft[key]=value;
    }
    for(const [axis,sides] of [['Horizontal',['left','right']],['Vertical',['top','bottom']]]) {
      const mode=$('#bgPolicy'+axis).value,key=axis.toLowerCase();
      if(mode==='default')delete draft[key];
      else {draft[key]={mode};for(const side of sides)draft[key][side]=mode==='fixed'?Number($('#bgPolicy'+side[0].toUpperCase()+side.slice(1)).value):0;}
    }
  }
  function apply(edit=draft) {
    const before=cloneBgPolicy(roomConfig(room).policy[bgIndex]),after=cloneBgPolicy(edit);
    if(JSON.stringify(before)===JSON.stringify(after)){drafts.delete(scope);editing=false;controls();return true;}
    try {
      const cfg=roomConfig(room);let text;
      cfg.policy[bgIndex]=after;
      try {text=roomSectionIni(room);parseBgPolicyDocument(text);}
      finally {cfg.policy[bgIndex]=before;}
      SharedRoomPreview.validateScenery(text);
      commitOp();cfg.policy[bgIndex]=after;
      undoStack.push({label:'background policy',policy:{room:roomKey(room),bg:bgIndex,before,after}});
      redoStack.length=0;drafts.delete(scope);editing=false;saved='';markEditorChanged();SharedRoomPreview.invalidate();
      refreshHistoryButtons();refresh();draw();status('Policy applied. Save project to keep it.');return true;
    } catch(error){status(`Policy not applied: ${error.message}`);return false;}
  }
  function refresh() {
    if(!room)return;
    try {
      const query=SharedRoomPreview.policyPlan();plan=query[bgIndex];
      defaults=SharedRoomPreview.policyPlan(true)[bgIndex];
      const next=roomKey(room)+':'+bgIndex,authored=JSON.stringify(roomConfig(room).policy[bgIndex]);
      if(next!==scope||authored!==saved) {
        if(next!==scope&&editing)drafts.set(scope,{saved,draft:cloneBgPolicy(draft)});
        scope=next;saved=authored;
        const held=drafts.get(scope);
        if(held&&held.saved===saved){draft=cloneBgPolicy(held.draft);editing=true;}
        else {drafts.delete(scope);draft=cloneBgPolicy(roomConfig(room).policy[bgIndex]);editing=false;}
        controls();
        status(editing?'Policy draft · Apply or discard before saving your project.':Object.keys(draft).length?'Applied override for this room · shared across terrain variants.':'Using room defaults · shared across terrain variants.');
      }
      elements.forEach(e=>{if(e.tagName==='SELECT')e.disabled=false;});
      $('#bgPolicyPreview').disabled=false;$('#bgPolicyReset').disabled=false;$('#bgPolicyPainted').disabled=plan.source!==1;
      $('#bgPolicyInfo').textContent=`BG${bgIndex+1} · ${['Unclassified','Playfield','Scene','Backdrop'][plan.role]} · ${['Native tilemap','World map','Authentic viewport'][plan.source]} · ${BG_POLICY_EDGE_LABELS[BG_POLICY_EDGES.indexOf(plan.edge)]}`;
    } catch(error) {
      elements.forEach(e=>{e.disabled=true;});
      for(const name of ['Apply','Discard','Reset','Painted','AddBand','Preview'])$('#bgPolicy'+name).disabled=true;
      status('Policy controls need the shared renderer. '+error.message);
    }
  }
  for(const name of fields)$('#bgPolicy'+name).onchange=()=>{
    readControls();editing=true;
    if(name==='Bands') {
      const mode=$('#bgPolicyBands').value;
      if(mode==='default')delete draft.bands;
      else if(mode==='none')draft.bands=[];
      else draft.bands=cloneBgPolicy(plan.bands);
    }
    controls();status('Policy draft · Apply policy to update the preview.');
  };
  $('#bgPolicyApply').onclick=()=>apply();
  $('#bgPolicyDiscard').onclick=()=>{
    drafts.delete(scope);draft=cloneBgPolicy(roomConfig(room).policy[bgIndex]);editing=false;
    controls();status('Policy draft discarded. Applied policy retained.');
  };
  $('#bgPolicyReset').onclick=()=>apply({});
  $('#bgPolicyPainted').onclick=()=>apply({edge:'world',motion:'normal',horizontal:{mode:'available',left:0,right:0},vertical:{mode:'available',top:0,bottom:0},bands:[]});
  $('#bgPolicyAddBand').onclick=()=>{
    draft.bands??=cloneBgPolicy(defaults.bands);
    if(draft.bands.length>=4)return;
    const last=draft.bands.at(-1),start=last?.y1??0;
    draft.bands.push({y0:start,y1:start+1,anchor:last?.anchor??'screen',edge:plan.edge,motion:plan.motion,
      horizontal:{mode:'inherit',left:0,right:0}});editing=true;controls();
  };
  $('#bgPolicyPreview').onclick=()=>setMode('shared');
  $('#bgPolicyGuides').onchange=()=>draw();
  function restore(op,which) {
    const target=DATA.rooms.find(r=>roomKey(r)===op.room);if(!target)return false;
    roomConfig(target).policy[op.bg]=cloneBgPolicy(op[which]);
    if(roomKey(room)!==op.room||bgIndex!==op.bg) {
      room=terrainRoom(target,terrainProfile);$('#room').value=String(DATA.rooms.indexOf(target));setLayer(op.bg);
    }
    drafts.delete(op.room+':'+op.bg);editing=false;saved='';markEditorChanged();SharedRoomPreview.invalidate();refresh();draw();return true;
  }
  function drawGuides() {
    if(!$('#bgPolicyGuides').checked||!plan||scope!==roomKey(room)+':'+bgIndex)return;
    const s=view.scale,x=plan.cameraX,y=plan.cameraY+1;
    const rect=(rx,ry,w,h)=>[view.x+rx*s,view.y+ry*s,w*s,h*s];
    const h=plan.horizontal,v=plan.vertical,left=h.mode==='fixed'?h.left:128,right=h.mode==='fixed'?h.right:128;
    const top=v.mode==='fixed'?v.top:128,bottom=v.mode==='fixed'?v.bottom:128;
    ctx.save();ctx.font='12px monospace';ctx.lineWidth=1.5;ctx.setLineDash([5,4]);ctx.strokeStyle='#58e7be';
    ctx.strokeRect(...rect(x-left,y-top,256+left+right,224+top+bottom));
    ctx.setLineDash([]);ctx.strokeStyle='#f5f6fa';ctx.strokeRect(...rect(x,y,256,224));
    ctx.fillStyle='#58e7be';ctx.fillText(`BG${bgIndex+1} policy limits · ${plan.edge}`,view.x+(x-left)*s+4,view.y+(y-top)*s-5);
    plan.bands.forEach((b,index)=>{
      const by=b.anchor==='world'?b.y0:y+b.y0,bh=b.y1-b.y0;
      const extent=b.horizontal.mode==='inherit'?h:b.horizontal;
      const l=extent.mode==='fixed'?extent.left:128,r=extent.mode==='fixed'?extent.right:128;
      ctx.strokeStyle='#e4a4ff';ctx.fillStyle='rgba(228,164,255,0.08)';
      ctx.fillRect(...rect(x-l,by,256+l+r,bh));ctx.strokeRect(...rect(x-l,by,256+l+r,bh));
      ctx.fillStyle='#e4a4ff';ctx.fillText(`Band ${index+1} · ${b.anchor} ${b.y0}–${b.y1} · ${b.edge}`,view.x+(x-l)*s+4,view.y+by*s+13);
    });ctx.restore();
  }
  return {refresh,apply,restore,drawGuides,pendingTargets,reviewPending,pending:()=>editing||drafts.size>0,
    clearDrafts:()=>{drafts.clear();editing=false;saved='';scope='';},
    captureDrafts:()=>cloneBgPolicy({scope,saved,draft,editing,drafts:[...drafts]}),
    restoreDrafts:state=>{
      drafts.clear();for(const [key,value] of state.drafts)drafts.set(key,value);
      scope=state.scope;saved=state.saved;draft=state.draft;editing=state.editing;
      if(defaults&&plan)controls();
    }};
})();
