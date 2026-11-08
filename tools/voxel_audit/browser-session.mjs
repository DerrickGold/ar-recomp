/* Isolated, bounded Chrome/CDP harness. SwiftShader only; never attaches to a
 * personal browser. No npm dependencies and no native game launch. */
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {spawn} from 'node:child_process';
import assert from 'node:assert/strict';

const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
export async function openEditor({html,out,width=1400,height=1000}) {
  html=path.resolve(html);out=path.resolve(out);await fs.access(html);await fs.mkdir(out,{recursive:true});
  const profile=await fs.mkdtemp(path.join(os.tmpdir(),'actraiser-editor-ui-cdp-'));
  let chrome,socket,session,closed=false,next=0,stderr='';const pending=new Map(),errors=[];
  const timer=setTimeout(()=>{
    for(const [,p]of pending){clearTimeout(p.timer);p.reject(Error('Bounded UI test deadline expired'));}pending.clear();
    try{if(chrome?.pid)process.kill(-chrome.pid,'SIGKILL');}catch{}
  },300000);
  function call(method,params={},sid=session) {
    return new Promise((resolve,reject)=>{
      const id=++next,timeout=setTimeout(()=>{pending.delete(id);reject(Error('CDP timeout: '+method));},20000);
      pending.set(id,{resolve,reject,timer:timeout});
      socket.send(JSON.stringify({id,method,params,...(sid?{sessionId:sid}:{})}));
    });
  }
  async function evaluate(expression) {
    const result=await call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});
    if(result.exceptionDetails)throw Error(result.exceptionDetails.exception?.description||result.exceptionDetails.text);
    return result.result.value;
  }
  async function close() {
    clearTimeout(timer);
    if(socket?.readyState===WebSocket.OPEN){try{await Promise.race([call('Browser.close',{},null),sleep(1000)]);}catch{}socket.close();}
    if(chrome?.pid&&!closed){try{process.kill(-chrome.pid,'SIGTERM');}catch{}for(let i=0;i<20&&!closed;i++)await sleep(100);if(!closed)try{process.kill(-chrome.pid,'SIGKILL');}catch{}}
    for(const [,p]of pending)clearTimeout(p.timer);pending.clear();chrome?.stderr?.destroy();
    await fs.rm(profile,{recursive:true,force:true});await fs.writeFile(path.join(out,'chrome-stderr.log'),stderr);
  }
  try {
    chrome=spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',[
      '--headless=new','--remote-debugging-port=0','--user-data-dir='+profile,
      '--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--disable-gpu-compositing',
      '--disable-background-networking','--disable-component-update','--disable-sync','--disable-extensions',
      '--disable-background-timer-throttling','--disable-renderer-backgrounding','--disable-backgrounding-occluded-windows',
      '--no-first-run','--no-default-browser-check',`--window-size=${width},${height}`,'about:blank'],
      {detached:true,stdio:['ignore','ignore','pipe']});
    assert(os.getPriority(chrome.pid)===15,'Owned Chrome inherits actual nice15');
    chrome.stderr.on('data',chunk=>{stderr=(stderr+chunk).slice(-32000);});chrome.on('exit',()=>{closed=true;});
    let active;
    for(let i=0;i<150;i++){try{active=(await fs.readFile(path.join(profile,'DevToolsActivePort'),'utf8')).trim().split('\n');break;}catch{if(closed)throw Error('Owned Chrome exited: '+stderr);await sleep(100);}}
    assert(active,'Owned Chrome starts CDP');socket=new WebSocket(`ws://127.0.0.1:${active[0]}${active[1]}`);
    await new Promise((resolve,reject)=>{const t=setTimeout(()=>reject(Error('CDP connection timeout')),10000);
      socket.onopen=()=>{clearTimeout(t);resolve();};socket.onerror=()=>{clearTimeout(t);reject(Error('CDP connection failed'));};socket.onclose=()=>{clearTimeout(t);reject(Error('CDP connection closed'));};});
    socket.onclose=()=>{for(const [id,p]of pending){clearTimeout(p.timer);p.reject(Error('Owned browser closed'));pending.delete(id);}};
    socket.onmessage=event=>{const msg=JSON.parse(event.data);if(msg.id&&pending.has(msg.id)){const p=pending.get(msg.id);pending.delete(msg.id);clearTimeout(p.timer);msg.error?p.reject(Error(JSON.stringify(msg.error))):p.resolve(msg.result);}else if(msg.method==='Runtime.exceptionThrown')errors.push(msg.params.exceptionDetails.exception?.description||msg.params.exceptionDetails.text);};
    const target=await call('Target.createTarget',{url:'about:blank'},null);
    session=(await call('Target.attachToTarget',{targetId:target.targetId,flatten:true},null)).sessionId;
    await call('Page.enable');await call('Runtime.enable');await call('Page.bringToFront');
    await call('Page.addScriptToEvaluateOnNewDocument',{source:`{const original=WebAssembly.instantiate;WebAssembly.instantiate=async function(...args){const result=await original.apply(this,args);const api=(result.instance||result).exports;if(api.RoomPreview_VoxelAuditRead)window.__voxelAuditApi=api;return result;}}`});
    await call('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:false});
    const renderer=await evaluate(`(()=>{const c=document.createElement('canvas'),g=c.getContext('webgl2');if(!g)return '';const e=g.getExtension('WEBGL_debug_renderer_info'),name=e?g.getParameter(e.UNMASKED_RENDERER_WEBGL):g.getParameter(g.RENDERER);g.getExtension('WEBGL_lose_context')?.loseContext();return name;})()`);
    assert(/SwiftShader/i.test(renderer),'Software renderer required');
    await call('Network.enable');await call('Network.setBlockedURLs',{urls:['http://*','https://*']});
    await call('Page.navigate',{url:pathToFileURL(html).href});
    const until=async(expression,label='condition')=>{for(let i=0;i<200;i++){if(await evaluate(expression))return;await sleep(100);}throw Error('Timed out waiting for '+label);};
    const settle=async()=>evaluate('new Promise(resolve=>{const t=setTimeout(resolve,500);requestAnimationFrame(()=>requestAnimationFrame(()=>{clearTimeout(t);resolve()}))})');
    const click=async selector=>{
      const box=await evaluate(`(()=>{const e=document.querySelector(${JSON.stringify(selector)});if(!e)throw Error('Missing control '+${JSON.stringify(selector)});e.scrollIntoView({block:'nearest',inline:'nearest'});const r=e.getBoundingClientRect(),x=r.x+r.width/2,y=r.y+r.height/2;const hit=document.elementFromPoint(x,y);return {x,y,visible:r.width>0&&r.height>0,enabled:!e.matches(':disabled'),reachable:hit===e||e.contains(hit),hit:hit?.id};})()`);
      assert(box.visible&&box.enabled&&box.reachable,'Visible reachable control '+selector+' '+JSON.stringify(box));
      await call('Input.dispatchMouseEvent',{type:'mousePressed',x:box.x,y:box.y,button:'left',clickCount:1});await call('Input.dispatchMouseEvent',{type:'mouseReleased',x:box.x,y:box.y,button:'left',clickCount:1});await settle();
    };
    const input=async(selector,value,event='input')=>evaluate(`(()=>{const e=document.querySelector(${JSON.stringify(selector)});if(!e||e.matches(':disabled'))throw Error('Unavailable input');e.scrollIntoView({block:'center',inline:'nearest'});const r=e.getBoundingClientRect(),hit=document.elementFromPoint(r.x+r.width/2,r.y+r.height/2);if(!r.width||!r.height||!(hit===e||e.contains(hit)))throw Error('Input is hidden or covered: '+${JSON.stringify(selector)});e.focus();e.value=${JSON.stringify(String(value))};e.dispatchEvent(new Event(${JSON.stringify(event)},{bubbles:true}));})()`);
    const screenshot=async name=>{const shot=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});const file=path.join(out,name+'.png');await fs.writeFile(file,Buffer.from(shot.data,'base64'));return file;};
    const resize=async(w,h)=>{await call('Emulation.setDeviceMetricsOverride',{width:w,height:h,deviceScaleFactor:1,mobile:false});await settle();};
    return {call,evaluate,until,settle,click,input,screenshot,resize,close,errors,renderer,out};
  }catch(error){await close();throw error;}
}
