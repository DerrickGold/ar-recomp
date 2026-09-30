/* Exercise the offline editor's actual scripts without a browser or ROM.
 * An optional built HTML also checks every exported C golden frame. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sources = path.join(root, 'tools/action_editor');
const manifest = fs.readFileSync(path.join(sources, 'editor.body.html'), 'utf8');
const scripts = [...manifest.matchAll(/<script src="([a-z_]+\.js)"><\/script>/g)]
  .map(match => match[1]);

/* Only host services used during startup are stubbed. Drawing is queued;
 * decoding, classification, INI merging, gestures and native parity run real code. */
function editor(data) {
  class Element {
    constructor() {
      this.value = '';
      this.style = {};
      this.children = [];
      this.listeners = {};
      const classes=new Set();
      this.classList = {toggle(name,on=!classes.has(name)) {
        if(on)classes.add(name);else classes.delete(name);
      }, add(name) {classes.add(name);}, remove(name) {classes.delete(name);},
      contains(name) {return classes.has(name);}};
      this.attributes = {};
    }
    addEventListener(name,callback) { this.listeners[name]=callback; }
    appendChild(child) {
      this.children.push(child);
      if (this.children.length === 1) this.value = String(child.value);
    }
    getContext() { return this.context ??= {putImageData(image) {this.image=image;},
      drawImage() {},clearRect() {},setTransform() {},fillRect() {},
      setLineDash(value) {this.dash=value;},measureText(text) {return {width:text.length*7};},
      fillText(text,x,y) {(this.labels??=[]).push({text,x,y});},
      strokeRect(x,y,w,h) {(this.strokes??=[]).push({x,y,w,h,color:this.strokeStyle});},
      save() {},restore() {}}; }
    getBoundingClientRect() { return this.selector==='#pixelCanvas'
      ? {left:0,top:0,width:224,height:224} : this.selector==='#tileMenu'
        ? {left:0,top:0,width:260,height:420} : {left:0,top:0,width:960,height:640}; }
    setPointerCapture() {}
    setAttribute(name,value) {this.attributes[name]=value;}
    contains(other) {return other===this||this.children.some(child=>child.contains(other));}
    focus() {documentHost.activeElement=this;}
    select() {this.selectionStart=0;this.selectionEnd=this.value.length;}
    showModal() {this.open=true;}
    close() {this.open=false;}
    click() {this.onclick?.({target:this});}
    remove() {}
    scrollIntoView() {}
  }
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) {
      const e=new Element();e.selector=selector;elements.set(selector,e);
      if(/^#tileAction(?!Status)/.test(selector))element('#tileMenu').children.push(e);
    }
    return elements.get(selector);
  };
  const documentHost={querySelector:element, querySelectorAll:() => [],
    createElement:() => new Element(),body:new Element(),
    execCommand(command) {
      if(command!=='copy')return false;
      if(!this.copyFallbackSuccess) {
        this.activeElement.listeners.copy?.({defaultPrevented:false});return false;
      }
      this.clipboardText=this.activeElement.value;
      this.activeElement.listeners.copy?.({defaultPrevented:false});return true;
    }};
  const context = vm.createContext({
    window:{__ACTION_BG__:data, innerWidth:960,innerHeight:640,listeners:{}, addEventListener(name,callback) {
      (this.listeners[name]??=[]).push(callback);
    }},
    document:documentHost,
    navigator:{clipboard:{async writeText(text) {documentHost.clipboardText=text;}}},
    atob:s => Buffer.from(s, 'base64').toString('binary'),
    requestAnimationFrame() {},
    ResizeObserver:class { observe() {} },
    ImageData:class {
      constructor(width, height) {
        this.data = new Uint8ClampedArray(width * height * 4);
      }
    },
    getComputedStyle:() => ({getPropertyValue:property =>
      ({'--behind':'#7c5cff','--plane':'#3d8f6b','--ahead':'#e0913a'})[property]??'#aabbcc'}),
    console,
    Blob:class {constructor(parts) {this.text=parts.join('');}},
    URL:{createObjectURL(blob) {documentHost.exportedText=blob.text;return 'blob:test';},
      revokeObjectURL() {}},
    setTimeout(callback) {callback();},
    devicePixelRatio:1,
  });
  for (const script of scripts)
    vm.runInContext(fs.readFileSync(path.join(sources, script), 'utf8'), context,
      {filename:script});
  return {run:code => vm.runInContext(code, context), elements};
}

function fixture() {
  const blobs = [];
  const blob = bytes => { blobs.push(bytes.toString('base64')); return blobs.length - 1; };
  const chars = blob(Buffer.alloc(0x4000));
  const palette = blob(Buffer.alloc(0x100));
  const definitions = Buffer.alloc(0x800);
  definitions.writeUInt16BE(0x2000, 8);
  const european = Buffer.from(definitions);
  european.writeUInt16BE(0x2000, 0);
  const base = {pagesWide:2, pagesHigh:1, map:blob(Buffer.alloc(512)),
    metatiles:blob(definitions)};
  const japanese = {...base, map:blob(Buffer.alloc(512, 1))};
  const eu = {...base, metatiles:blob(european)};
  const room = {group:1, map:1, bg:[base,base], chars, palette, extraChars:-1,
    video:Array(28).fill(0), videoProfile:0, raster:0, terrainVariants:[
      {profile:0, bg1:base, changedCells:0, changedMetatiles:0},
      {profile:1, bg1:japanese, changedCells:512, changedMetatiles:0},
      {profile:2, bg1:eu, changedCells:0, changedMetatiles:1},
    ]};
  return {blobs, rooms:[room, {...room, map:2}], tileWordMask:0xecff,
    bg1Attributes:0x10, bg2Attributes:1, frameWidth:256, frameHeight:224,
    terrainProfiles:[{profile:0,label:'US'}, {profile:1,label:'Japanese'},
      {profile:2,label:'European'}]};
}

const {run, elements} = editor(fixture());
assert.deepEqual(elements.get('#terrain').children.map(option => option.textContent),
  ['US', 'Japanese', 'European']);
run(String.raw`loadIniText('# keep\n[layers:01:01]\nbg1-virtual = cells:0,0-0,0 band:0\n'
  + 'obj2 = z:0.7\n[layers:01:01:camera:0]\nbg1 = z:0.8\n', 'custom.ini');
  setLayer(0);`);
const original = run('mergeDioramaIni()');
run(`nativeCamera.x=128; nativeFrame=9; setLayer(1);
  $('#terrain').value='1'; $('#terrain').onchange();`);
assert.equal(run('room.map'), 1);
assert.equal(run('bgIndex'), 1);
assert.equal(run('nativeCamera.x'), 128);
assert.equal(run('nativeFrame'), 9);
assert.equal(run('nativeDecodedLayers().layers[0].cellId[0]'), 1);
assert.equal(run('mergeDioramaIni()'), original);
assert.match(elements.get('#terrainInfo').textContent, /512 map cells/);
run("$('#terrain').value='2'; $('#terrain').onchange();");
assert.equal(run('nativeDecodedLayers().layers[0].cellId[0]'), 0);
assert.ok(run('nativeDecodedLayers().layers[0].words[0] & 0x2000'));
assert.equal(run('mergeDioramaIni()'), original);
run(`setLayer(0); brush='cell'; beginOp('terrain edit'); paintCell(2,0,2); commitOp();
  $('#terrain').value='0'; $('#terrain').onchange();`);
const edited = run('mergeDioramaIni()');
assert.match(edited, /bg1-virtual:eu = cells:2,0-2,0 band:2/);
assert.equal((edited.match(/\[layers:01:01\]/g) || []).length, 1);
assert.match(edited, /obj2 = z:0.7/);
assert.match(edited, /\[layers:01:01:camera:0\]\nbg1 = z:0.8/);
run(`$('#room').value='1'; $('#room').onchange();
  $('#terrain').value='1'; $('#terrain').onchange(); undo();`);
assert.equal(run('room.map'), 1);
assert.equal(run('room.terrainProfile'), 2);
assert.equal(run('L.cellId[0]'), 0);
assert.equal(run('mergeDioramaIni()'), original);
run('redo()');
assert.equal(run('mergeDioramaIni()'), edited);
assert.equal(run('bucket(terrainRoom(DATA.rooms[0],0),0) === bucket(room,0)'), false);
assert.equal(run("captureEditorSavepoint('exported'); $('#terrain').value='2'; $('#terrain').onchange(); configDirty"),
  false);
/* Selection must not create edits; Deselect must not erase saved ones. */
run("brush='select'; selectCell(0,0); selectCell(1,0);");
const beforeDeselect=run('mergeDioramaIni()');
run('deselect()');
assert.equal(run('selectedCells.size'),0);
assert.equal(run('pixelCell'),null);
assert.equal(run('mergeDioramaIni()'),beforeDeselect);
run("$('#selPrio').onclick();");
assert.equal(run('mergeDioramaIni()'),beforeDeselect);
run("band=0; $('#applyBand').onclick();");
assert.notEqual(run('mergeDioramaIni()'),beforeDeselect);
run('undo()');
assert.equal(run('mergeDioramaIni()'),beforeDeselect);

/* One black pixel on one cell leaves all other transparent pixels alone.
 * Native CHR/palette reads never consult the presentation mask. */
run("deselect(); selectCell(0,0); $('#pixelScope').value='cell'; beginOp('pixel'); editPixel(3,5); commitOp();");
assert.equal(run('pixelIsBlack(pixelMaskAt(room,0,L,0),3,5)'),true);
assert.equal(run('pixelIsBlack(pixelMaskAt(room,0,L,0),4,5)'),false);
assert.equal(run('pixelIsBlack(pixelMaskAt(room,0,L,1),3,5)'),false);
assert.equal(run('pixelOriginal(L,0,3,5)'),null);
const pixelSaved=run('mergeDioramaIni()');
assert.match(pixelSaved,/bg1-pixels:eu = cell:0,0 black:[0-9A-F]{64}/);
run('undo()');
assert.equal(run('pixelIsBlack(pixelMaskAt(room,0,L,0),3,5)'),false);
run('redo()');
assert.equal(run('mergeDioramaIni()'),pixelSaved);
run('deselect()');
assert.equal(run('mergeDioramaIni()'),pixelSaved);
run(`loadIniText(${JSON.stringify(pixelSaved)},'pixels.ini'); setLayer(0); selectCell(0,0);`);
assert.equal(run('pixelIsBlack(pixelMaskAt(room,0,L,0),3,5)'),true);
run("$('#pixelScope').value='id'; pixelBulk('whole'); $('#pixelScope').value='cell'; beginOp('restore'); editPixel(3,5,false); commitOp();");
assert.equal(run('pixelIsBlack(pixelMaskAt(room,0,L,0),3,5)'),false);
assert.equal(run('pixelIsBlack(pixelMaskAt(room,0,L,1),3,5)'),true);
run("pixelBulk('reset');");
assert.equal(run('pixelIsBlack(pixelMaskAt(room,0,L,0),3,5)'),true);
run(String.raw`loadIniText('[layers:01:01]\nbg1 = transparent:black\n','fill.ini'); setLayer(0);`);
assert.match(run('mergeDioramaIni()'),/transparent:black/);
run("selectCell(0,0); $('#pixelScope').value='cell';");
const pointerCanvas=elements.get('#pixelCanvas');
const event=(x,y)=>({clientX:x*14+2,clientY:y*14+2,button:0,pointerId:1,preventDefault(){}});
const historyBefore=run('undoStack.length');
pointerCanvas.listeners.pointerdown(event(3,5));
pointerCanvas.listeners.pointermove(event(9,5));
pointerCanvas.listeners.pointerup(event(9,5));
assert.equal(run('undoStack.length'),historyBefore+1);
for(let x=3;x<=9;x++)assert.equal(run(`pixelIsBlack(pixelMaskAt(room,0,L,0),${x},5)`),true);
run('undo()');
assert.equal(run('pixelMaskAt(room,0,L,0)'), '0'.repeat(64));

