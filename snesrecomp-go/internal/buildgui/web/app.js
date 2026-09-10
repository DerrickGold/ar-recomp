(() => {
"use strict";
const ui=window.workshopI18n;
const form=document.querySelector("#build-form"), build=document.querySelector("#build"), launch=document.querySelector("#launch");
const state=document.querySelector("#state"), log=document.querySelector("#log"), closeButton=document.querySelector("#close");
const track=document.querySelector("#track"), logBox=document.querySelector("#log-box");
const dock=document.querySelector("#dock"), dockPhase=document.querySelector("#workspace-status");
const dockPct=document.querySelector("#dock-pct");
const dockLaunch=document.querySelector("#dock-launch"), stepsBox=document.querySelector("#steps-box");
const steps=[...document.querySelectorAll(".step")];
const tabs=[...document.querySelectorAll(".tab")];
const buildTab=document.querySelector("#tab-build");
const languageTab=document.querySelector("#tab-localization");
let localizationReady=false, localizationChecked=false;
const assetTab=document.querySelector("#tab-assets"), assetForm=document.querySelector("#assets-form");
const titleToggle=document.querySelector("#title-toggle"), titleChange=document.querySelector("#title-change");
const assetState=document.querySelector("#asset-state");
const saveAssetsTop=document.querySelector("#save-assets-top"), discardAssets=document.querySelector("#discard-assets");
const assetBar=document.querySelector("#asset-bar"), assetBarNote=document.querySelector("#asset-bar-note");
const rowPrototype=document.querySelector("#asset-row-prototype");
/* Mutable: variant rows are built from the manifest when the configuration
 * loads, and every loop over the list must see them too. */
let assetRows=[...document.querySelectorAll(".asset-row")];
const generatePreviews=document.querySelector("#generate-previews"), previewState=document.querySelector("#preview-state");
const regeneratePreviews=document.querySelector("#regenerate-previews");
let polling=false;
let assetsLoaded=false;
let previewPolling=false, previewTimer=0;
const homePrimary=document.querySelector("#home-primary"), workspaceStatus=document.querySelector("#workspace-status");
let canLaunch=false, closed=false, launching=false;
let lastPaint=null;
let activeTab=document.querySelector("#tab-home");
const scrollPositions=new Map();

/* Tabs. The manual's <iframe> is HIDDEN rather than removed when its tab is not
 * showing: unmounting it would refetch 8 MB and lose the reader's page. It is
 * still created empty and only given its src on first open, so a user who never
 * opens the manual never pays for it. */
function selectTab(tab,{history=true,focus=true,activate=true}={}){
  if(!tab||tab.disabled||closed) return false;
  if(activeTab!==tab) scrollPositions.set(activeTab.id,window.scrollY);
  if(activate&&tab.id==="tab-localization") window.localizationActivate?.();
  for(const t of tabs){
    const on=t===tab;
    t.setAttribute("aria-selected",String(on));
    t.tabIndex=on?0:-1;
    document.querySelector("#"+t.getAttribute("aria-controls")).hidden=!on;
  }
  if(tab===document.querySelector("#tab-manual")){
    const frame=document.querySelector("#manual-frame");
    if(!frame.getAttribute("src")) frame.setAttribute("src","manual.pdf");
  }
  if(tab===assetTab){
    /* Re-read on EVERY visit, not just the first. The manifest can change under
       a long-lived builder -- edited by hand, or saved from a second tab -- and
       a configuration read once at page load would quietly disagree with it.
       Skipped while there are unsaved changes, which a reload would discard. */
    if(!assetsLoaded || assetBar.dataset.dirty!=="true") loadAssetConfiguration();
    loadAudioPreviewStatus();
  }
  if(tab===buildTab) buildTab.removeAttribute("data-badge");
  if(tab.id==="tab-home") loadHomeProjects();
  if(tab!==assetTab) assetForm.querySelectorAll("audio").forEach(audio=>audio.pause());
  if(history&&location.hash!=="#"+tab.id.slice(4)) window.history.pushState(null,"","#"+tab.id.slice(4));
  if(activeTab!==tab) window.scrollTo(0,scrollPositions.get(tab.id)||0);
  activeTab=tab;
  if(focus) tab.focus({preventScroll:true});
  document.dispatchEvent(new Event("workshop:navigate"));
  return true;
}
tabs.forEach(t=>t.addEventListener("click",()=>selectTab(t)));
document.querySelectorAll("[data-nav]").forEach(button=>button.addEventListener("click",()=>selectTab(document.getElementById("tab-"+button.dataset.nav))));
let routed=false;
function route(){
  const tab=tabs.find(t=>"#"+t.id.slice(4)===location.hash);
  // Ignore anchors such as the skip link; never treat the URL as a selector.
  if(tab || !location.hash){
    let target=tab||tabs[0];
    if(target===languageTab&&!localizationChecked) return;
    if(target.disabled){
      target=buildTab;
      window.history.replaceState(null,"","#build");
    }
    if(routed&&target===activeTab) return;
    routed=true; selectTab(target,{history:false,focus:false});
  }
}
window.addEventListener("popstate",route);
window.addEventListener("hashchange",route);
/* Both orientations work when the sidebar becomes a row on narrow screens. */
document.querySelector(".tabs").addEventListener("keydown",event=>{
  const available=tabs.filter(t=>!t.disabled);
  const i=available.indexOf(document.activeElement);
  if(i<0) return;
  if(["ArrowRight","ArrowLeft","ArrowDown","ArrowUp","Home","End"].includes(event.key)){
    event.preventDefault();
    const next=event.key==="Home"?0:event.key==="End"?available.length-1:(i+(["ArrowRight","ArrowDown"].includes(event.key)?1:available.length-1))%available.length;
    selectTab(available[next]);
  }
});
const compactNav=matchMedia("(max-width: 800px)");
function navOrientation(){ document.querySelector(".tabs").setAttribute("aria-orientation",compactNav.matches?"horizontal":"vertical"); }
compactNav.addEventListener("change",navOrientation); navOrientation();

function show(kind,text){
  ui.unbind(state); ui.unbind(workspaceStatus);
  state.dataset.kind=kind; state.textContent=text;
  workspaceStatus.dataset.kind=kind; workspaceStatus.textContent=text; workspaceStatus.title=text;
}
function showKey(kind,key,args={}) {
  show(kind,ui.text(key,args));
  ui.set(state,key,args); ui.set(workspaceStatus,key,args);
}
document.addEventListener("workshop:language",()=>{
  workspaceStatus.title=workspaceStatus.textContent;
  if(lastPaint) paint(lastPaint.progress,lastPaint.kind);
});

/* List summaries are supplied by Go. This is navigation only: pack contents,
 * authoring status, validation and installation still belong to localization. */
function projectCards(host,rows,limit=Infinity){
  const visible=rows.slice(0,limit);
  const section=document.querySelector("#home-translations");
  if(!visible.length){
    host.replaceChildren(); section.hidden=true; return;
  }
  section.hidden=false;
  host.replaceChildren(...visible.map(row=>{
    const card=document.createElement("button"); card.type="button"; card.className="project-card";
    card.disabled=!!row.error; card.dataset.projectId=row.id;
    const badge=document.createElement("span"); badge.className="project-monogram"; badge.setAttribute("aria-hidden","true"); badge.textContent=row.locale||"文";
    const copy=document.createElement("span"); copy.className="project-copy";
    const title=document.createElement("strong"); title.textContent=row.name;
    const note=document.createElement("small");
    if(row.error) note.textContent=row.error; else ui.set(note,"builder.home.project_copy",{id:row.id});
    copy.append(title,note);
    const arrow=document.createElement("span"); arrow.className="project-arrow"; arrow.textContent="↗"; arrow.setAttribute("aria-hidden","true");
    card.append(badge,copy,arrow);
    card.addEventListener("click",()=>{
      if(selectTab(languageTab,{activate:false})) window.localizationOpenProject?.(row.id);
    });
    return card;
  }));
}
let homeRequest=0;
async function loadHomeProjects(){
  if(closed||!localizationReady){ document.querySelector("#home-translations").hidden=true; return; }
  const request=++homeRequest;
  try {
    const rows=await responseJSON(await fetch("localization/projects",{cache:"no-store"}));
    if(!closed&&localizationReady&&request===homeRequest) projectCards(document.querySelector("#home-projects"),rows,4);
  } catch(error){
    if(!closed&&localizationReady&&request===homeRequest){
      const note=document.createElement("p"); note.className="empty-note"; ui.set(note,"builder.home.projects_failed",{detail:error.message});
      document.querySelector("#home-projects").replaceChildren(note);
      document.querySelector("#home-translations").hidden=false;
    }
  }
}

function applyLocalizationAvailability(data){
  const first=!localizationChecked, ready=data.localizationReady===true;
  const changed=first||ready!==localizationReady;
  localizationChecked=true; localizationReady=ready;
  const instruction=data.localizationError
    ?"builder.language.repair"
    :data.state==="building"
    ?"builder.language.building"
    :data.install?.canRebuild
      ?"builder.language.build_required"
      :"builder.language.restore_required";
  for(const el of document.querySelectorAll("[data-language-status]")) {
    el.hidden=ready;
    if(!ready) ui.set(el,instruction);
    el.title=data.localizationError||""; // Raw diagnostic, not an actionable instruction.
  }
  languageTab.disabled=!ready;
  languageTab.setAttribute("data-i18n-title",ready?"builder.language.available_title":"builder.language.unavailable_title"); ui.apply(languageTab);
  for(const el of document.querySelectorAll('[data-nav="localization"]')){ el.disabled=!ready; el.setAttribute("data-i18n-title",languageTab.getAttribute("data-i18n-title")); ui.apply(el); }
  if(changed){
    if(ready) loadHomeProjects();
    else {
      ++homeRequest; // Ignore a library response already in flight.
      document.querySelector("#home-projects").replaceChildren();
      document.querySelector("#home-translations").hidden=true;
      if(activeTab===languageTab) selectTab(buildTab);
    }
  }
  if(first) route(); // Resolve a direct #localization link only after checking.
}

/* Keep sticky editor/asset controls below the header even when it wraps at
 * narrow widths or increased text scaling. No fixed footer covers the editor. */
const workspaceHeader=document.querySelector(".workspace-bar");
function syncHeaderHeight(){
  document.documentElement.style.setProperty("--workspace-header-h",workspaceHeader.offsetHeight+"px");
}
if(window.ResizeObserver) new ResizeObserver(syncHeaderHeight).observe(workspaceHeader);
else window.addEventListener("resize",syncHeaderHeight);
syncHeaderHeight();

/* Track navigation hides, never recreates or disables, the upload controls.
 * Files and region drafts remain in the single form when switching tracks. */
let selectedTrack=assetRows.find(row=>!row.dataset.variant)?.dataset.track;
const trackButtons=new Map();
const trackList=document.querySelector("#asset-track-list");
const trackToggle=document.querySelector("#asset-track-toggle"), trackOptions=document.querySelector("#asset-track-options"), trackSearch=document.querySelector("#asset-search");
function closeTrackPicker(focus=false){ trackOptions.hidden=true; trackToggle.setAttribute("aria-expanded","false"); if(focus) trackToggle.focus(); }
function filterTracks(){
  const query=trackSearch.value.trim().toLocaleLowerCase(ui.locale);
  for(const button of trackButtons.values()) button.hidden=![button.textContent,button.dataset.track,button.dataset.nativeName]
    .some(text=>text.toLocaleLowerCase(ui.locale).includes(query));
  document.querySelector("#asset-search-empty").hidden=[...trackButtons.values()].some(button=>!button.hidden);
}
trackToggle.addEventListener("click",()=>{
  if(!trackOptions.hidden){ closeTrackPicker(); return; }
  trackOptions.hidden=false; trackToggle.setAttribute("aria-expanded","true"); trackSearch.value=""; filterTracks(); trackSearch.focus();
});
document.addEventListener("pointerdown",event=>{ if(!event.target.closest(".asset-track-picker")) closeTrackPicker(); });
document.querySelector(".asset-track-picker").addEventListener("focusout",event=>{ if(!event.currentTarget.contains(event.relatedTarget)) closeTrackPicker(); });
trackOptions.addEventListener("keydown",event=>{
  const visible=[...trackButtons.values()].filter(button=>!button.hidden);
  if(event.key==="Escape"){ event.preventDefault(); closeTrackPicker(true); }
  else if(event.key==="Enter"&&event.target===trackSearch){ event.preventDefault(); if(visible.length===1) visible[0].click(); else visible[0]?.focus(); }
  else if(["ArrowDown","ArrowUp","Home","End"].includes(event.key)){
    if(event.target===trackSearch&&["Home","End"].includes(event.key)) return;
    event.preventDefault(); if(!visible.length) return;
    const at=visible.indexOf(document.activeElement);
    const next=event.key==="Home"?0:event.key==="End"?visible.length-1:event.key==="ArrowDown"?(at+1)%visible.length:(at<=0?visible.length-1:at-1);
    visible[next]?.focus();
  }
});
for(const row of assetRows.filter(row=>!row.dataset.variant)){
  const button=document.createElement("button"); button.type="button"; button.className="asset-track-button";
  const label=row.querySelector(".asset-copy label");
  button.dataset.nativeName=label.textContent;
  const key="builder.assets.track."+row.dataset.track.replaceAll("-","_");
  ui.set(label,key); ui.set(button,key);
  ui.set(row.querySelector(".asset-source"),"builder.assets.source",{id:row.dataset.track,source:row.dataset.source});
  button.dataset.track=row.dataset.track;
  button.addEventListener("click",()=>{ selectedTrack=row.dataset.track; syncAssetSelection(); closeTrackPicker(true); });
  trackButtons.set(row.dataset.track,button); trackList.append(button);
}
function syncAssetSelection(){
  for(const row of assetRows.filter(row=>!row.dataset.variant)){
    const on=row.dataset.track===selectedTrack; row.hidden=!on;
    const sibling=row.nextElementSibling;
    const host=sibling?.classList.contains("asset-variants")?sibling:null;
    if(host) host.hidden=!on;
    if(!on){ row.querySelectorAll("audio").forEach(audio=>audio.pause()); host?.querySelectorAll("audio").forEach(audio=>audio.pause()); }
    const button=trackButtons.get(row.dataset.track);
    button?.setAttribute("aria-current",String(on));
    if(on&&button) ui.set(document.querySelector("#asset-selected-track"),button.dataset.i18n);
    if(button) button.dataset.dirty=String(!!row.dataset.pending || !!host?.querySelector('[data-pending="pending"],[data-pending="removed"],[data-pending-split="true"]') || row.querySelector(".split-change")?.value==="1");
  }
}
trackSearch.addEventListener("input",filterTracks);
// Bindings refresh labels in place. Only re-filter after a language change:
// repainting configuration/players here would discard drafts or pause audio.
document.addEventListener("workshop:language",filterTracks);
document.querySelectorAll("[data-asset-category]").forEach(button=>button.addEventListener("click",()=>{
  const category=button.dataset.assetCategory;
  document.querySelectorAll("[data-asset-category]").forEach(b=>b.setAttribute("aria-pressed",String(b===button)));
  document.querySelector("#asset-music-view").hidden=category!=="music";
  document.querySelector("#asset-title-view").hidden=category!=="title";
  if(category!=="music") assetForm.querySelectorAll("audio").forEach(audio=>audio.pause());
}));
syncAssetSelection();
window.addEventListener("beforeunload",event=>{
  if(!closed&&assetBar.dataset.dirty==="true"){ event.preventDefault(); event.returnValue=""; }
});

/* ASSETS. File inputs cannot be populated by a web page, so each row carries a
 * separate installed-file label. Choosing a file only changes that slot after
 * Save; the title's dirty bit similarly prevents an unrelated music save from
 * changing a pre-existing custom title mapping.
 *
 * Each row is one of four states, and the label and the revert button are both
 * derived from it rather than set at each call site -- the states are reachable
 * in any order (pick a file, undo it, revert the installed one, undo THAT), and
 * writing the label independently at every transition is how those paths
 * disagree:
 *   none      no replacement, nothing pending
 *   installed a saved replacement is in the manifest
 *   pending   a file is chosen but not yet saved
 *   removed   a saved replacement is queued for removal
 */
function rowState(row){
  if(row.querySelector("input[type=file]").files.length) return "pending";
  if(row.querySelector(".asset-remove").value==="1") return "removed";
  return row.dataset.installed==="true" ? "installed" : "none";
}

/* The row's player follows the row's STATE. It used to be driven only by the
 * file input's change event, which meant a replacement could be heard exactly
 * once -- in the session that uploaded it. Re-opening the builder showed
 * "Installed: ..." beside a player with no source, and the only way to hear
 * what was actually installed was to upload the file again. An installed
 * replacement is served back from game-assets instead. */
function paintRowAudio(row,state){
  const audio=row.querySelector(".replacement-audio");
  const caption=row.querySelector(".replacement-caption");
  if(state==="pending"){
    ui.set(caption,"builder.assets.selected_replacement");
    audio.hidden=false;   /* src is the object URL the change handler made */
    return;
  }
  const url=(state==="installed")?(row.dataset.installedUrl||""):"";
  /* "Selected" is the prospective label for a slot with nothing in it -- it
     names where a pick would appear. Only a slot that HAS one says installed. */
  ui.set(caption,url?"builder.assets.installed_replacement":"builder.assets.selected_replacement");
  if(!url){
    audio.pause();
    audio.hidden=true;
    audio.removeAttribute("src");
    return;
  }
  if(audio.getAttribute("src")!==url){
    audio.setAttribute("src",url);
    audio.load();
  }
  audio.hidden=false;
}

function paintRow(row){
  const state=rowState(row), label=row.querySelector(".asset-current");
  const clear=row.querySelector(".asset-clear");
  row.dataset.pending=(state==="pending"||state==="removed")?state:"";
  paintRowAudio(row,state);
  if(state==="pending"){
    label.dataset.configured="pending";
    ui.set(label,"builder.assets.selected_file",{file:row.querySelector("input[type=file]").files[0].name});
    clear.hidden=false; ui.set(clear,"builder.assets.cancel");
  } else if(state==="removed"){
    label.dataset.configured="removed";
    ui.set(label,"builder.assets.reverting");
    clear.hidden=false; ui.set(clear,"builder.assets.keep_replacement");
  } else if(state==="installed"){
    label.dataset.configured="true";
    ui.set(label,row.dataset.installedFile?"builder.assets.installed_file":"builder.assets.installed_replacement",{file:row.dataset.installedFile});
    clear.hidden=false; ui.set(clear,"builder.assets.use_original");
  } else if(row.dataset.pendingSplit==="true"){
    label.dataset.configured="pending";
    ui.set(label,"builder.assets.new_split");
    clear.hidden=true;
  } else {
    label.dataset.configured="false";
    ui.set(label,"builder.assets.using_original");
    clear.hidden=true;
  }
}

/* One place decides whether there is anything to save, so the two Save buttons,
 * Discard, and the toolbar's own wording can never disagree about it. */
function refreshAssetDirtyState(){
  let changes=0;
  if(titleChange.value==="1") changes++;
  assetRows.forEach(row=>{
    if(row.dataset.pending) changes++;
    const split=row.querySelector(".split-change");
    if(split&&split.value==="1") changes++;
  });
  const dirty=changes>0;
  assetBar.dataset.dirty=String(dirty);
  saveAssetsTop.disabled=!dirty;
  discardAssets.disabled=!dirty;
  ui.set(assetBarNote,dirty?"builder.assets.unsaved":"builder.assets.no_changes",{count:changes});
  syncAssetSelection();
}

/* Discarding drops every pending edit without touching what is installed: the
 * file inputs and the revert flags are page state, so clearing them is enough
 * and no request is needed. */
function discardAssetChanges(){
  titleChange.value="0";
  titleToggle.checked=assetBaseline.title;
  assetRows.forEach(row=>{
    clearRowSelection(row);
    row.querySelector(".asset-remove").value="0";
    const split=row.querySelector(".split-change");
    if(split&&split.value==="1"){
      split.value="0";
      row.querySelectorAll(".split-region input").forEach(box=>{
        box.checked=box.dataset.enabled==="true";
      });
    }
    paintRow(row);
  });
  refreshAssetDirtyState();
}

/* Drops the PENDING selection only. The player is left to paintRow, which will
 * put the installed replacement back if the row still has one -- clearing a
 * pick should reveal what is installed, not blank the row. */
function clearRowSelection(row){
  const input=row.querySelector("input[type=file]"), audio=row.querySelector(".replacement-audio");
  input.value="";
  window.workshopFileInputs?.refresh(input);
  if(audio.dataset.objectUrl){
    URL.revokeObjectURL(audio.dataset.objectUrl);
    delete audio.dataset.objectUrl;
  }
  audio.pause();
  audio.removeAttribute("src");
}

let assetBaseline={title:false};
/* The configuration the page is currently showing. Kept because a split
 * checkbox has to rebuild that slot's rows without asking the server again --
 * the record it would create is already described in this payload. */
let lastAssetConfig={tracks:[]};

function paintAssetConfiguration(config){
  titleToggle.checked=!!(config.title&&config.title.enabled);
  titleChange.value="0";
  assetBaseline={title:titleToggle.checked};
  lastAssetConfig=config;
  (config.tracks||[]).forEach(track=>{
    const row=document.querySelector('.asset-row[data-track="'+track.id+'"]');
    if(row) syncSplitPanel(row,track);
  });
  syncVariantRows(config.tracks||[]);
  const statuses=new Map((config.tracks||[]).map(track=>[track.id,track]));
  assetRows.forEach(row=>{
    /* Variant rows carry their own installed state from the manifest, set when
       they were built; only slot rows are looked up by track id. */
    if(!row.dataset.variant){
      const current=statuses.get(row.dataset.track);
      row.dataset.installed=String(!!(current&&current.configured));
      row.dataset.installedFile=(current&&current.file)||"";
      row.dataset.installedUrl=(current&&current.url)||"";
    }
    row.querySelector(".asset-remove").value="0";
    paintRow(row);
  });
  refreshAssetDirtyState();
}

async function loadAssetConfiguration(){
  assetState.hidden=false;
  assetState.dataset.kind="loading";
  ui.set(assetState,"builder.assets.loading");
  try {
    const config=await responseJSON(await fetch("assets",{cache:"no-store"}));
    // A read started on tab entry must not overwrite a change made while the
    // request was in flight. Keeping the existing draft also keeps file inputs.
    if(assetBar.dataset.dirty==="true"){
      assetState.dataset.kind="idle";
      ui.set(assetState,"builder.assets.draft_kept");
      return;
    }
    paintAssetConfiguration(config);
    assetsLoaded=true;
    assetState.dataset.kind="idle";
    assetState.hidden=true;
  } catch(error){
    assetState.hidden=false;
    assetState.dataset.kind="failed";
    ui.set(assetState,"builder.assets.load_failed",{detail:error.message});
  }
}

function paintAudioPreviewStatus(status){
  const generating=status.state==="generating";
  generatePreviews.disabled=!status.romAvailable||generating;
  if(!status.romAvailable){
    previewState.dataset.kind="idle";
    ui.set(previewState,"builder.assets.need_rom");
  } else if(generating){
    previewState.dataset.kind="loading";
    ui.set(previewState,status.current?"builder.assets.preview_progress":"builder.assets.preview_preparing",{count:status.completed||0,total:status.total||0});
  } else if(status.state==="failed"){
    previewState.dataset.kind="failed";
    ui.set(previewState,status.error?"builder.assets.preview_failed_detail":"builder.assets.preview_failed",{detail:status.error});
  } else if(status.state==="ready"){
    previewState.dataset.kind="ready";
    ui.set(previewState,"builder.assets.previews_ready");
  } else {
    previewState.dataset.kind="idle";
    ui.set(previewState,"builder.assets.rom_ready");
  }
  /* Re-rendering is only meaningful once something has been rendered, and never
     while a render is running. */
  regeneratePreviews.hidden=!(status.romAvailable&&(status.state==="ready"||status.state==="failed"));
  regeneratePreviews.disabled=generating;
  const tracks=new Map((status.tracks||[]).map(track=>[track.id,track]));
  assetRows.forEach(row=>{
    const track=tracks.get(row.dataset.track), audio=row.querySelector(".original-audio");
    if(row.dataset.variant){ audio.hidden=true; audio.removeAttribute("src"); return; }
    if(track&&track.ready&&track.url){
      /* The URL carries the rendered file's identity, so a re-render always
         arrives here as a NEW src. load() is what makes the element drop the
         bytes it already buffered for the old one. */
      if(audio.getAttribute("src")!==track.url){
        audio.setAttribute("src",track.url);
        audio.load();
      }
      audio.hidden=false;
    } else {
      audio.pause();
      audio.hidden=true;
      audio.removeAttribute("src");
    }
  });
  return generating;
}

async function loadAudioPreviewStatus(){
  clearTimeout(previewTimer);
  try {
    const status=await responseJSON(await fetch("audio-previews",{cache:"no-store"}));
    previewPolling=paintAudioPreviewStatus(status);
  } catch(error){
    previewPolling=false;
    previewState.dataset.kind="failed";
    ui.set(previewState,"builder.assets.preview_status_failed",{detail:error.message});
  }
  if(previewPolling) previewTimer=setTimeout(loadAudioPreviewStatus,500);
}

async function startAudioPreviews(force){
  generatePreviews.disabled=true;
  regeneratePreviews.disabled=true;
  previewState.dataset.kind="loading";
  ui.set(previewState,force?"builder.assets.preview_regenerating":"builder.assets.preview_starting");
  try {
    const status=await responseJSON(await fetch(
      force?"audio-previews?force=1":"audio-previews",{method:"POST"}));
    paintAudioPreviewStatus(status);
    previewPolling=true;
    previewTimer=setTimeout(loadAudioPreviewStatus,250);
  } catch(error){
    previewState.dataset.kind="failed";
    const key=["builder.assets.need_rom","builder.assets.preview_busy"].includes(error.code)
      ? error.code : "builder.assets.preview_start_failed";
    ui.set(previewState,key,{detail:error.message});
    generatePreviews.disabled=false;
    regeneratePreviews.disabled=false;
  }
}
generatePreviews.addEventListener("click",()=>startAudioPreviews(false));
regeneratePreviews.addEventListener("click",()=>startAudioPreviews(true));

titleToggle.addEventListener("change",()=>{
  /* Compared against the loaded value rather than latched: toggling twice
   * leaves the manifest as it was, and saving that would rewrite a
   * hand-authored title mapping for no reason. */
  titleChange.value=(titleToggle.checked===assetBaseline.title)?"0":"1";
  refreshAssetDirtyState();
});

function wireAssetRow(row){
  const input=row.querySelector("input[type=file]"), remove=row.querySelector(".asset-remove");
  input.addEventListener("change",()=>{
    const audio=row.querySelector(".replacement-audio");
    if(audio.dataset.objectUrl){
      URL.revokeObjectURL(audio.dataset.objectUrl);
      delete audio.dataset.objectUrl;
    }
    if(input.files.length){
      /* Choosing a file supersedes a queued revert -- the server reads the
         removal flag only when no upload accompanies it, and the row must say
         the same thing the save will do. */
      remove.value="0";
      const objectUrl=URL.createObjectURL(input.files[0]);
      audio.dataset.objectUrl=objectUrl;
      audio.src=objectUrl;
    } else {
      audio.pause();
      audio.removeAttribute("src");
    }
    paintRow(row);
    refreshAssetDirtyState();
  });
  const toggle=row.querySelector(".split-toggle");
  if(toggle){
    const panel=row.querySelector(".asset-split");
    toggle.addEventListener("click",()=>{
      const open=panel.hidden;
      panel.hidden=!open;
      toggle.setAttribute("aria-expanded",String(open));
    });
    row.querySelector(".split-regions").addEventListener("change",()=>{
      /* Compared against the loaded state rather than latched, so ticking a box
         and unticking it again leaves the manifest alone. */
      const boxes=[...row.querySelectorAll(".split-region input")];
      const moved=boxes.some(box=>String(box.checked)!==box.dataset.enabled);
      row.querySelector(".split-change").value=moved?"1":"0";
      const track=(lastAssetConfig.tracks||[])
        .find(item=>item.id===row.dataset.track);
      if(track) reconcileVariantRows(track);
      refreshAssetDirtyState();
    });
  }
  row.querySelector(".asset-clear").addEventListener("click",()=>{
    const state=rowState(row);
    if(state==="pending") clearRowSelection(row);
    else if(state==="removed") remove.value="0";
    else if(state==="installed") remove.value="1";
    paintRow(row);
    refreshAssetDirtyState();
  });
}
assetRows.forEach(wireAssetRow);

/* Built by cloning the server-rendered prototype and rewriting it through the
 * DOM, never by pasting strings: a manifest section name and its gate are the
 * author's text, not this page's, and setAttribute/textContent cannot be
 * talked into being markup.
 *
 * The clone keeps class "asset-row", so every part of the row state machine --
 * rowState, paintRow, the dirty count, discard -- applies to a variant without
 * knowing it is one. Only the field names differ, because the server manages a
 * variant by placing its FILE rather than by rewriting its record. */
/* A variant row carries its own installed state, so it has to be refreshed
 * whenever the descriptor behind it changes -- including on a row that was
 * REUSED rather than rebuilt. A save turns a pending split into a real record,
 * and a row that kept its old dataset would go on offering to create something
 * that already exists. */
function applyVariantState(row,variant){
  row.dataset.installed=String(!!variant.configured);
  row.dataset.installedFile=variant.file||"";
  row.dataset.installedUrl=variant.url||"";
  if(variant.pendingSplit) row.dataset.pendingSplit="true";
  else delete row.dataset.pendingSplit;
}

function buildVariantRow(variant){
  const row=rowPrototype.content.firstElementChild.cloneNode(true);
  const input=row.querySelector("input[type=file]");
  row.classList.add("asset-variant");
  row.style.removeProperty("--tint-h");   /* inherit the slot's, via the host */
  /* The prototype is rendered as a slot row, so the clone arrives carrying the
     placeholder track id. Left in place it makes a variant answer to a slot
     selector and pollutes any query over the slot list. */
  delete row.dataset.track;
  row.dataset.variant=variant.name;
  applyVariantState(row,variant);
  input.id="variant-"+variant.name;
  input.name="variant-"+variant.name;
  window.workshopFileInputs?.enhance(input);
  const label=row.querySelector("label");
  label.setAttribute("for",input.id);
  label.textContent=variant.name;
  const caption=row.querySelector(".asset-source");
  ui.set(caption,"builder.assets.variant_help",{file:variant.file||""});
  const gate=document.createElement("code");
  gate.className="variant-gate";
  if(variant.when) gate.textContent="when = "+variant.when;
  else ui.set(gate,"builder.assets.no_condition");
  (row.querySelector(".asset-technical")||row.querySelector(".asset-copy")).appendChild(gate);
  row.querySelector(".asset-current").id="variant-state-"+variant.name;
  row.querySelector(".asset-remove").name="variant-remove-"+variant.name;
  /* A variant is already the split of a slot; there is nothing left to divide
     it by, so the prototype's split controls are REMOVED rather than hidden --
     left in place they would also post a stray split-change field carrying the
     prototype's placeholder id. Removed before wiring, so no handler is
     attached to a control that does not exist. */
  row.querySelectorAll(".split-toggle,.asset-split,.split-change")
     .forEach(node=>node.remove());
  wireAssetRow(row);
  ui.apply(row); // Template contents are inert; new clones need their own pass.
  return row;
}

/* The region list comes from the server, which reads the map-group table the
 * game itself defines -- the page never invents a level or a WRAM address. Each
 * box remembers the state it loaded in, so unticking something back to how it
 * started stops counting as a change. */
function syncSplitPanel(row,track){
  const host=row.querySelector(".split-regions");
  if(!host) return;
  const splits=track.splits||[];
  host.replaceChildren(...splits.map(split=>{
    const item=document.createElement("div");
    item.className="split-region";
    const box=document.createElement("input");
    box.type="checkbox";
    box.name="split-"+track.id;
    box.value=split.slug;
    box.id="split-"+track.id+"-"+split.slug;
    box.checked=!!split.enabled;
    box.dataset.enabled=String(!!split.enabled);
    const label=document.createElement("label");
    label.setAttribute("for",box.id);
    const region=document.createElement("span");
    ui.set(region,"builder.assets.region."+split.slug.replaceAll("-","_"));
    label.append(region);
    if(split.acts){
      const acts=document.createElement("span");
      ui.set(acts,split.acts===3?"builder.assets.acts_both":"builder.assets.act",{act:split.acts===1?1:2});
      label.append(acts);
    }
    item.append(box,label);
    return item;
  }));
  row.querySelector(".asset-split").hidden=true;
  const toggle=row.querySelector(".split-toggle");
  if(toggle){
    toggle.setAttribute("aria-expanded","false");
    toggle.hidden=splits.length===0;
  }
  row.querySelector(".split-change").value="0";
}

/* The rows a slot should be showing: the variants the manifest already has,
 * plus one for every region ticked in the split panel that has no record yet.
 * A ticked region shows its row IMMEDIATELY -- the record and the file it needs
 * are created by the same save, so making the reader save once to reveal the
 * picker and again to fill it would be a round trip for nothing. Unticking
 * takes the row away again, which is what the save will do to the record. */
function variantDescriptors(track){
  const existing=new Map((track.variants||[]).map(v=>[v.name,v]));
  const parent=document.querySelector('.asset-row[data-track="'+track.id+'"]');
  const wanted=[];
  for(const variant of track.variants||[]) wanted.push(variant);
  for(const split of track.splits||[]){
    if(existing.has(split.name)) continue;
    const box=parent&&parent.querySelector("#split-"+track.id+"-"+split.slug);
    if(!box||!box.checked) continue;
    wanted.push({name:split.name, when:split.gate, file:split.file,
                 configured:false, url:"", pendingSplit:true});
  }
  /* A ticked-then-unticked record that DOES exist is dropped by the save, so
     drop its row too rather than leaving a row for something being removed. */
  const dropped=new Set();
  for(const split of track.splits||[]){
    if(!split.enabled) continue;
    const box=parent&&parent.querySelector("#split-"+track.id+"-"+split.slug);
    if(box&&!box.checked) dropped.add(split.name);
  }
  return wanted.filter(v=>!dropped.has(v.name));
}

/* Reconciled rather than rebuilt: a row already on the page may hold a file the
 * reader has picked but not saved, and replacing the node would silently throw
 * that away. Rows are matched by record name and only missing ones are built. */
function reconcileVariantRows(track){
  const parent=document.querySelector('.asset-row[data-track="'+track.id+'"]');
  if(!parent) return;
  let host=parent.nextElementSibling;
  if(!host||!host.classList.contains("asset-variants")){
    host=document.createElement("div");
    host.className="asset-variants";
    parent.after(host);
  }
  /* Inherited, not recomputed: a custom property set on the host reaches every
     row inside it, so a split is always the same colour as the song it splits. */
  host.style.setProperty("--tint-h",
    parent.style.getPropertyValue("--tint-h")||"210");
  const existing=new Map(
    [...host.children].map(row=>[row.dataset.variant,row]));
  const rows=variantDescriptors(track).map(variant=>{
    const row=existing.get(variant.name);
    if(!row) return buildVariantRow(variant);
    applyVariantState(row,variant);
    return row;
  });
  host.replaceChildren(...rows);
  assetRows=[...document.querySelectorAll(".asset-row")];
  /* A row built here has never been through paintRow, so it would sit showing
     the prototype's own placeholder text. Painting is derived from row state
     and idempotent, so re-painting the reused rows costs nothing and keeps one
     path responsible for what a row says. */
  rows.forEach(paintRow);
  syncAssetSelection();
}

function syncVariantRows(tracks){
  for(const track of tracks) reconcileVariantRows(track);
  assetRows=[...document.querySelectorAll(".asset-row")];
}

discardAssets.addEventListener("click",discardAssetChanges);

/* A/B means one source at a time; starting either side pauses every other
 * preview so overlapping tracks cannot make the comparison misleading. */
document.addEventListener("play",event=>{
  if(!(event.target instanceof HTMLAudioElement)) return;
  document.querySelectorAll(".audio-compare audio").forEach(audio=>{
    if(audio!==event.target) audio.pause();
  });
},true);

assetForm.addEventListener("submit",async event=>{
  event.preventDefault();
  saveAssetsTop.disabled=true; discardAssets.disabled=true;
  ui.set(assetBarNote,"builder.assets.saving");
  assetState.hidden=false;
  assetState.dataset.kind="loading";
  ui.set(assetState,"builder.assets.copying");
  try {
    const result=await responseJSON(await fetch("assets",{method:"POST",body:new FormData(assetForm)}));
    /* Clear the pickers BEFORE repainting: paintAssetConfiguration derives each
       row from what is still selected, and a file left in an input would make a
       saved row read as pending again. */
    assetRows.forEach(clearRowSelection);
    paintAssetConfiguration(result.config);
    assetState.dataset.kind="succeeded";
    ui.set(assetState,result.changed===false?"builder.assets.nothing_saved":"builder.assets.saved");
  } catch(error){
    assetState.dataset.kind="failed";
    ui.set(assetState,"builder.assets.save_failed",{detail:error.message});
  }
  refreshAssetDirtyState();
});

/* MODE. The server decides which shape the page takes (buildgui/install.go), so
 * the page cannot disagree with what the server will actually permit -- the same
 * reason the phase model lives server-side. Applied on every poll because a
 * cleanup or a finished build changes capability mid-session. */
const playBox=document.querySelector("#play-box"), playButton=document.querySelector("#play");
const playTitle=document.querySelector("#play-title"), playSub=document.querySelector("#play-sub");
const buildBox=document.querySelector("#build-box"), noBuild=document.querySelector("#nobuild");
const slimBox=document.querySelector("#slim-box"), slimButton=document.querySelector("#slim");
const slimCopy=document.querySelector("#slim-copy"), slimDone=document.querySelector("#slim-done");
const slimDismiss=document.querySelector("#slim-dismiss");
let slimDismissed=false, lastMode="", lastSceneBuildState="";

function applyMode(data){
  if(data.state!==lastSceneBuildState) {
    lastSceneBuildState=data.state;
    document.dispatchEvent(new Event("workshop:build-state"));
  }
  const mode=data.mode||"buildable", install=data.install||{};
  document.body.dataset.mode=mode;
  /* Play is hidden while a build runs: the binary it would launch is being
     overwritten. The server refuses it too (launch() checks state first) -- this
     is only so the button does not sit there inviting the click. */
  const building=(data.state==="building");
  playBox.hidden=!install.canLaunch||building;
  playButton.disabled=building||launching;
  dockLaunch.hidden=!install.canLaunch||building;
  dockLaunch.disabled=building||launching||!install.canLaunch;
  track.hidden=!["building","succeeded","failed"].includes(data.state);
  buildBox.hidden=!install.canRebuild;
  noBuild.hidden=(mode!=="launcher");
  slimDone.hidden=!data.slimDone;
  /* Offered only when the server says there is something to remove, and only
   * once the game is actually playable -- reclaiming space before there is a
   * working build would be the wrong trade. */
  slimBox.hidden=!(install.canSlim && install.canLaunch && !slimDismissed && !data.slimDone);
  if(data.slimSize) slimCopy.dataset.size=data.slimSize;
  ui.set(slimButton,data.slimSize?"builder.build.cleanup_size":"builder.build.cleanup",{size:data.slimSize});
  if(mode==="launcher"){
    ui.set(playTitle,"builder.build.ready");
    ui.set(playSub,"builder.build.launcher_only");
  } else if(install.canLaunch && data.state!=="building"){
    ui.set(playTitle,"builder.build.built");
    ui.unbind(playSub);
    playSub.textContent=install.result&&install.result.outputPath?install.result.outputPath:"";
  }
  if(mode==="unusable" && lastMode!==mode)
    showKey("failed","builder.home.unusable");
  canLaunch=!!install.canLaunch&&!building;
  homePrimary.disabled=closed||building||launching||(!canLaunch&&!install.canRebuild);
  ui.set(homePrimary,building?"builder.home.building":canLaunch?"builder.home.play":"builder.home.build");
  ui.set(document.querySelector("#home-game-note"),building?"builder.home.while_building":canLaunch?"builder.home.ready":install.canRebuild?"builder.home.rom":"builder.home.unusable");
  ui.set(document.querySelector("#build-nav-label"),canLaunch?"builder.nav.build":"builder.nav.build_game");
  lastMode=mode;
}

/* The phase list and percentage come from the server (buildgui/progress.go), so
 * the page never parses the build log itself -- one definition of the phase
 * model, and the dock cannot disagree with the step list. */
function paint(progress,kind){
  if(!progress) return;
  lastPaint={progress,kind};
  const done=new Set(progress.completed||[]);
  steps.forEach(step=>{
    const id=step.dataset.step;
    if(done.has(id)) step.dataset.state="done";
    else if(id===progress.phaseId) step.dataset.state=(kind==="failed"?"failed":"active");
    else step.removeAttribute("data-state");
  });
  const percent=Math.max(0,Math.min(100,progress.percent|0));
  dock.title=[ui.text("builder.phase."+progress.phaseId,{},progress.phaseLabel),progress.unitsTotal?ui.text("builder.build.units",{done:ui.number(progress.units),total:ui.number(progress.unitsTotal)}):""].filter(Boolean).join(" · ");
  dockPct.textContent=ui.number(percent)+"%";
  track.setAttribute("aria-valuenow",percent);
}

/* A finished build must be unmissable without hijacking the reader's tab: the
 * dock announces it (and offers Launch inline), and the Build tab gets a dot. */
function announce(kind,message){
  dock.dataset.kind=kind;
  dock.dataset.open="true";
  if(message) { ui.unbind(dockPhase); dockPhase.textContent=message; }
  if(document.querySelector("#panel-build").hidden)
    buildTab.dataset.badge=(kind==="failed"?"bad":"ok");
}

async function responseJSON(response){
  const body=await response.json();
  if(!response.ok){
    const error=new Error(body.error||"Request failed");
    error.code=body.errorCode; // Presentation code is separate from raw diagnostic details.
    throw error;
  }
  return body;
}

async function refresh(){
  if(closed) return;
  try {
    const data=await responseJSON(await fetch("status",{cache:"no-store"}));
    if(closed) return;
    if(data.log){ ui.unbind(log); log.textContent=data.log; log.scrollTop=log.scrollHeight; }
    applyMode(data);
    applyLocalizationAvailability(data);
    paint(data.progress,data.state);
    if(data.state==="building"){
      polling=true; stepsBox.hidden=false;
      showKey("building","builder.build.running");
      build.disabled=true; launch.disabled=true;
      dock.dataset.kind="building"; dock.dataset.open="true";
    }
    if(data.state==="succeeded"){
      showKey("succeeded","builder.build.complete");
      build.disabled=false; launch.disabled=false; polling=false;
      dockPct.textContent="100%";
      announce("succeeded");
    }
    if(data.state==="failed"){
      showKey("failed","builder.build.failed",{detail:data.error||""}); build.disabled=false; launch.disabled=true; polling=false;
      logBox.open=true;  /* a failure is the one time the log matters unprompted */
      announce("failed");
    }
    if(data.state==="idle"){
      /* The original copy assumed nothing was built. Say what is actually
       * true, so a returning user is not told to choose a ROM they do not
       * need. */
      const install=data.install||{};
      if(data.mode==="launcher") showKey("idle","builder.build.ready");
      else if(install.canLaunch){ showKey("idle","builder.build.ready_or_rebuild"); launch.disabled=false; }
      else if(install.canRebuild) showKey("idle","builder.build.ready_to_build");
    }
  } catch(error) { showKey("failed","builder.request_failed",{detail:error.message}); polling=false; announce("failed"); }
  if(polling) setTimeout(refresh,500);
}

form.addEventListener("submit",async event=>{
  event.preventDefault();
  if(!document.querySelector("#rom").files.length) return;
  build.disabled=true; launch.disabled=true; ui.unbind(log); log.textContent=ui.text("builder.build.preparing");
  showKey("building","builder.build.starting");
  steps.forEach(step=>step.removeAttribute("data-state"));
  stepsBox.hidden=false;   /* only worth showing once there is progress to show */
  buildTab.removeAttribute("data-badge");
  dock.dataset.kind="building"; dock.dataset.open="true";
  ui.set(dockPhase,"builder.build.preparing_rom"); dockPct.textContent="…"; track.hidden=false; dockLaunch.hidden=true;
  track.removeAttribute("aria-valuenow");
  try { await responseJSON(await fetch("build",{method:"POST",body:new FormData(form)})); polling=true; refresh(); }
  catch(error){ showKey("failed","builder.request_failed",{detail:error.message}); build.disabled=false; announce("failed"); }
});

async function doLaunch(){
  if(closed||launching) return;
  launching=true;
  const buttons=[homePrimary,playButton,launch,dockLaunch];
  const previous=buttons.map(button=>button.disabled);
  buttons.forEach(button=>button.disabled=true);
  showKey("idle","builder.build.launching");
  try { await responseJSON(await fetch("launch",{method:"POST"})); showKey("succeeded","builder.build.launched"); }
  catch(error){ showKey("failed","builder.request_failed",{detail:error.message}); }
  finally { launching=false; if(!closed) buttons.forEach((button,i)=>button.disabled=previous[i]); }
}
launch.addEventListener("click",doLaunch);
dockLaunch.addEventListener("click",doLaunch);
playButton.addEventListener("click",doLaunch);
homePrimary.addEventListener("click",()=>canLaunch?doLaunch():selectTab(buildTab));

slimDismiss.addEventListener("click",()=>{ slimDismissed=true; slimBox.hidden=true; });
slimButton.addEventListener("click",async()=>{
  slimButton.disabled=true; slimDismiss.disabled=true;
  showKey("building","builder.build.removing_tools");
  try {
    await responseJSON(await fetch("slim",{method:"POST"}));
    /* One poll settles everything: the server has re-probed, so the offer
     * disappears, the confirmation appears, and the page drops into launcher
     * mode without the page having to guess any of it. */
    await refresh();
    showKey("succeeded","builder.build.tools_removed");
  } catch(error){ showKey("failed","builder.request_failed",{detail:error.message}); }
  slimButton.disabled=false; slimDismiss.disabled=false;
});

/* First paint: ask the server what this copy can do before showing anything, so
 * a returning user never sees the build form flash past on the way to a
 * launcher. */
refresh();
route();
// Also discover a source/build prepared in another window or by the CLI.
window.addEventListener("focus",()=>{ if(!closed&&!polling) refresh(); });
closeButton.addEventListener("click",async()=>{
  if(window.localizationHasEdits?.()||assetBar.dataset.dirty==="true"){
    if(!window.confirm(ui.text("builder.close_confirm"))) return;
  }
  try {
    await responseJSON(await fetch("close",{method:"POST"}));
    showKey("idle","builder.closed");
    closed=true; polling=false; clearTimeout(previewTimer);
    document.dispatchEvent(new Event("workshop:closed"));
    document.querySelectorAll("button,input,select,textarea").forEach(control=>control.disabled=true);
    assetForm.querySelectorAll("audio").forEach(audio=>audio.pause());
    ui.set(document.querySelector("#home-game-note"),"builder.closed_note");
    document.querySelector("#scene-motion").value="still";
    document.querySelector("#scene-motion").dispatchEvent(new Event("change"));
    dock.dataset.open="false";
  } catch(error){ showKey("failed","builder.request_failed",{detail:error.message}); }
});
})();
