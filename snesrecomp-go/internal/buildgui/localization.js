(() => {
  "use strict";
  const $ = id => document.getElementById("loc-" + id);
  const panel = document.getElementById("panel-localization");
  const labelStatus = {not_started: "Not started", wip: "WIP", done: "Done"};
  let state = {project: null}, selected = "", message = null, dirty = false, detailsDirty = false, noticeDirty = false, busy = false;
  let offset = 0, searchSnapshot = null, loaded = false;
  let workflow = "home", phase = "choose", reviewPurpose = "install";
  let installPath = "", editorTab = "messages", installedExists = false, importPreview = null;
  let catalog = [], saveFailure = "";
  let fontsDirty = false, fontDraft = [], fontUploads = new Map();
  const expanded = new Set();
  const flowTitles = {install:"Install a language pack",create:"Create a translation",edit:"Continue editing",clone:"Clone a language pack",extract:"Extract a ROM reference"};
  function workflowView() {
    const home = workflow === "home", choose = phase === "choose", editing = phase === "edit";
    const importing = choose && (["edit","clone"].includes(workflow) || workflow === "install" && installPath === "import");
    panel.dataset.editing = String(!home && editing);
    $("home").hidden = !home; $("flow-header").hidden = home || editing;
    $("library-section").hidden = !home;
    $("home-prompt").hidden = !home;
    $("flow-title").textContent = editing ? "Edit language pack" : flowTitles[workflow] || "";
    $("flow-steps").textContent = workflow === "install" ? installPath === "existing" ? "Choose an imported pack → Install → Restart and select in game" : "Choose a pack folder → Import & install → Restart and select in game" : workflow === "clone" ? "Choose base → Name independent copy → Edit" : "Choose source or project → Edit and save";
    $("install-state").hidden = phase !== "review";
    $("install-choice").hidden = home || workflow !== "install" || !choose || !!installPath;
    $("project-picker").hidden = home || !choose || !(workflow === "install" && installPath === "existing" || ["edit","clone"].includes(workflow));
    $("project-picker-label").textContent = workflow === "install" ? "Already imported packs" : "Choose a saved project";
    $("open").textContent = workflow === "install" ? "Review for installation" : workflow === "clone" ? "Choose base pack" : "Open for editing";
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
    $("review-title").textContent = reviewPurpose === "install" ? "Review & install this pack" : "Review & export for sharing";
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
    if (phase === "clone") $("clone").elements.name.value = state.project.metadata.name + " — my edition";
    workflowView();
  }
  function feedback(text, error = false) { $("feedback").textContent = text; $("feedback").dataset.error = String(error); }
  function hasEdits() { return dirty || detailsDirty || noticeDirty || fontsDirty; }
  function saveIndicator() {
    const native = state.project?.origin === "native-source";
    $("save-state").textContent = native ? "Read-only reference" : saveFailure ? "Not saved: " + saveFailure : hasEdits() ? "Unsaved changes · save to keep your progress" : "All changes saved in workshop · not automatically installed";
    $("editor-bar").dataset.dirty = String(hasEdits());
    $("save-progress").disabled = busy || !state.project || native || !hasEdits();
    $("save").disabled = busy || !state.project || native || !hasEdits();
    $("save-fonts").disabled = busy || !state.project || native || !hasEdits();
    $("font-check").textContent = hasEdits() ? "Save & check coverage" : "Check coverage with game font backend";
  }
  function discard() { return !hasEdits() || window.confirm("Discard unsaved changes? Saved messages, progress and notes will be kept."); }
  function identity() { return {projectID: state.project?.metadata.id, revision: state.project?.revision}; }
  async function request(endpoint, data, query = {}) {
    const url = new URL("localization/" + endpoint, location.href);
    if (data === undefined) for (const [key, value] of Object.entries(query)) url.searchParams.set(key, value);
    const response = await fetch(url, data === undefined ? {cache: "no-store"} : data instanceof FormData ? {method: "POST", body: data} : {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(data)});
    if (!response.ok) {
      let error; try { error = (await response.json()).error; } catch { error = "Request failed (" + response.status + ")"; }
      const failure = new Error(error || "Request failed"); failure.status = response.status; throw failure;
    }
    return response;
  }
  async function json(endpoint, data, query) { return (await request(endpoint, data, query)).json(); }
  function readonly() {
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
    if (busy) return;
    busy = true;
    const controls = [...panel.querySelectorAll("button,input,textarea,select")].map(el => [el, el.disabled]);
    for (const [el] of controls) el.disabled = true;
    panel.setAttribute("aria-busy", "true");
    try { await action(); } catch (error) { feedback(error.message, true); }
    finally { for (const [el, disabled] of controls) el.disabled = disabled; busy = false; panel.removeAttribute("aria-busy"); readonly(); }
  }
  function option(value, text) { const el = document.createElement("option"); el.value = value; el.textContent = text; return el; }
  function markDirty() { dirty = true; saveFailure=""; $("dirty").textContent = "Unsaved message"; panel.querySelector(".loc-preview").hidden = true; saveIndicator(); }
  function fillMetadata() {
    const p = state.project; if (!p) return;
    for (const [key, value] of Object.entries(p.metadata)) if ($("metadata").elements.namedItem(key)) $("metadata").elements.namedItem(key).value = value;
    $("metadata").elements.notes.value = p.notes;
    $("fonts").textContent = "Fonts: " + [p.fonts.primary, ...(p.fonts.fallback || [])].join(" → ") + ". Use the Fonts tab to change the stack or check coverage.";
    fontDraft = [p.fonts.primary, ...(p.fonts.fallback || [])]; fontUploads = new Map(); fontsDirty = false;
    $("font-report").replaceChildren(); renderFonts();
    $("notices").replaceChildren(option("", "New notice"));
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
      const label=document.createElement("label"); label.textContent=index?"Fallback "+index:"Primary font";
      const select=document.createElement("select"); select.required=true; select.append(option("","Choose a font…"));
      for(const name of known) select.append(option(name,name==="builtin:actraiser-sans"?"Bundled ActRaiser Sans":name));
      select.value=reference; select.addEventListener("change",()=>{fontDraft[index]=select.value; fontChanged();}); label.append(select); row.append(label);
      if(fontUploads.has(reference)){const pending=document.createElement("small");pending.textContent="New file — not saved yet";label.append(pending);}
      const fileLabel=document.createElement("label"); fileLabel.textContent="Or choose a font file";
      const file=document.createElement("input"); file.type="file"; file.accept=".ttf,.otf";
      file.addEventListener("change",()=>{const chosen=file.files[0];if(!chosen)return;if(!chosen.size||chosen.size>64*1024*1024){feedback("Choose a nonempty font up to 64 MiB.",true);file.value="";return;}const path="fonts/"+chosen.name;fontUploads.set(path,chosen);fontDraft[index]=path;fontChanged();}); fileLabel.append(file);row.append(fileLabel);
      const actions=document.createElement("div");actions.className="loc-row";
      for(const [text,offset] of [["Move up",-1],["Move down",1],["Remove",0]]) {
        const button=document.createElement("button");button.type="button";button.textContent=text;button.setAttribute("aria-label",text+" "+(reference||"font "+(index+1)));
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
      const summary=document.createElement("p");summary.textContent=result.complete?"All "+result.scalars+" checked characters are covered.":result.missingCount+" characters are missing. Showing up to 256, with up to four locations each.";
      const list=document.createElement("ul");
      for(const gap of result.missing){const item=document.createElement("li");item.textContent=gap.codepoint+" “"+gap.character+"” — "+gap.locations.map(loc=>loc.messageID?loc.messageID+" · "+loc.source+":"+loc.line:loc.source).join("; ");list.append(item);}
      const values=document.createElement("p");values.className="loc-help";values.textContent=result.dynamicValues.length?"Live values not resolved: "+result.dynamicValues.join(", ")+". Add a sample above to check a player name.":"No unresolved dynamic values.";
      $("font-report").replaceChildren(summary,list,values);
    });
  });
  async function projects() {
    const rows = await json("projects"), old = $("projects").value;
    $("projects").replaceChildren(option("", "Choose a project…"));
    $("reference-project").replaceChildren(option("", state.sourceAvailable ? "Native US (from build / extraction)" : "Native US — not extracted yet"));
    for (const row of rows) {
      const el = option(row.id, row.name + " · " + row.locale + " · " + row.id + (row.error ? " — " + row.error : ""));
      el.disabled = !!row.error; $("projects").append(el);
      if (!row.error && row.id !== state.project?.metadata.id) $("reference-project").append(option(row.id, row.name + " · " + row.locale));
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
    $("library-summary").textContent=catalog.filter(p=>p.installed&&p.enabled).length+" enabled · "+catalog.filter(p=>p.installed&&!p.enabled).length+" disabled · "+catalog.filter(p=>p.project).length+" saved projects";
    $("library-empty").textContent=filter==="installed"&&!catalog.some(p=>p.installed)?"No installed language packs yet. Choose Install a language pack below to get started.":"No matching packages.";
    $("library").replaceChildren(...rows.map(p=>{
      const card=document.createElement("article"); card.className="loc-package-card"; card.dataset.packageId=p.id; card.dataset.packageKey=p.key||"";
      const title=document.createElement("h3"); title.textContent=p.name;
      const badge=document.createElement("span"); badge.className="loc-package-badge"; badge.dataset.installed=String(p.installed); badge.textContent=p.error?"Needs attention":p.installed?"Installed in game":p.readOnly?"Read-only reference":"Workshop only";
      const info=document.createElement("p"); info.className="loc-help"; info.textContent=(p.locale?p.locale+" · ":"")+p.id+(p.installed&&p.installedName!==p.name?" · Installed as “"+p.installedName+"”":"")+(p.installed&&p.key!==p.id?" · Folder: "+p.key:"");
      card.append(badge,title,info);
      if(p.installed){
        const label=document.createElement("label"); label.className="loc-package-enable";
        const checkbox=document.createElement("input"); checkbox.type="checkbox"; checkbox.checked=p.enabled; checkbox.disabled=!!p.error; checkbox.setAttribute("aria-label","Enable "+p.installedName);
        const caption=document.createElement("span");caption.textContent=p.enabled?"Enabled":"Disabled";
        label.append(checkbox,caption);card.prepend(label);
        checkbox.addEventListener("change",()=>{
          if(busy){checkbox.checked=p.enabled;return;}
          const enabled=checkbox.checked;
          run(async()=>{
            try { const result=await json("set-enabled",{id:p.id,directory:p.key,expected:p.installedRevision,enabled});feedback(result.message); }
            finally { await loadCatalog(p.key); }
          });
        });
      }
      const actions=document.createElement("div"); actions.className="loc-row";
      const action=(label,handler)=>{ const b=document.createElement("button"); b.type="button"; b.textContent=label; b.addEventListener("click",handler); actions.append(b); return b; };
      if (p.project) {
        action(p.readOnly?"View reference":"Edit project",()=>openProject(p.id));
        if (!p.readOnly) action(p.installed?"Review & update":"Review & install",()=>openProject(p.id,true));
      }
      if(p.installed&&!p.error) action("Uninstall…",()=>run(async()=>{
        if(!window.confirm("Uninstall “"+p.installedName+"” ("+p.id+", folder "+p.key+") from the game? Your editable project and progress will stay. Installed files are retained for recovery. Restart the game afterward.")) return;
        const result=await json("uninstall",{id:p.id,directory:p.key,expected:p.installedRevision,confirmUninstall:true});
        await loadCatalog(); feedback(result.message+" Recovery manifest: "+result.backup);
      })).className="loc-uninstall";
      if(p.error) { const error=document.createElement("p"); error.className="loc-help"; error.textContent=p.error; card.append(error); }
      else if(!p.project) { const note=document.createElement("p"); note.className="loc-help"; note.textContent="Installed package; no editable project. Import its folder or archive to edit a copy."; card.append(note); }
      card.append(actions); return card;
    }));
    if(typeof focusKey==="string") [...$("library").children].find(row=>row.dataset.packageKey===focusKey)?.querySelector('input[type="checkbox"]')?.focus({preventScroll:true});
  }
  $("library-search").addEventListener("input",renderCatalog);
  $("library-filter").addEventListener("change",renderCatalog);
  $("catalog-refresh").addEventListener("click",()=>run(loadCatalog));
  async function adopt(next, preserveMessage = false) {
    state = next; loaded = true; saveFailure="";
    $("path").textContent = "Workshop & runtime assets: " + next.root + ". Author projects are saved in projects/; installed translations are in packs/.";
    const p = state.project;
    if (!p) { workflowView(); await projects(); return; }
    fillMetadata();
    const totals = p.totals.reduce((a, x) => ({total:a.total+x.total, done:a.done+x.done, wip:a.wip+x.wip}), {total:0,done:0,wip:0});
    $("editor-title").textContent = p.metadata.name;
    $("progress").textContent = totals.done + "/" + totals.total + " done · " + totals.wip + " WIP";
    $("publication-report").textContent = "";
    $("download").hidden = true;
    dirty = false; $("dirty").textContent = "";
    if (!preserveMessage) { selected = ""; message = null; $("rights").checked = false; $("wip").checked = false; $("replace-install").checked = false; $("message-editor").hidden = true; $("message-title").textContent = "Choose a message"; }
    await projects(); await tree();
    const installed = await json("installation", identity());
    installedExists = !!installed.installed;
    $("install-state").textContent = installed.installed ? "An installed version exists. Opening or saving this workshop project does not update it; install again to apply edits." : "Workshop only — this pack is not installed. Opening, editing and exporting do not install it.";
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
        summary.textContent = row.label.replaceAll("_", " ") + " · " + row.done + "/" + row.total;
        details.append(summary, content); li.append(details);
        let fetched = false;
        const expand = async () => {
          if (!fetched) { content.replaceChildren(await treeChildren(row.id)); fetched = true; }
        };
        details.addEventListener("toggle", () => { if (details.open) { expanded.add(row.id); if (!fetched) run(expand); } else expanded.delete(row.id); });
        if (expanded.has(row.id) || selected.startsWith(row.id + ".")) { await expand(); details.open = true; }
      }
      if (row.is_message) li.append(messageButton(row.id, row.label.replaceAll("_", " ") + " · " + (row.done ? "Done" : row.wip ? "WIP" : "Not started")));
      ul.append(li);
    }
    return ul;
  }
  function messageButton(id, text) {
    const button = document.createElement("button"); button.type = "button"; button.textContent = text; button.title = id;
    button.setAttribute("aria-current", String(selected === id));
    button.addEventListener("click", () => run(async () => { if (discard()) { fillMetadata(); await showMessage(id); } }));
    return button;
  }
  function emptyBody(ref) { return [...ref.anchors.map(id => "@anchor " + id), "@empty", "@end", ""].join("\n"); }
  function showShape(p) {
    const descriptions = [];
    if (p?.maximum_pages) descriptions.push("Displays up to " + p.maximum_pages + " page(s).");
    if (p?.maximum_lines) descriptions.push("Up to " + p.maximum_lines + " authored line(s).");
    if (p?.required_nonempty_lines) descriptions.push("Exactly " + p.required_nonempty_lines + " choices, or intentionally empty.");
    if (p?.keyboard) descriptions.push("Every page: name field, dash-only underline slot, then " + p.keyboard.rows + " rows of " + p.keyboard.columns + " grapheme keys. Keep backspace and finish in the final two positions. Up to " + p.keyboard.maximum_lines + " normalized lines and " + p.keyboard.maximum_page_bytes + " UTF-8 bytes per page.");
    for (const rule of p?.table?.rules || []) {
      const rows = rule.last_line === 255 ? "All rows" : rule.first_line === rule.last_line ? "Row " + (rule.first_line + 1) : "Rows " + (rule.first_line + 1) + "–" + (rule.last_line + 1);
      descriptions.push(rows + ": " + (rule.native_reserved ? "native artwork is preserved." : rule.fields.join(", ") + " field(s), separated by |. Keep internal blank rows."));
    }
    $("shape").hidden = !descriptions.length;
    $("shape-rules").replaceChildren(...descriptions.map(text => { const item = document.createElement("li"); item.textContent = text; return item; }));
  }
  async function showMessage(id) {
    const data = await json("message", undefined, {...identity(), id});
    selected = id; message = data.message;
    showShape(message.reference.presentation);
    $("message-title").textContent = data.location?.title || id;
    $("message-context").textContent = id + " · " + (data.location?.context || data.location?.group_label || "") + "\n" + (message.present ? "Saved in " + message.path : "Not included yet — native US fallback is used until you supply a translation.") + (message.reference.native_in_profile ? "" : " This route is absent in the source release; it is included for cross-region reference.");
    renderReference(data);
    $("anchors").textContent = message.reference.anchors.length ? "Required native controls (in order): " + message.reference.anchors.join(" → ") : "No native control anchors required. You may shorten, extend or intentionally clear this text.";
    $("body").value = message.present ? message.body : emptyBody(message.reference);
    $("message-status").value = message.status;
    $("placeholders").replaceChildren(option("", "Choose a supported value…"));
    for (const p of message.reference.placeholders) $("placeholders").append(option(p.name, p.name.replaceAll("_", " ") + " (" + p.kind.replaceAll("_", " ") + ")"));
    $("message-editor").hidden = false; panel.querySelector(".loc-preview").hidden = true;
    dirty = false; $("dirty").textContent = "";
    for (const b of panel.querySelectorAll("#loc-tree button,#loc-results button")) b.setAttribute("aria-current", String(b.title === id));
    readonly();
  }
  function renderReference(data) {
    const m = data.referenceMetadata;
    $("reference-title").textContent = m ? m.name + " · " + m.locale + (state.reference && state.reference.id !== m.id ? " (fallback)" : "") : "Source reference";
    $("reference-body").textContent = data.reference?.body || "No matching local reference is available. Use Extract reference ROM above to add one without leaving your translation.";
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
    try { await refreshReference(await json("reference", {...identity(), id:$("reference-project").value})); feedback("Reference changed. Your unsaved work is unchanged."); }
    finally { const id=state.reference?.id; $("reference-project").value=[...$("reference-project").options].some(o=>o.value===id)?id:""; }
  }));
  function insert(text, command = false) {
    const input = $("body"), at = input.selectionStart;
    if (command) text = (at && input.value[at-1] !== "\n" ? "\n" : "") + text + "\n";
    input.setRangeText(text, input.selectionStart, input.selectionEnd, "end"); input.focus(); markDirty();
  }
  function publicationOptions() { return {...identity(), confirmRights: $("rights").checked, includeWIP: $("wip").checked}; }
  function report(r) { return r.included + " translations included (" + r.wip + " WIP). " + r.unchangedSource + " unchanged source messages omitted. " + r.fallback + " US routes use native fallback."; }
  async function download(endpoint) {
    if (hasEdits()) throw new Error("Save your changes before exporting.");
    const data = await json(endpoint, {...publicationOptions(), prepareDownload:true});
    const a = $("download");
    a.href = data.url; a.download = data.name; a.textContent = "Download " + data.name; a.hidden = false;
    $("editor-actions").open = false;
    a.scrollIntoView({block:"center"}); a.focus();
    feedback(endpoint === "backup" ? "Private backup ready. Use the download link below; keep ROM-derived backups private." : "Language pack ready with its credits and notices. Use the download link below.");
  }
  function submit(id, action) { $(id).addEventListener("submit", event => { event.preventDefault(); const form = event.currentTarget, data = new FormData(form); run(() => action(data, form)); }); }
  $("open").addEventListener("click", () => run(async () => { if (discard()) { await adopt(await json("open", {id: $("projects").value})); projectDestination(); if(phase==="review") await prepareReview("install"); feedback("Pack opened. Nothing has been installed yet."); } }));
  $("refresh").addEventListener("click", () => run(projects));
  submit("extract-reference", async data => {
    data.set("intent", "reference");
    for (const [key, value] of Object.entries(identity())) data.set(key, value);
    await refreshReference(await json("extract", data));
    referenceForm(false); $("extract-reference").reset();
    feedback("Reference extracted and selected. Your project, unsaved text and progress are unchanged.");
  });
  submit("extract", async (data) => { if (discard()) { await adopt(await json("extract", data)); if (workflow === "create") { phase="choose"; workflowView(); } else projectDestination(); feedback("Source extracted and validated. Create a translation to edit it; regional sources stay local-only references."); } });
  submit("create", async data => { if (discard()) { await adopt(await json("create", {metadata: {...Object.fromEntries(data), direction:"auto"}})); phase="edit"; workflowView(); feedback("Translation created in the workshop. Save your progress, then use Pack actions → Install in game when ready to test."); } });
  submit("clone", async data => { if (discard()) { await adopt(await json("clone", {...identity(),newID:data.get("newID"),metadata:{name:data.get("name")}})); phase="edit"; workflowView(); feedback("Independent copy created. Original text, progress and credits are retained; nothing has been installed."); } });
  function syncImportPreview() {
    if (!importPreview) { $("accept-import").disabled=true; return; }
    const copying=!!$("import-new-id").value.trim(), installing=workflow==="install";
    $("project-conflict").hidden=!importPreview.existingProject;
    $("replace-project").closest("label").hidden=copying;
    $("import-install-conflict").hidden=!installing||!importPreview.installed||copying;
    $("accept-import").textContent=installing?"Import & install":workflow==="clone"?"Import as clone base":"Import for editing";
    $("accept-import").disabled=busy || installing&&!!importPreview.installError || !!importPreview.existingProject&&!copying&&!$("replace-project").checked || installing&&!!importPreview.installed&&!copying&&!$("import-replace-installed").checked;
  }
  async function previewImport(endpoint, data) {
    importPreview=null; workflowView(); feedback("Loading and validating pack…");
    importPreview=await json(endpoint,data);
    $("replace-project").checked=$("import-replace-installed").checked=false;
    $("import-new-id").value="";
    const p=importPreview, m=p.metadata;
    $("import-name").textContent=m.name;
    $("import-summary").textContent=m.locale+" · "+m.id+" · "+p.messages+" messages · By "+m.author+" · "+m.license;
    $("project-conflict-note").textContent=p.existingProject?"A saved project with ID “"+m.id+"” already exists: “"+p.existingProject.name+"”. Keep it by choosing a new ID, or explicitly replace it below.":"";
    $("import-warning").textContent=p.installError||"All supplied messages, fonts and credits are kept. Nothing has been installed yet.";
    workflowView(); feedback("");
    $("import-preview").scrollIntoView({block:"nearest"});
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
  $("import").addEventListener("submit",event=>event.preventDefault());
  $("import").elements.file.addEventListener("change",event=>{
    const file=event.target.files[0]; if(!file) return;
    const data=new FormData(); data.set("file",file); data.set("intent","preview");
    run(async()=>{ if(discard()) await previewImport("import",data); });
  });
  for(const id of ["replace-project","import-new-id","import-replace-installed"]) $(id).addEventListener("input",syncImportPreview);
  $("accept-import").addEventListener("click",()=>run(async()=>{
    if(!importPreview) return;
    const incoming=importPreview, newID=$("import-new-id").value.trim();
    const replaceInstall=!newID&&$("import-replace-installed").checked;
    if(incoming.existingProject&&!newID&&!$("replace-project").checked) throw new Error("Choose whether to replace the same-ID project or keep both.");
    if(workflow==="install"&&incoming.installed&&!newID&&!replaceInstall) throw new Error("Confirm replacement of the installed pack with this ID.");
    await adopt(await json("accept-import",{importToken:incoming.token,newID,replace:!newID&&$("replace-project").checked,expected:incoming.existingProject?.revision||""}));
    importPreview=null; projectDestination();
    if(workflow==="install") {
      try { await installCurrent(replaceInstall); }
      catch(error) { await prepareReview("install"); throw new Error("Imported into the workshop, but not installed: "+error.message); }
    } else feedback("Imported for editing. The original folder and game installation are unchanged.");
  }));
  $("metadata").addEventListener("input", () => { detailsDirty = true; saveFailure=""; saveIndicator(); });
  $("notice").addEventListener("input", () => { noticeDirty = true; saveFailure=""; saveIndicator(); });
  $("notices").addEventListener("change", () => {
    if (noticeDirty && !window.confirm("Discard unsaved notice changes?")) { $("notices").value = ""; return; }
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
      catch (error) { saveFailure=error.message; throw error; }
      await adopt(next, true);
      feedback("Progress saved: message text, translation status, package details and notices. The installed game copy is unchanged.");
    });
  }
  for (const id of ["save", "save-progress"]) $(id).addEventListener("click", saveProgress);
  for (const id of ["metadata", "notice", "font-stack"]) $(id).addEventListener("submit", event => { event.preventDefault(); saveProgress(); });
  document.addEventListener("keydown", event => {
    if (!panel.hidden && phase === "edit" && workflow !== "home" && (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") { event.preventDefault(); saveProgress(); }
  });
  $("insert-value").addEventListener("click", () => { if ($("placeholders").value) insert("{" + $("placeholders").value + "}"); });
  for (const el of panel.querySelectorAll("[data-loc-insert]")) el.addEventListener("click", () => insert(el.dataset.locInsert, true));
  $("independent").addEventListener("click", () => run(async () => { if (dirty && !window.confirm("Replace this unsaved draft with the resolved alias text?")) return; $("body").value = (await json("materialize", {...identity(), id:selected})).body; markDirty(); }));
  $("empty").addEventListener("click", () => { if (window.confirm("Clear this message's text and authored pages, keeping required native controls? This is not saved until you click Save message.")) { $("body").value = emptyBody(message.reference); markDirty(); } });
  $("preview").addEventListener("click", () => run(async () => {
    const ops = await json("preview", {...identity(), id:selected, body:$("body").value, status:$("message-status").value});
    const container = $("preview-content"); container.replaceChildren();
    let page, number = 0;
    const newPage = () => { const heading = document.createElement("h4"); heading.textContent = "Page " + (++number); page = document.createElement("div"); page.className = "loc-preview-page"; container.append(heading,page); };
    newPage();
    for (const op of ops) {
      if (op.op === "page") newPage();
      else if (op.op === "text") page.append(document.createTextNode(op.value));
      else if (op.op === "line" || op.op === "paragraph") page.append(document.createTextNode(op.op === "line" ? "\n" : "\n\n"));
      else if (op.op === "placeholder") { const value = document.createElement("span"); value.textContent = "⟦" + op.name + (op.minimum_digits ? ":0" + op.minimum_digits : "") + "⟧"; value.title = "Runtime value; length varies in game"; page.append(value); }
      else { const control = document.createElement("div"); control.className = "loc-preview-control"; control.textContent = op.op + (op.id ? ": " + op.id : op.frames ? ": " + op.frames + " frames" : ""); page.append(control); }
    }
    panel.querySelector(".loc-preview").hidden = false; feedback("Script and native control contracts are valid. Preview does not change the saved project.");
  }));
  async function search(more) {
    if (!more) { offset = 0; searchSnapshot = {q:$("search").elements.q.value, status:$("search").elements.status.value, ...identity()}; $("results").replaceChildren(); }
    const data = await json("search", undefined, {...searchSnapshot, offset});
    $("tree").hidden = true; $("results").hidden = false;
    for (const row of data.rows) $("results").append(messageButton(row.id, (row.title || row.id) + " · " + labelStatus[row.status] + (row.present ? "" : " · Native fallback")));
    offset += data.rows.length; $("more").hidden = offset >= data.total;
    if (!data.total) $("results").textContent = "No matching messages.";
    feedback(offset + " of " + data.total + " results shown.");
  }
  submit("search", () => search(false)); $("more").addEventListener("click", () => run(() => search(true))); $("browse").addEventListener("click", () => run(tree));
  $("backup").addEventListener("click", () => run(() => download("backup")));
  $("publish").addEventListener("click", () => run(() => download("publish")));
  $("check").addEventListener("click", () => run(async () => { if (hasEdits()) throw new Error("Save your changes first."); $("publication-report").textContent = report(await json("publication-check", publicationOptions())); }));
  async function prepareReview(purpose) {
    reviewPurpose=purpose; phase="review"; $("editor-actions").open=false; feedback("");
    const m=state.project.metadata;
    $("review-pack").textContent=m.name+" · "+m.locale+" · "+m.id+" · By "+m.author;
    $("publication-report").textContent="";
    if(purpose==="install") {
      installedExists=!!(await json("installation",identity())).installed;
      const r=await json("installation-check",identity());
      $("publication-report").textContent=r.messages+" supplied messages will be installed. "+r.fallback+" US routes use native fallback.";
    }
    workflowView(); $("review-title").scrollIntoView({block:"nearest"}); $("review-title").focus();
  }
  async function installCurrent(replace) {
    if (hasEdits()) throw new Error("Save your changes before installing.");
    const data = await json("install", {...identity(),replace});
    installedExists=true;
    $("publication-report").textContent = data.report.messages+" messages installed at "+data.path;
    feedback("Pack installed. Select it by package name in the game's Localization menu after restarting the game.");
    $("installed-name").textContent = state.project.metadata.name + " · " + state.project.metadata.locale + " · " + state.project.metadata.id;
    $("installed-title").textContent=data.enabled?"Installed — now select it in game":"Updated — this pack is currently disabled";
    $("installed-steps").hidden=!data.enabled;
    if(!data.enabled) feedback("Updated the installed pack, preserving its Disabled setting. Enable it in the package checklist when you want to use it.");
    $("install-state").textContent = "Installed at " + data.path + ". Restart the game and select this pack; enhanced text rendering turns on automatically for community packs.";
    phase="installed"; workflowView();
  }
  $("install").addEventListener("click", () => run(async () => {
    if(installedExists&&!$("replace-install").checked) throw new Error("Confirm replacement of the installed pack with this ID.");
    try { await installCurrent($("replace-install").checked); }
    catch(error) { if(error.status===409) await prepareReview("install"); throw error; }
  }));
  for (const button of panel.querySelectorAll("[data-loc-flow]")) button.addEventListener("click", () => run(async () => {
    if (!discard()) return;
    dirty=detailsDirty=noticeDirty=fontsDirty=false; fontUploads.clear(); workflow=button.dataset.locFlow; phase="choose"; installPath=""; importPreview=null; editorTab="messages";
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
    if (hasEdits()) throw new Error("Save your edits before reviewing the pack.");
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
    else { await refreshReference(next); workflowView(); }
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
  document.addEventListener("workshop:closed", () => { dirty=detailsDirty=noticeDirty=fontsDirty=false; fontUploads.clear(); });
  workflowView();
})();