console.log('Deselect, selection-only gestures, partial pixels, undo and INI round trips passed');

console.log('Regional terrain selection, scoped INI, caches and cross-room undo passed');

/* Different BG1 terrain gets isolated masks, bands, stamps, bounds and
 * clipboards. BG2 and unused definition changes share a single authoring set. */
const familiesData=fixture();
const unused=Buffer.from(familiesData.blobs[familiesData.rooms[0].bg[0].metatiles],'base64');
unused.writeUInt16BE(0x2000,16); // metatile 2 never occurs in the map
familiesData.blobs.push(unused.toString('base64'));
familiesData.rooms[0].terrainVariants[2]={profile:2,
  bg1:{...familiesData.rooms[0].bg[0],metatiles:familiesData.blobs.length-1},
  changedCells:0,changedMetatiles:1};
// Room 2 shares its JP/EU variant instead of US/EU.
familiesData.rooms[1]={...familiesData.rooms[1],terrainVariants:[
  ...familiesData.rooms[1].terrainVariants.slice(0,2),
  {...familiesData.rooms[1].terrainVariants[1],profile:2}]};
const families=editor(familiesData);
assert.equal(families.run('editFamilies(room,0).length'),2);
assert.equal(families.run('editFamilies(room,1).length'),1);
assert.equal(families.run('editFamily(room,0).mask'),5);
families.run(`loadIniText('[layers:01:01]\\n'
  +'bg1-pixels = cell:0,0 black:'+ 'FFFF'.repeat(16)+'\\n'
  +'bg1-virtual = metatile:00 band:0\\n'
  +'bg1 = z:0.6\\n', 'us-default.ini');setLayer(0);`);
const familyIni=families.run('mergeDioramaIni()');
assert.match(familyIni,/bg1-pixels:us\+eu = cell:0,0/);
assert.match(familyIni,/bg1-virtual:us\+eu = metatile:00 band:0/);
families.run(`selectRectangle(0,0,0,0);copyTiles();stampTiles(-1,0);
  $('#terrain').value='2';$('#terrain').onchange();`);
assert.equal(families.run('stampBucket(room,0).cells["-1,0"]!==undefined'),true);
assert.equal(families.run('pixelIsBlack(pixelMaskAt(room,0,L,0),3,5)'),true);
assert.equal(families.run('tileClipboard.key===keyOf(room,0)'),true);
families.run(`$('#terrain').value='1';$('#terrain').onchange();`);
assert.equal(families.run('pixelMaskAt(room,0,L,0)'), '0'.repeat(64));
assert.equal(families.run('Object.keys(bucket(room,0).byId).length'),0);
assert.equal(families.run('Object.keys(stampBucket(room,0).cells).length'),0);
assert.equal(families.run('mapBounds(room,0,L).x0'),0);
assert.equal(families.run('stampTiles(-2,0)'),false); // frozen US art cannot leak into JP
families.run(`selectCell(0,0);$('#pixelScope').value='cell';
  beginOp('JP pixel');editPixel(4,5);commitOp();
  $('#terrain').value='0';$('#terrain').onchange();undo();`);
assert.equal(families.run('room.terrainProfile'),1); // undo restores the edited family
assert.equal(families.run('pixelIsBlack(pixelMaskAt(room,0,L,0),4,5)'),false);
families.run('redo()');
const savedFamilies=families.run('mergeDioramaIni()');
assert.match(savedFamilies,/bg1-pixels:jp = cell:0,0/);
assert.equal((savedFamilies.match(/bg1-stamp:us\+eu =/g)||[]).length,1);
families.run(`loadIniText(${JSON.stringify(savedFamilies)},'families.ini');setLayer(0);`);
assert.equal(families.run('mergeDioramaIni()'),savedFamilies);
assert.equal(families.run('resolvedPlane("bg1").z'),0.6);
families.run(`setLayer(1);selectCell(0,0);$('#pixelScope').value='cell';
  beginOp('shared BG2 pixel');editPixel(1,1);commitOp();
  $('#terrain').value='2';$('#terrain').onchange();`);
assert.equal(families.run('pixelIsBlack(pixelMaskAt(room,1,L,0),1,1)'),true);
assert.match(families.run('mergeDioramaIni()'),/bg2-pixels:us\+jp\+eu = cell:0,0/);
families.run(`loadIniText('[layers:01:02]\\n'
  +'bg1-pixels:ge = cell:0,0 black:'+ '8000'.repeat(16)+'\\n', 'german.ini');
  $('#room').value='1';$('#room').onchange();setLayer(0);`);
assert.match(families.run('mergeDioramaIni()'),/bg1-pixels:jp\+eu = cell:0,0/);
families.run(`$('#terrain').value='1';$('#terrain').onchange();`);
assert.equal(families.run('pixelIsBlack(pixelMaskAt(room,0,L,0),0,0)'),true);
families.run(`$('#terrain').value='0';$('#terrain').onchange();`);
assert.equal(families.run('pixelMaskAt(room,0,L,0)'), '0'.repeat(64));
families.run(`loadIniText('','regional-capacity.ini');$('#room').value='0';$('#room').onchange();
  setLayer(0);for(let id=0;id<255;id++)pixelBucket(room,0).byId[id]='F'.repeat(64);
  $('#terrain').value='1';$('#terrain').onchange();pixelBucket(room,0).byId[0]='F'.repeat(64);
  selectRectangle(0,0,1,0);`);
assert.equal(families.run('regionalPixelCount(room,0)'),255); // equal saved records count once
const beforeRegionalCapacity=families.run('mergeDioramaIni()');
const beforeRegionalHistory=families.run('undoStack.length');
assert.equal(families.run('fillSelectedTransparency()'),false);
assert.equal(families.run('mergeDioramaIni()'),beforeRegionalCapacity);
assert.equal(families.run('undoStack.length'),beforeRegionalHistory);
console.log('Regional family sharing, unused definitions, scoped pixels/stamps/bounds and German alias passed');

/* Context menus preserve an existing range, target a new tile without
 * painting, and use ordinary delta operations for every mutation. */
const contextEditor=editor(fixture()),cm=contextEditor.run,cmElements=contextEditor.elements;
const cmCanvas=cmElements.get('#map2d'),cmMenu=cmElements.get('#tileMenu');
const cmPoint=(x,y)=>({clientX:cm(`view.x+(${x}*16+8)*view.scale`),
  clientY:cm(`view.y+(${y}*16+8)*view.scale`),button:2,
  preventDefault(){this.prevented=true;},stopPropagation(){this.stopped=true;}});
const cmOpen=(x,y)=>{
  const ev=cmPoint(x,y);cmCanvas.listeners.mousedown(ev);cmCanvas.listeners.contextmenu(ev);
  assert.equal(ev.prevented,true);assert.equal(cmMenu.hidden,false);return ev;
};
const cmAction=name=>cmElements.get(`#tileAction${name}`).onclick();
cm(`loadIniText('','context.ini');setLayer(0);actor.show=false;
  selectRectangle(1,1,3,2);brush='class';`);
