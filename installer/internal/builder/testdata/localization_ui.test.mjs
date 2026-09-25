import {test as nodeTest} from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";
import {Node,setup} from "./interface_dom.mjs";
const test=(name,fn)=>nodeTest(name,{timeout:10000},fn);

// Minimal DOM for the real editor markup/controller. This is a state/lifetime
// test, not a layout engine or a substitute for browser visual qualification.
class Element extends Node {
  get children(){return (this.content||[]).filter(n=>n instanceof Element);}
  set children(value){this.content=value;}
  get textContent(){return (this.content||[]).map(n=>typeof n==="string"?n:n.textContent).join("");}
  set textContent(value){this.content=[String(value)];}
  get id(){return this.getAttribute("id")||"";}
  set id(value){this.setAttribute("id",value);}
  get name(){return this.getAttribute("name")||"";}
  set name(value){this.setAttribute("name",value);}
  get className(){return this.getAttribute("class")||"";}
  set className(value){this.setAttribute("class",value);}
  append(...nodes){for(const n of nodes){this.content.push(n);if(typeof n!=="string")n.parent=this;}}
  appendChild(node){this.append(node);return node;}
  after(node){const index=this.parent.content.indexOf(this);this.parent.content.splice(index+1,0,node);node.parent=this.parent;}
  remove(){if(this.parent)this.parent.content.splice(this.parent.content.indexOf(this),1);}
  prepend(...nodes){this.content.unshift(...nodes);for(const n of nodes)if(typeof n!=="string")n.parent=this;}
  replaceChildren(...nodes){this.content=[];this.append(...nodes);}
  get options(){return this.querySelectorAll("option");}
  get elements(){const all=this.querySelectorAll("input,select,textarea,button");const result=Object.fromEntries(all.filter(n=>n.name).map(n=>[n.name,n]));result.namedItem=name=>result[name];return result;}
  get lastElementChild(){return this.children.at(-1);}
  matches(selector){
    if(!selector)return super.matches();
    return selector.split(",").some(part=>{
      const pieces=part.trim().split(/\s+/),leaf=pieces.pop();
      if(!this.simple(leaf))return false;
      let parent=this.parent;
      while(pieces.length){const ancestor=pieces.pop();while(parent&&!parent.simple(ancestor))parent=parent.parent;if(!parent)return false;parent=parent.parent;}
      return true;
    });
  }
  simple(selector){
    const attrs=[...selector.matchAll(/\[([^=\]]+)(?:=["']?([^\]"']*)["']?)?\]/g)];
    const base=selector.replace(/\[[^\]]*\]/g,"");
    if(base.startsWith("#")&&this.id!==base.slice(1))return false;
    if(base.startsWith(".")&&!this.className.split(/\s+/).includes(base.slice(1)))return false;
    if(base&&!base.startsWith("#")&&!base.startsWith(".")&&base.toUpperCase()!==this.tagName)return false;
    return attrs.every(([,name,value])=>this.hasAttribute(name)&&(value===undefined||this.getAttribute(name)===value));
  }
  querySelectorAll(selector){return this.children.flatMap(n=>[...(n.matches(selector)?[n]:[]),...n.querySelectorAll(selector)]);}
  querySelector(selector){return this.querySelectorAll(selector)[0]||null;}
  closest(selector){let n=this;while(n&&!n.matches(selector))n=n.parent;return n;}
  contains(node){return node===this||this.children.some(child=>child.contains(node));}
  scrollIntoView(){}
  showModal(){this.open=true;}
  close(){if(this.open){this.open=false;void this.fire("close");}}
  checkValidity(){return true;}
  reportValidity(){return true;}
  reset(){for(const el of Object.values(this.elements))if(typeof el!=="function")el.value="";}
  setSelectionRange(start,end){this.selectionStart=start;this.selectionEnd=end;}
  setRangeText(text,start,end){this.value=this.value.slice(0,start)+text+this.value.slice(end);this.selectionStart=this.selectionEnd=start+text.length;}
}
function parseMarkup(html,doc){
  const root=new Element("main"),stack=[root];
  const decode=text=>text.replace(/&(?:amp|lt|gt|quot|apos);/g,entity=>({"&amp;":"&","&lt;":"<","&gt;":">","&quot;":'"',"&apos;":"'"}[entity]));
  for(const token of html.matchAll(/<!--[\s\S]*?-->|<[^>]+>|[^<]+/g)){
    const text=token[0];
    if(text.startsWith("<!--"))continue;
    if(text.startsWith("</")){assert.equal(stack.at(-1).tagName,text.slice(2,-1).toUpperCase());stack.pop();continue;}
    if(text.startsWith("<")){
      const tag=text.match(/^<([a-z0-9-]+)/)[1];
      const attrs=Object.fromEntries([...text.slice(tag.length+1,-1).matchAll(/([^\s=]+)(?:="([^"]*)")?/g)].map(([,name,value])=>[name,decode(value||"")]));
      const el=new Element(tag,"",attrs);el.files=[];el.checked=Object.hasOwn(attrs,"checked");el.disabled=Object.hasOwn(attrs,"disabled");el.hidden=Object.hasOwn(attrs,"hidden");
      el.value=attrs.value||"";el.focus=()=>{doc.activeElement=el;};stack.at(-1).append(el);
      if(!["link","input","img","br"].includes(tag))stack.push(el);
    }else stack.at(-1).append(decode(text));
  }
  assert.equal(stack.length,1);
  for(const select of root.querySelectorAll("select"))select.value=(select.options.find(o=>o.hasAttribute("selected"))||select.options[0])?.value||"";
  return root;
}
function setupEditor(){
  const s=setup();
  const root=parseMarkup(readFileSync(new URL("../localization.html",import.meta.url),"utf8"),s.doc);
  s.doc.children.push(root);
  const originalGet=s.doc.getElementById;
  s.doc.getElementById=id=>root.querySelector("#"+id)||originalGet(id);
  s.doc.createElement=tag=>{const node=new Element(tag);node.focus=()=>{s.doc.activeElement=node;};return node;};
  s.doc.createTextNode=text=>String(text);
  s.doc.querySelector=selector=>root.querySelector(selector);
  s.context.navigator={userAgent:"Workshop test browser"};
  const requests=[];const confirmations=[];
  let reply=async()=>{throw Error("unexpected request");};
  s.context.window.addEventListener=()=>{};
  s.context.window.scrollTo=()=>{};
  s.context.window.confirm=message=>{confirmations.push(message);return true;};
  class FormData {
    constructor(form){this.fields=new Map();if(form)for(const [name,node] of Object.entries(form.elements))if(name!=="namedItem"&&!node.disabled&&(node.getAttribute("type")!=="checkbox"||node.checked))this.fields.set(name,node.value);}
    set(key,value){this.fields.set(key,value);}
    get(key){return this.fields.get(key);}
    [Symbol.iterator](){return this.fields[Symbol.iterator]();}
  }
  Object.assign(s.context,{URL,location:{href:"http://127.0.0.1:1/test/"},FormData,fetch:async(url,options)=>{
    if(String(url)==="interface/preferences")return {ok:true,json:async()=>JSON.parse(options.body)};
    const endpoint=new URL(url).pathname.split("/").at(-1);
    const data=typeof options?.body==="string"?JSON.parse(options.body):options?.body;
    requests.push({endpoint,data,query:Object.fromEntries(new URL(url).searchParams)});
    return reply(endpoint,data,Object.fromEntries(new URL(url).searchParams));
  }});
  s.ui.apply(root);
  runInNewContext(readFileSync(new URL("../../workshopui/feedback.js",import.meta.url),"utf8"),s.context);
  runInNewContext(readFileSync(new URL("../localization_styling.js",import.meta.url),"utf8"),s.context);
  for (const name of ["localization_font_editor.js", "localization_library.js", "localization_import_editor.js", "localization.js"])
    runInNewContext(readFileSync(new URL("../" + name,import.meta.url),"utf8"),s.context);
  return {...s,root,requests,confirmations,node:id=>s.doc.getElementById("loc-"+id),respond(fn){reply=async(...args)=>({ok:true,json:async()=>fn(...args)});},response(fn){reply=fn;}};
}
const packageInfo={key:"edition-folder",id:"community.same-locale",name:"Français {name} <b>1234</b>",locale:"en-CA",project:true,installed:true,enabled:true,installedName:"Installed <i>edition</i>",installedRevision:"keep-revision"};
const snapshot={sourceAvailable:true,root:"/tmp/My translations",project:null};
const sharedPack={name:"Community 日本語.ARLANG",size:128};
const sharedPreview={token:"shared-preview",metadata:{...packageInfo,author:"Community team",license:"CC-BY-4.0"},messages:2};
function dragEvent(s,files=[sharedPack],extra={}){
  return {target:s.root,dataTransfer:{types:["Files"],files},prevented:false,preventDefault(){this.prevented=true;},...extra};
}
function importResponder(s,preview=sharedPreview){
  s.respond(endpoint=>{
    if(endpoint==="state")return snapshot;
    if(endpoint==="projects"||endpoint==="catalog")return [];
    if(endpoint==="import")return preview;
    throw Error("unexpected endpoint "+endpoint);
  });
}

test("install starts with arlang and keeps folder/backup sources secondary",async()=>{
  const s=setupEditor();importResponder(s);
  await s.context.window.localizationActivate();
  assert.equal(s.node("library-section").hidden,false);
  assert.equal(s.node("quick-import").elements.file.getAttribute("accept"),".arlang");
  await s.root.querySelector('[data-loc-flow="install"]').fire("click");
  assert.equal(s.node("import-source").hidden,false);
  assert.equal(s.node("import").elements.file.getAttribute("accept"),".arlang");
  assert.equal(s.root.querySelector('[data-loc-install="import"]').getAttribute("aria-pressed"),"true");
  assert.equal(!!s.node("folder-options").open,false);
  assert.equal(!!s.node("archive-options").open,false);
  assert.equal(s.node("import-backup").elements.file.getAttribute("accept"),".arproject,.zip");
  assert.ok(!s.requests.some(r=>["choose-directory","directory","import"].includes(r.endpoint)));
});

test("quick picker and global drop open the same detached preview before any install",async()=>{
  for(const method of ["picker","drop"]){
    const s=setupEditor();importResponder(s);
    let navigations=0;s.context.window.workshopOpenLanguages=()=>{navigations++;return true;};
    if(method==="picker"){
      const input=s.node("quick-import").elements.file;
      input.files=[sharedPack];input.value="C:\\fakepath\\Community.ARLANG";
      await input.fire("change");assert.equal(input.value,"");
    }else{
      const event=dragEvent(s);
      await s.doc.fire("dragenter",event);assert.equal(s.node("drop-overlay").hidden,false);
      await s.doc.fire("drop",event);
    }
    assert.equal(navigations,1);
    assert.deepEqual(s.requests.map(r=>r.endpoint),["state","projects","catalog","import"]);
    const upload=s.requests.at(-1).data;
    assert.equal(upload.get("file"),sharedPack);assert.equal(upload.get("intent"),"preview");
    assert.equal(s.node("import-preview").hidden,false);
    assert.equal(s.node("import-name").textContent,packageInfo.name);
    assert.equal(s.doc.activeElement,s.node("import-name"));
    assert.equal(s.node("accept-import").disabled,false);
    assert.equal(s.node("drop-overlay").hidden,true);
  }
});

test("dropped conflicts require explicit choices; installed packs remain toggleable",async()=>{
  const s=setupEditor();importResponder(s,{...sharedPreview,existingProject:{name:"Previous edition",revision:"prior-project"},installed:true});
  await s.doc.fire("drop",dragEvent(s));
  assert.equal(s.node("accept-import").disabled,true);
  s.node("replace-project").checked=true;await s.node("replace-project").fire("input");
  assert.equal(s.node("accept-import").disabled,true);
  s.node("import-replace-installed").checked=true;await s.node("import-replace-installed").fire("input");
  assert.equal(s.node("accept-import").disabled,false);
  s.respond((endpoint,data)=>{
    if(endpoint==="accept-import"){
      assert.deepEqual(data,{importToken:sharedPreview.token,newID:"",replace:true,expected:"prior-project"});return projectSnapshot();
    }
    if(endpoint==="projects"||endpoint==="tree")return [];
    if(endpoint==="installation")return {installed:true};
    if(endpoint==="install"){
      assert.equal(data.replace,true);return {path:"/game/languages/packs/edition",enabled:false,report:{messages:2}};
    }
    if(endpoint==="catalog")return [{...packageInfo,enabled:false}];
    throw Error(endpoint);
  });
  await s.node("accept-import").fire("click");
  assert.equal(s.node("installed").hidden,false);
  assert.equal(s.node("installed-title").textContent,s.ui.text("builder.language.updated_disabled"));
  await s.node("installed-home").fire("click");
  let enabled=false;
  s.respond((endpoint,data)=>{
    if(endpoint==="set-enabled"){
      assert.deepEqual(data,{id:packageInfo.id,directory:packageInfo.key,expected:packageInfo.installedRevision,enabled:!enabled});
      enabled=data.enabled;return {enabled};
    }
    assert.equal(endpoint,"catalog");return [{...packageInfo,enabled}];
  });
  for(const checked of [true,false]){
    const toggle=s.node("library").querySelector("input");toggle.checked=checked;await toggle.fire("change");
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(enabled,checked);assert.equal(s.node("library").querySelector("input").checked,checked);
  }
  assert.ok(!s.requests.some(r=>r.endpoint==="uninstall"));
});

async function openInstalledLibrary(s,rows=[packageInfo]){
  s.respond(endpoint=>{
    if(endpoint==="state")return snapshot;
    if(endpoint==="projects")return [];
    if(endpoint==="catalog")return rows;
    throw Error("unexpected endpoint "+endpoint);
  });
  // The desktop webview can silently reject native JavaScript dialogs.
  s.context.window.confirm=()=>{throw Error("native confirmation is unavailable");};
  await s.context.window.localizationActivate();
}

test("uninstall opens a localized in-page review; cancel and dismissal send no request",async()=>{
  const s=setupEditor();await openInstalledLibrary(s);
  for(const dismiss of [()=>s.node("uninstall-cancel").fire("click"),()=>s.node("uninstall-dialog").close()]){
    const before=s.requests.length;
    await s.node("library").querySelector(".loc-uninstall").fire("click");
    assert.equal(s.node("uninstall-dialog").open,true);
    assert.equal(s.doc.activeElement,s.node("uninstall-cancel"));
    for(const locale of ["fr","de","ja","en"]){
      s.picker.value=locale;await s.picker.fire("change");
      assert.equal(s.node("uninstall-question").textContent,s.ui.text("builder.language.uninstall_confirm",{
        name:packageInfo.installedName,id:packageInfo.id,folder:packageInfo.key
      }));
      assert.equal(s.node("uninstall-question").children.length,0,"pack names must remain literal text");
      assert.equal(s.node("uninstall-confirm").textContent,s.ui.text("builder.language.uninstall_now"));
    }
    await dismiss();
    assert.equal(s.node("uninstall-dialog").open,false);
    await s.node("uninstall-confirm").fire("click");
    assert.equal(s.requests.length,before);
  }
});

test("confirmed uninstall targets the reviewed installation and refreshes the library",async()=>{
  const s=setupEditor(),other={...packageInfo,key:"Disabled pack.arlang",enabled:false,installedRevision:"other-revision"};
  await openInstalledLibrary(s,[packageInfo,other]);
  await s.node("library").querySelectorAll(".loc-uninstall")[1].fire("click");
  let release;const pending=new Promise(resolve=>{release=resolve;});
  s.response(async(endpoint,data)=>{
    if(endpoint==="uninstall"){
      assert.deepEqual(data,{id:other.id,directory:other.key,expected:other.installedRevision,confirmUninstall:true});
      await pending;return {ok:true,json:async()=>({backup:"/packs/.uninstalled/recovery.arlang"})};
    }
    assert.equal(endpoint,"catalog");return {ok:true,json:async()=>[packageInfo]};
  });
  const uninstall=s.node("uninstall-confirm").fire("click");
  assert.equal(s.node("uninstall-dialog").open,false);
  assert.equal(s.node("uninstall-confirm").disabled,true);
  await s.node("uninstall-confirm").fire("click");
  await s.node("library").querySelector(".loc-uninstall").fire("click");
  assert.equal(s.node("uninstall-dialog").open,false);
  release();await uninstall;
  assert.equal(s.requests.filter(r=>r.endpoint==="uninstall").length,1);
  assert.equal(s.node("library").children.length,1);
  assert.equal(s.node("library").children[0].dataset.packageKey,packageInfo.key);
  assert.equal(s.node("feedback").textContent,s.ui.text("builder.language.uninstalled",{backup:"/packs/.uninstalled/recovery.arlang"}));
  assert.equal(s.doc.activeElement,s.node("library-search"));
});

test("failed uninstall preserves the installed card and allows a new review",async()=>{
  const s=setupEditor();await openInstalledLibrary(s);
  await s.node("library").querySelector(".loc-uninstall").fire("click");
  s.response(async endpoint=>{
    assert.equal(endpoint,"uninstall");
    return {ok:false,status:409,json:async()=>({errorCode:"builder.language.request_conflict",error:"Pack changed"})};
  });
  await s.node("uninstall-confirm").fire("click");
  assert.equal(s.node("uninstall-dialog").open,false);
  assert.equal(s.node("library").children.length,1);
  assert.equal(s.node("feedback").dataset.error,"true");
  assert.equal(s.node("feedback").textContent,s.ui.text("builder.language.request_conflict",{detail:"Pack changed"}));
  const button=s.node("library").querySelector(".loc-uninstall");
  assert.equal(button.disabled,false);await button.fire("click");
  assert.equal(s.node("uninstall-dialog").open,true);
  await s.doc.fire("workshop:closed");
  assert.equal(s.node("uninstall-dialog").open,false);
  await s.node("uninstall-confirm").fire("click");
  assert.equal(s.requests.filter(r=>r.endpoint==="uninstall").length,1);
});

test("invalid or multiple drops show localized guidance without requests or draft loss",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  s.node("body").value="My unsaved draft";await s.node("body").fire("input");
  const before=s.requests.length;
  for(const [files,key] of [
    [[{name:"notes.txt",size:50}],"package_types"],
    [[{name:"backup.arproject",size:50}],"package_types"],
    [[{name:"empty.arlang",size:0}],"archive_size"],
    [[{name:"oversized.arlang",size:257*1024*1024+1}],"archive_size"],
    [[sharedPack,sharedPack],"one_archive"]
  ]){
    await s.doc.fire("drop",dragEvent(s,files));
    assert.equal(s.node("drop-overlay").hidden,false);
    for(const locale of ["fr","de","ja","en"]){
      s.picker.value=locale;await s.picker.fire("change");
      assert.equal(s.node("drop-message").textContent,s.ui.text("builder.language."+key));
    }
    assert.equal(s.requests.length,before);assert.equal(s.confirmations.length,0);
    assert.equal(s.node("body").value,"My unsaved draft");assert.equal(s.context.window.localizationHasEdits(),true);
  }
  await s.node("dismiss-drop").fire("click");assert.equal(s.node("drop-overlay").hidden,true);
});

test("cancelled draft confirmation and unavailable game data prevent drop import",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  s.node("body").value="Keep this";await s.node("body").fire("input");
  s.context.window.confirm=()=>false;
  const before=s.requests.length;
  await s.doc.fire("drop",dragEvent(s));
  assert.equal(s.requests.length,before);assert.equal(s.node("body").value,"Keep this");
  assert.equal(s.context.window.localizationHasEdits(),true);
  s.context.window.confirm=()=>true;s.context.window.workshopOpenLanguages=()=>false;
  await s.doc.fire("drop",dragEvent(s));
  assert.equal(s.requests.length,before);assert.equal(s.context.window.localizationHasEdits(),true);
  assert.equal(s.node("drop-message").textContent,s.ui.text("builder.language.build_required"));
});

