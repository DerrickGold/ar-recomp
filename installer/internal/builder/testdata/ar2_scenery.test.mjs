import {test} from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";
import {MASTER_ACTIONS,sampleMasterPatrol,sampleMasterGlide} from "../web/ar2-scenes.mjs";
import {loadSceneAtlas,samplePose} from "../web/scene-assets.mjs";
import {createEncounterState,sampleEncounter} from "../web/encounters.mjs";

test("decorative paths stay bounded and continuous across pause and loop boundaries",()=>{
  for(const [sample,period] of [[sampleMasterPatrol,20000],[sampleMasterGlide,24000]]) {
    let previous=sample(0);
    for(let t=0;t<period*2;t+=20) {
      const pose=sample(t);
      assert(MASTER_ACTIONS.includes(pose.action));
      assert(pose.x>=128&&pose.x<=512&&pose.y>=192&&pose.y<=386);
      assert(Math.abs(pose.x-previous.x)<3&&Math.abs(pose.y-previous.y)<9,"no jump on loop or turn");
      assert.deepEqual(sample(t+period),pose);
      previous=pose;
    }
    const paused=sample(7654);sample(123456);assert.deepEqual(sample(7654),paused);
  }
});

test("atlas loader rejects external URLs and broken animation references",async()=>{
  globalThis.location=new URL("http://localhost/session/");
  globalThis.Image=class {width=1024;height=128;async decode(){}};
  globalThis.document={createElement:()=>({getContext:()=>({drawImage(){}})})};
  const profile={version:1,width:1024,animations:1,scale:()=>2};
  const data={atlasURL:"scene-atlas.png?content=ar2-master&v=example",catalog:{version:1,
    frames:[{id:"frame",x:0,y:0,width:128,height:128}],animations:[{id:"idle",poses:[{frame:"frame",ticks:4}]}]}};
  const art=await loadSceneAtlas(data,profile);
  assert.equal(samplePose(art,"idle",1000),art.sprites.get("frame"));
  await assert.rejects(loadSceneAtlas({...data,atlasURL:"https://example.com/scene-atlas.png"},profile));
  const invalid=structuredClone(data);invalid.catalog.animations[0].poses[0].frame="missing";
  await assert.rejects(loadSceneAtlas(invalid,profile));
});

test("import unlocks scenery live, preserves choice and obeys reduced motion",async()=>{
  const events=new Map(),windowEvents=new Map(),store=new Map();
  const context=new Proxy({createRadialGradient:()=>({addColorStop(){}})},{get:(o,k)=>o[k]||(()=>{})});
  class Element {
    value="";hidden=false;dataset={};options=[];listeners=new Map();
    addEventListener(name,fn){this.listeners.set(name,fn);}
    getContext(){return context;}
    setAttribute(name,value){this[name]=value;}
    querySelector(selector){return this.options.find(o=>selector.includes('"'+o.value+'"'));}
    append(option){this.options.push(option);option.remove=()=>{this.options=this.options.filter(o=>o!==option);};}
    emit(name){this.listeners.get(name)?.();}
  }
  const nodes=Object.fromEntries(["palace-scene","ambient-scene","scene-motion","scene-setting","panel-home","scene-caption","workshop-atmosphere"].map(id=>[id,new Element()]));
  nodes["palace-scene"].parentElement=new Element();nodes["scene-motion"].value="auto";
  const selector=nodes["scene-setting"];selector.value="fillmore";selector.options=[{value:"fillmore"},{value:"palace"}];
  const reduced={matches:true,addEventListener(name,fn){this.change=fn;}};
  let response={available:false},requests=0,frames=0;
  const document={hidden:false,body:new Element(),querySelector:id=>nodes[id.slice(1)],createElement:()=>new Element(),addEventListener:(n,fn)=>events.set(n,fn)};
  const art={sprites:new Map(),clips:new Map()};
  for(const facing of ["left","right"]) for(const action of MASTER_ACTIONS) {
    const id="ar2-master."+facing+"."+action;
    art.sprites.set(id,new Element());art.clips.set(id,{ticks:4,poses:[{frame:id,ticks:4}]});
  }
  const sandbox={document,createEncounterState,sampleEncounter,MASTER_ACTIONS,sampleMasterPatrol,sampleMasterGlide,samplePose,
    loadSceneAtlas:async()=>art,matchMedia:()=>reduced,AbortController,
    performance:{now:()=>2000},sessionStorage:{getItem:k=>store.get(k),setItem:(k,v)=>store.set(k,v)},
    fetch:async()=>{requests++;return {ok:true,json:async()=>response};},
    requestAnimationFrame:()=>++frames,cancelAnimationFrame(){},
    window:{addEventListener:(n,fn)=>windowEvents.set(n,fn),workshopI18n:{apply(){},set:(el,key)=>{el.key=key;}}}};
  const source=readFileSync(new URL("../web/scene.js",import.meta.url),"utf8").replace(/^import .*;\n/gm,"");
  runInNewContext(source,sandbox);
  const settle=()=>new Promise(resolve=>setImmediate(resolve));
  await settle();assert.equal(selector.options.length,2);
  response={available:false,imports:{"ar2-master":{catalog:{imageSHA256:"abc"}}}};
  events.get("workshop:content-imported")();await settle();
  assert.equal(selector.value,"fillmore","do not change the player's scenery on import");
  assert(selector.options.some(o=>o.value==="ar2-clouds"));
  assert(!selector.options.some(o=>o.value==="ar2-fillmore"),"Fillmore needs its own extracted background");
  selector.value="ar2-clouds";selector.emit("change");
  assert.equal(nodes["palace-scene"].dataset.art,"ar2-master");
  assert.equal(nodes["palace-scene"].dataset.running,"false");assert.equal(frames,0);
  reduced.matches=false;reduced.change();assert.equal(nodes["palace-scene"].dataset.running,"true");
  const before=requests;nodes["scene-motion"].value="off";nodes["scene-motion"].emit("change");
  events.get("workshop:content-imported")();await settle();assert.equal(requests,before);
  response={available:false};nodes["scene-motion"].value="still";nodes["scene-motion"].emit("change");await settle();
  assert.equal(selector.value,"fillmore");assert.equal(selector.options.length,2);
  assert.equal(nodes["palace-scene"].dataset.running,"false");
  events.get("workshop:closed")();await settle();
  assert.equal(nodes["palace-scene"].dataset.running,"false");
});