const cmOriginal=cm('mergeDioramaIni()'),cmWords=Buffer.from(cm('L.words.buffer'));
cmOpen(2,1);
assert.equal(cm('selectedCells.size'),6);
assert.equal(cm('JSON.stringify(selectionRect)'),JSON.stringify({x0:1,y0:1,x1:4,y1:3}));
assert.equal(cm('mergeDioramaIni()'),cmOriginal);
assert.equal(cm('undoStack.length'),0);
assert.equal(cm('pixelCell'),34);
assert.equal(cmElements.get('#tileActionPaste').disabled,true);
assert.equal(cmElements.get('#tileActionPriority').attributes['aria-checked'],'false');
cmAction('Priority');
assert.equal(cmMenu.hidden,true);
assert.equal(cm('selectedCells.size'),6);
assert.equal(cm('undoStack.length'),1);
assert.equal(cm('Object.keys(st.byId).length'),0); // class brush does not broaden a menu action
assert.equal(cm('Object.keys(st.byCell).length'),6);
assert.equal(cm('st.byCell[0]'),undefined);
assert.deepEqual(Buffer.from(cm('L.words.buffer')),cmWords); // authentic tile bits are immutable
cmOpen(2,1);assert.equal(cmElements.get('#tileActionPriority').attributes['aria-checked'],'true');
cmAction('Priority');assert.equal(cm('st.byCell[34]'),1);
cm('undo()');assert.equal(cm('st.byCell[34]'),2);
cm('undo()');assert.equal(cm('mergeDioramaIni()'),cmOriginal);
cmOpen(2,1);cmAction('Far');assert.equal(cm('st.byCell[34]'),0);
cmOpen(2,1);cmAction('ResetBand');assert.equal(cm('Object.keys(st.byCell).length'),0);
cm('undo()');assert.equal(cm('st.byCell[34]'),0);
cm('undo()');assert.equal(cm('mergeDioramaIni()'),cmOriginal);
cmOpen(2,1);cmAction('Copy');
assert.equal(cm('tileClipboard.w'),3);assert.equal(cm('tileClipboard.h'),2);
assert.equal(cm('mergeDioramaIni()'),cmOriginal);
cmOpen(5,4);assert.equal(cm('selectedCells.size'),1);assert.equal(cm('pixelCell'),133);
cmAction('Paste');assert.equal(cm('Object.keys(stampBucket(room,0).cells).length'),6);
assert.equal(cm('selectedStampKeys.size'),6);
assert.equal(cm('stampBucket(room,0).cells["5,4"].id'),0);
const cmPasted=cm('mergeDioramaIni()');
cm('undo()');assert.equal(cm('mergeDioramaIni()'),cmOriginal);
cm('redo()');assert.equal(cm('mergeDioramaIni()'),cmPasted);
cmOpen(6,4);assert.equal(cm('selectedStampKeys.size'),6);
cmAction('Priority');
assert.equal(cm('stampBucket(room,0).cells["5,4"].bands.every(b=>b===2)'),true);
cm('undo()');assert.equal(cm('mergeDioramaIni()'),cmPasted);
cmOpen(6,4);cmAction('Fill');
assert.equal(cm('stampBucket(room,0).cells["5,4"].black'),'F'.repeat(64));
assert.equal(cm('pixelMaskAt(room,0,L,0)'), '0'.repeat(64));
cm('undo()');assert.equal(cm('mergeDioramaIni()'),cmPasted);
cmOpen(6,4);cmAction('Remove');
assert.equal(cm('Object.keys(stampBucket(room,0).cells).length'),0);
cm('undo()');assert.equal(cm('mergeDioramaIni()'),cmPasted);
cmOpen(-1,0); // empty edge space is a valid destination without discarding selected tiles
assert.equal(cmElements.get('#tileActionPixels').disabled,true);
assert.equal(cmElements.get('#tileActionPaste').disabled,false);
cmAction('Paste');assert.equal(cm('mapBounds(room,0,L).x0'),-1);
assert.equal(cm('selectedStampKeys.size'),6);
cm('undo()');assert.equal(cm('mergeDioramaIni()'),cmPasted);
cmOpen(0,0);cmAction('Pixels');assert.equal(cm('pixelCell'),0);
assert.equal(cm('document.activeElement.selector'),'#pixelBlack');
assert.equal(cm('mergeDioramaIni()'),cmPasted);
cmOpen(0,0);cmAction('Stamp');assert.equal(cm('brush'),'stamp');
assert.equal(cm('JSON.stringify(stampHover)'), '[0,0]');
cmOpen(0,0);cmAction('Deselect');assert.equal(cm('selectedCells.size'),0);
assert.equal(cm('brush'),'select');assert.equal(cm('mergeDioramaIni()'),cmPasted);
/* A right click never pans or paints, including the Mac control-click gesture. */
cm(`brush='pan';const viewBeforeContext=JSON.stringify(view);`);cmOpen(2,2);
assert.equal(cm('drag'),null);assert.equal(cm('JSON.stringify(view)'),cm('viewBeforeContext'));
cm('closeTileMenu();brush="cell";window.navigator={platform:"MacIntel"};');
const controlClick={...cmPoint(2,2),button:0,ctrlKey:true};
cmCanvas.listeners.mousedown(controlClick);
assert.equal(cm('drag'),null);assert.equal(cm('mergeDioramaIni()'),cmPasted);
/* The menu stays inside the viewport, supports keyboard navigation, and
 * closes without losing selection on Escape, outside clicks or view changes. */
const edgeEvent={...cmPoint(2,2),clientX:958,clientY:638};
cmCanvas.listeners.contextmenu(edgeEvent);
assert.equal(cmMenu.style.left,'692px');assert.equal(cmMenu.style.top,'212px');
const cmKey=key=>({key,preventDefault(){this.prevented=true;},stopPropagation(){this.stopped=true;}});
cmMenu.listeners.keydown(cmKey('End'));
assert.equal(cm('document.activeElement.selector'),'#tileActionDeselect');
cmMenu.listeners.keydown(cmKey('Home'));
assert.equal(cm('document.activeElement.selector'),'#tileActionPriority');
const cmSelectionBeforeEsc=cm('selectedCells.size'),cmEsc=cmKey('Escape');
cmMenu.listeners.keydown(cmEsc);
assert.equal(cmEsc.stopped,true);assert.equal(cmMenu.hidden,true);
assert.equal(cm('selectedCells.size'),cmSelectionBeforeEsc);
const menuKeyboard={...cmKey('F10'),shiftKey:true};cmCanvas.listeners.keydown(menuKeyboard);
assert.equal(cmMenu.hidden,false);assert.equal(menuKeyboard.prevented,true);
for(const listener of cm('window.listeners.mousedown'))listener({target:cmElements.get('#tileActionCopy')});
assert.equal(cmMenu.hidden,false); // menu actions are not mistaken for outside clicks
for(const listener of cm('window.listeners.mousedown'))listener({target:cmElements.get('#room')});
assert.equal(cmMenu.hidden,true);
cmOpen(0,0);
for(const listener of cm('window.listeners.wheel'))listener({target:cmElements.get('#tileActionCopy')});
assert.equal(cmMenu.hidden,false); // a short viewport can scroll the menu itself
for(const listener of cm('window.listeners.wheel'))listener({target:cmCanvas});
assert.equal(cmMenu.hidden,true);
cmOpen(0,0);cm('setLayer(1)');assert.equal(cmMenu.hidden,true);
cmOpen(0,0);assert.equal(cmElements.get('#tileActionPaste').disabled,true);
cm('setMode("native")');assert.equal(cmMenu.hidden,true);
const cmNativeEvent=cmPoint(0,0);cmCanvas.listeners.contextmenu(cmNativeEvent);
assert.equal(cmNativeEvent.prevented,undefined);
console.log('Right-click selection, priority/bands, copy/paste, transparency, undo and keyboard menu passed');

/* Shift-click selects an inclusive range; Shift-drag keeps the existing pan
 * gesture. Repeated endpoints keep the same anchor and never author pixels. */
const range=editor(fixture());
range.run(`loadIniText('','range.ini');setLayer(0);$('#bSelect').onclick();`);
const rangeCanvas=range.elements.get('#map2d');
const rangePoint=(x,y,options={})=>({
  clientX:range.run(`view.x+(${x}*16+2)*view.scale`),
  clientY:range.run(`view.y+(${y}*16+2)*view.scale`),button:0,...options});
const rangeUp=()=>{for(const fn of range.run('window.listeners.mouseup'))fn({});};
const rangeMove=ev=>{for(const fn of range.run('window.listeners.mousemove'))fn(ev);};
const rangeClick=(x,y,options)=>{rangeCanvas.listeners.mousedown(rangePoint(x,y,options));rangeUp();};
const rangeIni=range.run('mergeDioramaIni()');
rangeClick(2,3);rangeClick(5,6,{shiftKey:true});
assert.equal(range.run('selectedCells.size'),16);
assert.equal(range.run('JSON.stringify(selectionRect)'),JSON.stringify({x0:2,y0:3,x1:6,y1:7}));
assert.equal(range.run('JSON.stringify(selectionAnchor)'), '[2,3]');
rangeClick(0,1,{shiftKey:true});
assert.equal(range.run('selectedCells.size'),9); /* reverse horizontal and vertical */
rangeClick(6,3,{shiftKey:true});assert.equal(range.run('selectedCells.size'),5);
rangeClick(2,7,{shiftKey:true});assert.equal(range.run('selectedCells.size'),5);
assert.equal(range.run('mergeDioramaIni()'),rangeIni);
assert.equal(range.run('configDirty'),false);
assert.equal(range.run('undoStack.length'),0);
range.run('deselect()');
assert.equal(range.run('selectionAnchor'),null);
const jitter=rangePoint(4,4,{shiftKey:true});
rangeCanvas.listeners.mousedown(jitter);
rangeMove({...jitter,clientX:jitter.clientX+2,clientY:jitter.clientY+1});rangeUp();
assert.equal(range.run('selectedCells.size'),1); /* first Shift-click seeds its own anchor */
rangeClick(6,5,{shiftKey:true});assert.equal(range.run('selectedCells.size'),6);
const panStart=rangePoint(8,6,{shiftKey:true});
const oldView=range.run('({...view})'),oldSelection=range.run('JSON.stringify(selectionRect)');
rangeCanvas.listeners.mousedown(panStart);
rangeMove({...panStart,clientX:panStart.clientX+8,clientY:panStart.clientY+6});rangeUp();
assert.equal(range.run('view.x'),oldView.x+8);
assert.equal(range.run('view.y'),oldView.y+6);
assert.equal(range.run('JSON.stringify(selectionRect)'),oldSelection);
assert.equal(range.run('JSON.stringify(selectionAnchor)'), '[4,4]');
rangeClick(1,2,{ctrlKey:true});rangeClick(3,4,{shiftKey:true});
assert.equal(range.run('selectedCells.size'),9);
assert.equal(range.run('JSON.stringify(selectionAnchor)'), '[1,2]');
range.run(`$('#bSelectRect').onclick();`);rangeClick(10,8);rangeClick(11,9,{shiftKey:true});
assert.equal(range.run('selectedCells.size'),4);
range.run(`selectRectangle(0,0,1,0);copyTiles();stampTiles(-2,-1);deselect();fitView();`);
rangeClick(-2,-1);rangeClick(1,1,{shiftKey:true});
assert.equal(range.run('selectedCells.size'),4);
assert.equal(range.run('selectedStampKeys.size'),8); // two pasted cells plus six blank edge cells
assert.equal(range.run('copyTiles()'),true);
assert.equal(range.run('tileClipboard.w'),4);assert.equal(range.run('tileClipboard.h'),3);
range.run('setLayer(1)');rangeClick(3,3,{shiftKey:true});
assert.equal(range.run('selectedCells.size'),1);
assert.equal(range.run('JSON.stringify(selectionAnchor)'), '[3,3]');
range.run(`$('#room').value='1';$('#room').onchange();`);
assert.equal(range.run('selectionAnchor'),null);
range.run(`setLayer(0);selectRectangle(0,0,0,0);copyTiles();startStamp();`);
const beforeShiftStamp=range.run('mergeDioramaIni()');
rangeClick(2,1,{shiftKey:true});
assert.equal(range.run('selectedCells.size'),6);
assert.equal(range.run('mergeDioramaIni()'),beforeShiftStamp);
range.run(`deselect();$('#bCell').onclick();`);
rangeClick(1,1,{shiftKey:true});
assert.equal(range.run('mergeDioramaIni()'),beforeShiftStamp);
console.log('Shift-click ranges, stable anchors, pasted edges and Shift-drag panning passed');

/* Bulk fill preserves opaque art and existing masks, stays cell-local even
 * with metatile scope selected, and commits originals + stamps as one undo. */