test("busy drops cannot supersede an in-flight preview and the same picker file can retry",async()=>{
  const s=setupEditor();importResponder(s);await s.context.window.localizationActivate();
  let release;const pending=new Promise(resolve=>{release=resolve;});
  s.response(async endpoint=>{assert.equal(endpoint,"import");await pending;return {ok:true,json:async()=>sharedPreview};});
  const input=s.node("quick-import").elements.file;input.files=[sharedPack];
  const first=input.fire("change");
  await s.doc.fire("drop",dragEvent(s,[{...sharedPack,name:"second.arlang"}]));
  assert.equal(s.node("drop-message").textContent,s.ui.text("builder.language.drop_busy"));
  release();await first;
  assert.equal(s.requests.filter(r=>r.endpoint==="import").length,1);
  importResponder(s);await input.fire("change");
  assert.equal(s.requests.filter(r=>r.endpoint==="import").length,2);
  assert.equal(input.value,"");
});

test("text drags and native file controls are not intercepted; closed sessions do not upload",async()=>{
  const s=setupEditor();let prevented=0;
  const event=dragEvent(s,[],{dataTransfer:{types:["text/plain"],files:[]},preventDefault(){prevented++;}});
  await s.doc.fire("dragover",event);await s.doc.fire("drop",event);assert.equal(prevented,0);
  const native=dragEvent(s,[sharedPack],{target:s.node("import").elements.file,preventDefault(){prevented++;}});
  await s.doc.fire("dragenter",dragEvent(s));assert.equal(s.node("drop-overlay").hidden,false);
  await s.doc.fire("dragover",native);await s.doc.fire("drop",native);
  assert.equal(prevented,0);assert.equal(s.node("drop-overlay").hidden,true);
  await s.doc.fire("workshop:closed");
  await s.doc.fire("drop",dragEvent(s,[sharedPack],{preventDefault(){prevented++;}}));
  assert.equal(prevented,1);assert.equal(s.requests.length,0);
});

