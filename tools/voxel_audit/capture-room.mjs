/* Current-source room surface witnesses. Capturing an image is not visual
 * acceptance: dispositions and coverage are recorded separately by the audit. */
import {session} from './editor-session.mjs';
import fs from 'node:fs/promises';import path from 'node:path';import {gzipSync} from 'node:zlib';import crypto from 'node:crypto';
const plan=JSON.parse(await fs.readFile(process.env.AUDIT_PLAN,'utf8'));
const selected=plan.rooms.find(r=>r.id===process.argv[4]);if(!selected)throw Error('Unknown room '+process.argv[4]);
const profiles=(process.env.AUDIT_PROFILES||'0').split(',').map(Number);
await session('surface-room',async({b,out,report,check,input,scene,project})=>{
 await scene();await b.evaluate('window.__voxelAuditApi.RoomPreview_GroundCoverAuditEnable(1)');const original=await project(),originalCameraMode=await b.evaluate('window.__ACTION_VIEW__.cameraMode'),originalEffects=await b.evaluate("document.querySelector('#sharedEffects').checked");report.project_sha256=crypto.createHash('sha256').update(original).digest('hex');report.project_files=Object.fromEntries(Object.entries(JSON.parse(original)).map(([name,source])=>[name,{sha256:crypto.createHash('sha256').update(source).digest('hex'),bytes:Buffer.byteLength(source)}]));report.room=selected.id;report.profiles=profiles;report.plan_sha256=crypto.createHash('sha256').update(JSON.stringify(selected)).digest('hex');report.views=[];report.contacts=[];report.native_frames=[...new Set(selected.windows.filter(w=>!w.profiles||w.profiles.some(p=>profiles.includes(p))).map(w=>w.frame||0))];report.native_frame=report.native_frames.length===1?report.native_frames[0]:null;
 report.observation_scope='Complete copy of existing picker face callbacks; specialized paths without callbacks require their explicit source inventory and visual witnesses.';
 report.scope='Current shared production compositor; static software scene. Source crops are BG1, not a full native PPU frame. Capture is not visual acceptance.';
 const save=async(file,data)=>{await fs.writeFile(path.join(out,file),Buffer.from(data.split(',')[1],'base64'));report.screenshots.push(file);return file;};
 await input('#room',await b.evaluate(`DATA.rooms.findIndex(r=>r.group===${selected.group}&&r.map===${selected.map}&&${selected.section?`r.section===${JSON.stringify(selected.section)}`:'!r.section'})`),'change');
 for(const profile of profiles){
  await input('#terrain',profile,'change');await b.evaluate("VoxelEditor.select(null);nativeFrame=0;document.querySelector('#previewReceivers').checked=false;draw();window.__surfaceContact=[]");await b.settle();let part=0;
  const windows=selected.windows.filter(w=>(!w.profiles||w.profiles.includes(profile))&&(!process.env.AUDIT_WINDOW_IDS||process.env.AUDIT_WINDOW_IDS.split(',').map(Number).includes(w.id)));
  for(const window of windows){
   const bounds=window.bounds,id=`t${profile}-w${String(window.id).padStart(3,'0')}`,native={},images=[];
   await b.evaluate(`window.__ACTION_VIEW__.cameraMode=${JSON.stringify(window.freeCamera?'free':originalCameraMode)};document.querySelector('#sharedEffects').checked=${window.effects??'originalEffects'};SharedRoomPreview.setInspection(${window.inspection||0})`.replace('originalEffects',JSON.stringify(originalEffects)));
   await b.evaluate(`nativeFrame=${window.frame||0};draw()`);await b.settle();
   for(const kind of ['native','effective']){
    const png=await b.evaluate(`(()=>{const s=gameLayerSurfaces(room,0,${kind==='native'}),c=document.createElement('canvas');c.width=${bounds.width};c.height=${bounds.height};const g=c.getContext('2d');g.fillStyle='#29323d';g.fillRect(0,0,c.width,c.height);if(s){g.drawImage(s.low,s.L.originX-(${bounds.x}),s.L.originY-(${bounds.y}));g.drawImage(s.high,s.L.originX-(${bounds.x}),s.L.originY-(${bounds.y}));}return c.toDataURL()})()`);
    native[kind]=await save(id+'-'+kind+'.png',png);images.push(png);
   }
   if(window.bg2){for(const source of ['native','effective']){const png=await b.evaluate(`(()=>{const s=gameLayerSurfaces(room,1,${source==='native'}),c=document.createElement('canvas');c.width=s.low.width;c.height=s.low.height;const g=c.getContext('2d');g.fillStyle='#29323d';g.fillRect(0,0,c.width,c.height);g.drawImage(s.low,0,0);g.drawImage(s.high,0,0);return c.toDataURL()})()`);native['bg2_'+source]=await save(id+'-bg2-'+source+'.png',png);}}
   const views=[];
   for(const angle of window.angles||['left','right']){
    const pitch=angle==='front'?0:(window.pitch??.25);
    await b.evaluate(`document.querySelector('#sharedDistance').value='${window.distance||1}';SharedRoomPreview.focusVoxelSelection(${JSON.stringify(bounds)});SharedRoomPreview.setCameraPreset('${angle}');document.querySelector('#sharedPitch').value='${angle==='front'?0:pitch}';draw()`);await b.settle();
    const png=await b.evaluate("document.querySelector('#sharedGl').toDataURL()");images.push(png);const file=await save(id+'-'+angle+'.png',png);
    const metadata=await b.evaluate(`(()=>{const a=window.__voxelAuditApi;if(!a)throw Error('Audit WASM exports unavailable');const complete=!!a.RoomPreview_VoxelAuditComplete(),count=a.RoomPreview_VoxelAuditCount(),faces=[];let fields;for(let start=0;complete&&start<count;start+=512){const n=a.RoomPreview_VoxelAuditRead(start,512);if(!n)throw Error('Incomplete observation read');const chunk=JSON.parse(new TextDecoder().decode(new Uint8Array(a.memory.buffer,a.DioramaPreview_Input(),n)));if(chunk.first!==start||chunk.total!==count||!chunk.complete)throw Error('Observation changed during copy');fields=chunk.fields;faces.push(...chunk.faces);}const groundCover={complete:!!a.RoomPreview_GroundCoverAuditComplete(),events:[]};for(let i=0;i<a.RoomPreview_GroundCoverAuditCount();++i){groundCover.events.push(Array.from({length:46},(_,f)=>a.RoomPreview_GroundCoverAuditValue(i,f)));}return {complete,count,fields,faces,groundCover,camera:{...nativeCamera},cameraSettings:{yaw:Number(document.querySelector('#sharedYaw').value),pitch:Number(document.querySelector('#sharedPitch').value),distance:Number(document.querySelector('#sharedDistance').value),mode:window.__ACTION_VIEW__.cameraMode},measure:SharedRoomPreview.measure(0),status:document.querySelector('#sharedRoomStatus').textContent,error:document.querySelector('#sharedRoomStatus').classList.contains('error')}})()`);
    if(metadata.error){report.failed_view={profile,id:window.id,angle,bounds,...metadata};delete report.failed_view.faces;delete report.failed_view.fields;}
    check(id+' '+angle+' rendered view',!metadata.error&&(!metadata.complete||metadata.faces.length===metadata.count));
    if(!metadata.complete){report.observation_gaps??=[];report.observation_gaps.push({profile,window:window.id,angle,count:metadata.count,reason:'Existing editor observer is incomplete; no face-registry acceptance for this view.'});metadata.fields??=[];}
    check(id+' '+angle+' complete cover observation',metadata.groundCover.complete);
    const observed=id+'-'+angle+'-observed.json.gz';await fs.writeFile(path.join(out,observed),gzipSync(JSON.stringify({fields:metadata.fields,faces:metadata.faces}),{level:6}));delete metadata.faces;delete metadata.fields;
    const rect=metadata.measure?.rect;if(rect)metadata.requestedCoverageFraction=Math.max(0,Math.min(bounds.x+bounds.width,rect.x+rect.w)-Math.max(bounds.x,rect.x))*Math.max(0,Math.min(bounds.y+bounds.height,rect.y+rect.h)-Math.max(bounds.y,rect.y))/(bounds.width*bounds.height);
    views.push({angle,pitch,image:file,observed,...metadata});
   }
   report.views.push({profile,id:window.id,freeCamera:!!window.freeCamera,distance:window.distance||1,effects:window.effects??originalEffects,inspection:window.inspection||0,label:window.label,frame:window.frame||0,invariant:window.invariant,bounds,native,views});
   await b.evaluate(`window.__surfaceContact.push(${JSON.stringify({label:`${id} world ${bounds.x},${bounds.y} + ${bounds.width}×${bounds.height}`,images})})`);
   async function contact(){const data=await b.evaluate(`(async()=>{const rows=window.__surfaceContact,c=document.createElement('canvas');c.width=1920;c.height=42+rows.length*340;const g=c.getContext('2d');g.fillStyle='#172027';g.fillRect(0,0,c.width,c.height);g.fillStyle='#f1f5f9';g.font='bold 20px sans-serif';g.fillText(${JSON.stringify(selected.id)}+' / terrain '+${profile}+' / native BG1 | effective BG1 | left | right',12,28);for(let y=0;y<rows.length;y++){const row=rows[y],at=42+y*340;g.fillStyle='#f1f5f9';g.font='16px sans-serif';g.fillText(row.label,10,at+21);for(let x=0;x<row.images.length;x++){const im=new Image();im.src=row.images[x];await im.decode();const scale=Math.min(480/im.width,310/im.height);g.imageSmoothingEnabled=false;g.drawImage(im,x*480+(480-im.width*scale)/2,at+28+(310-im.height*scale)/2,im.width*scale,im.height*scale)}}window.__surfaceContact=[];return c.toDataURL()})()`);const filename=`contact-t${profile}-${String(part++).padStart(2,'0')}.png`;await save(filename,data);report.contacts.push(filename);console.log('CONTACT '+filename);}
   if(report.views.filter(v=>v.profile===profile).length%3===0)await contact();
   if(window===windows.at(-1)&&await b.evaluate('window.__surfaceContact.length'))await contact();
   await fs.writeFile(path.join(out,'progress.json'),JSON.stringify(report,null,2));console.log('WINDOW '+selected.id+' '+id);
  }
 }
 check('Project unchanged',await project()===original);
 check('Every requested window captured',report.views.length===profiles.reduce((n,p)=>n+selected.windows.filter(w=>(!w.profiles||w.profiles.includes(p))&&(!process.env.AUDIT_WINDOW_IDS||process.env.AUDIT_WINDOW_IDS.split(',').map(Number).includes(w.id))).length,0));
 report.lowCoverage=report.views.flatMap(w=>w.views.filter(v=>v.requestedCoverageFraction<.995).map(v=>({profile:w.profile,window:w.id,angle:v.angle,fraction:v.requestedCoverageFraction})));
 report.visual_review='pending';
});