const fillData=fixture(),fillRoom=fillData.rooms[0];
const fillChars=Buffer.from(fillData.blobs[fillRoom.chars],'base64');
fillChars[0]=128; /* character zero has one opaque pixel per 8x8 quadrant */
fillData.blobs[fillRoom.chars]=fillChars.toString('base64');
const fillPalette=Buffer.from(fillData.blobs[fillRoom.palette],'base64');
fillPalette.writeUInt16LE(31,65*2);fillData.blobs[fillRoom.palette]=fillPalette.toString('base64');
fillRoom.video[0]=1;
const fill=editor(fillData);
fill.run(`loadIniText('','fill-selection.ini');setLayer(0);
  pixelBucket(room,0).byId[0]=pixelMaskSet(ZERO_PIXEL_MASK,1,1,true);
  pixelBucket(room,0).byCell[1]=pixelMaskSet(ZERO_PIXEL_MASK,0,0,true);
  selectRectangle(0,0,0,0);copyTiles();stampTiles(-1,0);
  const pastedFill=stampBucket(room,0).cells['-1,0'];
  stampBucket(room,0).cells['-1,0']={...pastedFill,
    words:[pastedFill.words[0]|0xc000,...pastedFill.words.slice(1)]};
  selectRectangle(-1,0,1,0);$('#pixelScope').value='id';
  const nativeBeforeFill=nativeFrameCanvas().getContext('2d').image.data.slice();`);
const beforeFill=fill.run('mergeDioramaIni()'),beforeFillUndo=fill.run('undoStack.length');
fill.elements.get('#pixelSelectionBlackQuick').onclick();
assert.equal(fill.run('undoStack.length'),beforeFillUndo+1);
assert.equal(fill.run('selectedCells.size'),2);assert.equal(fill.run('selectedStampKeys.size'),1);
assert.equal(fill.run('pixelIsBlack(pixelMaskAt(room,0,L,0),0,0)'),false); /* opaque red */
assert.equal(fill.run('pixelIsBlack(pixelMaskAt(room,0,L,0),1,0)'),true);
assert.equal(fill.run('pixelIsBlack(pixelMaskAt(room,0,L,1),0,0)'),true); /* prior black retained */
assert.equal(fill.run('pixelIsBlack(pixelMaskAt(room,0,L,1),8,0)'),false);
assert.equal(fill.run('pixelIsBlack(pixelMaskAt(room,0,L,2),2,2)'),false); /* unselected */
assert.equal(fill.run('pixelIsBlack(pixelMaskAt(room,0,L,2),1,1)'),true); /* inherited unchanged */
assert.equal(fill.run("pixelIsBlack(stampBucket(room,0).cells['-1,0'].black,7,7)"),false);
assert.equal(fill.run("pixelIsBlack(stampBucket(room,0).cells['-1,0'].black,0,0)"),true);
assert.deepEqual(Buffer.from(fill.run("nativeFrameCanvas().getContext('2d').image.data")),
  Buffer.from(fill.run('nativeBeforeFill')));
const afterFill=fill.run('mergeDioramaIni()');
fill.run('undo()');assert.equal(fill.run('mergeDioramaIni()'),beforeFill);
fill.run('redo()');assert.equal(fill.run('mergeDioramaIni()'),afterFill);
assert.equal(fill.run('fillSelectedTransparency()'),false); /* repeat is a no-op */
assert.equal(fill.run('undoStack.length'),beforeFillUndo+1);
fill.run(`deselect();loadIniText(${JSON.stringify(afterFill)},'filled.ini');setLayer(0);`);
assert.equal(fill.run('mergeDioramaIni()'),afterFill);
assert.equal(fill.elements.get('#pixelSelectionBlack').disabled,true);
fill.run(`loadIniText('','limit.ini');setLayer(0);selectRectangle(0,0,31,8);configDirty=false;`);
const beforeFillLimit=fill.run('mergeDioramaIni()'),beforeFillLimitUndo=fill.run('undoStack.length');
assert.equal(fill.run('fillSelectedTransparency()'),false);
assert.equal(fill.run('mergeDioramaIni()'),beforeFillLimit);
assert.equal(fill.run('undoStack.length'),beforeFillLimitUndo);
assert.equal(fill.run('configDirty'),false);
assert.match(fill.elements.get('#selectionPixelInfo').textContent,/limit is 256.*No pixels were changed/);
fill.run(`for(let row=0;row<8;row++)L.chars[row*2]=255;selectRectangle(0,0,0,0);`);
assert.equal(fill.run('fillSelectedTransparency()'),false); /* fully opaque tile */
assert.equal(fill.run('undoStack.length'),beforeFillLimitUndo);
console.log('Selection transparency fill, untouched art, mixed stamps, undo and atomic limits passed');

/* Rectangle gestures, frozen overlap copies, edge coordinates and persistent
 * masks exercise the same entry points as the authoring controls. */
run(`loadIniText('','stamp.ini');setLayer(0);actor.show=false;
  $('#pixelScope').value='cell';selectCell(0,0);
  beginOp('source pixels');editPixel(3,5);commitOp();
  brush='cell';beginOp('source band');paintCell(0,0,0);commitOp();deselect();
  $('#bSelectRect').onclick();`);
const mapCanvas=elements.get('#map2d');
const mapPoint=(x,y)=>({clientX:run(`view.x+(${x}*16+2)*view.scale`),
  clientY:run(`view.y+(${y}*16+2)*view.scale`),button:0});
mapCanvas.listeners.mousedown(mapPoint(0,0));
for(const fn of run("window.listeners.mousemove"))fn(mapPoint(1,1));
for(const fn of run("window.listeners.mouseup"))fn({});
assert.equal(run('selectedCells.size'),4);
const beforeCopy=run('mergeDioramaIni()');
assert.equal(run('copyTiles()'),true);
assert.equal(run('mergeDioramaIni()'),beforeCopy);
assert.equal(run('tileClipboard.w'),2);
assert.equal(run('tileClipboard.h'),2);
assert.equal(run('tileClipboard.cells[0].bands[0]'),0);
assert.equal(run('pixelIsBlack(tileClipboard.cells[0].black,3,5)'),true);
assert.equal(run('stampTiles(1,0)'),true); /* overlaps original source */
assert.equal(run('stampTiles(3,0)'),true); /* still uses the frozen clipboard */
assert.equal(run('stampBucket(room,0).cells["3,0"].bands[0]'),0);
assert.equal(run('pixelIsBlack(stampBucket(room,0).cells["3,0"].black,3,5)'),true);
assert.equal(run('stampBucket(room,0).cells["4,0"].black'), '0'.repeat(64));
assert.equal(run('stampTiles(-2,-1)'),true);
assert.equal(run('mapBounds(room,0,L).x0'),-2);
assert.equal(run('mapBounds(room,0,L).y0'),-1);
const scenerySaved=run('mergeDioramaIni()');
assert.match(scenerySaved,/bg1-map:eu = bounds:-2,-1,32,16/);
assert.match(scenerySaved,/bg1-stamp:eu = cell:-2,-1 metatile:00 words:/);
assert.match(scenerySaved,/bg1-pixels:eu = cell:-2,-1 black:[0-9A-F]{64}/);
const rawWords=Buffer.from(run('L.words.buffer'));
run('undo()');assert.equal(run('stampBucket(room,0).cells["-2,-1"]'),undefined);
run('redo()');assert.equal(run('mergeDioramaIni()'),scenerySaved);
run(`loadIniText(${JSON.stringify(scenerySaved)},'stamp.ini');setLayer(0);`);
assert.equal(run('mergeDioramaIni()'),scenerySaved);
assert.deepEqual(Buffer.from(run('L.words.buffer')),rawWords);
assert.equal(run('pixelIsBlack(stampBucket(room,0).cells["-2,-1"].black,3,5)'),true);
assert.equal(run('gameLayerSurfaces(room,0).L.originX'),-32);
assert.equal(run('gameLayerSurfaces(room,0).L.originY'),-16);
run('draw2d();startStamp();stampHover=[-2,-1];draw2d();deselect();');
run(`const mapBand=stampBucket(room,0).cells['-2,-1'].words[0]&0x2000?'high':'low';
  const expandedPixels=gameLayerSurfaces(room,0)[mapBand].getContext('2d').image.data;`);
assert.equal(run('expandedPixels[(5*544+3)*4+3]'),255);
assert.equal(run('expandedPixels[(5*544+4)*4+3]'),0);
/* Editing a pasted cell must leave its clipboard/source mask independent. */
run(`selectCell(-2,-1);beginOp('stamp pixel');editPixel(4,5);commitOp();`);
assert.equal(run('pixelIsBlack(stampBucket(room,0).cells["-2,-1"].black,4,5)'),true);
assert.equal(run('pixelIsBlack(tileClipboard.cells[0].black,4,5)'),false);
run('undo()');assert.equal(run('mergeDioramaIni()'),scenerySaved);
run(`$('#edgeCount').value='4';expandEdge('x1');`);
assert.equal(run('mapBounds(room,0,L).x1'),36);
run('undo()');assert.equal(run('mapBounds(room,0,L).x1'),32);
run(`selectRectangle(-2,-1,-1,0);removeStamps();`);
assert.equal(run('stampBucket(room,0).cells["-2,-1"]'),undefined);
run('undo()');assert.equal(run('mergeDioramaIni()'),scenerySaved);
run(`setMode('3d');setNativeCamera('x',-16);setNativeCamera('y',-16);`);
assert.equal(run('nativeCamera.x'),-16);
assert.equal(run('nativeCamera.y'),-16);
run(`setMode('native');`);
assert.equal(run('nativeCamera.x'),0);
assert.equal(run('nativeCamera.y'),0);
/* The stamped pixel is a real Diorama pixel, while native scanout ignores it. */
run(`room.video[0]=1;nativeCamera.x=0;nativeCamera.y=0;invalidateGameComposite();
  const nativeBeforeStamp=nativeFrameCanvas().getContext('2d').image.data.slice();
  selectRectangle(3,0,3,0);copyTiles();stampTiles(8,0);`);
assert.deepEqual(Buffer.from(run("nativeFrameCanvas().getContext('2d').image.data")),
  Buffer.from(run('nativeBeforeStamp')));
assert.equal(run('nativeBandSurfaces().images[0][0].data[((4)*256+131)*4+3]'),255);
/* Art comes from the copied character words and honours their flips. */
run(`const art=stampBucket(room,0).cells['8,0'];
  const entry=art.words[0],chr=(entry&1023)*32;
  L.chars[chr+2]=128;L.pal[((entry>>10)&7)*16+1]=0xff302010;`);
assert.equal(run("stampOriginal(L,stampBucket(room,0).cells['8,0'],0,1)"),0xff302010);
assert.equal(run("stampOriginal(L,{...stampBucket(room,0).cells['8,0'],words:[art.words[0]|0xc000,...art.words.slice(1)]},7,6)"),0xff302010);
run('setLayer(1)');
const beforeWrongLayer=run('mergeDioramaIni()');
assert.equal(run('stampTiles(0,0)'),false);
assert.equal(run('mergeDioramaIni()'),beforeWrongLayer);
run(`setLayer(0);selectRectangle(0,0,1,1);copyTiles();startStamp();`);
assert.equal(run('brush'),'stamp');
run('deselect()');assert.equal(run('brush'),'select');
/* Both capacity failures must be atomic, including map bounds and history. */
run(`const capacityCell=tileClipboard.cells[0];
  for(let i=0;i<512;i++)stampBucket(room,0).cells[i+',20']=capacityCell;`);