test("invalid archive contents leave a retryable picker and do not install",async()=>{
  const s=setupEditor();importResponder(s);await s.context.window.localizationActivate();
  s.response(async endpoint=>{
    assert.equal(endpoint,"import");return {ok:false,status:400,json:async()=>({error:"invalid ZIP archive"})};
  });
  const input=s.node("quick-import").elements.file;input.files=[sharedPack];input.value="selected.arlang";
  await input.fire("change");
  assert.equal(input.value,"");assert.equal(input.disabled,false);
  assert.equal(s.node("import-preview").hidden,true);assert.equal(s.node("accept-import").disabled,true);
  assert.ok(s.node("feedback").textContent.includes("invalid ZIP archive"));
  importResponder(s);
  // Some webviews expose the files on drop without a populated types list.
  let prevented=false;
  await s.doc.fire("drop",dragEvent(s,[sharedPack],{dataTransfer:{files:[sharedPack]},preventDefault(){prevented=true;}}));
  assert.equal(prevented,true);assert.equal(s.node("import-preview").hidden,false);
  assert.equal(s.requests.filter(r=>r.endpoint==="import").length,2);
  assert.ok(!s.requests.some(r=>["accept-import","install"].includes(r.endpoint)));
});

test("backup picker remains available to edit, with no automatic installation",async()=>{
  const s=setupEditor();importResponder(s);await s.context.window.localizationActivate();
  await s.root.querySelector('[data-loc-flow="edit"]').fire("click");
  assert.equal(s.node("archive-options").open,true);
  const input=s.node("import-backup").elements.file;input.files=[{name:"work.arproject",size:100}];
  await input.fire("change");
  assert.equal(s.requests.at(-1).endpoint,"import");
  assert.equal(s.node("accept-import").textContent,s.ui.text("builder.language.import_edit"));
  assert.ok(!s.requests.some(r=>["accept-import","install"].includes(r.endpoint)));
});

test("chooser recovery uses typed translated copy and preserves literal details",async()=>{
  const s=setupEditor(),detail="platform 100% {name} 日本語";
  for(const code of ["chooser_unavailable","chooser_failed","chooser_busy"]){
    s.response(async endpoint=>{
      assert.equal(endpoint,"choose-directory");
      return {ok:false,status:400,json:async()=>({errorCode:"builder.language."+code,error:detail})};
    });
    await s.node("pick-directory").fire("click");
    for(const locale of ["en","fr","de","ja"]){
      s.picker.value=locale;await s.picker.fire("change");
      assert.equal(s.node("feedback").textContent,s.ui.text("builder.language."+code,{detail}));
      assert.equal(s.node("feedback").dataset.error,"true");
    }
  }
});

test("create examples translate without replacing entered package identity",async()=>{
  const s=setupEditor(),fields=s.node("create").elements;
  fields.name.value="My custom package";fields.autonym.value="العربية";
  for(const locale of ["en","fr","de","ja"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(fields.name.getAttribute("placeholder"),s.ui.text("builder.language.name_example"));
    assert.equal(fields.autonym.getAttribute("placeholder"),s.ui.text("builder.language.autonym_example"));
    assert.equal(fields.name.value,"My custom package");assert.equal(fields.autonym.value,"العربية");
  }
});

test("real editor catalog translates in place without changing enabled choices or identifiers",async()=>{
  const s=setupEditor();
  s.respond(endpoint=>({state:snapshot,projects:[],catalog:[packageInfo]})[endpoint]);
  await s.context.window.localizationActivate();
  const card=s.node("library").children[0],checkbox=card.querySelector("input"),button=card.querySelector("button");
  s.doc.activeElement=checkbox;
  s.node("library-search").value="unsent search";s.node("library-search").selectionStart=3;
  const before=s.requests.length;
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.requests.length,before);assert.equal(s.node("library").children[0],card);
    assert.equal(s.doc.activeElement,checkbox);assert.equal(checkbox.checked,true);
    assert.equal(checkbox.getAttribute("aria-label"),s.ui.text("builder.language.enable_pack",{name:packageInfo.installedName}));
    assert.equal(button.textContent,s.ui.text("builder.language.edit_project"));
    assert.equal(card.querySelector("h3").textContent,packageInfo.name);
    assert.equal(card.dataset.packageId,packageInfo.id);assert.equal(card.dataset.packageKey,packageInfo.key);
    assert.equal(s.node("library-search").value,"unsent search");assert.equal(s.node("library-search").selectionStart,3);
    assert.equal(s.node("library-summary").textContent,s.ui.text("builder.language.library_counts",{enabled:1,disabled:0,projects:1}));
  }
  s.node("library-search").value="";
  s.respond((endpoint,data)=>{
    if(endpoint==="set-enabled"){assert.deepEqual(data,{id:packageInfo.id,directory:packageInfo.key,expected:packageInfo.installedRevision,enabled:false});return {enabled:false};}
    assert.equal(endpoint,"catalog");return [{...packageInfo,enabled:false}];
  });
  checkbox.checked=false;await checkbox.fire("change");
  const feedback=s.ui.text("builder.language.availability_saved");
  // The listener invokes run() asynchronously, as a browser event would.
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(s.node("feedback").textContent,feedback);
});

