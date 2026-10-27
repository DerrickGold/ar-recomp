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
