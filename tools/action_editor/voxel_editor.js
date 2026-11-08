/* Presentation-only voxel authoring. Applied records belong to the ordinary
 * project INI; a separate draft is sent only to the shared renderer. Native
 * collision and priority are deliberately absent from the editing contract. */
const VoxelEditor=(()=>{
  const clone=value=>JSON.parse(JSON.stringify(value));
  const empty=()=>({cells:[[],[],[]],objects:[{},{},{}]});
  const hex=value=>Number(value).toString(16).toUpperCase().padStart(4,'0');
  const number=value=>String(Number(Number(value).toFixed(4)));
  let draft=null,draftRoom='',selection=[],active=false,gesture=null,objects=[],message='';
  let sample=null,tool='select',previewGesture=null,previewUndo=[],previewRedo=[],navigation=-1;
  let pullSide='left',pullHandles=null;
  let selectionLabel='',selectionPivotDepth=null,transformHandles=null,transformSession=null,transformEditing=false,pivotPreset='bottom';
  let transformGroupCache=new Map();
  let conversionPreview=null;
  const artFields=['front','back','top','side','left','right','bottom'].map(face=>face+'-art');
  const faceNames=['front','back','top','bottom','left','right'];
  const faceEnum={front:0,top:1,side:2,back:3,left:5,right:6,bottom:7};
  const cellFields=['front','back','width','offset','backing',...artFields,'faces','shape','shape-span','front-end','back-end','bend','transform'];
  const element=id=>document.querySelector('#voxel'+id);
  const profile=r=>r.terrainProfile||0;
  const state=r=>{
    const value=roomConfig(r).voxel||empty();
    for(let p=0;p<3;p++)if(!Array.isArray(value.cells[p]))value.cells[p]=VoxelRecords.normalize(value.cells[p]);
    return value;
  };
  const current=()=>draft||state(room);
  const key=item=>item.kind==='tile'?`${item.x},${item.y}`:
    `${item.source}:${item.parent??'*'}:${item.animation??'*'}`;
  const group=item=>item.kind==='tile'?'cells':'objects';
  const recordFor=(item,value=current(),p=profile(room))=>item.kind==='tile'
    ?VoxelRecords.resolve(value.cells[p],item.x,item.y):Object.values(value.objects[p]).reverse().find(r=>
      r.source===item.source&&(r.parent===undefined||r.parent===(item.parent||0))&&
      (r.animation===undefined||r.animation===(item.animation||0)));
  const modeled=item=>{
    const record=recordFor(item)||{},mode=record.mode??item.mode??'auto';
    return item.canExtrude!==false&&(mode==='extruded'||mode==='auto'&&item.active===true);
  };
  function numeric(value,min,max,label,integer=false) {
    const result=Number(value);
    if(value===''||!Number.isFinite(result)||result<min||result>max||integer&&!Number.isInteger(result))
      throw Error(`${label} must be ${integer?'a whole number ':''}between ${min} and ${max}.`);
    return result;
  }
  function art(value) {
    if(value==='auto')return 'auto';
    const parts=String(value).split(',');
    if(parts.length!==8||!/^([01])$/.test(parts[0])||parts.slice(1,5).some(v=>!/^\w{1,4}$/.test(v)||!/^[0-9a-f]+$/i.test(v)))
      throw Error('Face artwork must use four native words from the current room palette.');
    const [x,y,size]=parts.slice(5).map(Number);
    if(![x,y,size].every(Number.isInteger)||![8,16].includes(size)||x<0||y<0||x+size>16||y+size>16)
      throw Error('Face artwork crop must fit the native 16 × 16 tile.');
    return [Number(parts[0]),...parts.slice(1,5).map(v=>hex(parseInt(v,16))),x,y,size].join(',');
  }
  function parseBend(value) {
    const parts=String(value).split(',');
    if(parts.length!==6||!['x','y'].includes(parts[0]))throw Error('Attached pull requires an X or Y axis, base, tip and three offsets.');
    const anchor=numeric(parts[1],-8192,8192,'Attached base',true),tip=numeric(parts[2],-8192,8192,'Attached tip',true);
    if(anchor===tip)throw Error('Attached base and tip must be different.');
    return {axis:parts[0],anchor,tip,dx:numeric(parts[3],-256,256,'Tip X'),
      dy:numeric(parts[4],-256,256,'Tip Y'),dz:numeric(parts[5],-1.2,1.2,'Tip depth')};
  }
  const bendText=b=>[b.axis,b.anchor,b.tip,number(b.dx),number(b.dy),number(b.dz)].join(',');
  const bendSide=b=>b.axis==='x'?(b.tip>b.anchor?'left':'right'):(b.tip>b.anchor?'top':'bottom');
  const transformFields=['px','py','pz','dx','dy','dz','sx','sy','sz'];
  function parseTransform(value) {
    const parts=String(value).split(',');
    if(parts.length!==9)throw Error('Move / resize requires a shared XYZ pivot, three offsets and three scales.');
    const limits=[[-8192,8192],[-8192,8192],[-2,2],[-256,256],[-256,256],[-1.2,1.2],[.125,4],[.125,4],[.125,4]];
    return Object.fromEntries(transformFields.map((field,i)=>[field,numeric(parts[i],...limits[i],field)]));
  }
  const transformText=t=>transformFields.map(field=>number(t[field])).join(',');
  function parse(token,values) {
    const out={};
    if(token==='voxel-cell') {
      const xy=String(values.cell||'').split(',');
      if(xy.length!==2)throw Error('Voxel cell requires cell:x,y.');
      out.x=numeric(xy[0],-512,511,'Cell X',true);out.y=numeric(xy[1],-512,511,'Cell Y',true);
      if(values.size!==undefined) {
        const size=String(values.size).split(',');if(size.length!==2)throw Error('Region size requires width,height.');
        const w=numeric(size[0],1,1024,'Region width',true),h=numeric(size[1],1,1024,'Region height',true);
        if(out.x+w>512||out.y+h>512)throw Error('Voxel region extends beyond cell 511.');
        if(w>1)out.w=w;if(h>1)out.h=h;
      }
      for(const [name,min,max] of [['front',-.4,.8],['back',-.4,.8],['width',1,64],['offset',-32,32]])
        if(values[name]!==undefined)out[name]=numeric(values[name],min,max,name);
      if(out.front!==undefined&&out.back!==undefined&&out.back<out.front)throw Error('Back depth must be at or behind front depth.');
      if(values.backing!==undefined) {
        if(!['auto','none','fill','replace'].includes(values.backing))throw Error('Invalid voxel backing.');
        out.backing=values.backing;
      }
      for(const name of artFields)if(values[name]!==undefined)out[name]=art(values[name]);
      if(values.faces!==undefined) {
        const parts=String(values.faces).split('+');
        if(!['all','none'].includes(values.faces)&&(new Set(parts).size!==parts.length||parts.some(face=>!faceNames.includes(face))))throw Error('Choose valid, distinct voxel faces.');
        out.faces=['all','none'].includes(values.faces)?values.faces:faceNames.filter(face=>parts.includes(face)).join('+');
      }
      if(values.shape!==undefined) {
        if(!['box','taper','curve'].includes(values.shape))throw Error('Unknown voxel shape.');out.shape=values.shape;
      }
      if(values['shape-span']!==undefined&&(!out.shape||out.shape==='box'))throw Error('A shape span requires Taper or Curve.');
      if(out.shape&&out.shape!=='box') {
        const domain=String(values['shape-span']??`${out.y*16},${(out.h||1)*16}`).split(',');
        if(domain.length!==2)throw Error('Shape span requires Y,height.');
        const y=numeric(domain[0],-8192,8191,'Shape origin',true),h=numeric(domain[1],1,16384,'Shape height',true);
        if(y+h>8192)throw Error('Shape span exceeds native row 8192.');out['shape-span']=`${y},${h}`;
      }
      for(const name of ['front-end','back-end'])if(values[name]!==undefined) {
        if(!out.shape||out.shape==='box')throw Error('Endpoint depths require Taper or Curve.');
        out[name]=numeric(values[name],-.4,.8,name);
      }
      if(out['front-end']!==undefined&&out['back-end']!==undefined&&out['back-end']<out['front-end'])throw Error('Bottom back depth must be at or behind bottom front depth.');
      if(values.bend!==undefined)out.bend=bendText(parseBend(values.bend));
      if(values.transform!==undefined)out.transform=transformText(parseTransform(values.transform));
      return out;
    }
    for(const name of ['source','parent','animation'])if(values[name]!==undefined) {
      if(!/^[0-9a-f]{1,4}$/i.test(values[name]))throw Error(`Invalid object ${name}.`);
      out[name]=parseInt(values[name],16);
    }
    if(!out.source)throw Error('Voxel object requires a nonzero native source family.');
    if(values.mode!==undefined) {
      if(!['auto','flat','extruded'].includes(values.mode))throw Error('Invalid object mode.');
      out.mode=values.mode;
    }
    for(const [name,min,max] of [['offset',-.4,.8],['thickness',0,.8],['width',.125,4]])
      if(values[name]!==undefined)out[name]=numeric(values[name],min,max,name);
    if(!['mode','offset','thickness','width'].some(name=>out[name]!==undefined))throw Error('Voxel object needs a visual property.');
    return out;
  }
  function importRecord(r,token,values,region) {
    const item=parse(token,values),target=roomConfig(r).voxel??=empty();
    item._order=target._sequence||0;target._sequence=item._order+1;
    const mask=region==='all'?7:terrainMask(region),kind=token==='voxel-cell'?'cells':'objects';
    const id=kind==='cells'?'':`${item.source}:${item.parent??'*'}:${item.animation??'*'}`;
    for(let i=0;i<3;i++)if(mask&(1<<i)) {
      if(kind==='cells')target.cells[i]=VoxelRecords.append(target.cells[i],item);
      else {delete target.objects[i][id];target.objects[i][id]=clone(item);}
    }
  }
  function recordLine(kind,item) {
    const fields=kind==='cells'?[`cell:${item.x},${item.y}`]:[`source:${hex(item.source)}`];
    if(kind==='cells'&&((item.w||1)>1||(item.h||1)>1))fields.push(`size:${item.w||1},${item.h||1}`);
    if(kind==='objects')for(const name of ['parent','animation'])if(item[name]!==undefined)fields.push(`${name}:${hex(item[name])}`);
    const names=kind==='cells'?cellFields:['mode','offset','thickness','width'];
    for(const name of names)if(item[name]!==undefined)fields.push(`${name}:${typeof item[name]==='number'?number(item[name]):item[name]}`);
    if(kind==='cells'&&!Object.keys(VoxelRecords.payload(item)).length)fields.push('auto:1');
    return `${kind==='cells'?'voxel-cell':'voxel-object'} = ${fields.join(' ')}`;
  }
  function lines(r,value=state(r)) {
    const output=[];
    for(const kind of ['cells','objects']) {
      // Both rectangles and object-family selectors may overlap. Combining
      // only equal sequence heads preserves each terrain's winning record.
      const queues=value[kind].map(records=>(Array.isArray(records)?records:Object.values(records)).map(item=>({line:recordLine(kind,item),order:item._order??Infinity})));
      const combined=[];
      while(queues.some(queue=>queue.length)) {
        // Original source order avoids duplicating a common suffix when one
        // terrain has extra leading records. It is editor metadata only.
        const line=queues.filter(queue=>queue.length).sort((a,b)=>a[0].order-b[0].order)[0][0].line;let mask=0;
        for(let p=0;p<3;p++)if(queues[p][0]?.line===line){mask|=1<<p;queues[p].shift();}
        combined.push(line.replace(' =',terrainField(mask)+' ='));
      }
      if(combined.length>(kind==='cells'?256:64))throw Error('Voxel limits: 256 compact regions and 64 object rules per room across terrains.');
      output.push(...combined);
    }
    return output;
  }
  function previewSection(r,text) {
    if(!draft||draftRoom!==roomKey(r))return text;
    return text.split('\n').filter(line=>!/^\s*voxel-(cell|object)(?::[^\s=]+)?\s*=/.test(line)).concat(lines(r,draft)).join('\n');
  }
  const pending=()=>!!draft&&lines(room,draft).join('\n')!==lines(room,state(room)).join('\n');
  function status(text,error=false) {
    message=text;const node=element('Status');if(node){node.textContent=text;node.classList.toggle('error',error);}
  }
  function refresh() {
    const panel=element('Panel');if(!panel)return;
    panel.hidden=!active||mode!=='shared';
    element('Edit').classList.toggle('on',active);element('Edit').setAttribute('aria-pressed',String(active));
    const selectedKind=selection[0]?.kind==='tile'?'tile':'object';
    element('Selection').textContent=selection.length?`${selectionLabel?selectionLabel+' · ':''}${selection.length} ${selectedKind}${selection.length===1?'':'s'} · ${TERRAIN_TOKENS[profile(room)].toUpperCase()}`:'Select a surface';
    element('Apply').disabled=element('Discard').disabled=!pending();
    element('Reset').disabled=!selection.length;
    element('Draft').textContent=pending()?'Preview draft':'Applied';
    element('Tool').value=tool;
    element('SampleSummary').textContent=sample?.label||'No sample';
    element('Sample').disabled=!selection.length;
    element('Paste').disabled=element('MatchDepth').disabled=!sample||!selection.length||sample.kind!==selection[0]?.kind;
    element('Undo').disabled=!previewUndo.length&&(!previewGesture||!previewChanged(previewGesture.before,previewSnapshot()));
    element('Redo').disabled=!previewRedo.length;
    element('Focus').disabled=element('ResetOne').disabled=!selection.length;
    const records=current().cells[profile(room)],encoded=lines(room,current());
    element('Budget').textContent=`${encoded.filter(line=>line.startsWith('voxel-cell')).length}/256 regions · ${encoded.filter(line=>line.startsWith('voxel-object')).length}/64 objects`;
    const navigator=element('Navigate'),signature=records.map(r=>`${r.x},${r.y}:${r.w||1},${r.h||1}`).join('|');
    if(navigator.dataset.signature!==signature) {
      navigator.dataset.signature=signature;navigator.replaceChildren();
      if(!records.length){const option=document.createElement('option');option.textContent='No authored regions';option.value='';navigator.appendChild(option);}
      records.forEach((r,i)=>{const option=document.createElement('option');option.value=String(i);option.textContent=`${i+1}. Cell ${r.x}, ${r.y} · ${r.w||1} × ${r.h||1}`;navigator.appendChild(option);});
    }
    navigator.disabled=element('Previous').disabled=element('Next').disabled=!records.length;
    if(navigation>=0&&navigation<records.length)navigator.value=String(navigation);
    const item=selection[0],isTile=item?.kind==='tile';
    for(const id of ['CopyBacking','CopyArt','CopyFaces','CopyTransform'])element(id).disabled=!!item&&!isTile;
    const resetFields=isTile?['depth','front','back','front-end','back-end','width','offset','backing','art','faces','shape','bend','transform']:['depth','width','offset','mode','thickness'];
    for(const field of ['depth','front','back','front-end','back-end','width','offset','backing','art','faces','shape','bend','transform','mode','thickness'])element('ResetProperty').querySelector(`[value="${field}"]`).disabled=!resetFields.includes(field);
    if(item&&!resetFields.includes(element('ResetProperty').value))element('ResetProperty').value='depth';
    element('Empty').hidden=!!item;
    element('TileFields').hidden=!isTile;element('ObjectFields').hidden=!item||isTile;
    element('PullFields').hidden=!isTile||tool!=='attached';
    element('TransformFields').hidden=!isTile||tool!=='transform';
    element('DepthControls').hidden=isTile&&['attached','transform'].includes(tool);
    element('TileLegend').textContent=['attached','transform'].includes(tool)?'Surfaces & geometry':'Depth';
    element('Tool').querySelector('[value="attached"]').disabled=!!item&&!isTile;
    element('Tool').querySelector('[value="transform"]').disabled=!!item&&!isTile;
    element('Grow').disabled=element('Shrink').disabled=!isTile;
    element('SelectWhole').disabled=!isTile;
    const unsupported=selection.some(value=>value.supported===false);
    element('TileFields').disabled=element('ObjectFields').disabled=unsupported||Number(element('Mode').value)!==2;
    element('PullFields').disabled=element('TileFields').disabled;
    const transformUnsupported=selection.find(value=>value.transformSupported===false);
    element('TransformFields').disabled=element('TileFields').disabled||!!transformUnsupported;
    element('TransformHint').textContent=transformUnsupported?.transformReason||
      'Move or resize the selection around one shared pivot. Its native collision stays fixed.';
    if(unsupported){element('Reset').disabled=true;status(item?.reason||'This surface uses dedicated enclosure controls.',true);}
    if(!item){element('Target').textContent='';element('SelectionDetails').textContent='';return;}
    const record=recordFor(item)||{};
    const face=item.face||'front';
    element('Target').textContent=isTile?`${face[0].toUpperCase()+face.slice(1)} face · cell ${item.x}, ${item.y}`:item.label||'Native object';
    element('SelectionDetails').textContent=isTile?`BG1 · native priority ${item.priority??'—'}. Edits affect the selected terrain only.`:
      `Native priority ${item.priority??'—'}. The captured pose is frozen; edits affect matching objects in this terrain.`;
    if(!isTile)element('ObjectScope').textContent=`Source $${hex(item.source)} · parent $${hex(item.parent||0)} · animation $${hex(item.animation||0)} · priority ${item.priority??'—'}. This is a family rule, not an individual placement. Capture poses are frozen.`;
    const names=isTile?['front','back','width','offset']:['offset','thickness','width'];
    for(const name of names) {
      const id=(isTile?'Tile':'Object')+name[0].toUpperCase()+name.slice(1);
      const fallback=isTile?(name==='width'?16:0):(name==='width'?1:0);
      const value=record[name]??item[name==='width'&&!isTile?'widthScale':name]??fallback;
      for(const suffix of ['','Range'])if(element(id+suffix)&&document.activeElement!==element(id+suffix))element(id+suffix).value=number(value);
    }
    if(isTile) {
      if(tool==='transform') {
        const context=transformContext(),t=context.control;
        if(context.baselines&&!transformUnsupported)element('TransformHint').textContent+=
          ' Mixed placements use a relative group adjustment.';
        for(const field of ['dx','dy','dz','sx','sy','sz']) {
          const node=element('Transform'+field[0].toUpperCase()+field.slice(1));
          if(document.activeElement!==node)node.value=number(t[field]*(field[0]==='s'?100:1));
        }
        element('TransformPivot').value=pivotPreset;
        element('TransformSummary').textContent=`Pivot ${number(t.px)}, ${number(t.py)} px · depth ${number(t.pz)}`+
          (context.baselines?' · keeps existing placements':'');
        element('TransformReset').disabled=!selection.some(value=>recordFor(value)?.transform);
      }
      const bend=currentBend();
      element('PullAnchor').value=bendSide(bend);
      for(const [id,field] of [['PullX','dx'],['PullY','dy'],['PullZ','dz']])
        if(document.activeElement!==element(id))element(id).value=number(bend[field]);
      element('PullReset').disabled=!selection.some(value=>{const token=recordFor(value)?.bend;
        if(!token)return false;const b=parseBend(token);return b.dx!==0||b.dy!==0||b.dz!==0;});
      element('PullSpan').textContent=`Fixed ${bendSide(bend)} edge at ${bend.anchor} px · tip at ${bend.tip} px · ${selection.length} selected tile${selection.length===1?'':'s'}`;
      element('Backing').value=record.backing??item.backing??'auto';
      const width=record.width??item.width??16,offset=record.offset??item.offset??0;
      element('PlacementSummary').textContent=`${number(width)} px${offset?' · '+(offset>0?'+':'')+number(offset):''}`;
      element('BackingSummary').textContent=({auto:'Automatic',none:'None',fill:'Fill',replace:'Replace'})[element('Backing').value];
      element('ArtSummary').textContent=artFields.some(name=>record[name]&&record[name]!=='auto')?'Custom':'Automatic';
      for(const name of ['front','back','top','side','left','right','bottom'])element(name[0].toUpperCase()+name.slice(1)+'ArtLabel').textContent=
        record[name+'-art']&&record[name+'-art']!=='auto'?'Custom native tile':'Automatic';
      const shape=record.shape??item.shape??'box';element('Shape').value=shape;
      element('ShapeSummary').textContent=({box:'Straight',taper:'Linear',curve:'Smooth'})[shape];
      for(const [id,field,native] of [['FrontEnd','front-end','frontEnd'],['BackEnd','back-end','backEnd']]) {
        element(id).disabled=shape==='box';if(document.activeElement!==element(id))element(id).value=number(record[field]??item[native]??record.back??item.back??0);
      }
      const domain=record['shape-span']??`${item.shapeY??item.y*16},${item.shapeH??16}`;
      element('ShapeDomain').textContent=shape==='box'?'Choose Taper or Curve to connect the selection from top to bottom.':`Connected profile: native Y ${domain.split(',')[0]}, height ${domain.split(',')[1]} px. Artwork edits retain this span.`;
      const faces=record.faces??(item.faces===undefined?'all':item.faces===63?'all':item.faces===0?'none':faceNames.filter((_,i)=>item.faces&(1<<i)).join('+'));
      for(const face of faceNames)element('Face'+face[0].toUpperCase()+face.slice(1)).checked=faces==='all'||faces.split('+').includes(face);
      element('FacesSummary').textContent=faces==='all'?'All':faces==='none'?'Hidden':`${faces.split('+').length} / 6`;
    } else {
      element('ObjectMode').value=record.mode??item.mode??'auto';
      element('ObjectMode').querySelector('[value="extruded"]').disabled=item.canExtrude===false;
      const transforms=selection.every(modeled);
      for(const name of ['Offset','Thickness','Width'])for(const suffix of ['','Range'])
        element('Object'+name+suffix).disabled=!transforms;
      element('ObjectWidthSummary').textContent=`${number(record.width??item.widthScale??1)}×`;
      element('ObjectTransformHint').textContent=transforms?'':
        item.canExtrude===false?'This captured object cannot be extruded. Auto / Flat preserve its native pixels.':
        'Choose Extruded to edit this sprite’s depth and thickness.';
    }
  }
  function invalidate() {SharedRoomPreview.invalidate();refresh();draw();}
  const previewSnapshot=()=>draft?clone(draft):null;
  let previewSession=null,artworkPreview=false;
  const previewChanged=(before,after)=>lines(room,before||state(room)).join('\n')!==lines(room,after||state(room)).join('\n');
  function rememberPreview(before,after,label) {
    if(!previewChanged(before,after))return;
    previewUndo.push({before,after,label});if(previewUndo.length>80)previewUndo.shift();previewRedo=[];
  }
  function finishOutsideArtwork() {
    if(previewSession&&!artworkPreview&&typeof finishVoxelFacePaletteForHistory==='function'&&!finishVoxelFacePaletteForHistory())
      throw Error('Finish or cancel the artwork choice before another edit.');
  }
  function beginPreviewGesture(label='voxel gesture') {finishOutsideArtwork();if(!previewGesture)previewGesture={before:previewSnapshot(),label};}
  function finishPreviewGesture() {
    if(!previewGesture)return;
    rememberPreview(previewGesture.before,previewSnapshot(),previewGesture.label);previewGesture=null;refreshHistoryButtons();
  }
  function beginPreviewSession(label='face artwork') {
    if(previewSession)cancelPreviewSession();
    endGesture();
    const saved={draft:previewSnapshot(),draftRoom,undo:previewUndo.slice(),redo:previewRedo.slice()};
    beginPreviewGesture(label);
    previewSession=saved;
  }
  function acceptPreviewSession() {
    if(!previewSession)return;
    previewSession=null;finishPreviewGesture();
  }
  function cancelPreviewSession() {
    if(!previewSession)return;
    const saved=previewSession;previewSession=null;gesture=null;previewGesture=null;conversionPreview=null;
    draft=saved.draft?clone(saved.draft):null;draftRoom=saved.draftRoom;
    previewUndo=saved.undo;previewRedo=saved.redo;
    status('Artwork choice cancelled. Previous preview restored.');refreshHistoryButtons();invalidate();
  }
  function stage(next,label='voxel edit') {
    if(lines(room,next).join('\n')===lines(room,current()).join('\n'))return;
    const before=previewSnapshot();draft=next;draftRoom=roomKey(room);
    if(!transformEditing){transformSession=null;transformGroupCache=new Map();}
    if(!previewGesture)rememberPreview(before,previewSnapshot(),label);
    status('Preview updated. Apply to keep these changes.');refreshHistoryButtons();invalidate();
  }
  function undoPreview() {
    if(typeof finishVoxelFacePaletteForHistory==='function'&&!finishVoxelFacePaletteForHistory())return false;
    endGesture();conversionPreview=null;transformSession=null;transformGroupCache=new Map();const op=previewUndo.pop();if(!op)return false;
    previewRedo.push(op);draft=op.before?clone(op.before):null;draftRoom=draft?roomKey(room):'';
    status(`Undid preview ${op.label}.`);refreshHistoryButtons();invalidate();return true;
  }
  function redoPreview() {
    if(typeof finishVoxelFacePaletteForHistory==='function'&&!finishVoxelFacePaletteForHistory())return false;
    endGesture();conversionPreview=null;transformSession=null;transformGroupCache=new Map();const op=previewRedo.pop();if(!op)return false;
    previewUndo.push(op);draft=op.after?clone(op.after):null;draftRoom=draft?roomKey(room):'';
    status(`Redid preview ${op.label}.`);refreshHistoryButtons();invalidate();return true;
  }
  function clearPreviewHistory(){previewUndo=[];previewRedo=[];previewGesture=null;}
  function mutate(items,change,label='voxel edit') {
    if(!items.length)return;
    finishOutsideArtwork();
    if(items.some(item=>item.supported===false))throw Error(items.find(item=>item.supported===false).reason||'This surface cannot be edited with voxel controls.');
    const next=clone(current()),p=profile(room),cells=[],order=next._sequence||0;next._sequence=order+1;
    for(const item of items) {
      const kind=group(item),base=recordFor(item,next,p)||{},values=change(item,clone(kind==='cells'?VoxelRecords.payload(base):base));
      if(kind==='cells') {
        const encoded=Object.fromEntries(Object.entries(values).filter(([,v])=>v!==undefined).map(([k,v])=>[k,String(v)]));
        encoded.cell=`${item.x},${item.y}`;const parsed=parse('voxel-cell',encoded);
        if(VoxelRecords.signature(parsed)!==VoxelRecords.signature(base))cells.push({...parsed,_order:order});
      } else {
        const selectors={source:item.source,parent:item.parent||0,animation:item.animation||0};
        const result={...selectors,...values};
        if(!['mode','offset','thickness','width'].some(name=>result[name]!==undefined))result.mode='auto';
        const encoded=Object.fromEntries(Object.entries(result).filter(([,v])=>v!==undefined).map(([k,v])=>[k,['source','parent','animation'].includes(k)?hex(v):String(v)]));
        const parsed=parse('voxel-object',encoded);
        if(['mode','offset','thickness','width'].every(name=>parsed[name]===base[name]))continue;
        delete next.objects[p][key(item)];next.objects[p][key(item)]={...parsed,_order:order};
      }
    }
    if(cells.length) {
      const before=next.cells[p];next.cells[p]=VoxelRecords.update(before,cells);
      try {lines(room,next);}catch(error) {
        // Across terrains, flattening can also lose useful shared source
        // rectangles. Try the ordered representation before rejecting it.
        next.cells[p]=before;
        for(const record of VoxelRecords.pack(cells))next.cells[p]=VoxelRecords.append(next.cells[p],{...record,_order:order});
        lines(room,next);
      }
      validateTransformCandidate(next,items,cells);
    }
    stage(next,label);
  }
  function selectedBounds(items=selection) {
    if(!items.length)return null;
    let x=Infinity,y=Infinity,right=-Infinity,bottom=-Infinity;
    for(const item of items){const c=item.kind==='tile'?{x:item.x*16,y:item.y*16,width:16,height:16}:item;
      x=Math.min(x,c.x);y=Math.min(y,c.y);right=Math.max(right,c.x+c.width);bottom=Math.max(bottom,c.y+c.height);}
    return {x,y,width:right-x,height:bottom-y};
  }
  function visibleBounds(items=selection) {
    if(!items.length)return null;
    let left=Infinity,top=Infinity,right=-Infinity,bottom=-Infinity;
    for(const item of items) {
      if(item.kind!=='tile') {
        left=Math.min(left,item.x);right=Math.max(right,item.x+item.width);
        top=Math.min(top,item.y);bottom=Math.max(bottom,item.y+item.height);continue;
      }
      const record=recordFor(item)||{},x=item.x*16,y=item.y*16;
      const width=record.width??item.width??16,offset=record.offset??item.offset??0;
      let x0=x+8+offset-width/2,x1=x+8+offset+width/2,y0=y,y1=y+16;
      if(record.bend) {
        const b=parseBend(record.bend),start=b.axis==='x'?x:y;
        const weight=position=>{const t=Math.max(0,Math.min(1,(position-b.anchor)/(b.tip-b.anchor)));return t*t*(3-2*t);};
        const a=weight(start),z=weight(start+16);
        // Independent ranges conservatively contain every curved point, even
        // if a large pull reverses direction inside this source cell.
        x0+=Math.min(a*b.dx,z*b.dx);x1+=Math.max(a*b.dx,z*b.dx);
        y0+=Math.min(a*b.dy,z*b.dy);y1+=Math.max(a*b.dy,z*b.dy);
      }
      if(record.transform) {
        const t=parseTransform(record.transform);
        x0=t.px+t.dx+(x0-t.px)*t.sx;x1=t.px+t.dx+(x1-t.px)*t.sx;
        y0=t.py+t.dy+(y0-t.py)*t.sy;y1=t.py+t.dy+(y1-t.py)*t.sy;
      }
      left=Math.min(left,x0);right=Math.max(right,x1);top=Math.min(top,y0);bottom=Math.max(bottom,y1);
    }
    return {x:left,y:top,width:right-left,height:bottom-top};
  }
  function defaultTransform(area=selectedBounds(),preset=pivotPreset) {
    if(!area)throw Error('Select some tiles to move or resize.');
    const item=metadata(selection[0]),depth=selectionPivotDepth??((Number(item.front)||0)+(Number(item.back)||0))/2;
    return {px:area.x+area.width/2,py:area.y+(preset==='center'?area.height/2:area.height),
      pz:Math.max(-2,Math.min(2,depth)),dx:0,dy:0,dz:0,sx:1,sy:1,sz:1};
  }
  function prepareTransformSelection() {
    transformSession=null;transformHandles=null;transformGroupCache=new Map();pivotPreset='bottom';
    const token=selection[0]?.kind==='tile'&&recordFor(selection[0])?.transform,area=selectedBounds();
    if(!token||!area)return;
    const t=parseTransform(token);
    pivotPreset=t.px===area.x+area.width/2&&t.py===area.y+area.height?'bottom':
      t.px===area.x+area.width/2&&t.py===area.y+area.height/2?'center':'custom';
  }
  function transformContext() {
    if(transformSession)return transformSession;
    const tokens=selection.map(item=>recordFor(item)?.transform||''),common=tokens.every(token=>token===tokens[0]);
    const control=common&&tokens[0]?parseTransform(tokens[0]):defaultTransform();
    transformSession={control,baselines:common?null:new Map(selection.map((item,i)=>
      [key(item),tokens[i]?parseTransform(tokens[i]):{...control,dx:0,dy:0,dz:0,sx:1,sy:1,sz:1}]))};
    return transformSession;
  }
  function composeTransform(outer,inner) {
    const result={...outer};
    for(const axis of ['x','y','z']) {
      result['s'+axis]=outer['s'+axis]*inner['s'+axis];
      result['d'+axis]=outer['d'+axis]+outer['s'+axis]*(inner['d'+axis]+
        (1-inner['s'+axis])*(inner['p'+axis]-outer['p'+axis]));
    }
    return parseTransform(transformText(result));
  }
  function validateTransformCandidate(candidate,items,changes) {
    const hint='Select the whole classified object to move/resize it; use Attached pull or convert a partial selection.';
    const surfaceFields=['front','back','width','offset','front-art','top-art','side-art','left-art','right-art','bottom-art',
      'shape','shape-span','front-end','back-end','bend'];
    const hasSurface=record=>surfaceFields.some(field=>record?.[field]!==undefined &&
        (!field.endsWith('-art')||record[field]!=='auto'));
    const nativeCandidate=(record,fact)=>fact.nativeGeometry!==undefined&&!hasSurface(record);
    const affected=changes.filter(record=>{
      const before=recordFor({kind:'tile',x:record.x,y:record.y})||{};
      return record.transform!==before.transform||record.transform&&
        (hasSurface(record)!==hasSurface(before)||record.backing!==before.backing||record['back-art']!==before['back-art']);
    });
    const selectedByKey=new Map(items.filter(item=>item.kind==='tile').map(item=>[key(item),item]));
    const candidateHasNativeTransforms=candidate.cells[profile(room)].some(record=>record.transform&&!hasSurface(record));
    const checked=new Set();
    for(const changed of affected) {
      const item=selectedByKey.get(`${changed.x},${changed.y}`)||{kind:'tile',x:changed.x,y:changed.y};
      const fact=metadata(item);if(fact.nativeGeometry===undefined)continue;
      if(!nativeCandidate(changed,fact)&&!candidateHasNativeTransforms)continue;
      let component=transformGroupCache.get(key(item));
      if(!component) {
        const found=SharedRoomPreview.voxelGroup?.(item.x,item.y);
        if(!found||found.supported===false||found.status!=='ok'||!found.cells?.length) {
          if(nativeCandidate(changed,fact))throw Error(hint);
          continue;
        }
        component=found.cells.map(([x,y])=>{
          const selected=selectedByKey.get(`${x},${y}`);
          return metadata(selected||{kind:'tile',x,y});
        });
        for(const member of component)transformGroupCache.set(key(member),component);
      }
      if(checked.has(component))continue;checked.add(component);
      const records=component.map(member=>recordFor(member,candidate)||{});
      const native=component.filter((member,i)=>records[i].transform&&nativeCandidate(records[i],member));
      if(!native.length)continue; // Authored surfaces can transform independently.
      for(const member of native) {
        const record=recordFor(member,candidate)||{};
        if(record.backing!==undefined||(record['back-art']!==undefined&&record['back-art']!=='auto'))throw Error(
          'Reset backing-only overrides before moving or resizing this native structural group.');
        if(member.transformSupported===false)throw Error(member.transformReason||hint);
      }
      const token=records[0].transform;
      if(records.some(record=>record.transform!==token))throw Error(hint+
        ' Every member needs one shared move/resize transform.');
    }
  }
  function setTransform(spec,label='group move / resize') {
    if(!selection.length||selection[0].kind!=='tile')throw Error('Select the tiles forming an object first.');
    const unsupported=selection.find(item=>(item.transformSupported===undefined?metadata(item):item).transformSupported===false);
    if(unsupported)throw Error(unsupported.transformReason||'Move / resize is unavailable for this material.');
    const value=parseTransform(transformText(spec)),context=transformContext(),previous=context.control;
    if(transformText(value)===transformText(previous)&&label!=='group pivot'&&
        !selection.some(item=>recordFor(item)?.transform))return;
    const proposed=new Map(selection.map(item=>[key(item),transformText(context.baselines?
      composeTransform(value,context.baselines.get(key(item))):value)]));
    context.control=value;transformEditing=true;
    try {mutate(selection,(item,base)=>({...base,transform:proposed.get(key(item))}),label);}
    catch(error){context.control=previous;throw error;}
    finally{transformEditing=false;}
    updateTransformHandles();overlay();
  }
  function chooseTransformPivot(preset=element('TransformPivot').value) {
    if(!['bottom','center'].includes(preset))return;
    const old=transformContext().control,next=defaultTransform(selectedBounds(),preset);
    for(const axis of ['x','y','z']) {
      next['s'+axis]=old['s'+axis];
      next['d'+axis]=old['d'+axis]+(1-old['s'+axis])*(old['p'+axis]-next['p'+axis]);
    }
    pivotPreset=preset;setTransform(next,'group pivot');
  }
  function resizeTransform(field,value) {
    const old=transformContext().control,next={...old,[field]:numeric(value,12.5,400,'Scale percent')/100};
    if(element('TransformLock').checked) {
      const ratio=next[field]/old[field];for(const axis of ['sx','sy','sz'])if(axis!==field)next[axis]=old[axis]*ratio;
    }
    setTransform(next,'group resize');
  }
  function updateTransformHandles() {
    transformHandles=null;
    if(tool!=='transform'||selection[0]?.kind!=='tile'||selection.some(item=>item.supported===false||item.transformSupported===false)||
        typeof SharedRoomPreview.voxelTransformHandles!=='function')return;
    const t=transformContext().control,area=selectedBounds();
    const result=SharedRoomPreview.voxelTransformHandles(area.x,area.y,area.width,area.height,...transformFields.map(field=>t[field]));
    if(result?.supported!==false)transformHandles=result;
    else status(result.reason||'Move / resize is unavailable for this surface.',true);
  }
  function bendDomain(side,area=selectedBounds()) {
    if(!area)throw Error('Select the whole limb first.');
    if(!['left','right','top','bottom'].includes(side))throw Error('Choose the attached side.');
    const axis=side==='left'||side==='right'?'x':'y',start=axis==='x'?area.x:area.y;
    const end=start+(axis==='x'?area.width:area.height),reverse=side==='right'||side==='bottom';
    return {axis,anchor:reverse?end:start,tip:reverse?start:end,dx:0,dy:0,dz:0};
  }
  function currentBend() {
    const first=selection[0];if(first?.kind!=='tile')return null;
    const value=recordFor(first)?.bend;
    return value?parseBend(value):bendDomain(pullSide);
  }
  function preparePullSelection() {
    pullHandles=null;const bounds=selectedBounds();
    if(selection[0]?.kind!=='tile'||!bounds)return;
    const value=recordFor(selection[0])?.bend;
    pullSide=value?bendSide(parseBend(value)):bounds.width>=bounds.height?'left':'bottom';
  }
  function setBend(spec,label='attached pull') {
    if(!selection.length||selection[0].kind!=='tile')throw Error('Select the BG1 tiles forming the limb first.');
    const value=bendText(parseBend(bendText(spec)));
    mutate(selection,(_,base)=>({...base,bend:value}),label);
  }
  function choosePullAnchor(side=element('PullAnchor').value) {
    const previous=currentBend(),domain=bendDomain(side);
    setBend({...domain,dx:previous?.dx||0,dy:previous?.dy||0,dz:previous?.dz||0},'attached base');
  }
  function resetPullTip() {
    mutate(selection,(_,base)=>{
      if(base.bend)base.bend=bendText({...parseBend(base.bend),dx:0,dy:0,dz:0});
      return base;
    },'reset tip position');
  }
  function updatePullHandles() {
    pullHandles=null;
    if(tool!=='attached'||selection[0]?.kind!=='tile'||selection.some(item=>item.supported===false)||
        typeof SharedRoomPreview.voxelBendHandles!=='function')return;
    const b=currentBend(),area=selectedBounds();
    const result=SharedRoomPreview.voxelBendHandles(area.x,area.y,area.width,area.height,
      b.axis==='x'?1:2,b.anchor,b.tip,b.dx,b.dy,b.dz);
    if(result?.supported!==false)pullHandles=result;
    else status(result.reason||'Attached pull is unavailable for this surface.',true);
  }
  function edit(field,value,items=selection) {
    if(!items.length)return;
    if(items[0].kind==='object') {
      if(field==='mode'&&value==='extruded'&&items.some(item=>item.canExtrude===false))throw Error('This captured object cannot be extruded.');
      if(value!==undefined&&['offset','thickness','width'].includes(field)&&!items.every(modeled))throw Error('Choose Extruded before editing the native sprite’s geometry.');
    }
    const area=selectedBounds(items);
    mutate(items,(item,base)=>{
      const proposed=typeof value==='function'?value(item,base):value;
      if(proposed===undefined)delete base[field];else base[field]=proposed;
      if(field==='shape') {
        delete base['front-end'];delete base['back-end'];
        if(proposed==='box'||proposed===undefined)delete base['shape-span'];else base['shape-span']=`${area.y},${area.height}`;
      }
      if(item.kind==='tile'&&['front','back'].includes(field)&&proposed!==undefined&&element('KeepThickness')?.checked) {
        metadata(item);
        const old=recordFor(item)||{},delta=Number(proposed)-(old[field]??item[field]??0),other=field==='front'?'back':'front';
        base[other]=(old[other]??item[other]??0)+delta;
        for(const endpoint of ['front-end','back-end'])if(old[endpoint]!==undefined)base[endpoint]=old[endpoint]+delta;
      }
      return base;
    },field);
  }
  function resetSelected() {
    if(!selection.length)return;
    mutate(selection,item=>item.kind==='object'?{mode:'auto'}:{},'reset selected');status('Automatic material settings previewed. Apply to keep the reset.');
  }
  function discard() {if(typeof closeTilePalette==='function')closeTilePalette(false);draft=null;draftRoom='';previewUndo=[];previewRedo=[];previewGesture=null;gesture=null;previewSession=null;conversionPreview=null;transformSession=null;transformGroupCache=new Map();status('Draft discarded. Applied settings restored.');refreshHistoryButtons();invalidate();}
  function apply() {
    if(typeof finishVoxelFacePaletteForHistory==='function'&&!finishVoxelFacePaletteForHistory())return false;
    endGesture();conversionPreview=null;transformSession=null;
    if(!pending())return false;
    try {
      lines(room,draft);SharedRoomPreview.validateScenery(previewSection(room,roomSectionIni(room)));
      commitOp();
      const op={label:'voxel visual edits',voxel:{room:roomKey(room),profile:profile(room),before:clone(state(room)),after:clone(draft)}};
      roomConfig(room).voxel=clone(draft);draft=null;draftRoom='';previewUndo=[];previewRedo=[];
      undoStack.push(op);if(undoStack.length>kMaxUndo)undoStack.shift();redoStack.length=0;
      markEditorChanged();refreshHistoryButtons();status('Applied. Save project to keep these edits on disk.');invalidate();return true;
    } catch(error){status(error.message,true);return false;}
  }
  function restore(op,side) {
    const target=DATA.rooms.find(r=>roomKey(r)===op.room);if(!target)return false;
    roomConfig(target).voxel=clone(op[side]);transformSession=null;transformGroupCache=new Map();
    if(roomKey(room)!==op.room||profile(room)!==op.profile) {
      selection=[];terrainProfile=op.profile;room=terrainRoom(target,terrainProfile);
      $('#room').value=String(DATA.rooms.indexOf(target));setLayer(0);
    }
    markEditorChanged();invalidate();return true;
  }
  function guard(action='changing rooms') {
    if(!pending())return true;
    active=true;if(mode!=='shared')setMode('shared');refresh();
    status(`Apply or discard the voxel preview before ${action}.`,true);element('Apply')?.focus();return false;
  }
  function clearLoaded() {
    previewSession=null;conversionPreview=null;transformSession=null;transformGroupCache=new Map();transformHandles=null;selectionLabel='';selectionPivotDepth=null;pivotPreset='bottom';
    draft=null;draftRoom='';selection=[];objects=[];gesture=null;previewGesture=null;previewUndo=[];previewRedo=[];tool='select';sample=null;navigation=-1;pullHandles=null;pullSide='left';status('');
    const panel=element('Panel');if(panel){panel.style.left='10px';panel.style.right='auto';}
    element('Objects').dataset.signature='';
    showObjects();refresh();
  }
  function dockInspector() {
    const quad=selection[0]?.quad,panel=element('Panel');if(!quad?.length||!panel)return;
    // Dock only when selection changes. Camera orbit must not make controls
    // jump while they are being used, and the selected handle stays exposed.
    const x=quad.reduce((sum,point)=>sum+point[0],0)/quad.length;
    const right=x<$('#sharedGl').width/2;
    panel.style.left=right?'auto':'10px';panel.style.right=right?'10px':'auto';
  }
  function select(item,add=false,continueGesture=false) {
    finishOutsideArtwork();if(!continueGesture)endGesture();conversionPreview=null;
    selectionLabel='';selectionPivotDepth=null;transformSession=null;transformHandles=null;
    if(!item){if(!add){selection=[];pullHandles=null;refresh();overlay();}return;}
    if(item.supported===false&&!item.kind) {
      selection=[];refresh();overlay();status(item.reason||'This surface has no editable voxel geometry.',true);return;
    }
    if(!add||selection[0]?.kind!==item.kind)selection=[];
    if(item.kind!=='tile'&&['attached','transform'].includes(tool))tool='select';
    const index=selection.findIndex(other=>key(other)===key(item));
    if(index>=0&&add)selection.splice(index,1);else if(index<0)selection.push(item);
    preparePullSelection();prepareTransformSelection();status('');dockInspector();refresh();updatePullHandles();updateTransformHandles();overlay();
  }
  function selectWhole(item=selection[0],add=false) {
    if(!item||item.kind!=='tile'){select(item,add);return;}
    let found;
    try {found=SharedRoomPreview.voxelGroup?.(item.x,item.y);}
    catch(error){select(item,add);status(`Whole-object selection unavailable: ${error.message} Tile selected.`,true);return;}
    if(!found||found.supported===false||found.status&&found.status!=='ok'||!found.cells?.length) {
      select(item,add);status(found?.reason||'No bounded object group here. Tile selected; use a Map rectangle or refine the selection.');return;
    }
    if(found.cells.length>4096){select(item,add);status('This group is too large. Select a smaller Map rectangle.',true);return;}
    finishOutsideArtwork();endGesture();conversionPreview=null;
    const members=new Map(found.cells.map(([x,y])=>[`${x},${y}`,{kind:'tile',x,y,face:item.face||'front',quad:[],
      ...(found.transformSupported===undefined?{}:{transformSupported:found.transformSupported,transformReason:found.transformReason})}]));
    if(!members.has(key(item)))members.set(key(item),item);else members.set(key(item),{...members.get(key(item)),...item});
    const existing=new Map((add&&selection[0]?.kind==='tile'?selection:[]).map(value=>[key(value),value]));
    const remove=add&&[...members.keys()].every(id=>existing.has(id));
    for(const [id,value] of members)if(remove)existing.delete(id);else existing.set(id,value);
    if(existing.size>65536)throw Error('Select at most 65,536 cells in one operation.');
    selection=[...existing.values()];
    // Put the actual clicked face first for inspector docking and projection.
    const seed=selection.findIndex(value=>key(value)===key(item));
    if(seed>0)selection.unshift(selection.splice(seed,1)[0]);
    selectionLabel=add?'Combined selection':found.label||'Whole object';
    selectionPivotDepth=add?null:found.pivotDepth??null;
    preparePullSelection();prepareTransformSelection();status('');dockInspector();refresh();updatePullHandles();updateTransformHandles();overlay();draw();
  }
  function selectPicked(item,add=false) {
    if(element('SelectScope').value!=='tile'&&item?.kind==='tile')selectWhole(item,add);else select(item,add);
  }
  function refineSelection(grow) {
    if(selection[0]?.kind!=='tile')return;
    const cells=new Map(selection.map(item=>[key(item),[item.x,item.y]])),next=new Map();
    for(const [id,[x,y]] of cells) {
      const neighbors=[[x-1,y],[x+1,y],[x,y-1],[x,y+1]];
      if(grow) {
        next.set(id,[x,y]);
        for(const [nx,ny] of neighbors)if(nx>=-512&&ny>=-512&&nx<=511&&ny<=511)next.set(`${nx},${ny}`,[nx,ny]);
      } else if(neighbors.every(([nx,ny])=>cells.has(`${nx},${ny}`)))next.set(id,[x,y]);
    }
    if(!next.size){status('The selection is one tile thick. Use Tile mode and Shift-click to refine it.');return;}
    selectCells([...next.values()]);selectionLabel='Manual selection';refresh();
    status(grow?'Added one cell border. Review its outline before editing.':'Removed the outside cell border.');
  }
  function showObjects() {
    const list=element('Objects');if(!list)return;
    list.replaceChildren();
    element('ObjectCount').textContent=objects.length?String(objects.length):'No capture';
    const blank=document.createElement('option');blank.value='';blank.textContent=objects.length?'Select a captured object…':'No capture loaded';list.appendChild(blank);
    for(const item of objects) {
      const option=document.createElement('option');option.value=String(item.index);
      option.textContent=`${item.label||'Native object'} · ${Math.round(item.x)}, ${Math.round(item.y)}${item.supported===false||item.canExtrude===false?' · inspect only':''}`;list.appendChild(option);
    }
  }
  function metadata(item) {
    if(item.kind!=='tile'||item.front!==undefined)return item;
    const found=SharedRoomPreview.voxelTile(item.x,item.y,-1);
    if(!found)throw Error('This cell has no editable voxel geometry. Curved Northwall rooms use Tree enclosure controls.');
    Object.assign(item,found);if(found.supported===false)throw Error(found.reason||'This cell uses dedicated enclosure controls.');return item;
  }
  function effective(item) {
    metadata(item);const record=recordFor(item)||{},out={...record};delete out.x;delete out.y;delete out.w;delete out.h;
    if(item.kind==='object')return {...out,mode:record.mode??item.mode??'auto',offset:record.offset??item.offset??0,
      thickness:record.thickness??item.thickness??0,width:record.width??item.widthScale??1};
    for(const name of ['front','back','width','offset','backing'])out[name]=record[name]??item[name]??({width:16,backing:'auto'}[name]??0);
    for(const name of artFields)out[name]=record[name]??item[name.replace('-art','Art')]??'auto';
    out.faces=record.faces??(typeof item.faces==='number'?(item.faces===63?'all':item.faces===0?'none':faceNames.filter((_,i)=>item.faces&(1<<i)).join('+')):'all');
    out.shape=record.shape??item.shape??'box';
    if(out.shape!=='box') {
      out['shape-span']=record['shape-span']??`${item.shapeY??item.y*16},${item.shapeH??16}`;
      out['front-end']=record['front-end']??item.frontEnd??out.back;out['back-end']=record['back-end']??item.backEnd??out.back;
    }
    if(record.bend)out.bend=record.bend;
    return out;
  }
  function sampleSettings(item=selection[0]) {
    if(!item)return;
    const values=effective(item);
    if(item.kind==='tile') {
      const hit={...item};for(const name of artFields)hit[name.replace('-art','Art')]=values[name];
      const sampled=paletteSampleFaceArt(hit);
      if(sampled)values[(item.face||'front')+'-art']=paletteArtValue(sampled.art);
    }
    sample={kind:item.kind,values,bounds:selectedBounds(selection.some(value=>key(value)===key(item))?selection:[item]),
      label:item.kind==='tile'?`cell ${item.x}, ${item.y}`:item.label||'native object'};
    tool='select';status(`Sampled ${sample.label}. Apply to selection or paint matching settings.`);refresh();
  }
  function copiedFields() {
    const names=[];
    if(element('CopyDepth').checked)names.push(...(sample?.kind==='object'?['offset','thickness']:['front','back']));
    if(element('CopyPlacement').checked)names.push(...(sample?.kind==='object'?['width']:['width','offset']));
    if(element('CopyBacking').checked)names.push('backing','back-art');
    if(element('CopyArt').checked)names.push(...artFields.filter(name=>name!=='back-art'));
    if(element('CopyFaces').checked)names.push('faces');
    if(element('CopyShape').checked)names.push('shape','shape-span','front-end','back-end','bend','mode');
    if(element('CopyTransform').checked)names.push('transform');
    const allowed=sample?.kind==='object'?['mode','offset','thickness','width']:cellFields;
    return [...new Set(names)].filter(name=>allowed.includes(name));
  }
  function applySample(items=selection,depthOnly=false) {
    if(!sample)throw Error('Sample a surface first.');
    if(items.some(item=>item.kind!==sample.kind))throw Error('Sample and target must both be tiles, or both native objects.');
    const fields=depthOnly?(sample.kind==='tile'?['front','back']:['offset']):copiedFields();
    if(!fields.length)throw Error('Choose which sampled settings to paint.');
    const area=selectedBounds(items);
    mutate(items,(item,base)=>{
      for(const field of fields) {
        if(sample.values[field]!==undefined)base[field]=sample.values[field];else delete base[field];
      }
      if(item.kind==='object'&&base.mode==='extruded'&&item.canExtrude===false)throw Error('This captured object cannot be extruded.');
      if(item.kind==='object'&&fields.some(field=>['offset','thickness','width'].includes(field))&&
          !(base.mode==='extruded'||(base.mode??item.mode??'auto')==='auto'&&item.active))throw Error('Choose Extruded before painting geometry onto a native flat sprite.');
      if(base.shape==='box'){delete base['shape-span'];delete base['front-end'];delete base['back-end'];}
      else if(fields.includes('shape')&&area)base['shape-span']=`${area.y},${area.height}`;
      if(fields.includes('bend')&&base.bend&&area) {
        const bend=parseBend(base.bend),domain=bendDomain(bendSide(bend),area);
        base.bend=bendText({...bend,anchor:domain.anchor,tip:domain.tip});
      }
      if(fields.includes('transform')&&base.transform&&area&&sample.bounds) {
        const t=parseTransform(base.transform),source=sample.bounds;
        t.px=area.x+(t.px-source.x)/source.width*area.width;
        t.py=area.y+(t.py-source.y)/source.height*area.height;
        base.transform=transformText(parseTransform(transformText(t)));
      }
      return base;
    },depthOnly?'match depth':'paint settings');
  }
  function convert(preset=element('Preset').value||'slab') {
    if(!selection.length||selection[0].kind!=='tile')throw Error('Select BG1 tiles to convert.');
    const amount=numeric(element('PresetDepth').value,.005,.8,'Conversion depth');
    const signature=roomKey(room)+':'+profile(room)+':'+selection.map(key).sort().join('|');
    const before=JSON.stringify(current());
    // Choosing a preset/amount previews against the same starting depths.
    // Otherwise typing .1 then .2 into Recess would accumulate .3 of movement.
    // Any intervening edit or selection/history change starts a new preview.
    if(!conversionPreview||conversionPreview.signature!==signature||conversionPreview.result!==before)
      conversionPreview={signature,result:before,sources:new Map(selection.map(item=>[key(item),effective(item)]))};
    const sources=conversionPreview.sources;
    mutate(selection,(item,base)=>{
      const source=sources.get(key(item)),far=source.back;
      if(preset==='flat')base.front=base.back=far;
      else if(preset==='slab'){base.front=Math.max(-.4,far-amount);base.back=far;}
      else if(preset==='thin'){base.front=source.front;base.back=Math.min(.8,source.front+Math.min(amount,.025));}
      else if(preset==='recess'){
        const delta=Math.min(amount,.8-Math.max(source.front,source.back));base.front=source.front+delta;base.back=source.back+delta;
      } else throw Error('Choose a conversion preset.');
      base.shape='box';delete base['shape-span'];delete base['front-end'];delete base['back-end'];return base;
    },`${preset} conversion`);
    conversionPreview.result=JSON.stringify(current());
  }
  function selectCells(positions,{focus=false}={}) {
    if(!positions.length)throw Error('Select some BG1 tiles first.');
    if(positions.length>65536)throw Error('Select at most 65,536 cells in one operation.');
    finishOutsideArtwork();endGesture();conversionPreview=null;selectionLabel='Manual selection';selectionPivotDepth=null;
    selection=[...new Map(positions.map(([x,y])=>[`${x},${y}`,{kind:'tile',x,y,face:'front',quad:[]}])).values()];
    active=true;element('Mode').value='2';setMode('shared');SharedRoomPreview.cancelPlacement();metadata(selection[0]);
    preparePullSelection();prepareTransformSelection();
    if(focus)SharedRoomPreview.focusVoxelSelection(visibleBounds());
    refresh();draw();return selection.length;
  }
  function fromMapSelection() {
    try {if(bgIndex!==0)throw Error('Voxel conversion targets BG1. Choose BG1 and select the tiles to edit.');
      selectCells(tileSelectionPositions(),{focus:true});status('Map selection ready. Adjust depth, repair backing, or choose a conversion preset.');
    }catch(error){status(error.message,true);tileActionStatus(error.message);}
  }
  function coordinateSelection() {
    const x=numeric(element('CellX').value,-512,511,'Cell X',true),y=numeric(element('CellY').value,-512,511,'Cell Y',true);
    const w=numeric(element('CellW').value,1,1024,'Selection width',true),h=numeric(element('CellH').value,1,1024,'Selection height',true);
    if(w*h>65536||x+w>512||y+h>512)throw Error('Selection exceeds the native cell bounds or selection limit.');
    selectCells(Array.from({length:w*h},(_,i)=>[x+i%w,y+Math.floor(i/w)]),{focus:true});
  }
  function navigate(index) {
    const records=current().cells[profile(room)];if(!records.length)return;
    navigation=(index+records.length)%records.length;const r=records[navigation],b=VoxelRecords.bounds(r);
    selectCells(Array.from({length:b.w*b.h},(_,i)=>[b.x+i%b.w,b.y+Math.floor(i/b.w)]),{focus:true});refresh();
  }
  function drawMapOverlay(ctx,view) {
    if(!element('HighlightEdits')?.checked||bgIndex!==0)return;
    ctx.save();ctx.strokeStyle='#f0c55a';ctx.fillStyle='#f0c55a18';ctx.lineWidth=1.5;
    for(const r of current().cells[profile(room)]) {
      const b=VoxelRecords.bounds(r),x=view.x+b.x*16*view.scale,y=view.y+b.y*16*view.scale;
      ctx.fillRect(x,y,b.w*16*view.scale,b.h*16*view.scale);ctx.strokeRect(x,y,b.w*16*view.scale,b.h*16*view.scale);
    }
    ctx.restore();
  }
  function resetProperty() {
    const field=element('ResetProperty').value;
    if(!selection.length)return;
    if(selection[0].kind==='object'&&!['depth','width','offset','mode','thickness'].includes(field))throw Error('Choose a property available on native objects.');
    if(selection[0].kind==='tile'&&['mode','thickness'].includes(field))throw Error('Choose a property available on BG1 tiles.');
    if(field==='depth')mutate(selection,(item,base)=>{if(item.kind==='object')delete base.offset;else{delete base.front;delete base.back;}return base;},'reset depth');
    else if(field==='art')mutate(selection,(_,base)=>{for(const name of artFields)delete base[name];return base;},'reset face artwork');
    else edit(field,undefined);
  }
  function overlay() {
    const svg=element('Overlay'),canvas=$('#sharedGl');if(!svg)return;
    const hidden=!active||mode!=='shared'||Number(element('Mode').value)!==2;
    // SVGElement does not reflect HTML's .hidden property. Remove the actual
    // attribute or the page's [hidden] rule keeps every outline invisible.
    if(hidden)svg.setAttribute('hidden','');else svg.removeAttribute('hidden');
    svg.setAttribute('viewBox',`0 0 ${canvas.width||960} ${canvas.height||600}`);svg.replaceChildren();
    for(const item of selection) {
      if(!item.quad?.length)continue;
      const polygon=document.createElementNS('http://www.w3.org/2000/svg','polygon');
      polygon.setAttribute('points',item.quad.map(p=>p.join(',')).join(' '));svg.appendChild(polygon);
    }
    if(tool==='attached'||tool==='transform') {
      const marker=(tag,attributes,color)=>{
        const node=document.createElementNS('http://www.w3.org/2000/svg',tag);
        for(const [name,value] of Object.entries(attributes))node.setAttribute(name,value);
        if(color){node.style.stroke=color;if(tag==='circle'||tag==='rect')node.style.fill=color;}
        svg.appendChild(node);return node;
      };
      if(tool==='attached'&&pullHandles?.anchor&&pullHandles?.tip) {
        const a=pullHandles.anchor,t=pullHandles.tip;
        marker('line',{x1:a[0],y1:a[1],x2:t[0],y2:t[1],'stroke-dasharray':'4 4'},'#c4c9d1');
        marker('rect',{id:'voxelPullBaseMarker',x:a[0]-5,y:a[1]-5,width:10,height:10},'#f8f5dc');
        marker('text',{x:a[0]+10,y:a[1]-8}).textContent='Fixed base';
      }
      if(tool==='transform'&&transformHandles?.pivot) {
        const p=transformHandles.pivot;
        marker('rect',{id:'voxelTransformPivotMarker',x:p[0]-5,y:p[1]-5,width:10,height:10},'#f8f5dc');
        marker('text',{x:p[0]+10,y:p[1]-8}).textContent='Group pivot';
      }
      const handles=tool==='attached'?pullHandles:transformHandles;
      if(handles) {
        for(const handle of handles.handles||[]) {
          const [x,y]=handle.end,color={x:'#ff8981',y:'#9de48d',z:'#8ebfff'}[handle.axis];
          marker('line',{x1:handle.origin[0],y1:handle.origin[1],x2:x,y2:y,...(handle.fallback?{'stroke-dasharray':'4 3'}:{})},color);
          marker('circle',{id:(tool==='attached'?'voxelPullHandle':'voxelTransformHandle')+handle.axis.toUpperCase(),cx:x,cy:y,r:8},color);
          marker('text',{x:x+12,y:y+4}).textContent=handle.axis.toUpperCase()+(handle.fallback?' control':'');
        }
      }
      return;
    }
    const first=selection[0];if(!first?.quad?.length)return;
    const field=first.kind==='tile'&&first.face==='back'?'back':'front';
    const center=first[field+'DepthOrigin']||first.quad.reduce((sum,p)=>[sum[0]+p[0]/first.quad.length,sum[1]+p[1]/first.quad.length],[0,0]);
    const end=first[field+'DepthHandle']||first.depthHandle;
    if(!end){first.handle=null;return;}
    const line=document.createElementNS('http://www.w3.org/2000/svg','line');
    for(const [name,value] of Object.entries({x1:center[0],y1:center[1],x2:end[0],y2:end[1]}))line.setAttribute(name,value);
    svg.appendChild(line);
    const circle=document.createElementNS('http://www.w3.org/2000/svg','circle');
    circle.setAttribute('cx',end[0]);circle.setAttribute('cy',end[1]);circle.setAttribute('r','9');svg.appendChild(circle);
    const label=document.createElementNS('http://www.w3.org/2000/svg','text');label.setAttribute('x',end[0]+13);label.setAttribute('y',end[1]+4);label.textContent=field==='back'?'Drag back depth':'Drag front depth';svg.appendChild(label);
    first.handle=end;
  }
  function afterRender() {
    if(!active||mode!=='shared')return;
    try {
      objects=SharedRoomPreview.voxelObjects();
      const signature=objects.map(o=>`${o.index}:${o.source}:${o.parent}:${o.animation}`).join('|');
      if(element('Objects').dataset.signature!==signature){element('Objects').dataset.signature=signature;showObjects();}
      // Keep direct coordinate/Map selection cheap even for whole walls.
      // Only query actual projected faces near the camera, with a bounded
      // overlay count. All selected cells still participate in edits.
      let queried=0;
      selection=selection.map((item,index)=>{
        if(item.kind!=='tile')return objects.find(o=>o.index===item.index)||item;
        const near=index===0||Math.abs(item.x*16-nativeCamera.x)<1100&&Math.abs(item.y*16-nativeCamera.y)<800;
        if(!near||queried>=160)return {...item,quad:[],handle:null};
        queried++;const found=SharedRoomPreview.voxelTile(item.x,item.y,faceEnum[item.face]??0);
        return found?{...item,sampleSource:null,sampleSourceAmbiguous:false,...found}:
          {...item,sampleSource:null,sampleSourceAmbiguous:false,quad:[],depthHandle:null,handle:null};
      });
      refresh();updatePullHandles();updateTransformHandles();overlay();
    } catch(error){status(error.message,true);}
  }
  function pointerDown(event) {
    if(!active||Number(element('Mode').value)!==2||event.altKey||event.button!==0)return false;
    const point=sharedPreviewCanvasPoint($('#sharedGl'),event);if(!point)return true;
    try {
      if(tool==='attached'||tool==='transform') {
        const handles=tool==='attached'?pullHandles:transformHandles;
        const handle=(handles?.handles||[]).map(h=>({h,d:Math.hypot(point.x-h.end[0],point.y-h.end[1])}))
          .filter(h=>h.d<15).sort((a,b)=>a.d-b.d)[0]?.h;
        if(handle) {
          const rect=$('#sharedGl').getBoundingClientRect(),scale=Math.min(rect.width/$('#sharedGl').width,rect.height/$('#sharedGl').height);
          beginPreviewGesture(`${tool==='attached'?'attached':'group move'} ${handle.axis.toUpperCase()} drag`);
          gesture={pointer:event.pointerId,pull:tool==='attached',transform:tool==='transform',x:event.clientX,y:event.clientY,field:'d'+handle.axis,
            bend:tool==='attached'?currentBend():null,spec:tool==='transform'?clone(transformContext().control):null,
            axis:[handle.end[0]-handle.origin[0],handle.end[1]-handle.origin[1]],scale,delta:handle.step};
          $('#sharedGl').setPointerCapture(event.pointerId);event.preventDefault();return true;
        }
        selectPicked(SharedRoomPreview.voxelPick(point.x,point.y,Number(element('Pick').value)),event.shiftKey);
        event.preventDefault();return true;
      }
      if(tool!=='select') {
        const hit=SharedRoomPreview.voxelPick(point.x,point.y,Number(element('Pick').value));
        if(hit?.kind) {
          if(tool==='sample'){sampleSettings(hit);select(hit);}
          else {
            beginPreviewGesture('paint stroke');gesture={pointer:event.pointerId,paint:true,visited:new Set([key(hit)]),items:[hit]};
            applySample([hit]);select(hit,false,true);$('#sharedGl').setPointerCapture(event.pointerId);
          }
        }
        event.preventDefault();return true;
      }
      const first=selection[0],handle=first?.handle;
      if(handle&&Math.hypot(point.x-handle[0],point.y-handle[1])<15) {
        const record=recordFor(first)||{},field=first.kind==='tile'?(first.face==='back'?'back':'front'):'offset';
        const center=first[field+'DepthOrigin']||first.quad.reduce((sum,p)=>[sum[0]+p[0]/first.quad.length,sum[1]+p[1]/first.quad.length],[0,0]);
        const axis=[handle[0]-center[0],handle[1]-center[1]],rect=$('#sharedGl').getBoundingClientRect();
        const scale=Math.min(rect.width/$('#sharedGl').width,rect.height/$('#sharedGl').height);
        const starts=new Map(selection.map(item=>[key(item),recordFor(item)?.[field]??metadata(item)[field]??0]));
        beginPreviewGesture('depth drag');
        gesture={pointer:event.pointerId,x:event.clientX,y:event.clientY,field,value:record[field]??first[field]??0,
          axis,scale,delta:first.depthDelta||.1,starts};
        $('#sharedGl').setPointerCapture(event.pointerId);event.preventDefault();return true;
      }
      selectPicked(SharedRoomPreview.voxelPick(point.x,point.y,Number(element('Pick').value)),event.shiftKey);
    }catch(error){status(error.message,true);}
    event.preventDefault();return true;
  }
  function pointerMove(event) {
    if(!gesture)return false;
    try {
      if(gesture.paint) {
        const point=sharedPreviewCanvasPoint($('#sharedGl'),event),hit=point&&SharedRoomPreview.voxelPick(point.x,point.y,Number(element('Pick').value));
        if(hit?.kind&&!gesture.visited.has(key(hit))){gesture.visited.add(key(hit));gesture.items.push(hit);
          // A copied connected profile shares one domain across the whole
          // stroke instead of restarting its taper at every painted row.
          applySample(element('CopyShape').checked||element('CopyTransform').checked?gesture.items:[hit]);select(hit,false,true);}
      } else {
        const axis=gesture.axis,denominator=axis[0]*axis[0]+axis[1]*axis[1];
        const distance=((event.clientX-gesture.x)*axis[0]+(event.clientY-gesture.y)*axis[1])/gesture.scale;
        const snap=(gesture.pull||gesture.transform)&&gesture.field!=='dz'?1:Number(element('Snap').value),raw=denominator>1?distance/denominator*gesture.delta:0;
        const delta=snap?Math.round(raw/snap)*snap:raw;
        if(gesture.transform) {
          const limit=gesture.field==='dz'?1.2:256;
          setTransform({...gesture.spec,[gesture.field]:Number(number(Math.max(-limit,Math.min(limit,gesture.spec[gesture.field]+delta))))},'group move drag');
        } else if(gesture.pull) {
          const limit=gesture.field==='dz'?1.2:256;
          setBend({...gesture.bend,[gesture.field]:Number(number(Math.max(-limit,Math.min(limit,gesture.bend[gesture.field]+delta))))});
        } else edit(gesture.field,item=>Number(number(Math.max(-.4,Math.min(.8,gesture.starts.get(key(item))+delta)))));
      }
    }catch(error){status(error.message,true);}
    event.preventDefault();return true;
  }
  function endGesture(){
    const pointer=gesture?.pointer;gesture=null;
    const canvas=$('#sharedGl');if(pointer!==undefined&&canvas.hasPointerCapture?.(pointer))canvas.releasePointerCapture(pointer);
    finishPreviewGesture();refresh();
  }
  function deactivate(){active=false;endGesture();refresh();overlay();}
  function keyDown(event) {
    if(!active||event.key!=='Escape')return false;
    endGesture();select(null);event.preventDefault();return true;
  }
  function init() {
    const safely=action=>()=>{try{action();}catch(error){status(error.message,true);}};
    for(const [id,value] of [['PresetDepth','.1'],['CellX','0'],['CellY','0'],['CellW','1'],['CellH','1']])if(!element(id).value)element(id).value=value;
    element('CopyDepth').checked=true;element('HighlightEdits').checked=true;
    element('SelectScope').value='whole';element('TransformLock').checked=false;
    element('SelectScope').onchange=()=>{endGesture();status(element('SelectScope').value==='tile'?'Click individual tiles; Shift-click refines the selection.':'Click to select a complete native object when one is available.');};
    element('SelectWhole').onclick=safely(()=>selectWhole());
    element('Grow').onclick=safely(()=>refineSelection(true));element('Shrink').onclick=safely(()=>refineSelection(false));
    element('Edit').onclick=()=>{active=!active;if(active)SharedRoomPreview.cancelPlacement();refresh();overlay();
      if(typeof updateLegend==='function')updateLegend();};
    element('Mode').onchange=()=>{refresh();draw();overlay();};
    element('Objects').onchange=()=>select(objects.find(item=>String(item.index)===element('Objects').value));
    element('LoadObjects').onclick=()=>$('#sharedLoadObjects').click();
    element('Apply').onclick=apply;element('Discard').onclick=discard;element('Reset').onclick=safely(resetSelected);
    element('Tool').onchange=safely(()=>{const value=element('Tool').value;endGesture();tool=value;
      status(tool==='sample'?'Click a face to sample its visual settings.':tool==='paint'?'Drag across faces to paint the checked settings.':
        tool==='attached'?'Select the whole limb. Choose its attached side, then drag an X, Y or Z tip handle.':
        tool==='transform'?'Move with the X, Y and Z handles; resize the entire selection around its shared pivot.':'');
      refresh();updatePullHandles();updateTransformHandles();overlay();draw();});
    element('TransformPivot').onchange=safely(()=>chooseTransformPivot());
    element('TransformReset').onclick=safely(()=>{endGesture();edit('transform',undefined);});
    for(const field of ['dx','dy','dz','sx','sy','sz']) {
      const input=element('Transform'+field[0].toUpperCase()+field.slice(1));
      input.onfocus=input.onpointerdown=()=>beginPreviewGesture(field[0]==='s'?'group resize':'group position');
      input.onchange=input.onblur=finishPreviewGesture;
      input.oninput=safely(()=>{
        if(field[0]==='s')resizeTransform(field,input.value);
        else {const limit=field==='dz'?1.2:256;setTransform({...transformContext().control,[field]:numeric(input.value,-limit,limit,field)},'group position');}
      });
    }
    element('PullAnchor').onchange=safely(()=>choosePullAnchor());
    element('PullReset').onclick=safely(resetPullTip);
    for(const [id,field,limit] of [['PullX','dx',256],['PullY','dy',256],['PullZ','dz',1.2]]) {
      const input=element(id);input.onfocus=input.onpointerdown=()=>beginPreviewGesture('attached tip');
      input.onchange=input.onblur=finishPreviewGesture;
      input.oninput=safely(()=>setBend({...currentBend(),[field]:numeric(input.value,-limit,limit,'Tip '+field.slice(1).toUpperCase())}));
    }
    element('Sample').onclick=safely(()=>sampleSettings());element('Paste').onclick=safely(()=>applySample());
    element('MatchDepth').onclick=safely(()=>applySample(selection,true));
    element('Preset').onchange=safely(()=>convert());
    element('PresetDepth').onfocus=element('PresetDepth').onpointerdown=()=>beginPreviewGesture('conversion amount');
    element('PresetDepth').oninput=safely(()=>{if(element('Preset').value)convert();});
    element('PresetDepth').onchange=element('PresetDepth').onblur=finishPreviewGesture;
    element('SelectCells').onclick=safely(coordinateSelection);element('Focus').onclick=safely(()=>SharedRoomPreview.focusVoxelSelection(visibleBounds()));
    element('Navigate').onchange=safely(()=>navigate(Number(element('Navigate').value)));
    element('Previous').onclick=safely(()=>navigate(navigation-1));element('Next').onclick=safely(()=>navigate(navigation+1));
    element('HighlightEdits').onchange=()=>draw();element('ResetOne').onclick=safely(resetProperty);
    element('Undo').onclick=undoPreview;element('Redo').onclick=redoPreview;
    for(const kind of ['Tile','Object'])for(const field of kind==='Tile'?['front','back','width','offset']:['offset','thickness','width']) {
      const id=kind+field[0].toUpperCase()+field.slice(1);
      for(const suffix of ['','Range']) {
        const input=element(id+suffix);if(!input)continue;
        input.onpointerdown=()=>beginPreviewGesture(field);input.onfocus=()=>beginPreviewGesture(field);
        input.onchange=input.onblur=finishPreviewGesture;
        input.oninput=()=>{
          try {
            const snap=Number(element('Snap').value);
            if(suffix&&snap&&(kind==='Object'&&field!=='width'||kind==='Tile'&&['front','back'].includes(field)))
              input.value=number(Math.round(Number(input.value)/snap)*snap);
            edit(field,input.value);
          }catch(error){status(error.message,true);}
        };
      }
    }
    for(const [id,field] of [['Backing','backing'],['ObjectMode','mode'],['Shape','shape']])element(id).onchange=safely(()=>edit(field,element(id).value));
    for(const [id,field] of [['FrontEnd','front-end'],['BackEnd','back-end']])element(id).onchange=safely(()=>edit(field,element(id).value));
    for(const face of faceNames)element('Face'+face[0].toUpperCase()+face.slice(1)).onchange=safely(()=>{
      const faces=faceNames.filter(name=>element('Face'+name[0].toUpperCase()+name.slice(1)).checked);
      edit('faces',faces.length===6?undefined:faces.length?faces.join('+'):'none');
    });
    for(const face of ['front','back','top','side','left','right','bottom']) {
      const cap=face[0].toUpperCase()+face.slice(1);
      element(cap+'Art').onclick=safely(()=>{
        const targets=clone(selection),targetRoom=roomKey(room),targetProfile=profile(room),field=face+'-art';
        const preview=value=>{
          if(roomKey(room)!==targetRoom||profile(room)!==targetProfile)throw Error('The artwork target room has changed. Choose a face again.');
          artworkPreview=true;try{edit(field,value,targets);}finally{artworkPreview=false;}
        };
        // Opening another picker cancels its old draft before this checkpoint.
        closeTilePalette(false);beginPreviewSession(`${face} artwork`);
        try {openVoxelFacePalette(preview,{preview,cancel:cancelPreviewSession,accept:acceptPreviewSession,
          currentArt:targets[0]?effective(targets[0])[field]:'auto',label:`${cap} artwork · ${targets.length} selected tiles`});}
        catch(error){cancelPreviewSession();throw error;}
      });
      element(cap+'Auto').onclick=safely(()=>edit(face+'-art','auto'));
    }
    refresh();
  }
  return {init,parse,parseBend,parseTransform,importRecord,lines,previewSection,pending,guard,clearLoaded,apply,discard,restore,select,edit,
    resetSelected,refresh,afterRender,pointerDown,pointerMove,keyDown,endGesture,deactivate,mode:()=>Number(element('Mode')?.value||2),
    fromMapSelection,selectCells,selectWhole,selectPicked,refineSelection,coordinateSelection,convert,sampleSettings,applySample,resetProperty,navigate,drawMapOverlay,setBend,choosePullAnchor,
    setTransform,chooseTransformPivot,resizeTransform,
    beginPreviewGesture,finishPreviewGesture,beginPreviewSession,acceptPreviewSession,cancelPreviewSession,
    undoPreview,redoPreview,clearPreviewHistory,canUndoPreview:()=>!!previewUndo.length||!!previewGesture&&previewChanged(previewGesture.before,previewSnapshot()),
    canRedoPreview:()=>!!previewRedo.length,editing:()=>active,status:()=>message};

})();