test("import preview keeps credit metadata, files and explicit conflict choices across locales",async()=>{
  const s=setupEditor();
  s.respond(endpoint=>({state:snapshot,projects:[],catalog:[]})[endpoint]);
  await s.context.window.localizationActivate();
  await s.root.querySelector('[data-loc-flow="install"]').fire("click");
  await s.root.querySelector('[data-loc-install="import"]').fire("click");
  const preview={token:"opaque-preview",metadata:{...packageInfo,author:"Élodie & Team {count}",license:"CC-BY-4.0"},messages:1234,existingProject:{name:"Old work",revision:"old-revision"},installed:true};
  s.respond(endpoint=>{assert.equal(endpoint,"directory");return preview;});
  await s.node("directory").fire("submit",{preventDefault(){},currentTarget:s.node("directory")});
  // Blank path is intentionally ignored; supplying one triggers the preview.
  s.node("directory").elements.directory.value="/tmp/shared pack";
  await s.node("directory").elements.directory.fire("change",{target:s.node("directory").elements.directory});
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(s.node("import-name").textContent,packageInfo.name);
  s.node("replace-project").checked=true;s.node("import-replace-installed").checked=true;
  await s.node("replace-project").fire("input");
  const files=[{name:"same locale.arlang"}];s.node("import").elements.file.files=files;
  s.node("import-new-id").value="community.new-edition";await s.node("import-new-id").fire("input");
  const before=s.requests.length,button=s.node("accept-import");
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.requests.length,before);assert.equal(s.node("import").elements.file.files,files);
    assert.equal(s.node("replace-project").checked,true);assert.equal(s.node("import-replace-installed").checked,true);
    assert.equal(s.node("import-new-id").value,"community.new-edition");
    assert.equal(button.disabled,false);assert.equal(button.textContent,s.ui.text("builder.language.import_install"));
    assert.ok(s.node("import-summary").textContent.includes(preview.metadata.author));
    assert.ok(s.node("import-summary").textContent.includes(preview.metadata.license));
    assert.ok(s.node("import-summary").textContent.includes(new Intl.NumberFormat(locale).format(1234)));
    assert.equal(s.node("project-conflict-note").textContent,s.ui.text("builder.language.project_conflict",{id:packageInfo.id,name:"Old work"}));
  }
});

test("dynamic accessibility bindings cannot change action targets or form values",async()=>{
  const s=setup();const button=new Node("button");s.doc.children.push(button);
  const name="<img> {name} Français";
  s.ui.attribute(button,"aria-label","builder.language.enable_pack",{name});
  for(const target of ["value","href","name","src","onclick"])assert.throws(()=>s.ui.attribute(button,target,"builder.language.enable_pack",{name}),/unsupported/);
  s.picker.value="ja";await s.picker.fire("change");
  assert.equal(button.getAttribute("aria-label"),s.ui.text("builder.language.enable_pack",{name}));
});

const messageID="sky.action_mode.confirm";
const nativeBody="@anchor reset_text_cursor.00\nInvented source.\n@anchor yield.01\n@end\n";
function projectSnapshot(formatVersion=2){return {...snapshot,project:{formatVersion,origin:"translation",revision:"original-revision",
  metadata:{id:packageInfo.id,name:packageInfo.name,locale:"en-CA",autonym:"English (Canada)",author:"Élodie & Team",license:"CC-BY-4.0",direction:"auto"},
  notes:"Private <note> {name}",notices:{"notices/CREDITS.txt":"Original contributor"},fonts:{primary:"builtin:actraiser-sans",fallback:[]},
  totals:[{total:1,done:0,wip:1}]}};}
function editorMessage(){return {message:{present:true,path:"text/translation.artext",body:nativeBody,status:"wip",reference:{anchors:["reset_text_cursor.00","yield.01"],placeholders:[],native_in_profile:true}},reference:{body:nativeBody},referenceMetadata:{id:"native-us",name:"Native US",locale:"en-US"}};}
async function openEditorMessage(s,view=editorMessage(),select=true,project=projectSnapshot()){
  s.respond(endpoint=>{
    if(endpoint==="open")return project;
    if(endpoint==="projects"||endpoint==="catalog")return [];
    if(endpoint==="tree")return [{id:messageID,label:"Invented message",is_message:true,done:0,wip:1}];
    if(endpoint==="installation")return {installed:false};
    if(endpoint==="message")return view;
    throw Error("unexpected endpoint "+endpoint);
  });
  await s.context.window.localizationOpenProject(packageInfo.id);
  assert.equal(s.node("feedback").textContent,"",s.node("feedback").textContent);
  if(select)await s.node("tree").querySelector("button").fire("click");
}

test("save adopts progress and reports whether the installed copy was updated",async()=>{
  for(const [update,key] of [
    [{installed:false,updated:false,enabled:false},"saved_feedback"],
    [{installed:true,updated:true,enabled:true},"saved_installed"],
    [{installed:true,updated:true,enabled:false},"saved_disabled"],
    [{installed:true,updated:false,enabled:true,error:"Font check <failed>"},"saved_update_failed"]
  ]){
    const s=setupEditor();await openEditorMessage(s);
    const body=nativeBody.replace("Invented source.","My saved translation.");
    s.node("body").value=body;await s.node("body").fire("input");
    const next=projectSnapshot();next.project.revision="saved-revision";next.installationUpdate=update;
    const view=editorMessage();view.message.body=body;
    s.respond((endpoint,data)=>{
      if(endpoint==="save"){assert.equal(data.body,body);return next;}
      if(endpoint==="projects"||endpoint==="catalog")return [];
      if(endpoint==="tree")return [{id:messageID,label:"Invented message",is_message:true,done:0,wip:1}];
      if(endpoint==="message")return view;
      throw Error("unexpected endpoint "+endpoint);
    });
    await s.node("save-progress").fire("click");
    assert.equal(s.context.window.localizationHasEdits(),false);
    assert.equal(s.node("body").value,body);
    assert.equal(s.node("save-progress").disabled,true);
    for(const locale of ["en","fr","de","ja"]){
      s.picker.value=locale;await s.picker.fire("change");
      assert.equal(s.node("feedback").textContent,s.ui.text("builder.language."+key,update.error?{detail:update.error}:{}));
    }
  }
});

test("editor language switches retain a real message, metadata, notice and font draft",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  const body="@anchor reset_text_cursor.00\nSir {player_name}, most excellent!\n@anchor yield.01\n@end\n";
  s.node("body").value=body;s.node("body").selectionStart=8;s.node("body").selectionEnd=19;
  await s.node("body").fire("input");s.node("message-status").value="done";await s.node("message-status").fire("change");
  s.node("metadata").elements.name.value="My <b>edition</b> {name}";s.node("metadata").elements.notes.value="Keep private 日本語";
  await s.node("metadata").fire("input");
  s.node("notice").elements.noticeText.value="Credit {name}, not UI text";await s.node("notice").fire("input");
  const font=s.node("font-list").querySelector('input');font.files=[{name:"future.ttf",size:100}];await font.fire("change");
  // The existing font controller owns the selected File in its draft map.
  // A UI switch must not re-render/reconstruct that stack or lose the upload.
  const fontRows=s.node("font-list").children;
  s.doc.activeElement=s.node("body");const before=s.requests.length;
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.requests.length,before);assert.equal(s.context.window.localizationHasEdits(),true);
    assert.equal(s.node("body").value,body);assert.equal(s.node("body").selectionStart,8);assert.equal(s.node("body").selectionEnd,19);
    assert.equal(s.doc.activeElement,s.node("body"));assert.equal(s.node("message-status").value,"done");
    assert.equal(s.node("metadata").elements.locale.value,"en-CA");assert.equal(s.node("metadata").elements.name.value,"My <b>edition</b> {name}");
    assert.equal(s.node("metadata").elements.notes.value,"Keep private 日本語");assert.equal(s.node("notice").elements.noticeText.value,"Credit {name}, not UI text");
    assert.equal(s.node("reference-body").textContent,nativeBody);
    assert.equal(s.node("font-list").children[0],fontRows[0]);
    assert.equal(fontRows[0].querySelector("label span").textContent,s.ui.text("builder.editor.primary_font"));
    assert.equal(fontRows[0].querySelector("small").textContent,s.ui.text("builder.editor.pending_font"));
    assert.equal(fontRows[0].querySelector("button").getAttribute("aria-label"),s.ui.text("builder.editor.move_up_font",{font:"fonts/future.ttf"}));
    assert.equal(s.node("font-check").textContent,s.ui.text("builder.editor.save_check"));
    assert.equal(s.node("dirty").textContent,s.ui.text("builder.editor.unsaved_message"));
    assert.equal(s.node("save-state").textContent,s.ui.text("builder.language.unsaved"));
    assert.equal(s.node("save-progress").disabled,false);
  }
  s.response(async(endpoint,data)=>{
    assert.equal(endpoint,"save");
    assert.ok(data instanceof s.context.FormData);
    const request=JSON.parse(data.get("request"));
    assert.equal(request.projectID,packageInfo.id);assert.equal(request.revision,"original-revision");
    assert.equal(request.body,body);assert.equal(request.status,"done");
    assert.equal(request.fonts.primary,"fonts/future.ttf");assert.equal(data.get("font0").name,"future.ttf");
    return {ok:false,status:409,json:async()=>({error:"revision conflict <raw>",errorCode:"builder.language.request_conflict"})};
  });
  await s.node("save-progress").fire("click");
  s.picker.value="ja";await s.picker.fire("change");
  assert.equal(s.node("feedback").textContent,s.ui.text("builder.language.request_conflict",{detail:"revision conflict <raw>"}));
  assert.equal(s.node("save-state").textContent,s.ui.text("builder.language.not_saved",{detail:"revision conflict <raw>"}));
  assert.equal(s.node("body").value,body);assert.equal(s.context.window.localizationHasEdits(),true);
});

