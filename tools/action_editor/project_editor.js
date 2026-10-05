/* One project download holds all scenery and effect edits in every room and
 * terrain. Its two ordinary INIs can also be extracted directly into the game. */
const ProjectEditor=(()=>{
  let name='action-project.zip',loading=false,downloadUrl=null,downloadBytes=null,activating=false;
  const saveLink=$('#projectSave');
  const status=message=>{$('#projectStatus').textContent=message;};
  function files() {
    return {'diorama-layers.ini':mergeDioramaIni(),'action-effects.ini':EffectEditor.text()};
  }
  function dirty(){return editorHasUnexportedChanges()||EffectEditor.dirty()||BackgroundPolicyEditor.pending();}
  function save(event) {
    // Pointer saves use the anchor's ordinary browser download. Keyboard/API
    // saves activate that same anchor once after preparing the current ZIP.
    if(activating)return downloadBytes;
    if(loading){event?.preventDefault();return;}
    try {
      if(EffectEditor.modalOpen())throw Error('Apply or cancel the effect draft before saving your project.');
      if(drag)throw Error('Finish the current map gesture before saving your project.');
      if(BackgroundPolicyEditor.pending())throw Error('Apply or discard background policy drafts before saving your project.');
      commitOp();SharedRoomPreview.validateEffects(EffectEditor.text());
      const documents=files();SharedRoomPreview.validatePolicyDocument(documents['diorama-layers.ini']);
      const bytes=ActionProjectArchive.encode(documents);
      const url=URL.createObjectURL(new Blob([bytes],{type:'application/zip'}));
      const previous=downloadUrl;downloadUrl=url;downloadBytes=bytes;
      saveLink.href=url;saveLink.download=name;
      // Keep the current URL alive for browsers which consume downloads later.
      // Each save gets a fresh ZIP; superseded downloads have time to finish.
      if(previous)setTimeout(()=>URL.revokeObjectURL(previous),60000);
      if(!event) {
        activating=true;
        try {saveLink.click();} finally {activating=false;}
      }
      EffectEditor.markSaved();captureEditorSavepoint('saved');refreshEditorFeedback();
      $('#iniName').textContent=name;$('#iniName').title='Project contains scenery and effects for all rooms and terrains.';
      status('Project download started: all scenery and effects. Extract both INIs beside settings.ini to use them in the game.');
      return bytes;
    } catch(error){event?.preventDefault();status(`Cannot save project: ${error.message}`);return null;}
  }
  function load(files,filename='action-project.zip') {
    for(const [key,limit] of Object.entries(ActionProjectArchive.limits)) {
      if(typeof files[key]!=='string')throw Error(`Project is missing ${key}.`);
      if(new TextEncoder().encode(files[key]).length>limit)throw Error(`${key} exceeds its project size limit.`);
    }
    if(EffectEditor.modalOpen()||drag)throw Error('Finish the current edit before loading a project.');
    // Validate both documents before accepting either. Layer parsing is staged
    // with its previous buckets retained for rollback if a capacity check fails.
    const previous={text:sourceIniText,name:sourceIniName,policyDrafts:BackgroundPolicyEditor.captureDrafts(),
      stores:[store,pixelStore,stampStore,configRooms].map(target=>({...target})),
      baseline:exportBaseline,kind:savepointKind,revision:editorRevision,
      feedbackRevision,feedbackRooms,changeCache,configDirty,selectionAnchor};
    SharedRoomPreview.validateEffects(files['action-effects.ini']);
    try {
      SharedRoomPreview.validatePolicyDocument(files['diorama-layers.ini']);
      loadIniText(files['diorama-layers.ini'],'diorama-layers.ini');
    }
    catch(error) {
      resetLoadedConfig();
      [store,pixelStore,stampStore,configRooms].forEach((target,index)=>Object.assign(target,previous.stores[index]));
      sourceIniText=previous.text;sourceIniName=previous.name;
      exportBaseline=previous.baseline;savepointKind=previous.kind;editorRevision=previous.revision;
      feedbackRevision=previous.feedbackRevision;feedbackRooms=previous.feedbackRooms;
      changeCache=previous.changeCache;configDirty=previous.configDirty;
      selectionAnchor=previous.selectionAnchor;BackgroundPolicyEditor.restoreDrafts(previous.policyDrafts);
      SharedRoomPreview.invalidate();SharedRoomPreview.validateEffects(EffectEditor.text());
      throw error;
    }
    SharedRoomPreview.invalidate();
    EffectEditor.loadDocument(files['action-effects.ini']);
    undoStack.length=0;redoStack.length=0;pendingOp=null;
    tileClipboard=null;EffectEditor.deactivateClipboard();
    surfacesDirty=compositeDirty=glDirty=true;composite=null;
    invalidateOther();invalidateGameComposite();SharedRoomPreview.invalidate();
    name=filename.replace(/\.zip$/i,'')+'.zip';
    savepointKind='project';$('#iniName').textContent=name;
    $('#iniName').title='Project contains scenery and effects for all rooms and terrains.';
    setLayer(bgIndex);refreshHistoryButtons();
    status('Loaded project: scenery and effects restored together.');
  }
  async function read(selected) {
    if(selected.length===1&&/\.zip$/i.test(selected[0].name)) {
      if(selected[0].size>ActionProjectArchive.maxSize)throw Error('Project ZIP exceeds 20 MiB.');
      return {files:await ActionProjectArchive.decode(new Uint8Array(await selected[0].arrayBuffer())),name:selected[0].name};
    }
    const files={};
    for(const file of selected) {
      const key=file.name.toLowerCase();
      if(!Object.hasOwn(ActionProjectArchive.limits,key)||Object.hasOwn(files,key))
        throw Error('Choose one project ZIP, or select diorama-layers.ini and action-effects.ini together.');
      if(file.size>ActionProjectArchive.limits[key])throw Error(`${key} exceeds its project size limit.`);
      files[key]=await file.text();
    }
    for(const key of Object.keys(ActionProjectArchive.limits))if(!Object.hasOwn(files,key))
      throw Error('Select both diorama-layers.ini and action-effects.ini, or use the individual INI imports.');
    return {files,name:'action-project.zip'};
  }
  saveLink.onclick=save;
  saveLink.addEventListener('keydown',event=>{
    if(event.key!=='Enter'&&event.key!==' ')return;
    event.preventDefault();save();
  });
  $('#projectLoad').onchange=async event=>{
    const selected=Array.from(event.target.files||[]);event.target.value='';
    if(!selected.length||loading)return;
    loading=true;saveLink.disabled=true;saveLink.setAttribute('aria-disabled','true');status('Reading project…');
    try {
      const project=await read(selected);
      if(dirty()&&!confirm('Loading a project replaces unsaved scenery and effects. Continue?')){status('Project load cancelled.');return;}
      load(project.files,project.name);
    }catch(error){status(`Cannot load project: ${error.message}`);}
    finally {loading=false;saveLink.disabled=false;saveLink.setAttribute('aria-disabled','false');}
  };
  window.addEventListener('beforeunload',event=>{
    if(!dirty())return;event.preventDefault();event.returnValue='';
  });
  return {files,save,load,read,dirty};
})();
