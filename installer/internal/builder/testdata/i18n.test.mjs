import {test} from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";
import {Node, setup, messages} from "./interface_dom.mjs";

test("initial language readiness copy is bound without waiting for a status response",()=>{
  const html=readFileSync(new URL("../web/index.html",import.meta.url),"utf8");
  const tags=[...html.matchAll(/<button[^>]*id="tab-localization"[^>]*>|<p[^>]*data-language-status[^>]*>/g)].map(m=>m[0]);
  assert.equal(tags.length,3);
  for(const locale of ["en","fr","de","ja"]){
    const s=setup(locale);
    for(const tag of tags){
      const attrs=Object.fromEntries([...tag.matchAll(/([a-z0-9-]+)="([^"]*)"/g)].map(m=>[m[1],m[2]]));
      const node=new Node(tag.startsWith("<button")?"button":"p","untranslated startup",attrs);
      s.doc.children.push(node);s.ui.apply(node);
      if(tag.includes("tab-localization"))assert.equal(node.getAttribute("title"),s.ui.text("builder.language.unavailable_title"));
      else assert.equal(node.textContent,s.ui.text("builder.language.source_checking"));
    }
    assert.equal(s.requests.length,0); // Status fetch may stall or fail entirely.
  }
});

test("language changes only explicitly bound UI and preserves author state",async()=>{
  const s=setup();const selectedFiles=s.files.files;
  s.picker.value="fr";await s.picker.fire("change");
  assert.equal(s.ui.locale,"fr");assert.equal(s.doc.documentElement.lang,"fr");
  assert.equal(s.label.textContent,"Enregistrer");assert.equal(s.attr.getAttribute("placeholder"),"Accueil");
  assert.equal(s.input.value,"Sir ÉLISE — {name}");assert.equal(s.textarea.value,"Unsaved 日本語");
  assert.equal(s.textarea.textContent,"Native {name}");assert.equal(s.localeField.value,"en-CA");
  assert.equal(s.doc.activeElement,s.textarea);assert.equal(s.textarea.selectionStart,4);assert.equal(s.textarea.selectionEnd,9);
  assert.equal(s.files.files,selectedFiles);assert.equal(s.parent.children.length,2);assert.equal(s.parent.children[0].textContent,"✦");
  assert.equal(s.raw.textContent,"common.save");assert.equal(s.attr.value,"Leave me alone");
  assert.equal(s.requests.length,1);assert.equal(s.requests[0].path,"interface/preferences");
  assert.deepEqual(JSON.parse(s.requests[0].options.body),{language:"fr"});
  assert.equal(s.picker.disabled,false);
});

test("dynamic bindings rerender literal named arguments, never HTML or nested templates",async()=>{
  const s=setup("de");const dynamic=new Node("small");s.doc.children.push(dynamic);
  const id="<img onerror=evil()> {section} & %s";
  s.ui.set(dynamic,"builder.home.project_copy",{id});
  assert.equal(dynamic.textContent,id+" · Arbeitskopie");
  s.picker.value="ja";await s.picker.fire("change");
  assert.equal(dynamic.textContent,id+"・作業用コピー");assert.equal(dynamic.children.length,0);
  assert.throws(()=>s.ui.set(s.input,"common.save"),/text leaf/);
  assert.throws(()=>s.ui.set(s.parent,"common.save"),/text leaf/);
  s.ui.unbind(dynamic);dynamic.textContent="Native US";
  s.ui.apply();assert.equal(dynamic.textContent,"Native US");
});

test("failed saves keep the old locale; a second change cannot race the pending one",async()=>{
  const s=setup("fr");let finish;
  s.setResponder(()=>new Promise(resolve=>{finish=resolve;}));
  s.picker.value="de";const saving=s.picker.fire("change");
  assert.equal(s.picker.disabled,true);assert.equal(s.ui.locale,"fr");
  assert.equal(s.status.className,"sr-only");
  s.picker.value="ja";await s.picker.fire("change");assert.equal(s.requests.length,1);
  finish({ok:false,json:async()=>({errorCode:"builder.interface.save_failed"})});await saving;
  assert.equal(s.ui.locale,"fr");assert.equal(s.picker.value,"fr");assert.equal(s.picker.disabled,false);
  assert.equal(s.status.textContent,messages["builder.interface.save_failed"][1]);
  assert.equal(s.status.hidden,false);assert.notEqual(s.status.className,"sr-only");
});

test("Big Screen shares the Builder language preference and can retry a failed save",async()=>{
  const s=setup("fr",{bigScreen:true});
  const get=id=>s.doc.getElementById("big-screen-language"+id);
  const [english,french,german,japanese]=get("-options").children.flatMap(row=>row.children);
  const dialog=get("-dialog"), label=get("-name");
  await get("").fire("click");
  assert.equal(dialog.open,true);
  assert.equal(s.doc.activeElement,french);
  assert.equal(label.textContent,"Français");
  assert.equal(s.requests.length,0);

  s.setResponder(async()=>({ok:false,json:async()=>({errorCode:"builder.interface.save_failed"})}));
  await german.fire("click");
  assert.equal(dialog.open,true);
  assert.equal(get("-status").textContent,s.ui.text("builder.interface.save_failed"));
  assert.equal(french.getAttribute("aria-pressed"),"true");

  s.setResponder(async options=>({ok:true,json:async()=>JSON.parse(options.body)}));
  await german.fire("click");
  assert.equal(dialog.open,false);
  assert.equal(label.textContent,"Deutsch");
  assert.equal(s.picker.value,"de");
  assert.equal(s.localeField.value,"en-CA");

  s.picker.value="ja";await s.picker.fire("change");
  assert.equal(label.textContent,"日本語");
  assert.equal(english.getAttribute("aria-pressed"),"false");
  assert.equal(japanese.getAttribute("aria-pressed"),"true");
});

test("an in-flight preference response cannot reopen a closed workshop",async()=>{
  const s=setup();let finish;s.setResponder(()=>new Promise(resolve=>{finish=resolve;}));
  s.picker.value="ja";const saving=s.picker.fire("change");
  await s.doc.fire("workshop:closed");finish({ok:true,json:async()=>({language:"ja"})});await saving;
  assert.equal(s.picker.disabled,true);assert.equal(s.ui.locale,"en");
  s.picker.value="fr";await s.picker.fire("change");assert.equal(s.requests.length,1);
});

test("asset numeric arguments reformat but filenames, IDs and media resources stay literal",async()=>{
  const s=setup();
  const count=new Node("span"),name=new Node("span"),sourceLabel=new Node("span");
  const media=new Node("audio","",{src:"blob:chosen-track"});media.currentTime=12.5;media.paused=false;
  const cover=new Node("img","",{src:"boxart.webp","data-i18n-alt":"builder.help.box_art"});
  const booklet=new Node("iframe","",{src:"manual.pdf#page=7","data-i18n-title":"builder.help.manual_title"});
  s.doc.children.push(count,name,sourceLabel,media,cover,booklet);
  s.ui.set(count,"builder.assets.unsaved",{count:1234});
  const file="1234 <script> {count} Français.ogg";
  s.ui.set(name,"builder.assets.selected_file",{file});
  s.ui.set(sourceLabel,"builder.assets.source",{id:"song-09",source:"0E:F69F"});
  for(const locale of ["en","fr","de","ja"]){
    s.picker.value=locale;await s.picker.fire("change");
    const column=["en","fr","de","ja"].indexOf(locale);
    assert.equal(count.textContent,messages["builder.assets.unsaved"][column].replace("{count}",new Intl.NumberFormat(locale).format(1234)));
    assert.ok(name.textContent.includes(file));
    assert.ok(sourceLabel.textContent.includes("song-09"));
    assert.ok(sourceLabel.textContent.includes("0E:F69F"));
    assert.equal(cover.getAttribute("alt"),messages["builder.help.box_art"][column]);
    assert.equal(cover.getAttribute("src"),"boxart.webp");
    assert.equal(booklet.getAttribute("title"),messages["builder.help.manual_title"][column]);
    assert.equal(booklet.getAttribute("src"),"manual.pdf#page=7");
    assert.equal(media.getAttribute("src"),"blob:chosen-track");
    assert.equal(media.currentTime,12.5);assert.equal(media.paused,false);
  }
});

test("choosing an MP3 or renamed MP3 explains how to fix the audio format",async()=>{
  const s=setup(), errors=[];
  s.context.window.workshopFeedback={clear(){},show(target,error,options){errors.push({target,error,options});}};
  runInNewContext(readFileSync(new URL("../web/file-input.js",import.meta.url),"utf8"),s.context);
  for(const name of ["replacement.mp3","renamed.ogg"]){
    const input={
      type:"file",accept:".ogg,.oga,audio/ogg",files:[new File(["ID3 fixture"],name)],
      setCustomValidity(message){this.validation=message;},
      set value(value){assert.equal(value,"");this.files=[];},
      dispatchEvent(){},
    };
    await s.doc.fire("change",{target:input,stopImmediatePropagation(){}});
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(input.files.length,0);
    assert.equal(input.validation,"");
    const {target,error,options}=errors.at(-1);
    assert.equal(target,input);
    assert.match(error.message,/Music replacements need Ogg Vorbis audio/);
    assert.ok(error.message.includes("Convert "+name+" to Ogg Vorbis"));
    assert.equal(options.messageKey,"builder.file.invalid_vorbis");
  }
});

test("pack publishing keeps metadata and regional selections per draft through close and discard", async()=>{
  const {doc,context}=setup(), nodes=new Map();
  doc.getElementById=id=>{
    if(!nodes.has(id)){
      const node=new Node("div");
      node.files=[];
      node.replaceChildren=()=>{node.children=[];};
      nodes.set(id,node);
    }
    return nodes.get(id);
  };
  const get=doc.getElementById;
  doc.createElement=tag=>{
    const node=new Node(tag);node.append=(...children)=>node.children.push(...children);return node;
  };
  doc.createTextNode=text=>text;
  const selections=get("asset-pack-export-tracks");
  selections.append=node=>selections.children.push(node);
  selections.querySelectorAll=selector=>selections.children.map(label=>label.children[0])
    .filter(input=>selector==="input"||input.checked);
  const fields=[{name:"name",defaultValue:""},{name:"version",defaultValue:"1.0.0"}].map(field=>({...field,value:field.defaultValue}));
  get("asset-pack-export-form").querySelectorAll=()=>fields;
  const dialog=get("asset-pack-export");
  dialog.showModal=()=>{dialog.open=true;};
  dialog.close=()=>{dialog.open=false;};
  context.window.workshopModPacks={
    titleType:"title", manualType:"manual", musicType:"music", targetName:id=>id,
    jsonOptions:body=>({method:"POST",body:JSON.stringify(body)}),
  };
  runInNewContext(readFileSync(new URL("../web/workshop-publishing.js",import.meta.url),"utf8"),context);
  const tracks=[{id:"song-09",track:"song-09"},{id:"song-09-fillmore",track:"song-09"},{id:"song-11",track:"song-11"}];
  let rejectExport;
  const publishing=context.window.workshopPublishing(async(_path,options)=>options.method==="POST"
    ? new Promise((_resolve,reject)=>{rejectExport=reject;}) : tracks);
  const open=async action=>{dialog.close();await action();return fields.map(field=>field.value);};
  const selected=()=>selections.querySelectorAll("input:checked").map(box=>box.value);
  await publishing.openMusic("song-09");
  assert.deepEqual(selected(),["song-09","song-09-fillmore"]);
  assert.equal(publishing.hasEdits(),false,"the initial selection is a default");
  for(const box of selections.querySelectorAll("input"))box.checked=false;
  await selections.fire("change");
  assert.equal(publishing.hasEdits(),true,"clearing every target is an edit");
  await open(()=>publishing.openMusic("song-11"));
  assert.deepEqual(selected(),["song-11"]);
  await open(()=>publishing.openMusic("song-09"));
  assert.deepEqual(selected(),[]);
  context.window.workshopConfirm=async()=>false;
  await get("asset-pack-export-discard").fire("click");
  assert.equal(dialog.open,true);
  context.window.workshopConfirm=async()=>true;
  await get("asset-pack-export-discard").fire("click");
  assert.equal(dialog.open,false);
  assert.equal(publishing.hasEdits(),false);
  await publishing.openTitle();
  assert.equal(publishing.hasEdits(),false);
  fields[0].value="Title artwork";
  fields[1].value="2.1.0";
  assert.deepEqual(await open(publishing.openManual),["","1.0.0"]);
  assert.equal(publishing.hasEdits(),true,"a canceled artwork draft still contains unsaved work");
  fields[0].value="Manual";
  assert.deepEqual(await open(publishing.openTitle),["Title artwork","2.1.0"]);
  assert.deepEqual(await open(publishing.openManual),["Manual","1.0.0"]);
  assert.deepEqual(await open(()=>publishing.openMusic("song-09")),["","1.0.0"]);
  fields[0].value="First track";
  assert.deepEqual(await open(()=>publishing.openMusic("song-11")),["","1.0.0"]);
  assert.deepEqual(await open(()=>publishing.openMusic("song-09")),["First track","1.0.0"]);
  context.FormData=class extends Map {
    constructor(){super([["name",fields[0].value]]);}
    getAll(){return selected();}
  };
  get("asset-pack-export-status").classList=new Set();
  get("asset-pack-export-status").classList.remove=Set.prototype.delete;
  const submit=get("asset-pack-export-submit");
  const pending=get("asset-pack-export-form").fire("submit",{preventDefault(){},submitter:submit});
  assert.equal(submit.textContent,"Exporting…");
  assert.ok(fields.every(field=>field.readOnly));
  assert.equal(get("asset-pack-export-music").disabled,true);
  rejectExport(new Error("Archive write failed"));
  await pending;
  assert.equal(dialog.open,true);
  assert.ok(fields.every(field=>!field.readOnly));
  assert.equal(fields[0].value,"First track");
  assert.deepEqual(selected(),["song-09","song-09-fillmore"]);
  assert.equal(submit.disabled,false);

  publishing.clearMusicDetails();
  const source={name:"Imported songs",version:"1.4.1",excluded:["song-11"]};
  tracks.push({id:"song-02",track:"song-02"});
  assert.deepEqual(await open(()=>publishing.openMusic("song-11",source)),["Imported songs","1.4.1"]);
  assert.deepEqual(selected(),["song-09","song-09-fillmore","song-02"]);
  assert.equal(publishing.hasMusicDetails(),false,"reopening a pack is clean");
  fields[1].value="2.0.0";
  assert.deepEqual(await open(()=>publishing.openMusic("song-02",source)),["Imported songs","2.0.0"]);
});

// Run the actual application alongside i18n, with a deliberately small DOM.
// No copied asset state machine and no browser/server dependency. Unsupported
// row selectors fail loudly so this fixture cannot silently hide new behavior.
function setupAssetApp(tracks=[["song-09","Track 09"],["song-01","Sky Palace"]],{desktopShell=false}={}){
  const s=setup(), ids=new Map(),rows=[];
  const node=id=>{
    if(!ids.has(id)){const n=new Node("div");n.id=id;n.files=[];n.focus=()=>{s.doc.activeElement=n;};n.closest=()=>null;ids.set(id,n);s.doc.children.push(n);}
    return ids.get(id);
  };
  for(const [index,[id,name]] of tracks.entries()){
    const row=new Node("div","",{"data-track":id,"data-source":index===0?"0E:F69F":"1C:A988"});
    const file=new Node("input");file.files=[];file.name="track-"+id;
    const remove=new Node("input");remove.value="0";
    const split=new Node("input");split.value="0";
    const regionChoice=new Node("input");regionChoice.name="split-"+id;regionChoice.value="fillmore";regionChoice.checked=false;
    const audio=new Node("audio");audio.paused=true;audio.currentTime=0;audio.pauses=0;audio.loads=0;
    audio.pause=()=>{audio.paused=true;audio.pauses++;};audio.load=()=>{audio.loads++;};
    const current=new Node("span"),clear=new Node("button"),label=new Node("label",name);
    const selectors={"input[type=file]":file,".asset-remove":remove,".split-change":split,
      ".replacement-audio":audio,".replacement-caption":new Node("span"),".asset-current":current,
      ".asset-clear":clear,".asset-copy label":label,".asset-source":new Node("span"),".split-toggle":null};
    row.children=[...Object.values(selectors).filter(Boolean),regionChoice];
    row.querySelector=selector=>{assert.ok(Object.hasOwn(selectors,selector),`unexpected row selector ${selector}`);return selectors[selector];};
    const bound=row.querySelectorAll.bind(row);
    row.querySelectorAll=selector=>selector==="audio"?[audio]:bound(selector);
    rows.push(row);s.doc.children.push(row);
    Object.assign(row,{file,remove,split,regionChoice,audio,current,clear,label});
  }
  const list=node("asset-track-list");
  const pickerHost=new Node("div"),tabsHost=new Node("div"),header=new Node("header");header.offsetHeight=70;
  const pickerNodes=new Set([pickerHost,list,...["asset-track-toggle","asset-track-options","asset-search"].map(node)]);
  const inPicker=n=>{n.closest=selector=>selector===".asset-track-picker"?pickerHost:null;pickerNodes.add(n);};
  pickerNodes.forEach(inPicker);pickerHost.contains=n=>pickerNodes.has(n);
  list.append=child=>{inPicker(child);list.children.push(child);};
  node("asset-track-options").hidden=true;
  node("build-view").hidden=true;
  node("home-projects").replaceChildren=()=>{};
  const tabs=[node("tab-home"),node("tab-build"),node("tab-progress")];
  tabs.forEach(tab=>tab.setAttribute("aria-controls",tab.id.replace("tab-","panel-")));
  node("tab-progress").setAttribute("aria-controls","build-view");
  node("tab-progress").hidden=true;
  for(const id of ["big-screen-rom-checklist","build-rom-checklist"]){
    const host=node(id);
    host.append=child=>{
      if(child.parentNode)child.parentNode.children=child.parentNode.children.filter(item=>item!==child);
      host.children.push(child);child.parentNode=host;
    };
  }
  node("build-rom-checklist").append(node("rom-checklist"));
  const originalQuery=s.doc.querySelectorAll.bind(s.doc);
  s.doc.querySelectorAll=selector=>{
    if(selector===".asset-row")return rows;
    if(selector===".tab")return tabs;
    if(selector==="dialog[open]")return openDialog?.open?[openDialog]:[];
    if([".step","[data-nav]","[data-asset-category]","[data-language-status]",'[data-nav="localization"]'].includes(selector))return [];
    return originalQuery(selector);
  };
  let openDialog;
  s.doc.querySelector=selector=>{
    if(selector.startsWith("#"))return node(selector.slice(1));
    if(selector==="dialog[open]")return openDialog||null;
    return {".tabs":tabsHost,".workspace-bar":header,".asset-track-picker":pickerHost}[selector]??assert.fail(`unexpected document selector ${selector}`);
  };
  s.doc.createElement=tag=>{const n=new Node(tag);n.focus=()=>{s.doc.activeElement=n;};n.click=()=>n.fire("click");
    n.append=(...children)=>n.children.push(...children);
    Object.defineProperty(n,"firstElementChild",{get:()=>n.children[0]});
    return n;};
  s.doc.body=new Node("body");
  if(desktopShell)s.doc.body.dataset.desktopShell="true";
  let closeRequests;
  s.context.EventSource=class extends Node {
    constructor(path){
      super("event-source");
      assert.equal(path,"__shell/close-requests");
      closeRequests=this;
    }
    close(){this.closed=true;}
  };
  s.doc.documentElement.style={setProperty(){}};
  s.context.window.addEventListener=()=>{};
  s.context.window.scrollTo=()=>{};
  s.context.window.history={pushState:(_,__,hash)=>{s.context.location.hash=hash;}};
  const staleActions=new Node("div");
  node("stale-build-dialog").querySelector=selector=>{
    assert.equal(selector,".dialog-actions");
    return staleActions;
  };
  // These tests exercise the desktop UI with its existing asset controls.
  // Big Screen input and requests are exercised separately.
  s.context.window.workshopBigScreenCustomize=()=>({});
  s.context.window.workshopBigScreenNavigation=()=>{};
  s.context.window.workshopFileBrowser=()=>({rom(){},mod(){}});
  let selecting=false, reportROMUpload;
  const droppedROMs=[];
  s.context.window.workshopROMChecklist=(_request,_browse,{onSelecting})=>{
    reportROMUpload=value=>{selecting=value;onSelecting();};
    return {load(){},availability(){},isSelecting:()=>selecting,async upload(files){droppedROMs.push(...files);}};
  };
  const requests=[], urls=[];
  Object.assign(s.context,{location:{hash:"#unknown"},matchMedia:()=>({matches:false,addEventListener(){}}),
    URL:{createObjectURL(file){urls.push(file);return "blob:pending-track";},revokeObjectURL(){throw Error("unexpected file release");}},
    // Initial status poll is left in flight. Switching UI language must not
    // trigger a configuration reload or a second asset request.
    fetch:(path,options)=>{
      requests.push(path);
      return path==="interface/preferences"?Promise.resolve({ok:true,json:async()=>JSON.parse(options.body)}):new Promise(()=>{});
    }});
  // Expose the actual private renderer only inside this isolated test context.
  // Production keeps its IIFE; no test hooks or duplicated UI logic ship.
  const getElementById = s.doc.getElementById;
  s.doc.getElementById = id => id.startsWith("asset-pack") || id === "builder-drop-hint" ? node(id) : getElementById(id);
  node("asset-pack-audio").pause = () => {};
  runInNewContext(readFileSync(new URL("../web/mod-packs.js",import.meta.url),"utf8"),s.context);
  runInNewContext(readFileSync(new URL("../web/workshop-publishing.js",import.meta.url),"utf8"),s.context);
  const source=readFileSync(new URL("../web/app.js",import.meta.url),"utf8");
  assert.match(source,/\}\)\(\);\s*$/);
  runInNewContext(source.replace(/\}\)\(\);\s*$/,"window.testApplyMode=applyMode;\nwindow.testRefresh=refresh;\n})();"),s.context);
  return {...s,node,rows,list,pickerHost,requests,urls,reportROMUpload,droppedROMs,closeRequests,setOpenDialog(dialog){openDialog=dialog;},applyMode:s.context.window.testApplyMode,refresh:s.context.window.testRefresh};
}