const beforeCapacity=run('JSON.stringify(stampBucket(room,0))');
const beforeCapacityUndo=run('undoStack.length');
assert.equal(run('stampTiles(0,25)'),false);
assert.equal(run('JSON.stringify(stampBucket(room,0))'),beforeCapacity);
assert.equal(run('undoStack.length'),beforeCapacityUndo);
run(`loadIniText('','capacity.ini');setLayer(0);
  for(let i=0;i<256;i++)pixelBucket(room,0).byId[i]='F'.repeat(64);
  selectRectangle(0,0,0,0);copyTiles();`);
const beforePixelCapacity=run('mergeDioramaIni()');
assert.equal(run('stampTiles(-1,0)'),false);
assert.equal(run('mergeDioramaIni()'),beforePixelCapacity);
console.log('Rectangle copy/stamp, overlap, signed edges, masks, atomic limits and undo passed');

/* Deleting an extension tightens saved scenery without taking away the empty
 * workspace needed for more painting. Old, oversized INI bounds also tighten. */
const elasticEditor=editor(fixture()),elastic=elasticEditor.run;
elastic(`loadIniText('','elastic.ini');setLayer(0);selectRectangle(0,0,0,0);
  copyTiles();stampTiles(-6,-9);stampTiles(0,19);`);
const extendedIni=elastic('roomSectionIni(room)');
assert.match(extendedIni,/bg1-map = bounds:-6,-9,32,20/);
elastic('selectRectangle(0,19,0,19);removeStamps();');
const trimmedIni=elastic('roomSectionIni(room)');
assert.match(trimmedIni,/bg1-map = bounds:-6,-9,32,16/);
assert.equal(elastic('mapBounds(room,0,L).y1'),20);
assert.equal(elastic('sceneryBounds(room,0).y1'),16);
assert.equal(elastic('gameLayerSurfaces(room,0).L.h'),400);
assert.match(elasticEditor.elements.get('#stampInfo').textContent,/saved bounds -6,-9 to 31,15/);
elastic("setMode('3d');setNativeCamera('y',95);");
assert.equal(elastic('nativeCamera.y'),31);
elastic('undo()');assert.equal(elastic('roomSectionIni(room)'),extendedIni);
assert.equal(elasticEditor.elements.get('#nativeCameraY').max,'95');
elastic("setNativeCamera('y',95);");
elastic('redo()');assert.equal(elastic('roomSectionIni(room)'),trimmedIni);
assert.equal(elastic('nativeCamera.y'),31);
assert.equal(elasticEditor.elements.get('#nativeCameraY').max,'31');
elastic("setMode('2d');");
const staleIni=trimmedIni.replace('bounds:-6,-9,32,16','bounds:-6,-9,32,20');
elastic(`loadIniText(${JSON.stringify(staleIni)},'stale.ini');setLayer(0);`);
assert.equal(elastic('roomSectionIni(room)'),trimmedIni);
assert.equal(elastic('gameLayerSurfaces(room,0).L.h'),400);
elastic(`$('#terrain').value='1';$('#terrain').onchange();`);
assert.equal(elastic('sceneryBounds(room,0).y0'),0); // different JP terrain
elastic(`$('#terrain').value='0';$('#terrain').onchange();
  selectRectangle(-6,-9,-6,-9);removeStamps();`);
assert.doesNotMatch(elastic('roomSectionIni(room)'),/bg1-(map|stamp)/);
assert.equal(elastic('gameLayerSurfaces(room,0).L.h'),256);
elastic(`$('#edgeCount').value='4';expandEdge('y1');`);
assert.doesNotMatch(elastic('roomSectionIni(room)'),/bg1-map/);
console.log('Elastic saved bounds, stale workspace, regional isolation and deletion undo passed');

/* Empty edge cells are selectable without authoring records. A fill or pixel
 * stroke materializes transparent source art, never an arbitrary ROM tile. */
const blanksEditor=editor(fixture()),blanks=blanksEditor.run;
blanks(`loadIniText('','blank.ini');setLayer(0);actor.show=false;
  $('#edgeCount').value='2';expandEdge('x0');selectOnlyTile(-2,0);`);
const beforeBlank=blanks('roomSectionIni(room)');
assert.equal(blanks('selectedStampKeys.size'),1);
assert.equal(blanks('Object.keys(stampBucket(room,0).cells).length'),0);
assert.equal(blanks('focusedOriginal(0,0)'),null);
assert.equal(blanksEditor.elements.get('#pixelSelectionBlackQuick').disabled,false);
blanks(`openTileMenu(-2,0,40,40);`);
assert.equal(blanksEditor.elements.get('#tileActionFill').disabled,false);
blanks("$('#tileActionFill').onclick();");
assert.equal(blanks("stampBucket(room,0).cells['-2,0'].blank"),true);
assert.equal(blanks("stampBucket(room,0).cells['-2,0'].black"),'F'.repeat(64));
const filledBlank=blanks('roomSectionIni(room)');
assert.match(filledBlank,/cell:-2,0 metatile:00 words:blank bands:1,1,1,1/);
blanks('undo()');assert.equal(blanks('roomSectionIni(room)'),beforeBlank);
blanks('redo()');assert.equal(blanks('roomSectionIni(room)'),filledBlank);
blanks(`selectOnlyTile(-1,1);beginOp('partial blank');editPixel(3,5);commitOp();`);
assert.equal(blanks('focusedOriginal(3,5)'),null);
assert.equal(blanks('pixelIsBlack(focusedMask(),3,5)'),true);
assert.equal(blanks('pixelIsBlack(focusedMask(),4,5)'),false);
blanks('selectRectangle(-2,0,-1,1);copyTiles();stampTiles(-2,2);');
assert.equal(blanks("stampBucket(room,0).cells['-1,2'].blank"),true);
assert.equal(blanks("stampOriginal(L,stampBucket(room,0).cells['-1,2'],0,0)"),null);
const blankRoundTrip=blanks('roomSectionIni(room)');
blanks(`loadIniText(${JSON.stringify(blankRoundTrip)},'blank.ini');setLayer(0);`);
assert.equal(blanks('roomSectionIni(room)'),blankRoundTrip);
assert.equal(blanks("stampBucket(room,0).cells['-1,1'].blank"),true);
blanks(`selectOnlyTile(-1,1);pixelBulk('reset');`);
assert.equal(blanks('focusedOriginal(3,5)'),null);
assert.equal(blanks('focusedMask()'),'0'.repeat(64));
blanks(`selectRectangle(-2,4,-1,5);fillSelectedTransparency();`);
assert.equal(blanks('selectedStampKeys.size'),4);
assert.equal(blanks("stampBucket(room,0).cells['-2,4'].black"),'F'.repeat(64));
blanks(`loadIniText('','blank-limit.ini');setLayer(0);$('#edgeCount').value='1';expandEdge('x0');
  for(let i=0;i<512;i++)stampBucket(room,0).cells['0,'+i]=blankTile();selectOnlyTile(-1,0);`);
const blankLimitBefore=blanks('roomSectionIni(room)'),blankLimitHistory=blanks('undoStack.length');
assert.equal(blanks('fillSelectedTransparency()'),false);
assert.equal(blanks('roomSectionIni(room)'),blankLimitBefore);
assert.equal(blanks('undoStack.length'),blankLimitHistory);
console.log('Blank edge selection, context fill, partial pixels, copy, round trip and atomic limits passed');

/* Designer offsets are saved per BG1 terrain family and do not move as the
 * authoring workspace grows, or alter the native preview camera. */
const framingEditor=editor(fixture()),framing=framingEditor.run;
framing(`loadIniText('','framing.ini');setLayer(0);nativeCamera.x=120;nativeCamera.y=31;
  $('#framingX').value='-40';$('#framingY').value='0';$('#framingX').onchange();`);
const savedFraming=framing('roomSectionIni(room)');
assert.match(savedFraming,/framing = x:-40 y:0/);
assert.equal(framing('nativeCamera.x'),120);
assert.equal(framing('nativeCamera.y'),31);
framing(`$('#edgeCount').value='4';expandEdge('x0');expandEdge('y0');`);
assert.equal(framing('roomFraming(room).x'),-40);
assert.equal(framing('roomSectionIni(room)'),savedFraming);
framing(`$('#framingReset').onclick();`);
assert.doesNotMatch(framing('roomSectionIni(room)'),/framing =/);
framing('undo()');assert.equal(framing('roomSectionIni(room)'),savedFraming);
assert.equal(framingEditor.elements.get('#framingX').value,'-40');
framing(`$('#terrain').value='1';$('#terrain').onchange();`);
assert.equal(framing('roomFraming(room).x'),0);
framing(`loadIniText(${JSON.stringify(savedFraming)},'framing.ini');setLayer(0);
  $('#terrain').value='0';$('#terrain').onchange();`);
assert.equal(framing('roomFraming(room).x'),-40);
framing("$('#framingX').value='65';$('#framingX').onchange();");
assert.equal(framing('roomFraming(room).x'),-40);
assert.equal(framing('roomSectionIni(room)'),savedFraming);
console.log('Saved relative framing, regional isolation, elastic bounds, reset and undo passed');

/* The actual map gesture edits offsets, not the preview camera or tiles. */
const guideEditor=editor(fixture()),guide=guideEditor.run;
guide(`loadIniText('','guide.ini');setLayer(0);nativeCamera.x=120;nativeCamera.y=31;
  selectCell(0,0);startFramingTool();`);
const guideCanvas=guideEditor.elements.get('#map2d');
const guideEvent=(dx=0,dy=0)=>({button:0,preventDefault(){},
  clientX:guide('view.x+(nativeCamera.x+128)*view.scale')+dx*guide('view.scale'),
  clientY:guide('view.y+(nativeCamera.y+113)*view.scale')+dy*guide('view.scale')});
const guideMove=event=>{for(const fn of guide('window.listeners.mousemove'))fn(event);};
const guideUp=()=>{for(const fn of guide('window.listeners.mouseup'))fn({});};
const guideKey=(key,shiftKey=false)=>{for(const fn of guide('window.listeners.keydown'))
  fn({key,shiftKey,target:{tagName:'CANVAS'},preventDefault(){}});};