test("editor form translations preserve script syntax, option values and control leaves",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  const codes=s.node("script-help").querySelectorAll("code").map(n=>n.textContent);
  assert.deepEqual(codes,["@line","@page","@anchor","{{","}}","@@","\\#","\\;"]);
  const controls=s.root.querySelectorAll("input,select,textarea,button");
  const options=s.root.querySelectorAll("option").map(n=>[n,n.value]);
  for(const node of s.root.querySelectorAll("[data-i18n]"))assert.equal(node.children.length,0,"binding would replace children: "+node.dataset.i18n);
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.deepEqual(s.root.querySelectorAll("input,select,textarea,button"),controls);
    for(const [node,value] of options)assert.equal(node.value,value);
    assert.deepEqual(s.node("script-help").querySelectorAll("code").map(n=>n.textContent),codes);
    assert.equal(s.node("body").value,nativeBody);
    assert.equal(s.node("body").closest("label").querySelector("span").textContent,s.ui.text("builder.editor.your_translation"));
    assert.equal(s.node("search").elements.q.getAttribute("placeholder"),s.ui.text("builder.editor.search_hint"));
    assert.equal(s.node("message-context").textContent,messageID+" · \n"+s.ui.text("builder.editor.saved_path",{path:"text/translation.artext"}));
  }
});

test("every authoritative layout shape renders across languages including native-only rows",async()=>{
  const contracts=JSON.parse(readFileSync(new URL("../../localization/data/author-contracts.json",import.meta.url),"utf8"));
  const shapes=[...new Map(contracts.routes.flatMap(route=>[route.presentation,...Object.values(route.presentation_by_profile||{})]).map(p=>[JSON.stringify(p),p])).values()];
  assert.ok(shapes.some(p=>p?.table?.rules.some(r=>r.native_reserved&&!r.fields)),"must exercise omitted native-row fields");
  assert.ok(shapes.some(p=>p?.keyboard));
  const s=setupEditor(),view=editorMessage();await openEditorMessage(s,view);
  for(const shape of shapes){
    view.message.reference.presentation=shape;
    await s.node("tree").querySelector("button").fire("click");
    assert.notEqual(s.node("feedback").dataset.error,"true",s.node("feedback").textContent);
    for(const locale of ["fr","de","ja","en"]){
      s.picker.value=locale;await s.picker.fire("change");
      const text=s.node("shape-rules").textContent;
      if(shape?.maximum_pages)assert.ok(text.includes(s.ui.text("builder.editor.maximum_pages",{count:shape.maximum_pages})));
      if(shape?.keyboard)assert.ok(text.includes(s.ui.text("builder.editor.keyboard_shape",{rows:shape.keyboard.rows,columns:shape.keyboard.columns,lines:shape.keyboard.maximum_lines,bytes:shape.keyboard.maximum_page_bytes})));
      for(const rule of shape?.table?.rules||[])assert.ok(text.includes(rule.native_reserved?s.ui.text("builder.editor.native_artwork"):s.ui.text("builder.editor.table_fields",{fields:rule.fields.join(", ")})));
      assert.equal(s.node("body").value,nativeBody);
    }
  }
});

test("font reports retranslate in place while diagnostics and sample payloads remain literal",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  const sample="ÉLISE 日本語 {count} <b>";s.node("font-sample").value=sample;
  let complete=false;
  s.respond((endpoint,data)=>{
    assert.equal(endpoint,"font-coverage");assert.equal(data.projectID,packageInfo.id);assert.deepEqual(Array.from(data.samples),[sample]);
    return {complete,scalars:1234,missingCount:1234,missing:complete?[]:[{codepoint:"U+65E5",character:"日",locations:[{messageID,source:"text/<raw>.artext",line:1234},{source:"sample[0]"}]}],dynamicValues:complete?[]:["master_name","current_city_name"]};
  });
  for(const state of [false,true]){
    complete=state;await s.node("font-check").fire("click");
    const report=s.node("font-report"),nodes=report.children;
    const before=s.requests.length;
    for(const locale of ["fr","de","ja","en"]){
      s.picker.value=locale;await s.picker.fire("change");
      assert.equal(s.requests.length,before);assert.deepEqual(report.children,nodes);
      assert.equal(nodes[0].textContent,s.ui.text(complete?"builder.editor.coverage_complete":"builder.editor.coverage_missing",{count:1234}));
      assert.equal(nodes[2].textContent,s.ui.text(complete?"builder.editor.no_unresolved_values":"builder.editor.unresolved_values",{values:"master_name, current_city_name"}));
      if(!complete)assert.equal(nodes[1].textContent,"U+65E5 “日” — "+messageID+" · text/<raw>.artext:1234; sample[0]");
      assert.equal(s.node("font-sample").value,sample);
    }
  }
  // Reordering invalidates this report and retains the real font references.
  await s.node("font-add").fire("click");
  assert.equal(s.node("font-report").children.length,0);
  s.picker.value="ja";await s.picker.fire("change");
  assert.equal(s.node("font-list").children[1].querySelector("button").getAttribute("aria-label"),s.ui.text("builder.editor.move_up_slot",{number:2}));
  assert.equal(s.node("font-list").children[0].querySelector("select").value,"builtin:actraiser-sans");
});

test("reference refresh preserves dirty work and never revives an obsolete missing-reference binding",async()=>{
  const s=setupEditor(),view=editorMessage();delete view.reference;delete view.referenceMetadata;
  await openEditorMessage(s,view);
  assert.equal(s.node("reference-body").textContent,s.ui.text("builder.editor.reference_missing"));
  s.node("body").value="Unsaved {master_name}";s.node("body").selectionStart=5;await s.node("body").fire("input");
  const literal="builder.editor.reference_missing\n{name} <not markup>";
  s.respond(endpoint=>{
    if(endpoint==="reference")return {...projectSnapshot(),reference:{id:"requested-de"}};
    if(endpoint==="projects")return [{id:"requested-de",name:"Deutsch {name}",locale:"de-DE"}];
    if(endpoint==="message")return {...view,reference:{body:literal},referenceMetadata:{id:"native-us",name:"Native {name}",locale:"en-US"}};
    throw Error(endpoint);
  });
  s.node("reference-project").value="requested-de";await s.node("reference-project").fire("change");
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.node("reference-body").textContent,literal);
    assert.equal(s.node("reference-title").textContent,s.ui.text("builder.editor.reference_fallback",{name:"Native {name}",locale:"en-US"}));
    assert.equal(s.node("body").value,"Unsaved {master_name}");assert.equal(s.node("body").selectionStart,5);
    assert.equal(s.node("feedback").textContent,s.ui.text("builder.editor.reference_changed"));
    assert.equal(s.context.window.localizationHasEdits(),true);
  }
  await openEditorMessage(s,editorMessage(),false);
  s.picker.value="ja";await s.picker.fire("change");
  assert.equal(s.node("message-title").textContent,s.ui.text("builder.editor.choose_message"));
  assert.equal(s.node("message-context").textContent,s.ui.text("builder.editor.choose_message_help"));
  assert.equal(s.node("dirty").textContent,"");
  await s.node("tree").querySelector("button").fire("click");
  assert.equal(s.node("dirty").textContent,"");assert.equal(s.node("reference-body").textContent,nativeBody);
  assert.equal(s.node("message-title").textContent,messageID);
});

test("script previews translate only framing, preserving controls, values and insertion syntax",async()=>{
  const s=setupEditor(),view=editorMessage();view.message.reference.placeholders=[{name:"master_name",kind:"localized_text"}];
  await openEditorMessage(s,view);
  s.node("body").selectionStart=s.node("body").selectionEnd=0;
  s.node("placeholders").value="master_name";
  s.picker.value="fr";await s.picker.fire("change");await s.node("insert-value").fire("click");
  assert.equal(s.node("body").value,"{master_name}"+nativeBody);
  await s.root.querySelector('[data-loc-insert="@line"]').fire("click");
  assert.ok(s.node("body").value.startsWith("{master_name}\n@line\n"));
  const draft=s.node("body").value;
  s.respond((endpoint,data)=>{assert.equal(endpoint,"preview");assert.equal(data.body,draft);return [{op:"text",value:"Literal {number} <i>日本語"},{op:"placeholder",name:"master_name",minimum_digits:2},{op:"page"},{op:"anchor",id:"yield.01"},{op:"wait",frames:1234}];});
  await s.node("preview").fire("click");
  const pages=s.node("preview-content").children,before=s.requests.length;
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.requests.length,before);assert.deepEqual(s.node("preview-content").children,pages);
    assert.equal(s.node("placeholders").options[1].value,"master_name");
    assert.equal(s.node("placeholders").options[1].textContent,s.ui.text("builder.editor.value_text",{name:"master_name"}));
    assert.equal(pages[0].textContent,s.ui.text("builder.editor.page_number",{number:1}));
    assert.equal(pages[1].textContent,"Literal {number} <i>日本語⟦master_name:02⟧");
    assert.equal(pages[1].querySelector("span").getAttribute("title"),s.ui.text("builder.editor.runtime_value"));
    assert.equal(pages[3].textContent,"anchor: yield.01wait: "+s.ui.text("builder.editor.frames",{count:1234}));
    assert.equal(s.node("feedback").textContent,s.ui.text("builder.editor.preview_valid"));
    assert.equal(s.node("body").value,draft);
  }
  s.picker.value="ja";await s.picker.fire("change");await s.node("empty").fire("click");
  assert.equal(s.confirmations.at(-1),s.ui.text("builder.editor.clear_confirm"));
  assert.equal(s.node("body").value,"@anchor reset_text_cursor.00\n@anchor yield.01\n@empty\n@end\n");
  assert.equal(s.context.window.localizationHasEdits(),true);assert.equal(s.requests.length,before);
});

