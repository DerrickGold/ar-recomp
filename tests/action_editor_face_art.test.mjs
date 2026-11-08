/* Native face artwork remains a visual draft: crops, flips, picking and four
 * piece composition must never leak into the map stamp clipboard. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source=name=>fs.readFileSync(new URL(`../tools/action_editor/${name}`,import.meta.url),'utf8');
const elements=new Map(),windowListeners={};
let documentHost;
class Element {
  constructor(id='') {
    this.id=id;this.value='';this.children=[];this.listeners={};this.style={};this.attributes={};
    this.hidden=false;this.disabled=false;this.checked=false;this.width=this.height=16;
    const classes=new Set();
    this.classList={toggle(n,on=!classes.has(n)){if(on)classes.add(n);else classes.delete(n);},
      add:n=>classes.add(n),remove:n=>classes.delete(n),contains:n=>classes.has(n)};
  }
  addEventListener(type,callback,capture=false){(this.listeners[type]??=[]).push({callback,capture});}
  setAttribute(name,value){this.attributes[name]=String(value);}
  replaceChildren(...nodes){this.children=nodes;}
  appendChild(node){this.children.push(node);if(this.children.length===1)this.value=String(node.value);}
  contains(node){return node===this||this.children.some(child=>child.contains(node));}
  focus(){documentHost.activeElement=this;}
  getContext(){return this.context??={putImageData(image){this.image=image;},clearRect(){this.image=null;}};}
  getBoundingClientRect(){return {left:0,top:0,width:128,height:128};}
}
const $=selector=>{
  if(!elements.has(selector))elements.set(selector,new Element(selector.slice(1)));
  return elements.get(selector);
};
documentHost={activeElement:$('#origin'),createElement:()=>new Element()};
const layer={chars:new Uint8Array(8*32),words:new Uint16Array([0x0401,0x4802,0x8c03,0xd004]),
  cellId:new Uint8Array([0]),metatileWords:new Uint16Array([1,2,3,4,0x0401,0x4802,0x8c03,0xd004]),pal:[]};
for(let t=0;t<8;t++)for(let y=0;y<8;y++)for(let x=0;x<8;x++) {
  const value=(t*3+x+y*2)%16;
  for(let b=0;b<4;b++)if(value&(1<<b))layer.chars[t*32+(b>>1)*16+y*2+(b&1)]|=1<<(7-x);
}
for(let i=0;i<128;i++)layer.pal.push((0xff000000|i*109|(i*47&255)<<8|(i*23&255)<<16)>>>0);
const room={group:2,map:1,terrainProfile:0,bg:[{},{}],chars:0,extraChars:-1,palette:1};
const context=vm.createContext({document:documentHost,window:{innerWidth:960,innerHeight:640,
  addEventListener(type,callback,capture=false){(windowListeners[type]??=[]).push({callback,capture});}},
  ImageData:class {constructor(w,h){this.width=w;this.height=h;this.data=new Uint8ClampedArray(w*h*4);}},
  $,room,L:layer,DATA:{rooms:[room,{...room,map:2}]},bgIndex:0,mode:'shared',cvs:$('#map'),
  ZERO_PIXEL_MASK:'0'.repeat(64),tileClipboard:{sentinel:true},stampCalls:0,status:'',picked:null,
  selectedCells:new Set([99]),selectedStampKeys:new Set(['-1,0']),refreshStampControls(){},
  pixelInspector:$('#pixelInspector'),authenticBand:w=>w&0x2000?2:1,
  terrainRoom:r=>r,sceneIdentity:r=>`${r.group}:${r.map}`,animationPhase:()=>0,
  decodeLayer:()=>layer,keyOf:()=> 'room:0',pixelIsBlack:()=>false,
  displayedCell:(_r,_bg,source,x,y)=>x===7&&y===9?{id:1,words:Array.from(source.words),bands:[0,1,2,1],black:'0'.repeat(64),transparent:'0'.repeat(64)}:null,
  macControlClick:e=>!!e.ctrlKey,toCell:()=>[7,9],
  sharedPreviewCanvasPoint:(_canvas,e)=>({x:e.clientX,y:e.clientY}),
  SharedRoomPreview:{voxelPick:()=>context.picked},returnToEditedTiles(){},
});
vm.runInContext(`function startStamp(){stampCalls++;}
  function tileActionStatus(text){status=text;}
  function setMode(next){if(next!=='2d')closeTilePalette(false);mode=next;}`,context);
for(const [file,start,end] of [['native_frame.js','function nativeCharacterPixel','function nativeLayerPixel'],
  ['stamp_editor.js','function stampOriginal','function selectRectangle'],
  ['stamp_editor.js','function sampleStampAt','function stampTiles']]) {
  const text=source(file);vm.runInContext(text.slice(text.indexOf(start),text.indexOf(end)),context);
}
vm.runInContext(source('tile_palette.js'),context);
const run=code=>vm.runInContext(code,context),json=code=>JSON.parse(run(`JSON.stringify(${code})`));
const event=(extra={})=>({button:0,clientX:64,clientY:64,preventDefault(){this.prevented=true;},
  stopImmediatePropagation(){this.stopped=true;},...extra});
const dispatch=(node,type,ev)=>{for(const l of node.listeners[type]||[])l.callback(ev);};
const facePixels=()=>Array.from($('#paletteFacePreview').getContext().image.data);

run('let chosen=null;openVoxelFacePalette(value=>{chosen=value;},{label:"Top face"});choosePaletteTile(1)');
assert.equal(context.stampCalls,0);assert.equal(run('chosen'),null,'a palette click stages art');
assert.deepEqual(context.tileClipboard,{sentinel:true});assert.equal($('#paletteFaceTarget').textContent,'Top face');
assert.equal($('#paletteStamp').hidden,true);assert.equal($('#paletteSource').children.length,1,'face donors remain in the current room');
run('setVoxelFaceCrop(4,4,8)');
assert.equal($('#paletteFaceCropInfo').textContent,'8 × 8 crop at 4, 4 · BG1');
assert.equal($('#paletteFaceCrop').style.left,'25%');
const cropped=facePixels();
for(let y=0;y<8;y++)for(let x=0;x<8;x++) {
  const color=run(`stampColor(L,{words:[0x0401,0x4802,0x8c03,0xd004]},${x+4},${y+4})`);
  const checker=((x>>1)^(y>>1))&1?43:29,i=(y*8+x)*4;
  assert.deepEqual(cropped.slice(i,i+4),color===null?[checker,checker,checker,255]:[color&255,(color>>>8)&255,(color>>>16)&255,255]);
}
run('setVoxelFaceCrop(2,3,8)');
const artBefore=json('voxelFacePalette.art'),before=facePixels();
run('flipVoxelFaceArt("h")');
assert.equal(run('voxelFacePalette.art.x'),6,'crop follows mirrored native 16px source');
assert.deepEqual(json('voxelFacePalette.art.words'),[0x0802,0x4401,0x9004,0xcc03]);
// Checkerboard is a preview background, not part of native art; compare ink.
for(let y=0;y<8;y++)for(let x=0;x<8;x++)assert.equal(
  run(`stampColor(L,{words:voxelFacePalette.art.words},${x+6},${y+3})`),
  run(`stampColor(L,{words:${JSON.stringify(artBefore.words)}},${2+7-x},${y+3})`));
run('flipVoxelFaceArt("h");flipVoxelFaceArt("v");flipVoxelFaceArt("v")');
assert.deepEqual(json('voxelFacePalette.art'),artBefore);assert.deepEqual(facePixels(),before);
run('setVoxelFaceCrop(4,4,8);applyVoxelFacePalette()');
assert.equal(run('chosen'),'0,0401,4802,8C03,D004,4,4,8');
assert.equal($('#tilePalette').hidden,true);assert.equal(documentHost.activeElement,$('#origin'));
assert.equal(context.stampCalls,0);assert.deepEqual(context.tileClipboard,{sentinel:true});

run(`openVoxelFacePalette(value=>{chosen=value;});$('#paletteKind').value='character';refreshTilePalette(true);
  $('#paletteRow').value='2';$('#palettePieceFlipH').checked=true;choosePaletteCharacter(1);
  $('#palettePieceFlipH').checked=false;choosePaletteCharacter(2);
  $('#palettePieceFlipV').checked=true;choosePaletteCharacter(3);
  $('#palettePieceFlipH').checked=true;choosePaletteCharacter(4);usePaletteComposite();`);
assert.deepEqual(json('voxelFacePalette.art.words'),[0x4801,0x0802,0x8803,0xc804]);
assert.equal(run('voxelFacePalette.art.size'),16);assert.equal(context.stampCalls,0);
run('applyVoxelFacePalette()');assert.equal(run('chosen'),'0,4801,0802,8803,C804,0,0,16');

run(`openVoxelFacePalette(value=>{chosen=value;},{currentArt:'1,0401,4802,8C03,D004,1,5,8'});`);
assert.equal($('#paletteFaceBg').value,'1');assert.equal(run('voxelFacePalette.art.x'),1);
const key=event({key:'ArrowRight',shiftKey:true});$('#paletteFaceSource').onkeydown(key);
assert.equal(run('voxelFacePalette.art.x'),5);assert.ok(key.prevented&&key.stopped);
$('#paletteFaceSource').onclick(event({currentTarget:$('#paletteFaceSource'),clientX:64,clientY:64}));
assert.equal(run('voxelFacePalette.art.x'),4);assert.equal(run('voxelFacePalette.art.y'),4);
run('beginVoxelFacePick("shared")');
context.picked={kind:'object'};
const rejected=event();dispatch($('#sharedGl'),'pointerdown',rejected);
assert.ok(rejected.prevented&&rejected.stopped);assert.equal(run('voxelFacePalette.picking'),'shared');
context.picked={kind:'tile',x:7,y:9,face:'left',leftArt:'0,0004,0003,0002,0001,3,6,8'};
const picked=event();dispatch($('#sharedGl'),'pointerdown',picked);
assert.ok(picked.prevented&&picked.stopped);assert.equal(run('voxelFacePalette.picking'),null);
assert.equal(run('paletteArtValue(voxelFacePalette.art)'),'0,0004,0003,0002,0001,3,6,8','authored visible face preserves its native source and crop');
assert.ok($('#sharedGl').listeners.pointerdown.every(l=>l.capture),'picker runs before selection/orbit');
context.picked={kind:'tile',x:7,y:9,face:'left',leftArt:'auto',sideArt:'0,0001,0002,0003,0004,4,4,8'};
run('pickupVoxelFaceArt(picked)');
assert.equal(run('paletteArtValue(voxelFacePalette.art)'),'0,0001,0002,0003,0004,4,4,8','unset per-side art inherits legacy side artwork');
context.picked={kind:'tile',x:3,y:4,face:'top',topArt:'auto',sampleSource:{bg:0,x:116,y:148,size:8}};
run('pickupVoxelFaceArt(picked)');
assert.equal(run('paletteArtValue(voxelFacePalette.art)'),'0,0401,4802,8C03,D004,4,4,8','automatic cap donor can differ from the selected cell');
for(const face of ['side','left','right','bottom']) {
  context.picked={kind:'tile',x:3,y:4,face,[face+'Art']:'auto',sideArt:'auto',
    sampleSource:{bg:0,x:116,y:148,size:8}};
  run('pickupVoxelFaceArt(picked)');
  assert.equal(run('paletteArtValue(voxelFacePalette.art)'),'0,0401,4802,8C03,D004,4,4,8',
    `automatic ${face} picks its displayed material patch instead of the selected cell's front`);
}
context.picked={kind:'tile',x:3,y:4,face:'bottom',bottomArt:'1,0004,0003,0002,0001,0,0,16',
  sideArt:'0,0001,0002,0003,0004,4,4,8',sampleSource:{bg:0,x:116,y:148,size:8}};
run('pickupVoxelFaceArt(picked)');
assert.equal(run('paletteArtValue(voxelFacePalette.art)'),'1,0004,0003,0002,0001,0,0,16',
  'explicit directional artwork wins over shared artwork and automatic donor provenance');
context.picked.bottomArt='auto';run('pickupVoxelFaceArt(picked)');
assert.equal(run('paletteArtValue(voxelFacePalette.art)'),'0,0001,0002,0003,0004,4,4,8',
  'directional Auto still inherits an explicitly painted shared side');
context.picked.sampleSourceAmbiguous=true;
assert.equal(run('paletteArtValue(paletteSampleFaceArt(picked).art)'),'0,0001,0002,0003,0004,4,4,8',
  'explicit shared art remains authoritative over ambiguous automatic geometry');
context.picked.sideArt='auto';
assert.equal(run('paletteSampleFaceArt(picked)'),null,
  'a coordinate selection spanning multiple native materials cannot invent one donor');
context.picked.bottomArt='1,0004,0003,0002,0001,0,0,16';
assert.equal(run('paletteArtValue(paletteSampleFaceArt(picked).art)'),'1,0004,0003,0002,0001,0,0,16',
  'explicit directional artwork remains sampleable on a multi-patch cell');
assert.equal(run('paletteSourceArt({x:116,y:148,size:16,bg:0})'),null,'unrepresentable donor does not silently change the crop');
run(`const baseDisplayedCell=displayedCell;displayedCell=(_r,_bg,_l,x,y)=>({words:[x*16+y,x*16+y+1,x*16+y+2,x*16+y+3]});`);
assert.deepEqual(json('paletteSourceArt({x:14,y:14,size:8,bg:0})'),{bg:0,words:[3,18,2,17],x:6,y:6,size:8},'an exact native donor may cross four metatiles');
run('displayedCell=baseDisplayedCell');

run('beginVoxelFacePick("2d")');assert.equal(context.mode,'2d');
const mapClick=event();dispatch($('#map'),'mousedown',mapClick);
assert.ok(mapClick.prevented&&mapClick.stopped);assert.equal(context.mode,'shared','temporary map eyedropper returns to 3D');
assert.equal(run('paletteArtValue(voxelFacePalette.art)'),'0,0401,4802,8C03,D004,0,0,16','map picks freeze displayed words, not a metatile ID');
run('voxelFacePalette.choose=()=>{throw Error("Target changed");}');
assert.equal(run('applyVoxelFacePalette()'),false);assert.equal($('#tilePalette').hidden,false);
assert.equal($('#paletteFaceStatus').textContent,'Target changed');
const esc=event({key:'Escape'});for(const l of windowListeners.keydown.filter(l=>l.capture))l.callback(esc);
assert.ok(esc.prevented&&esc.stopped);assert.equal($('#tilePalette').hidden,true);

run('openTilePalette();choosePaletteTile(1)');
assert.equal(context.stampCalls,1);assert.equal($('#paletteFaceControls').hidden,true);assert.equal($('#paletteStamp').hidden,false);
assert.equal($('#paletteSource').children.length,2,'ordinary stamping retains compatible-room donors');
run(`$('#paletteRow').value='3';paletteQuarter=0;choosePaletteCharacter(5);usePaletteComposite()`);
assert.equal(context.stampCalls,2);assert.equal(context.tileClipboard.cells[0].words[0],0x0c05,'face-only piece flips cannot leak into normal stamps');
run('openVoxelFacePalette(()=>{throw Error("stale callback");});choosePaletteTile(1);room.terrainProfile=1');
assert.equal(run('applyVoxelFacePalette()'),false,'changing terrain cancels stale destination');
assert.equal($('#tilePalette').hidden,true);
assert.equal(run('paletteReadArt("0,GGGG,0000,0000,0000,0,0,16")'),null);
assert.equal(run('paletteReadArt("0,0000,0000,0000,0000,9,0,8")'),null);
// A live destination transaction receives each artwork choice immediately;
// sampling may switch views but must not cancel or redirect that transaction.
run(`room.terrainProfile=0;mode='shared';let liveArt='0,0001,0002,0003,0004,0,0,16',previews=0,cancels=0,accepts=0;
  const baselineArt=liveArt,clipboardBeforeLive=JSON.stringify(tileClipboard);
  const liveOptions={currentArt:baselineArt,preview:value=>{liveArt=value;previews++;},
    cancel:()=>{liveArt=baselineArt;cancels++;},accept:()=>{accepts++;}};
  openVoxelFacePalette(()=>{throw Error('legacy apply must not run for live transactions');},liveOptions);
  choosePaletteTile(1);`);
assert.equal(run('previews'),1);assert.equal(run('liveArt'),'0,0401,4802,8C03,D004,0,0,16');
assert.equal($('#paletteFaceApply').textContent,'Done');assert.equal($('#tilePaletteClose').textContent,'Cancel');
run('setVoxelFaceCrop(4,4,8);flipVoxelFaceArt("h")');
assert.equal(run('previews'),3,'crop and flip preview without another Apply button');
run('beginVoxelFacePick("2d")');
assert.equal($('#tilePalette').hidden,true);assert.equal($('#palettePicking').hidden,false);
assert.equal(run('cancels'),0,'temporary map view does not cancel live artwork');
const livePick=event();dispatch($('#map'),'mousedown',livePick);
assert.ok(livePick.stopped);assert.equal(context.mode,'shared');
assert.equal($('#tilePalette').hidden,false);assert.equal($('#palettePicking').hidden,true);
assert.equal(run('liveArt'),'0,0401,4802,8C03,D004,0,0,16');
assert.equal(run('JSON.stringify(tileClipboard)'),run('clipboardBeforeLive'));
$('#tilePaletteClose').onclick();
assert.equal(run('liveArt'),run('baselineArt'));assert.equal(run('cancels'),1);
run('previews=0;openVoxelFacePalette(()=>{},liveOptions);applyVoxelFacePalette()');
assert.equal(run('previews'),0,'Done without a choice preserves the existing preview/history');
assert.equal(run('accepts'),1);
run(`openVoxelFacePalette(()=>{},liveOptions);$('#paletteKind').value='character';
  $('#paletteFacePieceMode').value='assemble';refreshTilePalette(true);$('#paletteRow').value='0';
  choosePaletteCharacter(1);choosePaletteCharacter(2);choosePaletteCharacter(3);choosePaletteCharacter(4);`);
assert.equal($('#paletteUseComposite').hidden,true,'live composition requires no separate Preview button');
assert.equal(run('liveArt'),'0,0001,0002,0003,0004,0,0,16');
assert.equal(run('finishVoxelFacePaletteForHistory()'),true);
assert.equal($('#tilePalette').hidden,true);assert.equal(run('accepts'),2,'history finishes the current live transaction before undo');
run('openVoxelFacePalette(()=>{},liveOptions);beginVoxelFacePick("shared")');
const pickEscape=event({key:'Escape'});for(const l of windowListeners.keydown.filter(l=>l.capture))l.callback(pickEscape);
assert.ok(pickEscape.stopped);assert.equal($('#tilePalette').hidden,false);assert.equal(run('cancels'),1);
const paletteEscape=event({key:'Escape'});for(const l of windowListeners.keydown.filter(l=>l.capture))l.callback(paletteEscape);
assert.equal(run('cancels'),2,'a second Escape cancels the picker transaction');
run('openVoxelFacePalette(()=>{},liveOptions);choosePaletteTile(1)');
const outsideProperty=event({target:$('#unrelatedProperty')});
for(const l of windowListeners.pointerdown.filter(l=>l.capture))l.callback(outsideProperty);
assert.equal(run('accepts'),3);assert.equal(run('cancels'),2);
assert.equal($('#tilePalette').hidden,true,'an outside property gesture first accepts the art preview');
assert.equal(outsideProperty.prevented,undefined,'the unrelated property action continues normally');

// Normal painting uses a separate donor-sampling path. It freezes the source
// pixels into the brush while leaving the destination selection untouched.
run('openTilePalette();beginPaletteStampPick()');
assert.equal($('#tilePalette').hidden,true);assert.equal($('#palettePicking').hidden,false);
const brushPick=event();dispatch($('#map'),'mousedown',brushPick);
assert.ok(brushPick.prevented&&brushPick.stopped);
assert.equal($('#tilePalette').hidden,false);assert.equal($('#palettePicking').hidden,true);
assert.deepEqual([...context.selectedCells],[99]);assert.deepEqual([...context.selectedStampKeys],['-1,0']);
assert.equal(context.tileClipboard.label,'Sampled tile 7, 9');
assert.deepEqual(Array.from(context.tileClipboard.cells[0].words),Array.from(layer.words));
run(`const sampledDonor={id:2,words:[1,2,3,4],bands:[0,1,2,1],black:ZERO_PIXEL_MASK,transparent:ZERO_PIXEL_MASK};
  displayedCell=()=>sampledDonor;sampleStampAt(7,9);tileClipboard.cells[0].words[0]=9;tileClipboard.cells[0].bands[0]=2;`);
assert.deepEqual(json('sampledDonor.words'),[1,2,3,4]);assert.deepEqual(json('sampledDonor.bands'),[0,1,2,1]);

// The settings brush shares the actual face donor resolver. Copy Art must
// freeze the clicked native face, not copy an "auto" instruction that would
// resolve to different artwork at the destination.
const settingsSource=source('voxel_editor.js');
run(`const settingsSampler=(()=>{
  const artFields=['front','back','top','side','left','right','bottom'].map(face=>face+'-art');
  const cellFields=['front','back','width','offset','backing',...artFields,'faces','shape','shape-span','front-end','back-end','bend'];
  const faceNames=['front','back','top','left','right','bottom'];
  let selection=[],sample=null,tool='sample';
  const metadata=item=>item,recordFor=item=>item.record||null;
  const element=id=>$('#voxel'+id),status=()=>{},refresh=()=>{},selectedBounds=()=>null;
  const mutate=(items,change)=>{for(const item of items)item.record=change(item,{...item.record});};
  ${settingsSource.slice(settingsSource.indexOf('  function effective('),settingsSource.indexOf('  function convert('))}
  return {sampleSettings,applySample,values:()=>sample.values};
})();
for(const name of ['CopyDepth','CopyPlacement','CopyBacking','CopyFaces','CopyShape'])$('#voxel'+name).checked=false;
$('#voxelCopyArt').checked=true;
displayedCell=(_r,_bg,_layer,x,y)=>x===7&&y===9?sampledDonor:null;
const artSource={kind:'tile',x:7,y:9,front:-.1,back:.3,faces:0,backing:'none'};
const artTarget={kind:'tile',x:4,y:2,record:{front:-.24,back:.4,width:12,offset:3,backing:'none',faces:'none',shape:'box'}};
const targetNonArt=JSON.stringify(artTarget.record);
settingsSampler.sampleSettings(artSource);sampledDonor.words[0]=8;settingsSampler.applySample([artTarget]);`);
assert.equal(run('artTarget.record["front-art"]'),'0,0001,0002,0003,0004,0,0,16','native source pixels freeze when sampled');
assert.equal(run('JSON.stringify(Object.fromEntries(Object.entries(artTarget.record).filter(([key])=>!key.endsWith("-art"))))'),run('targetNonArt'),'Copy Art does not change depth, placement, backing or visibility');
assert.equal(run('settingsSampler.values().faces'),'none','explicit face visibility remains sampled unchanged');
run(`sampledDonor.words[0]=1;settingsSampler.sampleSettings({...artSource,face:'top',sampleSource:{x:116,y:148,size:8,bg:0}});`);
assert.equal(run('settingsSampler.values()["top-art"]'),'0,0001,0002,0003,0004,4,4,8','automatic cap uses the exact displayed donor crop');
assert.equal(run('settingsSampler.values()["front-art"]'),'auto','only the clicked native face is frozen');
for(const face of ['side','left','right','bottom']) {
  run(`settingsSampler.sampleSettings({...artSource,face:${JSON.stringify(face)},sampleSource:{x:116,y:148,size:8,bg:0}});`);
  assert.equal(run(`settingsSampler.values()[${JSON.stringify(face+'-art')}]`),'0,0001,0002,0003,0004,4,4,8',
    `Copy Art freezes the automatic ${face} material patch`);
  assert.equal(run('settingsSampler.values()["front-art"]'),'auto','sampling a skin never replaces front artwork');
}
run(`settingsSampler.sampleSettings({...artSource,face:'bottom',sampleSource:{x:116,y:148,size:8,bg:0}});
  settingsSampler.applySample([artTarget]);`);
assert.equal(run('artTarget.record["bottom-art"]'),'0,0001,0002,0003,0004,4,4,8',
  'a native underside donor is frozen into BottomArt');
assert.equal(run('artTarget.record["side-art"]'),'auto','underside sampling does not freeze its donor into shared SideArt');
assert.equal(run('artTarget.record["front-art"]'),'auto','underside sampling does not freeze its donor onto the front');
assert.equal(run('JSON.stringify(Object.fromEntries(Object.entries(artTarget.record).filter(([key])=>!key.endsWith("-art"))))'),
  run('targetNonArt'),'underside Copy Art retains geometry, placement, backing and visibility');
run(`const oldDonorWord=sampledDonor.words[0];sampledDonor.words[0]=0xC123;
  settingsSampler.sampleSettings({...artSource,face:'right',sampleSource:{x:112,y:144,size:16,bg:0}});
  sampledDonor.words[0]=oldDonorWord;`);
assert.equal(run('settingsSampler.values()["right-art"]'),'0,C123,0002,0003,0004,0,0,16',
  'a donor freezes the currently displayed stamped words, including palette/flip bits, before a later map change');
run(`settingsSampler.sampleSettings({...artSource,face:'top',topArt:'auto',record:{'top-art':'1,0401,4802,8C03,D004,2,3,8'},sampleSource:{x:116,y:148,size:8,bg:0}});`);
assert.equal(run('settingsSampler.values()["top-art"]'),'1,0401,4802,8C03,D004,2,3,8','current custom artwork wins over automatic cap metadata');
run(`settingsSampler.sampleSettings({...artSource,face:'left',leftArt:'auto',sideArt:'0,0004,0003,0002,0001,0,0,16'});`);
assert.equal(run('settingsSampler.values()["left-art"]'),'0,0004,0003,0002,0001,0,0,16','per-side Auto inherits the displayed shared side donor');
run(`settingsSampler.sampleSettings({...artSource,face:'top',topArt:'none'});`);
assert.equal(run('settingsSampler.values()["top-art"]'),'none','an explicit empty marker never falls back to native art');
assert.equal(run('paletteSampleFaceArt({...artSource,face:"left",leftArt:"auto",sideArt:"none"})'),null);
console.log('Action editor face artwork: crops, native flips, composition, eyedroppers and stamp isolation passed.');