test("discard all music edits names changes on hidden tracks and honors cancellation",async()=>{
  const s=setupAssetApp();
  for(const row of s.rows){
    row.dataset.installed="true";
    await row.clear.fire("click");
  }
  const discard=s.node("discard-assets");
  let accept=false;
  s.context.window.workshopConfirm=async(action,message,{tracks})=>{
    assert.equal(action,"builder.assets.discard_all");
    assert.equal(message,"builder.assets.discard_tracks");
    assert.deepEqual(tracks.split("\n"),s.rows.map(row=>s.context.window.workshopModPacks.targetName(row.dataset.track)));
    return accept;
  };
  await discard.fire("click");
  assert.ok(s.rows.every(row=>row.remove.value==="1"));
  accept=true;
  await discard.fire("click");
  assert.ok(s.rows.every(row=>row.remove.value==="0"));
  assert.equal(s.node("asset-bar").dataset.dirty,"false");
  assert.equal(discard.hidden,true);
});

test("a clean Workshop closes directly from the Builder button and desktop shell",async()=>{
  for(const desktopShell of [false,true]){
    const s=setupAssetApp(undefined,{desktopShell});
    const closed=observeWorkshopClose(s);
    s.node("close-workshop-dialog").showModal=()=>assert.fail("a clean quit should skip confirmation");
    if(desktopShell)await s.closeRequests.fire("close-request");
    else await s.node("close").fire("click");
    await closed;
    assert.deepEqual(s.requests,["status","close"]);
    if(desktopShell)assert.equal(s.closeRequests.closed,true);
  }
});

