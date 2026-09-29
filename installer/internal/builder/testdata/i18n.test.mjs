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
    if(!ids.has(id)){const n=new Node("div");n.id=id;n.files=[];n.focus=()=>{s.doc.activeElement=n;};ids.set(id,n);s.doc.children.push(n);}
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
  const tabs=[node("tab-home"),node("tab-build")];
  tabs.forEach(tab=>tab.setAttribute("aria-controls",tab.id.replace("tab-","panel-")));
  const originalQuery=s.doc.querySelectorAll.bind(s.doc);
  s.doc.querySelectorAll=selector=>{
    if(selector===".asset-row")return rows;
    if(selector===".tab")return tabs;
    if([".step","[data-nav]","[data-asset-category]"].includes(selector))return [];
    return originalQuery(selector);
  };
  s.doc.querySelector=selector=>{
    if(selector.startsWith("#"))return node(selector.slice(1));
    return {".tabs":tabsHost,".workspace-bar":header,".asset-track-picker":pickerHost}[selector]??assert.fail(`unexpected document selector ${selector}`);
  };
  s.doc.createElement=tag=>new Node(tag);
  s.doc.body=new Node("body");
  s.doc.documentElement.style={setProperty(){}};
  s.context.window.addEventListener=()=>{};
  s.context.window.scrollTo=()=>{};
  s.context.window.history={pushState:(_,__,hash)=>{s.context.location.hash=hash;}};
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
  const source=readFileSync(new URL("../web/app.js",import.meta.url),"utf8");
  assert.match(source,/\}\)\(\);\s*$/);
  runInNewContext(source.replace(/\}\)\(\);\s*$/,"window.testApplyMode=applyMode;\n})();"),s.context);
  return {...s,node,rows,list,requests,urls,applyMode:s.context.window.testApplyMode};
}

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
  s.context.FormData=class extends Map {
    constructor(form){super();assert.equal(form,s.node("build-form"));if(s.node("rom").files.length)this.set("rom",s.node("rom").files[0]);}
  };
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

test("saved ROM path translates literally and removes file-picker requirement",async()=>{
  const s=setupAssetApp(), path="C:\\Games\\日本語 {path}\\user-rom.sfc";
  s.applyMode({state:"idle",install:{canLaunch:true,canRebuild:true,build:{state:"rebuild"}},rebuildROM:path});
  for(const locale of ["en","fr","de","ja"]){
    s.picker.value=locale;await s.picker.fire("change");
    assert.equal(s.node("rom").required,false);
    assert.equal(s.node("saved-rom-note").textContent,s.ui.text("builder.build.saved_rom",{path}));
    assert.equal(s.node("game-version-rebuild").textContent,s.ui.text("builder.version.rebuild_now"));
    assert.equal(s.node("build").textContent,s.ui.text("builder.build.rebuild_game"));
  }
  s.applyMode({state:"idle",install:{canLaunch:true,canRebuild:true,build:{state:"rebuild"}}});
  assert.equal(s.node("rom").required,true);
  assert.equal(s.node("saved-rom-note").hidden,true);
  assert.equal(s.node("game-version-rebuild").textContent,s.ui.text("builder.version.review"));
});

test("Rebuild now and the build form reuse the saved ROM with one request",async()=>{
  for(const id of ["game-version-rebuild","build-form"]){
    const s=readyRebuildFixture();
    const first=s.node(id).fire(id==="build-form"?"submit":"click",{preventDefault(){}});
    const duplicate=s.node(id).fire(id==="build-form"?"submit":"click",{preventDefault(){}});
    await Promise.all([first,duplicate]);
    const builds=s.requests.filter(([path])=>path==="build");
    assert.equal(builds.length,1);
    assert.equal(builds[0][2].get("reuseROM"),"true");
    assert.equal(builds[0][2].has("rom"),false);
    assert.equal(s.context.location.hash,"#build");
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

test("an explicit replacement file wins over the saved copy",async()=>{
  const s=readyRebuildFixture(), replacement={name:"new.SMC"};
  s.node("rom").files=[replacement];
  await s.node("build-form").fire("submit",{preventDefault(){}});
  const payload=s.requests.find(([path])=>path==="build")[2];
  assert.equal(payload.get("rom"),replacement);
  assert.equal(payload.has("reuseROM"),false);
});

test("a vanished saved ROM restores the picker and keeps the actionable error",async()=>{
  const s=readyRebuildFixture({fail:true});
  await s.node("game-version-rebuild").fire("click");
  assert.equal(s.node("rom").required,true);
  assert.equal(s.node("saved-rom-note").hidden,true);
  assert.equal(s.node("game-version-rebuild").textContent,s.ui.text("builder.version.review"));
  assert.match(s.node("state").textContent,/Saved ROM moved/);
  assert.equal(s.requests.length,1); // No hidden retries or error-clearing poll.
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