test("content direction follows translation drafts and effective reference, never the UI locale",async()=>{
  const s=setupEditor(), view=editorMessage();
  view.referenceMetadata.direction="ltr";
  await openEditorMessage(s,view);
  assert.equal(s.node("body").getAttribute("dir"),"auto");
  assert.equal(s.node("body").getAttribute("lang"),"en-CA");
  assert.equal(s.node("reference-body").getAttribute("dir"),"ltr");
  assert.equal(s.node("reference-body").getAttribute("lang"),"en-US");
  const draft="@anchor reset_text_cursor.00\nمرحبا {master_name} 123!\n@anchor yield.01\n@end\n";
  s.node("body").value=draft;
  s.node("body").selectionStart=32;s.node("body").selectionEnd=40;
  await s.node("body").fire("input");
  const fields=s.node("metadata").elements;
  fields.locale.value="ar";fields.direction.value="rtl";
  await s.node("metadata").fire("input");
  assert.equal(s.node("body").getAttribute("dir"),"rtl");
  assert.equal(s.node("body").getAttribute("lang"),"ar");
  assert.equal(s.node("body").selectionStart,32);
  assert.equal(s.node("body").selectionEnd,40);
  s.respond((endpoint,data)=>{assert.equal(endpoint,"preview");assert.equal(data.body,draft);return [{op:"text",value:"مرحبا "},{op:"placeholder",name:"master_name"},{op:"anchor",id:"yield.01"},{op:"page"},{op:"text",value:"שלום 123"}];});
  await s.node("preview").fire("click");
  const pages=s.node("preview-content").querySelectorAll(".loc-preview-page"),before=s.requests.length;
  assert.equal(pages.length,2);
  for(const locale of ["ja","fr","de","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.requests.length,before);
    assert.equal(s.node("body").value,draft);
    assert.equal(s.node("body").getAttribute("dir"),"rtl");
    assert.equal(s.node("reference-body").getAttribute("dir"),"ltr");
    assert.equal(s.node("reference-body").getAttribute("lang"),"en-US");
    for(const page of pages){assert.equal(page.getAttribute("dir"),"rtl");assert.equal(page.getAttribute("lang"),"ar");}
    assert.equal(pages[0].querySelector(".loc-preview-value").getAttribute("dir"),"ltr");
    assert.equal(pages[0].querySelector(".loc-preview-control").getAttribute("dir"),"ltr");
  }
  // Updating direction alone must neither replace the editable node/value nor
  // rebuild its preview; auto resolves mixed scripts per paragraph in browser.
  fields.direction.value="auto";fields.locale.value="he";
  await s.node("metadata").fire("input");
  assert.equal(s.node("body").getAttribute("dir"),"auto");
  assert.equal(s.node("body").selectionEnd,40);
  assert.deepEqual(s.node("preview-content").querySelectorAll(".loc-preview-page"),pages);
  assert.equal(pages[1].getAttribute("lang"),"he");
  s.respond(endpoint=>{
    if(endpoint==="reference")return {...projectSnapshot(),reference:{id:"requested-ar"}};
    if(endpoint==="projects")return [];
    if(endpoint==="message")return {...view,reference:{body:"@anchor yield.01\nمرحبا 123"},referenceMetadata:{id:"requested-ar",name:"عربي",locale:"ar",direction:"rtl"}};
    throw Error(endpoint);
  });
  s.node("reference-project").value="requested-ar";await s.node("reference-project").fire("change");
  assert.equal(s.node("reference-body").getAttribute("dir"),"rtl");
  assert.equal(s.node("reference-body").getAttribute("lang"),"ar");
  assert.equal(s.node("body").getAttribute("lang"),"he");
  assert.equal(s.node("body").value,draft);
  assert.equal(s.node("body").selectionStart,32);
  assert.equal(s.context.window.localizationHasEdits(),true);
});

test("search labels retain raw snippets and clear old empty-result feedback",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  let rows=[];
  s.respond(endpoint=>{assert.equal(endpoint,"search");return {rows,total:rows.length};});
  const search=()=>s.node("search").fire("submit",{preventDefault(){},currentTarget:s.node("search")});
  await search();await new Promise(resolve=>setImmediate(resolve));
  s.picker.value="ja";await s.picker.fire("change");
  assert.equal(s.node("results").textContent,s.ui.text("builder.editor.no_messages"));
  rows=[{id:messageID,title:"Source <b>{count}</b>",status:"done",present:false}];
  await search();await new Promise(resolve=>setImmediate(resolve));
  const button=s.node("results").querySelector("button"),before=s.requests.length;
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.requests.length,before);assert.equal(s.node("results").querySelector("button"),button);
    assert.equal(button.title,messageID);
    assert.equal(button.textContent,"Source <b>{count}</b> · "+s.ui.text("builder.editor.done")+" · "+s.ui.text("builder.editor.native_fallback"));
    assert.equal(s.node("feedback").textContent,s.ui.text("builder.editor.search_count",{shown:1,total:1}));
  }
});

test("expanded lazy tree nodes retain their identity and selected message on a language switch",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  s.respond((endpoint,data,query)=>{
    assert.equal(endpoint,"tree");
    return query.parent===""?[{id:"place.sky",label:"Sky Palace",label_key:"root.sky",has_children:true,done:1234,total:2000}]:[{id:messageID,label:"Literal_source {done}",is_message:true,shared:true,done:0,wip:1}];
  });
  await s.node("browse").fire("click");
  const details=s.node("tree").querySelector("details");details.open=true;await details.fire("toggle");
  await new Promise(resolve=>setImmediate(resolve));
  const button=details.querySelector("button"),summary=details.querySelector("summary"),before=s.requests.length;
  assert.equal(button.getAttribute("aria-current"),"true");
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.requests.length,before);assert.equal(details.open,true);
    assert.equal(details.querySelector("button"),button);assert.equal(button.getAttribute("aria-current"),"true");
    assert.equal(summary.textContent,s.ui.text("builder.navigation.root.sky")+" · "+s.ui.text("builder.editor.tree_count",{done:1234,total:2000}));
    assert.equal(button.textContent,"Literal_source {done} "+s.ui.text("builder.navigation.shared")+" · "+s.ui.text("builder.editor.wip"));
  }
});

test("message context uses supplied navigation IDs without changing user-authored content",async()=>{
  const s=setupEditor(),view=editorMessage();
  view.location={title:"New game — before name entry",title_key:"title.before_name",context:"Shared message.",context_key:"context.start",shared:true};
  await openEditorMessage(s,view);s.node("body").value="literal_source {master_name}";await s.node("body").fire("input");
  const before=s.requests.length;
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.requests.length,before);assert.equal(s.node("message-title").textContent,s.ui.text("builder.navigation.title.before_name"));
    assert.ok(s.node("message-context").textContent.includes(s.ui.text("builder.navigation.context.start")));
    assert.equal(s.node("message-context").textContent.split(s.ui.text("builder.navigation.context.shared")).length,2);
    assert.equal(s.node("body").value,"literal_source {master_name}");
  }
});

test("publication and installation keep rights, WIP and replacement choices independent of UI locale",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  await s.node("review-publish").fire("click");
  s.node("rights").checked=true;s.node("wip").checked=true;s.node("replace-install").checked=false;
  for(const locale of ["fr","de","ja"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.node("rights").checked,true);assert.equal(s.node("wip").checked,true);assert.equal(s.node("replace-install").checked,false);
    assert.equal(s.node("review-title").textContent,s.ui.text("builder.language.review_export_title"));
  }
  s.respond((endpoint,data)=>{
    if(endpoint==="publication-check"){assert.equal(data.confirmRights,true);assert.equal(data.includeWIP,true);return {included:1234,wip:1,unchangedSource:3,fallback:5};}
    assert.equal(endpoint,"publish");assert.equal(data.confirmRights,true);assert.equal(data.includeWIP,true);assert.equal(data.projectID,packageInfo.id);
    return {name:"Édition {name}.arlang",url:"localization/download/opaque-archive"};
  });
  await s.node("check").fire("click");
  await s.node("publish").fire("click");
  const href=s.node("download").href;
  s.picker.value="de";await s.picker.fire("change");
  assert.equal(s.node("publication-report").textContent,s.ui.text("builder.language.publication_report",{included:1234,wip:1,omitted:3,fallback:5}));
  assert.equal(s.node("download").textContent,s.ui.text("builder.language.download_name",{name:"Édition {name}.arlang"}));
  assert.equal(s.node("download").href,href);assert.equal(s.node("download").download,"Édition {name}.arlang");
  s.respond((endpoint,data)=>{
    if(endpoint==="installation")return {installed:true};
    if(endpoint==="installation-check")return {messages:1234,fallback:5};
    assert.equal(endpoint,"install");assert.equal(data.replace,true);assert.equal(data.projectID,packageInfo.id);
    assert.equal(data.confirmRights,undefined);assert.equal(data.includeWIP,undefined);
    return {enabled:false,path:"/tmp/My languages/packs/edition",report:{messages:1234}};
  });
  await s.node("review-install").fire("click");
  assert.equal(s.node("publication-options").hidden,true);
  const before=s.requests.length;
  await s.node("install").fire("click"); // Replacement is still explicitly required.
  assert.equal(s.requests.length,before);assert.equal(s.node("feedback").textContent,s.ui.text("builder.language.confirm_replace"));
  s.node("replace-install").checked=true;
  s.picker.value="ja";await s.picker.fire("change");
  assert.equal(s.node("replace-install").checked,true);
  await s.node("install").fire("click");
  assert.equal(s.node("installed-title").textContent,s.ui.text("builder.language.updated_disabled"));
  assert.equal(s.node("feedback").textContent,s.ui.text("builder.language.disabled_update"));
  assert.equal(s.node("installed-steps").hidden,true);
  s.picker.value="fr";await s.picker.fire("change");
  assert.equal(s.node("installed-title").textContent,s.ui.text("builder.language.updated_disabled"));
  assert.equal(s.node("installed-steps").hidden,true);
});

