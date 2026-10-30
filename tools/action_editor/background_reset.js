/* Reset one background across its regional terrain families. A bulk reset
 * retains only that BG's authored data for undo, never another BG or effects.
 * Camera-local records are otherwise passed through by the editor; remove
 * those too so they cannot silently reapply geometry after a reset. */
const BackgroundReset=(()=>{
  const clone=value=>JSON.parse(JSON.stringify(value));
  function ownsLine(line,bg) {
    if(!ownedIniLine(line))return false;
    const key=regionalKey(stripIniComment(line).split('=')[0].trim())[0];
    return key===`bg${bg+1}`||key===`bg${bg+1}hi`||key.startsWith(`bg${bg+1}-`)||
      bg===0&&(key==='framing'||key==='scenery-grade');
  }
  function scopedLines(target,bg) {
    const result=[];let active=false;
    sourceIniText.split('\n').forEach((line,index)=>{
      const clean=stripIniComment(line);
      if(clean.startsWith('[')) {
        const m=clean.match(/^\[layers:([\da-f]+):([\da-f]+):([^\]]+)\]$/i);
        active=!target.section&&!!m&&m[3].toLowerCase()!=='completion'&&
          parseInt(m[1],16)===target.group&&parseInt(m[2],16)===target.map;
      } else if(active&&ownsLine(line,bg))result.push({index,line});
    });
    return result;
  }
  function refresh() {
    $('#resetBackgroundScope').textContent=`BG${bgIndex+1} in this room, all terrain variants. Clears tile/pixel edits, added edge space, depth and policy overrides${bgIndex===0?', framing and palette grading':''}. Undo restores your edits. Other backgrounds and effects are kept.`;
    $('#resetBackgroundStatus').textContent='';
  }
  function restore(op,which) {
    const target=DATA.rooms.find(r=>roomKey(r)===op.room);if(!target)return false;
    deselect();
    const state=which==='before'?clone(op.before):null,cfg=roomConfig(target);
    for(const key of op.keys) {
      setKey(store,key,state?.layers[key]?.bands);
      setKey(pixelStore,key,state?.layers[key]?.pixels);
      setKey(stampStore,key,state?.layers[key]?.scenery);
    }
    for(const token of [`bg${op.bg+1}`,`bg${op.bg+1}hi`])
      setKey(cfg.planes,token,state?.planes[token]);
    cfg.virtual[op.bg]=state?.virtual??emptyVirtual();
    cfg.policy[op.bg]=state?.policy??emptyBgPolicy();
    if(op.bg===0)setKey(cfg,'grade',state?.grade);
    if(op.scoped.length) {
      // Preserve line positions and CRLF so successive BG resets can undo
      // independently without retaining full copies of the project INI.
      const lines=sourceIniText.split('\n');
      for(const {index,line} of op.scoped)
        lines[index]=which==='before'?line:line.endsWith('\r')?'\r':'';
      sourceIniText=lines.join('\n');
    }
    BackgroundPolicyEditor.restoreDraft(target,op.bg,state?.draft);
    terrainProfile=op.profile;room=terrainRoom(target,op.profile);
    $('#room').value=String(DATA.rooms.indexOf(target));
    markEditorChanged();SharedRoomPreview.invalidate();
    setLayer(op.bg);PaletteEditor.refresh();
    $('#resetBackgroundStatus').textContent=which==='before'
      ?`Restored BG${op.bg+1} edits.`:`BG${op.bg+1} reset to native. Save project to keep it, or Undo to restore your edits.`;
    return true;
  }
  function reset() {
    finishFramingDrag();commitOp();
    const bg=bgIndex,cfg=roomConfig(room),scoped=scopedLines(room,bg);
    const draft=BackgroundPolicyEditor.captureDraft(room,bg);
    if(!draft&&!scoped.length&&!roomIniLines(room).some(line=>ownsLine(line,bg))) {
      $('#resetBackgroundStatus').textContent=`BG${bg+1} already uses native tiles and default settings.`;
      return false;
    }
    const keys=editFamilies(room,bg).map(f=>keyOf(f.room,bg));
    const before=clone({layers:Object.fromEntries(keys.map(key=>[key,
      {bands:store[key],pixels:pixelStore[key],scenery:stampStore[key]}])),
      planes:Object.fromEntries([`bg${bg+1}`,`bg${bg+1}hi`].map(token=>[token,cfg.planes[token]])),
      virtual:cfg.virtual[bg],policy:cfg.policy[bg],grade:bg===0?cfg.grade:undefined,draft});
    const op={room:roomKey(room),bg,profile:terrainProfile,keys,before,scoped};
    restore(op,'after');
    undoStack.push({label:`reset BG${bg+1} to native`,backgroundReset:op});
    if(undoStack.length>kMaxUndo)undoStack.shift();
    redoStack.length=0;refreshHistoryButtons();return true;
  }
  $('#resetBackground').onclick=reset;
  return {refresh,restore};
})();
