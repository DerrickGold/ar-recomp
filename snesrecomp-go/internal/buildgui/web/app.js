(() => {
"use strict";
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
const saveAssets=document.querySelector("#save-assets"), assetState=document.querySelector("#asset-state");
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
  document.querySelector("#workspace-section").textContent={"tab-home":"Home","tab-build":"Build & play","tab-localization":"Languages","tab-assets":"Assets","tab-manual":"Help & manual"}[tab.id];
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
  state.dataset.kind=kind; state.textContent=text;
  workspaceStatus.dataset.kind=kind; workspaceStatus.textContent=text; workspaceStatus.title=text;
}

/* List summaries are supplied by Go. This is navigation only: pack contents,
 * authoring status, validation and installation still belong to localization. */
function projectCards(host,rows,limit=Infinity){
  const visible=rows.slice(0,limit);
  if(!visible.length){
    const empty=document.createElement("p"); empty.className="empty-note";
    empty.textContent="No saved projects yet. Create a translation or import a pack to begin.";
    host.replaceChildren(empty); return;
  }
  host.replaceChildren(...visible.map(row=>{
    const card=document.createElement("button"); card.type="button"; card.className="project-card";
    card.disabled=!!row.error; card.dataset.projectId=row.id;
    const badge=document.createElement("span"); badge.className="project-monogram"; badge.setAttribute("aria-hidden","true"); badge.textContent=row.locale||"文";
    const copy=document.createElement("span"); copy.className="project-copy";
    const title=document.createElement("strong"); title.textContent=row.name;
    const note=document.createElement("small"); note.textContent=row.error||row.id+" · Workshop copy";
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
  if(closed||!localizationReady) return;
  const request=++homeRequest;
  const button=document.querySelector("#home-refresh-projects"); button.disabled=true;
  try {
    const rows=await responseJSON(await fetch("localization/projects",{cache:"no-store"}));
    if(!closed&&localizationReady&&request===homeRequest) projectCards(document.querySelector("#home-projects"),rows,4);
  } catch(error){
    if(!closed&&localizationReady&&request===homeRequest){
      const note=document.createElement("p"); note.className="empty-note"; note.textContent="Could not load projects: "+error.message;
      document.querySelector("#home-projects").replaceChildren(note);
    }
  } finally { if(!closed&&request===homeRequest) button.disabled=!localizationReady; }
}
document.querySelector("#home-refresh-projects").addEventListener("click",loadHomeProjects);

function applyLocalizationAvailability(data){
  const first=!localizationChecked, ready=data.localizationReady===true;
  const changed=first||ready!==localizationReady;
  localizationChecked=true; localizationReady=ready;
  const instruction=data.localizationError
    ?"Restore a valid backup of game-assets/languages/native-us, or move that folder aside and rebuild with your US ROM. It will not be overwritten automatically."
    :data.state==="building"
    ?"Languages will unlock automatically as soon as the native US source is ready. The rest of the build can keep running."
    :data.install?.canRebuild
      ?"The Languages section requires the native US source. Start a build with your US ROM; extraction runs first and unlocks this section before the game finishes building."
      :"The Languages section requires the native US source. Restore your generated native-us folder, or download the build tools again and build with your US ROM. Saved language projects are kept.";
  const note=ready?"Native US source ready — Languages is available.":(data.localizationError?data.localizationError+" ":"")+instruction;
  for(const el of document.querySelectorAll("[data-language-status]")) if(el.textContent!==note) el.textContent=note;
  languageTab.disabled=!ready;
  languageTab.title=ready?"Language packages and translation projects":"Requires the native US language source — see Build & play";
  for(const el of document.querySelectorAll('[data-nav="localization"]')){ el.disabled=!ready; el.title=languageTab.title; }
  if(changed){
    document.querySelector("#home-refresh-projects").disabled=!ready;
    if(ready) loadHomeProjects();
    else {
      ++homeRequest; // Ignore a library response already in flight.
      const note=document.createElement("p"); note.className="empty-note";
      note.textContent="Saved projects will appear here once the native US source is ready. Nothing has been removed.";
      document.querySelector("#home-projects").replaceChildren(note);
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
  const query=trackSearch.value.trim().toLocaleLowerCase();
  for(const button of trackButtons.values()) button.hidden=!button.textContent.toLocaleLowerCase().includes(query);
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
  button.textContent=row.querySelector(".asset-copy label").textContent;
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
    if(on&&button) document.querySelector("#asset-selected-track").textContent=button.textContent;
    if(button) button.dataset.dirty=String(!!row.dataset.pending || !!host?.querySelector('[data-pending="pending"],[data-pending="removed"],[data-pending-split="true"]') || row.querySelector(".split-change")?.value==="1");
  }
}
trackSearch.addEventListener("input",filterTracks);
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
    caption.textContent="Selected replacement";
    audio.hidden=false;   /* src is the object URL the change handler made */
    return;
  }
  const url=(state==="installed")?(row.dataset.installedUrl||""):"";
  /* "Selected" is the prospective label for a slot with nothing in it -- it
     names where a pick would appear. Only a slot that HAS one says installed. */
  caption.textContent=url?"Installed replacement":"Selected replacement";
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
    label.textContent="Selected: "+row.querySelector("input[type=file]").files[0].name;
    clear.hidden=false; clear.textContent="Cancel";
  } else if(state==="removed"){
    label.dataset.configured="removed";
    label.textContent="Reverting to the original game music on save";
    clear.hidden=false; clear.textContent="Keep replacement";
  } else if(state==="installed"){
    label.dataset.configured="true";
    label.textContent="Installed: "+(row.dataset.installedFile||"replacement");
    clear.hidden=false; clear.textContent="Use original";
  } else if(row.dataset.pendingSplit==="true"){
    label.dataset.configured="pending";
    label.textContent="New \u2014 choose a file; the entry is written when you save";
    clear.hidden=true;
  } else {
    label.dataset.configured="false";
    label.textContent="Using original game music";
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
  saveAssets.disabled=!dirty;
  saveAssetsTop.disabled=!dirty;
  discardAssets.disabled=!dirty;
  assetBarNote.textContent=dirty
    ? changes+(changes===1?" unsaved change":" unsaved changes")
    : "No unsaved changes";
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
  assetState.dataset.kind="loading";
  assetState.textContent="Loading current assets…";
  try {
    const config=await responseJSON(await fetch("assets",{cache:"no-store"}));
    // A read started on tab entry must not overwrite a change made while the
    // request was in flight. Keeping the existing draft also keeps file inputs.
    if(assetBar.dataset.dirty==="true"){
      assetState.dataset.kind="idle";
      assetState.textContent="Your unsaved changes were kept. Save or discard to refresh the installed configuration.";
      return;
    }
    paintAssetConfiguration(config);
    assetsLoaded=true;
    assetState.dataset.kind="idle";
    assetState.textContent="Ready — choose any replacements, then save.";
  } catch(error){
    assetState.dataset.kind="failed";
    assetState.textContent=error.message;
  }
}

function paintAudioPreviewStatus(status){
  const generating=status.state==="generating";
  generatePreviews.disabled=!status.romAvailable||generating;
  if(!status.romAvailable){
    previewState.dataset.kind="idle";
    previewState.textContent="Supply a ROM on the Build tab to enable original-audio comparisons.";
  } else if(generating){
    previewState.dataset.kind="loading";
    previewState.textContent=status.message||("Rendering "+(status.current||"audio")+"…");
  } else if(status.state==="failed"){
    previewState.dataset.kind="failed";
    previewState.textContent=status.error||"Original-audio preview generation failed.";
  } else if(status.state==="ready"){
    previewState.dataset.kind="ready";
    previewState.textContent=status.message||"Original ROM previews are ready.";
  } else {
    previewState.dataset.kind="idle";
    previewState.textContent="ROM ready — extract 30-second WAV previews for side-by-side listening.";
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
    previewState.textContent=error.message;
  }
  if(previewPolling) previewTimer=setTimeout(loadAudioPreviewStatus,500);
}

async function startAudioPreviews(force){
  generatePreviews.disabled=true;
  regeneratePreviews.disabled=true;
  previewState.dataset.kind="loading";
  previewState.textContent=force
    ? "Discarding the cached previews and re-rendering…"
    : "Starting the pure-Go audio renderer…";
  try {
    const status=await responseJSON(await fetch(
      force?"audio-previews?force=1":"audio-previews",{method:"POST"}));
    paintAudioPreviewStatus(status);
    previewPolling=true;
    previewTimer=setTimeout(loadAudioPreviewStatus,250);
  } catch(error){
    previewState.dataset.kind="failed";
    previewState.textContent=error.message;
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
  const label=row.querySelector("label");
  label.setAttribute("for",input.id);
  label.textContent=variant.name;
  const caption=row.querySelector(".asset-copy span");
  caption.textContent="Plays instead when its condition holds \u00b7 "+(variant.file||"");
  const gate=document.createElement("code");
  gate.className="variant-gate";
  gate.textContent=variant.when?("when = "+variant.when):"no condition — always eligible";
  row.querySelector(".asset-copy").appendChild(gate);
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
    label.textContent=split.label;
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
  saveAssets.disabled=true; saveAssetsTop.disabled=true; discardAssets.disabled=true;
  assetBarNote.textContent="Saving…";
  assetState.dataset.kind="loading";
  assetState.textContent="Copying assets and updating the manifest…";
  try {
    const result=await responseJSON(await fetch("assets",{method:"POST",body:new FormData(assetForm)}));
    /* Clear the pickers BEFORE repainting: paintAssetConfiguration derives each
       row from what is still selected, and a file left in an input would make a
       saved row read as pending again. */
    assetRows.forEach(clearRowSelection);
    paintAssetConfiguration(result.config);
    assetState.dataset.kind="succeeded";
    assetState.textContent=result.message||"Assets saved.";
  } catch(error){
    assetState.dataset.kind="failed";
    assetState.textContent=error.message;
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
  slimButton.textContent=data.slimSize
    ? "Clean up build tools ("+data.slimSize+")" : "Clean up build tools";
  if(mode==="launcher"){
    playTitle.textContent="Ready to play";
    playSub.textContent="This copy contains just the game.";
  } else if(install.canLaunch && data.state!=="building"){
    playTitle.textContent="Your game is built and ready";
    playSub.textContent=install.result&&install.result.outputPath?install.result.outputPath:"";
  }
  if(mode==="unusable" && lastMode!==mode)
    show("failed","This copy has neither a built game nor the tools to build one — download the package again.");
  canLaunch=!!install.canLaunch&&!building;
  homePrimary.disabled=closed||building||launching||(!canLaunch&&!install.canRebuild);
  homePrimary.textContent=building?"Building your game…":canLaunch?"▶  Play ActRaiser":"Build your game";
  document.querySelector("#home-game-note").textContent=building?"You can explore the workshop while the build runs.":canLaunch?"Ready when you are. Your game is installed locally.":install.canRebuild?"Start with your local US ROM. No upload required.":"This copy needs a built game or the build tools. Download the package again to get started.";
  document.querySelector("#build-nav-label").textContent=canLaunch?"Build & play":"Build game";
  lastMode=mode;
}

/* The phase list and percentage come from the server (buildgui/progress.go), so
 * the page never parses the build log itself -- one definition of the phase
 * model, and the dock cannot disagree with the step list. */
function paint(progress,kind){
  if(!progress) return;
  const done=new Set(progress.completed||[]);
  steps.forEach(step=>{
    const id=step.dataset.step;
    if(done.has(id)) step.dataset.state="done";
    else if(id===progress.phaseId) step.dataset.state=(kind==="failed"?"failed":"active");
    else step.removeAttribute("data-state");
  });
  const percent=Math.max(0,Math.min(100,progress.percent|0));
  dock.title=[progress.phaseLabel,progress.detail].filter(Boolean).join(" · ");
  dockPct.textContent=percent+"%";
  track.setAttribute("aria-valuenow",percent);
}

/* A finished build must be unmissable without hijacking the reader's tab: the
 * dock announces it (and offers Launch inline), and the Build tab gets a dot. */
function announce(kind,message){
  dock.dataset.kind=kind;
  dock.dataset.open="true";
  if(message) dockPhase.textContent=message;
  if(document.querySelector("#panel-build").hidden)
    buildTab.dataset.badge=(kind==="failed"?"bad":"ok");
}

async function responseJSON(response){ const body=await response.json(); if(!response.ok) throw new Error(body.error||"Request failed"); return body; }

async function refresh(){
  if(closed) return;
  try {
    const data=await responseJSON(await fetch("status",{cache:"no-store"}));
    if(closed) return;
    if(data.log){ log.textContent=data.log; log.scrollTop=log.scrollHeight; }
    applyMode(data);
    applyLocalizationAvailability(data);
    paint(data.progress,data.state);
    if(data.state==="building"){
      polling=true; stepsBox.hidden=false;
      show("building","Building — this can take a few minutes");
      build.disabled=true; launch.disabled=true;
      dock.dataset.kind="building"; dock.dataset.open="true";
    }
    if(data.state==="succeeded"){
      show("succeeded",data.message||"Build complete");
      build.disabled=false; launch.disabled=false; polling=false;
      dockPct.textContent="100%";
      announce("succeeded",data.message||"Build complete");
    }
    if(data.state==="failed"){
      show("failed",data.error||"Build failed"); build.disabled=false; launch.disabled=true; polling=false;
      logBox.open=true;  /* a failure is the one time the log matters unprompted */
      announce("failed",data.error||"Build failed");
    }
    if(data.state==="idle"){
      /* The original copy assumed nothing was built. Say what is actually
       * true, so a returning user is not told to choose a ROM they do not
       * need. */
      const install=data.install||{};
      if(data.mode==="launcher") show("idle","Ready to play");
      else if(install.canLaunch){ show("idle","A built game is ready — Play, or rebuild below"); launch.disabled=false; }
      else if(install.canRebuild) show("idle","Ready to build — choose your ROM above");
    }
  } catch(error) { show("failed",error.message); polling=false; announce("failed",error.message); }
  if(polling) setTimeout(refresh,500);
}

form.addEventListener("submit",async event=>{
  event.preventDefault();
  if(!document.querySelector("#rom").files.length) return;
  build.disabled=true; launch.disabled=true; log.textContent="Preparing local ROM copy…";
  show("building","Starting build");
  steps.forEach(step=>step.removeAttribute("data-state"));
  stepsBox.hidden=false;   /* only worth showing once there is progress to show */
  buildTab.removeAttribute("data-badge");
  dock.dataset.kind="building"; dock.dataset.open="true";
  dockPhase.textContent="Preparing your ROM"; dockPct.textContent="…"; track.hidden=false; dockLaunch.hidden=true;
  track.removeAttribute("aria-valuenow");
  try { await responseJSON(await fetch("build",{method:"POST",body:new FormData(form)})); polling=true; refresh(); }
  catch(error){ show("failed",error.message); build.disabled=false; announce("failed",error.message); }
});

async function doLaunch(){
  if(closed||launching) return;
  launching=true;
  const buttons=[homePrimary,playButton,launch,dockLaunch];
  const previous=buttons.map(button=>button.disabled);
  buttons.forEach(button=>button.disabled=true);
  show("idle","Launching game…");
  try { await responseJSON(await fetch("launch",{method:"POST"})); show("succeeded","Game launched"); dockPhase.textContent="Game launched"; }
  catch(error){ show("failed",error.message); }
  finally { launching=false; if(!closed) buttons.forEach((button,i)=>button.disabled=previous[i]); }
}
launch.addEventListener("click",doLaunch);
dockLaunch.addEventListener("click",doLaunch);
playButton.addEventListener("click",doLaunch);
homePrimary.addEventListener("click",()=>canLaunch?doLaunch():selectTab(buildTab));

slimDismiss.addEventListener("click",()=>{ slimDismissed=true; slimBox.hidden=true; });
slimButton.addEventListener("click",async()=>{
  slimButton.disabled=true; slimDismiss.disabled=true;
  show("building","Removing build tools…");
  try {
    await responseJSON(await fetch("slim",{method:"POST"}));
    /* One poll settles everything: the server has re-probed, so the offer
     * disappears, the confirmation appears, and the page drops into launcher
     * mode without the page having to guess any of it. */
    await refresh();
    show("succeeded","Build tools removed");
  } catch(error){ show("failed",error.message); }
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
    if(!window.confirm("Close the workshop and discard unsaved edits? Saved projects and installed assets will be kept.")) return;
  }
  try {
    await responseJSON(await fetch("close",{method:"POST"}));
    show("idle","Builder closed — you can close this tab");
    closed=true; polling=false; clearTimeout(previewTimer);
    document.dispatchEvent(new Event("workshop:closed"));
    document.querySelectorAll("button,input,select,textarea").forEach(control=>control.disabled=true);
    assetForm.querySelectorAll("audio").forEach(audio=>audio.pause());
    document.querySelector("#home-game-note").textContent="Workshop closed. You can close this browser tab.";
    document.querySelector("#scene-motion").value="still";
    document.querySelector("#scene-motion").dispatchEvent(new Event("change"));
    dock.dataset.open="false";
  } catch(error){ show("failed",error.message); }
});
})();