test("review coverage separates contract completeness, live extras and author progress",async()=>{
  const s=setupEditor();await openEditorMessage(s);
  const coverage={runtime:true,contractComplete:true,required:{total:495,provided:495},liveOptional:{total:26,provided:0},surfaces:[{surface:"hud",total:8,provided:0,done:0,wip:0,notStarted:0,unchangedSource:0,missing:["action.hud.player_label"]}],dormant:["credits.special_mode"]};
  s.respond(endpoint=>endpoint==="installation"?{installed:false}:{messages:495,fallback:27,coverage});
  await s.node("review-install").fire("click");
  const host=s.node("text-coverage");assert.equal(host.hidden,false);
  const details=host.querySelector("details");assert.equal(details.hasAttribute("open"),false);
  const ids=details.querySelector("pre");assert.equal(ids.textContent,"action.hud.player_label");assert.equal(ids.getAttribute("dir"),"ltr");
  for(const locale of ["en","fr","de","ja"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.ok(host.textContent.includes(s.ui.text("builder.coverage.contract",{provided:495,total:495})));
    assert.ok(host.textContent.includes(s.ui.text("builder.coverage.live_optional",{provided:0,total:26})));
    assert.equal(ids.textContent,"action.hud.player_label");
  }
  await s.node("review-publish").fire("click");assert.equal(host.hidden,true);
  s.respond(()=>({included:495,wip:0,unchangedSource:0,fallback:27,coverage}));
  await s.node("check").fire("click");assert.equal(host.hidden,false);
  s.node("wip").checked=true;await s.node("wip").fire("change");assert.equal(host.hidden,true);
});

test("a completed in-flight editor request cannot re-enable a closed workshop",async()=>{
  for(const stage of ["headers","body"]){
    const s=setupEditor();await openEditorMessage(s);
    let finish,first=true;
    s.response(()=>{
      if(first){
        first=false;
        const pending=new Promise(resolve=>{finish=resolve;});
        return stage==="headers"?pending:Promise.resolve({ok:true,json:()=>pending});
      }
      return Promise.resolve({ok:true,json:async()=>({messages:1,fallback:0})});
    });
    const review=s.node("review-install").fire("click");
    await new Promise(resolve=>setImmediate(resolve));
    await s.doc.fire("workshop:closed");
    finish(stage==="headers"?{ok:true,json:async()=>({installed:false})}:{installed:false});
    await review;
    assert.equal(s.node("review-install").disabled,true);
    const requests=s.requests.length;
    await s.context.window.localizationActivate();
    assert.equal(s.requests.length,requests);
  }
});

test("font roles save together and removing one preserves the body stack",async()=>{
 const s=setupEditor();await openEditorMessage(s);
 s.node("font-role-name").value="hud";await s.node("font-role-add").fire("click");
 assert.equal(s.node("font-role").value,"hud");
 let saved;
 s.response(async(endpoint,data)=>{assert.equal(endpoint,"save");saved=data;return {ok:false,status:409,json:async()=>({error:"fixture conflict"})};});
 await s.node("save-progress").fire("click");
 assert.equal(saved.saveFonts,true);assert.equal(saved.fonts.primary,"builtin:actraiser-sans");
 assert.equal(saved.fonts.roles[0].name,"hud");
 s.node("font-role-name").value="constructor";await s.node("font-role-add").fire("click");
 assert.equal(s.node("font-role").value,"constructor");
 await s.node("font-role-remove").fire("click");
 assert.equal(s.node("font-role").value,"body");
 await s.node("save-progress").fire("click");
 assert.equal(saved.fonts.roles.length,1);assert.equal(saved.fonts.roles[0].name,"hud");
});

test("v1 upgrade adopts a separate v2 project and hides the upgrade action",async()=>{
 const s=setupEditor();
 const old=projectSnapshot();old.project.formatVersion=1;
 const upgraded=projectSnapshot();upgraded.project.formatVersion=2;upgraded.project.metadata.id="upgraded.copy";
 let received;
 s.respond((endpoint,data)=>{
   if(endpoint==="open") return old;
   if(endpoint==="upgrade-v2") {received=data;return {state:upgraded,report:{messages:2,aliasesMaterialized:1}};}
   if(["projects","catalog","tree"].includes(endpoint)) return [];
   if(endpoint==="installation") return {installed:false};
   throw Error("unexpected endpoint "+endpoint);
 });
 await s.context.window.localizationOpenProject(packageInfo.id);
 assert.equal(s.node("upgrade-v2").hidden,false);
 s.node("upgrade-v2").elements.newID.value="upgraded.copy";
 await s.node("upgrade-v2").fire("submit",{preventDefault(){},currentTarget:s.node("upgrade-v2")});
 assert.equal(received.projectID,packageInfo.id);assert.equal(received.newID,"upgraded.copy");
 assert.equal(s.node("upgrade-v2").hidden,true);
 assert.ok(s.node("feedback").textContent.includes("2 messages"));
});


test("v1 import shows an upgrade notice before installation, not when only editing",async()=>{
  for(const version of [1,2]){
    const upgrade=version===1?{fromVersion:1,toVersion:2}:undefined;
    const s=setupEditor();importResponder(s,{...sharedPreview,upgrade});
    await s.doc.fire("drop",dragEvent(s));
    assert.equal(s.node("import-preview").hidden,false);
    assert.equal(s.node("import-upgrade").hidden,version!==1);
    assert.ok(!s.requests.some(r=>["accept-import","install"].includes(r.endpoint)));
    for(const locale of ["fr","de","ja","en"]){
      s.picker.value=locale;await s.picker.fire("change");
      assert.equal(s.node("import-upgrade").textContent,s.ui.text("builder.language.install_upgrade"));
      assert.equal(s.node("import-upgrade").hidden,version!==1);
    }
    s.respond(endpoint=>{
      if(endpoint==="accept-import")return projectSnapshot(version);
      if(endpoint==="projects"||endpoint==="tree"||endpoint==="catalog")return [];
      if(endpoint==="installation")return {installed:false};
      if(endpoint==="install")return {enabled:true,path:"/game/languages/packs/edition",report:{messages:8,upgrade}};
      throw Error(endpoint);
    });
    await s.node("accept-import").fire("click");
    assert.equal(s.node("installed").hidden,false);
    assert.equal(s.node("installed-upgrade").hidden,version!==1);

    const editing=setupEditor();importResponder(editing,{...sharedPreview,upgrade});
    await editing.context.window.localizationActivate();
    await editing.root.querySelector('[data-loc-flow="edit"]').fire("click");
    const input=editing.node("import-backup").elements.file;input.files=[{name:"work.arproject",size:100}];
    await input.fire("change");
    assert.equal(editing.node("import-preview").hidden,false);
    assert.equal(editing.node("import-upgrade").hidden,true);
  }
});

test("saved v1 installation review explains upgrade and completion confirms it",async()=>{
  for(const version of [1,2]){
    const upgrade=version===1?{fromVersion:1,toVersion:2}:undefined;
    const s=setupEditor();await openEditorMessage(s,editorMessage(),false,projectSnapshot(version));
    await s.node("review-publish").fire("click");
    assert.equal(s.node("install-upgrade").hidden,true);
    s.respond(endpoint=>{
      if(endpoint==="installation")return {installed:false};
      if(endpoint==="installation-check")return {messages:8,fallback:5,upgrade};
      if(endpoint==="install")return {enabled:true,path:"/game/languages/packs/edition",report:{messages:8,upgrade}};
      throw Error(endpoint);
    });
    await s.node("review-install").fire("click");
    assert.equal(s.node("sharing").hidden,false);
    assert.equal(s.node("install-upgrade").hidden,version!==1);
    assert.equal(s.node("upgrade-v2").hidden,true);
    assert.ok(!s.requests.some(r=>r.endpoint==="install"));
    for(const locale of ["fr","de","ja","en"]){
      s.picker.value=locale;await s.picker.fire("change");
      assert.equal(s.node("install-upgrade").textContent,s.ui.text("builder.language.install_upgrade"));
    }
    await s.node("install").fire("click");
    assert.equal(s.node("installed").hidden,false);
    assert.equal(s.node("installed-upgrade").hidden,version!==1);
    s.picker.value="ja";await s.picker.fire("change");
    assert.equal(s.node("installed-upgrade").textContent,s.ui.text("builder.language.installed_upgrade"));
  }
});

test("inline style toolbar wraps selections, inserts at the caret, and preserves locale state",async()=>{
 const s=setupEditor(),project=projectSnapshot();
 project.project.fonts.roles=[{name:"hud",primary:"builtin:actraiser-sans",fallback:[]}];
 project.project.treatments=[{definition:{name:"retail"}},{definition:{name:"hud"}}];
 await openEditorMessage(s,editorMessage(),true,project);
 const input=s.node("body");
 s.node("style-font").value="hud";s.node("style-treatment").value="hud";
 s.node("style-color").value="#ff8040";s.node("style-scale").value="80";
 const cases=[['italic','<i>','</i>'],['upright','<span italic="false">','</span>'],
  ['font','<span font="hud">','</span>'],['style','<span style="hud">','</span>'],
  ['color','<span color="#FF8040">','</span>'],['scale','<span scale="80%">','</span>']];
 const before=s.requests.length;
 for(const [kind,open,close] of cases){
  input.value="Hello {master_name}!";input.setSelectionRange(6,19);
  s.doc.activeElement=s.node("style-font");
  await s.root.querySelector(`[data-loc-style="${kind}"]`).fire("click");
  assert.equal(input.value,"Hello "+open+"{master_name}"+close+"!");
  assert.equal(input.value.slice(input.selectionStart,input.selectionEnd),"{master_name}");
  assert.equal(s.doc.activeElement,input);assert.equal(s.context.window.localizationHasEdits(),true);
  input.value="Hello";input.setSelectionRange(2,2);
  await s.root.querySelector(`[data-loc-style="${kind}"]`).fire("click");
  assert.equal(input.value,"He"+open+close+"llo");assert.equal(input.selectionStart,2+open.length);
 }
 assert.equal(s.requests.length,before);
 const text=input.value,start=input.selectionStart;
 for(const locale of ["fr","de","ja","en"]){
  s.picker.value=locale;await s.picker.fire("change");
  assert.equal(input.value,text);assert.equal(input.selectionStart,start);
  assert.equal(s.node("style-font").value,"hud");assert.equal(s.node("style-treatment").value,"hud");
  assert.equal(s.node("style-scale").value,"80");assert.equal(s.node("style-color").value,"#ff8040");
  assert.equal(s.root.querySelector('[data-loc-style="upright"]').textContent,s.ui.text("builder.styling.upright"));
 }
});

test("inline styling respects format, source, alias and inherited-term restrictions",async()=>{
 for(const kind of ["v1","source","alias","term"]){
  const s=setupEditor(),project=projectSnapshot(kind==="v1"?1:2),view=editorMessage();
  if(kind==="source")project.project.origin="native-source";
  if(kind==="alias")view.message.body="@alias city.fillmore.name\n";
  if(kind==="term")view.message.reference.presentation={shape:"inline"};
  await openEditorMessage(s,view,true,project);
  const before=s.node("body").value,button=s.root.querySelector('[data-loc-style="italic"]');
  assert.equal(button.disabled,true,kind);await button.fire("click");assert.equal(s.node("body").value,before);
  assert.equal(s.node("style-color").disabled,true,kind);
  assert.ok(s.node("style-status").textContent);
 }
});

test("invalid styling leaves drafts untouched; unsaved font roles are available",async()=>{
 const s=setupEditor();await openEditorMessage(s);
 const button=kind=>s.root.querySelector(`[data-loc-style="${kind}"]`),input=s.node("body");
 assert.equal(button("style").disabled,true);assert.equal(button("font").disabled,false);
 input.value="Dude {master_name}";input.setSelectionRange(6,8);
 await button("italic").fire("click");assert.equal(input.value,"Dude {master_name}");
 assert.equal(s.node("style-status").textContent,s.ui.text("builder.styling.selection"));
 input.setSelectionRange(0,4);
 for(const value of ["24","401","99.5",""]){
  s.node("style-scale").value=value;await button("scale").fire("click");
  assert.equal(input.value,"Dude {master_name}");assert.equal(s.node("style-status").textContent,s.ui.text("builder.styling.invalid_size"));
 }
 s.node("font-role-name").value="quiet";await s.node("font-role-add").fire("click");
 assert.ok(s.node("style-font").options.some(o=>o.value==="quiet"));
 s.node("style-font").value="quiet";await button("font").fire("click");
 assert.equal(input.value,'<span font="quiet">Dude</span> {master_name}');
 await s.node("font-role-remove").fire("click");
 assert.equal(s.node("style-font").value,"body");
 assert.ok(!s.node("style-font").options.some(o=>o.value==="quiet"));
});

test("failed font check preserves imported state, specific code and recovery without a second check",async()=>{
  const s=setupEditor();importResponder(s);await s.doc.fire("drop",dragEvent(s));
  const detail="invalid game font-check response: record 2";
  s.response(async endpoint=>{
    if(endpoint==="accept-import")return {ok:true,json:async()=>projectSnapshot()};
    if(endpoint==="projects"||endpoint==="tree")return {ok:true,json:async()=>[]};
    if(endpoint==="installation")return {ok:true,json:async()=>({installed:false})};
    if(endpoint==="install")return {ok:false,status:500,json:async()=>({error:detail,errorCode:"builder.errors.font_protocol",recoveryKey:"builder.recovery.font_protocol"})};
    throw Error("unexpected recovery request: "+endpoint);
  });
  await s.node("accept-import").fire("click");
  assert.equal(s.node("feedback").textContent,s.ui.text("builder.language.import_partial",{detail:s.ui.text("builder.errors.font_protocol")}));
  assert.equal(s.requests.filter(r=>r.endpoint==="install").length,1);
  assert.equal(s.requests.filter(r=>r.endpoint==="installation-check").length,0);
  const report=s.root.querySelector(".workshop-feedback textarea").value;
  assert.match(report,/Code: builder.errors.font_protocol/);assert.match(report,/Operation: Install language pack/);
  assert.match(report,/Outcome: Project imported into Workshop/);assert.ok(report.includes(detail));
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.root.querySelector(".workshop-feedback p").textContent,s.ui.text("builder.recovery.font_protocol"));
  }
  assert.equal(s.node("install").disabled,false);
});

