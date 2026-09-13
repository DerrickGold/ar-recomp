import {test} from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";
const html=readFileSync(new URL("output.html",import.meta.url),"utf8");
const script=readFileSync(new URL("output.js",import.meta.url),"utf8");
class Node {
  constructor(tag){this.tag=tag;this.listeners={};this.value="";this.textContent="";this.hidden=false;this.disabled=false;this.checked=false;}
  addEventListener(name,fn){this.listeners[name]=fn;}
  async fire(name){await this.listeners[name]?.({preventDefault(){}});}
  replaceChildren(...nodes){this.children=nodes;}
}
async function setup(current=""){
  const nodes=Object.fromEntries([...html.matchAll(/<(\w+)[^>]*\bid="([^"]+)"/g)].map(([,tag,id])=>[id,new Node(tag)]));
  const requests=[],navigation=[];let response=(endpoint)=>({candidate:"/portable/Game",current});
  const document={getElementById:id=>nodes[id],createElement:tag=>new Node(tag),querySelectorAll:()=>Object.values(nodes).filter(n=>["button","input"].includes(n.tag))};
  runInNewContext(script,{window:{},document,location:{replace:path=>navigation.push(path)},fetch:async(endpoint,options)=>{
    const body=options?.body?JSON.parse(options.body):undefined;requests.push({endpoint,body});
    const value=await response(endpoint,body);return {ok:!value.error,status:value.error?400:200,json:async()=>value};
  }});
  await new Promise(resolve=>setImmediate(resolve));
  return {nodes,requests,navigation,respond(fn){response=fn;}};
}
test("first launch reviews a destination without accepting it or losing literal paths",async()=>{
  const s=await setup(),n=s.nodes;
  assert.equal(n.directory.value,"/portable/Game");assert.equal(n.apply.disabled,true);
  s.respond(()=>({directory:"/portable/Game <my files>",revision:"first",existing:false,game:false,entries:[]}));
  await n["folder-form"].fire("submit");
  assert.equal(n["review-path"].textContent,"/portable/Game <my files>");assert.equal(n.apply.disabled,false);
  assert.match(n["review-description"].textContent,/start a new portable game installation/);
  assert.deepEqual(s.requests.map(r=>r.endpoint),["state","review"]);
  s.respond((endpoint,body)=>{assert.equal(endpoint,"apply");assert.equal(body.revision,"first");return {restartRequired:false};});
  await n.apply.fire("click");assert.deepEqual(s.navigation,["../../"]);
});
test("non-empty destinations require confirmation, invalidated by edits",async()=>{
  const s=await setup(),n=s.nodes;
  s.respond(()=>({directory:"/old/Game",revision:"review",existing:true,game:true,entries:["saves","<not HTML>"]}));
  await n["folder-form"].fire("submit");
  assert.equal(n["review-title"].textContent,"Existing game folder");assert.equal(n.apply.disabled,true);
  assert.equal(n.apply.textContent,"Open existing installation");
  assert.match(n["review-description"].textContent,/use its settings, assets, ROM and saves directly; no import is needed/);
  n.confirm.checked=true;await n.confirm.fire("change");assert.equal(n.apply.disabled,false);
  assert.equal(n.entries.children[1].textContent,"<not HTML>");
  n.directory.value="/different";await n.directory.fire("input");
  assert.equal(n.apply.disabled,true);assert.equal(n.confirm.checked,false);assert.equal(n.review.hidden,true);
});
test("changing output during a session only saves for next launch",async()=>{
  const s=await setup("/current/Game"),n=s.nodes;
  assert.match(n.current.textContent,/\/current\/Game/);
  s.respond(()=>({directory:"/next/Game",revision:"next",existing:false,entries:[]}));await n["folder-form"].fire("submit");
  assert.equal(n.apply.textContent,"Save for next launch");
  s.respond(()=>({restartRequired:true,directory:"/next/Game"}));await n.apply.fire("click");
  assert.deepEqual(s.navigation,[]);assert.match(n.status.textContent,/close and reopen/);assert.equal(n.directory.disabled,true);
});
test("cancelled picker and failed review leave an editable retry path",async()=>{
  const s=await setup(),n=s.nodes;
  s.respond(()=>({directory:""}));await n.browse.fire("click");assert.equal(n.directory.value,"/portable/Game");
  s.respond(()=>({error:"read-only destination"}));await n["folder-form"].fire("submit");
  assert.equal(n.apply.disabled,true);assert.equal(n.directory.disabled,false);assert.equal(n.status.textContent,"read-only destination");
});
