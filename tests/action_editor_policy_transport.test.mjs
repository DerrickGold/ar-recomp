/* Policy transport integration with real C/WASM and stubbed graphics services. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const [htmlFile,wasmFile]=process.argv.slice(2);
const html=fs.readFileSync(htmlFile,'utf8');
const data=JSON.parse(html.match(/window\.__ACTION_BG__=(.*?);<\/script>/s)[1]);
const blobs=data.blobs.map(s=>Uint8Array.from(Buffer.from(s,'base64')));
let allocations=0,draws=0;
const imports={create:()=>++allocations,destroy:()=>{allocations--;},update:()=>1,
  target:()=>1,viewport:()=>1,clip:()=>1,clear:()=>1,
  geometry:()=>{draws++;return 1;},effect:()=>1,available:()=>1};
// Exercise the actual JS/WASM policy transport as well as the native entrypoints.
// Only UI/graphics services are stubbed; loading, parsing and planning remain C.
{
  const elements=new Map();
  function element(id) {
    if(!elements.has(id))elements.set(id,{value:'0',textContent:'',listeners:{},
      addEventListener(name,callback){this.listeners[name]=callback;},
      replaceChildren(){},append(){}});
    return elements.get(id);
  }
  let readyResolve;
  const ready=new Promise(resolve=>{readyResolve=resolve;});
  const selected=data.rooms.find(r=>r.group===7&&r.map===3);
  const context=vm.createContext({
    window:{__ROOM_PREVIEW_WASM__:fs.readFileSync(wasmFile).toString('base64')},
    document:{querySelector:element,createElement:()=>({}),addEventListener(){}},$:element,
    ActionWebGL2Backend:class {
      constructor(){this.imports=imports;this.gl={MAX_TEXTURE_SIZE:1,getParameter:()=>16384};}
      dispose(){}
    },
    WebAssembly,Uint8Array,Uint32Array,Int32Array,DataView,TextEncoder,TextDecoder,
    B64:text=>Uint8Array.from(Buffer.from(text,'base64')),
    DATA:data,BLOBS:blobs,room:{...selected,terrainProfile:0},sourceIniText:'',
    nativeCamera:{x:0,y:0},nativeFrame:37,
    currentIni:'[layers:07:03]\n',
    sceneKey:r=>`${r.group}:${r.map}:${r.section||''}:${r.terrainProfile??0}`,roomKey:r=>`${r.group}:${r.map}${r.section?':'+r.section:''}`,
    roomFromSection:header=>{const m=header.match(/^\[layers:([\da-f]{1,2}):([\da-f]{1,2})(?::(completion))?\]$/i);
      return m&&data.rooms.find(r=>r.group===parseInt(m[1],16)&&r.map===parseInt(m[2],16)&&(r.section||'')===(m[3]||''));},
    roomSectionHeader:r=>`[layers:${r.group.toString(16).padStart(2,'0')}:${r.map.toString(16).padStart(2,'0')}${r.section?':'+r.section:''}]`,
    terrainRoom:(r,profile)=>({...r,bg:[r.terrainVariants[profile].bg1,r.bg[1]],terrainProfile:profile}),
    terrainLabel:r=>['US','Japanese','European'][r.terrainProfile],
    stripIniComment:line=>line.split(/[;#]/)[0].trim(),
    draw:readyResolve,refreshEditorFeedback(){},
  });
  vm.runInContext('function roomSectionIni(){return currentIni;}',context);
  vm.runInContext(fs.readFileSync('tools/action_editor/shared_preview.js','utf8'),context);
  vm.runInContext(fs.readFileSync('tools/action_editor/bg_policy_editor.js','utf8'),context);
  vm.runInContext(fs.readFileSync('tools/action_editor/shared_room_preview.js','utf8'),context);
  let readyTimeout;
  try {
    await Promise.race([ready,new Promise((_,reject)=>{
      readyTimeout=setTimeout(()=>reject(Error(element('#sharedRoomStatus').textContent||'Shared adapter initialization timed out.')),10000);
    })]);
  } finally {clearTimeout(readyTimeout);}
  const run=code=>vm.runInContext(code,context);
  const read=()=>JSON.parse(run('JSON.stringify(SharedRoomPreview.policyPlan())'));
  const canonical=JSON.parse(run('JSON.stringify(SharedRoomPreview.policyPlan(true))'));
  assert.equal(read()[0].edge,'mirror');assert.equal(canonical[0].horizontal.left,80);
  const authored='[layers:07:03]\nbg1-policy = edge:world horizontal:available bands:0\n';
  run(`SharedRoomPreview.validateScenery(${JSON.stringify(authored)});currentIni=${JSON.stringify(authored)};SharedRoomPreview.invalidate();`);
  const before=read();assert.equal(before[0].edge,'world');assert.equal(before[0].horizontal.mode,'available');
  run(`SharedRoomPreview.validatePolicyDocument(${JSON.stringify(authored)})`);
  assert.deepEqual(read(),before,'all-terrain import validation restores the selected room and edits');
  const invalid='[layers:01:01]\nbg2-policy = bands:1\nbg2-policy-band = index:0 anchor:world rows:0,65535 edge:mirror motion:normal horizontal:inherit\n';
  assert.throws(()=>run(`SharedRoomPreview.validatePolicyDocument(${JSON.stringify(invalid)})`),/room 1:1/);
  assert.deepEqual(read(),before,'failure in another room retains the selected room configuration');
  const crossing='[layers:07:03]\nbg1-policy = bands:2\n'+
    'bg1-policy-band = index:0 anchor:screen rows:0,100 edge:mirror motion:normal horizontal:inherit\n'+
    'bg1-policy-band = index:1 anchor:world rows:100,224 edge:repeat motion:normal horizontal:inherit\n';
  assert.throws(()=>run(`SharedRoomPreview.validateScenery(${JSON.stringify(crossing)})`),/camera travel/);
  assert.deepEqual(read(),before);
  assert.deepEqual(JSON.parse(run('JSON.stringify(SharedRoomPreview.policyPlan(true))')),canonical);
  const both='[layers:07:01]\nbg2-policy = edge:repeat bands:0\n'+
    '[layers:07:01:completion]\nbg2-policy = edge:transparent bands:0\n';
  run(`room=terrainRoom(DATA.rooms.find(r=>r.group===7&&r.map===1&&r.section==='completion'),0);currentIni=${JSON.stringify(both)};SharedRoomPreview.invalidate();`);
  assert.equal(read()[1].edge,'transparent');
  run(`SharedRoomPreview.validatePolicyDocument(${JSON.stringify(both)})`);
  assert.equal(read()[1].edge,'transparent','all-terrain document validation restores scene B');
  assert.equal(JSON.parse(run('JSON.stringify(SharedRoomPreview.policyPlan(true))'))[1].edge,'mirror');
  run(`room=terrainRoom(DATA.rooms.find(r=>r.group===7&&r.map===1&&!r.section),0);SharedRoomPreview.invalidate();`);
  assert.equal(read()[1].edge,'repeat','scene A retains its own policy');
  console.log('Shared policy adapter: real WASM defaults, edits, all-terrain validation and atomic rejection passed.');
}
assert.equal(allocations,0);assert.equal(draws,0,'Policy transport never renders');