assert.equal(guide('brush'),'framing');
const guideInitial=guide('roomSectionIni(room)'),guideHistory=guide('undoStack.length');
guideCanvas.listeners.mousedown(guideEvent());
guideMove(guideEvent(40,-16));guideMove(guideEvent(32,-8));guideUp();
assert.match(guide('roomSectionIni(room)'),/framing = x:32 y:-8/);
assert.equal(guide('undoStack.length'),guideHistory+1);
assert.equal(guide('nativeCamera.x'),120);assert.equal(guide('nativeCamera.y'),31);
assert.equal(guide('selectedCells.size'),1);
assert.equal(guide('Object.keys(stampBucket(room,0).cells).length'),0);
guide('ctx.strokes=[];drawFramingGuide();');
const guideStrokes=guideCanvas.context.strokes.filter(s=>['#e6eaf2','#ffdc74'].includes(s.color));
assert.deepEqual({...guideStrokes[0]}, {
  x:guide('view.x+120*view.scale'),y:guide('view.y+32*view.scale'),
  w:guide('256*view.scale'),h:guide('224*view.scale'),color:'#e6eaf2'});
assert.equal(guideStrokes[1].x,guide('view.x+152*view.scale'));
assert.equal(guideStrokes[1].y,guide('view.y+24*view.scale'));
guide('undo()');assert.equal(guide('roomSectionIni(room)'),guideInitial);
guide('redo()');assert.equal(guide('roomFraming(room).x'),32);
guideKey('ArrowRight',true);assert.equal(guide('roomFraming(room).x'),48);
guideKey('ArrowDown');assert.equal(guide('roomFraming(room).y'),-7);
const beforeCancel=guide('roomSectionIni(room)'),beforeCancelHistory=guide('undoStack.length');
guideCanvas.listeners.mousedown(guideEvent());guideMove(guideEvent(200,-200));
assert.equal(guide('roomFraming(room).x'),64);assert.equal(guide('roomFraming(room).y'),-64);
guideKey('Escape');guideUp();
assert.equal(guide('roomSectionIni(room)'),beforeCancel);
assert.equal(guide('undoStack.length'),beforeCancelHistory);
assert.equal(guide('brush'),'select');
guide(`$('#framingReset').onclick();startFramingTool();`);
const noOpHistory=guide('undoStack.length');
guideCanvas.listeners.mousedown(guideEvent());guideMove(guideEvent(3,4));
guideMove(guideEvent());guideUp();
assert.equal(guide('undoStack.length'),noOpHistory);
assert.equal(guide('stampBucket(room,0).framing'),undefined);
guide(`$('#framingX').value='-12';$('#framingY').value='0';$('#framingX').onchange();`);
assert.equal(guide('mode'),'2d'); // numeric tweaks keep the guide visible
guide(`$('#terrain').value='1';$('#terrain').onchange();`);
assert.equal(guide('roomFraming(room).x'),0);
guide('setLayer(1);');assert.equal(guide('brush'),'select');
guide('ctx.strokes=[];drawFramingGuide();');assert.equal(guideCanvas.context.strokes.length,0);
guide('startFramingTool();');assert.equal(guide('bgIndex'),0);
guide(`$('#showFraming').checked=false;$('#showFraming').onchange();ctx.strokes=[];drawFramingGuide();`);
assert.equal(guide('brush'),'select');assert.equal(guideCanvas.context.strokes.length,0);
console.log('Native frame overlay, drag/nudge, limits, cancel, single-step undo and regional isolation passed');

/* Widescreen coverage uses the game's PAR and equal whole-column margins.
 * Guide preferences never author offsets, and every frame shares one anchor. */
const wideEditor=editor(fixture()),wide=wideEditor.run;
wide(`loadIniText('','wide.ini');setLayer(0);nativeCamera.x=120;nativeCamera.y=31;startFramingTool();`);
const wideIni=wide('roomSectionIni(room)'),wideHistory=wide('undoStack.length');
assert.equal(wide('framingRect(true,16/9).w'),342);
assert.equal(wide('framingRect(true,16/10).w'),308);
assert.equal(wide('framingRect(true,16/9).x'),77);
assert.equal(wide('framingRect(true,16/10).x'),94);
assert.equal(wide('framingRect(true,16/9).y'),32);
wide('ctx.strokes=[];ctx.labels=[];drawFramingGuide();');
const wideCanvas=wideEditor.elements.get('#map2d');
assert.ok(wideCanvas.context.labels.some(l=>l.text==='16:9'));
assert.ok(wideCanvas.context.labels.some(l=>l.text==='16:10'));
const cyan=wideCanvas.context.strokes.find(s=>s.color==='#65dce9');
const purple=wideCanvas.context.strokes.find(s=>s.color==='#c49aff');
assert.equal(cyan.w,wide('342*view.scale'));assert.equal(purple.w,wide('308*view.scale'));
assert.equal(cyan.x+cyan.w/2,purple.x+purple.w/2);
wide(`$('#framingPixelAspect').value='square';$('#framingPixelAspect').onchange();`);
assert.equal(wide('framingRect(true,16/9).w'),400);
assert.equal(wide('framingRect(true,16/10).w'),360);
assert.match(wideEditor.elements.get('#framingViewportInfo').textContent,/16:9: 400×224.*16:10: 360×224/);
wide(`$('#framing169').checked=false;$('#framing169').onchange();ctx.strokes=[];drawFramingGuide();`);
assert.equal(wide('framingCoverageRect().w'),360);
assert.ok(!wideCanvas.context.strokes.some(s=>s.color==='#65dce9'));
assert.ok(wideCanvas.context.strokes.some(s=>s.color==='#c49aff'));
wide(`$('#framing1610').checked=false;$('#framing1610').onchange();`);
assert.equal(wide('framingCoverageRect().w'),256);
assert.equal(wide('roomSectionIni(room)'),wideIni);assert.equal(wide('undoStack.length'),wideHistory);
wide(`$('#framing169').checked=true;$('#framing169').onchange();`);
// Drag the widescreen-only area to the left of the gold native rectangle.
const wideStart={button:0,preventDefault(){},clientX:wide('view.x+60*view.scale'),
  clientY:wide('view.y+140*view.scale')};
wideCanvas.listeners.mousedown(wideStart);
assert.equal(wide('drag.framing'),true);
for(const fn of wide('window.listeners.mousemove'))fn({...wideStart,
  clientX:wideStart.clientX+20*wide('view.scale'),clientY:wideStart.clientY-8*wide('view.scale')});
for(const fn of wide('window.listeners.mouseup'))fn({});
assert.match(wide('roomSectionIni(room)'),/framing = x:20 y:-8/);
assert.equal(wide('framingRect().x+framingRect().w/2'),
  wide('framingRect(true,16/9).x+framingRect(true,16/9).w/2'));
assert.equal(wide('framingRect().y'),wide('framingRect(true,16/10).y'));
assert.equal(wide('nativeCamera.x'),120);assert.equal(wide('nativeCamera.y'),31);
wide('undo()');assert.equal(wide('roomSectionIni(room)'),wideIni);
console.log('16:9/16:10 guide coverage, pixel aspect, toggles, shared drag and preview-only preferences passed');

wide(`$('#framingProjection').value='diorama';$('#framingProjection').onchange();`);
assert.equal(wide('framingRect(true,16/10).w'),473);
assert.equal(wide('framingRect(true,16/10).h'),296);
assert.equal(wide('framingRect(true,16/9).w'),525);
assert.equal(wide('framingRect().h'),224);
assert.equal(wide('framingRect().y+framingRect().h/2'),
  wide('framingRect(true,16/10).y+framingRect(true,16/10).h/2'));
wide(`$('#framingDistance').value='4';$('#framingDistance').onchange();`);
assert.equal(wide('framingRect(true,16/10).w'),582);
wide(`$('#framingPixelAspect').value='crt';$('#framingPixelAspect').onchange();`);
assert.equal(wide('framingRect(true,16/10).w'),499);
wide(`$('#framingDistance').value='NaN';$('#framingDistance').onchange();`);
assert.equal(wide('framingDistance'),4);
wide(`$('#framingDistance').value='0';$('#framingDistance').onchange();`);
assert.equal(wide('framingDistance'),4);
assert.equal(wide('roomSectionIni(room)'),wideIni);
assert.equal(wide('undoStack.length'),wideHistory);
console.log('Diorama coverage estimate follows runtime FOV, distance and pixel aspect without exporting preferences');

/* Trim only empty authoring margins; artwork, pixel edits and framing survive. */
const trimEditor=editor(fixture()),trim=trimEditor.run;
trim(`loadIniText('','trim.ini');setLayer(0);selectRectangle(0,0,0,0);copyTiles();
  stampTiles(-2,-1);fillSelectedTransparency();
  $('#framingX').value='-20';$('#framingY').value='0';setRoomFraming();
  $('#edgeCount').value='4';expandEdge('x0');expandEdge('y1');
  selectRectangle(-6,19,-6,19);`);
const trimIni=trim('roomSectionIni(room)'),trimWorkspace=trim('JSON.stringify(mapBounds(room,0))');
assert.equal(trimEditor.elements.get('#trimEdgeSpace').disabled,false);
assert.equal(trim('trimEdgeSpace()'),true);
assert.equal(trim('roomSectionIni(room)'),trimIni);
assert.equal(trim('JSON.stringify(mapBounds(room,0))'),trim('JSON.stringify(sceneryBounds(room,0))'));
assert.equal(trim('selectedStampKeys.size'),0);assert.equal(trim('selectionAnchor'),null);
assert.equal(trim('pixelStamp'),null);assert.equal(trim('roomFraming(room).x'),-20);
assert.equal(trimEditor.elements.get('#trimEdgeSpace').disabled,true);
trim('undo()');assert.equal(trim('JSON.stringify(mapBounds(room,0))'),trimWorkspace);
trim('redo()');assert.equal(trim('roomSectionIni(room)'),trimIni);
trim(`$('#terrain').value='1';$('#terrain').onchange();expandEdge('x1');trimEdgeSpace();`);
assert.equal(trim('JSON.stringify(mapBounds(room,0))'),JSON.stringify({x0:0,y0:0,x1:32,y1:16}));
assert.equal(trim('trimEdgeSpace()'),false);
trim(`$('#terrain').value='0';$('#terrain').onchange();`);
assert.equal(trim('roomSectionIni(room)'),trimIni);
console.log('Trim unused workspace, preserved artwork/framing, original bounds, undo and regional isolation passed');

/* Applied state must describe the selection, independently of the paintbrush.
 * Export badges follow serialized data, including undo to/from a savepoint. */
const feedbackEditor=editor(fixture()),fb=feedbackEditor.run,fbElements=feedbackEditor.elements;
fb(`loadIniText('','feedback.ini');setLayer(0);actor.show=false;
  const feedbackBaseline=mergeDioramaIni();selectRectangle(2,0,3,1);setBand(0);`);