test("language errors retain server codes and malformed responses get a response code",async()=>{
  for(const malformed of [false,true]){
    const s=setupEditor();
    s.response(async()=>({ok:false,status:409,json:async()=>{
      if(malformed)throw new SyntaxError("Unexpected HTML");
      return {error:"Project revision changed",errorCode:"builder.language.request_conflict"};
    }}));
    await s.context.window.localizationActivate();
    const report=s.root.querySelector(".workshop-feedback textarea").value;
    assert.match(report,malformed?/Code: AR_RESPONSE/:/Code: builder.language.request_conflict/);
    assert.match(report,/Operation: Read language workspace/);
    assert.doesNotMatch(report,/Unexpected HTML/);
  }
});

test("connection loss after import reports an uncertain installation without retrying",async()=>{
  const s=setupEditor();importResponder(s);await s.doc.fire("drop",dragEvent(s));
  s.response(async endpoint=>{
    if(endpoint==="accept-import")return {ok:true,json:async()=>projectSnapshot()};
    if(endpoint==="projects"||endpoint==="tree")return {ok:true,json:async()=>[]};
    if(endpoint==="installation")return {ok:true,json:async()=>({installed:false})};
    if(endpoint==="install")throw new TypeError("Failed to fetch");
    throw Error("unexpected endpoint "+endpoint);
  });
  await s.node("accept-import").fire("click");
  assert.match(s.node("feedback").textContent,/Installation could not be confirmed/);
  assert.equal(s.requests.filter(r=>r.endpoint==="install").length,1);
  const report=s.root.querySelector(".workshop-feedback textarea").value;
  assert.match(report,/Code: AR_NETWORK/);assert.match(report,/Outcome: .*could not be confirmed/);
  assert.equal(s.node("install").disabled,false);
});

test("installation conflicts retain their original diagnostic when refreshing choices fails",async()=>{
  const s=setupEditor();importResponder(s);await s.doc.fire("drop",dragEvent(s));
  let reads=0;
  s.response(async endpoint=>{
    if(endpoint==="accept-import")return {ok:true,json:async()=>projectSnapshot()};
    if(endpoint==="projects"||endpoint==="tree")return {ok:true,json:async()=>[]};
    if(endpoint==="installation"){
      if(++reads>1)throw new TypeError("status refresh lost connection");
      return {ok:true,json:async()=>({installed:false})};
    }
    if(endpoint==="install")return {ok:false,status:409,json:async()=>({error:"Original installation conflict",errorCode:"builder.language.request_conflict",recoveryKey:"builder.recovery.conflict"})};
    throw Error("unexpected endpoint "+endpoint);
  });
  await s.node("accept-import").fire("click");
  const report=s.root.querySelector(".workshop-feedback textarea").value;
  assert.match(report,/Original installation conflict/);assert.match(report,/Code: builder.language.request_conflict/);
  assert.doesNotMatch(report,/lost connection/);
  assert.equal(s.requests.filter(r=>r.endpoint==="installation-check").length,0);
});