test("native quit lets a player keep editing or discard an unsaved Workshop draft",async()=>{
  const s=setupAssetApp(undefined,{desktopShell:true});
  const closed=observeWorkshopClose(s);
  const dialog=s.node("close-workshop-dialog");
  dialog.showModal=()=>{assert.ok(!dialog.open);dialog.open=true;};
  const dismiss=async value=>{
    dialog.returnValue=value;
    dialog.open=false;
    await dialog.fire("close");
  };
  s.node("asset-bar").dataset.dirty="true";
  await s.closeRequests.fire("close-request");
  await s.closeRequests.fire("close-request");
  assert.equal(dialog.open,true);
  await dismiss("cancel");
  assert.equal(s.node("asset-bar").dataset.dirty,"true");
  assert.deepEqual(s.requests,["status"]);
  await s.closeRequests.fire("close-request");
  await dismiss("close");
  await closed;
  assert.deepEqual(s.requests,["status","close"]);
});

function observeWorkshopClose(s){
  const closed=new Promise(resolve=>s.doc.addEventListener("workshop:closed",resolve));
  s.node("scene-motion").dispatchEvent=()=>{};
  s.context.fetch=async(path,options)=>{
    s.requests.push(path);
    assert.equal(path,"close");
    assert.equal(options.method,"POST");
    return {ok:true,json:async()=>({state:"closed"})};
  };
  return closed;
}