assert.equal(fb('mergeDioramaIni()'),fb('feedbackBaseline'));
assert.equal(fb('editorHasUnexportedChanges()'),false);
assert.equal(fbElements.get('#selectionSummary').textContent,'4 tiles selected');
assert.equal(fbElements.get('#selectionApplied').textContent,'Applied band: Normal');
assert.equal(fbElements.get('#selectionBand1').attributes['aria-pressed'],'true');
assert.equal(fb('pixelCell'),35); // rectangle endpoint, rather than a stale prior click
assert.match(fbElements.get('#selectionScope').textContent,/US · Diorama only/);
const fbHistory=fb('undoStack.length');
fb("$('#selectionBand2').onclick();");
assert.equal(fb('undoStack.length'),fbHistory+1);
assert.equal(fbElements.get('#selectionApplied').textContent,'Applied band: Priority');
assert.equal(fb('band'),0); // applying a selection action does not change the brush
assert.equal(fb('currentTileChanges().cells.length'),4);
assert.equal(fbElements.get('#saveState').textContent,'Unexported changes');
fb('undo()');
assert.equal(fb('editorHasUnexportedChanges()'),false);
assert.equal(fb('currentTileChanges().cells.length'),0);
fb("redo();$('#export').onclick();");
assert.equal(fb('document.exportedText'),undefined); // opening is not a download or savepoint
assert.equal(fbElements.get('#saveState').textContent,'Unexported changes');
assert.equal(fbElements.get('#exportText').value,fb('roomSectionIni(room)'));
await fb("$('#exportCopy').onclick();");
assert.equal(fb('document.clipboardText'),fb('roomSectionIni(room)'));
assert.equal(fbElements.get('#saveState').textContent,'Matches last copy');
fb("$('#exportClose').onclick();");
fb('undo()');
assert.equal(fbElements.get('#saveState').textContent,'Unexported changes');
fb('redo()');
assert.equal(fbElements.get('#saveState').textContent,'Matches last copy');
fb(`selectOnlyTile(0,0);$('#pixelScope').value='cell';beginOp('one pixel');editPixel(3,5);commitOp();
  selectRectangle(0,0,1,0);`);
assert.match(fbElements.get('#selectionApplied').textContent,/Normal/);
fb(`selectedCells.add(2);refreshSelectionControls();`);
assert.match(fbElements.get('#selectionApplied').textContent,/Mixed \(Normal, Priority\)/);
assert.equal(fb('currentTileChanges().pixels'),1);

/* Pixel inspector focuses the range; closing it and comparisons are read-only.
 * Original art excludes black masks, pasted cells, edge bounds and tint. */
fb(`selectRectangle(0,0,1,0);$('#selectionPixels').onclick();`);
assert.equal(fbElements.get('#pixelInspector').hidden,false);
assert.equal(fb('pixelCell'),1);
fb("$('#pixelInspectorClose').onclick();");
assert.equal(fbElements.get('#pixelInspector').hidden,true);
assert.equal(fb('selectedCells.size'),2);
const fbCanvas=fbElements.get('#map2d');
const fbPoint=fb(`({clientX:view.x+8*view.scale,clientY:view.y+8*view.scale})`);
fbCanvas.listeners.dblclick(fbPoint);
assert.equal(fbElements.get('#pixelInspector').hidden,false);
assert.equal(fb('pixelCell'),0);
assert.equal(fb('selectedCells.size'),1);
fb("$('#pixelInspectorClose').onclick();copyTiles();");
assert.equal(fbElements.get('#selectionPaste').disabled,false);
assert.equal(fb('stampTiles(-1,0)'),true);
fb(`const beforeCompare=mergeDioramaIni();tint=true;invalidateGameComposite();
  const editedSurface=gameLayerSurfaces(room,0);
  const originalSurface=gameLayerSurfaces(room,0,true);
  $('#compareOriginal').onclick();`);
assert.equal(fb('compareOriginal'),true);
assert.equal(fb('originalSurface.L.originX'),0);
assert.equal(fb('originalSurface.L.w'),512);
assert.equal(fb('editedSurface.L.originX'),-16);
assert.equal(fb('editedSurface.L.w'),528);
assert.equal(fb('originalSurface.low.getContext("2d").image.data.some(v=>v!==0)'),false);
assert.equal(fb('editedSurface.low.getContext("2d").image.data.some(v=>v!==0)'),true);
assert.equal(fb('mergeDioramaIni()'),fb('beforeCompare'));
fb("$('#selectionBand0').onclick();");
assert.equal(fb('compareOriginal'),false);
assert.equal(fb('currentTileChanges().pasted'),1);
fb("$('#selectChanges').onclick();");
assert.equal(fb('selectedCells.size+selectedStampKeys.size'),6);
assert.ok(fb('activeSelectionPosition()'));
fb("$('#previousChange').onclick();");
assert.equal(fb('selectedCells.size+selectedStampKeys.size'),1);
assert.match(fbElements.get('#tileActionStatus').textContent,/Edit \d+ of 6/);
fb("selectOnlyTile(20,10);$('#nextChange').onclick();");
assert.equal(fb('pixelStamp'),'-1,0');
fb("$('#selectionPreview').onclick();");
assert.equal(fb('mode'),'3d');
assert.equal(fbElements.get('#pixelInspector').hidden,true);
assert.equal(fbElements.get('#selectionTools').hidden,true);
assert.equal(fbElements.get('#compareOriginal').disabled,true);
assert.equal(fb('view.scale>=2'),true);
fb("setMode('2d');selectRectangle(8,2,9,2);$('#selectionPaste').onclick();");
assert.ok(fb('stampBucket(room,0).cells["8,2"]'));
assert.equal(fb('stampBucket(room,0).cells["9,2"]'),undefined);
console.log('Selection actions, applied state, pixel inspector, original comparison, change navigation and export savepoints passed');

/* A copied room is a complete replacement, not an append-only patch. Other
 * rooms stay dirty; clipboard failures and partial selections never save them. */
const exportEditor=editor(fixture()),ex=exportEditor.run,exElements=exportEditor.elements;
const exportSource=['# file preamble','[layers:1:1] # room comment',
  '# keep this comment','obj2 = z:0.7','unknown-setting = keep',
  'bg1-map:us+jp+eu = bounds:-1,-1,32,16',
  'bg1-stamp:us+jp+eu = cell:-1,-1 metatile:00 words:0010,0010,0010,0010 bands:1,1,1,1',
  'bg1-virtual = cells:0,0-2,0 band:0',
  'bg2-virtual:us+jp+eu = cells:1,1-2,1 band:0',
  '[layers:01:02]','obj2 = z:0.9','[layers:01:01:camera:0]','bg1 = z:0.8',
  '[layers:01:01]','bg1-virtual = cells:1,0-1,0 band:2',
  'bg1-virtual:jp = cells:2,1-2,1 band:2',
  'bg1-stamp:us+jp+eu = cell:-1,-1 metatile:00 words:0020,0020,0020,0020 bands:1,1,1,1',
  ''].join('\r\n');
ex(`loadIniText(${JSON.stringify(exportSource)},'levels.ini');setLayer(0);
  beginOp('room one');paintCell(10,0,2);commitOp();
  $('#room').value='1';$('#room').onchange();
  beginOp('room two');paintCell(11,0,2);commitOp();
  $('#room').value='0';$('#room').onchange();$('#export').onclick();`);
