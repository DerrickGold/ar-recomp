(() => {
  "use strict";
  const ui = window.workshopI18n;
  const $ = id => document.getElementById("loc-" + id);
  const label = (id,key,args={}) => ui.set($(id),key,args);
  function raw(id,text) { ui.unbind($(id)); $(id).textContent=text; }
  function keyedOption(value,key,args={}) { const el=option(value,""); ui.set(el,key,args); return el; }
  function phrase(key,args={},tag="span") { const el=document.createElement(tag); ui.set(el,key,args); return el; }
  // Only the authoritative Go navigation model supplies caption IDs. Source
  // excerpts and technical message IDs have no caption key and remain literal.
  function navigationCaption(key,text) { const el=document.createElement("span"); el.textContent=text; if(key)ui.set(el,"builder.navigation."+key); else contentLanguage(el); return el; }
  // Content direction is never the workshop UI locale. Keep UTF-8 untouched;
  // dir/lang affect presentation only, not saved scripts or selection offsets.
  function contentLanguage(el, metadata={}) {
    el.setAttribute("dir", ["ltr","rtl"].includes(metadata?.direction) ? metadata.direction : "auto");
    el.setAttribute("lang", metadata?.locale || "");
  }
  function draftLanguage() {
    const fields=$("metadata").elements;
    return {locale:fields.locale.value,direction:fields.direction.value};
  }
  function updateContentLanguage() {
    const metadata=draftLanguage();
    contentLanguage($("body"),metadata);
    for(const page of $("preview-content").querySelectorAll(".loc-preview-page")) contentLanguage(page,metadata);
  }
  function failure(key,args={}) { const error=new Error(ui.text(key,args)); error.uiKey=key; error.uiArgs=args; return error; }
  function feedbackKey(key,args={},error=false) { label("feedback",key,args); $("feedback").dataset.error=String(error); if(!error)window.workshopFeedback?.clear($("feedback")); }
  const panel = document.getElementById("panel-localization");
  const statusKeys = {not_started: "builder.editor.not_started", wip: "builder.editor.wip", done: "builder.editor.done"};
  const valueKindKeys = {number:"builder.editor.value_number",localized_term:"builder.editor.value_term",localized_text:"builder.editor.value_text",icon:"builder.editor.value_icon"};
  const coverageSurfaceKeys = {dialogue:"builder.coverage.surface.dialogue",menus:"builder.coverage.surface.menus",keyboard:"builder.coverage.surface.keyboard",terms:"builder.coverage.surface.terms",hud:"builder.coverage.surface.hud",credits:"builder.coverage.surface.credits"};
  let state = {project: null}, selected = "", message = null, dirty = false, detailsDirty = false, noticeDirty = false, busy = false;
  let offset = 0, searchSnapshot = null, loaded = false, closed = false;
  let workflow = "home", phase = "choose", reviewPurpose = "install";
  let installPath = "", editorTab = "messages", installedExists = false, importPreview = null;
  let catalog = [], saveFailure = "";
  let fontsDirty = false, fontDraft = [], fontUploads = new Map();
  const expanded = new Set();
  const flowTitles = {install:"builder.language.flow_install",create:"builder.language.flow_create",edit:"builder.language.flow_edit",clone:"builder.language.flow_clone",extract:"builder.language.flow_extract"};
  function workflowView() {
    const home = workflow === "home", choose = phase === "choose", editing = phase === "edit";
    const importing = choose && (["edit","clone"].includes(workflow) || workflow === "install" && installPath === "import");
    panel.dataset.editing = String(!home && editing);
    $("home").hidden = !home; $("flow-header").hidden = home || editing;
    $("library-section").hidden = !home;
    $("home-prompt").hidden = !home;
    if(editing || flowTitles[workflow]) label("flow-title",editing?"builder.language.edit_pack":flowTitles[workflow]);
    else raw("flow-title","");
    label("flow-steps",workflow === "install" ? installPath === "existing" ? "builder.language.steps_existing" : "builder.language.steps_import" : workflow === "clone" ? "builder.language.steps_clone" : "builder.language.steps_edit");
    $("install-state").hidden = phase !== "review";
    $("install-choice").hidden = home || workflow !== "install" || !choose;
    for(const button of panel.querySelectorAll("[data-loc-install]")) button.setAttribute("aria-pressed",String(button.dataset.locInstall===installPath));
    $("project-picker").hidden = home || !choose || !(workflow === "install" && installPath === "existing" || ["edit","clone"].includes(workflow));
    label("project-picker-label",workflow === "install" ? "builder.language.imported_packs" : "builder.language.choose_saved");
    label("open",workflow === "install" ? "builder.language.review_install" : workflow === "clone" ? "builder.language.choose_base" : "builder.language.open_edit");
    $("start").hidden = home || !choose || !(importing || ["create","extract"].includes(workflow));
    $("extract").hidden = workflow !== "extract" && !(workflow === "create" && !state.sourceAvailable);
    $("create").hidden = workflow !== "create" || !state.sourceAvailable;
    $("import-source").hidden = !importing;
    $("import-preview").hidden = !importing || !importPreview;
    $("clone").hidden = home || phase !== "clone";
    $("workspace").hidden = home || !state.project || !["edit","review","installed"].includes(phase);
    $("editor-actions").hidden = !editing || state.project?.origin === "native-source";
    if (state.project?.origin === "native-source") editorTab = "messages";
    $("details").hidden = !editing || editorTab !== "details";
    $("editing").hidden = !editing || editorTab !== "messages";
    $("font-panel").hidden = !editing || editorTab !== "fonts";
    $("tab-details").hidden = state.project?.origin === "native-source";
    $("tab-fonts").hidden = state.project?.origin === "native-source";
    for (const name of ["messages","details","fonts"]) { $("tab-"+name).setAttribute("aria-selected",String(editorTab===name)); $("tab-"+name).tabIndex=editorTab===name?0:-1; }
    $("editor-bar").hidden = !editing;
    $("reference-tools").hidden = !editing;
    $("sharing").hidden = phase !== "review";
    $("installed").hidden = phase !== "installed";
    label("review-title",reviewPurpose === "install" ? "builder.language.review_install_title" : "builder.language.review_export_title");
    $("install").hidden = reviewPurpose !== "install";
    $("publish").hidden = reviewPurpose !== "publish";
    $("check").hidden = reviewPurpose !== "publish";
    $("publication-options").hidden = reviewPurpose !== "publish";
    $("install-copy").hidden = reviewPurpose !== "install";
    $("replace-install").closest("label").hidden = reviewPurpose !== "install" || !installedExists;
    syncImportPreview();
    saveIndicator();
  }
  function projectDestination() {
    phase = workflow === "install" ? "review" : workflow === "clone" ? "clone" : "edit";
    reviewPurpose = "install";
    if (phase === "clone") $("clone").elements.name.value = ui.text("builder.language.clone_default",{name:state.project.metadata.name});
    workflowView();
  }
  function renderTextCoverage(report) {
    const host=$("text-coverage");host.replaceChildren();host.hidden=!report;
    if(!report)return;
    host.append(phrase("builder.coverage.contract",{provided:report.required.provided,total:report.required.total},"p"));
    if(report.runtime)host.append(phrase("builder.coverage.live_optional",{provided:report.liveOptional.provided,total:report.liveOptional.total},"p"));
    host.append(phrase("builder.coverage.explanation",{},"p"));
    for(const surface of report.surfaces){
      const details=document.createElement("details"),summary=document.createElement("summary");
      summary.append(phrase(coverageSurfaceKeys[surface.surface]),document.createTextNode(" · "),phrase("builder.coverage.supplied",{provided:surface.provided,total:surface.total}));
      details.append(summary,phrase("builder.coverage.review",{done:surface.done,wip:surface.wip,unreviewed:surface.notStarted,unchanged:surface.unchangedSource},"p"));
      if(surface.missing?.length){
        details.append(phrase("builder.coverage.fallback_ids",{},"p"));
        const ids=document.createElement("pre");ids.setAttribute("dir","ltr");ids.textContent=surface.missing.join("\n");details.append(ids);
      }
      host.append(details);
    }
    if(report.dormant?.length)host.append(phrase("builder.coverage.dormant",{ids:report.dormant.join(", ")},"p"));
  }
  function feedback(text, error = false) { raw("feedback",text); $("feedback").dataset.error = String(error); if(!error)window.workshopFeedback?.clear($("feedback")); }
  function hasEdits() { return dirty || detailsDirty || noticeDirty || fontsDirty; }
  function saveIndicator() {
    const native = state.project?.origin === "native-source";
    label("save-state",native?"builder.language.read_only":saveFailure?"builder.language.not_saved":hasEdits()?"builder.language.unsaved":"builder.language.saved_workshop",{detail:saveFailure});
    $("editor-bar").dataset.dirty = String(hasEdits());
    $("save-progress").disabled = busy || !state.project || native || !hasEdits();
    $("save").disabled = busy || !state.project || native || !hasEdits();
    $("save-fonts").disabled = busy || !state.project || native || !hasEdits();
    label("font-check",hasEdits()?"builder.editor.save_check":"builder.editor.check_coverage");
  }
  function discard() { return !hasEdits() || window.confirm(ui.text("builder.language.discard_confirm")); }
  function identity() { return {projectID: state.project?.metadata.id, revision: state.project?.revision}; }
  async function request(endpoint, data, query = {}) {
    if(closed) throw failure("builder.closed");
    const url = new URL("localization/" + endpoint, location.href);
    if (data === undefined) for (const [key, value] of Object.entries(query)) url.searchParams.set(key, value);
    const response = await fetch(url, data === undefined ? {cache: "no-store"} : data instanceof FormData ? {method: "POST", body: data} : {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(data)});
    if(closed) throw failure("builder.closed");
    if (!response.ok) {
      let body; try { body = await response.json(); } catch { body = {}; }
      const key=["builder.language.request_conflict","builder.language.request_failed","builder.language.chooser_unavailable","builder.language.chooser_failed","builder.language.chooser_busy"].includes(body.errorCode)
        ? body.errorCode : "builder.language.request_failed";
      const error = failure(key,{detail:body.error||String(response.status)});
      error.status = response.status; throw error;
    }
    return response;
  }
  async function json(endpoint, data, query) {
    const response=await request(endpoint, data, query);
    const result=await (window.workshopFeedback?window.workshopFeedback.readJSON(response):response.json());
    if(closed) throw failure("builder.closed");
    return result;
  }
  function readonly() {
    if(closed) return;
    const native = !state.project || state.project.origin === "native-source";
    for (const el of panel.querySelectorAll("#loc-message-editor button, #loc-body, #loc-message-status, #loc-placeholders")) el.disabled = native;
    $("preview").disabled = !message;
    $("independent").disabled = native || !message?.body.trimStart().startsWith("@alias ");
    for (const id of ["install","publish","check","review-install","review-publish"]) $(id).disabled = native;
    for (const el of $("font-stack").querySelectorAll("input,select,button")) el.disabled = native || busy || el.dataset.unavailable === "true";
    $("font-add").disabled = native || busy || fontDraft.length >= 9;
    saveIndicator();
    syncImportPreview();
  }
  async function run(action) {
    if (busy || closed) return;
    busy = true;
    const controls = [...panel.querySelectorAll("button,input,textarea,select")].map(el => [el, el.disabled]);
    for (const [el] of controls) el.disabled = true;
    panel.setAttribute("aria-busy", "true");
    try { await action(); } catch (error) {
      if(!closed){
        feedbackKey(error.uiKey||"builder.language.request_failed",error.uiArgs||{detail:error.message},true);
        window.workshopFeedback?.show($("feedback"),error,{operation:"Language pack / editor"});
      }
    }
    finally {
      for (const [el, disabled] of controls) el.disabled = closed || disabled;
      busy = false; panel.removeAttribute("aria-busy"); readonly();
    }
  }
  function option(value, text) { const el = document.createElement("option"); el.value = value; el.textContent = text; return el; }
  function markDirty() { dirty = true; saveFailure=""; label("dirty","builder.editor.unsaved_message"); panel.querySelector(".loc-preview").hidden = true; saveIndicator(); }
  function fillMetadata() {
    const p = state.project; if (!p) return;
    for (const [key, value] of Object.entries(p.metadata)) if ($("metadata").elements.namedItem(key)) $("metadata").elements.namedItem(key).value = value;
    $("metadata").elements.notes.value = p.notes;
    updateContentLanguage();
    label("fonts","builder.editor.font_summary",{fonts:[p.fonts.primary, ...(p.fonts.fallback || [])].join(" → ")});
    fontDraft = [p.fonts.primary, ...(p.fonts.fallback || [])]; fontUploads = new Map(); fontsDirty = false;
    $("font-report").replaceChildren(); renderFonts();
    $("notices").replaceChildren(keyedOption("", "builder.editor.new_notice"));
    for (const name of Object.keys(p.notices).sort()) $("notices").append(option(name, name));
    $("notice").reset();
    detailsDirty = noticeDirty = false;
  }
  function fontChanged() {
    fontsDirty=true; saveFailure="";
    for(const name of fontUploads.keys()) if(!fontDraft.includes(name)) fontUploads.delete(name);
    $("font-report").replaceChildren(); renderFonts(); saveIndicator();
  }
  function renderFonts() {
    const known = [...new Set(["builtin:actraiser-sans", state.project?.fonts.primary, ...(state.project?.fonts.fallback || []), ...fontUploads.keys(), ...fontDraft].filter(Boolean))];
    $("font-list").replaceChildren(...fontDraft.map((reference,index)=>{
      const row=document.createElement("li"); row.className="loc-font-row";
      const label=document.createElement("label"); label.append(phrase(index?"builder.editor.fallback_number":"builder.editor.primary_font",{number:index}));
      const select=document.createElement("select"); select.required=true; select.append(keyedOption("","builder.editor.choose_font"));
      for(const name of known) select.append(name==="builtin:actraiser-sans"?keyedOption(name,"builder.editor.bundled_font"):option(name,name));
      select.value=reference; select.addEventListener("change",()=>{fontDraft[index]=select.value; fontChanged();}); label.append(select); row.append(label);
      if(fontUploads.has(reference))label.append(phrase("builder.editor.pending_font",{},"small"));
      const fileLabel=document.createElement("label"); fileLabel.append(phrase("builder.editor.choose_font_file"));
      const file=document.createElement("input"); file.type="file"; file.accept=".ttf,.otf";
      file.addEventListener("change",()=>{const chosen=file.files[0];if(!chosen)return;if(!chosen.size||chosen.size>64*1024*1024){feedbackKey("builder.editor.font_size_error",{},true);file.value="";return;}const path="fonts/"+chosen.name;fontUploads.set(path,chosen);fontDraft[index]=path;fontChanged();}); fileLabel.append(file);row.append(fileLabel);
      window.workshopFileInputs?.enhance(file);
      const actions=document.createElement("div");actions.className="loc-row";
      for(const [key,aria,emptyAria,offset] of [
        ["builder.editor.move_up","builder.editor.move_up_font","builder.editor.move_up_slot",-1],
        ["builder.editor.move_down","builder.editor.move_down_font","builder.editor.move_down_slot",1],
        ["builder.editor.remove","builder.editor.remove_font","builder.editor.remove_slot",0]]) {
        const button=phrase(key,{},"button");button.type="button";
        ui.attribute(button,"aria-label",reference?aria:emptyAria,{font:reference,number:index+1});
        button.dataset.unavailable=String(offset?index+offset<0||index+offset>=fontDraft.length:fontDraft.length===1);
        button.disabled=button.dataset.unavailable==="true";
        button.addEventListener("click",()=>{if(offset)[fontDraft[index],fontDraft[index+offset]]=[fontDraft[index+offset],fontDraft[index]];else fontDraft.splice(index,1);fontChanged();});actions.append(button);
      }
      row.append(actions);return row;
    }));
    readonly();
  }
  $("font-add").addEventListener("click",()=>{if(fontDraft.length<9){fontDraft.push("");fontChanged();$("font-list").lastElementChild.querySelector("select").focus();}});
  $("font-check").addEventListener("click",async()=>{
    if(hasEdits()) await saveProgress();
    if(hasEdits()||busy)return;
    return run(async()=>{
      const sample=$("font-sample").value;
      const result=await json("font-coverage",{...identity(),samples:sample?[sample]:[]});
      const summary=phrase(result.complete?"builder.editor.coverage_complete":"builder.editor.coverage_missing",{count:result.complete?result.scalars:result.missingCount},"p");
      const list=document.createElement("ul");
      for(const gap of result.missing){const item=document.createElement("li");item.textContent=gap.codepoint+" “"+gap.character+"” — "+gap.locations.map(loc=>loc.messageID?loc.messageID+" · "+loc.source+":"+loc.line:loc.source).join("; ");list.append(item);}
      const values=phrase(result.dynamicValues.length?"builder.editor.unresolved_values":"builder.editor.no_unresolved_values",{values:result.dynamicValues.join(", ")},"p");values.className="loc-help";
      $("font-report").replaceChildren(summary,list,values);
    });
  });
  async function projects() {
    const rows = await json("projects"), old = $("projects").value;
    $("projects").replaceChildren(keyedOption("", "builder.language.choose_project"));
    $("reference-project").replaceChildren(keyedOption("",state.sourceAvailable?"builder.editor.native_reference":"builder.editor.native_unavailable"));
    for (const row of rows) {
      const el = option(row.id, row.name + " · " + row.locale + " · " + row.id + (row.error ? " — " + row.error : ""));
      contentLanguage(el);
      el.disabled = !!row.error; $("projects").append(el);
      if (!row.error && row.id !== state.project?.metadata.id) { const ref=option(row.id, row.name + " · " + row.locale); contentLanguage(ref); $("reference-project").append(ref); }
    }
    $("projects").value = state.project?.metadata.id || old;
    // A built US source need not have an author-store copy yet.
    $("reference-project").value = [...$("reference-project").options].some(o => o.value === state.reference?.id) ? state.reference.id : "";
    if (workflow === "home") await loadCatalog();
  }
  async function loadCatalog(focusKey) { catalog = await json("catalog"); renderCatalog(focusKey); }
  function renderCatalog(focusKey) {
    const query=$("library-search").value.trim().toLocaleLowerCase(), filter=$("library-filter").value;
    const rows=catalog.filter(p => [p.name,p.installedName,p.locale,p.id].join(" ").toLocaleLowerCase().includes(query) && (filter === "all" || filter === "installed" && p.installed || filter === "workshop" && !p.installed));
    $("library-empty").hidden=rows.length>0;
    label("library-summary","builder.language.library_counts",{enabled:catalog.filter(p=>p.installed&&p.enabled).length,disabled:catalog.filter(p=>p.installed&&!p.enabled).length,projects:catalog.filter(p=>p.project).length});
    label("library-empty",filter==="installed"&&!catalog.some(p=>p.installed)?"builder.language.library_empty":"builder.language.no_packages");
    $("library").replaceChildren(...rows.map(p=>{
      const card=document.createElement("article"); card.className="loc-package-card"; card.dataset.packageId=p.id; card.dataset.packageKey=p.key||"";
      const title=document.createElement("h3"); title.textContent=p.name;
      contentLanguage(title,{locale:p.locale});
      const badge=document.createElement("span"); badge.className="loc-package-badge"; badge.dataset.error=String(!!p.error);
      if(p.error || !p.installed) ui.set(badge,p.error?"builder.language.needs_attention":p.readOnly?"builder.language.read_only":"builder.language.workshop_only");
      const info=document.createElement("p"); info.className="loc-help"; info.textContent=(p.locale?p.locale+" · ":"")+p.id;
      if(p.installed&&p.installedName!==p.name){const name=document.createElement("span");ui.set(name,"builder.language.installed_as",{name:p.installedName});info.append(name);}
      if(p.installed&&p.key!==p.id){const folder=document.createElement("span");ui.set(folder,"builder.language.folder",{folder:p.key});info.append(folder);}
      if(p.error || !p.installed) card.append(badge);
      card.append(title,info);
      if(p.installed){
        const label=document.createElement("label"); label.className="loc-package-enable";
        const checkbox=document.createElement("input"); checkbox.type="checkbox"; checkbox.checked=p.enabled; checkbox.disabled=!p.installedRevision||(!p.enabled&&!!p.error); ui.attribute(checkbox,"aria-label","builder.language.enable_pack",{name:p.installedName});
        const caption=document.createElement("span");ui.set(caption,p.enabled?"builder.language.enabled_game":"builder.language.disabled_game");
        label.append(checkbox,caption);card.prepend(label);
        checkbox.addEventListener("change",()=>{
          if(busy){checkbox.checked=p.enabled;return;}
          const enabled=checkbox.checked;
          run(async()=>{
            try { await json("set-enabled",{id:p.id,directory:p.key,expected:p.installedRevision,enabled});feedbackKey("builder.language.availability_saved"); }
            finally { await loadCatalog(p.key); }
          });
        });
      }
      const actions=document.createElement("div"); actions.className="loc-row";
      const action=(key,handler)=>{ const b=document.createElement("button"); b.type="button"; ui.set(b,key); b.addEventListener("click",handler); actions.append(b); return b; };
      if (p.project) {
        action(p.readOnly?"builder.language.view_reference":"builder.language.edit_project",()=>openProject(p.id));
        if (!p.readOnly) action(p.installed?"builder.language.review_update":"builder.language.review_install",()=>openProject(p.id,true));
      }
      if(p.installed&&p.installedRevision) action("builder.language.uninstall",()=>run(async()=>{
        if(!window.confirm(ui.text("builder.language.uninstall_confirm",{name:p.installedName,id:p.id,folder:p.key}))) return;
        const result=await json("uninstall",{id:p.id,directory:p.key,expected:p.installedRevision,confirmUninstall:true});
        await loadCatalog(); feedbackKey("builder.language.uninstalled",{backup:result.backup});
      })).className="loc-uninstall";
      if(p.error) { const error=document.createElement("p"); error.className="loc-help"; error.textContent=p.error; card.append(error); }
      else if(!p.project) { const note=document.createElement("p"); note.className="loc-help"; ui.set(note,"builder.language.no_project"); card.append(note); }
      card.append(actions); return card;
    }));
    if(typeof focusKey==="string") [...$("library").children].find(row=>row.dataset.packageKey===focusKey)?.querySelector('input[type="checkbox"]')?.focus({preventScroll:true});
  }
  $("library-search").addEventListener("input",renderCatalog);
  $("library-filter").addEventListener("change",renderCatalog);
  async function adopt(next, preserveMessage = false) {
    state = next; loaded = true; saveFailure="";
    label("path","builder.language.paths",{root:next.root});
    const p = state.project;
    if (!p) { workflowView(); await projects(); return; }
    fillMetadata();
    const totals = p.totals.reduce((a, x) => ({total:a.total+x.total, done:a.done+x.done, wip:a.wip+x.wip}), {total:0,done:0,wip:0});
    $("editor-title").textContent = p.metadata.name;
    contentLanguage($("editor-title"),{locale:p.metadata.locale});
    label("progress","builder.language.progress",totals);
    raw("publication-report","");
    renderTextCoverage(null);
    $("download").hidden = true;
    dirty = false; raw("dirty","");
    if (!preserveMessage) {
      selected = ""; message = null; $("rights").checked = false; $("wip").checked = false; $("replace-install").checked = false; $("message-editor").hidden = true;
      label("message-title","builder.editor.choose_message");
      raw("message-context",""); label("message-context","builder.editor.choose_message_help");
    }
    await projects(); await tree();
    const installed = await json("installation", identity());
    installedExists = !!installed.installed;
    label("install-state",installed.installed ? "builder.language.existing_install" : "builder.language.not_installed");
    if (preserveMessage && selected) await showMessage(selected);
    readonly();
    workflowView();
  }
  async function tree() {
    $("tree").hidden = false; $("results").hidden = true; $("more").hidden = true;
    $("tree").replaceChildren(await treeChildren(""));
  }
  async function treeChildren(parent) {
    const rows = await json("tree", undefined, {...identity(), parent, view:$("tree-view").value});
    const ul = document.createElement("ul");
    for (const row of rows) {
      const li = document.createElement("li");
      if (row.has_children) {
        const details = document.createElement("details"), summary = document.createElement("summary"), content = document.createElement("div");
        summary.append(navigationCaption(row.label_key,row.label),document.createTextNode(" · "),phrase("builder.editor.tree_count",{done:row.done,total:row.total}));
        details.append(summary, content); li.append(details);
        let fetched = false;
        const expand = async () => {
          if (!fetched) { content.replaceChildren(await treeChildren(row.id)); fetched = true; }
        };
        details.addEventListener("toggle", () => { if (details.open) { expanded.add(row.id); if (!fetched) run(expand); } else expanded.delete(row.id); });
        if (expanded.has(row.id) || selected.startsWith(row.id + ".")) { await expand(); details.open = true; }
      }
      if (row.is_message) li.append(messageButton(row.id,row.label,row.done?"done":row.wip?"wip":"not_started",false,row.label_key,row.shared));
      ul.append(li);
    }
    return ul;
  }
  function messageButton(id, text, status, fallback=false,captionKey="",shared=false) {
    const button = document.createElement("button"); button.type = "button"; button.title = id;
    button.append(navigationCaption(captionKey,text));
    if(shared)button.append(document.createTextNode(" "),phrase("builder.navigation.shared"));
    button.append(document.createTextNode(" · "),phrase(statusKeys[status]||statusKeys.not_started));
    if(fallback) button.append(document.createTextNode(" · "),phrase("builder.editor.native_fallback"));
    button.setAttribute("aria-current", String(selected === id));
    button.addEventListener("click", () => run(async () => { if (discard()) { fillMetadata(); await showMessage(id); } }));
    return button;
  }
  function emptyBody(ref) { return [...ref.anchors.map(id => "@anchor " + id), "@empty", "@end", ""].join("\n"); }
  function showShape(p) {
    const descriptions = [];
    const add=(key,args)=>descriptions.push(phrase(key,args,"li"));
    if (p?.maximum_pages) add("builder.editor.maximum_pages",{count:p.maximum_pages});
    if (p?.maximum_lines) add("builder.editor.maximum_lines",{count:p.maximum_lines});
    if (p?.required_nonempty_lines) add("builder.editor.required_choices",{count:p.required_nonempty_lines});
    if (p?.keyboard) add("builder.editor.keyboard_shape",{rows:p.keyboard.rows,columns:p.keyboard.columns,lines:p.keyboard.maximum_lines,bytes:p.keyboard.maximum_page_bytes});
    for (const rule of p?.table?.rules || []) {
      const item=document.createElement("li");
      item.append(phrase(rule.last_line===255?"builder.editor.all_rows":rule.first_line===rule.last_line?"builder.editor.single_row":"builder.editor.row_range",{first:rule.first_line+1,last:rule.last_line+1}),document.createTextNode(": "),phrase(rule.native_reserved?"builder.editor.native_artwork":"builder.editor.table_fields",rule.native_reserved?{}:{fields:rule.fields.join(", ")}));
      descriptions.push(item);
    }
    $("shape").hidden = !descriptions.length;
    $("shape-rules").replaceChildren(...descriptions);
  }
  async function showMessage(id) {
    const data = await json("message", undefined, {...identity(), id});
    selected = id; message = data.message;
    showShape(message.reference.presentation);
    raw("message-title",data.location?.title || id);
    if(data.location?.title_key)ui.set($("message-title"),"builder.navigation."+data.location.title_key);
    ui.unbind($("message-context"));
    const loc=data.location;
    $("message-context").replaceChildren(document.createTextNode(id+" · "));
    if(loc?.shared)$("message-context").append(phrase("builder.navigation.context.shared"),document.createTextNode(" "));
    $("message-context").append(navigationCaption(loc?.context_key||loc?.group_key,loc?.context||loc?.group_label||""),document.createTextNode("\n"),phrase(message.present?"builder.editor.saved_path":"builder.editor.message_absent",{path:message.path}));
    if(!message.reference.native_in_profile) $("message-context").append(document.createTextNode(" "),phrase("builder.editor.cross_region"));
    renderReference(data);
    label("anchors",message.reference.anchors.length?"builder.editor.required_anchors":"builder.editor.no_anchors",{anchors:message.reference.anchors.join(" → ")});
    $("body").value = message.present ? message.body : emptyBody(message.reference);
    $("message-status").value = message.status;
    $("placeholders").replaceChildren(keyedOption("", "builder.editor.choose_value"));
    for (const p of message.reference.placeholders) $("placeholders").append(Object.hasOwn(valueKindKeys,p.kind)?keyedOption(p.name,valueKindKeys[p.kind],{name:p.name}):option(p.name,p.name+" ("+p.kind+")"));
    $("message-editor").hidden = false; panel.querySelector(".loc-preview").hidden = true;
    dirty = false; raw("dirty","");
    for (const b of panel.querySelectorAll("#loc-tree button,#loc-results button")) b.setAttribute("aria-current", String(b.title === id));
    readonly();
  }
  function renderReference(data) {
    const m = data.referenceMetadata;
    if(m) label("reference-title",state.reference&&state.reference.id!==m.id?"builder.editor.reference_fallback":"builder.editor.reference_name",{name:m.name,locale:m.locale});
    else label("reference-title","builder.editor.source_reference");
    if(data.reference?.body) raw("reference-body",data.reference.body);
    else label("reference-body","builder.editor.reference_missing");
    if(data.reference?.body) contentLanguage($("reference-body"),m);
    else { $("reference-body").removeAttribute("dir"); $("reference-body").removeAttribute("lang"); }
  }
  async function refreshReference(next) {
    // This is deliberately not adopt(): it must preserve the selected message,
    // caret, status, metadata, notices and every unsaved draft verbatim.
    state.reference = next.reference; state.sourceAvailable = next.sourceAvailable;
    await projects();
    if (selected) renderReference(await json("message", undefined, {...identity(), id:selected}));
  }
  function referenceForm(open) {
    $("extract-reference").hidden = !open;
    $("extract-reference-toggle").setAttribute("aria-expanded", String(open));
    if (open) $("extract-reference").elements.file.focus();
  }
  $("extract-reference-toggle").addEventListener("click", () => referenceForm($("extract-reference").hidden));
  $("cancel-reference").addEventListener("click", () => referenceForm(false));
  $("reference-project").addEventListener("change", () => run(async () => {
    try { await refreshReference(await json("reference", {...identity(), id:$("reference-project").value})); feedbackKey("builder.editor.reference_changed"); }
    finally { const id=state.reference?.id; $("reference-project").value=[...$("reference-project").options].some(o=>o.value===id)?id:""; }
  }));
  function insert(text, command = false) {
    const input = $("body"), at = input.selectionStart;
    if (command) text = (at && input.value[at-1] !== "\n" ? "\n" : "") + text + "\n";
    input.setRangeText(text, input.selectionStart, input.selectionEnd, "end"); input.focus(); markDirty();
  }
  function publicationOptions() { return {...identity(), confirmRights: $("rights").checked, includeWIP: $("wip").checked}; }
  async function download(endpoint) {
    if (hasEdits()) throw failure("builder.language.save_before_export");
    const data = await json(endpoint, {...publicationOptions(), prepareDownload:true});
    const a = $("download");
    a.href = data.url; a.download = data.name; ui.set(a,"builder.language.download_name",{name:data.name}); a.hidden = false;
    $("editor-actions").open = false;
    a.scrollIntoView({block:"center"}); a.focus();
    feedbackKey(endpoint === "backup" ? "builder.language.backup_ready" : "builder.language.export_ready");
  }
  function submit(id, action) { $(id).addEventListener("submit", event => { event.preventDefault(); const form = event.currentTarget, data = new FormData(form); run(() => action(data, form)); }); }
  $("open").addEventListener("click", () => run(async () => { if (discard()) { await adopt(await json("open", {id: $("projects").value})); projectDestination(); if(phase==="review") await prepareReview("install"); feedbackKey("builder.language.opened_feedback"); } }));
  $("refresh").addEventListener("click", () => run(projects));
  submit("extract-reference", async data => {
    data.set("intent", "reference");
    for (const [key, value] of Object.entries(identity())) data.set(key, value);
    await refreshReference(await json("extract", data));
    referenceForm(false); $("extract-reference").reset();
    feedbackKey("builder.editor.reference_extracted");
  });
  submit("extract", async (data) => { if (discard()) { await adopt(await json("extract", data)); if (workflow === "create") { phase="choose"; workflowView(); } else projectDestination(); feedbackKey("builder.language.extracted_feedback"); } });
  submit("create", async data => { if (discard()) { await adopt(await json("create", {metadata: {...Object.fromEntries(data), direction:"auto"}})); phase="edit"; workflowView(); feedbackKey("builder.language.created_feedback"); } });
  submit("clone", async data => { if (discard()) { await adopt(await json("clone", {...identity(),newID:data.get("newID"),metadata:{name:data.get("name")}})); phase="edit"; workflowView(); feedbackKey("builder.language.cloned_feedback"); } });
  function syncImportPreview() {
    if (!importPreview) { $("accept-import").disabled=true; return; }
    const copying=!!$("import-new-id").value.trim(), installing=workflow==="install";
    $("project-conflict").hidden=!importPreview.existingProject;
    $("replace-project").closest("label").hidden=copying;
    $("import-install-conflict").hidden=!installing||!importPreview.installed||copying;
    label("accept-import",installing?"builder.language.import_install":workflow==="clone"?"builder.language.import_clone":"builder.language.import_edit");
    $("accept-import").disabled=busy || installing&&!!importPreview.installError || !!importPreview.existingProject&&!copying&&!$("replace-project").checked || installing&&!!importPreview.installed&&!copying&&!$("import-replace-installed").checked;
  }
  async function previewImport(endpoint, data) {
    importPreview=null; workflowView(); feedbackKey("builder.language.loading_pack");
    importPreview=await json(endpoint,data);
    $("replace-project").checked=$("import-replace-installed").checked=false;
    $("import-new-id").value="";
    const p=importPreview, m=p.metadata;
    $("import-name").textContent=m.name;
    label("import-summary","builder.language.import_summary",{locale:m.locale,id:m.id,count:p.messages,author:m.author,license:m.license});
    if(p.existingProject) label("project-conflict-note","builder.language.project_conflict",{id:m.id,name:p.existingProject.name});
    else raw("project-conflict-note","");
    label("import-warning",p.installError?"builder.language.import_not_installable":"builder.language.import_kept",{detail:p.installError});
    workflowView(); feedback("");
    $("import-preview").scrollIntoView({block:"nearest"});
    $("import-name").focus({preventScroll:true});
  }
  async function loadDirectory(directory) {
    if(!directory.trim()||!discard()) return;
    await previewImport("directory",{directory,previewImport:true});
  }
  $("pick-directory").addEventListener("click",()=>run(async()=>{
    if(!discard()) return;
    const result=await json("choose-directory",{});
    if(result.cancelled) return;
    $("directory").elements.directory.value=result.directory;
    await loadDirectory(result.directory);
  }));
  submit("directory",data=>loadDirectory(data.get("directory")));
  $("directory").elements.directory.addEventListener("change",event=>{ const path=event.target.value; run(()=>loadDirectory(path)); });
  let dragDepth=0;
  function clearDrop() { dragDepth=0; $("drop-overlay").hidden=true; }
  function dropNotice(key) {
    label("drop-message",key); $("drop-overlay").dataset.notice="true";
    $("dismiss-drop").hidden=false; $("drop-overlay").hidden=false;
  }
  $("dismiss-drop").addEventListener("click",clearDrop);
  function importFiles(files, install=false, backup=false) {
    if(closed || !files.length) return Promise.resolve();
    if(busy) { dropNotice("builder.language.drop_busy"); return Promise.resolve(); }
    // One detached server preview per session; never silently install only the
    // first item of a multi-file drop or overwrite a preview still loading.
    if(files.length!==1) { dropNotice("builder.language.one_archive"); return Promise.resolve(); }
    const file=files[0], extension=file.name.split(".").pop().toLowerCase();
    if(!(backup?["arproject","zip"]:["arlang"]).includes(extension)) {
      dropNotice(backup?"builder.language.backup_types":"builder.language.package_types"); return Promise.resolve();
    }
    if(!file.size || file.size>257*1024*1024) { dropNotice("builder.language.archive_size"); return Promise.resolve(); }
    return run(async()=>{
      if(!discard()) return;
      if(install) {
        if(window.workshopOpenLanguages && !window.workshopOpenLanguages()) { dropNotice("builder.language.build_required"); return; }
        if(!loaded) await adopt(await json("state"));
        dirty=detailsDirty=noticeDirty=fontsDirty=false; fontUploads.clear();
        workflow="install"; phase="choose"; installPath="import"; editorTab="messages";
        $("download").hidden=true;
      }
      clearDrop();
      const data=new FormData(); data.set("file",file); data.set("intent","preview");
      await previewImport("import",data);
    });
  }
  for(const id of ["quick-import","import","import-backup"]) {
    const form=$(id), input=form.elements.file;
    form.addEventListener("submit",event=>event.preventDefault());
    input.addEventListener("change",()=>importFiles([...input.files],id==="quick-import",id==="import-backup").finally(()=>{
      // A failed/cancelled selection must be selectable again without first
      // choosing a different file. The preview already owns its uploaded bytes.
      input.value=""; window.workshopFileInputs?.refresh(input);
    }));
  }
  const fileDrag=event=>[...(event.dataTransfer?.types||[])].includes("Files") || event.dataTransfer?.files?.length>0;
  const fileTarget=event=>event.target?.closest?.('input[type="file"],.file-control');
  document.addEventListener("dragenter",event=>{
    if(!fileDrag(event) || fileTarget(event)) return;
    event.preventDefault(); ++dragDepth;
    if(closed || busy) return;
    label("drop-message","builder.language.drop_prompt"); $("drop-overlay").dataset.notice="false";
    $("dismiss-drop").hidden=true; $("drop-overlay").hidden=false;
  });
  document.addEventListener("dragover",event=>{
    if(!fileDrag(event)) return;
    if(fileTarget(event)) { clearDrop(); return; }
    event.preventDefault(); event.dataTransfer.dropEffect=closed||busy?"none":"copy";
  });
  document.addEventListener("dragleave",event=>{
    if(!fileDrag(event)) return;
    if(--dragDepth<=0) clearDrop();
  });
  document.addEventListener("dragend",clearDrop);
  document.addEventListener("drop",event=>{
    clearDrop();
    if(!fileDrag(event) || fileTarget(event)) return; // ROM/font controls keep native drop behavior.
    event.preventDefault();
    return importFiles([...(event.dataTransfer.files||[])],true);
  });
  document.addEventListener("keydown",event=>{ if(event.key==="Escape") clearDrop(); });
  for(const id of ["replace-project","import-new-id","import-replace-installed"]) $(id).addEventListener("input",syncImportPreview);
  $("accept-import").addEventListener("click",()=>run(async()=>{
    if(!importPreview) return;
    const incoming=importPreview, newID=$("import-new-id").value.trim();
    const replaceInstall=!newID&&$("import-replace-installed").checked;
    if(incoming.existingProject&&!newID&&!$("replace-project").checked) throw failure("builder.language.choose_conflict");
    if(workflow==="install"&&incoming.installed&&!newID&&!replaceInstall) throw failure("builder.language.confirm_replace");
    await adopt(await json("accept-import",{importToken:incoming.token,newID,replace:!newID&&$("replace-project").checked,expected:incoming.existingProject?.revision||""}));
    importPreview=null; projectDestination();
    if(workflow==="install") {
      try { await installCurrent(replaceInstall); }
      catch(error) { await prepareReview("install"); throw failure("builder.language.import_partial",{detail:error.uiArgs?.detail||error.message}); }
    } else feedbackKey("builder.language.imported_edit");
  }));
  $("metadata").addEventListener("input", () => { detailsDirty = true; saveFailure=""; updateContentLanguage(); saveIndicator(); });
  $("notice").addEventListener("input", () => { noticeDirty = true; saveFailure=""; saveIndicator(); });
  $("notices").addEventListener("change", () => {
    if (noticeDirty && !window.confirm(ui.text("builder.editor.discard_notice"))) { $("notices").value = ""; return; }
    const name = $("notices").value;
    $("notice").elements.noticeName.value = name.replace(/^notices\//, "");
    $("notice").elements.noticeText.value = state.project.notices[name] || ""; noticeDirty = false; saveIndicator();
  });
  $("body").addEventListener("input", markDirty); $("message-status").addEventListener("change", markDirty);
  function saveProgress() {
    if (busy || !hasEdits() || state.project?.origin === "native-source") return;
    for (const [id, changed] of [["metadata", detailsDirty],["notice", noticeDirty],["font-stack", fontsDirty]]) {
      if (changed && !$(id).checkValidity()) { editorTab=id==="font-stack"?"fonts":"details"; workflowView(); $(id).reportValidity(); return; }
    }
    // Capture before run() disables controls: FormData omits disabled fields.
    const fields = Object.fromEntries(new FormData($("metadata"))), notes = fields.notes; delete fields.notes;
    const q = {...identity(), saveMessage:dirty, saveDetails:detailsDirty, saveNotice:noticeDirty, id:selected, body:$("body").value, status:$("message-status").value, metadata:fields, notes, ...Object.fromEntries(new FormData($("notice")))};
    q.saveFonts=fontsDirty; q.fonts={primary:fontDraft[0],fallback:fontDraft.slice(1)};
    const uploads=[...fontUploads].filter(([name])=>fontDraft.includes(name));
    let payload=q;
    if(fontsDirty&&uploads.length){q.fontPaths=uploads.map(([name])=>name);payload=new FormData();payload.set("request",JSON.stringify(q));uploads.forEach(([,file],i)=>payload.set("font"+i,file));}
    return run(async () => {
      let next;
      try { next = await json("save", payload); }
      catch (error) { saveFailure=error.uiArgs?.detail||error.message; throw error; }
      await adopt(next, true);
      feedbackKey("builder.language.saved_feedback");
    });
  }
  for (const id of ["save", "save-progress"]) $(id).addEventListener("click", saveProgress);
  for (const id of ["metadata", "notice", "font-stack"]) $(id).addEventListener("submit", event => { event.preventDefault(); saveProgress(); });
  document.addEventListener("keydown", event => {
    if (!panel.hidden && phase === "edit" && workflow !== "home" && (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") { event.preventDefault(); saveProgress(); }
  });
  $("insert-value").addEventListener("click", () => { if ($("placeholders").value) insert("{" + $("placeholders").value + "}"); });
  for (const el of panel.querySelectorAll("[data-loc-insert]")) el.addEventListener("click", () => insert(el.dataset.locInsert, true));
  $("independent").addEventListener("click", () => run(async () => { if (dirty && !window.confirm(ui.text("builder.editor.replace_alias"))) return; $("body").value = (await json("materialize", {...identity(), id:selected})).body; markDirty(); }));
  $("empty").addEventListener("click", () => { if (window.confirm(ui.text("builder.editor.clear_confirm"))) { $("body").value = emptyBody(message.reference); markDirty(); } });
  $("preview").addEventListener("click", () => run(async () => {
    const ops = await json("preview", {...identity(), id:selected, body:$("body").value, status:$("message-status").value});
    const container = $("preview-content"); container.replaceChildren();
    let page, number = 0;
    const newPage = () => { const heading=phrase("builder.editor.page_number",{number:++number},"h4"); page = document.createElement("div"); page.className = "loc-preview-page"; contentLanguage(page,draftLanguage()); container.append(heading,page); };
    newPage();
    for (const op of ops) {
      if (op.op === "page") newPage();
      else if (op.op === "text") page.append(document.createTextNode(op.value));
      else if (op.op === "line" || op.op === "preferred_line" || op.op === "paragraph") page.append(document.createTextNode(op.op === "paragraph" ? "\n\n" : "\n"));
      else if (op.op === "placeholder") { const value = document.createElement("span"); value.textContent = "⟦" + op.name + (op.minimum_digits ? ":0" + op.minimum_digits : "") + "⟧"; value.className="loc-preview-value"; contentLanguage(value,{direction:"ltr"}); ui.attribute(value,"title","builder.editor.runtime_value"); page.append(value); }
      else { const control = document.createElement("div"); control.className = "loc-preview-control"; contentLanguage(control,{direction:"ltr"}); control.textContent = op.op + (op.id ? ": " + op.id : ""); if(!op.id&&op.frames)control.append(document.createTextNode(": "),phrase("builder.editor.frames",{count:op.frames})); page.append(control); }
    }
    panel.querySelector(".loc-preview").hidden = false; feedbackKey("builder.editor.preview_valid");
  }));
  async function search(more) {
    if (!more) { offset = 0; searchSnapshot = {q:$("search").elements.q.value, status:$("search").elements.status.value, ...identity()}; ui.unbind($("results")); $("results").replaceChildren(); }
    const data = await json("search", undefined, {...searchSnapshot, offset});
    $("tree").hidden = true; $("results").hidden = false;
    for (const row of data.rows) $("results").append(messageButton(row.id,row.title||row.id,row.status,!row.present,row.label_key,row.shared));
    offset += data.rows.length; $("more").hidden = offset >= data.total;
    if (!data.total) label("results","builder.editor.no_messages");
    feedbackKey("builder.editor.search_count",{shown:offset,total:data.total});
  }
  submit("search", () => search(false)); $("more").addEventListener("click", () => run(() => search(true))); $("browse").addEventListener("click", () => run(tree));
  $("backup").addEventListener("click", () => run(() => download("backup")));
  $("publish").addEventListener("click", () => run(() => download("publish")));
  for(const id of ["rights","wip"]) $(id).addEventListener("change",()=>{
    raw("publication-report","");renderTextCoverage(null);
  });
  $("check").addEventListener("click", () => run(async () => {
    if (hasEdits()) throw failure("builder.language.save_first");
    const report=await json("publication-check", publicationOptions());
    label("publication-report","builder.language.publication_report",{included:report.included,wip:report.wip,omitted:report.unchangedSource,fallback:report.fallback});
    renderTextCoverage(report.coverage);
  }));
  async function prepareReview(purpose) {
    reviewPurpose=purpose; phase="review"; $("editor-actions").open=false; feedback("");
    const m=state.project.metadata;
    label("review-pack","builder.language.review_pack",{name:m.name,locale:m.locale,id:m.id,author:m.author});
    raw("publication-report","");
    renderTextCoverage(null);
    if(purpose==="install") {
      installedExists=!!(await json("installation",identity())).installed;
      const r=await json("installation-check",identity());
      label("publication-report","builder.language.install_report",{count:r.messages,fallback:r.fallback});
      renderTextCoverage(r.coverage);
    }
    workflowView(); $("review-title").scrollIntoView({block:"nearest"}); $("review-title").focus();
  }
  async function installCurrent(replace) {
    if (hasEdits()) throw failure("builder.language.save_before_install");
    const data = await json("install", {...identity(),replace});
    installedExists=true;
    label("publication-report","builder.language.installed_report",{count:data.report.messages,path:data.path});
    feedbackKey("builder.language.installed_feedback");
    $("installed-name").textContent = state.project.metadata.name + " · " + state.project.metadata.locale + " · " + state.project.metadata.id;
    label("installed-title",data.enabled?"builder.language.installed_title":"builder.language.updated_disabled");
    $("installed-steps").hidden=!data.enabled;
    if(!data.enabled) feedbackKey("builder.language.disabled_update");
    label("install-state","builder.language.installed_path",{path:data.path});
    phase="installed"; workflowView();
  }
  $("install").addEventListener("click", () => run(async () => {
    if(installedExists&&!$("replace-install").checked) throw failure("builder.language.confirm_replace");
    try { await installCurrent($("replace-install").checked); }
    catch(error) { if(error.status===409) await prepareReview("install"); throw error; }
  }));
  for (const button of panel.querySelectorAll("[data-loc-flow]")) button.addEventListener("click", () => run(async () => {
    if (!discard()) return;
    dirty=detailsDirty=noticeDirty=fontsDirty=false; fontUploads.clear(); workflow=button.dataset.locFlow; phase="choose"; installPath=workflow==="install"?"import":""; importPreview=null; editorTab="messages";
    $("archive-options").open=workflow==="edit";
    $("download").hidden=true; feedback(""); workflowView(); await projects();
  }));
  for(const button of panel.querySelectorAll("[data-loc-install]")) button.addEventListener("click",()=>run(async()=>{
    installPath=button.dataset.locInstall; importPreview=null; feedback(""); workflowView(); await projects();
  }));
  function backToLanguages() { return run(async () => { if (discard()) { dirty=detailsDirty=noticeDirty=fontsDirty=false; fontUploads.clear(); workflow="home"; $("download").hidden=true; feedback(""); workflowView(); await loadCatalog(); window.scrollTo(0,0); $("title").focus({preventScroll:true}); } }); }
  $("home-button").addEventListener("click", backToLanguages);
  $("installed-home").addEventListener("click", backToLanguages);
  $("back-projects").addEventListener("click", backToLanguages);
  for (const purpose of ["install","publish"]) $("review-"+purpose).addEventListener("click", () => run(async () => {
    if (hasEdits()) throw failure("builder.language.save_before_review");
    await prepareReview(purpose);
  }));
  for (const id of ["back-edit","installed-edit"]) $(id).addEventListener("click", () => { phase="edit"; feedback(""); workflowView(); window.scrollTo(0,0); });
  function editorView(name,focus=false) {
    editorTab=name; workflowView();
    // Tab panels have different heights. Reset their scroll position so the
    // sticky toolbar cannot conceal the new panel's first heading/controls.
    panel.scrollIntoView({block:"start"});
    if(focus) $("tab-"+name).focus({preventScroll:true});
  }
  for(const name of ["messages","details","fonts"]) $("tab-"+name).addEventListener("click",()=>editorView(name));
  panel.querySelector(".loc-editor-tabs").addEventListener("keydown",event=>{
    if(["ArrowLeft","ArrowRight","Home","End"].includes(event.key)&&!$("tab-details").hidden) {
      const tabs=["messages","details","fonts"], step=event.key==="ArrowLeft"?-1:1;
      event.preventDefault(); editorView(event.key==="Home"?tabs[0]:event.key==="End"?tabs[2]:tabs[(tabs.indexOf(editorTab)+step+tabs.length)%tabs.length],true);
    }
  });
  document.addEventListener("pointerdown",event=>{ if(!$("editor-actions").contains(event.target)) $("editor-actions").open=false; });
  $("tree-view").addEventListener("change", () => run(async () => { expanded.clear(); await tree(); }));
  window.addEventListener("beforeunload", event => { if (hasEdits()) { event.preventDefault(); event.returnValue = ""; } });
  // The surrounding builder handles tab keyboard navigation. Unsaved text is
  // kept when visiting Build/Assets/Manual and guarded before project changes.
  window.localizationActivate = () => run(async () => {
    const next = await json("state");
    if (!loaded) await adopt(next);
    else { await refreshReference(next); workflowView(); if(workflow==="home") await loadCatalog(); }
  });
  // The shell passes only a project ID. It never reads archives or edits pack
  // state; this same guarded Go-backed workflow handles library navigation.
  function openProject(id,review=false) { return run(async () => {
    if (!discard()) return;
    await adopt(await json("open", {id}));
    workflow="edit"; phase=review?"review":"edit"; reviewPurpose="install"; workflowView();
    editorTab="messages"; workflowView();
    if(review) await prepareReview("install");
    window.scrollTo(0,0);
    feedback("");
  }); }
  window.localizationOpenProject = openProject;
  window.localizationHasEdits = hasEdits;
  document.addEventListener("workshop:closed", () => {
    clearDrop();
    closed=true; dirty=detailsDirty=noticeDirty=fontsDirty=false; fontUploads.clear();
    for(const control of panel.querySelectorAll("button,input,textarea,select")) control.disabled=true;
  });
  workflowView();
})();
