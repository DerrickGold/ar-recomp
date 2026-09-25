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
  s.picker.value="ja";await s.picker.fire("change");assert.equal(s.requests.length,1);
  finish({ok:false,json:async()=>({errorCode:"builder.interface.save_failed"})});await saving;
  assert.equal(s.ui.locale,"fr");assert.equal(s.picker.value,"fr");assert.equal(s.picker.disabled,false);
  assert.equal(s.status.textContent,messages["builder.interface.save_failed"][1]);
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
    assert.ok(sourceLabel.textContent.includes("[music:song-09]"));
    assert.ok(sourceLabel.textContent.includes("0E:F69F"));
    assert.equal(cover.getAttribute("alt"),messages["builder.help.box_art"][column]);
    assert.equal(cover.getAttribute("src"),"boxart.webp");
    assert.equal(booklet.getAttribute("title"),messages["builder.help.manual_title"][column]);
    assert.equal(booklet.getAttribute("src"),"manual.pdf#page=7");
    assert.equal(media.getAttribute("src"),"blob:chosen-track");
    assert.equal(media.currentTime,12.5);assert.equal(media.paused,false);
  }
});

// Run the actual application alongside i18n, with a deliberately small DOM.
// No copied asset state machine and no browser/server dependency. Unsupported
// row selectors fail loudly so this fixture cannot silently hide new behavior.
function setupAssetApp(){
  const s=setup(), ids=new Map(),rows=[];
  const node=id=>{
    if(!ids.has(id)){const n=new Node("div");n.id=id;n.files=[];ids.set(id,n);s.doc.children.push(n);}
    return ids.get(id);
  };
  const names=["Track 09","Sky Palace"];
  for(const [index,id] of ["song-09","song-01"].entries()){
    const row=new Node("div","",{"data-track":id,"data-source":index===0?"0E:F69F":"1C:A988"});
    const file=new Node("input");file.files=[];
    const remove=new Node("input");remove.value="0";
    const split=new Node("input");split.value="0";
    const regionChoice=new Node("input");regionChoice.name="split-"+id;regionChoice.value="fillmore";regionChoice.checked=false;
    const audio=new Node("audio");audio.paused=true;audio.currentTime=0;audio.pauses=0;audio.loads=0;
    audio.pause=()=>{audio.paused=true;audio.pauses++;};audio.load=()=>{audio.loads++;};
    const current=new Node("span"),clear=new Node("button"),label=new Node("label",names[index]);
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
  const list=node("asset-track-list");list.append=child=>list.children.push(child);
  const pickerHost=new Node("div"),tabsHost=new Node("div"),header=new Node("header");header.offsetHeight=70;
  const originalQuery=s.doc.querySelectorAll.bind(s.doc);
  s.doc.querySelectorAll=selector=>{
    if(selector===".asset-row")return rows;
    if([".tab",".step","[data-nav]","[data-asset-category]"].includes(selector))return [];
    return originalQuery(selector);
  };
  s.doc.querySelector=selector=>{
    if(selector.startsWith("#"))return node(selector.slice(1));
    return {".tabs":tabsHost,".workspace-bar":header,".asset-track-picker":pickerHost}[selector]??assert.fail(`unexpected document selector ${selector}`);
  };
  s.doc.createElement=tag=>new Node(tag);
  s.doc.documentElement.style={setProperty(){}};
  s.context.window.addEventListener=()=>{};
  const requests=[], urls=[];
  Object.assign(s.context,{location:{hash:"#unknown"},matchMedia:()=>({matches:false,addEventListener(){}}),
    URL:{createObjectURL(file){urls.push(file);return "blob:pending-track";},revokeObjectURL(){throw Error("unexpected file release");}},
    // Initial status poll is left in flight. Switching UI language must not
    // trigger a configuration reload or a second asset request.
    fetch:(path,options)=>{
      requests.push(path);
      return path==="interface/preferences"?Promise.resolve({ok:true,json:async()=>JSON.parse(options.body)}):new Promise(()=>{});
    }});
  runInNewContext(readFileSync(new URL("../web/app.js",import.meta.url),"utf8"),s.context);
  return {...s,node,rows,list,requests,urls};
}

test("launch errors name the action and offer only a read-only status recheck",async()=>{
  const s=setupAssetApp(), reports=[],requests=[];
  const failure=new Error("SDL_Init failed: No available video device");
  failure.code="AR_HTTP_500";failure.status=500;
  s.context.window.workshopFeedback={clear(){},show:(target,error,options)=>reports.push({target,error,options}),readJSON:async()=>{throw failure;}};
  s.context.fetch=async(path,options)=>{requests.push({path,method:options?.method||"GET"});return {};};
  await s.node("launch").fire("click");
  assert.equal(reports[0].options.operation,"Launch game");
  assert.equal(reports[0].error,failure);
  assert.equal(reports[0].target,s.node("state"));
  assert.equal(s.node("workspace-status").textContent,s.ui.text("builder.feedback.failed"));
  assert.equal(s.node("launch").disabled,false);
  await reports[0].options.retry();
  assert.deepEqual(requests,[{path:"launch",method:"POST"},{path:"status",method:"GET"}]);
  assert.equal(reports[1].options.operation,"Check Builder status");
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
    assert.equal(row.label.textContent,s.ui.text("builder.assets.track.song_09"));
    assert.equal(s.node("asset-selected-track").textContent,row.label.textContent);
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

test("manual errors name removal and a successful install clears the previous report",async()=>{
  const s=setupAssetApp(),reports=new Map();
  s.context.window.workshopFeedback={clear:target=>reports.delete(target),show:(target,error,options)=>reports.set(target,{error,options}),readJSON:async response=>{
    const body=await response.json();if(!response.ok)throw new Error(body.error);return body;
  }};
  s.context.FormData=class {};
  s.context.fetch=async()=>({ok:false,json:async()=>({error:"Permission denied"})});
  await s.node("manual-remove").fire("click");
  assert.equal(reports.get(s.node("manual-message")).options.operation,"Remove manual");
  s.node("manual-file").files=[{name:"book.pdf"}];
  await s.node("manual-form").fire("submit",{preventDefault(){}});
  assert.equal(reports.get(s.node("manual-message")).options.operation,"Install manual");
  s.context.fetch=async()=>({ok:true,json:async()=>({present:true,bytes:128,pages:2,width:640,height:480})});
  await s.node("manual-form").fire("submit",{preventDefault(){}});
  assert.equal(reports.has(s.node("manual-message")),false);
  assert.equal(s.node("manual-message").dataset.kind,"succeeded");
});