test("blocked build shows localized policy recovery while preserving the raw report",async()=>{
  const s=setupAssetApp();
  const queryAll=s.doc.querySelectorAll.bind(s.doc);
  s.doc.querySelectorAll=selector=>["[data-language-status]",'[data-nav="localization"]'].includes(selector)?[]:queryAll(selector);
  s.node("home-projects").replaceChildren=()=>{};
  const query=s.doc.querySelector.bind(s.doc);
  s.doc.querySelector=selector=>selector==='meta[name="workshop-version"]'?{content:"test-version"}:query(selector);
  s.doc.createElement=tag=>{
    const node=new Node(tag);
    node.append=(...children)=>node.children.push(...children);
    node.remove=()=>{s.doc.children=s.doc.children.filter(child=>child!==node);};
    node.focus=()=>{s.doc.activeElement=node;};
    node.showModal=()=>{node.open=true;};
    node.close=()=>{node.open=false;void node.fire("close");};
    return node;
  };
  let card;
  s.doc.body.append=node=>{card=node;s.doc.children.push(node);};
  s.context.navigator={userAgent:"Test webview"};
  runInNewContext(readFileSync(new URL("../../workshopui/feedback.js",import.meta.url),"utf8"),s.context);
  const detail="start snesbuild regen: fork/exec C:\\Users\\Alice\\Builder\\snesbuild.exe: An Application Control policy has blocked this file.";
  let data={state:"failed",mode:"builder",install:{canRebuild:true},rebuildROM:"user-rom.sfc",error:detail,
    errorCode:"builder.errors.windows_app_control",recoveryKey:"builder.recovery.windows_app_control",log:"=== Build failed ===\n"+detail};
  s.context.fetch=async(path,options)=>({ok:true,json:async()=>path==="interface/preferences"?JSON.parse(options.body):data});
  await s.refresh();
  assert.equal(card.tagName,"DIALOG");
  assert.equal(card.open,true);
  const report=card.children[3].children[1].value;
  assert.match(report,/Code: builder.errors.windows_app_control/);
  assert.match(report,/Operation: Build game/);
  assert.match(report,/Details: start snesbuild regen/);
  assert.match(report,/Recent build log \(tail\):\n=== Build failed ===/);
  assert.doesNotMatch(report,/Alice|HTTP status:/);
  assert.equal(s.node("log").textContent,data.log);
  assert.equal(s.node("view-build-log").hidden,false);
  await s.node("view-build-log").fire("click");
  assert.equal(s.node("build-view").hidden,false);
  assert.equal(s.node("build").disabled,false);
  assert.equal(s.node("launch").disabled,true);
  for(const locale of ["en","fr","de","ja"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.node("state").textContent,s.ui.text(data.errorCode));
    assert.equal(card.children[2].textContent,s.ui.text(data.recoveryKey));
    assert.equal(card.children[3].children[1].value,report);
  }
  // A status recheck must retain recovery without starting another build.
  await card.children[5].children[1].fire("click");
  assert.equal(card.children[3].children[1].value,report);
  await card.children[5].children[2].fire("click");
  assert.equal(card.open,false);
  await s.refresh();
  assert.equal(card.open,false,"polling the same failure must not reopen its dialog");
  data={state:"failed",mode:"builder",install:{canRebuild:true},error:"compiler failed",log:"compiler log"};
  await s.refresh();
  assert.equal(card.children[2].textContent,s.ui.text("builder.feedback.help"));
  assert.match(card.children[3].children[1].value,/Code: AR_OPERATION/);
});

