/* Room-wide BG1 material grading. Lives in scenery authoring, so it cannot
 * accidentally recolour actors, HUD, atmospheric particles or light maps. */
const defaultSceneryGrade = {strength:0,saturation:1,shadows:'ffffff',highlights:'ffffff'};
function parseSceneryGrade(values) {
  const grade={strength:Number(values.strength),saturation:Number(values.saturation),
    shadows:String(values.shadows||'').toLowerCase(),highlights:String(values.highlights||'').toLowerCase()};
  if(!Number.isFinite(grade.strength)||grade.strength<0||grade.strength>1||
      !Number.isFinite(grade.saturation)||grade.saturation<0||grade.saturation>2||
      !/^[0-9a-f]{6}$/.test(grade.shadows)||!/^[0-9a-f]{6}$/.test(grade.highlights))
    throw Error('Palette grade needs strength 0–1, saturation 0–2 and two RGB colours.');
  return grade;
}
function sceneryGradeIniLines(r) {
  const g=roomConfig(r).grade;
  return g?[`scenery-grade = strength:${g.strength} saturation:${g.saturation} shadows:${g.shadows} highlights:${g.highlights}`]:[];
}
const PaletteEditor=(()=>{
  const controls={strength:$('#paletteStrength'),saturation:$('#paletteSaturation'),
    shadows:$('#paletteShadows'),highlights:$('#paletteHighlights')};
  let loaded='';
  const status=message=>{$('#paletteStatus').textContent=message;};
  function refresh() {
    if(!room)return;
    const signature=roomKey(room)+JSON.stringify(roomConfig(room).grade);
    if(signature===loaded)return;
    loaded=signature;
    const g=roomConfig(room).grade||defaultSceneryGrade;
    for(const [key,input] of Object.entries(controls))input.value=(input.type==='color'?'#':'')+g[key];
    status('Applies to BG1 scenery in the shared/native 3D renderer with Environmental effects enabled.');
  }
  function apply(reset=false) {
    const before=roomConfig(room).grade;
    try {
      const after=reset?undefined:parseSceneryGrade(Object.fromEntries(Object.entries(controls)
        .map(([key,input])=>[key,input.value.replace(/^#/, '')])));
      if(JSON.stringify(before)===JSON.stringify(after))return;
      let text;roomConfig(room).grade=after;
      try{text=roomSectionIni(room);}finally{roomConfig(room).grade=before;}
      SharedRoomPreview.validateScenery(text);
      commitOp();roomConfig(room).grade=after;
      undoStack.push({label:'scenery palette',palette:{room:roomKey(room),before,after}});
      if(undoStack.length>kMaxUndo)undoStack.shift();
      redoStack.length=0;markEditorChanged();SharedRoomPreview.invalidate();
      refreshHistoryButtons();refresh();draw();status('Palette applied. Save project to keep it.');
    }catch(error){status(`Palette not applied: ${error.message}`);}
  }
  function restore(op,which) {
    const target=DATA.rooms.find(r=>roomKey(r)===op.room);if(!target)return false;
    roomConfig(target).grade=op[which];
    if(roomKey(room)!==op.room) {
      room=terrainRoom(target,terrainProfile);$('#room').value=String(DATA.rooms.indexOf(target));setLayer(bgIndex);
    }
    loaded='';markEditorChanged();SharedRoomPreview.invalidate();refresh();draw();return true;
  }
  for(const [key,input] of Object.entries(controls))input.addEventListener('click',event=>{
    if(!event.shiftKey)return;
    event.preventDefault();input.value=(input.type==='color'?'#':'')+defaultSceneryGrade[key];
  });
  $('#paletteApply').onclick=()=>apply();$('#paletteReset').onclick=()=>apply(true);
  $('#palettePreview').onclick=()=>setMode('shared');
  return {refresh,restore};
})();

/* Shared native-art glow. Environmental effects controls its preview and
 * in-game activation; authored material values remain independently editable. */
const defaultNativeBloom={strength:.85,radius:13,threshold:.24,saturation:1.08,contrast:1.05,light:1.3,receivers:6,
  dust:.3,'dust-scale':1,'dust-speed':1,'dust-blue':'467ccf','dust-rose':'bc438b'};
function parseNativeBloom(values) {
  const result={};
  const limits={strength:[0,3],radius:[1,24],threshold:[0,.95],saturation:[0,2],contrast:[.5,2],light:[0,4],receivers:[0,6],
    dust:[0,1],'dust-scale':[.5,2],'dust-speed':[0,4]};
  for(const [key,[lo,hi]] of Object.entries(limits)) {
    const value=Number(values[key]??(key.startsWith('dust')?defaultNativeBloom[key]:undefined));
    if(!Number.isFinite(value)||value<lo||value>hi||key==='receivers'&&(!Number.isInteger(value)||(value&~6))||
        key==='dust-speed'&&!Number.isInteger(value))
      throw Error(`Invalid native glow ${key} (${lo}–${hi}).`);
    result[key]=value;
  }
  for(const key of ['dust-blue','dust-rose']) {
    const value=String(values[key]??defaultNativeBloom[key]).toLowerCase();
    if(!/^[0-9a-f]{6}$/.test(value))throw Error(`Invalid nebula ${key} colour.`);
    result[key]=value;
  }
  return result;
}
function nativeBloomIniLines(r) {
  const saved=roomConfig(r).nativeBloom,b={...defaultNativeBloom,...saved};
  return saved?[`native-bloom = ${Object.keys(defaultNativeBloom).map(key=>`${key}:${b[key]}`).join(' ')}`]:[];
}
const NativeBloomEditor=(()=>{
  const controls=Object.fromEntries(Object.keys(defaultNativeBloom).filter(k=>k!=='receivers')
    .map(k=>[k,$(`#nebula-${k}`)]));
  let loaded='';
  function refresh() {
    if(!room)return;
    const eligible=room.group===7&&room.map===8&&!room.section;
    for(const input of [...Object.values(controls),$('#nebula-player'),$('#nebula-enemies'),$('#nebula-apply'),$('#nebula-reset')])input.disabled=!eligible;
    if(!eligible)$('#nebula-status').textContent='These controls apply to Death Heim’s final battle, 07:08.';
    const signature=roomKey(room)+JSON.stringify(roomConfig(room).nativeBloom);
    if(signature===loaded)return;loaded=signature;
    const b={...defaultNativeBloom,...roomConfig(room).nativeBloom};
    for(const [key,input] of Object.entries(controls))input.value=(input.type==='color'?'#':'')+b[key];
    $('#nebula-player').checked=!!(b.receivers&2);$('#nebula-enemies').checked=!!(b.receivers&4);
  }
  function apply(reset=false) {
    const before=roomConfig(room).nativeBloom;
    try {
      const after=reset?undefined:parseNativeBloom({...Object.fromEntries(Object.entries(controls).map(([k,v])=>[k,String(v.value).replace(/^#/,'')])),
        receivers:($('#nebula-player').checked?2:0)|($('#nebula-enemies').checked?4:0)});
      let text;roomConfig(room).nativeBloom=after;
      try{text=roomSectionIni(room);}finally{roomConfig(room).nativeBloom=before;}
      SharedRoomPreview.validateScenery(text);
      if(JSON.stringify(before)===JSON.stringify(after))return;
      commitOp();roomConfig(room).nativeBloom=after;
      undoStack.push({label:'native nebula glow',nativeBloom:{room:roomKey(room),before,after}});
      if(undoStack.length>kMaxUndo)undoStack.shift();redoStack.length=0;
      markEditorChanged();SharedRoomPreview.invalidate();refreshHistoryButtons();refresh();draw();
      $('#nebula-status').textContent='Applied. Enable Environmental effects in the shared view to preview. Save project to keep it.';
    }catch(e){$('#nebula-status').textContent=`Not applied: ${e.message}`;}
  }
  function restore(op,which) {
    const target=DATA.rooms.find(r=>roomKey(r)===op.room);if(!target)return false;
    roomConfig(target).nativeBloom=op[which];
    if(roomKey(room)!==op.room){room=terrainRoom(target,terrainProfile);$('#room').value=String(DATA.rooms.indexOf(target));setLayer(bgIndex);}
    loaded='';markEditorChanged();SharedRoomPreview.invalidate();refresh();draw();return true;
  }
  for(const [key,input] of Object.entries(controls))input.addEventListener('click',e=>{
    if(e.shiftKey){e.preventDefault();input.value=(input.type==='color'?'#':'')+defaultNativeBloom[key];}
  });
  for(const id of ['player','enemies'])$('#nebula-'+id).addEventListener('click',e=>{
    if(e.shiftKey){e.preventDefault();queueMicrotask(()=>{$('#nebula-'+id).checked=true;});}
  });
  $('#nebula-apply').onclick=()=>apply();$('#nebula-reset').onclick=()=>apply(true);
  return {refresh,restore};
})();