const section=exElements.get('#exportText').value;
assert.equal((section.match(/^\[/gm)||[]).length,1);
assert.ok(section.startsWith('[layers:01:01]\n'));
assert.match(section,/# room comment\n# keep this comment\nobj2 = z:0.7/);
assert.match(section,/unknown-setting = keep/);
assert.doesNotMatch(section,/01:02|camera:0|z:0.8|file preamble/);
assert.match(section,/bg1-virtual:jp = cells:2,1-2,1 band:2/);
assert.match(section,/bg2-virtual:us\+jp\+eu = cells:1,1-2,1 band:0/);
assert.equal((section.match(/-stamp/g)||[]).length,1); // last overlapping paste wins, regions share one line
assert.match(section,/words:0020,0020,0020,0020/);
assert.equal(exElements.get('#exportDlg').open,true);
assert.equal(ex('document.exportedText'),undefined);
ex("$('#exportClose').onclick();");
assert.equal(ex('editorHasUnexportedChanges()'),true);
ex("$('#export').onclick();");
await ex("$('#exportCopy').onclick();");
assert.equal(ex('document.clipboardText'),section);
assert.equal(ex('editorHasUnexportedChanges()'),true); // room 2 still needs export
assert.equal(exElements.get('#saveState').textContent,'Unexported changes');
ex(`$('#exportClose').onclick();$('#room').value='1';$('#room').onchange();$('#export').onclick();`);
await ex("$('#exportCopy').onclick();");
assert.equal(ex('editorHasUnexportedChanges()'),false);
ex("$('#exportClose').onclick();undo();");
assert.equal(ex('editorHasUnexportedChanges()'),true);
ex('redo()');
assert.equal(ex('editorHasUnexportedChanges()'),false);

/* A new edit with blocked clipboard access keeps its dirty state. Keyboard
 * copy is available, and editor shortcuts cannot intercept text selection. */
ex(`beginOp('another edit');paintCell(12,0,2);commitOp();$('#export').onclick();
  navigator.clipboard.writeText=async()=>{throw new Error('blocked');};`);
await ex("$('#exportCopy').onclick();");
assert.match(exElements.get('#exportStatus').textContent,/Ctrl\/Cmd-C/);
assert.equal(ex('editorHasUnexportedChanges()'),true);
assert.equal(exElements.get('#exportText').selectionEnd,exElements.get('#exportText').value.length);
ex(`$('#exportText').selectionEnd=5;$('#exportText').listeners.copy({defaultPrevented:false});`);
assert.equal(ex('editorHasUnexportedChanges()'),true);
ex(`$('#exportSelect').onclick();$('#exportText').listeners.copy({defaultPrevented:true});`);
assert.equal(ex('editorHasUnexportedChanges()'),true);
ex(`for(const callback of window.listeners.keydown)callback({key:'c',ctrlKey:true,
  target:$('#exportText'),preventDefault(){throw new Error('Editor intercepted text copy');}});
  $('#exportText').listeners.copy({defaultPrevented:false});`);
assert.equal(ex('editorHasUnexportedChanges()'),false);
ex(`$('#exportClose').onclick();beginOp('fallback copy');paintCell(13,0,2);commitOp();
  $('#export').onclick();document.copyFallbackSuccess=true;`);
await ex("$('#exportCopy').onclick();");
assert.equal(ex('document.clipboardText'),exElements.get('#exportText').value);
assert.equal(ex('editorHasUnexportedChanges()'),false);
ex("$('#exportDownload').onclick();");
assert.equal(ex('document.exportedText'),ex('mergeDioramaIni()'));
assert.equal(exElements.get('#saveState').textContent,'Matches last export');

/* Round trip the copied block alone: region bands, overlapping stamps and
 * non-owned settings survive. Resetting the last edit still emits a header. */
const exportReload=editor(fixture());
exportReload.run(`loadIniText(${JSON.stringify(section)},'one-room.ini');setLayer(0);`);
assert.equal(exportReload.run('roomSectionIni(room)'),section);
assert.equal(exportReload.run('bandAt(st,L,2,0)'),2);
assert.equal(exportReload.run('bandAt(st,L,0,0)'),0);
assert.equal(exportReload.run('bandAt(st,L,4,0)'),0);
exportReload.run("loadIniText('','empty.ini');$('#export').onclick();");
assert.equal(exportReload.elements.get('#exportText').value,'[layers:01:01]\n');
assert.equal(exportReload.run('editorHasUnexportedChanges()'),false);
console.log('Room-only export, preserved settings, overlap deduplication, clipboard fallbacks and per-room savepoints passed');

/* Highlight filters inspect resolved bands, never the brush or just ROM flags.
 * Filtering is read-only; explicit Select highlighted enables a bulk action. */
fb(`const beforeHighlight=mergeDioramaIni();deselect();setBand(0);
  $('#highlightBy').value='2';$('#highlightBy').onchange();`);
assert.equal(fb('currentHighlightedTiles().length'),4);
assert.equal(fb('selectedCells.size'),0);
assert.equal(fb('mergeDioramaIni()'),fb('beforeHighlight'));
assert.equal(fbElements.get('#nextChange').textContent,'Next match');
fb("$('#selectChanges').onclick();");
assert.equal(fb('selectedCells.size'),4);
assert.equal(fb('selectedStampKeys.size'),0);
fb("$('#highlightBy').value='0';$('#highlightBy').onchange();");
assert.equal(fb('currentHighlightedTiles().length'),1);
assert.equal(fb('currentHighlightedTiles()[0].x'),-1);
fb("$('#selectChanges').onclick();");
assert.equal(fb('selectedCells.size'),0);
assert.equal(fb('selectedStampKeys.has("-1,0")'),true);
fb("$('#editOutlines').onclick();refreshEditorFeedback();");
assert.equal(fb('showEditOutlines'),false);
assert.match(fbElements.get('#highlightSummary').textContent,/highlights hidden/);
fb("$('#highlightBy').value='modified';$('#highlightBy').onchange();");
assert.equal(fb('showEditOutlines'),true);
assert.equal(fb('currentHighlightedTiles().length'),7); // four bands, one pixel, two pastes
fb("$('#terrain').value='1';$('#terrain').onchange();");
assert.equal(fb('currentHighlightedTiles().length'),0);
fb("$('#highlightBy').value='2';$('#highlightBy').onchange();");
assert.equal(fb('currentHighlightedTiles().length'),512); // mixed priority + normal quadrants
fb("$('#highlightBy').value='1';$('#highlightBy').onchange();");
assert.equal(fb('currentHighlightedTiles().length'),512);
fb("selectOnlyTile(0,0);applySelectionBand(0);");
assert.equal(fb('currentHighlightedTiles().length'),511);
fb("$('#highlightBy').value='2';$('#highlightBy').onchange();");
assert.equal(fb('currentHighlightedTiles().length'),511);
fb('undo()');
assert.equal(fb('currentHighlightedTiles().length'),512);
fb("selectOnlyTile(0,0);copyTiles();stampTiles(0,0);applySelectionBand(1);");
assert.equal(fb('currentHighlightedTiles().length'),511); // paste hides the original priority quadrant
fb("$('#terrain').value='0';$('#terrain').onchange();");
assert.equal(fb('currentHighlightedTiles().length'),4);
fb('deselect();ctx.strokes=[];drawNow();');
assert.equal(fb('ctx.strokes.filter(s=>s.color==="#e0913a").length'),4);
fb("$('#selectChanges').onclick();ctx.strokes=[];drawNow();");
assert.equal(fb('ctx.strokes.filter(s=>s.color==="#e0913a").length'),4);
assert.equal(fb('ctx.strokes.filter(s=>s.color==="#5aa9ff").length'),4);
assert.ok(fb('ctx.strokes.find(s=>s.color==="#e0913a").w < ctx.strokes.find(s=>s.color==="#5aa9ff").w'));
fb("deselect();$('#compareOriginal').onclick();ctx.strokes=[];drawNow();");
assert.equal(fb('ctx.strokes.filter(s=>s.color==="#e0913a").length'),0);
fb("$('#highlightBy').value='modified';$('#highlightBy').onchange();ctx.strokes=[];drawNow();");
assert.equal(fb('ctx.strokes.filter(s=>s.color==="#f0c55a").length'),7);
fb(`$('#highlightBy').value='1';$('#highlightBy').onchange();$('#selectChanges').onclick();
  const beforeRejectedFill=mergeDioramaIni();$('#pixelSelectionBlackQuick').onclick();`);
assert.equal(fb('mergeDioramaIni()'),fb('beforeRejectedFill'));
assert.match(fbElements.get('#tileActionStatus').textContent,/limit is 256.*No pixels were changed/);
fb("selectedStampKeys.add('-1,0');refreshSelectionControls();$('#copyTilesQuick').onclick();");
assert.match(fbElements.get('#tileActionStatus').textContent,/rectangle of up to 512/);
console.log('Modified/band highlighting, mixed quadrants, pasted overrides, regional isolation and bulk selection passed');

if (process.argv[2]) {
  const html = fs.readFileSync(process.argv[2], 'utf8');
  const match = html.match(/window\.__ACTION_BG__=(.*);<\/script>/);
  assert.ok(match, 'Built HTML must embed its room data');
  const data = JSON.parse(match[1]);
  assert.equal(data.rooms.length, 49);
  assert.equal(data.terrainProfiles.length, 3);
  const actual = editor(data);
  const sharing=actual.run(`DATA.rooms.map(r=>({group:r.group,map:r.map,
    bg1:editFamilies(r,0).map(f=>f.mask),bg2:editFamilies(r,1).map(f=>f.mask)}))`);
  assert.ok(sharing.every(r=>r.bg2.length===1&&r.bg2[0]===7));
  console.log(`Regional BG1 sharing: ${sharing.filter(r=>r.bg1.length===1).length} fully shared, `
    +`${sharing.filter(r=>r.bg1.length===2).length} with two layouts, `
    +`${sharing.filter(r=>r.bg1.length===3).length} with three layouts; all BG2 shared`);
  console.log('Kassandora 3:4 terrain masks:', sharing.find(r=>r.group===3&&r.map===4).bg1);
  let verified = 0;
  for (let index = 0; index < data.rooms.length; index++) {
    assert.equal(data.rooms[index].terrainVariants.length, 3);
    for (const profile of data.terrainProfiles) {
      assert.equal(actual.run(`verifyRoomNativeGolden(terrainRoom(DATA.rooms[${index}],
        ${profile.profile}))`), true, `Native parity: room ${index}, ${profile.label}`);
      verified++;
    }
  }
  assert.equal(actual.run('nativeGoldenStatus.size'), 147);
  const kassandora=data.rooms.findIndex(r=>r.group===3&&r.map===4);
  assert.ok(kassandora>=0);
  actual.run(`$('#room').value='${kassandora}'; $('#room').onchange(); actor.show=false;
    let partialCell=null;
    for(let cy=0;cy<L.cellsH&&!partialCell;cy++)for(let cx=0;cx<Math.min(L.cellsW,64)&&!partialCell;cx++) {
      const cell=cy*L.cellsW+cx;
      let transparent=0,opaque=0,first=null;
      for(let y=1;y<16;y++)for(let x=0;x<16;x++) {
        if(pixelOriginal(L,cell,x,y)===null){transparent++;first??={cx,cy,x,y};}
        else opaque++;
      }
      if(transparent>1&&opaque>0)partialCell=first;
    }
  `);
  assert.ok(actual.run('partialCell'), 'Kassandora 3:4 must have a partially transparent tile');
  actual.run(`setNativeCamera('x',Math.max(0,Math.min(partialCell.cx*16-32,L.w-256)));
    setNativeCamera('y',Math.max(0,Math.min(partialCell.cy*16-32,L.h-224)));
    const originalNative=nativeFrameCanvas().getContext('2d').image.data.slice();
    const originalBands=nativeBandSurfaces().images;
    selectCell(partialCell.cx,partialCell.cy); $('#pixelScope').value='cell';
    beginOp('partial black'); editPixel(partialCell.x,partialCell.y); commitOp();`);
  assert.deepEqual(Buffer.from(actual.run("nativeFrameCanvas().getContext('2d').image.data")),
    Buffer.from(actual.run('originalNative')));
  assert.equal(actual.run(`(()=>{
    const edited=nativeBandSurfaces().images;let changed=0;
    for(let bg=0;bg<2;bg++)for(let band=0;band<3;band++)
      for(let offset=0;offset<edited[bg][band].data.length;offset+=4) {
        const before=originalBands[bg][band].data,after=edited[bg][band].data;
        if(before[offset+3]!==after[offset+3]) {
          changed++;if(after[offset]||after[offset+1]||after[offset+2]||after[offset+3]!==255)
            throw new Error('Pixel must become opaque black');
        }
      }
    return changed;
  })()`),1);
  console.log('Kassandora 3:4: one transparent pixel becomes black, remaining art and native frame preserved');
  actual.run(`selectRectangle(partialCell.cx,partialCell.cy,
    Math.min(L.cellsW-1,partialCell.cx+1),Math.min(L.cellsH-1,partialCell.cy+1));
    const beforeRangeFill=[...selectedCells].map(cell=>({cell,
      mask:pixelMaskAt(room,0,L,cell)}));`);
  assert.equal(actual.run('fillSelectedTransparency()'),true);
  assert.equal(actual.run(`beforeRangeFill.every(({cell,mask})=>{
    const filled=pixelMaskAt(room,0,L,cell);
    for(let y=0;y<16;y++)for(let x=0;x<16;x++) {
      const expected=pixelOriginal(L,cell,x,y)===null||pixelIsBlack(mask,x,y);
      if(pixelIsBlack(filled,x,y)!==expected)return false;
    }
    return true;
  })`),true);
  assert.deepEqual(Buffer.from(actual.run("nativeFrameCanvas().getContext('2d').image.data")),
    Buffer.from(actual.run('originalNative')));
  console.log('Kassandora 3:4: range transparency fill preserves opaque artwork and native frame');
  console.log(`${verified} exported room/terrain C/JavaScript golden frames passed`);
}
