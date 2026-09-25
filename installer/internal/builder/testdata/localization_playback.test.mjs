import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";
import {messages} from "./interface_dom.mjs";

class Element {
  constructor(tag) {this.tagName=tag;this.children=[];this.listeners={};this.dataset={};this.style={};this.value="";this.textContent="";this.draws=[];}
  append(...children) {this.children.push(...children);for(const child of children)if(typeof child!=="string")child.parent=this;}
  after(node) {this.parent.children.splice(this.parent.children.indexOf(this)+1,0,node);node.parent=this.parent;}
  remove() {if(this.parent)this.parent.children.splice(this.parent.children.indexOf(this),1);}
  replaceChildren(...children) {this.children=[...children];}
  setAttribute(name,value) {this[name]=value;}
  addEventListener(name,action) {(this.listeners[name]??=[]).push(action);}
  fire(name) {for(const action of this.listeners[name]||[]) action();}
  querySelectorAll(tag) {const tags=tag.split(",").map(t=>t.trim());return this.children.filter(x=>typeof x!=="string").flatMap(x=>[...(tags.includes(x.tagName)?[x]:[]),...x.querySelectorAll(tag)]);}
  getContext() {return {clearRect:()=>{},drawImage:(...args)=>this.draws.push(args)};}
}
const frames=[
  {tick:0,sheet:0,index:0,kind:"start"},
  {tick:3,sheet:0,index:1,kind:"reveal"},
  {tick:6,sheet:0,index:2,kind:"control",control:"yield.00"},
  {tick:9,sheet:0,index:3,kind:"end"}
];
function movie(id) {return {width:100,height:30,page:0,pages:2,nativeBounds:true,messageID:id,resolvedID:id,source:"text/example.artext",line:4,layout:"flow",fonts:[{reference:"fallback.otf",pixels:25,missing:false}],frames,sheets:["data:image/png;base64,test"]};}
function response(id="message.one") {const scenario={page:0,delay:3,size:140,sampling:0,pixelation:2,pixelSize:2,values:{master_name:"Player"},inks:{}};return {source:movie(id),draft:movie(id),scenario,sourceScenario:structuredClone(scenario),sourceLanguage:"en",draftLanguage:"fr"};}
function setup() {
  const host=new Element("div"),errors=[],requests=[],animations=new Map();let next=1,now=0;
  let answer=async()=>({ok:true,json:async()=>response()});
  const ui={set(node,key,args={}) {node.textContent=this.text(key,args);}, unbind(){}, attribute(node,name,key){node.setAttribute(name,this.text(key));}, text(key,args={}) {let value=messages[key]?.[0]??key;for(const [name,text] of Object.entries(args)) value=value.replaceAll("{"+name+"}",String(text));return value;}};
  class Image {set src(value) {this.url=value;queueMicrotask(()=>this.onload());}}
  const context={window:{workshopI18n:ui},navigator:{userAgent:"Playback test browser"},document:{querySelector:()=>null,createElement:tag=>new Element(tag),createTextNode:text=>String(text),addEventListener:()=>{}},URL,location:{href:"http://localhost:1/secret/"},Image,AbortController,structuredClone,
    performance:{now:()=>now},requestAnimationFrame:callback=>{const id=next++;animations.set(id,callback);return id;},cancelAnimationFrame:id=>animations.delete(id),
    fetch:async(url,options)=>{requests.push({url:String(url),...options});return answer(url,options);}};
  runInNewContext(readFileSync(new URL("../../workshopui/feedback.js",import.meta.url),"utf8"),context);
  runInNewContext(readFileSync(new URL("../localization_playback.js",import.meta.url),"utf8"),context);
  const player=context.window.workshopPlayback.create(host,()=>({projectID:"pack",revision:"r1",id:"message.one",body:"Unsaved <i>words</i>",status:"wip"}),error=>errors.push(error));
  return {host,player,requests,errors,respond(fn){answer=fn;},tick(time){now=time;const callbacks=[...animations.values()];animations.clear();callbacks.forEach(fn=>fn(now));},button(text,index=0){return host.querySelectorAll("button").filter(b=>b.textContent===text)[index];}};
}

