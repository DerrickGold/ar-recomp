import {test} from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";

class Node {
  constructor(tag){this.tag=tag;this.children=[];this.events={};this.textContent="";this.disabled=false;this.value="";}
  append(...nodes){this.children.push(...nodes);}
  after(node){this.next=node;}
  remove(){this.removed=true;}
  setAttribute(name,value){this[name]=value;}
  addEventListener(name,fn){this.events[name]=fn;}
  focus(){this.focused=true;}
  select(){this.selected=true;}
}
function setup(clipboard, fetch=async()=>{throw new TypeError("Failed to fetch");}){
  const window={},document={createElement:tag=>new Node(tag),querySelector:()=>({content:"test-version"})};
  const navigator={userAgent:"Test webview",clipboard};
  runInNewContext(readFileSync(new URL("feedback.js",import.meta.url),"utf8"),{window,document,navigator,TypeError,Error,Date,fetch});
  return {api:window.workshopFeedback,target:new Node("p"),window};
}
const find=(node,tag)=>node.children.flatMap(child=>[...(child.tag===tag?[child]:[]),...find(child,tag)]);
test("reports strip session addresses, tokens and home names without changing payload hashes",()=>{
  const {api}=setup();const token="a".repeat(36),hash="b".repeat(64);
  const result=api.sanitize(`http://127.0.0.1:1234/${token}/launch\n${token}\n/Users/Alice/Game\n/home/bob/Game\nC:\\Users\\Alice\\Game\n${hash}\nAuthorization: Bearer secret-value`);
  assert.doesNotMatch(result,/Alice|bob|127\.0\.0\.1|secret-value/);assert.ok(!result.includes(token));assert.ok(result.includes(hash));
  assert.match(result,/~\/Game/);
});
test("non-JSON responses get a stable error code and HTTP status, not a parser error",async()=>{
  const {api}=setup();await assert.rejects(api.readJSON({status:502,ok:false,json:async()=>{throw new SyntaxError("HTML <script>secret</script>");}}),error=>error.code==="AR_RESPONSE"&&error.status===502&&!error.message.includes("script"));
  await assert.rejects(api.readJSON({status:200,ok:true,json:async()=>null}),error=>error.code==="AR_RESPONSE");
  await assert.rejects(api.readJSON({status:409,ok:false,json:async()=>({error:"Close the game first",errorCode:"GAME_BUSY"})}),error=>error.code==="GAME_BUSY"&&error.status===409);
});
test("copy includes only the previewed bounded report; duplicate errors keep the same card",async()=>{
  let copied;const {api,target}=setup({writeText:async value=>{copied=value;}});
  const error=new Error("<script>literal failure</script>");
  const options={operation:"Launch game",log:"old-log"+"x".repeat(17000)};
  api.show(target,error,options);const card=target.next,report=find(card,"textarea")[0];
  assert.equal(report.readOnly,true);assert.match(report.value,/test-version/);assert.match(report.value,/<script>literal failure/);assert.ok(!report.value.includes("old-log"));
  assert.ok(report.value.length<18000);api.show(target,error,options);assert.equal(target.next,card);
  await find(card,"button")[0].events.click();assert.equal(copied,report.value);
  api.clear(target);assert.equal(card.removed,true);
});
test("clipboard denial selects the report for manual copy without losing the error",async()=>{
  const {api,target}=setup({writeText:async()=>{throw new Error("not permitted");}});api.show(target,new Error("disk full"));
  const button=find(target.next,"button")[0];await button.events.click();
  const report=find(target.next,"textarea")[0];assert.equal(report.focused,true);assert.equal(report.selected,true);assert.equal(button.disabled,false);
  assert.equal(find(target.next,"details")[0].open,true);
});
test("status retries are explicit and errors without a safe retry do not offer one",async()=>{
  const {api,target}=setup();let retries=0;
  api.show(target,Object.assign(new Error("Failed to fetch"),{code:"AR_NETWORK"}),{retry:async()=>{retries++;}});assert.equal(retries,0);
  assert.match(target.next.children[0].textContent,/connection/);
  await find(target.next,"button")[1].events.click();assert.equal(retries,1);
  api.show(target,new Error("import failed"));assert.equal(find(target.next,"button").length,1);
});
test("a failed status recheck remains visible without an unhandled rejection",async()=>{
  const {api,target}=setup();
  api.show(target,new Error("first failure"),{retry:async()=>{throw new Error("still offline");}});
  await find(target.next,"button")[1].events.click();
  assert.match(find(target.next,"textarea")[0].value,/still offline/);
  assert.equal(find(target.next,"button")[1].disabled,false);
});

test("only fetch failures are classified as network errors; cancellation is retained",async()=>{
  const {api,target}=setup();
  await assert.rejects(api.request("status"),error=>error.code==="AR_NETWORK");
  api.show(target,new TypeError("Cannot read properties of undefined"));
  assert.doesNotMatch(target.next.children[0].textContent,/connection/);
  assert.match(find(target.next,"textarea")[0].value,/Code: AR_OPERATION/);
  const abort=Object.assign(new Error("cancelled"),{name:"AbortError"});
  const cancelled=setup(undefined,async()=>{throw abort;});
  await assert.rejects(cancelled.api.request("status"),error=>error===abort);
});

test("structured errors keep localized guidance, raw details, exact action and partial outcome",async()=>{
  const {api,target,window}=setup();
  window.workshopI18n={text:(key,args,fallback)=>key==="builder.errors.font_protocol"?"Font check failed":fallback,
    set:(node,key)=>{node.textContent=key;},attribute(){}};
  const error=api.responseError({error:"invalid response: record 2",errorCode:"builder.errors.font_protocol",recoveryKey:"builder.recovery.font_protocol"},500);
  assert.equal(error.message,"Font check failed");assert.equal(error.detail,"invalid response: record 2");
  assert.equal(error.uiKey,"builder.errors.font_protocol");
  error.operation="Install language pack";error.outcome="Project imported; installation failed.";
  api.show(target,error,{operation:"Language editor"});
  const report=find(target.next,"textarea")[0].value;
  assert.match(report,/Operation: Install language pack/);assert.match(report,/Outcome: Project imported/);
  assert.match(report,/Code: builder.errors.font_protocol/);assert.match(report,/Details: invalid response: record 2/);
  assert.equal(target.next.children[0].textContent,"builder.recovery.font_protocol");
  assert.equal(api.responseError({error:"Readable fallback",errorCode:"builder.unknown"},400).uiKey,undefined);
});

test("expected conflict responses remain available to explicit confirmation flows",async()=>{
  const {api}=setup();const body={code:"replace_required",release:"jp"};
  assert.equal(await api.readJSON({status:409,ok:false,json:async()=>body},[409]),body);
  await assert.rejects(api.readJSON({status:500,ok:false,json:async()=>({error:"failed"})},[409]),error=>error.status===500);
});
