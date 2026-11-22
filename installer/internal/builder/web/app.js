(() => {
"use strict";
  const fetchResponse = (...args) => window.workshopFeedback?.request ?
    window.workshopFeedback.request(...args) : fetch(...args);
const ui=window.workshopI18n;
const form=document.querySelector("#build-form"), build=document.querySelector("#build"), launch=document.querySelector("#launch");
const state=document.querySelector("#state"), log=document.querySelector("#log"), closeButton=document.querySelector("#close");
const track=document.querySelector("#track");
const buildView=document.querySelector("#build-view"), buildViewTitle=document.querySelector("#build-view-title");
const buildProgress=document.querySelector("#build-progress"), buildPhase=document.querySelector("#build-phase");
const viewBuildLog=document.querySelector("#view-build-log"), buildLogBox=document.querySelector("#log-box");
const buildViewActions=document.querySelector("#build-view-actions"), buildViewPlay=document.querySelector("#build-view-play");
const dock=document.querySelector("#dock"), dockPhase=document.querySelector("#workspace-status");
const dockPct=document.querySelector("#dock-pct");
const dockLaunch=document.querySelector("#dock-launch"), stepsBox=document.querySelector("#steps-box");
const steps=[...document.querySelectorAll(".step")];
const tabs=[...document.querySelectorAll(".tab")];
const buildTab=document.querySelector("#tab-build"), progressTab=document.querySelector("#tab-progress");
const languageTab=document.querySelector("#tab-localization");
const workshopTab=document.querySelector("#tab-workshop"), modsTab=document.querySelector("#tab-mods");
let localizationReady=false, localizationChecked=false;
const assetTab=document.querySelector("#tab-assets"), assetForm=document.querySelector("#assets-form");
const authorTabs=[assetTab,languageTab];
const workshopOverview=document.querySelector("#workshop-overview"), workshopNavigation=document.querySelector("#workshop-navigation");
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
let statusRequestID=0, statusTimer;
let assetsLoaded=false;
let previewPolling=false, previewTimer=0;
const homePrimary=document.querySelector("#home-primary"), workspaceStatus=document.querySelector("#workspace-status");
let canLaunch=false, closed=false, launching=false;
let savedROM="", buildSubmitting=false, latestModeData=null;
let selectedROM="", firstBuildAttempted=false;
const savedROMNote=document.querySelector("#saved-rom-note");
const importROMExtras=document.querySelector("#rom-import-selected");
const romChecklistSection=document.querySelector("#rom-checklist");
const buildROMChecklist=document.querySelector("#build-rom-checklist");
const bigScreenROMChecklist=document.querySelector("#big-screen-rom-checklist");
const startBigScreen=document.body.dataset.bigScreen==="true";
const desktopShell=document.body.dataset.desktopShell==="true";
let bigScreen=false;
const playSession=document.body.dataset.playSession==="true";
const closeOnLaunchInput=document.querySelector("#close-on-launch");
let closeOnLaunch=closeOnLaunchInput.checked===true;
document.querySelector("#close-on-launch-setting").hidden=playSession;
const bigActions=document.querySelector("#big-screen-actions"), bigRebuild=document.querySelector("#big-screen-rebuild");
const bigROM=document.querySelector("#big-screen-rom"), bigExit=document.querySelector("#big-screen-exit");
const bigExitLabel=document.querySelector("#big-screen-exit-label");
const bigROMActions=document.querySelector("#big-rom-actions");
const bigROMBack=document.querySelector("#big-rom-checklist-back");
const bigROMsOpen=document.querySelector("#big-screen-roms-open");
const bigProgress=document.querySelector("#big-screen-progress");
const bigCustomizeOpen=document.querySelector("#big-screen-customize-open");
const bigMenu=document.querySelector("#big-screen-menu");
const bigBuild=document.querySelector("#big-screen-build"), bigBuildPhase=document.querySelector("#big-build-phase");
const bigBuildOpen=document.querySelector("#big-screen-build-open");
const bigBuildBack=document.querySelector("#big-build-back");
const buildReport=document.querySelector("#build-report"), reportStatus=document.querySelector("#build-report-status");
const bigBuildPlay=document.querySelector("#big-build-play");
const bigLaunchDetails=document.querySelector("#big-launch-details");
let bigROMOpen=false, bigROMOpener, closing=false;
let bigBuildError="", bigBuildDismissed=false, savingReport=false;
let lastPaint=null;
let activeTab=document.querySelector("#tab-home");
let lastWorkspaceTab=activeTab;
const scrollPositions=new Map();

function isBuilding(){
  return buildSubmitting||latestModeData?.state==="building";
}

function openBuildView(){
  if(bigScreen||activeTab===progressTab) return;
  progressTab.hidden=false;
  selectTab(progressTab,{focus:false});
  log.scrollTop=log.scrollHeight;
  if(!document.querySelector("dialog[open]")) buildViewTitle.focus({preventScroll:true});
}

function updateBuildView(){
  const building=isBuilding();
  const newBuild=building&&progressTab.hidden;
  progressTab.hidden=!building&&!log.textContent&&activeTab!==progressTab;
  if(newBuild) openBuildView();
  closeButton.disabled=building||closed||launching||closing;
  buildViewActions.hidden=building;
  buildViewPlay.hidden=state.dataset.kind!=="succeeded"||latestModeData?.state!=="succeeded"||!canLaunch;
  buildViewPlay.disabled=launching||!!latestModeData?.playing;
  if(!bigScreen){
    buildReport.hidden=building||state.dataset.kind!=="failed"||bigBuildError!=="build";
    ui.set(buildReport,"builder.big_screen.save_report");
    if(buildReport.hidden) reportStatus.hidden=true;
  }
  buildProgress.hidden=!building;
  buildLogBox.hidden=!building&&!log.textContent;
  viewBuildLog.hidden=!log.textContent;
}

function selectTab(tab,{history=true,focus=true,activate=true}={}){
  if(bigScreen||!tab||tab.disabled||tab.hidden||closed) return false;
  if(activeTab!==tab) scrollPositions.set(activeTab.id,window.scrollY);
  if(romChecklistSection.parentNode!==buildROMChecklist) buildROMChecklist.append(romChecklistSection);
  const authoring=authorTabs.includes(tab);
  workshopOverview.hidden=authoring;
  workshopNavigation.hidden=!authoring;
  if(activate&&tab.id==="tab-localization") window.localizationActivate?.();
  for(const t of tabs){
    const on=t===tab||(t===workshopTab&&authoring);
    t.setAttribute("aria-selected",String(on));
    t.tabIndex=on?0:-1;
    document.querySelector("#"+t.getAttribute("aria-controls")).hidden=!on;
  }
  if(tab===assetTab){
    /* Re-read on EVERY visit, not just the first. The manifest can change under
       a long-lived builder -- edited by hand, or saved from a second tab -- and
       a configuration read once at page load would quietly disagree with it.
       Skipped while there are unsaved changes, which a reload would discard. */
    if(!assetsLoaded || assetBar.dataset.dirty!=="true") loadAssetConfiguration();
    loadAudioPreviewStatus();
  }
  if(tab===modsTab){
    legacyMods?.load();
    if(activate) assetPacks.load();
  }
  if(tab===buildTab) buildTab.removeAttribute("data-badge");
  if(tab===workshopTab) loadHomeProjects();
  if(tab===progressTab) progressTab.removeAttribute("data-badge");
  else lastWorkspaceTab=tab;
  if(tab!==assetTab) assetForm.querySelectorAll("audio").forEach(audio=>audio.pause());
  if(history&&location.hash!=="#"+tab.id.slice(4)) window.history.pushState(null,"","#"+tab.id.slice(4));
  if(activeTab!==tab) window.scrollTo(0,scrollPositions.get(tab.id)||0);
  activeTab=tab;
  if(focus) tab.focus({preventScroll:true});
  document.dispatchEvent(new Event("workshop:navigate"));
  return true;
}
tabs.forEach(t=>t.addEventListener("click",()=>selectTab(t)));
// File drops use the same navigation/readiness gate without starting a second
// asynchronous language activation while the import controller is loading.
document.querySelectorAll("[data-nav]").forEach(button=>button.addEventListener("click",()=>selectTab(document.getElementById("tab-"+button.dataset.nav))));
let routed=false;
function route(){
  if(bigScreen) return;
  const hash=location.hash==="#manual"?"#settings":location.hash;
  const tab=tabs.find(t=>"#"+t.id.slice(4)===hash);
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
function navigateTabs(event, controls) {
  const available=controls.filter(t=>!t.disabled&&!t.hidden);
  const i=available.indexOf(document.activeElement);
  if(i<0) return;
  if(["ArrowRight","ArrowLeft","ArrowDown","ArrowUp","Home","End"].includes(event.key)){
    event.preventDefault();
    const next=event.key==="Home"?0:event.key==="End"?available.length-1:(i+(["ArrowRight","ArrowDown"].includes(event.key)?1:available.length-1))%available.length;
    selectTab(available[next]);
  }
}
/* Each tab list has its own arrow-key order. */
document.querySelector(".tabs").addEventListener("keydown",event=>navigateTabs(event,tabs.filter(t=>!authorTabs.includes(t))));
workshopNavigation.addEventListener("keydown",event=>navigateTabs(event,authorTabs));
const compactNav=matchMedia("(max-width: 800px)");
function navOrientation(){ document.querySelector(".tabs").setAttribute("aria-orientation",compactNav.matches?"horizontal":"vertical"); }
compactNav.addEventListener("change",navOrientation); navOrientation();

function show(kind,text){
  ui.unbind(state); ui.unbind(workspaceStatus);
  state.dataset.kind=kind; state.textContent=text;
  workspaceStatus.dataset.kind=kind; workspaceStatus.textContent=text; workspaceStatus.title=text;
}
function showKey(kind,key,args={},error,operation="Check Builder status") {
  const message=ui.text(key,args);
  if(state.dataset.kind!==kind||state.dataset.i18n!==key||state.textContent!==message){
    show(kind,message);
    ui.set(state,key,args); ui.set(workspaceStatus,key,args);
  }
  const feedback=window.workshopFeedback;
  const buildFailure=operation==="Build game"||operation==="Start game build";
  const launchFailure=operation==="Launch game";
  if(kind==="failed"){
    feedback?.show(state,error||new Error(args.detail||ui.text(key,args)),{
      operation:buildFailure?"Build game":operation,log:buildFailure?log.textContent:"",retry:refresh,messageKey:key,messageArgs:args,
      deduplicate:true,
    });
    if(feedback) ui.set(workspaceStatus,"builder.feedback.failed");
  }else feedback?.clear(state);
  const details=document.getElementById("dock-error");if(details)details.hidden=kind!=="failed";
  if(kind==="failed"&&(buildFailure||launchFailure)){
    bigBuildDismissed=false;
    bigBuildError=launchFailure?"launch":"build";
    if(launchFailure){
      bigLaunchDetails.textContent=error?.detail||args.detail||message;
      bigLaunchDetails.scrollTop=0;
    }
    updateBigScreen();
  }
  updateBuildView();
}
document.getElementById("dock-error")?.addEventListener("click",()=>{
  if(!window.workshopFeedback?.reopen?.(state)) openBuildView();
});
viewBuildLog.addEventListener("click",openBuildView);
document.querySelector("#build-view-back").addEventListener("click",()=>{
  selectTab(state.dataset.kind==="failed"&&bigBuildError==="build"||lastWorkspaceTab.disabled?buildTab:lastWorkspaceTab);
});
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
    const arrow=document.createElement("span"); arrow.className="project-arrow"; arrow.textContent="›"; arrow.setAttribute("aria-hidden","true");
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
    const rows=await responseJSON(await fetchResponse("localization/projects",{cache:"no-store"}));
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
    if(ready){ if(activeTab===workshopTab) loadHomeProjects(); }
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
const selectedTrackName=document.querySelector("#asset-selected-track");
const selectedMusicMark=document.querySelector("#asset-selected-music");
const selectedEditMark=document.querySelector("#asset-selected-edit");
const trackToggle=document.querySelector("#asset-track-toggle"), trackOptions=document.querySelector("#asset-track-options"), trackSearch=document.querySelector("#asset-search");
function closeTrackPicker(focus=false){ trackOptions.hidden=true; trackToggle.setAttribute("aria-expanded","false"); if(focus) trackToggle.focus(); }
function filterTracks(){
  const query=trackSearch.value.trim().toLocaleLowerCase(ui.locale);
  for(const button of trackButtons.values()) button.hidden=![button.firstElementChild.textContent,button.dataset.track,button.dataset.nativeName]
    .some(text=>text.toLocaleLowerCase(ui.locale).includes(query));
  document.querySelector("#asset-search-empty").hidden=[...trackButtons.values()].some(button=>!button.hidden);
}
trackToggle.addEventListener("click",()=>{
  if(!trackOptions.hidden){ closeTrackPicker(); return; }
  trackOptions.hidden=false; trackToggle.setAttribute("aria-expanded","true"); trackSearch.value=""; filterTracks(); trackSearch.focus();
});
// WebKit can blur the search field without focusing a mouse-clicked button.
// Wait for focus to arrive outside: closing on focusout would hide the option
// before its click can select the track.
for(const type of ["pointerdown","focusin"]){
  document.addEventListener(type,event=>{ if(!event.target.closest(".asset-track-picker")) closeTrackPicker(); });
}
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
  const name=document.createElement("span");
  const music=document.createElement("small");
  const edited=document.createElement("small");
  music.className="asset-track-music";
  edited.className="asset-track-edited";
  ui.set(name,key);
  ui.set(edited,"builder.assets.track_unsaved");
  button.append(name,music,edited);
  ui.set(label,"builder.assets.replacement_file");
  ui.set(row.querySelector(".asset-source"),"builder.assets.source",{id:row.dataset.track,source:row.dataset.source});
  button.dataset.track=row.dataset.track;
  // WebKit may focus the enclosing tabindex=-1 workspace instead of this
  // button on mouse down. Keep focus in the picker until click selects it.
  // Use mousedown so touch scrolling and keyboard activation keep working.
  button.addEventListener("mousedown",event=>{ if(event.button===0) event.preventDefault(); });
  button.addEventListener("click",()=>{ selectedTrack=row.dataset.track; syncAssetSelection(); closeTrackPicker(true); });
  trackButtons.set(row.dataset.track,button); trackList.append(button);
}
function hasDraftAudio(row){
  const state=rowState(row);
  return state==="installed"||state==="pending";
}

function showTrackMarks(music,edited,musicKey,dirty){
  music.hidden=!musicKey;
  if(musicKey && music.dataset.i18n!==musicKey) ui.set(music,musicKey);
  edited.hidden=!dirty;
}

function syncAssetSelection(){
  for(const row of assetRows.filter(row=>!row.dataset.variant)){
    const on=row.dataset.track===selectedTrack; row.hidden=!on;
    const sibling=row.nextElementSibling;
    const host=sibling?.classList.contains("asset-variants")?sibling:null;
    if(host) host.hidden=!on;
    if(!on){ row.querySelectorAll("audio").forEach(audio=>audio.pause()); host?.querySelectorAll("audio").forEach(audio=>audio.pause()); }
    const button=trackButtons.get(row.dataset.track);
    const variants=host ? Array.from(host.children) : [];
    let musicKey="";
    if(hasDraftAudio(row)) musicKey="builder.assets.track_custom";
    else if(variants.some(hasDraftAudio)) musicKey="builder.assets.track_regional";
    const dirty=!!row.dataset.pending || row.querySelector(".split-change")?.value==="1" ||
      variants.some(variant=>variant.dataset.pending||variant.dataset.pendingSplit==="true");
    const [name,music,edited]=button.children;
    button.setAttribute("aria-current",String(on));
    showTrackMarks(music,edited,musicKey,dirty);
    if(on){
      if(selectedTrackName.dataset.i18n!==name.dataset.i18n) ui.set(selectedTrackName,name.dataset.i18n);
      showTrackMarks(selectedMusicMark,selectedEditMark,musicKey,dirty);
    }
  }
}
trackSearch.addEventListener("input",filterTracks);
// Bindings refresh labels in place. Only re-filter after a language change:
// repainting configuration/players here would discard drafts or pause audio.
document.addEventListener("workshop:language",filterTracks);
syncAssetSelection();



/* ASSETS. File inputs cannot be populated by a web page, so each row carries a
 * separate installed-file label. Choosing a file only changes that slot after
 * Save.
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
  ui.set(caption,url?"builder.assets.draft_replacement":"builder.assets.selected_replacement");
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
    ui.set(label,"builder.assets.draft_removing");
    clear.hidden=false; ui.set(clear,"builder.assets.keep_replacement");
  } else if(state==="installed"){
    label.dataset.configured="true";
    ui.set(label,row.dataset.installedFile?"builder.assets.draft_file":"builder.assets.draft_replacement",{file:row.dataset.installedFile});
    clear.hidden=false; ui.set(clear,"builder.assets.draft_remove");
  } else if(row.dataset.pendingSplit==="true"){
    label.dataset.configured="pending";
    ui.set(label,"builder.assets.new_split");
    clear.hidden=true;
  } else {
    label.dataset.configured="false";
    ui.set(label,"builder.assets.draft_empty");
    clear.hidden=true;
  }
}

/* One place decides whether there is anything to save, so the two Save buttons,
 * Discard, and the toolbar's own wording can never disagree about it. */
const packageMusic = document.querySelector("#asset-pack-export-open");
const importMusicButton = document.getElementById("asset-pack-music-import-open");
let savingMusicDraft = false;
let importGeneration = 0;
function refreshAssetDirtyState(){
  let changes=0;
  assetRows.forEach(row=>{
    if(row.dataset.pending) changes++;
    const split=row.querySelector(".split-change");
    if(split&&split.value==="1") changes++;
  });
  const dirty=changes>0;
  assetBar.dataset.dirty=String(dirty);
  saveAssetsTop.disabled=!dirty||savingMusicDraft;
  discardAssets.hidden=!dirty;
  discardAssets.disabled=!dirty||savingMusicDraft;
  packageMusic.disabled=savingMusicDraft;
  importMusicButton.disabled=savingMusicDraft;
  ui.set(packageMusic,dirty?"builder.mods.save_draft_and_continue":"builder.mods.install_export");
  ui.set(assetBarNote,dirty?"builder.assets.unsaved":"builder.assets.no_changes",{count:changes});
  syncAssetSelection();
}

/* Discarding drops every pending edit without touching what is installed: the
 * file inputs and the revert flags are page state, so clearing them is enough
 * and no request is needed. */
async function discardAssetChanges(){
  const targets=new Set();
  const changedSplits=new Set();
  for(const row of assetRows){
    if(row.dataset.pending) targets.add(row.dataset.variant||row.dataset.track);
    if(row.querySelector(".split-change")?.value==="1"){
      changedSplits.add(row.dataset.track);
      for(const box of row.querySelectorAll(".split-region input")){
        if(String(box.checked)!==box.dataset.enabled) targets.add(row.dataset.track+"-"+box.value);
      }
    }
  }
  if(!targets.size || !await window.workshopConfirm("builder.assets.discard_all", "builder.assets.discard_tracks",
    {tracks:[...targets].map(window.workshopModPacks.targetName).join("\n")})) return;
  assetRows.forEach(row=>{
    if(row.dataset.pending){
      clearRowSelection(row);
      row.querySelector(".asset-remove").value="0";
      paintRow(row);
    }
    const split=row.querySelector(".split-change");
    if(split&&split.value==="1"){
      split.value="0";
      row.querySelectorAll(".split-region input").forEach(box=>{
        box.checked=box.dataset.enabled==="true";
      });
    }
  });
  for(const track of lastAssetConfig.tracks||[]){
    if(changedSplits.has(track.id)) reconcileVariantRows(track);
  }
  refreshAssetDirtyState();
  trackToggle.focus();
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

/* The configuration the page is currently showing. Kept because a split
 * checkbox has to rebuild that slot's rows without asking the server again --
 * the record it would create is already described in this payload. */
let lastAssetConfig={tracks:[]};

function paintAssetConfiguration(config){
  lastAssetConfig=config;
  const heading=document.querySelector("#asset-bar-title");
  if(config.source) ui.set(heading,"builder.mods.editing_pack",{name:config.source.name});
  else ui.set(heading,"builder.assets.music_draft");
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
  const generation=importGeneration;
  window.workshopFeedback?.clear(assetState);
  assetState.hidden=false;
  assetState.dataset.kind="loading";
  ui.set(assetState,"builder.assets.loading");
  try {
    const config=await responseJSON(await fetchResponse("music-draft",{cache:"no-store"}));
    if(generation!==importGeneration) return;
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
    window.workshopFeedback?.show(assetState,error,{operation:"Load asset configuration",retry:loadAssetConfiguration});
  }
}

function paintAudioPreviewStatus(status){
  window.workshopFeedback?.clear(previewState);
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
    window.workshopFeedback?.show(previewState,new Error(status.error||"Audio preview generation failed"),{operation:"Extract audio previews"});
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
    const status=await responseJSON(await fetchResponse("audio-previews",{cache:"no-store"}));
    previewPolling=paintAudioPreviewStatus(status);
  } catch(error){
    previewPolling=false;
    previewState.dataset.kind="failed";
    ui.set(previewState,"builder.assets.preview_status_failed",{detail:error.message});
    window.workshopFeedback?.show(previewState,error,{operation:"Read audio preview status",retry:loadAudioPreviewStatus});
  }
  if(previewPolling) previewTimer=setTimeout(loadAudioPreviewStatus,500);
}

async function startAudioPreviews(force){
  window.workshopFeedback?.clear(previewState);
  generatePreviews.disabled=true;
  regeneratePreviews.disabled=true;
  previewState.dataset.kind="loading";
  ui.set(previewState,force?"builder.assets.preview_regenerating":"builder.assets.preview_starting");
  try {
    const status=await responseJSON(await fetchResponse(
      force?"audio-previews?force=1":"audio-previews",{method:"POST"}));
    paintAudioPreviewStatus(status);
    previewPolling=true;
    previewTimer=setTimeout(loadAudioPreviewStatus,250);
  } catch(error){
    previewState.dataset.kind="failed";
    const key=["builder.assets.need_rom","builder.assets.preview_busy"].includes(error.code)
      ? error.code : "builder.assets.preview_start_failed";
    ui.set(previewState,key,{detail:error.message});
    window.workshopFeedback?.show(previewState,error,{operation:"Start audio previews"});
    generatePreviews.disabled=false;
    regeneratePreviews.disabled=false;
  }
}
generatePreviews.addEventListener("click",()=>startAudioPreviews(false));
regeneratePreviews.addEventListener("click",()=>startAudioPreviews(true));

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

async function saveMusicDraft(){
  if(savingMusicDraft||assetForm.reportValidity?.()===false) return false;
  if(assetBar.dataset.dirty!=="true") return true;
  savingMusicDraft=true;
  const body=new FormData(assetForm);
  const controls=[...assetForm.querySelectorAll("input[type=file],.split-region input,.asset-clear")]
    .filter(control=>!control.disabled);
  for(const control of controls) control.disabled=true;
  window.workshopFeedback?.clear(assetState);
  packageMusic.disabled=importMusicButton.disabled=true;
  saveAssetsTop.disabled=true; discardAssets.disabled=true;
  ui.set(assetBarNote,"builder.assets.saving");
  assetState.hidden=false;
  assetState.dataset.kind="loading";
  ui.set(assetState,"builder.assets.copying");
  try {
    const result=await responseJSON(await fetchResponse("music-draft",{method:"POST",body}));
    assetRows.forEach(clearRowSelection);
    paintAssetConfiguration(result.config);
    assetState.dataset.kind="succeeded";
    ui.set(assetState,result.changed===false?"builder.assets.nothing_saved":"builder.assets.draft_saved");
    return true;
  } catch(error){
    assetState.dataset.kind="failed";
    ui.set(assetState,"builder.assets.save_failed",{detail:error.message});
    window.workshopFeedback?.show(assetState,error,{operation:"Save music draft"});
    return false;
  } finally {
    for(const control of controls) control.disabled=false;
    savingMusicDraft=false;
    refreshAssetDirtyState();
  }
}
assetForm.addEventListener("submit",event=>{
  event.preventDefault();
  return saveMusicDraft();
});
packageMusic.addEventListener("click",async ()=>{
  const trackID=selectedTrack;
  if(await saveMusicDraft()&&activeTab===assetTab&&!closed) {
    packageMusic.focus();
    await publishing.openMusic(trackID,lastAssetConfig.source);
  }
});

/* MODE. The server decides which shape the page takes (builder/install.go), so
 * the page cannot disagree with what the server will actually permit -- the same
 * reason the phase model lives server-side. Applied on every poll because a
 * cleanup or a finished build changes capability mid-session. */
const playBox=document.querySelector("#play-box"), playButton=document.querySelector("#play");
const playLabels=new Map([
  [playButton,"builder.build.play"], [launch,"builder.build.launch"],
  [dockLaunch,"builder.run"], [buildViewPlay,"builder.run"], [bigBuildPlay,"builder.home.play"],
]);
function setLaunching(value){
  launching=value;
  document.body.dataset.launching=String(value);
  for(const [button,label] of playLabels) ui.set(button,value?"builder.launch.starting":label);
  if(value) ui.set(homePrimary,"builder.launch.starting");
  updateBuildView();
}
const playTitle=document.querySelector("#play-title"), playSub=document.querySelector("#play-sub");
const buildBox=document.querySelector("#build-box"), noBuild=document.querySelector("#nobuild");
const slimBox=document.querySelector("#slim-box"), slimButton=document.querySelector("#slim");
const slimCopy=document.querySelector("#slim-copy"), slimDone=document.querySelector("#slim-done");
const slimDismiss=document.querySelector("#slim-dismiss");
let slimDismissed=false, lastMode="", lastSceneBuildState="";

const versionNotice=document.querySelector("#game-version-notice"), versionReview=document.querySelector("#game-version-rebuild");
function coreROM(){ return selectedROM||savedROM; }
function romSetupKey(){
  if(romChecklist.isSelecting()) return "builder.roms.checking_files";
  return coreROM()?"builder.home.rom_ready":"builder.home.rom";
}

function preferredROMRow(){
  if(coreROM()||latestModeData?.install?.canLaunch){
    const extra=bigScreenROMChecklist.querySelector('.rom-slot:not(#rom-slot-us):not(:disabled)');
    if(extra) return extra;
  }
  return document.getElementById("rom-slot-us");
}

function updateBigScreen(){
  if(!bigScreen) return;
  const data=latestModeData, install=data?.install||{};
  const building=isBuilding(), playing=!!data?.playing;
  const busy=closed||closing||building||playing||launching||romChecklist.isSelecting();
  const rebuild=install.build?.state==="rebuild"||install.build?.state==="unknown";
  const hasBuild=building||bigBuildError||!!log.textContent;
  const buildPanel=hasBuild&&!customize.isOpen()&&!bigBuildDismissed;
  const launchError=buildPanel&&bigBuildError==="launch";
  const buildFailed=data?.state==="failed"||bigBuildError==="build";
  const buildComplete=buildPanel&&!launchError&&canLaunch&&data?.state==="succeeded";
  const playJustAppeared=buildComplete&&bigBuildPlay.hidden;
  bigBuild.hidden=!buildPanel;
  bigBuildPlay.hidden=!buildComplete;
  bigBuildPlay.disabled=busy;
  buildReport.hidden=!buildPanel||building||!(buildFailed||launchError);
  document.body.dataset.bigBuild=String(buildPanel);
  bigMenu.hidden=buildPanel||customize.isOpen();
  bigBuildBack.disabled=closed;
  ui.set(document.querySelector("#big-build-title"),launchError?"builder.big_screen.launch_failed":"builder.build.status");
  ui.set(document.querySelector("#big-build-log-title"),launchError?"builder.big_screen.launch_details":"builder.build.log");
  ui.set(buildReport,launchError?"builder.big_screen.save_launch_report":"builder.big_screen.save_report");
  log.hidden=!!launchError;
  bigLaunchDetails.hidden=!launchError;
  if(buildPanel){
    const summary=document.querySelector("#big-build-summary");
    if(state.parentNode!==summary) summary.append(state,bigProgress);
  }else{
    const status=document.querySelector("#big-screen-status");
    if(state.parentNode!==status) status.prepend(state,bigProgress);
  }
  const canReviewROMs=install.canRebuild||install.canLaunch;
  bigROM.hidden=!bigROMOpen||!canReviewROMs||building||playing;
  bigBuildOpen.hidden=!hasBuild||!bigROM.hidden;
  form.hidden=!install.canRebuild;
  homePrimary.hidden=!bigROM.hidden||data?.mode==="unusable";
  homePrimary.disabled=busy||(!canLaunch&&!install.canRebuild);
  ui.set(homePrimary,launching?"builder.launch.starting":building?"builder.home.building":canLaunch?"builder.home.play":
    coreROM()?"builder.home.build":install.canRebuild?"builder.big_screen.choose_rom":"builder.home.checking");
  bigRebuild.hidden=!bigROM.hidden||!install.canRebuild||!install.canLaunch||!rebuild||building;
  bigRebuild.disabled=busy;
  bigExit.hidden=!bigROM.hidden;
  bigExit.disabled=busy;
  bigCustomizeOpen.hidden=!bigROM.hidden;
  bigCustomizeOpen.disabled=closed||closing||playing||launching;
  bigROMsOpen.hidden=!bigROM.hidden||!canReviewROMs;
  bigROMsOpen.disabled=busy;
  ui.set(bigExitLabel,playSession?"builder.big_screen.library":"builder.close");
  bigProgress.hidden=!building;
  if(launchError) bigBuildPhase.hidden=true;
  if(customize.isOpen()||document.querySelector("dialog[open]")) return;
  const focused=document.activeElement;
  if(!data&&state.dataset.kind!=="failed") return;
  if(playJustAppeared&&!busy&&(focused===state||focused===buildReport||focused===bigBuildBack||focused===document.body)){
    bigBuildPlay.focus({preventScroll:true});
  }else if(focused===document.body||focused.disabled||!focused.getClientRects().length){
    const next=(buildPanel?[bigBuildPlay,buildReport,bigBuildBack,state]:[bigROM.hidden?null:preferredROMRow(),homePrimary,bigBuildOpen,bigCustomizeOpen,bigROMBack,bigExit,state])
      .find(node=>node&&!node.disabled&&node.getClientRects().length);
    next?.focus({preventScroll:true});
  }else if(focused===state&&!busy&&!homePrimary.disabled){
    homePrimary.focus({preventScroll:true});
  }
}

function openBigROMChecklist(opener=document.activeElement){
  bigROMOpener=opener;
  bigROMOpen=true;
  updateBigScreen();
  preferredROMRow()?.focus();
}

function openBigROM(){
  openBigROMChecklist();
  document.getElementById("rom-slot-us")?.focus();
  romChecklist.chooseCore();
}

function syncROMSelection(){
  const selected=coreROM();
  const state=latestModeData;
  const busy=closed||closing||launching||state?.playing||isBuilding();
  romChecklist.availability(!!state?.install?.canLaunch,!!busy);
  build.disabled=!!busy||romChecklist.isSelecting()||!selected;
  savedROMNote.hidden=!selected;
  const romNoteKey=selectedROM?"builder.build.browser_rom":"builder.build.saved_rom";
  ui.set(savedROMNote,romNoteKey,{path:selected});
  ui.attribute(savedROMNote,"title",romNoteKey,{path:selected});
}

let dismissedBuildVersion, displayedBuildVersion;
function applyBuildFreshness(install,building){
  const info=install.build||{}, state=info.state||"unknown";
  const recommended=state==="rebuild"||state==="unknown";
  displayedBuildVersion=JSON.stringify([state,info.builtVersion,info.builderVersion]);
  versionNotice.hidden=!install.canLaunch||building||state==="current"||
    displayedBuildVersion===dismissedBuildVersion;
  versionNotice.dataset.state=state;
  ui.set(document.querySelector("#game-version-title"),recommended?"builder.version.recommended":"builder.version.unavailable");
  ui.set(document.querySelector("#game-version-help"),state==="rebuild"?"builder.version.different":state==="unknown"?"builder.version.legacy":"builder.version.unavailable_help");
  const labels=document.querySelector("#game-version-labels");
  labels.hidden=!info.builderVersion;
  ui.set(labels,info.builtVersion?"builder.version.both_labels":"builder.version.builder_label",{built:info.builtVersion||"",builder:info.builderVersion||""});
  versionReview.hidden=!recommended||!install.canRebuild;
  versionReview.disabled=closed||building||romChecklist.isSelecting();
  ui.set(versionReview,savedROM?"builder.version.rebuild_now":"builder.version.review");
}
document.querySelector("#game-version-dismiss").addEventListener("click",()=>{
  dismissedBuildVersion=displayedBuildVersion;
  versionNotice.hidden=true;
  activeTab?.focus();
});
versionReview.addEventListener("click",()=>savedROM?startGameBuild(true):selectTab(buildTab));

function applyMode(data){
  latestModeData=data;
  if(data.state!==lastSceneBuildState) {
    lastSceneBuildState=data.state;
    document.dispatchEvent(new Event("workshop:build-state"));
  }
  const mode=data.mode||"buildable", install=data.install||{};
  document.body.dataset.mode=mode;
  /* Play is hidden while a build runs: the binary it would launch is being
     overwritten. The server refuses it too (launch() checks state first) -- this
     is only so the button does not sit there inviting the click. */
  const building=isBuilding(), playing=!!data.playing;
  savedROM=typeof data.rebuildROM==="string"?data.rebuildROM:"";
  selectedROM=typeof data.selectedROM==="string"?data.selectedROM:"";
  syncROMSelection();
  ui.set(build,install.canLaunch?"builder.build.rebuild_game":"builder.nav.build_game");
  applyBuildFreshness(install,building);
  playBox.hidden=!install.canLaunch||building;
  playButton.disabled=building||playing||launching;
  dockLaunch.hidden=!install.canLaunch||building||data.state!=="succeeded";
  dockLaunch.disabled=building||playing||launching||!install.canLaunch;
  track.hidden=!["building","succeeded","failed"].includes(data.state);
  buildBox.hidden=!install.canRebuild;
  noBuild.hidden=(mode!=="launcher");
  slimDone.hidden=!data.slimDone;
  /* Offered only when the server says there is something to remove, and only
   * once the game is actually playable -- reclaiming space before there is a
   * working build would be the wrong trade. */
  slimBox.hidden=!(install.canSlim && install.canLaunch && !building && !slimDismissed && !data.slimDone);
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
  homePrimary.disabled=closed||building||playing||launching||(!canLaunch&&!install.canRebuild);
  ui.set(homePrimary,launching?"builder.launch.starting":building?"builder.home.building":canLaunch?"builder.home.play":"builder.home.setup");
  ui.set(document.querySelector("#home-game-note"),building?"builder.home.while_building":
    canLaunch?(install.build?.state==="current"?"builder.version.current":"builder.home.ready"):
    install.canRebuild?romSetupKey():"builder.home.unusable");
  if(bigScreen&&data.state==="idle"&&!canLaunch&&install.canRebuild&&state.dataset.kind==="idle"){
    showKey("idle",romSetupKey());
  }
  lastMode=mode;
  updateBuildView();
  updateBigScreen();
}

/* The phase list and percentage come from the server (builder/progress.go), so
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
  for(const meter of [buildProgress,bigProgress]) meter.value=percent;
  for(const phase of [buildPhase,bigBuildPhase]){
    phase.hidden=kind!=="building"&&kind!=="failed";
    ui.set(phase,"builder.build.phase_progress",{phase:dock.title,percent});
  }
}

/* A finished build must be unmissable without hijacking the reader's tab: the
 * dock announces it (and offers Launch inline), and the Build tab gets a dot. */
function announce(kind,message){
  dock.dataset.kind=kind;
  dock.dataset.open="true";
  if(message) { ui.unbind(dockPhase); dockPhase.textContent=message; }
  if(buildView.hidden)
    progressTab.dataset.badge=(kind==="failed"?"bad":"ok");
}

async function responseJSON(response){
  if(window.workshopFeedback)return window.workshopFeedback.readJSON(response);
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
  const requestID=++statusRequestID;
  clearTimeout(statusTimer);
  try {
    const data=await responseJSON(await fetchResponse("status",{cache:"no-store"}));
    if(closed||requestID!==statusRequestID) return;
    if(data.log&&log.textContent!==data.log){
      const follow=log.scrollHeight-log.scrollTop-log.clientHeight<24;
      ui.unbind(log);
      log.textContent=data.log;
      if(follow) log.scrollTop=log.scrollHeight;
    }
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
      bigBuildError="";
      if(polling){
        romChecklist.load();
        bigROMOpen=false;
        bigROMOpener=null;
      }
      updateBigScreen();
      showKey("succeeded","builder.build.complete");
      launch.disabled=false; polling=false;
      dockPct.textContent="100%";
      announce("succeeded");
    }
    if(data.state==="failed"){
      if(polling) romChecklist.load();
      const error=data.errorCode&&window.workshopFeedback?.responseError(data);
      showKey("failed",error?.uiKey||"builder.build.failed",error?.uiArgs||{detail:data.error||""},error,"Build game");
      launch.disabled=true; polling=false;
      announce("failed");
    }
    if(data.state==="idle"){
      /* The original copy assumed nothing was built. Say what is actually
       * true, so a returning user is not told to choose a ROM they do not
       * need. */
      const install=data.install||{};
      if(data.mode==="launcher") showKey("idle","builder.build.ready");
      else if(install.canLaunch){
        const ready=bigScreen&&!bigRebuild.hidden?"builder.version.recommended":"builder.build.ready";
        showKey("idle",ready);
        launch.disabled=false;
      }
      else if(install.canRebuild) showKey("idle",romSetupKey());
      if(!firstBuildAttempted&&data.buildOnFirstRun&&selectedROM&&!savedROM&&
          !romChecklist.isSelecting()&&install.canRebuild&&!install.canLaunch&&!data.playing){
        void startGameBuild(false,true);
      }
    }
  } catch(error) {
    if(closed||requestID!==statusRequestID) return;
    showKey("failed","builder.request_failed",{detail:error.message},error);
    polling=false; announce("failed"); updateBigScreen();
  }
  if(polling&&requestID===statusRequestID) statusTimer=setTimeout(refresh,500);
}

async function startGameBuild(reuseSaved=false, firstRun=false){
  if(closed||closing||launching||switchingView||latestModeData?.playing||isBuilding()||romChecklist.isSelecting())return false;
  if(!coreROM()){
    if(bigScreen) openBigROM();
    else { selectTab(buildTab); document.getElementById("rom-slot-us")?.focus(); }
    return false;
  }
  const payload=new FormData();
  payload.set("useSelectedROMs","true");
  if(reuseSaved||(!selectedROM&&savedROM)) payload.set("reuseROM","true");
  if(firstRun){
    firstBuildAttempted=true;
    payload.set("firstRun","true");
  }
  bigBuildError="";
  bigBuildDismissed=false;
  reportStatus.hidden=true;
  buildSubmitting=true;
  ++statusRequestID;
  clearTimeout(statusTimer);
  syncROMSelection();
  launch.disabled=true; ui.unbind(log); log.textContent=ui.text("builder.build.preparing");
  log.scrollTop=0;
  lastPaint=null;
  showKey("building","builder.build.starting");
  openBuildView();
  steps.forEach(step=>step.removeAttribute("data-state"));
  stepsBox.hidden=false;   /* only worth showing once there is progress to show */
  progressTab.removeAttribute("data-badge");
  dock.dataset.kind="building"; dock.dataset.open="true";
  ui.set(dockPhase,"builder.build.preparing_rom"); dockPct.textContent="…"; track.hidden=false; dockLaunch.hidden=true;
  track.removeAttribute("aria-valuenow");
  for(const meter of [buildProgress,bigProgress]) meter.removeAttribute("value");
  for(const phase of [buildPhase,bigBuildPhase]) phase.hidden=true;
  updateBigScreen();
  try {
    await responseJSON(await fetchResponse("build",{method:"POST",body:payload}));
    applyMode({...latestModeData,state:"building"});
    polling=true;refresh();return true;
  }
  catch(error){
    log.textContent=error.detail||error.message;
    showKey("failed","builder.request_failed",{detail:error.message},error,"Start game build");
    announce("failed");
    if(error.code==="builder.build.saved_rom_missing")applyMode({...latestModeData,rebuildROM:""});
    if(error.code==="builder.build.launch_rom_invalid") romChecklist.load();
    return false; // Never silently retry a build after a network failure.
  }
  finally {buildSubmitting=false;syncROMSelection();updateBuildView();updateBigScreen();}
}
form.addEventListener("submit",async event=>{
  event.preventDefault();
  await startGameBuild();
});

const staleBuildDialog=document.querySelector("#stale-build-dialog");
let pendingStalePlay=null;
function finishStalePlay(action){
  const resolve=pendingStalePlay;
  pendingStalePlay=null;
  staleBuildDialog.close();
  resolve?.(action);
}
document.querySelector("#stale-build-cancel").addEventListener("click",()=>finishStalePlay("cancel"));
document.querySelector("#stale-build-play").addEventListener("click",()=>finishStalePlay("play"));
document.querySelector("#stale-build-review").addEventListener("click",()=>finishStalePlay("rebuild"));
staleBuildDialog.addEventListener("cancel",event=>{event.preventDefault();finishStalePlay("cancel");});
staleBuildDialog.addEventListener("close",()=>{const resolve=pendingStalePlay;pendingStalePlay=null;resolve?.("cancel");});
document.addEventListener("workshop:closed",()=>{if(pendingStalePlay)finishStalePlay("cancel");});

const modRecoveryDialog=document.querySelector("#mod-recovery-dialog");
const modRecoveryPlay=document.querySelector("#mod-recovery-play");
let pendingModRecovery=null;
function finishModRecovery(action){
  const resolve=pendingModRecovery;
  pendingModRecovery=null;
  modRecoveryDialog.close();
  resolve?.(action);
}
document.querySelector("#mod-recovery-cancel").addEventListener("click",()=>finishModRecovery("cancel"));
document.querySelector("#mod-recovery-retry").addEventListener("click",()=>finishModRecovery("retry"));
modRecoveryPlay.addEventListener("click",()=>finishModRecovery("play"));
modRecoveryDialog.addEventListener("cancel",event=>{event.preventDefault();finishModRecovery("cancel");});
document.addEventListener("workshop:closed",()=>{if(pendingModRecovery)finishModRecovery("cancel");});

async function launchWithModRecovery(){
  let recovery="";
  while(!closed){
    const options=recovery?window.workshopModPacks.jsonOptions({modRecovery:recovery}):{method:"POST"};
    const response=await modRequests.run(()=>fetchResponse("launch",options));
    const issue=response.status===409?await response.clone().json().catch(()=>null):null;
    if(issue?.errorCode!=="builder.mods.scan_failed"){
      await responseJSON(response);
      return true;
    }
    document.querySelector("#mod-recovery-detail").textContent=issue.detail;
    recovery=issue.modRecovery;
    modRecoveryPlay.hidden=recovery!=="prepared"&&recovery!=="legacy";
    if(!modRecoveryPlay.hidden) ui.set(modRecoveryPlay,recovery==="prepared"?"builder.mods.play_prepared":"builder.mods.play_legacy");
    const action=await new Promise(resolve=>{
      pendingModRecovery=resolve;
      modRecoveryDialog.showModal();
      document.querySelector("#mod-recovery-retry").focus();
    });
    if(action==="cancel"){
      const error=new Error(issue.detail);
      error.code=issue.errorCode;
      showKey("failed","builder.mods.scan_failed",{},error,"Refresh mods before launch");
      return false;
    }
    if(action==="retry") recovery="";
  }
  return false;
}

async function doLaunch(){
  if(closed||launching) return;
  if(isBuilding()||romChecklist.isSelecting()) return;
  bigBuildError="";
  reportStatus.hidden=true;
  setLaunching(true);
  const buttons=[homePrimary,...playLabels.keys()];
  const previous=buttons.map(button=>button.disabled);
  buttons.forEach(button=>button.disabled=true);
  updateBigScreen();
  let launchStatus=null;
  try {
    // Recheck on the click: the game or Builder may have changed since the
    // initial page load. All Play buttons share this path, including the dock.
    const data=await responseJSON(await fetchResponse("status",{cache:"no-store"}));
    if(closed||data.playing)return;
    launchStatus=data;
    applyMode(data);
    if(data.state==="building"){void refresh();return;}
    const info=data.install||{}, state=info.build?.state||"unknown";
    if(info.canLaunch&&state!=="current"&&state!=="unavailable"){
      document.querySelector("#stale-build-review").hidden=!info.canRebuild;
      ui.set(document.querySelector("#stale-build-review"),savedROM?"builder.version.rebuild_now":"builder.version.review");
      const action=await new Promise(resolve=>{
        pendingStalePlay=resolve;
        staleBuildDialog.showModal();
        if(bigScreen&&info.canRebuild) document.querySelector("#stale-build-review").focus();
      });
      if(action==="rebuild"){
        setLaunching(false);
        if(savedROM){
          await startGameBuild(true);
        }else if(bigScreen) openBigROM();
        else selectTab(buildTab);
        return;
      }
      if(action!=="play"||closed)return;
    }
    if(closed)return;
    showKey("idle","builder.build.launching");
    if(!await launchWithModRecovery())return;
    if(bigScreen&&playSession) applyMode({...latestModeData,playing:true});
    showKey("succeeded","builder.build.launched");
    if(closeOnLaunch&&!playSession){
      if(hasUnsavedWorkshopEdits()||modRequests.isWriting()){
        showKey("succeeded","builder.launch.kept_open");
      }else await closeWorkshop();
    }
  }
  catch(error){ showKey("failed","builder.request_failed",{detail:error.message},error,"Launch game"); }
  finally {
    pendingStalePlay=null;
    setLaunching(false);
    if(!closed){
      if(launchStatus){const current=latestModeData||launchStatus;applyMode(current);launch.disabled=!current.install?.canLaunch||current.state==="building"||!!current.playing;}
      else {
        buttons.forEach((button,i)=>button.disabled=previous[i]);
        ui.set(homePrimary,canLaunch?"builder.home.play":"builder.home.setup");
      }
      if(bigScreen&&bigROMOpen&&!bigROM.hidden&&!document.querySelector("dialog[open]")) preferredROMRow()?.focus();
      updateBigScreen();
    }
  }
}
launch.addEventListener("click",doLaunch);
dockLaunch.addEventListener("click",doLaunch);
playButton.addEventListener("click",doLaunch);
buildViewPlay.addEventListener("click",doLaunch);
bigBuildPlay.addEventListener("click",doLaunch);
homePrimary.addEventListener("click",()=>{
  if(canLaunch) doLaunch();
  else if(!bigScreen) selectTab(buildTab);
  else if(coreROM()) startGameBuild();
  else openBigROM();
});

slimDismiss.addEventListener("click",()=>{ slimDismissed=true; slimBox.hidden=true; });
slimButton.addEventListener("click",async()=>{
  slimButton.disabled=true; slimDismiss.disabled=true;
  showKey("building","builder.build.removing_tools");
  try {
    await responseJSON(await fetchResponse("slim",{method:"POST"}));
    /* One poll settles everything: the server has re-probed, so the offer
     * disappears, the confirmation appears, and the page drops into launcher
     * mode without the page having to guess any of it. */
    await refresh();
    showKey("succeeded","builder.build.tools_removed");
  } catch(error){ showKey("failed","builder.request_failed",{detail:error.message},error,"Remove build tools"); }
  slimButton.disabled=false; slimDismiss.disabled=false;
});

const closeDialog=document.querySelector("#close-workshop-dialog");
const closeConfirm=document.querySelector("#close-workshop-confirm");
const hasUnsavedWorkshopEdits=()=>window.localizationHasEdits?.()||assetBar.dataset.dirty==="true"||publishing.hasEdits();
function requestWorkshopClose(){
  if(closed||closing||closeDialog.open) return;
  const blocked=isBuilding()?"builder.close_build_running":
    latestModeData?.playing?"builder.close_game_running":
    launching?"builder.build.launching":"";
  if(blocked){
    window.workshopFeedback?.show(closeButton,new Error(ui.text(blocked)),{
      operation:"Close Workshop",messageKey:blocked,
    });
    return;
  }
  if(!hasUnsavedWorkshopEdits()){
    void closeWorkshop();
    return;
  }
  ui.set(document.querySelector("#close-workshop-title"),bigScreen&&playSession?"builder.big_screen.library":"builder.close_title");
  ui.set(closeConfirm,bigScreen&&playSession?"builder.big_screen.library":"builder.close");
  closeDialog.returnValue="";
  closeDialog.showModal();
}

async function closeWorkshop(){
  if(closed||closing||isBuilding()) return;
  closing=true;
  updateBigScreen();
  try {
    await responseJSON(await fetchResponse("close",{method:"POST"}));
    showKey("idle","builder.closed");
    closed=true; polling=false; clearTimeout(previewTimer); clearTimeout(statusTimer);
    document.dispatchEvent(new Event("workshop:closed"));
    document.querySelectorAll("dialog[open]").forEach(dialog=>dialog.close());
    document.querySelectorAll("button,input,select,textarea").forEach(control=>control.disabled=true);
    assetForm.querySelectorAll("audio").forEach(audio=>audio.pause());
    ui.set(document.querySelector("#home-game-note"),"builder.closed_note");
    document.querySelector("#scene-motion").value="still";
    document.querySelector("#scene-motion").dispatchEvent(new Event("change"));
    dock.dataset.open="false";
  } catch(error){ showKey("failed","builder.request_failed",{detail:error.message},error,"Close Workshop"); }
  finally { closing=false;updateBigScreen(); }
}
closeDialog.addEventListener("close",()=>{
  if(closeDialog.returnValue==="close") void closeWorkshop();
});
closeButton.addEventListener("click",requestWorkshopClose);
if(desktopShell){
  const closeRequests=new EventSource("__shell/close-requests");
  closeRequests.addEventListener("close-request",requestWorkshopClose);
  document.addEventListener("workshop:closed",()=>closeRequests.close(),{once:true});
}else{
  // A standalone browser tab cannot postpone navigation for an HTML dialog.
  window.addEventListener("beforeunload",event=>{
    if(!closed&&!closing&&hasUnsavedWorkshopEdits()){
      event.preventDefault();
      event.returnValue="";
    }
  });
}
closeOnLaunchInput.addEventListener("change",async()=>{
  const requested=closeOnLaunchInput.checked;
  closeOnLaunchInput.disabled=true;
  try{
    const response=await fetchResponse("interface/preferences",{
      method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({closeOnLaunch:requested}),
    });
    const result=await responseJSON(response);
    closeOnLaunch=result.closeOnLaunch===true;
  }catch(error){
    window.workshopFeedback?.show(closeOnLaunchInput,error,{
      operation:"Save launch setting",messageKey:"builder.launch.save_failed",
    });
  }finally{
    closeOnLaunchInput.checked=closeOnLaunch;
    closeOnLaunchInput.disabled=closed;
  }
});
const bigPanel=document.querySelector("#big-screen-panel"), homePanel=document.querySelector("#panel-home");
const staleActions=staleBuildDialog.querySelector(".dialog-actions");
const desktopSlots=[homePrimary,form,savedROMNote,importROMExtras,state,log,stepsBox,buildReport,reportStatus].map(node=>[node,node.parentNode,node.nextSibling]);
const desktopStaleOrder=[...staleActions.children];
let desktopTab=tabs[0], switchingView=false;

function setView(next, destination){
  window.workshopFeedback?.clear(state);
  if(next){
    desktopTab=activeTab;
    if(activeTab!==tabs[0]||!buildView.hidden) selectTab(tabs[0],{history:false,focus:false,activate:false});
    bigScreen=true;
    document.body.dataset.bigScreen="true";
    bigScreenROMChecklist.append(romChecklistSection);
    bigActions.prepend(homePrimary);
    bigROMActions.prepend(form,importROMExtras);
    bigROMActions.append(savedROMNote);
    document.querySelector("#big-screen-status").prepend(state,bigProgress);
    document.querySelector("#big-build-log").append(log);
    document.querySelector("#big-build-stages").append(stepsBox);
    bigBuildBack.before(buildReport);
    document.querySelector("#big-build-summary").after(reportStatus);
    log.dataset.bigScroll="";
    homePanel.removeAttribute("role");
    homePanel.setAttribute("aria-labelledby","big-screen-title");
    staleActions.prepend(document.querySelector("#stale-build-review"));
    staleActions.append(document.querySelector("#stale-build-cancel"));
    bigPanel.hidden=false;
    romChecklist.render();
    if(!latestModeData) ui.set(state,"builder.home.checking");
    if(lastPaint) paint(lastPaint.progress,lastPaint.kind);
    if(document.activeElement!==document.body) document.activeElement.blur();
    window.scrollTo(0,0);
    if(latestModeData) applyMode(latestModeData);
    else updateBigScreen();
  }else{
    customize.close();
    bigScreen=false;
    document.body.dataset.bigScreen="false";
    document.body.dataset.bigBuild="false";
    bigPanel.hidden=true;
    bigROMOpen=false;
    for(const [node,parent,before] of desktopSlots) parent.insertBefore(node,before);
    delete log.dataset.bigScroll;
    log.hidden=false;
    form.hidden=false;
    homePrimary.hidden=false;
    staleActions.append(...desktopStaleOrder);
    homePanel.setAttribute("role","tabpanel");
    homePanel.setAttribute("aria-labelledby","tab-home");
    romChecklist.render();
    if(latestModeData) applyMode(latestModeData);
    const isAsset=destination==="music";
    const tab=isAsset?assetTab:destination==="languages"?languageTab:
      destination==="legacy-manual"?modsTab:destination==="title"?workshopTab:desktopTab;
    if(destination==="legacy-manual") assetPacks.setType(window.workshopModPacks.manualType);
    selectTab(tab.disabled?tabs[0]:tab);
    if(destination==="title") document.querySelector("#workshop-package-title").focus();
  }
}

async function switchView(next, destination){
  if(next===bigScreen||switchingView||closed||closing||launching||romChecklist.isSelecting()||latestModeData?.playing||
      customize.isBusy()||document.querySelector("dialog[open]")) return;
  switchingView=true;
  try {
    const response=await fetchResponse("__shell/window-mode",{
      method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({bigScreen:next}),
    });
    // The browser-only Builder can still change layouts without a native window.
    if(!response.ok&&response.status!==404) throw new Error(ui.text("builder.big_screen.switch_failed"));
    if(!closed) setView(next,destination);
  }catch(error){
    showKey("failed","builder.request_failed",{detail:error.message},error,"Switch Builder view");
  }finally { switchingView=false; }
}

const legacyMods=window.workshopLegacyMods?.(async (...args)=>responseJSON(await fetchResponse(...args)), types=>{
  if(types.length) assetPacks.setType(types.length===1?types[0]:"all");
  assetPacks.load();
});
const modRequest=async (...args)=>responseJSON(await fetchResponse(...args));
const modRequests=window.workshopModPacks.createRequests(modRequest);
const modPreview=window.workshopModPreview?.(modRequest,modRequests);
const assetPacks=window.workshopAssetPackPanel(
  modRequest,
  showInstalledMods,
  modPreview,
  modRequests,
);
function showInstalledMods(result){
  if(closed) return;
  // A slow install can finish while the user is in a different dialog.
  const dialog=document.querySelector("dialog[open]");
  if(dialog){
    dialog.addEventListener("close",()=>{
      if(showInstalledMods(result)){
        if(result.catalog) assetPacks.showInstalled(result.catalog);
        else void assetPacks.load();
      }
    },{once:true});
    return;
  }
  const {type,outcomes,legacyManual,catalog}=result;
  if(bigScreen) return void customize.openAssetPacks(type,{outcomes,legacyManual,catalog});
  assetPacks.setType(type);
  return selectTab(modsTab,{activate:false});
}
window.workshopShowInstalledMods=async (type,catalog)=>{
  if(!showInstalledMods({type,catalog})) return;
  if(catalog) assetPacks.showInstalled(catalog);
  else await assetPacks.load();
};
const publishing=window.workshopPublishing(modRequest,modRequests,result=>{
  if(showInstalledMods({type:result.installedType,catalog:result})) assetPacks.showInstalled(result);
},fetchResponse,async ()=>{
  const form=new FormData();
  form.set("finish-pack","1");
  const result=await modRequests.write(()=>modRequest("music-draft",{method:"POST",body:form}));
  importGeneration++;
  paintAssetConfiguration(result.config);
});

function bindPackImport(button,input,handle){
  button.addEventListener("click",()=>input.click());
  input.addEventListener("change",async ()=>{
    const file=input.files[0];
    input.value="";
    if(!file||button.disabled||button.hasAttribute("aria-busy")) return;
    button.setAttribute("aria-busy","true");
    try {
      await handle(file);
    } catch(error){
      window.workshopFeedback.show(button,error,{operation:"Import Workshop pack"});
    } finally {
      button.removeAttribute("aria-busy");
    }
  });
}

bindPackImport(importMusicButton,document.getElementById("asset-pack-music-import-file"),async file=>{
  const hasDraft=!assetsLoaded||lastAssetConfig.source||assetBar.dataset.dirty==="true"||publishing.hasMusicDetails()||
    lastAssetConfig.tracks.some(track=>track.configured||track.variants?.some(variant=>variant.configured));
  if(hasDraft&&!await window.workshopConfirm("builder.language.import_edit","builder.mods.import_music_prompt",{name:file.name})) return;
  assetForm.inert=true;
  try {
    if(!await saveMusicDraft()) return;
    const body=new FormData();
    body.set("pack",file);
    const config=await modRequests.write(()=>modRequest("mods/import-draft?type="+window.workshopModPacks.musicType,{method:"POST",body}));
    importGeneration++;
    publishing.clearMusicDetails();
    const included=id=>!config.source.excluded?.includes(id);
    const first=config.tracks.find(track=>(track.configured&&included(track.id))||
      track.variants?.some(variant=>variant.configured&&included(variant.name)));
    if(first) selectedTrack=first.id;
    paintAssetConfiguration(config);
    assetState.hidden=true;
    assetsLoaded=true;
  } finally {
    assetForm.inert=false;
    if(activeTab===assetTab) importMusicButton.focus();
  }
});
bindPackImport(document.getElementById("asset-pack-file-import-open"),document.getElementById("asset-pack-file-import-file"),publishing.importFilePack);
document.querySelector("#loc-manage-mods").addEventListener("click",()=>window.workshopShowInstalledMods(window.workshopModPacks.languageType));
const dropHint = document.getElementById("builder-drop-hint");
const localDrop = target => target?.closest?.('input[type="file"],.file-control,#loc-import-source');
let dragDepth = 0;
function clearDropHint() {
  dragDepth = 0;
  dropHint.hidden = true;
}
document.addEventListener("dragenter", event => {
  if ([...event.dataTransfer?.types || []].includes("Files")) dragDepth++;
}, true);
document.addEventListener("dragover", event => {
  if (![...event.dataTransfer?.types || []].includes("Files")) return;
  const local = localDrop(event.target);
  dropHint.hidden = !!local || closed || bigScreen || !!document.querySelector("dialog[open]");
  if (!local) event.preventDefault();
}, true);
document.addEventListener("dragleave", () => {
  dragDepth = Math.max(0, dragDepth - 1);
  if (!dragDepth) dropHint.hidden = true;
}, true);
window.addEventListener("blur", clearDropHint);
document.addEventListener("dragend", clearDropHint, true);
function reportDrop(key){
  window.workshopFeedback?.show(document.body,new Error(ui.text(key)),{
    operation:"Drop files",messageKey:key,
  });
}
document.addEventListener("drop",event=>{
  clearDropHint();
  const files=[...(event.dataTransfer?.files||[])];
  if(!files.length||localDrop(event.target)) return;
  event.preventDefault();
  if(closed) return;
  if(document.querySelector("dialog[open]")) return reportDrop("builder.drop.finish_dialog");
  const roms=files.filter(file=>/\.s(?:fc|mc)$/i.test(file.name));
  const otherFiles=files.filter(file=>!roms.includes(file));
  const hasMods=otherFiles.some(window.workshopModPacks.canInstall);
  if(!roms.length&&!hasMods) return reportDrop("builder.drop.formats");
  if(roms.length&&(isBuilding()||latestModeData?.playing||romChecklist.isSelecting()||(bigScreen&&customize.isBusy()))){
    reportDrop("builder.drop.rom_busy");
  }else if(roms.length){
    if(bigScreen){
      bigROMOpener=bigROMsOpen;
      customize.close();
      bigROMOpen=true;
      updateBigScreen();
    }else if(activeTab!==buildTab||!buildView.hidden) selectTab(buildTab,{focus:false});
    const selection=romChecklist.upload(roms);
    if(!hasMods&&otherFiles.length) void selection.then(()=>reportDrop("builder.drop.skipped"));
  }
  if(hasMods) void assetPacks.upload(otherFiles);
},true);
const customize=window.workshopBigScreenCustomize(
  modRequest,
  destination=>switchView(false,destination),
  ()=>{updateBigScreen();bigCustomizeOpen.focus();},
  modPreview,
  modRequests,
);
const fileBrowser=window.workshopFileBrowser(
  async (...args)=>responseJSON(await fetchResponse(...args)), {
  selectROM: selection=>romChecklist.selectLocal(selection),
  selected: id=>{
    if(bigROMOpen&&id==="us"&&!build.disabled) build.focus();
    else (document.getElementById("rom-slot-" + id)||preferredROMRow())?.focus();
  },
  installMod: selection=>modRequests.write(()=>modRequest("mods/install-browsed",window.workshopModPacks.jsonOptions(selection))),
  installed: result=>customize.openAssetPacks(result.installedType,{legacyManual:result.installedLegacyManual,catalog:result}),
  },
);
const romChecklist=window.workshopROMChecklist(
  async (...args)=>responseJSON(await fetchResponse(...args)),
  fileBrowser.rom,
  {
    onImported: async()=>{document.dispatchEvent(new Event("workshop:content-imported"));await refresh();},
    onSelectionChanged: refresh,
    onSelecting: ()=>{
      if(latestModeData) applyMode(latestModeData);
      else syncROMSelection();
    },
  },
);
bigROMsOpen.addEventListener("click",()=>openBigROMChecklist(bigROMsOpen));
document.querySelector("#workshop-package-title").addEventListener("click",publishing.openTitle);
document.querySelector("#workshop-package-pdf").addEventListener("click",publishing.openManual);
bigCustomizeOpen.addEventListener("click",customize.open);
document.querySelector("#big-mod-install").addEventListener("click",event=>{
  if(modRequests.isBusy()) return;
  event.currentTarget.focus();
  fileBrowser.mod();
});
document.querySelector("#open-big-screen").addEventListener("click",()=>switchView(true));
document.querySelector("#big-screen-desktop").addEventListener("click",()=>switchView(false));
bigRebuild.addEventListener("click",()=>savedROM?startGameBuild(true):openBigROM());
bigBuildOpen.addEventListener("click",()=>{
  bigBuildDismissed=false;
  updateBigScreen();
});
function backBigScreen(){
  if(customize.isOpen()) customize.back();
  else if(!bigBuild.hidden){
    if(bigBuildBack.disabled) return;
    bigBuildDismissed=true;
    updateBigScreen();
  }else if(bigROMOpen&&!bigROM.hidden){
    bigROMOpen=false;
    updateBigScreen();
    const opener=bigROMOpener;
    bigROMOpener=null;
    (opener?.getClientRects().length&&!opener.disabled?opener:homePrimary).focus();
  }else if(!bigExit.disabled) requestWorkshopClose();
}
bigExit.addEventListener("click",backBigScreen);
bigROMBack.addEventListener("click",backBigScreen);
bigBuildBack.addEventListener("click",backBigScreen);
buildReport.addEventListener("click",async()=>{
  if(buildReport.hidden||savingReport||closed) return;
  savingReport=true;
  reportStatus.hidden=false;
  ui.set(reportStatus,"builder.big_screen.saving_report");
  try {
    const result=await responseJSON(await fetchResponse("build-report",{
      method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({operation:bigBuildError==="launch"?"launch":"build",
        message:(bigBuildError==="launch"?bigLaunchDetails:state).textContent.slice(0,4096)}),
    }));
    ui.set(reportStatus,"builder.big_screen.report_saved",{path:result.path});
  }catch(error){
    ui.set(reportStatus,"builder.request_failed",{detail:error.message});
    window.workshopFeedback?.show(reportStatus,error,{operation:"Save build report"});
  }finally { savingReport=false; }
});
window.workshopBigScreenNavigation(bigPanel,backBigScreen,{
  navigationRoot:homePanel,
  isActive:()=>bigScreen,
  toggleView:()=>switchView(!bigScreen),
});
if(startBigScreen) setView(true);
syncROMSelection();
romChecklist.load();
refresh();
route();
// Also discover a source/build prepared in another window or by the CLI.
function refreshDesktopMods() {
  if (closed || bigScreen || activeTab !== modsTab) return;
  const dialog = document.querySelector("dialog[open]");
  if (dialog) dialog.addEventListener("close", refreshDesktopMods, {once: true});
  else assetPacks.load();
}
window.addEventListener("focus",()=>{
  if(!closed&&bigScreen) void customize.refreshOnFocus();
  if(!closed&&!polling&&!launching) {
    romChecklist.load();
    refresh();
    refreshDesktopMods();
  }
});
})();
