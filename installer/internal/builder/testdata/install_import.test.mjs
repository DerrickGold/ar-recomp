import {test} from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";

const html=readFileSync(new URL("../web/index.html",import.meta.url),"utf8");
const script=readFileSync(new URL("../web/install-import.js",import.meta.url),"utf8");
class Node {
  constructor(tag,attributes="") {
    Object.assign(this,{tag,listeners:{},value:"",textContent:"",checked:false,open:false,
      hidden:/\bhidden\b/.test(attributes),disabled:/\bdisabled\b/.test(attributes)});
  }
  addEventListener(name,fn){this.listeners[name]=fn;}
  async fire(name){await this.listeners[name]?.({preventDefault(){}});}
  replaceChildren(...nodes){this.children=nodes;}
  showModal(){assert.equal(this.open,false);this.open=true;}
  close(){this.open=false;}
  focus(){this.focused=true;}
}
async function setup(state={}) {
  const nodes=Object.fromEntries([...html.matchAll(/<(\w+)([^>]*\bid="(import-install[^"]*)"[^>]*)>/g)]
    .map(([,tag,attributes,id])=>[id.replace("import-install",""),new Node(tag,attributes)]));
  nodes[""].querySelectorAll=()=>Object.entries(nodes).filter(([id,node])=>id!=="-open"&&["button","input","select"].includes(node.tag)).map(([,node])=>node);
  const requests=[];let reloads=0;
  let response=()=>({enabled:true,decided:false,destination:"/portable/Game",candidates:[],...state});
  const document={getElementById:id=>nodes[id.replace("import-install","")],createElement:tag=>new Node(tag)};
  runInNewContext(script,{document,window:{workshopI18n:{set(node,key){node.textContent=key;},unbind(){},text:key=>key}},
    location:{reload(){reloads++;}},fetch:async(path,options)=>{
      const endpoint=path.replace("installation-import/",""),body=options?.body?JSON.parse(options.body):undefined;
      requests.push({endpoint,body});
      const value=await response(endpoint,body);return {ok:!value.error,status:value.error?400:200,json:async()=>value};
    }});
  await new Promise(resolve=>setImmediate(resolve));
  return {nodes,requests,get reloads(){return reloads;},respond(fn){response=fn;}};
}

test("fresh and existing outputs never open a second startup dialog or write an import decision",async()=>{
  for(const state of [{},{candidates:null},{decided:true},{candidates:["/old/Game"]},{decided:true,candidates:["/old/Game"]}]) {
    const s=await setup(state),n=s.nodes;
    assert.equal(n[""].open,false);
    assert.equal(n["-source"].focused,undefined);
    assert.equal(n["-open"].hidden,false);
    assert.equal(n["-destination"].textContent,"/portable/Game");
    assert.deepEqual(s.requests,[{endpoint:"state",body:undefined}]);
  }
});

test("manual import keeps detected folders, and Cancel or Escape closes without writing",async()=>{
  const path="/old/Game <literal>",s=await setup({candidates:[path]}),n=s.nodes;
  assert.equal(n["-detected"].hidden,false);
  assert.equal(n["-candidates"].children[0].textContent,path);
  assert.equal(n["-source"].value,path);
  await n["-open"].fire("click");assert.equal(n[""].open,true);
  assert.equal(n["-source"].focused,true);
  await n["-skip"].fire("click");assert.equal(n[""].open,false);
  await n["-open"].fire("click");await n[""].fire("cancel");
  assert.equal(n[""].open,false);
  assert.deepEqual(s.requests.map(r=>r.endpoint),["state"]);
});

test("manual import into a fresh folder still requires review and confirmation",async()=>{
  const s=await setup(),n=s.nodes;
  await n["-open"].fire("click");
  n["-source"].value="/older/Game";await n["-source"].fire("input");
  assert.equal(n["-apply"].disabled,true);
  s.respond((endpoint,body)=>{
    if(endpoint==="preview") {
      assert.deepEqual(body,{directory:"/older/Game",exact:false});
      return {source:"/older/Game",revision:"reviewed",copy:1,files:[{action:"copy",path:"saves/save.srm"}]};
    }
    assert.equal(endpoint,"apply");assert.deepEqual(body,{revision:"reviewed",confirm:true});
    return {copy:1,conflicts:0};
  });
  await n["-preview"].fire("click");assert.equal(n["-apply"].disabled,true);
  n["-confirm"].checked=true;await n["-confirm"].fire("change");
  assert.equal(n["-apply"].disabled,false);
  await n["-apply"].fire("click");
  assert.equal(n["-reload"].hidden,false);assert.equal(n["-skip"].hidden,true);assert.equal(n["-apply"].hidden,true);
  await n[""].fire("cancel");assert.equal(s.reloads,1);
  assert.deepEqual(s.requests.map(r=>r.endpoint),["state","preview","apply"]);
});

test("an active import review cannot be dismissed, and failures leave a cancellable dialog",async()=>{
  const s=await setup(),n=s.nodes;
  await n["-open"].fire("click");
  let finish;s.respond(()=>new Promise(resolve=>{finish=resolve;}));
  const review=n["-preview"].fire("click");
  await n[""].fire("cancel");assert.equal(n[""].open,true);
  finish({error:"The source folder no longer exists."});await review;
  assert.equal(n["-status"].textContent,"The source folder no longer exists.");
  assert.equal(n["-apply"].disabled,true);assert.equal(n["-skip"].disabled,false);
  await n["-skip"].fire("click");assert.equal(n[""].open,false);
});

test("disabled import stays hidden; detection warnings never interrupt startup",async()=>{
  let s=await setup({enabled:false});assert.equal(s.nodes["-open"].hidden,true);assert.equal(s.nodes[""].open,false);
  for(const state of [{warning:"Previous folder is unavailable."},{error:"Could not read import history."}]) {
    s=await setup(state);assert.equal(s.nodes[""].open,false);assert.equal(s.nodes["-open"].hidden,false);
    await s.nodes["-open"].fire("click");assert.equal(s.nodes["-status"].textContent,state.warning||state.error);
  }
});