test("launch errors name the action and offer only a read-only status recheck",async()=>{
  const s=setupAssetApp(), reports=[],requests=[];
  const failure=new Error("SDL_Init failed: No available video device");
  failure.code="AR_HTTP_500";failure.status=500;
  let preflight=true;
  s.context.window.workshopFeedback={clear(){},show:(target,error,options)=>reports.push({target,error,options}),readJSON:async()=>{
    if(preflight){preflight=false;return {state:"idle",mode:"ready",install:{canLaunch:true,canRebuild:true,build:{state:"current"}}};}
    throw failure;
  }};
  s.context.fetch=async(path,options)=>{requests.push({path,method:options?.method||"GET"});return {};};
  await s.node("launch").fire("click");
  assert.equal(reports[0].options.operation,"Launch game");
  assert.equal(reports[0].error,failure);
  assert.equal(reports[0].target,s.node("state"));
  assert.equal(s.node("workspace-status").textContent,s.ui.text("builder.feedback.failed"));
  assert.equal(s.node("launch").disabled,false);
  await reports[0].options.retry();
  assert.deepEqual(requests,[{path:"status",method:"GET"},{path:"launch",method:"POST"},{path:"status",method:"GET"}]);
  assert.equal(reports[1].options.operation,"Check Builder status");
});

test("release freshness distinguishes current, changed and legacy builds in every interface language",async()=>{
  const s=setupAssetApp();
  const install={canLaunch:true,canRebuild:true,build:{state:"rebuild",builderVersion:"same-version",builtVersion:"same-version"}};
  for(const locale of ["en","fr","de","ja"]){
    s.picker.value=locale;await s.picker.fire("change");
    for(const state of ["current","rebuild","unknown","unavailable"]){
      install.build.state=state;
      s.applyMode({state:"idle",mode:"ready",install});
      assert.equal(s.node("game-version-notice").hidden,state==="current");
      assert.equal(s.node("game-version-rebuild").hidden,state==="current"||state==="unavailable");
      if(state!=="current")assert.equal(s.node("game-version-title").textContent,s.ui.text(state==="unavailable"?"builder.version.unavailable":"builder.version.recommended"));
      assert.equal(s.node("home-primary").disabled,false);
      assert.equal(s.node("play").disabled,false);
      assert.equal(s.node("home-game-note").textContent,s.ui.text(state==="current"?"builder.version.current":"builder.home.ready"));
    }
  }
  s.applyMode({state:"idle",install:{canLaunch:false,canRebuild:true}});
  assert.equal(s.node("game-version-notice").hidden,true);
  s.applyMode({state:"building",install});
  assert.equal(s.node("game-version-notice").hidden,true);
});

function stalePlayFixture(state){
  const s=setupAssetApp(),requests=[];
  const status={state:"idle",mode:"ready",install:{canLaunch:true,canRebuild:true,build:{state}}};
  s.applyMode(status);
  s.context.fetch=async(path,options)=>{requests.push([path,options?.method||"GET"]);return {ok:true,json:async()=>status};};
  const dialog=s.node("stale-build-dialog");let shown;
  const opened=new Promise(resolve=>{shown=resolve;});
  dialog.showModal=()=>{dialog.open=true;shown();};
  dialog.close=()=>{dialog.open=false;dialog.fire("close");};
  return {...s,requests,status,dialog,opened};
}

test("all Play buttons warn on stale or legacy builds; cancel never launches",async()=>{
  for(const state of ["rebuild","unknown"]){
    for(const id of ["home-primary","play","launch","dock-launch"]){
      const s=stalePlayFixture(state);
      const clicking=s.node(id).fire("click");await s.opened;
      assert.deepEqual(s.requests,[["status","GET"]]);
      assert.equal(s.dialog.open,true);
      await s.node("stale-build-cancel").fire("click");await clicking;
      assert.deepEqual(s.requests,[["status","GET"]]);
      assert.equal(s.dialog.open,false);
      assert.equal(s.node(id).disabled,false);
    }
  }
});

test("Play existing build launches once; up-to-date games skip the prompt",async()=>{
  for(const state of ["rebuild","unknown","current"]){
    const s=stalePlayFixture(state);
    const clicking=s.node("play").fire("click");
    if(state!=="current"){
      await s.opened;
      await s.node("play").fire("click");
      await s.node("stale-build-play").fire("click");
    }
    await clicking;
    assert.deepEqual(s.requests,[["status","GET"],["launch","POST"]]);
    assert.notEqual(s.dialog.open,true);
  }
});

test("Review rebuild and Escape dismiss stale Play without launching",async()=>{
  for(const action of ["rebuild","escape"]){
    const s=stalePlayFixture("rebuild");
    const clicking=s.node("play").fire("click");await s.opened;
    if(action==="rebuild")await s.node("stale-build-review").fire("click");
    else await s.dialog.fire("cancel",{preventDefault(){}});
    await clicking;
    assert.deepEqual(s.requests,[["status","GET"]]);
    if(action==="rebuild")assert.equal(s.context.location.hash,"#build");
  }
});

test("Play rechecks freshness and launcher-only installs can still play",async()=>{
  const s=stalePlayFixture("current");
  // The page last saw a current build. A replacement/receipt change is caught
  // at the click, not only when reopening the page or finishing another build.
  s.status.install.build.state="unknown";
  s.status.install.canRebuild=false;
  s.status.mode="launcher";
  const clicking=s.node("home-primary").fire("click");await s.opened;
  assert.equal(s.node("stale-build-review").hidden,true);
  await s.node("stale-build-play").fire("click");await clicking;
  assert.deepEqual(s.requests,[["status","GET"],["launch","POST"]]);
});

test("Play offers prepared mods, retry, and cancel after a scan failure",async()=>{
  for(const action of ["play","retry","cancel"]){
    const s=stalePlayFixture("current");
    const dialog=s.node("mod-recovery-dialog");
    let shown;
    const opened=new Promise(resolve=>{shown=resolve;});
    dialog.showModal=()=>{dialog.open=true;shown();};
    dialog.close=()=>{dialog.open=false;};
    const launches=[];
    s.context.fetch=async(path,options)=>{
      if(path==="status")return {ok:true,json:async()=>s.status};
      assert.equal(path,"launch");
      launches.push(options?.body ? JSON.parse(options.body).modRecovery : "");
      if(launches.length===1)return {status:409,clone:()=>({json:async()=>({
        errorCode:"builder.mods.scan_failed",detail:"too many installed mods",modRecovery:"prepared",
      })})};
      return {ok:true,json:async()=>({state:"launched"})};
    };
    const clicking=s.node("play").fire("click");
    await opened;
    assert.equal(s.node("mod-recovery-play").textContent,s.ui.text("builder.mods.play_prepared"));
    assert.equal(s.node("mod-recovery-detail").textContent,"too many installed mods");
    await s.node("mod-recovery-"+action).fire("click");
    await clicking;
    assert.equal(dialog.open,false);
    assert.deepEqual(launches, action==="cancel"?[""]:["",action==="play"?"prepared":""]);
  }
});

