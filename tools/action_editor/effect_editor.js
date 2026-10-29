/* Sparse environmental overrides. The shared C parser validates every edit;
 * this module owns only controls, undo and document transport. */
const EffectEditor=(()=>{
  const empty='[effects]\nversion=1\n';
  let documentText=window.__ACTION_EFFECTS__||empty, saved=documentText;
  let selected='', sources=[], sourceKey='', sourceScope='', catalogueSources=[], catalogueScope='', catalogueDocument='', modal=null, records=new Map();
  let exposureDefaults=new Map(),exposureScope="",contourTarget=null;
  const regionDefinitions=new Map();
  let selectedGuideIndex;
  const mapSelection=new Set();let effectClipboard=null,clipboardActive=false;
  const status=message=>{$('#effectStatus').textContent=message;$('#effectDocumentStatus').textContent=message;if(modal)$('#effectInspectorStatus').textContent=message;};
  const mapStatus=message=>{status(message);tileActionStatus(message);};
  function decode(text) {
    const map=new Map();let record=null;
    for(const raw of text.split(/\r?\n/)) {
      const line=raw.split(/[;#]/,1)[0].trim();if(!line)continue;
      if(line.startsWith('[')) {
        const match=line.match(/^\[(source|emitter|member|field):([\da-f]+):([\da-f]+):(\d+):([a-z-]+):([\da-f]+)\]$/i);
        record=null;
        if(match) {
          const id=[match[1],parseInt(match[2],16).toString(16).padStart(2,'0'),
            parseInt(match[3],16).toString(16).padStart(2,'0'),Number(match[4]),match[5],
            parseInt(match[6],16).toString(16).padStart(8,'0')].join(':');
          record={};map.set(id,record);
        }
      }else if(record){const at=line.indexOf('=');record[line.slice(0,at).trim()]=line.slice(at+1).trim();}
    }
    return map;
  }
  function encode(map) {
    return empty+[...map].sort(([a],[b])=>a.localeCompare(b)).map(([id,r])=>
      `\n[${id}]\n`+Object.entries(r).map(([k,v])=>`${k}=${v}\n`).join('')).join('');
  }
  records=decode(documentText);
  // A loaded or placed definition is its reset baseline for this session.
  // Native source/member overrides always reset to inherited game defaults.
  const resetRecords=new Map([...records].filter(([id])=>/^(emitter|field):/.test(id)).map(([id,r])=>[id,{...r}]));
  function restore(text) {
    if(drag?.effects){EmitterMapTools.cancel();drag=null;}
    const previous=records;
    documentText=text;records=decode(text);
    regionDefinitions.clear();
    for(const [id,r] of records)if(/^(emitter|field):/.test(id)&&!resetRecords.has(id))resetRecords.set(id,{...r});
    const added=[...records.keys()].find(id=>!previous.has(id));
    if(added)selected=added;
    rebuildSources(sources.filter(s=>!s.emitter));
    refreshInspector();
    SharedRoomPreview.invalidate();draw();
    status(text===saved?'Effects match the loaded/saved document.':'Unsaved effect changes. Save project to keep scenery and effects together.');
    refreshEditorFeedback();
  }
  function markSaved() {saved=documentText;status('Effects match the loaded/saved document.');refreshEditorFeedback();}
  function loadDocument(text) {
    exposureDefaults.clear();exposureScope='';
    resetRecords.clear();mapSelection.clear();effectClipboard=null;clipboardActive=false;
    selected='';sources=[];sourceKey='';sourceScope='';catalogueSources=[];catalogueScope='';catalogueDocument='';
    saved=text;restore(text);
  }
  function change(text,label) {
    try {
      // Editing a native dimming guide extracts just its exposure into a box.
      // The complete parent keeps every other atmosphere setting unchanged.
      const next=decode(text);let extracted=false;
      for(const source of sources.filter(s=>s.nativeExposure&&next.has(s.id))) {
        next.set(source.id,{...source.record,...next.get(source.id)});
        next.set(source.owner,{...source.definition,dimming:'0'});extracted=true;
      }
      if(extracted)text=encode(next);
      SharedRoomPreview.validateEffects(text);
      if(text===documentText)return true;
      if(modal){restore(text);$('#effectInspectorStatus').textContent='Draft only. Apply to keep these changes; Cancel restores the previous effect.';return true;}
      if(pendingOp)commitOp();
      undoStack.push({label,effects:{before:documentText,after:text}});
      if(undoStack.length>kMaxUndo)undoStack.shift();redoStack.length=0;
      restore(text);refreshHistoryButtons();return true;
    }catch(error){status(error.message);if(!modal)tileActionStatus(error.message);refreshInspector();return false;}
  }
  const moteFields=[['emitterSizeMin','size-min','0.35'],['emitterSizeMax','size-max','0.75'],
    ['emitterTravelX','travel-x','0'],['emitterTravelY','travel-y',''],['emitterWander','wander','2'],
    ['emitterSpread','spread','1'],['emitterSeed','seed','']];
  const particleNames=['fireflies','particle-path','flame','motes','particle-area','water-surface','drips','waterfall-spray','cloud-bank','torch'];
  const lightDefaults=window.__ACTION_LIGHT_RESPONSE__;
  const lightNames=Object.keys(lightDefaults);
  // The distant castle sky is atmospheric scattering behind masonry. The
  // window light owns illumination entering the room; promoting the sky to
  // object/material maps would bypass the wall and recreate light leaks.
  const receiverNames=lightNames.filter(name=>name!=='castle-sky');
  const sourceDefault=(name,key,member=false)=>member?1:(lightDefaults[name]?.[key]??1);
  const dimNames=['exposure','cave-light','castle-light'];
  const fieldNames=['cloud-light','storm-lightning','fireflies','canopy-light','foliage-shadow','particle-path','heat-haze','torch','halo','light-gradient','particle-area','light-fan','water-surface','drips','waterfall-spray','cloud-bank','wet-contour'];
  const terrainNames=['ground-mist','wet-contour','exposure','canopy-light'];
  const requiresTerrain=name=>name.startsWith('stage:')||terrainNames.includes(name)||name.endsWith('-field')&&name!=='moon-field';
  const fieldFields=[['emitterGlowStyle','glow-style','insects'],['emitterChorus','chorus','0'],['emitterTrail','trail','0'],['emitterRipple','ripple','0'],['emitterPlacement','placement','foreground'],['emitterPattern','pattern','motes'],
    ['emitterFoliageSeed','seed',''],['emitterRayThickness','thickness','1'],['emitterRayFadeIn','fade-in','0'],['emitterAngle','angle','0'],['emitterFan','fan','36'],['emitterStrands','strands','8'],
    ['emitterSoftness','softness','0.35'],['emitterDrift','drift','0'],['emitterAmplitude','amplitude','2'],['emitterCarry','carry','0'],['emitterGust','gust','0'],['emitterTwinkle','twinkle','0'],['emitterActivity','activity','1'],['emitterShadows','shadows','0'],['emitterScatter','scatter','.12'],['emitterBounce','bounce','.4'],['emitterTextured','textured','0'],['emitterRolling','rolling','0'],['emitterVolume','volume','0'],['emitterRainbow','rainbow','0'],['emitterSparkle','sparkle','0'],['emitterWake','wake','0'],['emitterLightResponse','canopy-response','0']];
  const cloudFields=[['edge-strength',9],['halo-strength',1.3],['streak-strength',1.7],
    ['echo-strength',.55],['falloff',.42],['cloud-cover',.55],['edge-position',.15],['echo-position',.8],['sun-depth',48],['background-brightness',1]];
  const stormFields=[['parallax-x',''],['ground-palette','0'],['ground-colors','none']];
  const cloudDefaults={anchor:'bg2-point',placement:'background',width:'272',height:'184',
    particles:'0',lifetime:'1440',angle:'-32',softness:'.5',strands:'3',
    color:'ffd18f','color-end':'ed8a3d',...Object.fromEntries(cloudFields.map(([k,v])=>[k,String(v)]))};
  function supportsFieldControl(name,record,key) {
    const area=name==='particle-area',cloud=name==='cloud-bank';
    const freeTexturedCloud=cloud&&!record.points&&record.textured==='1';
    const terrainCloud=freeTexturedCloud&&(!record.anchor||record.anchor==='bg1');
    switch(key) {
      case 'glow-style': return name==='fireflies';
      case 'chorus': return name==='fireflies'&&(record['glow-style']??'insects')==='insects';
      case 'trail': return name==='fireflies'&&record['glow-style']==='wisps';
      case 'ripple': return name==='drips';
      case 'placement': return !['ground-mist','exposure','heat-haze','cloud-light'].includes(name);
      case 'pattern': return area||name==='particle-path';
      case 'seed': return ['storm-lightning','cloud-light','foliage-shadow','canopy-light'].includes(name)||freeTexturedCloud;
      case 'thickness': case 'fan': case 'fade-in': return name==='light-fan';
      case 'angle': return ['light-fan','cloud-light','light-gradient','foliage-shadow','canopy-light'].includes(name);
      case 'strands': return ['storm-lightning','cloud-light','light-fan','cloud-bank','foliage-shadow'].includes(name);
      case 'softness': return ['storm-lightning','cloud-light','light-fan','cloud-bank','halo','light-gradient','foliage-shadow','canopy-light'].includes(name)&&!(cloud&&record.textured==='1');
      case 'twinkle': return name==='light-fan'||area||name==='particle-path'||cloud&&!!record.points;
      case 'gust': return cloud||name==='particle-path';
      case 'activity': return ['storm-lightning','particle-path','drips'].includes(name);
      case 'scatter': case 'bounce': return name==='canopy-light';
      case 'canopy-response': return area||['particle-path','drips'].includes(name)||terrainCloud;
      case 'rolling': case 'volume': case 'rainbow': case 'sparkle': return freeTexturedCloud;
      case 'wake': return terrainCloud;
      case 'textured': return (cloud||name==='foliage-shadow'||area&&record.pattern==='leaves')&&!record.points;
      case 'shadows': return ['torch','light-fan','canopy-light'].includes(name)||name==='foliage-shadow'&&record.textured==='1';
      case 'carry': return cloud&&!!record.points;
      case 'drift': return ['water-surface','cloud-bank'].includes(name);
      case 'amplitude': return ['storm-lightning','water-surface','cloud-bank','heat-haze','foliage-shadow','canopy-light'].includes(name);
      default: return true;
    }
  }
  const exposureFields=[['exposureBrightness','brightness'],...['left','top','right','bottom'].map(edge=>['exposureFade'+edge,'fade-'+edge])];
  const nativeFields=[['nativeOffsetX','offset-x',0],['nativeOffsetY','offset-y',0],
    ['nativeWidth','width-scale',1],['nativeLength','length-scale',1],['nativeAngle','angle',0]];
  function nativeCapabilities(source) {
    const name=source?.name;
    return {width:!!source?.movable&&!['cave-drips','wall-torch'].includes(name),
      length:!!source?.movable&&!['cave-water','wall-torch'].includes(name),angle:!!source?.angled};
  }
  const surfaceNames={'lava-pit':['lava-pit-field',0],'lava-lake':['lava-lake-field',1],'water-splash':['splash-field',2],'waterfall':['waterfall-field',3],'waterfall-mist':['waterfall-mist-field',4]};
  const projectileNames={'enemy-fireball':['fireball-field',0],'statue-orb':['orb-field',1],'jungle-fireball':['jungle-fire-field',2],'lava-fireball':['lava-fire-field',3]};
  const arcNames={'lightning-trap':['trap-field',0],'boss-lightning':['bolt-field',1],'centaur-lightning':['centaur-field',2]};
  const atmosphereNames=['temple-dust','temple-grit','floor-mist','cave-light','tower-window'];
  const castleNames=['castle-light','castle-sky','castle-mist','castle-water'];
  const marshNames=['blood-water','blood-mist','wet-timber','marsh-air'];
  const moonNames=['moonlight','moon-reflection','moon-cloud'];
  const waterNames=['cave-water','cave-drips','cave-mist','cave-sheen'];
  function inheritedReceiverMask(source,mode) {
    const name=source?.name,kindDefault=mode==='Light'?(lightDefaults[name]?.receivers??1):1;
    const owner=castleNames.includes(name)?'castle-field':atmosphereNames.includes(name)?'atmosphere-field':
      surfaceNames[name]?.[0]||projectileNames[name]?.[0]||arcNames[name]?.[0]||
      (['torch','wall-torch'].includes(name)?'glow-field':null);
    const definition=owner&&[...records].find(([id])=>id.startsWith(fieldPrefix()+owner+':'))?.[1];
    const slot=['castle-field','atmosphere-field'].includes(owner)?mode==='Light'?1:0:0;
    const fieldMask=Number(definition?.receivers?.split(/\s+/)[slot]??8);
    let mask=fieldMask<8?fieldMask:kindDefault;
    if(source?.member) {
      const parent=records.get(source.parent)||{},prefix=mode.toLowerCase();
      if(['scenery','player','enemies'].some(target=>parent[`${prefix}-${target}`]!==undefined))mask=kindDefault;
      for(const [target,bit] of [['scenery',1],['player',2],['enemies',4]])
        if(parent[`${prefix}-${target}`]!==undefined)mask=parent[`${prefix}-${target}`]==='1'?mask|bit:mask&~bit;
    }
    return mask;
  }
  function refreshInspector() {
    const source=sources.find(s=>s.id===selected);
    const record={...(source?.name==='cloud-light'?cloudDefaults:{}),...(records.get(selected)||source?.record||{})};
    const definition=!!source?.field;
    $('#effectDefinitionCreate').hidden=!!source?.member||![...Object.keys(surfaceNames),...Object.keys(projectileNames),...Object.keys(arcNames),'torch','wall-torch',...castleNames,...marshNames,...moonNames,...atmosphereNames,...waterNames,'forest-canopy','forest-forward','forest-leaves'].includes(source?.name);
    $('#effectDefinitionCreate').textContent=surfaceNames[source?.name]?'Edit complete lava / waterfall definition…':projectileNames[source?.name]?'Edit complete fireball response…':arcNames[source?.name]?'Edit complete lightning response…':['torch','wall-torch'].includes(source?.name)?'Edit complete flame and glow definition…':castleNames.includes(source?.name)?'Edit complete castle-field definition…':marshNames.includes(source?.name)?'Edit complete marsh-field definition…':moonNames.includes(source?.name)?'Edit complete moon-field definition…':atmosphereNames.includes(source?.name)?'Edit complete atmosphere definition…':waterNames.includes(source?.name)?'Edit complete water-field definition…':'Edit complete ray-field definition…';
    $('#effectDefinitionControls').hidden=!definition;
    if(definition)refreshDefinition(record);
    const disabled=!source;
    for(const id of ['effectEnabled','effectIntensity','effectColor','effectReset'])$('#'+id).disabled=disabled;
    $('#effectReach').disabled=disabled||!source.reach;
    $('#emitterControls').hidden=!source?.emitter;
    $('#nativeShapeControls').hidden=!source?.movable;
    const capability=nativeCapabilities(source);
    for(const [id,key,fallback] of nativeFields) {
      $('#'+id).value=record[key]??fallback;
      $('#'+id).disabled=!source?.movable||key==='width-scale'&&!capability.width||
        key==='length-scale'&&!capability.length||key==='angle'&&!capability.angle;
    }
    $('#effectReset').textContent=source?.emitter?'Remove emitter':'Restore source defaults';
    const name=source?.name,area=name==='particle-area',large=area||['exposure','cloud-light','light-gradient','heat-haze','cloud-bank','foliage-shadow','canopy-light'].includes(name);
    for(const key of ['x','y','width','height','particles','lifetime','mist-height'])
      $('#emitter'+key).value=record[key]??((name==='storm-lightning'?{width:192,height:112,particles:0,lifetime:720}[key]:undefined)??({width:['foliage-shadow','canopy-light'].includes(name)?512:96,height:name==='foliage-shadow'?384:name==='canopy-light'?256:64,particles:['cloud-bank','torch'].includes(name)?0:24,lifetime:name==='canopy-light'?1440:name==='foliage-shadow'?960:240,'mist-height':26}[key]??0));
    const attached=!!record['actor-target'];
    $('#actorBindingControls').hidden=!source?.emitter||['ground-mist','exposure','wet-contour','water-surface','particle-area','heat-haze','cloud-light','canopy-light'].includes(name);
    $('#actorBindingTarget').value=record['actor-target']??'';
    const family=$('#actorBindingFamily');family.replaceChildren();
    const custom=document.createElement('option');custom.value='';custom.textContent='Custom family ID';family.append(custom);
    for(const item of (window.__ACTION_ACTORS__||[]).filter(a=>a.group===room.group)) {
      const option=document.createElement('option');option.value=item.source;option.textContent=`${item.label} · ${item.source}`;family.append(option);
    }
    family.value=record['actor-source']??'';
    for(const [id,key] of actorFields)$('#actorBinding'+id).value=record[key]??(key==='actor-limit'?'4':'');
    $('#actorBindingBossFire').hidden=room.group!==2||room.map!==1;
    $('#actorBindingPreview').disabled=!attached;
    const anchor=record.anchor??(name==='cloud-light'?'bg2-point':'bg1');
    $('#emitterAnchor').value=anchor;
    $('#emitterAnchor').disabled=attached||terrainNames.includes(name)||name==='cloud-light';
    $('#emitterAnchor option[value="bg2-raster"]').disabled=name==='heat-haze';
    $('#emitterAnchorHint').textContent=attached?'X/Y are offsets from each matching actor. The map marker and attachment preview use the reference actor position.':terrainNames.includes(name)?'This effect uses BG1 terrain coordinates.':
      anchor==='bg2-point'?'X/Y is a fixed point in the BG2 map. The whole effect follows that point as BG2 scrolls, without bending at water raster bands. Draw placement controls which objects cover it.':
      anchor==='bg2-raster'?'X/Y uses BG2 pixels. Each row follows BG2 raster motion; use this for effects attached to moving water.':
      'X/Y uses BG1 playfield pixels. Change the anchor to attach an effect to BG2 instead; the saved coordinates are not converted.';
    $('#emitterLocate').textContent=attached?'Apply and show at reference actor':`Apply and show anchor on ${anchor==='bg1'?'BG1':'BG2'} map`;
    document.querySelector('label[for="emitterx"]').textContent=attached?'Actor offset X':`${anchor==='bg1'?'BG1':'BG2'} anchor X`;
    document.querySelector('label[for="emittery"]').textContent=attached?'Actor offset Y':`${anchor==='bg1'?'BG1':'BG2'} anchor Y`;
    $('#effectColor').disabled=disabled||definition||['exposure','heat-haze'].includes(name);
    $('#effectIntensity').disabled=disabled||definition||name==='exposure';
    $('#cloudLightControls').hidden=name!=='cloud-light';
    for(const [key,value] of cloudFields)$('#cloudLight-'+key).value=record[key]??value;
    $('#stormControls').hidden=name!=='storm-lightning';
    for(const [key,value] of stormFields)$('#storm-'+key).value=record[key]??value;
    $('#exposureControls').hidden=name!=='exposure';
    for(const [id,key] of exposureFields)$('#'+id).value=key==='brightness'?Math.round(100*Number(record.brightness??(1-Number(record.intensity??.35)))):record[key]??0;
    $('#emitterlifetime').disabled=['exposure','soft-light','halo','light-gradient'].includes(name);
    $('#receiverControls').hidden=!receiverNames.includes(name)&&!dimNames.includes(name);
    $('#effectFamilyOpen').hidden=!source?.member;
    for(const [mode,names] of [['Light',receiverNames],['Dim',dimNames]]) {
      const available=names.includes(name)&&!(source?.member&&mode==='Dim'),prefix=mode.toLowerCase();
      $('#effect'+mode+'Group').hidden=!available;
      const active=['scenery','player','enemies'].some(target=>record[`${prefix}-${target}`]!==undefined);
      $('#effect'+mode+'Targets').checked=active||name==='exposure'&&mode==='Dim';
      $('#effect'+mode+'Targets').disabled=!available||name==='exposure'&&mode==='Dim';
      for(const target of ['Scenery','Player','Enemies']) {
        const element=$('#effect'+mode+target);element.disabled=!available||!$('#effect'+mode+'Targets').checked;
        const key=`${prefix}-${target.toLowerCase()}`;
        element.checked=record[key]!==undefined?record[key]==='1':
          !!((active?(mode==='Light'?(lightDefaults[name]?.receivers??1):1):inheritedReceiverMask(source,mode))&{Scenery:1,Player:2,Enemies:4}[target]);
      }
    }
    $('#effectIntensity').max=name==='exposure'?1:lightNames.includes(name)?16:4;
    $('#effectIntensity').title=lightNames.includes(name)?
      'Overall effect strength; 0 hides the effect. Torches default to 4; other light families default to 1. Above 4, brightness increases without flattening the light falloff. Maximum 16.':
      'Brightness multiplier: 1 keeps the default brightness; 0 hides the effect. Maximum 4.';
    $('#contourDraw').disabled=attached;
    $('#contourControls').hidden=!['wet-contour','cloud-bank','particle-path'].includes(name);
    $('#emitterPoints').value=record.points??(name==='wet-contour'?'-32,0 32,0':name==='particle-path'?'0,-24 0,24':'');
    $('#contourDraw').textContent=name==='cloud-bank'?'Trace ground path on map':name==='particle-path'?'Trace particle route on map':'Trace wet edge on map';
    $('#emitterparticles').disabled=!particleNames.includes(name)||(name==='cloud-bank'&&!record.points);
    $('#emitterparticles').min=['cloud-bank','torch'].includes(name)?0:1;
    $('#emitterparticles').max=area?16:name==='fireflies'?32:128;
    document.querySelector('label[for="emitterparticles"]').textContent=area?'Density / 256px cell':name==='torch'?'Motes in light pool':name==='cloud-bank'?'Lifted dust grains':'Particle count';
    for(const key of ['width','height']){$('#emitter'+key).max=large?16384:512;$('#emitter'+key).disabled=name==='torch';}
    $('#fieldControls').hidden=!fieldNames.includes(name);
    for(const [id,key,fallback] of fieldFields) {
      const element=$('#'+id);element.value=record[key]??(name==='storm-lightning'&&['activity','strands','amplitude','placement'].includes(key)?({activity:'.05',strands:'3',amplitude:'10',placement:'background'})[key]:key==='shadows'&&['light-fan','canopy-light'].includes(name)?'1':key==='placement'&&name==='canopy-light'?'playfield':key==='placement'&&name==='foliage-shadow'?'background':key==='strands'&&name==='foliage-shadow'?'4':key==='softness'&&name==='canopy-light'?'.65':key==='softness'&&name==='foliage-shadow'?'.7':fallback);
      element.disabled=!supportsFieldControl(name,record,key);
      if(['glow-style','chorus','trail','ripple','fade-in'].includes(key))element.closest('.ctl').hidden=element.disabled;
    }
    $('#emitterAngle').min=['canopy-light','cloud-light'].includes(name)?-70:-180;$('#emitterAngle').max=['canopy-light','cloud-light'].includes(name)?70:180;
    document.querySelector('label[for="emitterFoliageSeed"]').textContent=name==='cloud-light'?'Cloud-opening seed':name==='storm-lightning'?'Storm pattern seed':name==='cloud-bank'?'Cloud pattern seed':'Canopy pattern seed';
    $('#emitterStrands').max=name==='storm-lightning'?4:name==='cloud-light'?5:name==='foliage-shadow'?8:['cloud-bank','canopy-light'].includes(name)?24:32;
    document.querySelector('label[for="emitterStrands"]').textContent=name==='cloud-light'?'Cloud openings':name==='storm-lightning'?'Bolt paths (main + forks)':name==='foliage-shadow'?'Vine clusters':name==='canopy-light'?'Light patches':'Rays / cloud lobes';
    document.querySelector('label[for="emitterSoftness"]').textContent=name==='storm-lightning'?'Distant bolt softness':name==='cloud-light'?'Edge / halo softness':name==='canopy-light'?'Light / shadow softness':name==='foliage-shadow'?'Leaf edge softness':'Soft backdrop';
    document.querySelector('label[for="emitterTwinkle"]').textContent=name==='light-fan'?'Cloud-break modulation':'Grain glint strength';
    $('#emitterTwinkle').title=name==='light-fan'?'Slowly vary ray brightness as cloud openings change. Zero keeps the fan steady; cycle length controls the pace. Geometry stays anchored.':'Brief highlights on moving grains. Zero disables glints; positions and particle count stay unchanged.';
    document.querySelector('label[for="emitterAmplitude"]').textContent=name==='storm-lightning'?'Channel bends (pixels)':['foliage-shadow','canopy-light'].includes(name)?'Canopy sway (pixels)':name==='cloud-bank'?'Vertical billow':name==='heat-haze'?'Heat displacement (pixels)':'Wave height';
    document.querySelector('label[for="emitterShadows"]').textContent=['canopy-light','foliage-shadow'].includes(name)?'Solid terrain shadows':name==='light-fan'?'Scenery occlusion':'Moving torch shadows';
    document.querySelector('label[for="emitterTextured"]').textContent=name==='foliage-shadow'?'Textured vine silhouettes':area?'Feathered leaf silhouettes':'Textured wisps';
    $('#emitterActivity').min=name==='storm-lightning'?.01:.05;$('#emitterActivity').max=name==='storm-lightning'?.2:1;
    $('#emitterActivity').step=name==='storm-lightning'?.01:.05;
    $('#emitterlifetime').min=name==='storm-lightning'?120:16;
    document.querySelector('label[for="emitterlifetime"]').textContent=name==='storm-lightning'?'Mean strike interval (frames)':'Cycle frames';
    $('#fieldHint').textContent=name==='storm-lightning'?'Distant lightning with crooked channels, thinner forks and sustained return strokes. Bolts hold bright, linger between discharges, then retrace the same channel and light new forks; some strikes have a third pulse. Storm pattern seed varies shapes and timing. Bolt paths controls the main channel plus forks; Channel bends sets their irregularity. Cycle frames sets mean spacing; Active fraction stretches the whole sequence, including bright holds, the gaps between discharges and their afterglow. Width and height bound the strike region. Place the dashed Cloud attachment guide inside the cloud artwork; the bolt starts and its halo glows there. Background placement lets the terrain cover the bolt. BG2 fixed-point anchoring keeps bolts straight across raster bands; BG1 anchors weather in the level. Receiver flags optionally illuminate scenery or actors.':name==='fireflies'?'A bounded glow cluster. Insects wander with independent pulses; Chorus adds occasional staggered replies. Wisps drift with soft tails controlled by Trail. Moss makes fixed irregular patches and ignores movement controls. Particle count covers the whole region. Seed changes the layout; size sets the visible core or patch. Light receivers and illumination control nearby scenery/player/enemy glow separately. Position moss on roots or damp masonry, not in empty air.':name==='canopy-light'?'Broken canopy sunlight with straight transport. Width defines the top opening, height the vertical reach, and angle leans the light. Solid terrain shadows stop light at collision surfaces; surface spill adds a short warm return there. Air scattering controls visible haze independently of material illumination. Leaf gaps retain square world-space proportions as angle and height change. Softness blends field edges and terrain contacts; overlap the soft margins of neighbouring fields. Seed varies shadow clusters, orientation, size and sway phase; cycle and sway control their slow movement. Vary width, spacing and intensity between fields for sheltered areas and open glades. Receiver flags select scenery, player and enemies.':name==='foliage-shadow'?'Soft leafy silhouettes with hanging vines. Width/height cover the shadow area; clusters set density, intensity sets opacity, and softness feathers the leaves. Angle rotates the whole arrangement. Sway and cycle animate it around fixed roots. Seed changes the layout. Textured silhouettes use a shared atlas. With solid terrain shadows enabled on BG1, the stencil stops above collision surfaces. Start its area in open canopy and keep it out of enclosed rooms; decorative non-solid walls are not collision blockers. Background placement draws behind BG1, while playfield placement shows over the backdrop.':name==='heat-haze'?'Scenery heat distortion. BG1 warps playfield scenery around lava; BG2 warps the distant background, including skybox-only mode. Actors and HUD stay sharp. Width/height bound the heated air with feathered edges; amplitude sets displacement and lifetime the cycle. Up to four regions per room. Shared diorama renderer only.':name==='particle-path'?'Ordered local X,Y points describe the particle route. Trace from source to destination; a smaller activity fraction leaves quiet gaps. Glints stay fixed on the route. Canopy response makes wet glints visible only in canopy light, respecting its terrain shadows. Gust links sand to the room wind.':area?'One region covers the room. Density is particles per 256px world cell; only nearby cells are generated. Seeds stay fixed as you scroll. Light response makes BG1 particles catch canopy light and direct sky sunlight; BG2 particles catch sky sunlight. Use it for spores and glints. Water glints follow Travel X/Y; surface glints remain stationary.':
      name==='halo'?'Width and height define the outer ring. Softness widens its corona; the centre remains clear. End tint colors the outer edge.':
      name==='light-gradient'?'A room-sized color wash. 0° blends from top to bottom, 90° from left to right. Softness fades the outer edges. Choose BG2 for distant sky, or scenery/player/enemy light receivers.':
      name==='light-fan'?'X/Y is the opening centre. Width spreads origins across the sill, arch or cloud opening; height is ray length. 0° points down. Cloud-break modulation slowly varies brightness over Cycle frames without moving the rays. Zero keeps existing fans steady. Rays are clipped, never bent at screen edges. Scenery occlusion blocks light behind opaque BG1 pixels; turn it off when distant backdrop artwork is baked into that layer.':
      name==='water-surface'?'X/Y is the area centre; its top edge is the water surface. Ripples and glints share the wave clock. This adds surface geometry, not refraction.':
      name==='drips'?'Drops fall from the top edge toward the bottom edge. Place the top on a wet ceiling or vine tip. Reduce Active fraction for occasional dew. Water landing adds a tiny ripple at the bottom edge: enable only over exposed water, with no platform between source and landing. Canopy response lets drops catch sunlight. Drag the region corners to adjust source width and fall height.':
      name==='waterfall-spray'?'Spray rises from the bottom edge. Anchor this edge to the native waterfall splash.':
      name==='cloud-bank'?'For free clouds, enable Textured wisps. Rolling mist adds dense billows and thin curls. Cloud volume replaces the flat wisps with stable rounded billows: higher values deepen their shaded undersides, while drift and billow move whole clouds without stretching. Spray rainbow response reveals spectral fragments only where droplets receive direct Cloud-break sunlight at the correct viewing angle. The light’s beam angle and Sun direction in depth control the shared spectrum; resizing this bank changes spray coverage without stretching a bow. Strength controls scattering, with actual density still limiting visibility. Spray sparkle response adds bright droplets only in sunlit moving mist density, with no sparkle in empty air. The main color tints crests and the end tint colors their shadowed body; billow, drift and cycle control their waves. Body wake stirs the fog near moving player/enemy feet and settles over 1.5 seconds. Canopy response catches the current canopy-light color and terrain shadows. Body wakes and canopy response require BG1; a rolling cloud bank without them also supports BG2 anchoring. Drag the shared renderer stand-ins while animation plays to preview wakes. Trace a BG1 ground path. Carry is the downwind settling distance: 0 hugs slopes; larger values carry dust over crests. Drift sets wind direction/travel, billow sets lift, and lifetime sets the gust cycle. Optional grains lift from this path; size, wander, tint and seed vary them. Clearing points also clears carry and grains for a free cloud.':'';
    $('#emittermist-height').disabled=source?.name!=='ground-mist';
    $('#mistHint').hidden=!['ground-mist','free-mist'].includes(name);
    $('#moteControls').hidden=!particleNames.includes(name)||(name==='cloud-bank'&&!record.points);
    $('#effectTintControls').hidden=(!particleNames.includes(name)&&!['halo','cloud-light','light-gradient'].includes(name))||(name==='cloud-bank'&&!record.points&&!(Number(record.rolling)>0));
    for(const [id,key,fallback] of moteFields) {
      $('#'+id).value=record[key]??fallback;
      $('#'+id).disabled=(name==='fireflies'&&record['glow-style']==='moss'&&['travel-x','travel-y','wander'].includes(key))||(['water-surface','drips','waterfall-spray'].includes(name)&&['travel-x','travel-y','wander','spread'].includes(key))||(['cloud-bank','particle-path'].includes(name)&&['travel-x','travel-y','spread'].includes(key));
    }
    $('#emitterAutoRise').disabled=!['flame','motes','particle-area','torch'].includes(name);
    $('#emitterColorEndEnabled').disabled=false;
    document.querySelector('label[for="emitterColorEnd"]').textContent=name==='cloud-light'?'Amber spill tint':name==='cloud-bank'&&Number(record.rolling)>0?'Shadowed body tint':'End tint';
    $('#emitterColorEndEnabled').checked=record['color-end']!==undefined;
    $('#emitterColorEnd').disabled=record['color-end']===undefined;
    $('#emitterColorEnd').value='#'+(record['color-end']??record.color??'ffffff');
    $('#effectEnabled').checked=record.enabled!=='0';
    $('#effectIntensity').value=record.intensity??sourceDefault(name,'intensity',source?.member);
    $('#effectReach').value=record.reach??sourceDefault(name,'reach',source?.member);
    $('#effectIllumination').value=record.illumination??sourceDefault(name,'illumination');
    $('#effectIllumination').disabled=disabled||definition||!!source?.member||!receiverNames.includes(name);
    $('#effectColor').value='#'+(record.color??'ffffff');
    $('#effectSourceId').textContent=source?.id??'';
    const markers=source?mapEmitters().filter(e=>e.id===source.id):[];
    const marker=markers.find(e=>e.guideIndex===(modal?.guideIndex??selectedGuideIndex))||markers[0];
    $('#effectRegionControls').hidden=!marker?.region;
    for(const key of ['X','Y','Width','Height'])$('#effectRegion'+key).value=marker?.[key.toLowerCase()]??0;
    $('#effectRegionY').disabled=marker?.moveY===false;$('#effectRegionHeight').disabled=marker?.resizeHeight===false;
    $('#effectRegionWidth').max=marker?.maxWidth??16384;$('#effectRegionHeight').max=marker?.maxHeight??16384;
    $('#effectRegionWidth').min=marker?.minWidth??4;
    $('#effectRegionHint').textContent=name==='castle-mist'?
      'Y is the supporting floor. Drag the upper corners to change mist height without lifting it off the floor. Width must stay over a contiguous supported floor.':
      name==='blood-water'?'Drag horizontally to resize the shoreline span. Its height and Y remain on the native water band; edit the complete field for water depth and motion.':
      'Handles edit the complete field definition. Shared shoreline heights and atmosphere styles also affect linked regions. Source witnesses and collision remain required.';
    $('#effectSourceInfo').textContent=source?.nativeExposure?'Native light box. Editing extracts this dimming zone from its atmosphere definition. Apply saves both changes together; Cancel leaves the original intact.':source?
      `${source.name.replaceAll('-',' ')}${source.name==='moon-field'?` · BG2 source at ${Math.round(marker?.x??source.x)}, ${Math.round(marker?.y??source.y)}`:source.catalogue&&!source.movable&&!source.guides?.length?' · room family':` at ${Math.round(marker?.x??source.x)}, ${Math.round(marker?.y??source.y)} map pixels`}. `+
      (source.name==='ground-mist'?(source.support===undefined?'Preview to inspect supported spans.':source.support?`${source.support} supported spans. No surfaces outside the painted area are used.`:'No supported floor inside this area. Move or expand it to include a cyan floor edge.'):
      source.name==='castle-sky'?'Distant sky scattering stays behind the castle wall. Configure scenery/player/enemy illumination on the castle window lights.':
      source.name==='floor-mist'?'Collision-supported mist span. The outline ends at the supporting floor, including spike bottoms. Edit the complete atmosphere definition to change the floor search area or mist height.':
      attached?'Actor attachment. Applies to every matching visible actor; the marker previews its offset at the reference actor.':source.field?'Complete definition. Edit the source contacts, shapes, profiles and motion below.':source.member?'Individual catalogue member. Move, tint or disable this member independently.':source.movable?'Individual source: move its anchor or scale its area.':source.actor?'Actor family override. Applies to all recognized actors of this kind in this room and terrain.':source.emitter?`Placed emitter in ${anchor==='bg1'?'BG1':'BG2'} coordinates.`:source.reach?'Reach changes the spill radius without brightening its centre.':'Compound source: tint/intensity affect the whole group.'):
      'Select an effect marker on the map, or right-click to add an effect.';
    for(const row of document.querySelectorAll('#effectInspector .ctl'))
      row.hidden=!!row.querySelector('input,select')?.disabled;
  }
  for(const [id,key] of exposureFields)$('#'+id).onchange=e=>editEmitter(selected,{[key]:key==='brightness'?Number(e.target.value)/100:e.target.value});
  $('#effectSource').onchange=e=>{selected=e.target.value;refreshInspector();};
  for(const [id,key] of [['effectEnabled','enabled'],['effectIntensity','intensity'],['effectReach','reach'],['effectIllumination','illumination'],['effectColor','color']])
    $('#'+id).onchange=e=>{
      if(!selected)return;
      const next=new Map(records),value=id==='effectEnabled'?(e.target.checked?'1':'0'):
        id==='effectColor'?e.target.value.slice(1):e.target.value;
      next.set(selected,{...next.get(selected),[key]:value});
      change(encode(next),'edit environmental effect');
    };
  function receivers(mode) {
    if(!selected)return;
    const next=new Map(records),r={...next.get(selected)},prefix=mode.toLowerCase();
    for(const target of ['Scenery','Player','Enemies']) {
      const key=`${prefix}-${target.toLowerCase()}`;
      if($('#effect'+mode+'Targets').checked)r[key]=$('#effect'+mode+target).checked?'1':'0';else delete r[key];
    }
    next.set(selected,r);change(encode(next),'edit effect receivers');
  }
  for(const mode of ['Light','Dim']) {
    $('#effect'+mode+'Targets').onchange=()=>receivers(mode);
    for(const target of ['Scenery','Player','Enemies'])$('#effect'+mode+target).onchange=()=>receivers(mode);
  }
  for(const key of ['x','y','width','height','particles','lifetime','mist-height'])
    $('#emitter'+key).onchange=e=>editEmitter(selected,{[key]:e.target.value});
  $('#emitterAnchor').onchange=e=>editEmitter(selected,{anchor:e.target.value,
    ...(selected.includes(':storm-lightning:')&&e.target.value!=='bg2-point'?
      {'parallax-x':null,'ground-palette':null,'ground-colors':null}:{})});
  function newEmitterId(name) {
    let id;
    do{id=['emitter',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),
      terrainProfile,name,crypto.getRandomValues(new Uint32Array(1))[0].toString(16).padStart(8,'0')].join(':');}
    while(records.has(id));
    return id;
  }
  const stagePresets=window.__ACTION_EFFECT_PRESETS__||[];
  for(const preset of stagePresets) {
    const option=document.createElement('option');option.value='stage:'+preset.id;
    option.textContent=preset.title;$('#emitterPreset').append(option);
  }
  function placeStagePreset(id,x,y) {
    const preset=stagePresets.find(p=>p.id===id);if(!preset)return false;
    // Presets are editable compositions. Every member is serialized as an
    // ordinary emitter; native rendering has no preset-specific code path.
    if(bgIndex!==0){status('Place this composition on BG1, then change an individual source to a BG2 anchor if needed.');return false;}
    const next=new Map(records);let first='';
    for(const member of preset.members) {
      const {kind,x:dx=0,y:dy=0,...properties}=member,id=newEmitterId(kind);
      if(!first)first=id;
      const attached=!!properties['actor-target'];
      next.set(id,{...properties,x:String(Math.round((attached?0:x)+Number(dx))),
        y:String(Math.round((attached?0:y)+Number(dy)))});
    }
    if(!change(encode(next),'add '+preset.title))return false;
    selected=first;refreshInspector();status(preset.description+' Each marker can be edited separately.');return true;
  }
  function placeEmitter(name,x,y) {
    if(name.startsWith('stage:'))return placeStagePreset(name.slice(6),x,y);
    if(name==='cloud-light'&&bgIndex!==1){status('Cloud-break lighting uses the BG2 map. Switch to BG2 to place it.');return false;}
    if(bgIndex===1&&requiresTerrain(name)) {
      status('This terrain definition uses BG1. Switch to the BG1 map to place it.');return false;
    }
    if(Object.values(surfaceNames).some(([field])=>field===name))return createSurfaceDefinition(name,x,y,false);
    if(['fireball-field','orb-field','jungle-fire-field','lava-fire-field'].includes(name))return createProjectileDefinition(name);
    if(['trap-field','bolt-field','centaur-field'].includes(name))return createArcDefinition(name);
    if(name==='ray-field')return createDefinition(x,y,false);
    if(name==='glow-field')return createGlowDefinition(x,y,false);
    if(name==='castle-field')return createCastleDefinition(x,y,false);
    if(name==='marsh-field')return createMarshDefinition(x,y,false);
    if(name==='moon-field')return createMoonDefinition(x,y,false);
    if(name==='atmosphere-field')return createAtmosphereDefinition(x,y,false);
    if(name==='water-field')return createWaterDefinition(x,y,false);
    const preset=name.startsWith('preset:')?name.slice(7):null;
    if(preset==='ground-dust'&&bgIndex!==0){status('Ground dust paths use BG1.');return false;}
    if(preset)name=['wisps','moss'].includes(preset)?'fireflies':preset==='dew'?'drips':preset==='halo'?'halo':preset==='ground-dust'?'cloud-bank':'particle-area';
    const presets={
      wisps:{'glow-style':'wisps',trail:'.8',width:'160',height:'80',particles:'1',color:'a6efcf','color-end':'549f9b',intensity:'1.6',illumination:'.7','size-min':'1.5','size-max':'2.1',wander:'24','travel-x':'24','travel-y':'10',lifetime:'1100'},
      moss:{'glow-style':'moss',width:'48',height:'8',particles:'2',color:'68b38a','color-end':'305d49',intensity:'.9',illumination:'.25','size-min':'.8','size-max':'1.2',wander:'0','travel-x':'0','travel-y':'0',lifetime:'1800',placement:'playfield'},
      dew:{width:'4',height:'96',particles:'1',color:'c3dfc8','color-end':'8faba0',intensity:'2',activity:'.28',ripple:'0','size-min':'.45','size-max':'.65',lifetime:'420',placement:'foreground'},
      'ground-dust':{width:'320',height:'64',color:'e6c99a',intensity:'2.6',strands:'5',amplitude:'18',drift:'100',carry:'140',particles:'32','size-min':'.25','size-max':'.75',wander:'4',lifetime:'320',softness:'.9',placement:'foreground',points:'-160,24 -128,24 -96,24 -64,24 -32,24 0,24 32,24 64,24 96,24 128,24 160,24'},halo:{width:'160',height:'160',color:'efe4ff','color-end':'7256b8',intensity:'1.4',softness:'.55'},
      dust:{color:'d5b990','travel-y':'-32',wander:'12','size-min':'.7','size-max':'1.8',lifetime:'480'},
      leaves:{textured:'1',color:'739644','travel-y':'160',wander:'24','size-min':'1','size-max':'2.5',lifetime:'360'},
      snow:{color:'d5e8ff','travel-x':'32','travel-y':'224',wander:'12',lifetime:'480'},
      sand:{color:'d9af72','travel-x':'224','travel-y':'24',wander:'4',lifetime:'240'},
      insects:{color:'ffd780','travel-y':'0',wander:'24',lifetime:'360'},
      scarabs:{color:'916c3f','travel-y':'0','travel-x':'96',wander:'16','size-min':'1','size-max':'2'},
      sparks:{color:'ffbd55','color-end':'c12d15','travel-y':'-160',wander:'8',lifetime:'180'}};
    const next=new Map(records),id=newEmitterId(name);
    next.set(id,{x:String(Math.round(x)),y:String(Math.round(y)),width:'96',height:'64',
      ...(bgIndex===1&&!terrainNames.includes(name)?{anchor:name==='water-surface'?'bg2-raster':'bg2-point',placement:'background'}:{}),
      color:name.endsWith('mist')||['drips','waterfall-spray','water-surface'].includes(name)?'91bedf':
        ['flame','motes','particle-area'].includes(name)?'ffe6bb':'dde6ff',
      ...(name==='particle-area'?{particles:'4',width:'512',height:'512','travel-y':'-96'}:name==='exposure'?{brightness:'.65',width:'512',height:'512','fade-left':'64','fade-top':'64','fade-right':'64','fade-bottom':'64'}:{}),
      ...(name==='canopy-light'?{width:'512',height:'256',color:'ffedc7',intensity:'1',placement:'playfield',shadows:'1',softness:'.65',scatter:'.12',bounce:'.4',amplitude:'2',angle:'0',lifetime:'1440'}:{}),
      ...(name==='foliage-shadow'?{width:'128',height:'256',color:'162c1c',intensity:'.5',placement:bgIndex===0?'playfield':'background',strands:'2',textured:'1',shadows:bgIndex===0?'1':'0',softness:'.7',amplitude:'2',angle:'0',lifetime:'960'}:{}),
      ...(name==='cloud-light'?{...cloudDefaults,intensity:'1.1',seed:'104'}:{}),
      ...(name==='storm-lightning'?{width:'192',height:'112',color:'c5d5ff',lifetime:'720',activity:'.05',strands:'3',amplitude:'10',placement:'background','light-scenery':'0','light-player':'0','light-enemies':'0'}:{}),
      ...(name==='fireflies'?{width:'192',height:'96',color:'e5f7a2','color-end':'86b66c',particles:'6','size-min':'.7','size-max':'1.2',wander:'18','travel-x':'12','travel-y':'0',lifetime:'720',placement:'foreground','light-scenery':'1','light-player':'1','light-enemies':'1',illumination:'1'}:{}),
      ...(name==='heat-haze'?{anchor:bgIndex===1?'bg2-point':'bg1',placement:'background',width:bgIndex===1?'512':'128',height:bgIndex===1?'256':'112',amplitude:'1.5',lifetime:'720'}:{}),
      ...(name==='particle-path'?{points:'0,-24 0,24',pattern:'sand',particles:'24',wander:'2',activity:'.6',lifetime:'240',color:'e8c38a'}:{}),
      ...(name==='torch'?{width:'32',height:'32',color:'ffffff',placement:bgIndex===1?'background':'playfield'}:{}),
      ...(name==='flame'?{width:'64',height:'96',color:'ff6628','color-end':'ffe6a0',particles:'12',lifetime:'80','travel-y':'-64',wander:'5',spread:'.25','size-min':'.4','size-max':'1.2'}:{}),
      ...(preset?{...(name==='particle-area'?{pattern:preset}:{}),...presets[preset]}:{})});
    return change(encode(next),'add emitter');
  }
  $('#emitterAdd').onclick=()=>{
    const layer=room.bg[bgIndex];
    openModal(null,bgIndex===1?layer.pagesWide*128:nativeCamera.x+128,
      bgIndex===1?layer.pagesHigh*128:nativeCamera.y+112);
  };
  for(const [id,key] of nativeFields)$('#'+id).onchange=e=>editEmitter(selected,{[key]:e.target.value},'edit native effect member');
  for(const key of ['X','Y','Width','Height']) {
    const input=$('#effectRegion'+key);
    input.onchange=e=>editEmitter(selected,{[key.toLowerCase()]:e.target.value},'edit effect region');
    input.title='Shift-click to restore the loaded region value';
    input.addEventListener('pointerdown',ev=>{
      if(!ev.shiftKey)return;ev.preventDefault();ev.stopPropagation();
      const source=sources.find(s=>s.id===selected),definition=regionDefinition(source.name);
      const original=EffectRegions.bounds(source.name,resetRecords.get(definition.id)||definition.record,modal?.guideIndex??selectedGuideIndex??0);
      const axis=['X','Width'].includes(key)?'X':'Y',scale=bgIndex===1?1:source['mapScale'+axis]||1;
      editEmitter(selected,{[key.toLowerCase()]:original[key.toLowerCase()]*scale},'reset effect region value');
    });
  }
  function editEmitter(id,patch,label='edit emitter') {
    const source=sources.find(s=>s.id===id);
    if(source&&!source.member&&!source.movable&&EffectRegions.owner(source.name)&&
        ['x','y','width','height'].some(key=>patch[key]!==undefined))
      return editRegion(source,patch,label,patch.guideIndex??modal?.guideIndex??selectedGuideIndex??0);
    if(!id.startsWith(emitterPrefix())&&!source?.movable&&!source?.field)return false;
    if(source?.movable) {
      const converted={...patch};
      for(const axis of ['x','y'])if(patch[axis]!==undefined) {
        converted['offset-'+axis]=Math.round(Number(patch[axis])/(source['mapScale'+axis.toUpperCase()]||1)-source['base'+axis.toUpperCase()]);
        delete converted[axis];
      }
      for(const [key,scale,base] of [['width','width-scale','baseWidth'],['height','length-scale','baseHeight']])
        if(patch[key]!==undefined){if(nativeCapabilities(source)[key==='width'?'width':'length'])converted[scale]=Number(patch[key])/(source[key==='width'?'mapScaleX':'mapScaleY']||1)/source[base];delete converted[key];}
      patch=converted;
    }
    if(source?.field&&id.includes(':moon-field:')&&(patch.x!==undefined||patch.y!==undefined)) {
      const anchor=records.get(id).anchor.split(/\s+/).map(Number);
      patch={anchor:`${Math.round(patch.x??anchor[0])} ${Math.round(patch.y??anchor[1])}`};
    }
    const next=new Map(records),record={...(source?.nativeExposure?source.record:{}),...next.get(id)};
    if(id.includes(':cloud-bank:')) {
      if(patch.points)patch={...patch,textured:null,rolling:null,volume:null,rainbow:null,sparkle:null,wake:null,'canopy-response':null};
      if((Number(patch.volume)>0||Number(patch.rainbow)>0||Number(patch.sparkle)>0)&&!(Number(patch.rolling??record.rolling)>0))patch={...patch,rolling:'.8'};
      if(patch.anchor?.startsWith('bg2'))patch={...patch,wake:null,'canopy-response':null};
      if(patch.textured!==undefined&&String(patch.textured)!=='1'||patch.rolling!==undefined&&!(Number(patch.rolling)>0))
        patch={...patch,rolling:null,volume:null,rainbow:null,sparkle:null,wake:null,'canopy-response':null};
    }
    if(id.includes(':wet-contour:')&&!record.points&&(patch.width!==undefined||patch.height!==undefined))record.points='-32,0 32,0';
    if(['cloud-bank','particle-path','wet-contour'].some(kind=>id.includes(':'+kind+':'))&&record.points&&patch.points===undefined&&(patch.width!==undefined||patch.height!==undefined)) {
      const sx=Number(patch.width===null?96:patch.width??record.width??96)/Number(record.width??96),sy=Number(patch.height===null?64:patch.height??record.height??64)/Number(record.height??64);
      patch={...patch,points:record.points.split(/\s+/).map(pair=>{const [x,y]=pair.split(',').map(Number);return `${x*sx},${y*sy}`;}).join(' ')};
    }
    if(source?.field&&id.includes(':ray-field:'))for(const [countKey,prefix] of [['ray-count','ray'],['fan-count','fan'],['profile-count','profile'],['witness-count','witness']]) {
      if(patch[countKey]===undefined)continue;
      const count=Number(patch[countKey]),previous=Number(record[countKey]);
      if(!Number.isInteger(count)||count<0||count>12)continue;
      for(let i=count+1;i<=previous;i++)delete record[`${prefix}-${i}`];
      for(let i=previous+1;i<=count;i++) {
        let value=record[`${prefix}-${Math.max(1,previous)}`];
        if(value===undefined)value=prefix==='fan'?'0 -1024':prefix==='witness'?'0 0 0 0':undefined;
        if(value!==undefined)record[`${prefix}-${i}`]=value;
      }
      if(prefix==='fan'||prefix==='profile')for(const key of Object.keys(record).filter(k=>/^ray-\d+$/.test(k))) {
        const values=record[key].split(/\s+/),at=prefix==='fan'?4:5;
        if(Number(values[at])>=(prefix==='fan'?count+1:count))values[at]='0';record[key]=values.join(' ');
      }
    }
    for(const [key,value] of Object.entries(patch)) {
      if(value===null)delete record[key];else record[key]=String(value);
    }
    next.set(id,record);return change(encode(next),label);
  }
  function resetValues(id,keys,index) {
    const next=new Map(records),record={...next.get(id)},defaults=resetRecords.get(id)||{};
    for(const key of keys) {
      if(index!==undefined) {
        const parts=String(record[key]).split(/\s+/),base=String(defaults[key]??'').split(/\s+/);
        if(base[index]===undefined||base[index]==='')return false;
        parts[index]=base[index];record[key]=parts.join(' ');
      } else if(defaults[key]!==undefined)record[key]=defaults[key];else delete record[key];
    }
    if(!Object.keys(record).length)next.delete(id);else next.set(id,record);
    let ok;
    if(id.includes(':ray-field:')&&keys.some(k=>k.endsWith('-count'))) {
      const patch=Object.fromEntries(keys.map(k=>[k,record[k]??null]));
      for(const key of keys.filter(k=>k.endsWith('-count'))) {
        const prefix=key.slice(0,-6),previous=Number(records.get(id)?.[key]);
        for(let i=previous+1;i<=Number(record[key]);++i)
          if(defaults[`${prefix}-${i}`]!==undefined)patch[`${prefix}-${i}`]=defaults[`${prefix}-${i}`];
      }
      ok=editEmitter(id,patch,'reset field count');
    } else if(['cloud-bank','particle-path','wet-contour'].some(kind=>id.includes(':'+kind+':'))&&keys.some(k=>k==='width'||k==='height')) {
      ok=editEmitter(id,Object.fromEntries(keys.map(k=>[k,record[k]??null])),'reset cloud bounds');
    } else ok=change(encode(next),'reset effect value');
    // A focused input may contain an uncommitted value while the document is
    // already at its default. Discard that text too, without making an undo.
    definitionKey='';refreshInspector();return ok;
  }
  function resetControl(input,keys,index) {
    input.title=(input.title?input.title+' · ':'')+'Shift-click to reset to default';
    const reset=ev=>{
      if(!ev.shiftKey||input.disabled)return;
      ev.preventDefault();ev.stopPropagation();
      resetValues(selected,keys,index);
    };
    input.addEventListener('pointerdown',reset);
    // Also handle keyboard/accessible clicks, which need not send pointerdown.
    // Repeating a reset is a no-op, so it never creates a second undo entry.
    input.addEventListener('click',reset);
  }
  function resetHandle(entry,handle) {
    if(entry.region) {
      const source=sources.find(s=>s.id===entry.id),definition=regionDefinition(source.name);
      const base=resetRecords.get(definition.id)||definition.record;
      const original=EffectRegions.bounds(source.name,base,entry.guideIndex);
      const current=EffectRegions.bounds(source.name,definition.record,entry.guideIndex);
      const desired=handle==='size'?{...current,width:original.width,height:original.height}:
          {...current,x:original.x,y:original.y};
      const sx=bgIndex===1?1:source.mapScaleX||1,sy=bgIndex===1?1:source.mapScaleY||1;
      return editRegion(source,{x:desired.x*sx,y:desired.y*sy,width:desired.width*sx,height:desired.height*sy},'reset effect region',entry.guideIndex);
    }
    const keys=handle==='rotate'?['angle']:handle==='size'?
      (entry.native?['width-scale','length-scale']:['cloud-bank','particle-path','wet-contour'].includes(entry.kind)&&entry.points?['width','height','points']:['width','height']):
      entry.kind==='moon-field'?['anchor']:entry.native?['offset-x','offset-y']:['x','y'];
    return resetValues(entry.id,keys);
  }
  const actorFields=[['Source','actor-source'],['State','actor-state'],['Visual','actor-visual'],['Parent','actor-parent'],['Limit','actor-limit'],['Animation','actor-animation'],['Handler','actor-handler'],['Resume','actor-resume']];
  $('#actorBindingFamily').onchange=e=>{$('#actorBindingSource').value=e.target.value;};
  $('#actorBindingApply').onclick=()=>{
    const record=records.get(selected);if(!record)return;
    const target=$('#actorBindingTarget').value,patch={};
    for(const key of Object.keys(record).filter(k=>k.startsWith('actor-')))patch[key]=null;
    if(target){patch['actor-target']=target;patch.anchor='bg1';patch.placement='foreground';
      for(const [id,key] of actorFields){const value=$('#actorBinding'+id).value.trim();if(value)patch[key]=value;}
      if(!record['actor-target']){patch.x='0';patch.y='0';}
    }
    editEmitter(selected,patch,'change actor attachment');
  };
  $('#actorBindingBossFire').onclick=()=>editEmitter(selected,{'actor-target':'family','actor-source':'B786','actor-parent':'B786','actor-state':'0,1','actor-animation':'5000','actor-handler':null,'actor-resume':null,'actor-visual':null,'actor-limit':'4',anchor:'bg1',placement:'foreground',x:'0',y:'0'},'bind Bloodpool boss fire');
  $('#actorBindingPreview').onclick=()=>{
    const record=records.get(selected);if(!record?.['actor-target'])return;
    try{SharedRoomPreview.previewActorBinding(parseInt(selected.split(':').at(-1),16),actor.x,actor.y,nativeFrame);status('Attachment preview active at the reference actor. Scrub time in the shared renderer.');}
    catch(error){status(error.message);}
  };
  $('#actorBindingStop').onclick=()=>{try{SharedRoomPreview.previewActorBinding(null,0,0,0);}catch(error){status(error.message);}};
  $('#emitterPoints').onchange=e=>editEmitter(selected,{points:e.target.value.trim()||null,
    ...(e.target.value.trim()&&selected.includes(':cloud-bank:')?{textured:null,rolling:null,volume:null,rainbow:null,sparkle:null,wake:null,'canopy-response':null}:{}),
    ...(!e.target.value.trim()&&selected.includes(':cloud-bank:')?{carry:null,particles:null,twinkle:null}:{})});
  for(const [key] of cloudFields)$('#cloudLight-'+key).onchange=e=>editEmitter(selected,{[key]:e.target.value});
  for(const [key] of stormFields)$('#storm-'+key).onchange=e=>editEmitter(selected,
    {[key]:e.target.value.trim()||null,anchor:'bg2-point'});
  for(const [id,key] of fieldFields)$('#'+id).onchange=e=>editEmitter(selected,{[key]:e.target.value===''?null:e.target.value,
    ...(key==='textured'&&e.target.value!=='1'?(selected.includes(':foliage-shadow:')?{shadows:null}:selected.includes(':cloud-bank:')?{rolling:null,volume:null,rainbow:null,sparkle:null,wake:null,'canopy-response':null}:{}):{}),
    ...(key==='pattern'&&e.target.value!=='leaves'?{textured:null}:{}),
    ...(key==='rolling'&&Number(e.target.value)===0?{volume:null,rainbow:null,sparkle:null,wake:null,'canopy-response':null}:{}),
    ...(selected.includes(':cloud-bank:')&&['volume','rainbow','sparkle','wake','canopy-response'].includes(key)&&Number(e.target.value)>0&&!(Number(records.get(selected)?.rolling)>0)?{rolling:'.8'}:{})});
  for(const [id,key] of moteFields)$('#'+id).onchange=e=>
    editEmitter(selected,{[key]:e.target.value===''?null:e.target.value});
  $('#emitterAutoRise').onclick=()=>editEmitter(selected,{'travel-y':null});
  $('#emitterSeedShuffle').onclick=()=>editEmitter(selected,{seed:crypto.getRandomValues(new Uint32Array(1))[0]},'change mote pattern');
  $('#emitterColorEndEnabled').onchange=e=>editEmitter(selected,{'color-end':e.target.checked?$('#effectColor').value.slice(1):null});
  $('#emitterColorEnd').onchange=e=>editEmitter(selected,{'color-end':e.target.value.slice(1)});
  function fieldPrefix() {return emitterPrefix().replace(/^emitter:/,'field:');}
  const definitionLabels={ray:['World X','Half width','Strength','Shoulder','Origin group','Profile (0-based)'],
    fan:['Source X','Source Y'],profile:['Row 1','Row 2','Row 3','Row 4','Row 5','Row 6','Front fade start','Front fade range','Rear R','Rear G','Rear B','Rear opacity','Front R','Front G','Front B','Front opacity'],
    witness:['BG (0-based)','World X','World Y','Metatile ID'],dimensions:['BG1 width','BG1 height','BG2 width','BG2 height'],
    anchor:['Camera offset X','Camera offset Y'],window:['Left','Top','Right','Bottom'],bounds:['Left','Top','Right','Bottom'],
    color:['Red','Green','Blue','Opacity'],counts:['Pools','Falls','Wet contacts','Map witnesses'],components:['Enabled component bits'],
    pool:['Left X','Right X','Surface Y'],fall:['Splash X','Splash Y'],wet:['World X','Ceiling Y','Landing Y','Ceiling metatile','Landing metatile','Water contact (0/1)'],
    'glint-shape':['Minimum width','Width variation','Height','Opacity'],
    'glint-motion':['Depth offset','Depth spread','Horizontal sway','Spacing','Period (ticks)'],
    'glint-color':['Red','Green','Blue'],'ripple-color':['Red','Green','Blue'],
    'ripple-shape':['Initial width','Width expansion','Initial height','Height expansion','Rim width','Rim height','Fade in rate','Lifetime (ticks)'],
    'pool-insets':['Left inset','Right inset'],
    'pool-ripple':['Side inset','Depth offset','Opacity'],
    'fall-motion':['Row spacing','Period (ticks)','Horizontal spread','Horizontal offset','Splash exclusion','Pulse base','Pulse variation'],
    'fall-glint':['Width','Height','Opacity'],'fall-pulse':['Period (ticks)','Phase per tick'],
    'fall-glow':['Radius X','Radius Y','Splash offset Y','Axis X'],'fall-rings':['Inner scale','Middle scale','Outer scale'],
    'fall-center':['Red','Green','Blue','Opacity'],'fall-ring-1':['Red','Green','Blue','Opacity'],
    'fall-ring-2':['Red','Green','Blue','Opacity'],'fall-ring-3':['Red','Green','Blue','Opacity'],
    'drip-timing':['Short period','Long period','Gather ticks','Fall ticks','Contact ticks'],
    'drip-gather':['Initial width','Width per tick','Initial height','Height per tick','Opacity'],
    'drip-fall':['Width','Initial height','Height expansion','Opacity'],
    'drip-contact':['Ripple offset Y','Ripple opacity','Spray spread','Spray lift','Spray width','Spray height','Spray opacity'],
    'sheen-motion':['Period (ticks)','Phase per tick','Column phase','Contact phase','Shimmer base','Shimmer variation'],
    'sheen-shape':['Contour centre column','Edge falloff','Contour offset Y','Minimum depth','Centre depth','Lip depth'],
    'sheen-body':['Red','Green','Blue','Opacity'],'sheen-lip':['Red','Green','Blue','Opacity base','Opacity variation'],
    'mist-volume':['Slices','Base period','Drift fraction','Width fraction','Width loss per slice','Height fraction','Billow height','Height loss per slice'],
    'mist-billow':['Base height','Primary amplitude','Secondary amplitude','Primary frequency','Secondary frequency','Secondary clock'],
    'mist-density':['Base density','Billow density'],'mist-lighting':['Ambient','Height gain','Billow gain'],
    'mist-color':['Red','Green','Blue'],'mist-gain':['Red gain','Green gain','Blue gain'],
    'spray-shape':['Half width','Height','Splash offset Y','Illumination']};
  const atmosphereLabels={
    counts:['Ambient pools','Dust regions','Grit contacts','Tower sources','Map witnesses'],
    ambient:['World X','World Y','Radius X','Radius Y','Lean','Exposure','Surface gain','Phase index'],
    area:['Left','Top','Right','Bottom'],grit:['World X','Ceiling Y','Landing Y','Seed index'],tower:['Source X','Source Y','Lean'],
    'ambient-motion':['Period (ticks)','Phase per tick','Intensity base','Intensity variation','Phase spacing'],
    'dust-grid':['Cell width','Cell height','Columns (max 25)','Rows (max 13)','Camera inset X'],
    'dust-motion':['Period (ticks)','Inset X','Jitter X','Sway X','Inset Y','Jitter Y','Rise'],
    'dust-shape':['Radius','Radius variation','Height ratio','Opacity base','Opacity variation','Light response'],
    'dust-density':['Use density band / culling (0/1)','Band starts at Y','Band opacity','Cull margin'],
    'dust-skip':['Seed mask (0 disables grains)'],
    'floor-area':['Left (16px grid)','Search top','Right','Search bottom'],
    'floor-style':['Mist height','Ambient light response','Exposure sample height'],
    'floor-source':['Source ID high word','Animation ID high word'],
    'grit-timing':['Period (ticks)','Falling grains','Delay per grain','Fall duration','Burst ends at'],
    'grit-shape':['Offset X','Radius','Radius per grain','Upper height','Lower height','Cull margin'],
    'grit-burst':['Burst strength','Burst grains','Cull margin'],
    'grain-timing':['Delay variation','Lifetime','Lifetime variation','Fade in rate'],
    'grain-motion':['Spread base','Spread growth','Spread variation','Lift','Lift variation','Wobble amplitude','Wobble frequency','Floor inset'],
    'grain-shape':['Radius','Radius variation','Height ratio','Opacity','Opacity variation'],
    'tower-rows':['Y 1','Y 2','Y 3','Y 4','Y 5'],'tower-widths':['Half width 1','Half width 2','Half width 3','Half width 4','Half width 5'],
    'tower-strengths':['Gain 1','Gain 2','Gain 3','Gain 4','Gain 5'],'tower-across':['Left edge','Left shoulder','Centre','Right shoulder','Right edge'],
    'tower-motion':['Period (ticks)','Phase per tick','Intensity base','Cloud amplitude','Staggered amplitude','Staggered frequency','Window phase spacing']};
  const castleLabels={
    "style":["Soft / gallery / boss (0 / 1 / 2)"],
    "dimensions":["BG1 width (0 = any)", "BG1 height", "BG2 width", "BG2 height"],
    "witness-count":["Required map samples"],
    "witness-1":["Layer (0 = BG1)", "X", "Y", "Metatile ID"],
    "witness-2":["Layer (0 = BG1)", "X", "Y", "Metatile ID"],
    "material":["Window material ID", "Top left CHR", "Top right CHR", "Bottom left CHR", "Bottom right CHR"],
    "source-count":["Active sources"],
    "source":["Kind: window / fill / torch (0 / 1 / 2)", "Art sample X", "Art sample Y", "Top metatile", "Below metatile", "Source X", "Arch Y", "Lower reach", "Half width", "Spread", "Lean", "Clip left", "Clip right", "Clip bottom", "Sill Y", "Sill metatile", "Stable variation ID", "Arch: narrow / wide / crown (0 / 1 / 2)"],
    "floor":["Left", "Right", "Floor Y (0 = off)"],
    "floor-exclude":["Exclude metatile 1", "Exclude metatile 2", "Exclude metatile 3", "Exclude metatile 4"],
    "sky-witness-1":["Layer", "X", "Y", "Metatile ID"],
    "sky-witness-2":["Layer", "X", "Y", "Metatile ID"],
    "sky-anchor":["BG2 X", "BG2 Y"],
    "sky-window":["Left", "Top", "Right", "Bottom"],
    "sky-clip":["Left", "Top", "Right", "Bottom"],
    "sky-shape":["Glow radius X", "Glow radius Y"],
    "sky-color":["Red", "Green", "Blue", "Opacity"],
    "sky-rays":["Enable fan (0 / 1)", "Ray brightness"],
    "water-surface":["Left", "Surface Y", "Strip width", "Strip count (max 8)"],
    "water-material":["Metatile ID", "Top left CHR", "Top right CHR", "Bottom left CHR", "Bottom right CHR"],
    "water-tiles":["Surface metatile", "Open metatile above"],
    "water-seed":["Variation seed"],
    "water-motion":["Cycle ticks (power of two)", "Horizontal drift"],
    "water-fine":["First glint X", "Spacing", "Base width", "Width variation", "Height", "Opacity"],
    "water-broad":["First highlight X", "Spacing", "Width", "Height", "Opacity"],
    "water-rows":["First row", "Row variation"],
    "water-color":["Red", "Green", "Blue"],
    "ripple-timing":["Cycle ticks (power of two)", "Lifetime ticks"],
    "ripple-position":["X inset", "X variation", "Y inset", "Y variation"],
    "ripple-shape":["Initial radius X", "Growth X", "Initial radius Y", "Growth Y", "Ring width X", "Ring width Y"],
    "ripple-color":["Red", "Green", "Blue", "Opacity"],
    "ripple-fade":["Fade-in speed"],
    "arch-widths":["Narrow half width", "Wide half width", "Crown half width"],
    "arch-insets":["Narrow haze inset", "Wide haze inset", "Crown haze inset"],
    "upper-shape":["Max reach", "Opening-height fraction", "Max spread", "Spread scale", "Lean scale"],
    "strength":["Overall brightness", "Upper multiplier", "Lower multiplier"],
    "opening":["Interior opacity", "Side inset", "Arch inset", "Sill inset"],
    "opening-color":["Red", "Green", "Blue"],
    "ledge":["Extra width", "Y offset", "Height", "Opacity"],
    "ledge-color":["Red", "Green", "Blue"],
    "scatter":["Upper reach scale", "Lower reach scale", "Lower spread scale", "Upper opacity", "Lower opacity"],
    "breath":["Cycle ticks (power of two)", "Phase per tick", "Base gain", "Gain variation"],
    "fan-depths":["Row 1 depth", "Row 2 depth", "Row 3 depth", "Row 4 depth"],
    "fan-fade":["Row 1 opacity", "Row 2 opacity", "Row 3 opacity", "Row 4 opacity"],
    "fan-offset":["Pattern jitter"],
    "fan-pattern":["Background gain", "Shaft gain", "Left strength", "Left position", "Left width", "Middle width", "Right strength", "Right position", "Right width"],
    "upper-color":["Red", "Green", "Blue"],
    "lower-color":["Red", "Green", "Blue"],
    "gallery-gain":["Base brightness", "Variation", "Pillar brightness"],
    "gallery-floor":["Row 1 offset from sill", "Row 2 offset", "Row 3 offset"],
    "gallery-radius":["Row 1 half width", "Row 2 half width", "Row 3 half width"],
    "gallery-edges":["Pillar X offset", "Highlight width", "Y offset from arch", "Row spacing"],
    "gallery-color":["Red", "Green", "Blue"],
    "dust-counts":["Motes per window", "Motes per fill"],
    "dust-periods":["Short cycle ticks", "Long cycle ticks"],
    "dust-depth":["Start depth", "Window travel", "Fill travel", "Depth wander"],
    "dust-across":["Horizontal range", "Range center", "Horizontal wander", "Wander frequency"],
    "dust-gain":["Base opacity", "Opacity variation"],
    "dust-size":["Base size", "Size variation"],
    "dust-color":["Red", "Green", "Blue"],
    "mist-cells":["Spacing (48 minimum)", "Cycle ticks", "Phase per tick"],
    "mist-shape":["Base height", "Height variation", "Horizontal drift", "Base width", "Width variation", "Lean"],
    "mist-color":["Red", "Green", "Blue", "Opacity"],
    "mist-seed":["Variation seed"],
    "torch-pulse":["Cycle ticks", "Phase per tick", "Base gain", "Slow flicker", "Fast flicker", "Fast frequency", "Fast phase"],
    "torch-shape":["Y offset", "Height", "Lean", "Lean cycle ticks", "Lean phase per tick"],
    "torch-color":["Red", "Green", "Blue", "Opacity"],
    "ambient-shape":["Center depth", "Height scale"],
    "ambient-color":["Red", "Green", "Blue", "Opacity"],
    "seeds":["Window pattern", "Gallery highlights", "Dust variation"],
    "narrow-rows":["Arch pixel 1 height", "Arch pixel 2 height", "Arch pixel 3 height", "Arch pixel 4 height"],
    "wide-rows":["Arch pixel 1 height", "Arch pixel 2 height", "Arch pixel 3 height", "Arch pixel 4 height", "Arch pixel 5 height", "Arch pixel 6 height", "Arch pixel 7 height", "Arch pixel 8 height", "Arch pixel 9 height", "Arch pixel 10 height", "Arch pixel 11 height", "Arch pixel 12 height", "Arch pixel 13 height", "Arch pixel 14 height", "Arch pixel 15 height"],
    "boss-rows":["Arch pixel 1 height", "Arch pixel 2 height", "Arch pixel 3 height", "Arch pixel 4 height", "Arch pixel 5 height", "Arch pixel 6 height", "Arch pixel 7 height", "Arch pixel 8 height", "Arch pixel 9 height", "Arch pixel 10 height", "Arch pixel 11 height", "Arch pixel 12 height", "Arch pixel 13 height", "Arch pixel 14 height", "Arch pixel 15 height", "Arch pixel 16 height", "Arch pixel 17 height", "Arch pixel 18 height", "Arch pixel 19 height", "Arch pixel 20 height", "Arch pixel 21 height", "Arch pixel 22 height", "Arch pixel 23 height", "Arch pixel 24 height", "Arch pixel 25 height", "Arch pixel 26 height", "Arch pixel 27 height", "Arch pixel 28 height", "Arch pixel 29 height", "Arch pixel 30 height", "Arch pixel 31 height"],
  };
  const marshLabels={
  "dimensions": [
    "BG1 width (0 = any)",
    "BG1 height",
    "BG2 width",
    "BG2 height"
  ],
  "bounds": [
    "Left",
    "Top",
    "Right",
    "Bottom"
  ],
  "surface": [
    "Water tile top Y",
    "Exposed water Y",
    "Bottom pixel Y",
    "Shore inset"
  ],
  "span-count": [
    "Active water spans"
  ],
  "span": [
    "Left",
    "Right"
  ],
  "witness-count": [
    "Required map witnesses"
  ],
  "witness": [
    "Layer (0 = BG1)",
    "X",
    "Y",
    "Metatile ID"
  ],
  "detail-witness-count": [
    "Required detail witnesses"
  ],
  "detail-witness-1": [
    "Layer",
    "X",
    "Y",
    "Metatile ID"
  ],
  "detail-witness-2": [
    "Layer",
    "X",
    "Y",
    "Metatile ID"
  ],
  "detail-words": [
    "Definition mask",
    "Definition attributes"
  ],
  "water-tiles": [
    "Metatile ID 1",
    "Metatile ID 2",
    "Metatile ID 3",
    "Metatile ID 4",
    "Metatile ID 5",
    "Metatile ID 6",
    "Metatile ID 7",
    "Metatile ID 8",
    "Metatile ID 9"
  ],
  "post-tiles": [
    "Metatile ID 1",
    "Metatile ID 2",
    "Metatile ID 3",
    "Metatile ID 4"
  ],
  "material": [
    "Metatile ID",
    "Top left word",
    "Top right word",
    "Bottom left word",
    "Bottom right word"
  ],
  "water-rows": [
    "Local Y 1",
    "Local Y 2",
    "Local Y 3",
    "Local Y 4",
    "Local Y 5"
  ],
  "water-exposure": [
    "Row gain 1",
    "Row gain 2",
    "Row gain 3",
    "Row gain 4",
    "Row gain 5"
  ],
  "water-light": [
    "Moon response",
    "Red",
    "Green",
    "Blue"
  ],
  "water-cells": [
    "Spacing (minimum 24)",
    "Edge cull allowance",
    "Shore fade distance",
    "Position base",
    "Position variation"
  ],
  "mist-cells": [
    "Spacing (minimum 64)",
    "Edge cull allowance",
    "Shore fade distance",
    "Position base",
    "Position variation"
  ],
  "water-motion": [
    "Long period",
    "Short period",
    "Drift",
    "Angular speed",
    "First row Y",
    "Row gap",
    "Y variation"
  ],
  "water-gain": [
    "Opacity",
    "Opacity variation",
    "Pulse base",
    "Pulse amplitude"
  ],
  "water-shape": [
    "Half width",
    "Width variation",
    "Halo width scale",
    "Halo height",
    "Halo gain",
    "Core height"
  ],
  "mist-motion": [
    "Period",
    "Radians per tick",
    "Drift X",
    "Base Y",
    "Y variation",
    "Vertical drift",
    "Lean"
  ],
  "mist-shape": [
    "Half width",
    "Width variation",
    "Half height",
    "Height variation"
  ],
  "mist-gain": [
    "Opacity",
    "Opacity variation",
    "Pulse base",
    "Pulse amplitude"
  ],
  "mist-window": [
    "Top (relative to water)",
    "Bottom"
  ],
  "timber-motion": [
    "Period",
    "Radians per tick"
  ],
  "timber-gain": [
    "Moon response",
    "Pulse base",
    "Pulse amplitude"
  ],
  "timber-shape": [
    "Edge inset",
    "Offset Y",
    "Highlight depth"
  ],
  "drip-timing": [
    "Minimum fall",
    "Maximum fall",
    "Gravity",
    "Gravity variation",
    "Short period",
    "Long period"
  ],
  "drip-shape": [
    "Fade-in ticks",
    "Opacity",
    "Length",
    "Length growth",
    "Width"
  ],
  "ripple-shape": [
    "Duration",
    "Initial radius",
    "Radius growth",
    "Ring thickness",
    "Height ratio"
  ],
  "ripple-drip": [
    "Contact Y offset",
    "First ripple gain",
    "Second ripple delay",
    "Second ripple gain"
  ],
  "ripple-post": [
    "Period",
    "First ripple gain",
    "Second ripple delay",
    "Second ripple gain"
  ],
  "insect-count": [
    "Insects per bank (maximum 4)"
  ],
  "insect-region": [
    "Bank inset",
    "Base Y",
    "Height variation"
  ],
  "insect-motion": [
    "Period",
    "Radians per tick",
    "X drift",
    "X frequency",
    "X curl",
    "Curl frequency",
    "Y drift",
    "Y frequency",
    "Y phase",
    "Flash phase"
  ],
  "insect-gain": [
    "Ambient opacity",
    "Moon response",
    "Flash gain",
    "Halo threshold",
    "Halo opacity"
  ],
  "insect-shape": [
    "Halo width",
    "Halo height",
    "Width",
    "Width variation",
    "Height",
    "Height variation"
  ],
  "under-mist": [
    "Minimum timber Y",
    "Mist Y",
    "Period",
    "Radians per tick",
    "Drift X",
    "Half width",
    "Width variation",
    "Half height",
    "Lean"
  ],
  "under-mist-color": [
    "Red",
    "Green",
    "Blue",
    "Opacity"
  ],
  "seeds": [
    "Timber row salt",
    "Gravity salt",
    "Post salt",
    "Insect salt",
    "Mist width salt",
    "Mist height salt",
    "Mist position salt",
    "Mist opacity salt",
    "Water row salt",
    "Water opacity salt"
  ]
};
  const moonLabels={
    components:['Components: rays 1 + reflections 2 + cloud 4 + wave caps 8'],
    anchor:['BG2 fixed source X','BG2 fixed source Y'],window:['Left','Top','Right','Bottom'],
    'cloud-window':['Left','Top','Right','Bottom'],
    low:['Slope','Angular width','Intensity'],middle:['Slope','Angular width','Intensity','Reach'],
    'ray-domain':['Starting slope','Slope span','Start Y','Length'],
    'ray-shape':['Base haze','Angular falloff','Fade-in start Y','Fade-in length','Far end Y','Far fade length','Middle fade length'],
    'ray-gain':['Opacity limit','Distant haze gain','Middle haze gain'],pulse:['Period (ticks)','Phase per tick','Base','Amplitude'],
    shadow:['Source radius','Haze depth 1','Haze depth 2','Haze depth 3','Surface source weight','Surface receiver weight'],
    'cloud-motion':['Period','Phase per tick','Centre X','Drift X','X phase','Centre Y','Drift Y','Y frequency','Y phase','Opacity phase'],
    'cloud-opacity':['Minimum opacity','Opacity variation'],'cloud-shape':['Radius X','Radius Y','Lean'],
    'reflection-rows':['Rows (max 23)','Horizon Y','First row Y','Row spacing','Y variation'],
    'reflection-motion':['Period','Phase per tick','Base spread','Spread per row','Side spread','Wander'],
    'reflection-gain':['Side intensity','Centre intensity','Base exposure','Variation','Horizon fade','Bottom fade'],
    'reflection-shape':['Width','Width variation','Height'],'reflection-glow':['Centre X','Centre Y','Radius X','Radius Y'],
    'wave-rows':['Rows (max 12)','First BG2 raster row','Row spacing'],
    'wave-shape':['Width','Width per row','Width variation','Crest height','Spark offset','Spark width','Spark height'],
    'wave-motion':['Period','Phase per tick','Flash frequency'],
    'wave-gain':['Minimum exposure','Centre exposure','Exposure width','Width per row','Base shimmer','Shimmer variation','Flash gain'],
    seeds:['Reflection seed','Wave seed']};
  const surfaceLabels={
    'molten-surface':['Hot seam strength (0–1)','Surface cells (1–16)','Flow cycle (frames)','Surface depth (pixels)','Horizontal flow (pixels)','Surface height fraction'],
    'molten-bursts':['Bubble sites (0–4)','Mean interval (frames)','Active fraction (0.1–0.6)','Droplet height (pixels)','Bubble radius (pixels)'],
    'molten-color':['Red','Green','Blue','Opacity'],
    heat:['Heat shimmer enabled (flat presentation only)'],'heat-amplitude':['Minimum output pixels','Authentic pixel displacement','Maximum output pixels'],
    'heat-motion':['Animation speed','Secondary speed','Vertical speed'],'heat-envelope':['Top weight','Floor weight'],
    'heat-wave':['Main X frequency','Main Y frequency','Secondary strength','Secondary X frequency','Secondary Y frequency'],
    'heat-cross':['Vertical X frequency','Vertical Y frequency','Vertical strength'],
    kind:['Surface family (fixed by definition)'],mode:['Map witnesses (0) / explicit sources (1)'],
    scan:['Scan margin X','Scan margin Y'],anchor:['Source offset X','Source offset Y'],
    bounds:['Left','Top','Right','Bottom'],identity:['Identity high','Identity low','Animation identity high','Animation identity low'],
    projection:['Projection plane','Draw layer'],source:['World X','World Y','Local left','Local top','Local right','Local bottom','Unique source ID'],
    particles:['Particles (bounded per family)','Minimum lifetime','Lifetime seed shift','Lifetime variation mask'],
    segments:['Lava light span (96–512)'],shape:['Width','Width variation','Length','Length variation'],
    style:['Minimum radius X','Minimum radius Y','Width scale','Height scale','Added radius X','Added radius Y','Inner ring','Middle ring','Outer ring',
      'Centre red','Centre green','Centre blue','Centre opacity','Inner red','Inner green','Inner blue','Inner opacity',
      'Middle red','Middle green','Middle blue','Middle opacity','Outer red','Outer green','Outer blue','Outer opacity',
      'Flare','Rise','Axis X','Axis Y','Lift X','Lift Y','Offset X','Offset Y (pit: fraction of half-height)'],
    'cloud-counts':['Columns (max 6)','Depth tiers (max 4)'],'cloud-periods':['Horizontal period','Period per tier','Horizontal variation mask','Vertical period','Period per tier','Vertical variation mask','Breathing period','Fixed first anchor'],
    'cloud-jitter':['Horizontal scatter','Vertical scatter','Foam scatter','Horizontal drift','Drift per tier','Vertical rise','Rise per tier'],
    'cloud-shape':['Minimum size','Size variation','Breathing size','Breathing variation','Base opacity','Pulse opacity','Seed opacity','Opacity variation'],
    tier:['Offset Y','Radius X','Radius Y','Opacity','Offset X','Red','Green','Blue','Alpha']
  };
  const surfaceMotionLabels=[
    ['Edge inset','Horizontal drift','Surface height fraction','Birth Y scatter','Rise speed','Rise acceleration'],
    ['Edge inset','Horizontal drift','Lip Y offset','Birth Y scatter','Rise speed','Rise acceleration'],
    ['Edge inset','Horizontal drift','Drip start Y','Drip speed','Drip acceleration','Spray start Y','Spray rise','Spray fall'],
    ['Lanes','Left inset','Total width inset','Flow height extension','Flow period','Period per row','Lane scatter','Top extension','Side streak drift','Streak length','Length variation'],
    ['Horizontal drift','Birth Y','Birth Y scatter','Rise speed','Rise acceleration']
  ];
  const surfaceRuleLabels=[
    ['Left tile','Middle tile','Right tile','Fill tile','Bubble tile','Maximum middle cells'],
    ['Lip tile','Body tile','First animated tile','Last animated tile','Alternate animated tile','Maximum cells','Minimum cells','Left bank A','Right bank A','Left bank B','Right bank B','Left bank C','Right bank C'],
    ['Top left','Top middle','Top right','Body left','Body middle','Body right','Drip left','Drip middle','Drip right','Maximum width in cells','Maximum splashes']
  ];
  const projectileLabels={
    particles:['Trail particles (max 12)','Minimum lifetime','Lifetime seed shift','Lifetime variation mask'],
    motion:['Wake start distance','Wake travel','Side spread at birth','Side spread growth'],
    shape:['Particle width','Width at birth bonus','Particle reach','Reach at birth bonus'],
    'rest-heading':['Stationary direction X','Stationary direction Y'],
    'orb-heading':['Resting orb direction X','Resting orb direction Y'],
    'body-offset':['Along motion','Across motion'],
    style:['Radius X','Radius Y','Inner ring','Middle ring','Outer ring',
      'Centre red','Centre green','Centre blue','Centre opacity','Inner red','Inner green','Inner blue','Inner opacity',
      'Middle red','Middle green','Middle blue','Middle opacity','Outer red','Outer green','Outer blue','Outer opacity','Flare','Rise']
  };
  const arcLabels={
    "clock":["Animation speed", "Visual seed multiplier", "Swell period", "Flicker period", "Flicker seed multiplier"],
    "pulse":["Base brightness", "Swell gain", "Flicker gain"],
    "particles":["Sparks per actor (max 12)", "Minimum lifetime", "Lifetime seed shift", "Lifetime variation mask"],
    "hot":["Red", "Green", "Blue", "Opacity"],
    "cool":["Red", "Green", "Blue", "Opacity"],
    "strike-motion":["Shaft share", "Contact start radius", "Contact expansion", "Contact height scale", "Travel along path"],
    "shaft-motion":["Shaft jitter", "Previous jitter share", "Vertical jitter"],
    "burst-motion":["Burst start radius", "Burst expansion", "Burst height scale", "Impact Y offset"],
    "strike-shape":["Width", "Width at birth bonus", "Reach", "Reach at birth bonus"],
    "burst-shape":["Width", "Width at birth bonus", "Reach", "Reach at birth bonus"],
    "staff-radius":["Staff spill radius", "Staff core radius"],
    "ribbon":["Corona width", "Filament width", "Endpoint taper"],
    "corona":["Red", "Green", "Blue", "Opacity"],
    "filament":["Red", "Green", "Blue", "Opacity"],
    "path-counts":["Pose 1 joints", "Pose 2 joints", "Pose 3 joints", "Pose 4 joints", "Pose 5 joints", "Pose 6 joints", "Pose 7 joints", "Pose 8 joints"],
    "path":["Origin X", "Origin Y", "Row step", "Joint 1 X", "Joint 2 X", "Joint 3 X", "Joint 4 X", "Joint 5 X", "Joint 6 X", "Joint 7 X", "Joint 8 X", "Joint 9 X", "Joint 10 X", "Joint 11 X", "Joint 12 X", "Joint 13 X", "Joint 14 X", "Joint 15 X", "Joint 16 X", "Joint 17 X", "Joint 18 X", "Joint 19 X", "Joint 20 X", "Joint 21 X", "Joint 22 X", "Joint 23 X", "Joint 24 X", "Joint 25 X"],
    "style":["Minimum radius X", "Minimum radius Y", "Pose width scale", "Pose height scale", "Inner ring", "Middle ring", "Outer ring", "Centre red", "Centre green", "Centre blue", "Centre opacity", "Inner red", "Inner green", "Inner blue", "Inner opacity", "Middle red", "Middle green", "Middle blue", "Middle opacity", "Outer red", "Outer green", "Outer blue", "Outer opacity", "Flare", "Rise", "Axis X", "Axis Y", "Lift X", "Lift Y"]
  };
  const glowLabels={
    "components":["Light 1 + embers 2"],
    "mode":["0: matching tiles, 1: placed lights"],
    "map-rule":["Top metatile", "Bottom metatile", "Require bottom tile (0/1)", "Scan near camera (0/1)"],
    "anchor":["Tile anchor X", "Tile anchor Y"],
    "geometry":["Left", "Top", "Right", "Bottom"],
    "source-count":["Placed light count (max 16)"],
    "source":["World X", "World Y", "Stable identity"],
    "clock":["Animation speed", "Visual seed multiplier", "Swell period", "Flicker period", "Flicker seed multiplier"],
    "pulse":["Base brightness", "Swell gain", "Flicker gain"],
    "particles":["Embers per light (max 7)", "Minimum lifetime", "Lifetime seed shift", "Lifetime variation mask"],
    "particle-motion":["Side drift", "Emission width", "Birth Y", "Vertical travel", "Vertical acceleration"],
    "particle-shape":["Width", "Width at birth bonus", "Streak length", "Streak growth"],
    "particle-hot":["Red", "Green", "Blue", "Opacity"],
    "particle-cool":["Red", "Green", "Blue", "Opacity"],
    "body-offset":["Core offset X", "Core Y"],
    "radius":["Radius X", "Radius Y"],
    "rings":["Inner radius share", "Middle radius share", "Outer radius share"],
    "color":["Red", "Green", "Blue", "Opacity"],
    "flame":["Flare", "Rise", "Axis X", "Axis Y", "Lift X", "Lift Y"]
  };
  let definitionKey='';
  function refreshDefinition(record) {
    const key=selected+JSON.stringify(record);
    if(key===definitionKey)return;definitionKey=key;
    const holder=$('#effectDefinitionFields');holder.replaceChildren();
    const groups=new Map();
    for(const [property,value] of Object.entries(record)) {
      if(property==='enabled')continue;
      const surface=Object.values(surfaceNames).find(([name])=>selected.includes(':'+name+':'))?.[1];
      if(surface!==undefined){
        if(property==='kind'||(property.startsWith('heat')&&surface!==1)||(property.startsWith('molten-')&&surface!==0)||
          ((property.startsWith('cloud-')||property.startsWith('tier-'))&&surface!==4)||
          (['spill','body'].includes(property)&&surface===4)||(property==='segments'&&surface!==1)||
          (property==='map-rule'&&(surface>2||Number(record.mode)===1))||
          (['scan','anchor','bounds','projection','identity'].includes(property)&&Number(record.mode)===1&&property!=='projection'&&property!=='identity')||
          (property.startsWith('source-')&&property!=='source-count'&&Number(property.slice(7))>Number(record['source-count'])))continue;
      }
      const type=property.split('-')[0],moon=selected.includes(':moon-field:'),marsh=selected.includes(':marsh-field:'),castle=selected.includes(':castle-field:'),glow=selected.includes(':glow-field:'),arc=/:(trap|bolt|centaur)-field:/.test(selected),projectile=/:(fireball|orb|jungle-fire|lava-fire)-field:/.test(selected),category=
        (arc||projectile||surface!==undefined)?(['components','receivers','clock','pulse'].includes(property)?'Source':type):
        glow?(['components','mode','map-rule','anchor','geometry','receivers'].includes(property)?'Source':type==='source'?'Placed lights':type):
        castle?(property==='components'?'Source':type==='source'?'Openings and lights':type):
        marsh?(property==='components'?'Source':type):
        moon&&['anchor','components'].includes(property)?'Source':moon&&['cloud','reflection','wave'].includes(type)?type:/^(low|middle|ray|fan|profile|witness|pool|fall|wet|contour|ambient|area|grit|tower)-\d+$/.test(property)?type:
        /^(mote|cluster|leaf)-/.test(property)?type:'Field and motion';
      let group=groups.get(category);
      if(!group) {group=document.createElement('details');group.open=['Source','low','middle','ray','fan','profile','pool','fall','wet','ambient','area','grit','tower'].includes(category);
        const summary=document.createElement('summary');summary.textContent=category[0].toUpperCase()+category.slice(1);group.append(summary);holder.append(group);groups.set(category,group);}
      const row=document.createElement('div');row.className='recipe-vector';
      const title=document.createElement('strong');title.textContent=property.replaceAll('-',' ');row.append(title);
      if(property==='receivers') {
        const masks=value.split(/\s+/).map(Number);
        for(let target=0;target<((glow||arc||projectile||surface!==undefined)?1:2);++target) {
          const names=(glow||arc||projectile||surface!==undefined||target)?[[8,'Light: preserve original receivers'],[1,'Light scenery'],[2,'Light player'],[4,'Light enemies & objects']]:[[1,'Dim scenery'],[2,'Dim player'],[4,'Dim enemies & objects']];
          for(const [bit,name] of names){const label=document.createElement('label'),input=document.createElement('input');input.type='checkbox';label.className='recipe-component';
            input.checked=!!(masks[target]&bit);input.setAttribute('aria-label',name);
            input.onchange=()=>{const next=[...masks];next[target]=bit===8?(input.checked?8:1):input.checked?(masks[target]&7)|bit:(masks[target]&7)&~bit;
              editEmitter(selected,{receivers:next.join(' ')},'edit field receivers');};
            const caption=document.createElement('span');caption.textContent=name;label.append(input,caption);row.append(label);}
        }
        group.append(row);continue;
      }
      if((moon||marsh||castle||glow||arc||projectile||surface!==undefined)&&property==='components') {
        for(const [bit,name] of (surface!==undefined?[[1,'Glow / cloud volume'],[2,'Particles']]:projectile?[[1,'Fireball lighting'],[2,'Trail particles']]:arc?[[1,'Arc lighting'],[2,'Arc sparks']]:glow?[[1,'Flame and spill light'],[2,'Ember particles']]:castle?[[1,'Window and torch light'],[2,'Supported floor haze'],[4,'Sky moonlight'],[8,'Moat water']]:marsh?[[1,'Foreground water and ripples'],[2,'Shoreline mist'],[4,'Wet timber light'],[8,'Drips and insects']]:[[1,'Moon rays'],[2,'Distant reflections'],[4,'Cloud veil and light modulation'],[8,'Wave caps following water rows']])) {
          const label=document.createElement('label'),input=document.createElement('input');input.type='checkbox';label.className='recipe-component';
          input.checked=!!(Number(value)&bit);input.setAttribute('aria-label',name);
          input.onchange=()=>editEmitter(selected,{components:input.checked?Number(value)|bit:Number(value)&~bit},'toggle field component');
          const caption=document.createElement('span');caption.textContent=name;label.append(input);label.append(caption);row.append(label);
        }
        group.append(row);continue;
      }
      if(surface!==undefined&&property==='source-count'){
        const count=Number(value),limit=[16,4,14,1,1][surface];
        const add=document.createElement('button');add.textContent='Add surface source';add.disabled=count>=limit;
        add.onclick=()=>{const values=(record[count?'source-'+count:'bounds']||'-16 -16 16 16').split(/\s+/).map(Number);
          const used=new Set(Array.from({length:count},(_,i)=>Number(record['source-'+(i+1)].split(/\s+/)[6])));
          let id=1;while(used.has(id))++id;
          const point=count?[values[0]+32,values[1],...values.slice(2,6),id]:[actor.x,actor.y,...values,id];
          editEmitter(selected,{mode:'1','source-count':String(count+1),['source-'+(count+1)]:point.join(' ')},'add surface source');};
        const remove=document.createElement('button');remove.textContent='Remove last surface source';remove.disabled=!count;
        remove.onclick=()=>editEmitter(selected,{'source-count':String(count-1)},'remove surface source');
        row.append(add,remove);group.append(row);continue;
      }
      if(surface===1&&property==='heat'){
        const hint=document.createElement('p');hint.className='hint';hint.textContent='Refraction runs over the composited world in flat game presentation. The diorama preview shows lava glow and sparks, but does not run this flat-only pass.';group.append(hint);
      }
      if(surface===0&&property==='molten-surface'){
        const hint=document.createElement('p');hint.className='hint';hint.textContent='Moving cooling crust, hot seams and bubbles stay inside each lava source. Molten detail uses alpha draw layer 8; receiver lighting is routed separately. Surface height is measured upward from the middle of the captured lava rectangle. Set seam strength and bubble sites to zero for the original glow and embers.';group.append(hint);
      }
      const values=String(value).trim().split(/\s+/);
      const labels=(surface!==undefined?(property==='motion'?surfaceMotionLabels[surface]:property==='map-rule'?surfaceRuleLabels[surface]:surfaceLabels[property]||surfaceLabels[/^source-\d+$/.test(property)?'source':/^tier-\d+$/.test(property)?'tier':['spill','body'].includes(property)?'style':''])||arcLabels[property]:null)||(projectile?(projectileLabels[property]||(['spill','body'].includes(property)?projectileLabels.style:null)||arcLabels[property]):null)||(arc?(arcLabels[property]||arcLabels[/^path-\d+$/.test(property)?'path':/-(spill|body)$/.test(property)?'style':'']):null)||(glow?(glowLabels[property]||glowLabels[/^source-\d+$/.test(property)?'source':/-(centre|ring-\d)$/.test(property)?'color':/-(radius|rings|flame)$/.test(property)?property.split('-').at(-1):'']):null)||({dimming:['Scenery darkness (0–1)'],'dimming-ramp':['World X','World Y','Ramp width','Ramp height']}[property])||(castle?(castleLabels[property]||(/^source-\d+$/.test(property)?castleLabels.source:null)||(property.startsWith('moon-')?(moonLabels[property.slice(5)]||moonLabels[property.slice(5).split('-')[0]]):null)):null)||(marsh?(marshLabels[property]||marshLabels[/^(span|material|witness)-\d+$/.test(property)?type:'']):null)||(selected.includes(':moon-field:')?(moonLabels[property]||moonLabels[/^(low|middle)-\d+$/.test(property)?type:'']):null)||(selected.includes(':atmosphere-field:')?(atmosphereLabels[property]||atmosphereLabels[/^(ambient|area|grit|tower)-\d+$/.test(property)?type:'']):null)||definitionLabels[property]||definitionLabels[/-color$/.test(property)?'color':/^(low|middle|ray|fan|profile|witness|pool|fall|wet|contour|ambient|area|grit|tower)-\d+$/.test(property)?type:''];
      values.forEach((number,index)=>{
        if(surface!==undefined&&labels&&index>=labels.length)return;
        const label=document.createElement('label');label.textContent=labels?.[index]|| (type==='contour'?`Column ${index-16}`:values.length===1?'Value':`Point ${Math.floor(index/2)+1} ${index%2?'Y':'X'}`);
        const input=document.createElement('input');input.type='number';input.step='any';
        // Display useful precision. Untouched values retain the codec's exact
        // round-trip representation; opening the inspector never rounds data.
        input.value=String(Number(Number(number).toPrecision(6)));
        input.setAttribute('aria-label',`${property} ${label.textContent}`);
        resetControl(input,[property],index);
        input.onchange=()=>{const parts=String(records.get(selected)?.[property]??value).trim().split(/\s+/);parts[index]=input.value;
          editEmitter(selected,{[property]:parts.join(' ')},'edit complete field definition');};
        label.append(input);row.append(label);
      });group.append(row);
    }
  }
  function createDefinition(x=0,y=0,bundled=true) {
    try {
      const id=fieldPrefix()+'ray-field:46000000',next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.rayFieldDefinition().trim().split('\n')) {
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      if(!bundled) {
        if([...records.keys()].some(key=>key.startsWith(fieldPrefix()+'ray-field:'))) {
          status('This room already has a grouped field. Add or edit its rays and origin groups within that field.');return false;
        }
        record.dimensions='0 0 0 0';record['witness-count']='0';delete record['witness-1'];delete record['witness-2'];
        const [scaleX,scaleY]=SharedRoomPreview.rayFieldMapScale();
        record['origin-y']=String(Math.round(y/scaleY));
        record.bounds=`0 0 ${room.bg[1]?.pagesWide*256||room.bg[0].pagesWide*256} ${room.bg[1]?.pagesHigh*256||room.bg[0].pagesHigh*256}`;
        const delta=Math.round(x/scaleX)-Number(record['ray-1'].split(/\s+/)[0]);
        for(const key of Object.keys(record).filter(k=>/^(ray|fan)-\d+$/.test(k))) {
          const parts=record[key].split(/\s+/);parts[0]=String(Number(parts[0])+delta);record[key]=parts.join(' ');
        }
      }
      next.set(id,record);
      if(!change(encode(next),'extract complete ray-field definition'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();return true;
    }catch(error){status(error.message);return false;}
  }
  function createWaterDefinition(x=0,y=0,bundled=true) {
    try {
      const id=fieldPrefix()+'water-field:c2000200',next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.waterFieldDefinition().trim().split('\n')) {
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      if(!bundled) {
        if([...records.keys()].some(key=>key.startsWith(fieldPrefix()+'water-field:'))) {
          status('This room already has a linked surface field. Add or edit its contacts within that field.');return false;
        }
        const [sx,sy]=SharedRoomPreview.waterFieldMapScale();
        const centre=Math.max(24,Math.round(x/sx)),floor=Math.max(0,Math.round(y/sy));
        record.dimensions='0 0 0 0';record.counts='1 0 0 0';record.components='1';
        record['pool-1']=`${centre-24} ${centre+24} ${floor}`;
        record.bounds=`0 0 ${room.bg[1]?.pagesWide*256||room.bg[0].pagesWide*256} ${room.bg[1]?.pagesHigh*256||room.bg[0].pagesHigh*256}`;
      }
      next.set(id,record);
      if(!change(encode(next),'extract complete water-field definition'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();return true;
    }catch(error){status(error.message);return false;}
  }
  function createSurfaceDefinition(name,x=0,y=0,bundled=true) {
    try {
      const entry=Object.values(surfaceNames).find(([field])=>field===name);if(!entry)return false;
      const id=fieldPrefix()+name+':00000000';
      if(records.has(id)){selected=id;refreshInspector();return true;}
      const next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.surfaceFieldDefinition(entry[1]).trim().split('\n')){
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      if(!bundled){record.mode='1';record['source-count']='1';record['source-1']=[Math.round(x),Math.round(y),...record.bounds.split(/\s+/),1].join(' ');}
      next.set(id,record);
      if(!change(encode(next),'extract complete lava / waterfall definition'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();return true;
    }catch(error){status(error.message);return false;}
  }
  function createProjectileDefinition(name) {
    try {
      const index=['fireball-field','orb-field','jungle-fire-field','lava-fire-field'].indexOf(name);if(index<0)return false;
      const id=fieldPrefix()+name+':00000000';
      if(records.has(id)){selected=id;refreshInspector();return true;}
      const next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.projectileFieldDefinition(index).trim().split('\n')){
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      next.set(id,record);
      if(!change(encode(next),'extract complete fireball response'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();
      status('This changes the room’s recognized fireball response. Use the existing event preview to inspect it; adding a response does not place or spawn an actor.');return true;
    }catch(error){status(error.message);return false;}
  }
  function createArcDefinition(name) {
    try {
      const index=['trap-field','bolt-field','centaur-field'].indexOf(name);if(index<0)return false;
      const id=fieldPrefix()+name+':00000000';
      if(records.has(id)){selected=id;refreshInspector();return true;}
      const next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.arcFieldDefinition(index).trim().split('\n')){
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      next.set(id,record);
      if(!change(encode(next),'extract complete lightning response'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();
      status('This changes the room’s recognized lightning response. Use the existing event preview to inspect it; adding a response does not place or spawn a trap or boss.');return true;
    }catch(error){status(error.message);return false;}
  }
  function createGlowDefinition(x=0,y=0,bundled=true) {
    try {
      const existing=[...records.keys()].find(key=>key.startsWith(fieldPrefix()+'glow-field:'));
      if(existing){selected=existing;refreshInspector();return true;}
      const id=fieldPrefix()+'glow-field:54000000',next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.glowFieldDefinition().trim().split('\n')) {
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      if(!bundled) {
        record.mode='1';record['source-count']='1';
        record['source-1']=`${Math.round(x)} ${Math.round(y)} 0`;
      }
      next.set(id,record);
      if(!change(encode(next),'extract complete flame and glow definition'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();return true;
    }catch(error){status(error.message);return false;}
  }
  function createCastleDefinition(x=0,y=0,bundled=true) {
    try {
      const existing=[...records.keys()].find(key=>key.startsWith(fieldPrefix()+'castle-field:'));
      if(existing){selected=existing;refreshInspector();return true;}
      const id=fieldPrefix()+'castle-field:'+((0xca000000+room.map)>>>0).toString(16),next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.castleFieldDefinition().trim().split('\n')) {
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      if(!bundled) {
        // Start with one ordinary narrow opening. Witnesses remain explicit:
        // choose matching art in the dialog, or stamp the matching opening.
        const px=Math.max(16,Math.round(x/16)*16),py=Math.max(16,Math.round(y/16)*16);
        record.dimensions='0 0 0 0';record['witness-count']='0';record.components='1';record.style='0';record['source-count']='1';
        record['source-1']=`0 ${px} ${py} 68 76 ${px+8} ${py+2} 32 4 16 0 ${px-16} ${px+32} ${py+72} ${py+40} 84 0 0`;
      }
      next.set(id,record);
      if(!change(encode(next),'extract complete castle definition'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();return true;
    }catch(error){status(error.message);return false;}
  }
  function createMarshDefinition(x=112,y=62,bundled=true) {
    try {
      const existing=[...records.keys()].find(key=>key.startsWith(fieldPrefix()+'marsh-field:'));
      if(existing){selected=existing;refreshInspector();return true;}
      const id=fieldPrefix()+'marsh-field:b1000000',next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.marshFieldDefinition().trim().split('\n')) {
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      if(!bundled) {
        const w=room.bg[0].pagesWide*256,h=room.bg[0].pagesHigh*256;
        const floor=Math.max(0,Math.min(h-32,Math.floor(y/16)*16));
        const left=Math.max(0,Math.min(w-64,Math.floor(x/16)*16-32));
        record.dimensions='0 0 0 0';record['witness-count']='0';record['detail-witness-count']='0';
        record['span-count']='1';record['span-1']=`${left} ${left+64}`;
        record.bounds=`0 ${Math.max(0,floor-480)} ${w} ${floor+32}`;
        record.surface=`${floor} ${floor+8} ${floor+31} 6`;
        record['insect-region']=`24 ${Math.max(0,floor-30)} 14`;
        const under=record['under-mist'].split(/\s+/);under[0]=String(Math.max(0,floor-128));under[1]=String(Math.max(0,floor-8));record['under-mist']=under.join(' ');
      }
      next.set(id,record);
      if(!change(encode(next),'extract linked marsh definition'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();return true;
    }catch(error){status(error.message);return false;}
  }
  function createMoonDefinition(x=112,y=62,bundled=true) {
    try {
      const existing=[...records.keys()].find(key=>key.startsWith(fieldPrefix()+'moon-field:'));
      if(existing){selected=existing;refreshInspector();return true;}
      const id=fieldPrefix()+'moon-field:b1000002',next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.moonFieldDefinition().trim().split('\n')) {
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      if(!bundled) {
        record.dimensions='0 0 0 0';record['witness-count']='0';
        // BG1 cannot identify a point on a static sky. Use the BG2 map centre
        // when adding here; adding on BG2 uses the exact clicked coordinate.
        record.anchor=bgIndex===1?`${x} ${y}`:`${room.bg[1]?.pagesWide*128||128} ${room.bg[1]?.pagesHigh*128||128}`;
      }
      next.set(id,record);
      if(!change(encode(next),'extract linked moon definition'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();return true;
    }catch(error){status(error.message);return false;}
  }
  function createAtmosphereDefinition(x=0,y=0,bundled=true) {
    try {
      const existing=[...records.keys()].find(key=>key.startsWith(fieldPrefix()+'atmosphere-field:'));
      if(!bundled&&existing){status('This room already has an atmosphere field. Add sources and regions in that field.');return false;}
      const presetRoom=room.group===1&&room.map>=2&&room.map<=4?room.map:2;
      const id=existing||fieldPrefix()+`atmosphere-field:c200${presetRoom.toString(16).padStart(2,'0')}00`;
      const next=new Map(records),record={enabled:'1'};
      for(const line of SharedRoomPreview.atmosphereFieldDefinition().trim().split('\n')) {
        const at=line.indexOf('=');record[line.slice(0,at)]=line.slice(at+1);
      }
      if(!bundled) {
        record.dimensions='0 0 0 0';record.counts='1 1 0 0 0';record.components='5';
        record['ambient-1']=`${x} ${y} 128 96 0 0.5 0.5 0`;
        record['area-1']=`${x-128} ${y-96} ${x+128} ${y+96}`;
        record.bounds=`0 0 ${room.bg[0].pagesWide*256} ${room.bg[0].pagesHigh*256}`;
      }
      next.set(id,record);
      if(!change(encode(next),'extract complete atmosphere definition'))return false;
      selected=id;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();return true;
    }catch(error){status(error.message);return false;}
  }
  $('#effectDefinitionCreate').onclick=()=>{
    const name=sources.find(s=>s.id===selected)?.name;
    return surfaceNames[name]?createSurfaceDefinition(surfaceNames[name][0]):projectileNames[name]?createProjectileDefinition(projectileNames[name][0]):arcNames[name]?createArcDefinition(arcNames[name][0]):['torch','wall-torch'].includes(name)?createGlowDefinition():castleNames.includes(name)?createCastleDefinition():marshNames.includes(name)?createMarshDefinition():moonNames.includes(name)?createMoonDefinition():atmosphereNames.includes(name)?createAtmosphereDefinition():waterNames.includes(name)?createWaterDefinition():createDefinition();
  };
  function emitterPrefix() {
    return ['emitter',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),terrainProfile].join(':')+':';
  }
  function regionDefinition(kind) {
    const owner=EffectRegions.owner(kind);if(!owner)return null;
    const key=sceneKey(room)+':'+terrainProfile+':'+owner;
    if(regionDefinitions.has(key))return regionDefinitions.get(key);
    const existing=[...records].find(([id])=>id.startsWith(fieldPrefix()+owner+'-field:'));
    let definition;
    if(existing)definition={id:existing[0],record:existing[1]};
    else try {
      const record={enabled:'1'};
      for(const line of SharedRoomPreview[owner+'FieldDefinition']().trim().split('\n')) {
        const at=line.indexOf('=');if(at>0)record[line.slice(0,at)]=line.slice(at+1);
      }
      const source=owner==='castle'?0xca000000+room.map:owner==='moon'?0xb1000002:
          owner==='marsh'?0xb1000000:0xc2000000+((room.group===1&&room.map>=2&&room.map<=4?room.map:2)<<8);
      definition={id:fieldPrefix()+owner+'-field:'+source.toString(16).padStart(8,'0'),record};
    } catch {return null;}
    regionDefinitions.set(key,definition);return definition;
  }
  function editRegion(source,patch,label,index=0) {
    try {
      const definition=regionDefinition(source.name);if(!definition)return false;
      const sx=bgIndex===1?1:source.mapScaleX||1,sy=bgIndex===1?1:source.mapScaleY||1,draft={};
      for(const key of ['x','y','width','height'])if(patch[key]!==undefined)
        draft[key]=Number(patch[key])/(['x','width'].includes(key)?sx:sy);
      const updates=EffectRegions.patch(source.name,definition.record,index,draft),next=new Map(records);
      if(!resetRecords.has(definition.id))resetRecords.set(definition.id,{...definition.record});
      next.set(definition.id,{...definition.record,...updates});
      if(!change(encode(next),label))return false;
      selected=source.id;selectedGuideIndex=index;refreshInspector();draw();return true;
    } catch(error){status(error.message);return false;}
  }
  function mapEmitters(layer=bgIndex) {
    const field=[...records].find(([id])=>id.startsWith(fieldPrefix()+'ray-field:'))?.[1];
    const water=[...records].find(([id])=>id.startsWith(fieldPrefix()+'water-field:'))?.[1];
    const atmosphere=[...records].find(([id])=>id.startsWith(fieldPrefix()+'atmosphere-field:'))?.[1];
    const castle=[...records].find(([id])=>id.startsWith(fieldPrefix()+'castle-field:'))?.[1];
    const marshEntry=[...records].find(([id])=>id.startsWith(fieldPrefix()+'marsh-field:'));
    const marsh=marshEntry?.[1];
    const moon=[...records].find(([id])=>id.startsWith(fieldPrefix()+'moon-field:'));
    const native=sources.filter(s=>s.name!=='moon-field'&&s.name!=='marsh-field'&&(!moon||s.name!=='moonlight')&&!s.emitter&&!s.actor&&(s.movable||s.guides?.length)&&
      (moonNames.includes(s.name)||s.name==='castle-sky'?layer===1:layer===0||s.anchor===1&&!s.member)).flatMap(original=>{
      const s=layer===1?{...original,baseX:original.anchorX,baseY:original.anchorY,mapScaleX:1,mapScaleY:1}:original;
      const r=records.get(s.id)||{},cap=nativeCapabilities(s);
      const sx=s.mapScaleX||1,sy=s.mapScaleY||1;
      const entry={id:s.id,kind:s.name,angle:Number(r.angle??0),rotatable:cap.angle,guide:s.guide,scaleX:sx,scaleY:sy,lengthScale:Number(r['length-scale']??1),native:true,movable:s.movable,family:!s.movable,layer,
        anchorY:s.member&&['forest-canopy','forest-forward','cave-drips','cave-sheen'].includes(s.name)?0:.5,
        x:(s.baseX+Number(r['offset-x']??0))*sx,y:(s.baseY+Number(r['offset-y']??0))*sy,
        width:s.movable?s.baseWidth*Number(r['width-scale']??1)*sx:24,
        height:s.movable?s.baseHeight*Number(r['length-scale']??1)*sy:24,
        baseX:s.baseX*sx,baseY:s.baseY*sy,baseWidth:s.baseWidth*sx,baseHeight:s.baseHeight*sy,
        offsetLimitX:512*sx,offsetLimitY:512*sy,
        resizeWidth:cap.width,resizeHeight:cap.length,enabled:r.enabled!=='0'&&records.get(s.parent)?.enabled!=='0'&&
          (!castleNames.includes(s.name)||!castle||castle.enabled!=='0'&&(Number(castle.components)&({'castle-light':1,'castle-mist':2,'castle-sky':4,'castle-water':8}[s.name]))!==0)&&
          (!marshNames.includes(s.name)||!marsh||marsh.enabled!=='0'&&(Number(marsh.components)&({'blood-water':1,'blood-mist':2,'wet-timber':4,'marsh-air':8}[s.name]))!==0)&&
          (!moonNames.includes(s.name)||!moon||moon[1].enabled!=='0'&&(Number(moon[1].components)&({'moonlight':1,'moon-reflection':10,'moon-cloud':4}[s.name]))!==0)&&
          (!atmosphereNames.includes(s.name)||atmosphere?.enabled!=='0'&&(!atmosphere||(Number(atmosphere.components)&({'temple-dust':1,'tower-window':2,'cave-light':4,'temple-grit':8,'floor-mist':16}[s.name]))!==0))&&
          (!['forest-canopy','forest-forward','forest-leaves'].includes(s.name)||field?.enabled!=='0'&&
            (!field||(Number(field.components)&(s.name==='forest-canopy'?3:s.name==='forest-leaves'?4:8))!==0))&&
          (!['cave-water','cave-drips','cave-mist','cave-sheen'].includes(s.name)||water?.enabled!=='0'&&
            (!water||(Number(water.components)&({'cave-water':1,'cave-drips':2,'cave-mist':4,'cave-sheen':8}[s.name]))!==0))};
      if(!s.guides?.length)return [entry];
      // Several terrain contacts can edit one bounded field. Keep the guides
      // at those contacts and say explicitly that their settings are linked.
      const definition=EffectRegions.owner(s.name)&&regionDefinition(s.name);
      if(definition)return s.guides.map((g,ordinal)=>{
        const guideIndex=g.index??ordinal;
        const b=EffectRegions.bounds(s.name,definition.record,guideIndex);
        const limits=EffectRegions.limits(s.name,definition.record,guideIndex);
        return {...entry,guideIndex,region:true,native:false,family:false,movable:true,
          x:b.x*sx,y:b.y*sy,width:b.width*sx,height:b.height*sy,anchorY:b.anchorY??.5,
          ...limits,resizeWidth:true,minWidth:(limits.minWidth??4)*sx,maxWidth:limits.maxWidth*sx,maxHeight:limits.maxHeight*sy};
      });
      return s.guides.map((g,ordinal)=>({...entry,guideIndex:g.index??ordinal,x:g.x*sx,y:g.y*sy,width:g.width*sx,height:g.height*sy,
        label:s.name==='wet-timber'?'Wet timber · terrain contact':s.name==='marsh-air'?'Bank insects / drips · linked field':
          s.name==='blood-mist'?'Shoreline mist · linked span':s.name==='blood-water'?'Water light · linked span':
          s.name==='floor-mist'?'Floor mist · supported span':
          s.name==='moon-cloud'?'Moon cloud · BG2':s.name==='moon-reflection'?'Water reflections · BG2':s.name==='moonlight'?'Moon source · BG2':
          `${s.name.replaceAll('-',' ')} · ${s.name==='castle-sky'?'BG2 source':'linked region'}`}));
    });
    // A newly placed marsh in another room has no native catalogue parent.
    // Keep its editable field reachable at its declared shoreline as well.
    if(layer===0&&marsh&&!native.some(e=>marshNames.includes(e.kind))) {
      const surface=marsh.surface.split(/\s+/).map(Number);
      for(let i=1;i<=Number(marsh['span-count']);++i) {
        const [left,right]=marsh[`span-${i}`].split(/\s+/).map(Number);
        native.push({id:marshEntry[0],layer,kind:'marsh-field',family:true,movable:false,
          x:(left+right)/2,y:surface[0],width:right-left,height:32,anchorY:0,
          label:'Marsh field · linked shoreline',enabled:marsh.enabled!=='0'&&Number(marsh.components)!==0});
      }
    }
    if(layer===1&&moon) {
      const [id,r]=moon,[x,y]=r.anchor.split(/\s+/).map(Number);
      native.push({id,layer,kind:'moon-field',movable:true,resizeWidth:false,resizeHeight:false,
        x,y,width:24,height:24,enabled:r.enabled!=='0'&&Number(r.components)!==0});
    }
    return native.concat([...records,...sources.filter(s=>s.nativeExposure).map(s=>[s.id,s.record])]
      .map(([id,r])=>[id,id.split(':')[4]==='cloud-light'?{...cloudDefaults,...r}:r]).filter(([id,r])=>id.startsWith(emitterPrefix())&&
      Number((r.anchor??'bg1')!=='bg1')===layer).map(([id,r])=>({id,layer,
      label:id.includes(':exposure:')?'Light box':undefined,brightness:Number(r.brightness??(1-Number(r.intensity??.35))),fade:['left','top','right','bottom'].map(edge=>Number(r['fade-'+edge]??0)),kind:id.split(':')[4],seed:Number(r.seed??0),edgePosition:Number(r['edge-position']??.15),echoPosition:Number(r['echo-position']??.8),angle:Number(r.angle??0),fan:Number(r.fan??36),strands:Number(r.strands??8),amplitude:Number(r.amplitude??2),carry:Number(r.carry??0),drift:Number(r.drift??0),rotatable:['light-fan','cloud-light','light-gradient','foliage-shadow','canopy-light'].includes(id.split(':')[4]),attached:!!r['actor-target'],actorX:r['actor-target']?actor.x:0,actorY:r['actor-target']?actor.y:0,movable:true,resizeWidth:id.split(':')[4]!=='torch',resizeHeight:id.split(':')[4]!=='torch',x:Number(r.x??0)+(r['actor-target']?actor.x:0),y:Number(r.y??0)+(r['actor-target']?actor.y:0),points:r.points??(id.split(':')[4]==='wet-contour'?'-32,0 32,0':id.split(':')[4]==='particle-path'?'0,-24 0,24':''),width:Number(r.width??(['foliage-shadow','canopy-light'].includes(id.split(':')[4])?512:96)),
      height:Number(r.height??(id.split(':')[4]==='canopy-light'?256:id.split(':')[4]==='foliage-shadow'?384:64)),enabled:r.enabled!=='0'})));
  }
  function selectEmitter(id,additive=false,guideIndex) {
    if(!sources.some(s=>s.id===id)&&(!records.has(id)||!id.startsWith(emitterPrefix())))return false;
    if(!additive)mapSelection.clear();
    if(additive&&mapSelection.has(id))mapSelection.delete(id);else mapSelection.add(id);
    selected=id;selectedGuideIndex=guideIndex;rebuildSources(sources.filter(s=>!s.emitter));refreshInspector();draw();return true;
  }
  function copyEffects() {
    const entries=mapEmitters(),ids=mapSelection.size?[...mapSelection]:[selected];
    const copied=[];
    for(const id of ids) {
      const entry=entries.find(e=>e.id===id);if(!entry)continue;
      const source=sources.find(s=>s.id===id),r=records.get(id)||source?.record||{};
      if(id.startsWith('emitter:'))copied.push({kind:entry.kind,record:{...r},extract:source?.nativeExposure?id:null,x:entry.x,y:entry.y});
      else if(entry.kind==='wall-torch'&&!entry.family)copied.push({kind:'torch',x:entry.x,y:entry.y,
        record:{x:String(entry.x),y:String(entry.y),width:'32',height:'32',color:r.color??'ffffff',intensity:r.intensity??String(sourceDefault('wall-torch','intensity')),reach:r.reach??String(sourceDefault('wall-torch','reach')),
          ...(r.illumination!==undefined?{illumination:r.illumination}:{}),placement:'playfield',enabled:r.enabled??'1',
          ...Object.fromEntries(Object.entries(r).filter(([key])=>key.startsWith('light-')))}});
      else {mapStatus('Copy placed emitters or individual torches. A complete room definition is edited as one field.');return false;}
    }
    if(!copied.length){mapStatus('Select an effect marker to copy first.');return false;}
    effectClipboard={entries:copied,layer:bgIndex,x:copied[0].x,y:copied[0].y};clipboardActive=true;
    mapStatus(`Copied ${copied.length} effect${copied.length===1?'':'s'}. Right-click the destination and choose Paste effects here, or Ctrl/Cmd-V then click to place.`);return true;
  }
  function pasteEffects(x,y) {
    if(!clipboardActive||!effectClipboard){mapStatus('Copy one or more effect markers first.');return false;}
    if(bgIndex!==effectClipboard.layer){mapStatus(`This selection uses BG${effectClipboard.layer+1}. Switch to that map before pasting.`);return false;}
    const next=new Map(records),ids=[];
    for(const entry of effectClipboard.entries){let id;
      if(entry.extract&&sources.some(s=>s.id===entry.extract&&s.nativeExposure))next.set(entry.extract,{...entry.record});
      do{id=newEmitterId(entry.kind);}while(next.has(id));
      const record={...entry.record};
      // A binding duplicates its selector and actor-local offsets. Map placement
      // changes only sources that actually live in map coordinates.
      if(!record['actor-target']){record.x=String(Math.round(x+entry.x-effectClipboard.x));record.y=String(Math.round(y+entry.y-effectClipboard.y));}
      next.set(id,record);ids.push(id);
    }
    if(!change(encode(next),`paste ${ids.length} effects`))return false;
    mapSelection.clear();for(const id of ids)mapSelection.add(id);selected=ids[0];
    $('#effectSource').value=selected;$('#effectGuides').checked=true;
    refreshInspector();draw();
    const attached=effectClipboard.entries.every(e=>e.record['actor-target']);
    mapStatus(`Pasted ${ids.length} effect${ids.length===1?'':'s'} ${attached?'with the same actor attachment':`at BG${bgIndex+1} ${Math.round(x)}, ${Math.round(y)}`}. Ctrl/Cmd-Z undoes this paste.`);
    return true;
  }
  function clearMapSelection(){mapSelection.clear();}
  function startEffectPaste(){
    if(!clipboardActive||!effectClipboard)return false;
    EmitterMapTools.cancel();setMode('2d');brush='effectPaste';cvs.style.cursor='crosshair';$('#effectGuides').checked=true;refreshEditorFeedback();draw();
    mapStatus(`Click to place the copied effect${effectClipboard.entries.length===1?'':'s'} on BG${effectClipboard.layer+1}. The first marker lands at your click; Esc finishes.`);return true;
  }
  $('#emitterDuplicate').onclick=()=>{
    const entry=mapEmitters().find(e=>e.id===selected);if(!entry)return;
    mapSelection.clear();mapSelection.add(selected);if(copyEffects())pasteEffects(entry.x+16,entry.y);
  };
  $('#effectReset').onclick=()=>{
    const authored=selected.startsWith('emitter:');
    const next=new Map(records);next.delete(selected);
    if(change(encode(next),'reset environmental effect')&&authored&&modal)finishModal(true);
  };
  $('#effectLoad').onchange=async e=>{
    const file=e.target.files[0];if(!file)return;
    try{
      if(file.size>ActionProjectArchive.limits['action-effects.ini'])throw Error('Effect document exceeds 512 KiB.');
      if(change(await file.text(),'load effect document'))markSaved();
    }catch(error){status(error.message);}finally{e.target.value='';}
  };
  $('#effectExport').onclick=()=>{
    try {
      SharedRoomPreview.validateEffects(documentText);
      const url=URL.createObjectURL(new Blob([documentText],{type:'text/plain'}));
      const link=document.createElement('a');link.href=url;link.download='action-effects.ini';link.click();
      setTimeout(()=>URL.revokeObjectURL(url),1000);markSaved();
      status('Download requested. Place action-effects.ini beside settings.ini and restart the game.');
    }catch(error){status(error.message);}
  };
  $('#effectDocumentOpen').onclick=()=>{
    $('#effectDocumentText').value=documentText;$('#effectDocument').showModal();
  };
  $('#effectDocumentClose').onclick=()=>$('#effectDocument').close();
  $('#effectDocumentApply').onclick=()=>change($('#effectDocumentText').value,'apply effects document');
  $('#effectDocumentCopy').onclick=async()=>{
    const field=$('#effectDocumentText');field.focus();field.select();
    try{await navigator.clipboard.writeText(field.value);if(field.value===documentText)markSaved();status('Copied effects INI. Save it as action-effects.ini beside settings.ini.');}
    catch{status('Text selected. Press Ctrl/Cmd-C to copy.');}
  };
  function updateSources(api) {
    const bytes=new Uint8Array(api.memory.buffer), decoder=new TextDecoder();
    const found=[],parents=new Map();
    for(let i=0;i<api.RoomPreview_EffectCount();i++) {
      const kind=api.RoomPreview_SourceValue(i,1), p=api.RoomPreview_KindName(kind);if(!p)continue;
      let end=p;while(bytes[end])end++;
      const name=decoder.decode(bytes.subarray(p,end)),generation=api.RoomPreview_SourceValue(i,0)>>>0;
      const id=[api.RoomPreview_SourceValue(i,5)?'emitter':'source',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),
        terrainProfile,name,generation.toString(16).padStart(8,'0')].join(':');
      // One definition can produce several actor instances. Keep a single
      // editable source marker, separate from per-instance animation seeds.
      if(found.some(source=>source.id===id))continue;
      const record=records.get(id)||{},x=api.RoomPreview_SourceValue(i,2)|0,y=api.RoomPreview_SourceValue(i,3)|0;
      found.push({id,name,movable:!!api.RoomPreview_SourceValue(i,8),baseX:x-Number(record['offset-x']??0),baseY:y-Number(record['offset-y']??0),
        baseWidth:api.RoomPreview_SourceValue(i,9)/Number(record['width-scale']??1),baseHeight:api.RoomPreview_SourceValue(i,10)/Number(record['length-scale']??1),
        emitter:!!api.RoomPreview_SourceValue(i,5),reach:!!api.RoomPreview_ReachSupported(kind),
        actor:!!api.RoomPreview_SourceValue(i,7),x:api.RoomPreview_SourceValue(i,2)|0,y:api.RoomPreview_SourceValue(i,3)|0,support:api.RoomPreview_SourceValue(i,6)});
      parents.set(i,found.at(-1));
    }
    for(let i=0;i<api.RoomPreview_EffectCount();i++)for(let m=0;m<api.RoomPreview_MemberCount(i);m++) {
      const parent=parents.get(i),ordinal=api.RoomPreview_MemberValue(i,m,0),id=parent.id.replace(/^source:/,'member:').replace(/:[^:]+$/,':'+ordinal.toString(16).padStart(8,'0'));
      found.push({id,name:parent.name,parent:parent.id,member:ordinal,movable:true,angled:!!api.RoomPreview_MemberValue(i,m,5),baseX:api.RoomPreview_MemberValue(i,m,1),baseY:api.RoomPreview_MemberValue(i,m,2),
        guide:Array.from({length:5},(_,f)=>api.RoomPreview_MemberValue(i,m,f+6)||0),
        baseWidth:api.RoomPreview_MemberValue(i,m,3),baseHeight:api.RoomPreview_MemberValue(i,m,4),x:api.RoomPreview_MemberValue(i,m,1),y:api.RoomPreview_MemberValue(i,m,2)});
    }
    if(catalogueScope===sceneKey(room)) {
      const ids=new Set(catalogueSources.map(s=>s.id));
      rebuildSources(catalogueSources.concat(found.filter(s=>!ids.has(s.id))));
    } else rebuildSources(found);
  }
  function cacheExposureDefaults(api) {
    if(exposureScope!==sceneKey(room)){exposureDefaults.clear();exposureScope=sceneKey(room);}
    for(const [kind,getter] of [['atmosphere-field','RoomPreview_AtmosphereFieldText'],['castle-field','RoomPreview_CastleFieldText']]) {
      if(exposureDefaults.has(kind)||[...records.keys()].some(id=>id.startsWith(fieldPrefix()+kind+':')))continue;
      const pointer=api[getter]?.();if(!pointer)continue;
      const bytes=new Uint8Array(api.memory.buffer);let end=pointer;while(end<bytes.length&&bytes[end])end++;
      const record=Object.fromEntries(new TextDecoder().decode(bytes.subarray(pointer,end)).trim().split('\n').map(line=>{
        const at=line.indexOf('=');return [line.slice(0,at),line.slice(at+1)];
      }));
      exposureDefaults.set(kind,record);
    }
  }
  function nativeExposureSources(found) {
    const result=[];
    for(const [kind,parentName] of [['atmosphere-field','cave-light'],['castle-field','castle-light']]) {
      const parent=found.find(s=>s.name===parentName&&!s.member);if(!parent)continue;
      const existing=[...records].find(([id])=>id.startsWith(fieldPrefix()+kind+':'));
      const definition=existing?.[1]||(exposureScope===sceneKey(room)?exposureDefaults.get(kind):null);
      if(!definition||!(Number(definition.dimming)>0))continue;
      const [rx,ry,rw,rh]=(definition['dimming-ramp']||'0 0 0 0').split(/\s+/).map(Number);
      const ramp=rw>0&&rh>0,width=room.bg[0].pagesWide*256,height=room.bg[0].pagesHigh*256;
      const left=ramp?Math.max(0,rx):0,top=ramp?Math.max(0,ry):0;
      if(left>=width||top>=height)continue;
      const owner=existing?.[0]||fieldPrefix()+kind+':'+(kind==='atmosphere-field'?(0xc2000000+room.map*256)>>>0:(0xca000000+room.map)>>>0).toString(16);
      let serial=parseInt(owner.split(':').at(-1),16),id;
      do{id=emitterPrefix()+'exposure:'+(serial++>>>0).toString(16).padStart(8,'0');}while(records.has(id));
      const receiver=Number((definition.receivers||'1').split(/\s+/)[0]);
      const overrides=records.get(parent.id)||{};
      const record={x:String((left+width)/2),y:String((top+height)/2),width:String(width-left),height:String(height-top),
        brightness:String(1-Number(definition.dimming)),'fade-left':String(ramp?rw:0),'fade-top':String(ramp?rh:0),'fade-right':'0','fade-bottom':'0',
        enabled:definition.enabled==='0'||overrides.enabled==='0'||!(Number(definition.components)&(kind==='atmosphere-field'?4:1))?'0':'1'};
      for(const [target,mask] of [['scenery',1],['player',2],['enemies',4]])record['dim-'+target]=overrides['dim-'+target]??String(!!(receiver&mask)?1:0);
      result.push({id,name:'exposure',emitter:true,nativeExposure:true,owner,definition,record,x:Number(record.x),y:Number(record.y)});
    }
    return result;
  }
  function updateCatalogue(api) {
    if(catalogueScope===sceneKey(room)&&catalogueDocument===documentText)return;
    cacheExposureDefaults(api);
    const bytes=new Uint8Array(api.memory.buffer),decoder=new TextDecoder(),found=[];
    for(let i=0;i<api.RoomPreview_CatalogueCount();++i) {
      const kind=api.RoomPreview_CatalogueValue(i,1),p=api.RoomPreview_KindName(kind);if(!p)continue;
      let end=p;while(bytes[end])end++;
      const name=decoder.decode(bytes.subarray(p,end)),id=['source',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),terrainProfile,name,(api.RoomPreview_CatalogueValue(i,0)>>>0).toString(16).padStart(8,'0')].join(':');
      const movable=!!api.RoomPreview_CatalogueValue(i,6),members=api.RoomPreview_CatalogueValue(i,7);
      const source={id,name,catalogue:true,movable,members,reach:!!api.RoomPreview_ReachSupported(kind),
        anchor:api.RoomPreview_CatalogueValue(i,10),anchorX:api.RoomPreview_CatalogueValue(i,2),anchorY:api.RoomPreview_CatalogueValue(i,3),
        baseX:api.RoomPreview_CatalogueValue(i,2),baseY:api.RoomPreview_CatalogueValue(i,3),
        baseWidth:api.RoomPreview_CatalogueValue(i,4),baseHeight:api.RoomPreview_CatalogueValue(i,5),
        mapScaleX:api.RoomPreview_CatalogueValue(i,8),mapScaleY:api.RoomPreview_CatalogueValue(i,9)};
      source.guides=Array.from({length:api.RoomPreview_CatalogueGuideCount?.(i)||0},(_,g)=>({
        index:g,x:api.RoomPreview_CatalogueGuideValue(i,g,0),y:api.RoomPreview_CatalogueGuideValue(i,g,1),
        width:api.RoomPreview_CatalogueGuideValue(i,g,2),height:api.RoomPreview_CatalogueGuideValue(i,g,3)})).filter(g=>g.width>0&&g.height>0);
      if(source.guides.length){source.baseX=source.guides[0].x;source.baseY=source.guides[0].y;}
      source.x=source.baseX;source.y=source.baseY;found.push(source);
      for(let m=0;m<members;++m) {
        const value=f=>api.RoomPreview_CatalogueMemberValue(i,m,f),ordinal=value(0);
        found.push({...source,id:id.replace(/^source:/,'member:').replace(/:[^:]+$/,':'+ordinal.toString(16).padStart(8,'0')),
          parent:id,members:0,member:ordinal,movable:true,guides:[],angled:!!value(5),guide:Array.from({length:5},(_,f)=>value(f+6)||0),
          baseX:value(1),baseY:value(2),x:value(1),y:value(2),baseWidth:value(3),baseHeight:value(4),
          mapScaleX:api.RoomPreview_CatalogueValue(i,8),mapScaleY:api.RoomPreview_CatalogueValue(i,9)});
      }
    }
    catalogueScope=sceneKey(room);catalogueDocument=documentText;catalogueSources=found.slice();rebuildSources(found);
  }
  function rebuildSources(found) {
    sourceScope=sceneKey(room);
    if(catalogueScope===sceneKey(room)) {
      const ids=new Set(catalogueSources.map(s=>s.id));
      found=catalogueSources.concat(found.filter(s=>!ids.has(s.id)));
    }
    found=found.filter(s=>!s.nativeExposure&&(!s.field||records.has(s.id)));
    found.push(...nativeExposureSources(found));
    // A complete recipe owns the base ray geometry. Sparse member overrides
    // remain relative to those bases and retain their stable numbered IDs.
    const field=[...records].find(([id])=>id.startsWith(fieldPrefix()+'ray-field:'))?.[1];
    if(field) {
      const count=Number(field['ray-count']),originY=Number(field['origin-y']);
      found=found.filter(s=>!s.member||!['forest-canopy','forest-forward'].includes(s.name)||s.member<=count);
      found=found.map(s=>{
        if(!s.member||!['forest-canopy','forest-forward'].includes(s.name))return s;
        const ray=field[`ray-${s.member}`].split(/\s+/).map(Number);
        return {...s,baseX:ray[0],baseY:originY,baseWidth:ray[1]*2,x:ray[0],y:originY};
      });
    }
    const water=[...records].find(([id])=>id.startsWith(fieldPrefix()+'water-field:'))?.[1];
    if(water) {
      const counts=water.counts.split(/\s+/).map(Number);
      found=found.filter(s=>!s.member||!['cave-water','cave-drips','cave-sheen','cave-mist'].includes(s.name)||
        s.member<=(s.name==='cave-water'?counts[0]+counts[1]:s.name==='cave-mist'?counts[1]:counts[2]));
      found=found.map(s=>{
        if(!s.member||!s.name.startsWith('cave-'))return s;
        const pool=s.name==='cave-water'&&s.member<=counts[0];
        const index=s.member-(s.name==='cave-water'&&!pool?counts[0]:0);
        const key=pool?'pool':s.name==='cave-water'||s.name==='cave-mist'?'fall':'wet';
        const v=water[`${key}-${index}`]?.split(/\s+/).map(Number);if(!v)return s;
        const sheen=s.name==='cave-sheen';
        const x=pool?(v[0]+v[1])*.5:v[0],y=pool||sheen?v[2]:v[1];
        return {...s,baseX:x,baseY:y,baseWidth:pool?v[1]-v[0]:key==='fall'?Number(water['spray-shape'].split(/\s+/)[0])*2:32,
          baseHeight:sheen?Math.max(4,water['sheen-shape'].split(/\s+/).map(Number).slice(3,5).reduce((a,b)=>a+b,0)):key==='wet'?v[2]-v[1]:s.baseHeight,x,y};
      });
    }
    const prefix=['emitter',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),terrainProfile].join(':')+':';
    for(const [id,record] of records)if(id.startsWith(prefix)&&!found.some(s=>s.id===id))
      found.push({id,name:id.split(':')[4],emitter:true,reach:id.split(':')[4]==='torch',x:Number(record.x??0),y:Number(record.y??0)});
    const nativePrefix=prefix.replace(/^emitter:/,'source:');
    for(const [id] of records)if(id.startsWith(nativePrefix)&&!found.some(s=>s.id===id))
      found.push({id,name:id.split(':')[4],emitter:false,actor:id.endsWith(':00000000'),reach:id.split(':')[4]==='wall-torch',x:0,y:0});
    for(const [id] of records)if(id.startsWith(fieldPrefix())&&!found.some(s=>s.id===id))
      found.push({id,name:id.split(':')[4],field:true,catalogue:true,movable:false,reach:false,x:24,y:24,
        baseX:24,baseY:24,baseWidth:24,baseHeight:24,mapScaleX:1,mapScaleY:1});
    found=found.map(s=>{
      if(s.name!=='moon-field')return s;
      const [x,y]=(records.get(s.id)?.anchor||'112 62').split(/\s+/).map(Number);
      return {...s,x,y,baseX:x,baseY:y,anchorX:x,anchorY:y,anchor:1};
    });
    sources=found;
    const key=found.map(s=>`${s.id}:${s.x}:${s.y}:${s.support}`).join('|');
    if(key===sourceKey){$('#effectSource').value=selected;return;}sourceKey=key;
    const select=$('#effectSource');select.replaceChildren();
    for(const source of sources){const option=document.createElement('option');option.value=source.id;
      option.textContent=`${source.name==='exposure'?'Light box'+(source.nativeExposure?' (native)':''):source.name}${source.member?" #"+source.member:""} · ${source.catalogue&&!source.movable&&!source.guides?.length?'room controls':`${source.x}, ${source.y}`}`;select.append(option);}
    if(!found.some(s=>s.id===selected))selected=found[0]?.id??'';
    select.value=selected;refreshInspector();
  }
  function paintFloorRect(x0,y0,x1,y1,erase) {
    const left=Math.min(x0,x1)*16, top=Math.min(y0,y1)*16;
    const width=(Math.abs(x1-x0)+1)*16,height=(Math.abs(y1-y0)+1)*16;
    if(!erase&&(width>512||height>512)){status('Mist areas are limited to 512 × 512 pixels. Paint several smaller areas.');return;}
    const next=new Map(records);
    if(erase) {
      const prefix=['emitter',room.group.toString(16).padStart(2,'0'),room.map.toString(16).padStart(2,'0'),terrainProfile,'ground-mist'].join(':')+':';
      for(const [id,r] of next)if(id.startsWith(prefix)) {
        const x=Number(r.x??0),y=Number(r.y??0),w=Number(r.width??96),h=Number(r.height??64);
        if(x+w/2>left&&x-w/2<left+width&&y+h/2>top&&y-h/2<top+height)next.delete(id);
      }
    } else {
      const id=newEmitterId('ground-mist');
      next.set(id,{x:String(left+width/2),y:String(top+height/2),width:String(width),height:String(height),
        'mist-height':'26',color:'91bedf'});
    }
    change(encode(next),erase?'erase ground mist areas':'paint ground mist');
  }
  function paintParticleRect(x0,y0,x1,y1) {
    const left=Math.min(x0,x1)*16,top=Math.min(y0,y1)*16;
    const width=(Math.abs(x1-x0)+1)*16,height=(Math.abs(y1-y0)+1)*16;
    if(width>16384||height>16384){status('Particle regions are limited to 16384 × 16384 pixels.');return false;}
    const next=new Map(records),id=newEmitterId('particle-area');
    next.set(id,{x:String(left+width/2),y:String(top+height/2),width:String(width),height:String(height),
      particles:'4',pattern:'motes',color:'ffe6bb','travel-y':'-96'});
    return change(encode(next),'draw particle region');
  }
  function paintExposureRect(x0,y0,x1,y1) {
    const left=Math.min(x0,x1)*16,top=Math.min(y0,y1)*16;
    const width=(Math.abs(x1-x0)+1)*16,height=(Math.abs(y1-y0)+1)*16;
    $('#emitterPreset').value='exposure';
    if(!openModal(null,left+width/2,top+height/2))return false;
    return editEmitter(selected,{width,height,'fade-left':Math.min(64,width/4),'fade-right':Math.min(64,width/4),
      'fade-top':Math.min(64,height/4),'fade-bottom':Math.min(64,height/4)},'draw light box');
  }
  $('#lightBoxDraw').onclick=()=>{
    if(modal&&!finishModal(true))return;
    finishFramingDrag();setMode('2d');setLayer(0);$('#bSelect').onclick();
    brush='lightBox';cvs.style.cursor='crosshair';$('#effectGuides').checked=true;
    refreshEditorFeedback();status('Drag a light box, then set brightness and edge fades. Matching brightness boxes merge without stacking.');draw();
  };
  function paintContour(points) {
    const target=contourTarget?.scope===sceneKey(room)&&records.has(contourTarget.id)?contourTarget.id:null;
    const cloud=target?.includes(':cloud-bank:');
    if(cloud)points=[...points].sort((a,b)=>a.x-b.x).filter((p,i,a)=>!i||p.x!==a[i-1].x);
    if(points.length<2)return false;
    const xs=points.map(p=>p.x),ys=points.map(p=>p.y),left=Math.min(...xs),top=Math.min(...ys);
    const width=Math.max(4,Math.max(...xs)-left+4),height=Math.max(4,Math.max(...ys)-top+4);
    if(width>512||height>512){status('Split a long contour into strokes of at most 512px.');return false;}
    const x=Math.round(left+(width-4)/2),y=Math.round(top+(height-4)/2),next=new Map(records),id=target||newEmitterId('wet-contour');
    next.set(id,{...(target?records.get(id):{color:'91bedf',placement:'playfield'}),x:String(x),y:String(y),width:String(width),height:String(height),
      ...(cloud?{anchor:'bg1'}:{}),points:points.map(p=>`${Math.round(p.x-x)},${Math.round(p.y-y)}`).join(' ')});
    if(cloud)for(const key of ['textured','rolling','volume','wake','canopy-response'])delete next.get(id)[key];
    return change(encode(next),cloud?'trace ground dust path':'trace wet contour');
  }
  $('#contourDraw').onclick=()=>{
    if(modal&&!finishModal(true))return;
    contourTarget=(selected.includes(':cloud-bank:')||selected.includes(':particle-path:'))?{id:selected,scope:sceneKey(room)}:null;
    finishFramingDrag();setMode('2d');setLayer(0);$('#bSelect').onclick();
    brush='contour';cvs.style.cursor='crosshair';$('#effectGuides').checked=true;
    refreshEditorFeedback();status(contourTarget?'Trace the effect route from source to destination. The stroke replaces its path; Esc cancels.':'Trace a visible edge. One stroke creates one contour; Esc cancels.');draw();
  };
  $('#particleAreaDraw').onclick=()=>{
    finishFramingDrag();setMode('2d');setLayer(0);$('#bSelect').onclick();
    brush='particleArea';cvs.style.cursor='crosshair';$('#effectGuides').checked=true;
    refreshEditorFeedback();status('Drag a large region. One record emits a stable particle field across it.');draw();
  };
  for(const [id,tool] of [['floorMistDraw','floorMist'],['floorMistErase','floorErase']])$('#'+id).onclick=()=>{
    try {SharedRoomPreview.collisionGrid();}catch(error){status(error.message);return;}
    finishFramingDrag();setMode('2d');setLayer(0);$('#bSelect').onclick();
    brush=tool;cvs.style.cursor='crosshair';$('#effectCollision').checked=true;
    refreshEditorFeedback();status(tool==='floorMist'?'Drag an area around the floors to cover with mist.':'Drag across authored mist areas to remove them.');draw();
  };
  $('#effectCollision').onchange=()=>{if($('#effectCollision').checked)setMode('2d');draw();};
  $('#emitterPreview').onclick=()=>{
    if(modal){finishModal(true,true);return;}
    const r=records.get(selected);if(!r)return;
    if(r['actor-target'])SharedRoomPreview.previewActorBinding(parseInt(selected.split(':').at(-1),16),actor.x,actor.y,nativeFrame);
    previewAt(Number(r.x??0)+(r['actor-target']?actor.x:0),Number(r.y??0)+(r['actor-target']?actor.y:0),Number((r.anchor??'bg1')!=='bg1'));
  };
  function drawMapOverlay(ctx,view,viewport) {
    if(modal&&modal.scope!==sceneKey(room))finishModal(false);
    if(sourceScope!==sceneKey(room)||catalogueScope!==sceneKey(room)||catalogueDocument!==documentText)SharedRoomPreview.syncSources?.();
    if(sourceScope!==sceneKey(room))rebuildSources([]);
    if(bgIndex!==0){EmitterMapTools.draw(ctx,view,viewport);return;}
    const size=16*view.scale;
    if($('#effectCollision').checked) {
      try {
        const grid=SharedRoomPreview.collisionGrid();
        const left=Math.max(0,Math.floor(-view.x/size)),right=Math.min(grid.width,Math.ceil((viewport.width-view.x)/size));
        const top=Math.max(0,Math.floor(-view.y/size)),bottom=Math.min(grid.height,Math.ceil((viewport.height-view.y)/size));
        for(let y=top;y<bottom;y++)for(let x=left;x<right;x++) {
          const cell=grid.cells[y*grid.width+x];if(!cell)continue;
          ctx.fillStyle=(cell&15)===15?'rgba(64,200,100,.22)':'rgba(255,174,55,.32)';
          ctx.fillRect(view.x+x*size,view.y+y*size,size,size);
          if(cell&16){ctx.fillStyle='#63efff';ctx.fillRect(view.x+x*size,view.y+y*size,size,Math.max(1,view.scale));}
        }
      }catch(error){status(error.message);}
    }
    EmitterMapTools.draw(ctx,view,viewport);
    if(drag?.contour) {
      ctx.strokeStyle='#85c6ef';ctx.beginPath();
      drag.points.forEach((p,i)=>ctx[i?'lineTo':'moveTo'](view.x+p.x*view.scale,view.y+p.y*view.scale));ctx.stroke();
    }
    if(drag?.floorMist||drag?.particleArea||drag?.lightBox) {
      ctx.strokeStyle=drag.erase?'#ff857e':'#fff19b';
      ctx.strokeRect(view.x+Math.min(drag.x0,drag.x1)*size,view.y+Math.min(drag.y0,drag.y1)*size,
        (Math.abs(drag.x1-drag.x0)+1)*size,(Math.abs(drag.y1-drag.y0)+1)*size);
    }
  }
  function previewAt(x,y,layer=bgIndex) {
    // A static backdrop can be visible throughout the entire room. Its pixel
    // coordinates cannot uniquely identify a foreground camera location.
    if(layer===0) {
      setNativeCamera('x',Math.max(0,Math.round(x-128)));
      setNativeCamera('y',Math.max(0,Math.round(y-112)));
    } else status('Previewing this BG2 anchor at the current playfield camera. Scrub the level to inspect its attachment.');
    setMode('shared');draw();
  }
  function removeEffect(id) {
    const next=new Map(records);
    if(sources.find(s=>s.id===id)?.nativeExposure)next.set(id,{enabled:'0'});
    else if(id.startsWith('emitter:'))next.delete(id);
    else next.set(id,{...next.get(id),enabled:'0'});
    return change(encode(next),id.startsWith('emitter:')?'delete effect':'disable default effect');
  }
  function openModal(id,x,y) {
    if(modal)finishModal(false);
    if(pendingOp)commitOp();
    if(id&&!selectEmitter(id))return false;
    const entries=id?mapEmitters().filter(e=>e.id===id):[];
    const entry=Number.isFinite(x)&&Number.isFinite(y)?entries.sort((a,b)=>Math.hypot(a.x-x,a.y-y)-Math.hypot(b.x-x,b.y-y))[0]:entries[0];
    x=entry?.x??x??0;y=entry?.y??y??0;
    modal={before:documentText,scope:sceneKey(room),isNew:!id,x,y,id,layer:bgIndex,guideIndex:entry?.guideIndex};
    selectedGuideIndex=entry?.guideIndex;
    if(id&&sources.find(s=>s.id===id)?.nativeExposure&&!editEmitter(id,{},'extract light box')){modal=null;return false;}
    if(!id) {
      for(const option of $('#emitterPreset').options??[])
        option.disabled=bgIndex===1&&requiresTerrain(option.value);
      if(bgIndex===1&&requiresTerrain($('#emitterPreset').value))$('#emitterPreset').value='soft-light';
      if(!placeEmitter($('#emitterPreset').value||'soft-light',x,y)){modal=null;return false;}
      modal.id=selected;
    }
    $('#effectTypeControls').hidden=!!id;
    $('#effectInspectorTitle').textContent=id?'Configure effect':'Add effect';
    const preset=stagePresets.find(p=>'stage:'+p.id===$('#emitterPreset').value);
    const source=sources.find(s=>s.id===id);
    $('#effectInspectorHelp').textContent=source?.guides?.length?
      'These guides show the field’s source regions or terrain contacts. Resize supported regions with their map corners or the region controls below. Appearance settings are shared; the complete definition also exposes motion and terrain rules.':
      !id&&preset?preset.description+' Adds '+preset.members.length+' separately editable markers.':id?'Change the selected effect. Apply saves one undoable edit. Default effects stay tied to their native source art.':`Add an effect at ${Math.round(x)}, ${Math.round(y)} game pixels. Choose a type, adjust its area and appearance, then add it.`;
    $('#effectInspectorApply').textContent=id?'Apply changes':'Add effect';
    $('#effectInspectorDelete').hidden=!id;
    $('#effectInspectorDelete').textContent=id?.startsWith('emitter:')?'Delete effect':id?.startsWith('field:')?'Disable field':'Disable default effect';
    $('#effectInspectorStatus').textContent='Changes remain a draft until you apply them.';
    refreshInspector();$('#effectInspector').showModal();return true;
  }
  function finishModal(commit,preview=false) {
    if(!modal)return false;
    const session=modal,after=documentText,id=selected;
    const layer=id.startsWith('emitter:')?Number((records.get(id)?.anchor??'bg1')!=='bg1'):session.layer;
    const entry=mapEmitters(layer).find(e=>e.id===id&&(session.guideIndex===undefined||e.guideIndex===session.guideIndex));
    modal=null;restore(session.before);
    const ok=!commit||session.scope===sceneKey(room)&&change(after,session.isNew?'add effect':'configure effect');
    $('#effectInspector').close();
    if(commit&&ok) {selectEmitter(id);if(preview){if(records.get(id)?.['actor-target'])SharedRoomPreview.previewActorBinding(parseInt(id.split(':').at(-1),16),actor.x,actor.y,nativeFrame);previewAt(entry?.x??session.x,entry?.y??session.y,layer);}}
    draw();return ok;
  }
  $('#effectInspectorOpen').onclick=()=>openModal(selected);
  $('#effectInspectorClose').onclick=()=>finishModal(false);
  $('#effectInspector').addEventListener('cancel',e=>{e.preventDefault();finishModal(false);});
  $('#effectInspectorApply').onclick=()=>finishModal(true);
  $('#effectInspectorPreview').onclick=()=>finishModal(true,true);
  $('#emitterLocate').onclick=()=>{
    const id=selected,r=records.get(id),layer=Number((r?.anchor??'bg1')!=='bg1');
    if(!r||modal&&!finishModal(true))return;
    setMode('2d');setLayer(layer);$('#effectGuides').checked=true;selectEmitter(id);
    const rect=cvs.getBoundingClientRect();
    view.x=rect.width/2-(Number(r.x??0)+(r['actor-target']?actor.x:0))*view.scale;view.y=rect.height/2-(Number(r.y??0)+(r['actor-target']?actor.y:0))*view.scale;
    draw();
  };
  $('#effectInspectorDelete').onclick=()=>{if(modal&&removeEffect(selected))finishModal(true);};
  $('#effectFamilyOpen').onclick=()=>{
    const parent=sources.find(s=>s.id===selected)?.parent;
    if(parent&&finishModal(true))openModal(parent);
  };
  $('#emitterPreset').onchange=()=>{
    if(!modal?.isNew)return;
    const session=modal;restore(session.before);
    if(placeEmitter($('#emitterPreset').value,session.x,session.y))modal.id=selected;
  };
  for(const [id,key] of [...exposureFields,...nativeFields,...moteFields,...fieldFields,
      ...cloudFields.map(([key])=>["cloudLight-"+key,key]),
      ...stormFields.map(([key])=>["storm-"+key,key]),
      ...['x','y','width','height','particles','lifetime','mist-height'].map(k=>['emitter'+k,k]),
      ['effectIntensity','intensity'],['effectReach','reach'],['effectIllumination','illumination'],['effectColor','color'],
      ['emitterColorEnd','color-end'],['emitterAnchor','anchor'],['emitterPoints','points'],
      ...actorFields.map(([id,key])=>['actorBinding'+id,key])])resetControl($('#'+id),[key]);
  refreshInspector();
  return {resetValues,resetHandle,copyEffects,pasteEffects,startEffectPaste,clearMapSelection,hasClipboard:()=>clipboardActive&&!!effectClipboard,deactivateClipboard:()=>{clipboardActive=false;},isMapSelected:id=>mapSelection.has(id),hasMapSelection:()=>mapEmitters().some(e=>mapSelection.has(e.id)),openModal,finishModal,previewAt,removeEffect,updateCatalogue,modalOpen:()=>!!modal,placeEmitter,editEmitter,mapEmitters,paintContour,selectEmitter,selected:()=>selected,selectedGuide:()=>selectedGuideIndex,paintFloorRect,paintParticleRect,paintExposureRect,drawMapOverlay,text:()=>documentText,restore,loadDocument,markSaved,updateSources,dirty:()=>documentText!==saved};
})();