test("native image playback keeps linked clocks, pauses on controls and exposes template/font trace",async()=>{
 const s=setup();await s.player.show();
 assert.equal(JSON.parse(s.requests[0].body).body,"Unsaved <i>words</i>");
 const canvases=s.host.querySelectorAll("canvas");assert.equal(canvases.length,2);
 for(let i=0;i<3;i++) s.button("Step 1 tick").fire("click");
 assert.equal(canvases[0].draws.at(-1)[2],30);assert.equal(canvases[1].draws.at(-1)[2],30);
 s.button("Play / continue").fire("click");s.tick(500);
 assert.equal(canvases[0].draws.at(-1)[2],60);assert.equal(canvases[1].draws.at(-1)[2],60);
 assert.equal(s.host.querySelectorAll("button").filter(b=>b.textContent==="Pause").length,0);
 s.button("Confirm / acknowledge").fire("click");s.button("Play / continue").fire("click");s.tick(1000);
 assert.equal(canvases[0].draws.at(-1)[2],90);
 assert.ok(s.host.querySelectorAll("p").some(p=>p.textContent.includes("text/example.artext:4")));
 assert.ok(s.host.querySelectorAll("li").some(p=>p.textContent.includes("fallback.otf · 25px")));
 const link=s.host.querySelectorAll("input").find(input=>input.type==="checkbox");link.checked=false;
 s.button("Step 1 tick").fire("click");assert.equal(s.errors.length,0);
});

test("invalidated and out-of-order requests cannot replace the latest preview",async()=>{
 const s=setup();let complete;
 s.respond(async()=>({ok:true,json:()=>new Promise(resolve=>{complete=resolve;})}));
 const first=s.player.show();await new Promise(resolve=>setImmediate(resolve));
 s.player.invalidate();assert.equal(s.requests[0].signal.aborted,true);
 s.respond(async()=>({ok:true,json:async()=>response("new.message")}));
 await s.player.show();complete(response("old.message"));await first;
 assert.ok(s.host.querySelectorAll("p").some(p=>p.textContent.includes("new.message")));
 assert.ok(!s.host.querySelectorAll("p").some(p=>p.textContent.includes("old.message")));
 assert.equal(s.button("Render playback").disabled,false);
});

test("render errors leave the draft untouched and offer a retry",async()=>{
 const s=setup();s.respond(async()=>({ok:false,status:400,json:async()=>({error:"text/source.artext:8: unknown role"})}));
 await s.player.show();assert.match(s.errors[0],/unknown role/);assert.equal(s.button("Render playback").disabled,false);
 assert.equal(JSON.parse(s.requests[0].body).body,"Unsaved <i>words</i>");
});

test("linked stepping uses ticks despite different reveal spacing; waits cannot be skipped", async () => {
 const s=setup();
 const answer=response();
 answer.draft.frames=[{tick:0,sheet:0,index:0,kind:"start"},{tick:1,sheet:0,index:1,kind:"reveal"},{tick:2,sheet:0,index:2,kind:"wait"},{tick:12,sheet:0,index:3,kind:"end"}];
 s.respond(async()=>({ok:true,json:async()=>answer}));
 await s.player.show();
 const canvases=s.host.querySelectorAll("canvas");
 s.button("Step 1 tick").fire("click");
 assert.equal(canvases[0].draws.at(-1)[2],0);
 assert.equal(canvases[1].draws.at(-1)[2],30);
 s.button("Reveal to next wait").fire("click");
 assert.equal(canvases[0].draws.at(-1)[2],60);
 assert.equal(canvases[1].draws.at(-1)[2],60);
 s.button("Reveal to next wait").fire("click");
 assert.equal(canvases[1].draws.at(-1)[2],60);
 s.player.invalidate();
 assert.ok(s.host.querySelectorAll("canvas").length===2);
 assert.equal(s.button("Step 1 tick").disabled,true);
});

test("validating an edited message starts at page zero and retains scenario settings", async () => {
 const s=setup();
 const initial=response();
 initial.source.page=initial.draft.page=1;
 initial.scenario.page=initial.sourceScenario.page=1;
 s.respond(async()=>({ok:true,json:async()=>initial}));
 await s.player.show();
 s.button("Reveal to next wait").fire("click");
 s.respond(async()=>({ok:true,json:async()=>response()}));
 await s.player.show();
 const request=JSON.parse(s.requests.at(-1).body);
 assert.equal(request.scenario.page,0);
 assert.equal(request.sourceScenario.page,0);
 assert.equal(request.scenario.size,140);
 for(const canvas of s.host.querySelectorAll("canvas")) assert.equal(canvas.draws.at(-1)[2],0);
});


test("playback reports response metadata and clears it after a successful retry",async()=>{
 const s=setup();
 s.respond(async()=>({ok:false,status:504,json:async()=>({error:"worker deadline",errorCode:"builder.errors.timeout",recoveryKey:"builder.recovery.timeout"})}));
 await s.player.show();
 const report=s.host.querySelectorAll("textarea").find(node=>node.readOnly);
 assert.match(report.value,/Operation: Render language playback/);
 assert.match(report.value,/Code: builder.errors.timeout/);
 assert.match(report.value,/HTTP status: 504/);
 assert.match(report.value,/Details: worker deadline/);
 s.respond(async()=>({ok:true,json:async()=>response()}));
 await s.player.show();
 assert.equal(s.host.querySelectorAll("textarea").filter(node=>node.readOnly).length,0);
});