test("a failed Play preflight never starts the game",async()=>{
  const s=stalePlayFixture("rebuild"),reports=[];
  s.context.window.workshopFeedback={clear(){},show:(_,error,options)=>reports.push({error,options}),readJSON:async()=>{throw Error("Status unavailable");}};
  await s.node("play").fire("click");
  assert.deepEqual(s.requests,[["status","GET"]]);
  assert.equal(reports[0].options.operation,"Launch game");
  assert.equal(s.node("play").disabled,false);
  assert.notEqual(s.dialog.open,true);
});

function readyRebuildFixture({preflight=false,fail=false}={}){
  const s=stalePlayFixture("rebuild");
  s.status.rebuildROM="/Games/日本語 {path}/user-rom.sfc";
  s.applyMode(s.status);
  s.context.FormData=Map;
  s.context.fetch=async(path,options)=>{
    s.requests.push([path,options?.method||"GET",options?.body]);
    if(path==="status"){
      if(preflight){preflight=false;return {ok:true,json:async()=>s.status};}
      return new Promise(()=>{}); // Leave build polling in flight.
    }
    assert.equal(path,"build");
    return fail?{ok:false,json:async()=>({error:"Saved ROM moved",errorCode:"builder.build.saved_rom_missing"})}:{ok:true,json:async()=>({state:"building"})};
  };
  return s;
}

test("saved ROM path translates literally and enables rebuilding",async()=>{
  const s=setupAssetApp(), path="C:\\Games\\日本語 {path}\\user-rom.sfc";
  s.applyMode({state:"idle",install:{canLaunch:true,canRebuild:true,build:{state:"rebuild"}},rebuildROM:path});
  for(const locale of ["en","fr","de","ja"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.node("build").disabled,false);
    assert.equal(s.node("saved-rom-note").textContent,s.ui.text("builder.build.saved_rom",{path}));
    assert.equal(s.node("game-version-rebuild").textContent,s.ui.text("builder.version.rebuild_now"));
    assert.equal(s.node("build").textContent,s.ui.text("builder.build.rebuild_game"));
  }
  s.applyMode({state:"idle",install:{canLaunch:true,canRebuild:true,build:{state:"rebuild"}}});
  assert.equal(s.node("build").disabled,true);
  assert.equal(s.node("saved-rom-note").hidden,true);
  assert.equal(s.node("game-version-rebuild").textContent,s.ui.text("builder.version.review"));
});

test("desktop Setup keeps the checklist and accepts preselected ROMs",async()=>{
  const s=setupAssetApp();
  const checklist=s.node("rom-checklist");
  s.applyMode({state:"idle",install:{canRebuild:true},selectedROM:"ActRaiser (USA).sfc",hasSelectedROMs:true});
  assert.equal(s.node("build").disabled,false);
  assert.equal(s.node("home-game-note").textContent,s.ui.text("builder.home.rom_ready"));
  await s.node("home-primary").fire("click");
  assert.equal(checklist.parentNode,s.node("build-rom-checklist"));
  assert.equal(s.node("saved-rom-note").textContent,s.ui.text("builder.build.browser_rom",{path:"ActRaiser (USA).sfc"}));
  await s.node("tab-home").fire("click");
  assert.equal(checklist.parentNode,s.node("build-rom-checklist"));
  assert.equal(s.node("panel-build").hidden,true);
});

test("desktop build requires a core ROM and waits for checklist selection",()=>{
  const s=setupAssetApp();
  s.applyMode({state:"idle",install:{canRebuild:true}});
  assert.equal(s.node("build").disabled,true);
  s.reportROMUpload(true);
  s.applyMode({state:"idle",install:{canRebuild:true},selectedROM:"ActRaiser (USA).sfc",hasSelectedROMs:true});
  assert.equal(s.node("home-game-note").textContent,s.ui.text("builder.roms.checking_files"));
  assert.equal(s.node("build").disabled,true);
  s.reportROMUpload(false);
  assert.equal(s.node("build").disabled,false);
});

test("Rebuild now and the build form reuse the saved ROM with one request",async()=>{
  for(const id of ["game-version-rebuild","build-form"]){
    const s=readyRebuildFixture();
    const first=s.node(id).fire(id==="build-form"?"submit":"click",{preventDefault(){}});
    const duplicate=s.node(id).fire(id==="build-form"?"submit":"click",{preventDefault(){}});
    await Promise.all([first,duplicate]);
    const builds=s.requests.filter(([path])=>path==="build");
    assert.equal(builds.length,1);
    assert.equal(builds[0][2].get("useSelectedROMs"),"true");
    assert.equal(builds[0][2].get("reuseROM"),"true");
    assert.equal(builds[0][2].has("rom"),false);
    assert.equal(s.node("build-view").hidden,false);
    assert.equal(s.node("build-view-actions").hidden,true);
    assert.equal(s.node("build-report").hidden,true);
    assert.equal(s.node("game-version-notice").hidden,true);
    assert.equal(s.node("play").disabled,true);
  }
});

test("stale Play dialog can rebuild immediately without launching the old game",async()=>{
  const s=readyRebuildFixture({preflight:true});
  const clicking=s.node("play").fire("click");await s.opened;
  assert.equal(s.node("stale-build-review").textContent,s.ui.text("builder.version.rebuild_now"));
  await s.node("stale-build-review").fire("click");await clicking;
  assert.deepEqual(s.requests.map(([path,method])=>[path,method]),[["status","GET"],["build","POST"],["status","GET"]]);
  assert.equal(s.requests[1][2].get("reuseROM"),"true");
  assert.equal(s.node("play").disabled,true);
});

test("an explicit checklist selection wins over the saved copy",async()=>{
  const s=readyRebuildFixture();
  s.applyMode({...s.status,selectedROM:"new.SMC",hasSelectedROMs:true});
  await s.node("build-form").fire("submit",{preventDefault(){}});
  const payload=s.requests.find(([path])=>path==="build")[2];
  assert.equal(payload.get("useSelectedROMs"),"true");
  assert.equal(payload.has("reuseROM"),false);
});

test("a vanished saved ROM keeps the error and offers a return to Setup",async()=>{
  const s=readyRebuildFixture({fail:true});
  await s.node("game-version-rebuild").fire("click");
  assert.equal(s.node("build").disabled,true);
  assert.equal(s.node("saved-rom-note").hidden,true);
  assert.equal(s.node("game-version-rebuild").textContent,s.ui.text("builder.version.review"));
  assert.match(s.node("state").textContent,/Saved ROM moved/);
  assert.equal(s.node("build-view-actions").hidden,false);
  assert.equal(s.node("build-report").hidden,false);
  const saved=[];
  s.context.fetch=async(path,options)=>{
    saved.push([path,options.method,JSON.parse(options.body)]);
    return {ok:true,json:async()=>({path:"logs/build.txt"})};
  };
  await s.node("build-report").fire("click");
  assert.deepEqual(saved,[["build-report","POST",{operation:"build",message:s.node("state").textContent}]]);
  assert.match(s.node("build-report-status").textContent,/logs\/build\.txt/);
  await s.node("build-view-back").fire("click");
  assert.equal(s.node("build-view").hidden,true);
  assert.equal(s.context.location.hash,"#build");
  assert.equal(s.requests.length,1); // No hidden retries or error-clearing poll.
});

