import os from 'node:os'; if(os.getPriority()!==15)throw Error('Actual nice15 required'); console.log('ACTUAL_NICE '+os.getPriority());
import {openEditor} from './browser-session.mjs';
import fs from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
export async function session(name,body){
 const html=path.resolve(process.argv[2]||'build/action-editor/ar-action-layer-editor.html');
 const out=path.resolve(process.argv[3]||'runs/voxel-audit/'+name);
 const report={html,actual_nice:os.getPriority(),checks:[],screenshots:[]};let b;
 const check=(label,ok)=>{assert(ok,label);report.checks.push(label);console.log('PASS '+label)};
 try{
  report.html_sha256=crypto.createHash('sha256').update(await fs.readFile(html)).digest('hex');
  b=await openEditor({html,out,width:1100,height:800});report.renderer=b.renderer;
  await b.until("typeof VoxelEditor!=='undefined'&&!document.querySelector('#modeShared').disabled",'editor ready');
  await b.evaluate("window.__qaBackends=new Set();const old=ActionWebGL2Backend.prototype.flushDepth;ActionWebGL2Backend.prototype.flushDepth=function(...a){window.__qaBackends.add(this);return old.apply(this,a)}");
  async function reveal(s){for(let i=0;i<9;i++){const selector=await b.evaluate(`(()=>{let e=document.querySelector(${JSON.stringify(s)}),p=e?.parentElement,found;while(p){if(p.tagName==='DETAILS'&&!p.open&&!(e.tagName==='SUMMARY'&&e.parentElement===p))found=p;p=p.parentElement}if(!found)return null;found.dataset.testDisclosure='open';return '[data-test-disclosure="open"] > summary'})()`);if(!selector)return;await b.click(selector);await b.evaluate("delete document.querySelector('[data-test-disclosure]')?.dataset.testDisclosure")}throw Error('Cannot reveal '+s)}
  const click=async s=>{await reveal(s);await b.click(s)};
  const input=async(s,v,event='input')=>{await reveal(s);await b.input(s,v,event);await b.settle()};
  const key=async(key,code,modifiers=0)=>{const params={key,code,modifiers,windowsVirtualKeyCode:key.length===1?key.toUpperCase().charCodeAt(0):key==='Escape'?27:0};await b.call('Input.dispatchKeyEvent',{type:'keyDown',...params});await b.call('Input.dispatchKeyEvent',{type:'keyUp',...params});await b.settle()};
  const pointClick=async(p,button='left',modifiers=0)=>{await b.call('Input.dispatchMouseEvent',{type:'mousePressed',...p,button,buttons:button==='right'?2:button==='middle'?4:1,modifiers,clickCount:1});await b.call('Input.dispatchMouseEvent',{type:'mouseReleased',...p,button,buttons:0,modifiers,clickCount:1});await b.settle()};
  const shot=async name=>{report.screenshots.push(await b.screenshot(name));const png=await b.evaluate("document.querySelector('#sharedGl').toDataURL()");await fs.writeFile(path.join(out,name+'-canvas.png'),Buffer.from(png.split(',')[1],'base64'))};
  const project=()=>b.evaluate('JSON.stringify(ProjectEditor.files())');
  const tile=(x=126,y=17)=>b.evaluate(`SharedRoomPreview.voxelTile(${x},${y},-1)`);
  const select=async(cells=[[126,17]])=>{await b.evaluate(`VoxelEditor.selectCells(${JSON.stringify(cells)},{focus:false});draw()`);await b.settle()};
  async function scene(){console.log('SETUP room');await input('#room',await b.evaluate('DATA.rooms.findIndex(r=>r.group===1&&r.map===1)'),'change');console.log('SETUP shared');await click('#modeShared');console.log('SETUP voxel mode');await input('#voxelMode',2,'change');console.log('SETUP edit');await click('#voxelEdit');console.log('SETUP camera');await b.evaluate("SharedRoomPreview.focusVoxelSelection({x:2016,y:272,width:64,height:64});SharedRoomPreview.setCameraPreset('left');document.querySelector('#sharedPitch').value='.2';draw()");await b.settle();console.log('SETUP select');await select();console.log('SETUP complete')}
  async function findFace(x,y,face='front'){
   return b.evaluate(`(()=>{const c=document.querySelector('#sharedGl'),r=c.getBoundingClientRect(),scale=Math.min(r.width/c.width,r.height/c.height),index={front:0,top:1,side:2,back:3,left:5,right:6,bottom:7}[${JSON.stringify(face)}],q=SharedRoomPreview.voxelTile(${x},${y},index).quad;if(!q?.length)return null;const xx=q.map(p=>p[0]),yy=q.map(p=>p[1]),minX=Math.max(0,Math.floor(Math.min(...xx))),maxX=Math.min(c.width,Math.ceil(Math.max(...xx))),minY=Math.max(0,Math.floor(Math.min(...yy))),maxY=Math.min(c.height,Math.ceil(Math.max(...yy)));for(let py=minY;py<maxY;py++)for(let px=minX;px<maxX;px++){const hit=SharedRoomPreview.voxelPick(px+.5,py+.5,1);if(hit?.x!==${x}||hit?.y!==${y}||hit?.face!==${JSON.stringify(face)})continue;const p={x:r.x+(r.width-c.width*scale)/2+(px+.5)*scale,y:r.y+(r.height-c.height*scale)/2+(py+.5)*scale};if(document.elementFromPoint(p.x,p.y)===c)return {...p,canvasX:px+.5,canvasY:py+.5,hit}}return null})()`)
  }
  await body({b,out,report,check,reveal,click,input,key,pointClick,shot,project,tile,select,scene,findFace});
  report.backends=await b.evaluate('[...window.__qaBackends].map(b=>({warnings:b.warnings,error:b.gl.getError()}))');
  check('No backend warnings or GL errors',report.backends.length>0&&report.backends.every(v=>!v.warnings.length&&v.error===0));
  check('No JavaScript errors',b.errors.length===0);report.passed=true;
 }catch(error){report.error=error.stack;report.passed=false;process.exitCode=1;console.error(error.stack);try{if(b)report.screenshots.push(await b.screenshot('failure'))}catch{}}
 finally{report.errors=b?.errors;await fs.mkdir(out,{recursive:true});await fs.writeFile(path.join(out,'result.json'),JSON.stringify(report,null,2));if(b)await b.close()}
}