test("global drops report unsupported files and leave an active dialog in charge",async()=>{
  const s=setupAssetApp(), messages=[];
  s.context.window.workshopFeedback={show:(_target,_error,options)=>messages.push(options.messageKey)};
  let consumed=0;
  const drop=(name,target)=>s.doc.fire("drop",{
    target,preventDefault(){consumed++;},dataTransfer:{files:[{name}]},
  });
  const drag={preventDefault(){},dataTransfer:{types:["Files"]}};
  await s.doc.fire("dragenter",drag);
  await s.doc.fire("dragover",drag);
  assert.equal(s.node("builder-drop-hint").hidden,false);
  await s.doc.fire("dragleave",drag);
  assert.equal(s.node("builder-drop-hint").hidden,true);
  await drop("unrecognized.zip");
  s.setOpenDialog(s.node("asset-pack-export"));
  await drop("music.o2r");
  await drop("core.sfc");
  await drop("artwork.png",{closest:()=>true});
  assert.equal(consumed,3,"leave a dialog's own file input to the browser");
  assert.deepEqual(messages,["builder.drop.formats","builder.drop.finish_dialog","builder.drop.finish_dialog"]);
  assert.deepEqual(s.droppedROMs,[]);
  assert.ok(!s.requests.some(path=>path.startsWith("mods/install")));
});

test("an active build keeps other tabs available while blocking ROM changes",async()=>{
  const s=setupAssetApp(), rom={name:"game.SMC"};
  const drop=()=>s.doc.fire("drop",{preventDefault(){},dataTransfer:{files:[rom,{name:"notes.txt"}]}});
  await drop();
  assert.deepEqual(s.droppedROMs,[rom]);
  assert.equal(s.context.location.hash,"#build");

  const install={canLaunch:true,canRebuild:true,build:{state:"current"}};
  s.applyMode({state:"building",install});
  assert.equal(s.node("build-view").hidden,false);
  assert.equal(s.context.location.hash,"#progress");
  assert.equal(s.node("build-progress").hidden,false);
  await s.node("tab-home").fire("click");
  await drop();
  assert.equal(s.context.location.hash,"#home");
  assert.equal(s.node("build-view").hidden,true);
  assert.deepEqual(s.droppedROMs,[rom]);

  s.context.fetch=async()=>({ok:true,json:async()=>({state:"succeeded",install,log:"Installed game",progress:{percent:100}})});
  await s.refresh();
  assert.equal(s.node("build-view").hidden,true,"finishing a build must not switch tabs");
  await s.node("tab-progress").fire("click");
  assert.equal(s.node("build-view-actions").hidden,false);
  assert.equal(s.node("build-view-play").hidden,false);
  assert.equal(s.node("build-report").hidden,true);
  assert.equal(s.node("log").textContent,"Installed game");
  await s.node("build-view-back").fire("click");
  assert.equal(s.node("build-view").hidden,true);
  assert.equal(s.context.location.hash,"#home");
  assert.equal(s.node("view-build-log").hidden,false);
});

test("real asset controller keeps unsaved files, playback, search and form state during language changes",async()=>{
  const s=setupAssetApp(),row=s.rows[0];
  const files=[{name:"custom {count} 日本語.ogg"}];row.file.files=files;
  row.split.value="1";row.regionChoice.checked=true;
  await row.file.fire("change");
  assert.equal(s.node("asset-bar").dataset.dirty,"true");
  assert.equal(row.dataset.pending,"pending");
  row.audio.paused=false;row.audio.currentTime=17.25;
  const pauses=row.audio.pauses,loads=row.audio.loads;
  s.node("asset-search").value="Sky Palace";
  s.node("asset-search").selectionStart=3;s.doc.activeElement=s.node("asset-search");
  for(const locale of ["fr","de","ja","en"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(row.file.files,files);
    assert.equal(row.remove.value,"0");assert.equal(row.split.value,"1");
    assert.equal(row.regionChoice.checked,true);assert.equal(row.regionChoice.value,"fillmore");
    assert.equal(row.audio.currentTime,17.25);assert.equal(row.audio.paused,false);
    assert.equal(row.audio.pauses,pauses);assert.equal(row.audio.loads,loads);
    assert.equal(row.audio.src,"blob:pending-track");
    assert.equal(row.current.textContent,s.ui.text("builder.assets.selected_file",{file:files[0].name}));
    assert.equal(row.clear.textContent,s.ui.text("builder.assets.cancel"));
    assert.equal(row.label.textContent,s.ui.text("builder.assets.replacement_file"));
    assert.equal(s.node("asset-selected-track").textContent,s.ui.text("builder.assets.track.song_09"));
    assert.equal(s.node("asset-bar-note").textContent,s.ui.text("builder.assets.unsaved",{count:2}));
    assert.equal(s.node("asset-bar").dataset.dirty,"true");
    assert.equal(s.node("save-assets-top").disabled,false);
    assert.equal(s.node("asset-search").value,"Sky Palace");
    assert.equal(s.node("asset-search").selectionStart,3);
    assert.equal(s.doc.activeElement,s.node("asset-search"));
    assert.equal(s.list.children[0].hidden,true);assert.equal(s.list.children[1].hidden,false);
  }
  assert.deepEqual(s.requests,["status",...Array(4).fill("interface/preferences")]);
  assert.equal(s.urls.length,1);
  // Both translated names and stable manifest IDs remain searchable.
  s.picker.value="ja";await s.picker.fire("change");
  for(const query of ["天空城","song-01"]){
    s.node("asset-search").value=query;await s.node("asset-search").fire("input");
    assert.equal(s.list.children[1].hidden,false);assert.equal(s.list.children[0].hidden,true);
  }
});

test("track picker marks custom and regional audio independently from unsaved edits",async()=>{
  const s=setupAssetApp(),[base,other]=s.rows,[button,untouched]=s.list.children;
  const [,music,edited]=button.children;
  const check=(label,dirty)=>{
    for(const mark of [music,s.node("asset-selected-music")]){
      assert.equal(mark.hidden,!label);
      if(label) assert.equal(mark.textContent,label);
    }
    assert.equal(edited.hidden,!dirty);
    assert.equal(s.node("asset-selected-edit").hidden,!dirty);
    assert.ok(untouched.children[1].hidden && untouched.children[2].hidden);
  };
  check("",false);
  base.dataset.installed="true";
  await button.fire("click");
  check("Custom music",false);
  await base.clear.fire("click");
  check("",true);
  await base.clear.fire("click");
  check("Custom music",false);

  base.dataset.installed="false";
  // Reuse a row-shaped fixture for a regional replacement.
  const variant={dataset:{},querySelector:other.querySelector};
  base.nextElementSibling={classList:{contains:name=>name==="asset-variants"},children:[variant],querySelectorAll:()=>[]};
  variant.dataset.pendingSplit="true";
  await button.fire("click");
  check("",true);
  variant.dataset.installed="true";
  variant.dataset.pendingSplit="false";
  await button.fire("click");
  check("Regional only",false);
  base.file.files=[{name:"new.ogg"}];
  await base.file.fire("change");
  check("Custom music",true);
});

test("music track mouse clicks retain search focus through WebKit's default focus fallback",async()=>{
  const s=setupAssetApp([["title-theme","Title Theme"],["song-13","Track 13"]]);
  const toggle=s.node("asset-track-toggle"),options=s.node("asset-track-options"),search=s.node("asset-search");
  const track=s.list.children[1],workspace=s.node("workspace");
  workspace.setAttribute("tabindex","-1");
  await toggle.fire("click");
  await s.doc.fire("pointerdown",{target:track,button:0,pointerType:"mouse"});
  let prevented=false;
  await track.fire("mousedown",{target:track,button:0,preventDefault(){prevented=true;}});
  // Native WebKit can focus the tabindex=-1 workspace instead of the button.
  // This is the mouse event's default action, before mouseup and click.
  if(!prevented){
    await s.pickerHost.fire("focusout",{target:search,currentTarget:s.pickerHost,relatedTarget:workspace});
    workspace.focus();await s.doc.fire("focusin",{target:workspace});
  }
  // A held press must neither dismiss the picker nor select a track early.
  await new Promise(setImmediate);
  assert.equal(options.hidden,false,"the pressed track must survive default mouse focus movement");
  assert.equal(s.doc.activeElement,search);
  assert.equal(s.rows[0].hidden,false);assert.equal(s.rows[1].hidden,true);
  await track.fire("mouseup",{target:track,button:0});
  await track.fire("click",{target:track,button:0});
  assert.equal(s.rows[0].hidden,true);assert.equal(s.rows[1].hidden,false);
  assert.equal(s.node("asset-selected-track").textContent,track.firstElementChild.textContent);
  assert.equal(options.hidden,true);assert.equal(s.doc.activeElement,toggle);
  // Secondary presses retain their native behavior and never select a track.
  await toggle.fire("click");
  await s.list.children[0].fire("mousedown",{button:2,preventDefault(){assert.fail("secondary mouse defaults must remain available");}});
  assert.equal(s.rows[0].hidden,true);assert.equal(s.rows[1].hidden,false);
});

test("music track clicks survive WebKit's null focusout and preserve pending uploads",async()=>{
  const s=setupAssetApp([["title-theme","Title Theme"],["song-01","Sky Palace"]]);
  const [title,palace]=s.rows,[titleButton,palaceButton]=s.list.children;
  const toggle=s.node("asset-track-toggle"),options=s.node("asset-track-options"),search=s.node("asset-search");
  assert.equal(title.hidden,false);assert.equal(palace.hidden,true);
  const files=[{name:"title replacement.ogg"}];title.file.files=files;await title.file.fire("change");
  await toggle.fire("click");
  assert.equal(s.doc.activeElement,search);assert.equal(options.hidden,false);
  await s.doc.fire("pointerdown",{target:palaceButton});
  // WebKit blurs the search input without focusing the mouse-clicked button.
  // Hiding its ancestor here prevents the subsequent browser click entirely.
  s.doc.activeElement=s.doc.body;
  await s.pickerHost.fire("focusout",{target:search,currentTarget:s.pickerHost,relatedTarget:null});
  assert.equal(options.hidden,false,"the pressed track must remain available until click");
  await palaceButton.fire("click");
  assert.equal(title.hidden,true);assert.equal(palace.hidden,false);
  assert.equal(s.node("asset-selected-track").textContent,palaceButton.firstElementChild.textContent);
  assert.equal(palaceButton.getAttribute("aria-current"),"true");
  assert.equal(options.hidden,true);assert.equal(toggle.getAttribute("aria-expanded"),"false");
  assert.equal(s.doc.activeElement,toggle);
  await toggle.fire("click");await titleButton.fire("click");
  assert.equal(title.hidden,false);assert.equal(palace.hidden,true);
  assert.equal(title.file.files,files);assert.equal(title.file.name,"track-title-theme");
  assert.equal(title.dataset.pending,"pending");assert.equal(s.node("asset-bar").dataset.dirty,"true");
});

test("music track picker stays open for internal focus and closes for outside interaction",async()=>{
  const s=setupAssetApp(),toggle=s.node("asset-track-toggle"),options=s.node("asset-track-options");
  const button=s.list.children[1],outside=s.node("save-assets-top");
  await toggle.fire("click");
  await s.pickerHost.fire("focusout",{target:s.node("asset-search"),currentTarget:s.pickerHost,relatedTarget:button});
  button.focus();await s.doc.fire("focusin",{target:button});
  assert.equal(options.hidden,false);
  await s.doc.fire("pointerdown",{target:outside});
  assert.equal(options.hidden,true);assert.equal(toggle.getAttribute("aria-expanded"),"false");
  await toggle.fire("click");
  await s.pickerHost.fire("focusout",{target:s.node("asset-search"),currentTarget:s.pickerHost,relatedTarget:outside});
  outside.focus();await s.doc.fire("focusin",{target:outside});
  assert.equal(options.hidden,true);assert.equal(s.doc.activeElement,outside);
});

test("music track picker keeps keyboard navigation, search selection and Escape",async()=>{
  const s=setupAssetApp(),toggle=s.node("asset-track-toggle"),options=s.node("asset-track-options"),search=s.node("asset-search");
  const key=(key,target=s.doc.activeElement)=>options.fire("keydown",{key,target,preventDefault(){}});
  await toggle.fire("click");
  await key("ArrowDown");assert.equal(s.doc.activeElement,s.list.children[0]);
  await key("End");assert.equal(s.doc.activeElement,s.list.children[1]);
  await key("Home");assert.equal(s.doc.activeElement,s.list.children[0]);
  await key("ArrowUp");assert.equal(s.doc.activeElement,s.list.children[1]);
  await key("Escape");assert.equal(options.hidden,true);assert.equal(s.doc.activeElement,toggle);
  await toggle.fire("click");search.value="Sky Palace";await search.fire("input");
  assert.equal(s.list.children[0].hidden,true);assert.equal(s.list.children[1].hidden,false);
  await key("Enter",search);
  assert.equal(s.rows[0].hidden,true);assert.equal(s.rows[1].hidden,false);
  assert.equal(options.hidden,true);assert.equal(s.node("asset-selected-track").textContent,s.list.children[1].firstElementChild.textContent);
});

test("asset errors use stable actionable codes and retain raw details and pending files",async()=>{
  const s=setupAssetApp(),fetch=s.context.fetch;
  for(const code of ["builder.assets.need_rom","builder.assets.preview_busy"]){
    s.context.fetch=async()=>({ok:false,json:async()=>({error:"English server diagnostic",errorCode:code})});
    await s.node("generate-previews").fire("click");
    s.context.fetch=fetch;s.picker.value="ja";await s.picker.fire("change");
    assert.equal(s.node("preview-state").textContent,s.ui.text(code));
    assert.ok(!s.node("preview-state").textContent.includes("English server diagnostic"));
  }
  const files=[{name:"pending.ogg"}];s.rows[0].file.files=files;await s.rows[0].file.fire("change");
  s.context.FormData=class {};
  s.context.fetch=async()=>({ok:false,json:async()=>({error:"disk full: /tmp/<file>{detail}"})});
  await s.node("assets-form").fire("submit",{preventDefault(){}});
  s.context.fetch=fetch;s.picker.value="de";await s.picker.fire("change");
  assert.equal(s.node("asset-state").textContent,s.ui.text("builder.assets.save_failed",{detail:"disk full: /tmp/<file>{detail}"}));
  assert.equal(s.rows[0].file.files,files);assert.equal(s.node("asset-bar").dataset.dirty,"true");
});
