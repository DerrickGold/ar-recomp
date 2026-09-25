(() => {
  "use strict";
  const fetchResponse = (...args) => window.workshopFeedback?.request ?
    window.workshopFeedback.request(...args) : fetch(...args);
  const ui = window.workshopI18n;
  const $ = id => document.getElementById("loc-" + id);
  const label = (id, key, args = {}) => ui.set($(id), key, args);
  function raw(id, text) {
    ui.unbind($(id));
    $(id).textContent = text;
  }
  function keyedOption(value, key, args = {}) {
    const el = option(value, "");
    ui.set(el, key, args);
    return el;
  }
  function phrase(key, args = {}, tag = "span") {
    const el = document.createElement(tag);
    ui.set(el, key, args);
    return el;
  }
  // Only the authoritative Go navigation model supplies caption IDs. Source
  // excerpts and technical message IDs have no caption key and remain literal.
  function navigationCaption(key, text) {
    const el = document.createElement("span");
    el.textContent = text;
    if (key)
      ui.set(el, "builder.navigation." + key);
    else
      contentLanguage(el);
    return el;
  }
  // Content direction is never the workshop UI locale. Keep UTF-8 untouched;
  // dir/lang affect presentation only, not saved scripts or selection offsets.
  function contentLanguage(el, metadata = {}) {
    el.setAttribute(
      "dir", ["ltr", "rtl"].includes(metadata?.direction) ? metadata.direction : "auto");
    el.setAttribute("lang", metadata?.locale || "");
  }
  function draftLanguage() {
    const fields = $("metadata").elements;
    return {locale: fields.locale.value, direction: fields.direction.value};
  }
  function updateContentLanguage() {
    const metadata = draftLanguage();
    contentLanguage($("body"), metadata);
    for (const page of $("preview-content").querySelectorAll(".loc-preview-page"))
      contentLanguage(page, metadata);
  }
  function failure(key, args = {}) {
    const error = new Error(ui.text(key, args));
    error.code = key;
    error.uiKey = key;
    error.uiArgs = args;
    return error;
  }
  function feedbackKey(key, args = {}, error = false) {
    label("feedback", key, args);
    $("feedback").dataset.error = String(error);
    if (!error)
      window.workshopFeedback?.clear($("feedback"));
  }
  const panel = document.getElementById("panel-localization");
  function previewMetadata() {
    // Preview can start while run() temporarily disables the form. Read named
    // controls directly; FormData would omit them along with their direction.
    const form = $("metadata");
    return Object.fromEntries(["name", "locale", "autonym", "author", "license", "direction"]
      .map(name => [name, form.querySelector(`[name="${name}"]`).value]));
  }
  const playback = window.workshopPlayback?.create($("playback"), () => ({
    ...identity(), id: draft.selected, body: $("body").value,
    status: $("message-status").value, saveFonts: fonts.dirty,
    fonts: fonts.fonts(), fontUploads: fonts.uploads(),
    saveDetails: draft.detailsDirty, metadata: previewMetadata()
  }), message => feedback(message, true), () => {
    $("body").focus();
    $("body").scrollIntoView({block: "center"});
  });
  const statusKeys = {
    not_started: "builder.editor.not_started",
    wip: "builder.editor.wip",
    done: "builder.editor.done"
  };
  const valueKindKeys = {
    number: "builder.editor.value_number",
    localized_term: "builder.editor.value_term",
    localized_text: "builder.editor.value_text",
    icon: "builder.editor.value_icon"
  };
  const coverageSurfaceKeys = {
    dialogue: "builder.coverage.surface.dialogue",
    menus: "builder.coverage.surface.menus",
    keyboard: "builder.coverage.surface.keyboard",
    terms: "builder.coverage.surface.terms",
    hud: "builder.coverage.surface.hud",
    credits: "builder.coverage.surface.credits"
  };
  // The server snapshot is replaced atomically after a successful request.
  let state = {project: null};
  let busy = false, loaded = false, closed = false;
  let offset = 0, searchSnapshot = null, installedExists = false;
  const flow = {
    kind: "home",
    phase: "choose",
    reviewPurpose: "install",
    installPath: "",
    editorTab: "messages",
  };
  const draft = {
    selected: "",
    message: null,
    messageDirty: false,
    detailsDirty: false,
    noticeDirty: false,
    saveFailure: "",
  };
  const fonts = window.workshopLanguageFonts.create({
    $, ui, phrase, option, keyedOption, feedbackKey,
    onChange() {
      playback?.invalidate();
      draft.saveFailure = "";
      saveIndicator();
    },
    onRender() {
      populateStyleChoices();
      readonly();
    },
  });
  const library = window.workshopLanguageLibrary.create({
    $, ui, label, contentLanguage, run, json, feedbackKey, openProject,
    isBusy: () => busy, isClosed: () => closed,
  });
  const imports = window.workshopLanguageImport.create({
    $, label, raw, feedback, feedbackKey, failure, json, run, submit, discard,
    kind: () => flow.kind, isBusy: () => busy, isClosed: () => closed,
    onPreviewChanged: workflowView,
    beginInstall: beginImportInstallation,
    onAccepted: adoptImportedProject,
  });
  const expanded = new Set();
  const flowTitles = {
    install: "builder.language.flow_install",
    create: "builder.language.flow_create",
    edit: "builder.language.flow_edit",
    clone: "builder.language.flow_clone",
    extract: "builder.language.flow_extract"
  };
  function workflowView() {
    const home = flow.kind === "home", choose = flow.phase === "choose",
          editing = flow.phase === "edit";
    const importing = choose &&
      (["edit", "clone"].includes(flow.kind) ||
        flow.kind === "install" && flow.installPath === "import");
    panel.dataset.editing = String(!home && editing);
    $("home").hidden = !home;
    $("flow-header").hidden = home || editing;
    $("library-section").hidden = !home;
    $("home-prompt").hidden = !home;
    if (editing || flowTitles[flow.kind])
      label("flow-title", editing ? "builder.language.edit_pack" : flowTitles[flow.kind]);
    else
      raw("flow-title", "");
    label("flow-steps",
      flow.kind === "install" ? flow.installPath === "existing" ?
                                "builder.language.steps_existing" :
                                "builder.language.steps_import" :
        flow.kind === "clone" ? "builder.language.steps_clone" :
                                "builder.language.steps_edit");
    $("install-state").hidden = flow.phase !== "review";
    $("install-choice").hidden = home || flow.kind !== "install" || !choose;
    for (const button of panel.querySelectorAll("[data-loc-install]"))
      button.setAttribute("aria-pressed", String(button.dataset.locInstall === flow.installPath));
    $("project-picker").hidden = home || !choose ||
      !(flow.kind === "install" && flow.installPath === "existing" ||
        ["edit", "clone"].includes(flow.kind));
    label("project-picker-label",
      flow.kind === "install" ? "builder.language.imported_packs" :
                                "builder.language.choose_saved");
    label("open",
      flow.kind === "install" ? "builder.language.review_install" :
        flow.kind === "clone" ? "builder.language.choose_base" :
                                "builder.language.open_edit");
    $("start").hidden =
      home || !choose || !(importing || ["create", "extract"].includes(flow.kind));
    $("extract").hidden =
      flow.kind !== "extract" && !(flow.kind === "create" && !state.sourceAvailable);
    $("create").hidden = flow.kind !== "create" || !state.sourceAvailable;
    $("import-source").hidden = !importing;
    $("import-preview").hidden = !importing || !imports.preview;
    $("clone").hidden = home || flow.phase !== "clone";
    $("workspace").hidden =
      home || !state.project || !["edit", "review", "installed"].includes(flow.phase);
    $("upgrade-v2").hidden = !editing || state.project?.formatVersion !== 1;
    $("editor-actions").hidden = !editing || state.project?.origin === "native-source";
    $("details").hidden = !editing || flow.editorTab !== "details";
    $("editing").hidden = !editing || flow.editorTab !== "messages";
    $("font-panel").hidden = !editing || flow.editorTab !== "fonts";
    $("tab-details").hidden = state.project?.origin === "native-source";
    $("tab-fonts").hidden = state.project?.origin === "native-source";
    for (const name of ["messages", "details", "fonts"]) {
      $("tab-" + name).setAttribute("aria-selected", String(flow.editorTab === name));
      $("tab-" + name).tabIndex = flow.editorTab === name ? 0 : -1;
    }
    $("editor-bar").hidden = !editing;
    $("reference-tools").hidden = !editing;
    $("sharing").hidden = flow.phase !== "review";
    $("installed").hidden = flow.phase !== "installed";
    label("review-title",
      flow.reviewPurpose === "install" ? "builder.language.review_install_title" :
                                         "builder.language.review_export_title");
    $("install").hidden = flow.reviewPurpose !== "install";
    $("publish").hidden = flow.reviewPurpose !== "publish";
    $("check").hidden = flow.reviewPurpose !== "publish";
    $("publication-options").hidden = flow.reviewPurpose !== "publish";
    $("install-copy").hidden = flow.reviewPurpose !== "install";
    $("install-upgrade").hidden = flow.reviewPurpose !== "install" || state.project?.formatVersion !== 1;
    $("replace-install").closest("label").hidden =
      flow.reviewPurpose !== "install" || !installedExists;
    imports.render();
    saveIndicator();
  }
  function projectDestination() {
    flow.phase = flow.kind === "install" ? "review" : flow.kind === "clone" ? "clone" : "edit";
    flow.reviewPurpose = "install";
    if (flow.phase === "clone")
      $("clone").elements.name.value =
        ui.text("builder.language.clone_default", {name: state.project.metadata.name});
    workflowView();
  }
  function renderTextCoverage(report) {
    const host = $("text-coverage");
    host.replaceChildren();
    host.hidden = !report;
    if (!report)
      return;
    host.append(phrase("builder.coverage.contract",
      {provided: report.required.provided, total: report.required.total}, "p"));
    if (report.runtime)
      host.append(phrase("builder.coverage.live_optional",
        {provided: report.liveOptional.provided, total: report.liveOptional.total}, "p"));
    host.append(phrase("builder.coverage.explanation", {}, "p"));
    for (const surface of report.surfaces) {
      const details = document.createElement("details"),
            summary = document.createElement("summary");
      summary.append(phrase(coverageSurfaceKeys[surface.surface]), document.createTextNode(" · "),
        phrase("builder.coverage.supplied", {provided: surface.provided, total: surface.total}));
      details.append(summary,
        phrase("builder.coverage.review", {
          done: surface.done,
          wip: surface.wip,
          unreviewed: surface.notStarted,
          unchanged: surface.unchangedSource
        },
          "p"));
      if (surface.missing?.length) {
        details.append(phrase("builder.coverage.fallback_ids", {}, "p"));
        const ids = document.createElement("pre");
        ids.setAttribute("dir", "ltr");
        ids.textContent = surface.missing.join("\n");
        details.append(ids);
      }
      host.append(details);
    }
    if (report.dormant?.length)
      host.append(phrase("builder.coverage.dormant", {ids: report.dormant.join(", ")}, "p"));
  }
  function feedback(text, error = false) {
    raw("feedback", text);
    $("feedback").dataset.error = String(error);
    if (!error)
      window.workshopFeedback?.clear($("feedback"));
  }
  function clearDraftEdits() {
    draft.messageDirty = false;
    draft.detailsDirty = false;
    draft.noticeDirty = false;
    fonts.clearDirty();
  }
  function hasEdits() {
    return draft.messageDirty || draft.detailsDirty || draft.noticeDirty || fonts.dirty;
  }
  function saveIndicator() {
    const native = state.project?.origin === "native-source";
    label("save-state",
      native              ? "builder.language.read_only" :
        draft.saveFailure ? "builder.language.not_saved" :
        hasEdits()        ? "builder.language.unsaved" :
                            "builder.language.saved_workshop",
      {detail: draft.saveFailure});
    $("editor-bar").dataset.dirty = String(hasEdits());
    $("save-progress").disabled = busy || !state.project || native || !hasEdits();
    $("save").disabled = busy || !state.project || native || !hasEdits();
    $("save-fonts").disabled = busy || !state.project || native || !hasEdits();
    label("font-check", hasEdits() ? "builder.editor.save_check" : "builder.editor.check_coverage");
  }
  function discard() {
    return !hasEdits() || window.confirm(ui.text("builder.language.discard_confirm"));
  }
  function identity() {
    return {projectID: state.project?.metadata.id, revision: state.project?.revision};
  }
  const actionNames = {
    state: "Read language workspace",
    projects: "List language projects",
    catalog: "List installed language packs",
    open: "Open language project",
    import: "Inspect language pack",
    directory: "Inspect language pack folder",
    "accept-import": "Import language project",
    "choose-directory": "Choose language pack folder",
    install: "Install language pack",
    "installation-check": "Check language pack for installation",
    installation: "Read language pack installation",
    publish: "Export language pack",
    "publication-check": "Check language pack for export",
    "font-coverage": "Check language pack fonts",
    save: "Save language project",
    backup: "Back up language project",
    uninstall: "Uninstall language pack",
    "set-enabled": "Change language pack availability",
    preview: "Validate translated message"
  };
  async function readResponse(response) {
    if (window.workshopFeedback?.readJSON)
      return window.workshopFeedback.readJSON(response);
    const body = await response.json();
    if (!response.ok) {
      const error = failure(body.errorCode || "builder.language.request_failed", {detail: body.error});
      error.status = response.status;
      error.detail = body.error;
      error.recoveryKey = body.recoveryKey;
      throw error;
    }
    return body;
  }
  async function request(endpoint, data, query = {}) {
    try {
      if (closed)
        throw failure("builder.closed");
      const url = new URL("localization/" + endpoint, location.href);
      if (data === undefined)
        for (const [key, value] of Object.entries(query))
          url.searchParams.set(key, value);
      const response = await fetchResponse(url,
        data === undefined ? {cache: "no-store"} :
        data instanceof FormData ? {method: "POST", body: data} : {
          method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(data)
        });
      if (closed)
        throw failure("builder.closed");
      if (!response.ok)
        await readResponse(response);
      return response;
    } catch (error) {
      error.operation = actionNames[endpoint] || "Language editor: " + endpoint;
      throw error;
    }
  }
  async function json(endpoint, data, query) {
    try {
      const result = await readResponse(await request(endpoint, data, query));
      if (closed)
        throw failure("builder.closed");
      return result;
    } catch (error) {
      error.operation = actionNames[endpoint] || "Language editor: " + endpoint;
      throw error;
    }
  }
  function reportFailure(error) {
    feedbackKey(error.uiKey || "builder.language.request_failed",
      error.uiArgs || {detail: error.message}, true);
    window.workshopFeedback?.show($("feedback"), error, {operation: "Edit language project"});
  }
  function withOutcome(error, key, outcome) {
    // Keep the underlying code, action and raw detail when adding a partial
    // success message. Recovery must not repeat the operation that just failed.
    error.detail ||= error.uiArgs?.detail || error.message;
    error.uiArgs = {detail: error.message};
    error.uiKey = key;
    error.message = ui.text(key, error.uiArgs);
    error.outcome = outcome;
    return error;
  }
  function readonly() {
    if (closed)
      return;
    const native = !state.project || state.project.origin === "native-source";
    for (const el of panel.querySelectorAll(
           "#loc-message-editor button:not([data-playback]), #loc-body, #loc-message-status, #loc-placeholders"))
      el.disabled = native;
    $("preview").disabled = !draft.message;
    $("independent").disabled = native || !draft.message?.body.trimStart().startsWith("@alias ");
    for (const id of ["install", "publish", "check", "review-install", "review-publish"])
      $(id).disabled = native;
    for (const el of $("font-stack").querySelectorAll("input,select,button"))
      el.disabled = native || busy || el.dataset.unavailable === "true";
    $("font-add").disabled = native || busy || fonts.stackLength >= 9;
    saveIndicator();
    imports.render();
    updateInlineStyleAvailability();
  }
  async function run(action) {
    if (busy || closed)
      return;
    busy = true;
    window.workshopFeedback?.clear($("feedback"));
    const controls =
      [...panel.querySelectorAll("button,input,textarea,select")]
        .filter(el => !el.closest("#loc-playback")).map(el => [el, el.disabled]);
    for (const [el] of controls)
      el.disabled = true;
    panel.setAttribute("aria-busy", "true");
    try {
      await action();
    } catch (error) {
      if (!closed) {
        reportFailure(error);
      }
    } finally {
      for (const [el, disabled] of controls)
        el.disabled = closed || disabled;
      busy = false;
      panel.removeAttribute("aria-busy");
      readonly();
    }
  }
  function option(value, text) {
    const el = document.createElement("option");
    el.value = value;
    el.textContent = text;
    return el;
  }
  function markDirty() {
    playback?.invalidate();
    draft.messageDirty = true;
    draft.saveFailure = "";
    label("dirty", "builder.editor.unsaved_message");
    panel.querySelector(".loc-preview").hidden = true;
    updateInlineStyleAvailability();
    saveIndicator();
  }
  function fillMetadata() {
    const p = state.project;
    if (!p)
      return;
    for (const [key, value] of Object.entries(p.metadata))
      if ($("metadata").elements.namedItem(key))
        $("metadata").elements.namedItem(key).value = value;
    $("metadata").elements.notes.value = p.notes;
    updateContentLanguage();
    label("fonts", "builder.editor.font_summary",
      {fonts: [p.fonts.primary, ...(p.fonts.fallback || [])].join(" → ")});
    fonts.load(p.fonts);
    $("notices").replaceChildren(keyedOption("", "builder.editor.new_notice"));
    for (const name of Object.keys(p.notices).sort())
      $("notices").append(option(name, name));
    $("notice").reset();
    draft.detailsDirty = draft.noticeDirty = false;
  }
  $("font-check").addEventListener("click", async () => {
    if (hasEdits())
      await saveProgress();
    if (hasEdits() || busy)
      return;
    return run(async () => {
      const sample = $("font-sample").value;
      const result = await json("font-coverage", {...identity(), samples: sample ? [sample] : []});
      fonts.showCoverage(result);
    });
  });
  async function projects() {
    const rows = await json("projects"), old = $("projects").value;
    $("projects").replaceChildren(keyedOption("", "builder.language.choose_project"));
    $("reference-project")
      .replaceChildren(keyedOption("",
        state.sourceAvailable ? "builder.editor.native_reference" :
                                "builder.editor.native_unavailable"));
    for (const row of rows) {
      const el = option(row.id,
        row.name + " · " + row.locale + " · " + row.id + (row.error ? " — " + row.error : ""));
      contentLanguage(el);
      el.disabled = !!row.error;
      $("projects").append(el);
      if (!row.error && row.id !== state.project?.metadata.id) {
        const ref = option(row.id, row.name + " · " + row.locale);
        contentLanguage(ref);
        $("reference-project").append(ref);
      }
    }
    $("projects").value = state.project?.metadata.id || old;
    // A built US source need not have an author-store copy yet.
    $("reference-project").value =
      [...$("reference-project").options].some(o => o.value === state.reference?.id) ?
      state.reference.id :
      "";
    if (flow.kind === "home")
      await library.refresh();
  }
  async function adopt(next, preserveMessage = false) {
    playback?.invalidate();
    state = next;
    loaded = true;
    draft.saveFailure = "";
    if (state.project?.origin === "native-source")
      flow.editorTab = "messages";
    label("path", "builder.language.paths", {root: next.root});
    const p = state.project;
    if (!p) {
      workflowView();
      await projects();
      return;
    }
    fillMetadata();
    $("upgrade-v2").elements.newID.value = (p.metadata.id + "-v2").slice(0, 96);
    const totals = p.totals.reduce(
      (a, x) => ({total: a.total + x.total, done: a.done + x.done, wip: a.wip + x.wip}),
      {total: 0, done: 0, wip: 0});
    $("editor-title").textContent = p.metadata.name;
    contentLanguage($("editor-title"), {locale: p.metadata.locale});
    label("progress", "builder.language.progress", totals);
    raw("publication-report", "");
    renderTextCoverage(null);
    $("download").hidden = true;
    draft.messageDirty = false;
    raw("dirty", "");
    if (!preserveMessage) {
      draft.selected = "";
      draft.message = null;
      $("rights").checked = false;
      $("wip").checked = false;
      $("replace-install").checked = false;
      $("message-editor").hidden = true;
      label("message-title", "builder.editor.choose_message");
      raw("message-context", "");
      label("message-context", "builder.editor.choose_message_help");
    }
    await projects();
    await tree();
    const installed = next.installationUpdate || await json("installation", identity());
    installedExists = !!installed.installed;
    label("install-state",
      installed.installed ? "builder.language.existing_install" : "builder.language.not_installed");
    if (preserveMessage && draft.selected)
      await showMessage(draft.selected);
    readonly();
    workflowView();
  }
  async function tree() {
    $("tree").hidden = false;
    $("results").hidden = true;
    $("more").hidden = true;
    $("tree").replaceChildren(await treeChildren(""));
  }
  async function treeChildren(parent) {
    const rows = await json("tree", undefined, {...identity(), parent, view: $("tree-view").value});
    const ul = document.createElement("ul");
    for (const row of rows) {
      const li = document.createElement("li");
      if (row.has_children) {
        const details = document.createElement("details"),
              summary = document.createElement("summary"), content = document.createElement("div");
        summary.append(navigationCaption(row.label_key, row.label), document.createTextNode(" · "),
          phrase("builder.editor.tree_count", {done: row.done, total: row.total}));
        details.append(summary, content);
        li.append(details);
        let fetched = false;
        const expand = async () => {
          if (!fetched) {
            content.replaceChildren(await treeChildren(row.id));
            fetched = true;
          }
        };
        details.addEventListener("toggle", () => {
          if (details.open) {
            expanded.add(row.id);
            if (!fetched)
              run(expand);
          } else
            expanded.delete(row.id);
        });
        if (expanded.has(row.id) || draft.selected.startsWith(row.id + ".")) {
          await expand();
          details.open = true;
        }
      }
      if (row.is_message)
        li.append(messageButton(row.id, row.label,
          row.done  ? "done" :
            row.wip ? "wip" :
                      "not_started",
          false, row.label_key, row.shared));
      ul.append(li);
    }
    return ul;
  }
  function messageButton(id, text, status, fallback = false, captionKey = "", shared = false) {
    const button = document.createElement("button");
    button.type = "button";
    button.title = id;
    button.append(navigationCaption(captionKey, text));
    if (shared)
      button.append(document.createTextNode(" "), phrase("builder.navigation.shared"));
    button.append(
      document.createTextNode(" · "), phrase(statusKeys[status] || statusKeys.not_started));
    if (fallback)
      button.append(document.createTextNode(" · "), phrase("builder.editor.native_fallback"));
    button.setAttribute("aria-current", String(draft.selected === id));
    button.addEventListener("click", () => run(async () => {
      if (discard()) {
        fillMetadata();
        await showMessage(id);
      }
    }));
    return button;
  }
  function emptyBody(ref) {
    return [...ref.anchors.map(id => "@anchor " + id), "@empty", "@end", ""].join("\n");
  }
  function showShape(p) {
    const descriptions = [];
    const add = (key, args) => descriptions.push(phrase(key, args, "li"));
    if (p?.maximum_pages)
      add("builder.editor.maximum_pages", {count: p.maximum_pages});
    if (p?.maximum_lines)
      add("builder.editor.maximum_lines", {count: p.maximum_lines});
    if (p?.required_nonempty_lines)
      add("builder.editor.required_choices", {count: p.required_nonempty_lines});
    if (p?.keyboard)
      add("builder.editor.keyboard_shape", {
        rows: p.keyboard.rows,
        columns: p.keyboard.columns,
        lines: p.keyboard.maximum_lines,
        bytes: p.keyboard.maximum_page_bytes
      });
    for (const rule of p?.table?.rules || []) {
      const item = document.createElement("li");
      item.append(phrase(rule.last_line === 255          ? "builder.editor.all_rows" :
                      rule.first_line === rule.last_line ? "builder.editor.single_row" :
                                                           "builder.editor.row_range",
                    {first: rule.first_line + 1, last: rule.last_line + 1}),
        document.createTextNode(": "),
        phrase(
          rule.native_reserved ? "builder.editor.native_artwork" : "builder.editor.table_fields",
          rule.native_reserved ? {} : {fields: rule.fields.join(", ")}));
      descriptions.push(item);
    }
    $("shape").hidden = !descriptions.length;
    $("shape-rules").replaceChildren(...descriptions);
  }
  async function showMessage(id) {
    playback?.invalidate();
    const data = await json("message", undefined, {...identity(), id});
    draft.selected = id;
    draft.message = data.message;
    showShape(draft.message.reference.presentation);
    raw("message-title", data.location?.title || id);
    if (data.location?.title_key)
      ui.set($("message-title"), "builder.navigation." + data.location.title_key);
    ui.unbind($("message-context"));
    const loc = data.location;
    $("message-context").replaceChildren(document.createTextNode(id + " · "));
    if (loc?.shared)
      $("message-context")
        .append(phrase("builder.navigation.context.shared"), document.createTextNode(" "));
    $("message-context")
      .append(navigationCaption(
                loc?.context_key || loc?.group_key, loc?.context || loc?.group_label || ""),
        document.createTextNode("\n"),
        phrase(
          draft.message.present ? "builder.editor.saved_path" : "builder.editor.message_absent",
          {path: draft.message.path}));
    if (!draft.message.reference.native_in_profile)
      $("message-context")
        .append(document.createTextNode(" "), phrase("builder.editor.cross_region"));
    renderReference(data);
    label("anchors",
      draft.message.reference.anchors.length ? "builder.editor.required_anchors" :
                                               "builder.editor.no_anchors",
      {anchors: draft.message.reference.anchors.join(" → ")});
    $("body").value =
      draft.message.present ? draft.message.body : emptyBody(draft.message.reference);
    $("message-status").value = draft.message.status;
    $("placeholders").replaceChildren(keyedOption("", "builder.editor.choose_value"));
    for (const p of draft.message.reference.placeholders)
      $("placeholders")
        .append(Object.hasOwn(valueKindKeys, p.kind) ?
            keyedOption(p.name, valueKindKeys[p.kind], {name: p.name}) :
            option(p.name, p.name + " (" + p.kind + ")"));
    $("message-editor").hidden = false;
    panel.querySelector(".loc-preview").hidden = true;
    draft.messageDirty = false;
    raw("dirty", "");
    for (const b of panel.querySelectorAll("#loc-tree button,#loc-results button"))
      b.setAttribute("aria-current", String(b.title === id));
    readonly();
  }
  function renderReference(data) {
    const m = data.referenceMetadata;
    if (m)
      label("reference-title",
        state.reference && state.reference.id !== m.id ? "builder.editor.reference_fallback" :
                                                         "builder.editor.reference_name",
        {name: m.name, locale: m.locale});
    else
      label("reference-title", "builder.editor.source_reference");
    if (data.reference?.body)
      raw("reference-body", data.reference.body);
    else
      label("reference-body", "builder.editor.reference_missing");
    if (data.reference?.body)
      contentLanguage($("reference-body"), m);
    else {
      $("reference-body").removeAttribute("dir");
      $("reference-body").removeAttribute("lang");
    }
  }
  async function refreshReference(next) {
    // This is deliberately not adopt(): it must preserve the selected message,
    // caret, status, metadata, notices and every unsaved draft verbatim.
    state.reference = next.reference;
    state.sourceAvailable = next.sourceAvailable;
    await projects();
    if (draft.selected)
      renderReference(await json("message", undefined, {...identity(), id: draft.selected}));
  }
  function referenceForm(open) {
    $("extract-reference").hidden = !open;
    $("extract-reference-toggle").setAttribute("aria-expanded", String(open));
    if (open)
      $("extract-reference").elements.file.focus();
  }
  $("extract-reference-toggle")
    .addEventListener("click", () => referenceForm($("extract-reference").hidden));
  $("cancel-reference").addEventListener("click", () => referenceForm(false));
  $("reference-project").addEventListener("change", () => run(async () => {
    try {
      await refreshReference(
        await json("reference", {...identity(), id: $("reference-project").value}));
      feedbackKey("builder.editor.reference_changed");
    } finally {
      const id = state.reference?.id;
      $("reference-project").value =
        [...$("reference-project").options].some(o => o.value === id) ? id : "";
    }
  }));
  function insert(text, command = false) {
    const input = $("body"), at = input.selectionStart;
    if (command)
      text = (at && input.value[at - 1] !== "\n" ? "\n" : "") + text + "\n";
    input.setRangeText(text, input.selectionStart, input.selectionEnd, "end");
    input.focus();
    markDirty();
  }
  function populateStyleChoices() {
    const font = $("style-font"), treatment = $("style-treatment");
    const previousFont = font.value, previousStyle = treatment.value;
    const roles = fonts.roles;
    font.replaceChildren(...roles.map(name => option(name, name)));
    font.value = roles.includes(previousFont) ? previousFont : "body";
    const styles = (state.project?.treatments || []).map(style => style.definition.name);
    treatment.replaceChildren(...(styles.length ? styles.map(name => option(name, name)) :
      [keyedOption("", "builder.styling.no_styles")]));
    treatment.value = styles.includes(previousStyle) ? previousStyle : styles[0] || "";
  }
  function inlineStyleUnavailable() {
    if (!state.project || state.project.origin === "native-source") return "builder.styling.read_only";
    if (state.project.formatVersion !== 2) return "builder.styling.upgrade";
    if ($("body").value.trimStart().startsWith("@alias ")) return "builder.styling.alias";
    if (draft.message?.reference.presentation?.shape === "inline") return "builder.styling.term";
    return "";
  }
  function updateInlineStyleAvailability() {
    const reason = inlineStyleUnavailable(), status = $("style-status");
    for (const control of $("inline-styles").querySelectorAll("button,input,select")) {
      const needsStyle = control.dataset.locStyle === "style" || control === $("style-treatment");
      control.disabled = closed || busy || !!reason || !draft.message || needsStyle && !$("style-treatment").value;
    }
    if (reason) {
      label("style-status", reason);
      status.dataset.error = "false";
    } else if (status.dataset.unavailable) {
      raw("style-status", "");
    }
    status.dataset.unavailable = reason;
  }
  function inlineStyleTags(kind) {
    if (kind === "italic") return ["<i>", "</i>"];
    if (kind === "upright") return ['<span italic="false">', "</span>"];
    let value;
    if (kind === "font") {
      value = $("style-font").value;
      if (!fonts.roles.includes(value)) throw failure("builder.styling.choose_font");
    } else if (kind === "style") {
      value = $("style-treatment").value;
      if (!(state.project?.treatments || []).some(style => style.definition.name === value))
        throw failure("builder.styling.choose_style");
    } else if (kind === "color") {
      value = $("style-color").value.toUpperCase();
      if (!/^#[0-9A-F]{6}$/.test(value)) throw failure("builder.styling.invalid_color");
    } else if (kind === "scale") {
      const percent = Number($("style-scale").value);
      if (!Number.isInteger(percent) || percent < 25 || percent > 400)
        throw failure("builder.styling.invalid_size");
      value = percent + "%";
    } else return null;
    return [`<span ${kind}="${value}">`, "</span>"];
  }
  function applyInlineStyle(kind) {
    if (busy || closed || !draft.message || inlineStyleUnavailable()) return;
    try {
      const tags = inlineStyleTags(kind);
      if (!tags) return;
      const input = $("body");
      const edit = window.workshopInlineStyle.wrap(input.value,
        input.selectionStart, input.selectionEnd, ...tags);
      input.focus();
      input.setSelectionRange(edit.start, edit.end);
      // insertText participates in the browser's native undo history. The
      // fallback keeps insertion available in hosts without that editing API.
      if (!document.execCommand?.("insertText", false, edit.text))
        input.setRangeText(edit.text, edit.start, edit.end, "end");
      input.setSelectionRange(edit.selectStart, edit.selectEnd);
      raw("style-status", "");
      markDirty();
    } catch (error) {
      label("style-status", error.uiKey || "builder.styling.selection");
      $("style-status").dataset.error = "true";
    }
  }
  for (const button of panel.querySelectorAll("[data-loc-style]")) {
    button.addEventListener("pointerdown", event => event.preventDefault());
    button.addEventListener("click", () => applyInlineStyle(button.dataset.locStyle));
  }
  function publicationOptions() {
    return {...identity(), confirmRights: $("rights").checked, includeWIP: $("wip").checked};
  }
  async function download(endpoint) {
    if (hasEdits())
      throw failure("builder.language.save_before_export");
    const data = await json(endpoint, {...publicationOptions(), prepareDownload: true});
    const a = $("download");
    a.href = data.url;
    a.download = data.name;
    ui.set(a, "builder.language.download_name", {name: data.name});
    a.hidden = false;
    $("editor-actions").open = false;
    a.scrollIntoView({block: "center"});
    a.focus();
    feedbackKey(
      endpoint === "backup" ? "builder.language.backup_ready" : "builder.language.export_ready");
  }
  function submit(id, action) {
    $(id).addEventListener("submit", event => {
      event.preventDefault();
      const form = event.currentTarget, data = new FormData(form);
      return run(() => action(data, form));
    });
  }
  async function openSelectedProject() {
    if (!discard())
      return;
    await adopt(await json("open", {id: $("projects").value}));
    projectDestination();
    if (flow.phase === "review")
      await prepareReview("install");
    feedbackKey("builder.language.opened_feedback");
  }
  $("open").addEventListener("click", () => run(openSelectedProject));
  $("refresh").addEventListener("click", () => run(projects));
  submit("extract-reference", async data => {
    data.set("intent", "reference");
    for (const [key, value] of Object.entries(identity()))
      data.set(key, value);
    await refreshReference(await json("extract", data));
    referenceForm(false);
    $("extract-reference").reset();
    feedbackKey("builder.editor.reference_extracted");
  });
  async function extractSource(data) {
    if (!discard())
      return;
    await adopt(await json("extract", data));
    if (flow.kind === "create") {
      flow.phase = "choose";
      workflowView();
    } else {
      projectDestination();
    }
    feedbackKey("builder.language.extracted_feedback");
  }
  async function beginImportInstallation() {
    if (window.workshopOpenLanguages && !window.workshopOpenLanguages())
      return false;
    if (!loaded)
      await adopt(await json("state"));
    clearDraftEdits();
    flow.kind = "install";
    flow.phase = "choose";
    flow.installPath = "import";
    flow.editorTab = "messages";
    $("download").hidden = true;
    return true;
  }
  async function adoptImportedProject(next, replaceInstall) {
    await adopt(next);

    imports.reset();
    projectDestination();
    if (flow.kind === "install") {
      try {
        await installCurrent(replaceInstall);
      } catch (error) {
        const uncertain = error.code === "AR_NETWORK" || error.code === "AR_RESPONSE";
        throw withOutcome(error,
          uncertain ? "builder.language.import_uncertain" : "builder.language.import_partial",
          "Project imported into Workshop; installation " + (uncertain ? "could not be confirmed." : "did not complete."));
      }
    } else
      feedbackKey("builder.language.imported_edit");
  }
  async function createProject(data) {
    if (!discard())
      return;
    await adopt(await json("create", {
      metadata: {...Object.fromEntries(data), direction: "auto"},
    }));
    flow.phase = "edit";
    workflowView();
    feedbackKey("builder.language.created_feedback");
  }
  async function cloneProject(data) {
    if (!discard())
      return;
    await adopt(await json("clone", {
      ...identity(),
      newID: data.get("newID"),
      metadata: {name: data.get("name")},
    }));
    flow.phase = "edit";
    workflowView();
    feedbackKey("builder.language.cloned_feedback");
  }
  submit("extract", extractSource);
  submit("create", createProject);
  submit("clone", cloneProject);
  submit("upgrade-v2", async data => {
    if (!discard()) return;
    const upgraded = await json("upgrade-v2", {...identity(), newID: data.get("newID")});
    await adopt(upgraded.state);
    flow.phase = "edit";
    workflowView();
    feedbackKey("builder.language.upgrade_complete", {
      messages: upgraded.report.messages,
      expanded: upgraded.report.aliasesMaterialized,
    });
  });
  $("metadata").addEventListener("input", () => {
    draft.detailsDirty = true;
    draft.saveFailure = "";
    updateContentLanguage();
    saveIndicator();
  });
  $("notice").addEventListener("input", () => {
    draft.noticeDirty = true;
    draft.saveFailure = "";
    saveIndicator();
  });
  $("notices").addEventListener("change", () => {
    if (draft.noticeDirty && !window.confirm(ui.text("builder.editor.discard_notice"))) {
      $("notices").value = "";
      return;
    }
    const name = $("notices").value;
    $("notice").elements.noticeName.value = name.replace(/^notices\//, "");
    $("notice").elements.noticeText.value = state.project.notices[name] || "";
    draft.noticeDirty = false;
    saveIndicator();
  });
  $("body").addEventListener("input", markDirty);
  $("message-status").addEventListener("change", markDirty);
  function saveProgress() {
    if (busy || !hasEdits() || state.project?.origin === "native-source")
      return;
    for (const [id, changed] of [["metadata", draft.detailsDirty], ["notice", draft.noticeDirty],
           ["font-stack", fonts.dirty]]) {
      if (changed && !$(id).checkValidity()) {
        flow.editorTab = id === "font-stack" ? "fonts" : "details";
        workflowView();
        $(id).reportValidity();
        return;
      }
    }
    // Capture before run() disables controls: FormData omits disabled fields.
    const fields = Object.fromEntries(new FormData($("metadata"))), notes = fields.notes;
    delete fields.notes;
    const q = {
      ...identity(),
      saveMessage: draft.messageDirty,
      saveDetails: draft.detailsDirty,
      saveNotice: draft.noticeDirty,
      id: draft.selected,
      body: $("body").value,
      status: $("message-status").value,
      metadata: fields,
      notes,
      ...Object.fromEntries(new FormData($("notice")))
    };
    q.saveFonts = fonts.dirty;
    q.fonts = fonts.fonts();
    const payload = fonts.payload(q);
    return run(async () => {
      let next;
      try {
        next = await json("save", payload);
      } catch (error) {
        draft.saveFailure = error.uiArgs?.detail || error.message;
        throw error;
      }
      await adopt(next, true);
      const update = next.installationUpdate;
      if (update?.error) {
        const error = window.workshopFeedback?.responseError ?
          window.workshopFeedback.responseError(update, 200) :
          failure(update.errorCode || "builder.language.request_failed", {detail: update.error});
        error.operation = "Update installed language pack after saving";
        reportFailure(withOutcome(error, "builder.language.saved_update_failed",
          "Workshop project saved; installed copy was not updated."));
      }
      else if (update?.updated)
        feedbackKey(update.enabled ? "builder.language.saved_installed" :
                                     "builder.language.saved_disabled");
      else
        feedbackKey("builder.language.saved_feedback");
    });
  }
  for (const id of ["save", "save-progress"])
    $(id).addEventListener("click", saveProgress);
  for (const id of ["metadata", "notice", "font-stack"])
    $(id).addEventListener("submit", event => {
      event.preventDefault();
      saveProgress();
    });
  document.addEventListener("keydown", event => {
    if (!panel.hidden && flow.phase === "edit" && flow.kind !== "home" &&
      (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
      event.preventDefault();
      saveProgress();
    }
  });
  $("insert-value").addEventListener("click", () => {
    if ($("placeholders").value)
      insert("{" + $("placeholders").value + "}");
  });
  for (const el of panel.querySelectorAll("[data-loc-insert]"))
    el.addEventListener("click", () => insert(el.dataset.locInsert, true));
  $("independent").addEventListener("click", () => run(async () => {
    if (draft.messageDirty && !window.confirm(ui.text("builder.editor.replace_alias")))
      return;
    $("body").value = (await json("materialize", {...identity(), id: draft.selected})).body;
    markDirty();
  }));
  $("empty").addEventListener("click", () => {
    if (window.confirm(ui.text("builder.editor.clear_confirm"))) {
      $("body").value = emptyBody(draft.message.reference);
      markDirty();
    }
  });
  $("preview").addEventListener("click", () => run(async () => {
    const ops = await json("preview", fonts.payload({
      ...identity(), id: draft.selected, body: $("body").value,
      status: $("message-status").value, saveFonts: fonts.dirty, fonts: fonts.fonts(),
      saveDetails: draft.detailsDirty, metadata: previewMetadata()
    }));
    const container = $("preview-content");
    container.replaceChildren();
    let page, number = 0;
    const newPage = () => {
      const heading = phrase("builder.editor.page_number", {number: ++number}, "h4");
      page = document.createElement("div");
      page.className = "loc-preview-page";
      contentLanguage(page, draftLanguage());
      container.append(heading, page);
    };
    newPage();
    for (const op of ops) {
      if (op.op === "page")
        newPage();
      else if (op.op === "text")
        page.append(document.createTextNode(op.value));
      else if (op.op === "line" || op.op === "preferred_line" || op.op === "paragraph")
        page.append(document.createTextNode(op.op === "paragraph" ? "\n\n" : "\n"));
      else if (op.op === "placeholder") {
        const value = document.createElement("span");
        value.textContent =
          "⟦" + op.name + (op.minimum_digits ? ":0" + op.minimum_digits : "") + "⟧";
        value.className = "loc-preview-value";
        contentLanguage(value, {direction: "ltr"});
        ui.attribute(value, "title", "builder.editor.runtime_value");
        page.append(value);
      } else {
        const control = document.createElement("div");
        control.className = "loc-preview-control";
        contentLanguage(control, {direction: "ltr"});
        control.textContent = op.op + (op.id ? ": " + op.id : "");
        if (!op.id && op.frames)
          control.append(
            document.createTextNode(": "), phrase("builder.editor.frames", {count: op.frames}));
        page.append(control);
      }
    }
    panel.querySelector(".loc-preview").hidden = false;
    feedbackKey("builder.editor.preview_valid");
    void playback?.show();
  }));
  async function search(more) {
    if (!more) {
      offset = 0;
      searchSnapshot = {
        q: $("search").elements.q.value,
        status: $("search").elements.status.value,
        ...identity()
      };
      ui.unbind($("results"));
      $("results").replaceChildren();
    }
    const data = await json("search", undefined, {...searchSnapshot, offset});
    $("tree").hidden = true;
    $("results").hidden = false;
    for (const row of data.rows)
      $("results").append(messageButton(
        row.id, row.title || row.id, row.status, !row.present, row.label_key, row.shared));
    offset += data.rows.length;
    $("more").hidden = offset >= data.total;
    if (!data.total)
      label("results", "builder.editor.no_messages");
    feedbackKey("builder.editor.search_count", {shown: offset, total: data.total});
  }
  submit("search", () => search(false));
  $("more").addEventListener("click", () => run(() => search(true)));
  $("browse").addEventListener("click", () => run(tree));
  $("backup").addEventListener("click", () => run(() => download("backup")));
  $("publish").addEventListener("click", () => run(() => download("publish")));
  for (const id of ["rights", "wip"])
    $(id).addEventListener("change", () => {
      raw("publication-report", "");
      renderTextCoverage(null);
    });
  $("check").addEventListener("click", () => run(async () => {
    if (hasEdits())
      throw failure("builder.language.save_first");
    const report = await json("publication-check", publicationOptions());
    label("publication-report", "builder.language.publication_report", {
      included: report.included,
      wip: report.wip,
      omitted: report.unchangedSource,
      fallback: report.fallback
    });
    renderTextCoverage(report.coverage);
  }));
  async function prepareReview(purpose) {
    flow.reviewPurpose = purpose;
    flow.phase = "review";
    $("editor-actions").open = false;
    feedback("");
    const m = state.project.metadata;
    label("review-pack", "builder.language.review_pack",
      {name: m.name, locale: m.locale, id: m.id, author: m.author});
    raw("publication-report", "");
    renderTextCoverage(null);
    if (purpose === "install") {
      installedExists = !!(await json("installation", identity())).installed;
      const r = await json("installation-check", identity());
      label("publication-report", "builder.language.install_report",
        {count: r.messages, fallback: r.fallback});
      renderTextCoverage(r.coverage);
    }
    workflowView();
    $("review-title").scrollIntoView({block: "nearest"});
    $("review-title").focus();
  }
  async function installCurrent(replace) {
    if (hasEdits())
      throw failure("builder.language.save_before_install");
    let data;
    try {
      data = await json("install", {...identity(), replace});
    } catch (error) {
      if (error.code === "builder.language.request_conflict") {
        try {
          installedExists = !!(await json("installation", identity())).installed;
          workflowView();
        } catch {
          // Refreshing the replacement choice is optional. Retain the original
          // conflict if the read fails, and never rerun font validation here.
        }
      }
      throw error;
    }
    installedExists = true;
    $("installed-upgrade").hidden = !data.report.upgrade;
    label("publication-report", "builder.language.installed_report",
      {count: data.report.messages, path: data.path});
    feedbackKey("builder.language.installed_feedback");
    $("installed-name").textContent = state.project.metadata.name + " · " +
      state.project.metadata.locale + " · " + state.project.metadata.id;
    label("installed-title",
      data.enabled ? "builder.language.installed_title" : "builder.language.updated_disabled");
    $("installed-steps").hidden = !data.enabled;
    if (!data.enabled)
      feedbackKey("builder.language.disabled_update");
    label("install-state", "builder.language.installed_path", {path: data.path});
    flow.phase = "installed";
    workflowView();
  }
  $("install").addEventListener("click", () => run(async () => {
    if (installedExists && !$("replace-install").checked)
      throw failure("builder.language.confirm_replace");
    await installCurrent($("replace-install").checked);
  }));
  async function startWorkflow(kind) {
    if (!discard())
      return;
    clearDraftEdits();
    flow.kind = kind;
    flow.phase = "choose";
    flow.installPath = kind === "install" ? "import" : "";
    imports.reset();
    flow.editorTab = "messages";
    $("archive-options").open = kind === "edit";
    $("download").hidden = true;
    feedback("");
    workflowView();
    await projects();
  }
  async function chooseInstallPath(path) {
    flow.installPath = path;
    imports.reset();
    feedback("");
    workflowView();
    await projects();
  }
  async function returnToLanguages() {
    if (!discard())
      return;
    clearDraftEdits();
    flow.kind = "home";
    $("download").hidden = true;
    feedback("");
    workflowView();
    await library.refresh();
    window.scrollTo(0, 0);
    $("title").focus({preventScroll: true});
  }
  for (const button of panel.querySelectorAll("[data-loc-flow]")) {
    button.addEventListener("click", () => run(() => startWorkflow(button.dataset.locFlow)));
  }
  for (const button of panel.querySelectorAll("[data-loc-install]")) {
    button.addEventListener("click", () => run(() => chooseInstallPath(button.dataset.locInstall)));
  }
  function backToLanguages() {
    return run(returnToLanguages);
  }
  $("home-button").addEventListener("click", backToLanguages);
  $("installed-home").addEventListener("click", backToLanguages);
  $("back-projects").addEventListener("click", backToLanguages);
  for (const purpose of ["install", "publish"])
    $("review-" + purpose).addEventListener("click", () => run(async () => {
      if (hasEdits())
        throw failure("builder.language.save_before_review");
      await prepareReview(purpose);
    }));
  for (const id of ["back-edit", "installed-edit"])
    $(id).addEventListener("click", () => {
      flow.phase = "edit";
      feedback("");
      workflowView();
      window.scrollTo(0, 0);
    });
  function editorView(name, focus = false) {
    flow.editorTab = state.project?.origin === "native-source" ? "messages" : name;
    workflowView();
    // Tab panels have different heights. Reset their scroll position so the
    // sticky toolbar cannot conceal the new panel's first heading/controls.
    panel.scrollIntoView({block: "start"});
    if (focus)
      $("tab-" + name).focus({preventScroll: true});
  }
  for (const name of ["messages", "details", "fonts"])
    $("tab-" + name).addEventListener("click", () => editorView(name));
  panel.querySelector(".loc-editor-tabs").addEventListener("keydown", event => {
    if (["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key) &&
      !$("tab-details").hidden) {
      const tabs = ["messages", "details", "fonts"], step = event.key === "ArrowLeft" ? -1 : 1;
      event.preventDefault();
      editorView(event.key === "Home" ?
          tabs[0] :
          event.key === "End" ?
          tabs[2] :
          tabs[(tabs.indexOf(flow.editorTab) + step + tabs.length) % tabs.length],
        true);
    }
  });
  document.addEventListener("pointerdown", event => {
    if (!$("editor-actions").contains(event.target))
      $("editor-actions").open = false;
  });
  $("tree-view").addEventListener("change", () => run(async () => {
    expanded.clear();
    await tree();
  }));
  window.addEventListener("beforeunload", event => {
    if (hasEdits()) {
      event.preventDefault();
      event.returnValue = "";
    }
  });
  // The surrounding builder handles tab keyboard navigation. Unsaved text is
  // kept when visiting Build/Assets/Manual and guarded before project changes.
  window.localizationActivate = () => run(async () => {
    const next = await json("state");
    if (!loaded)
      await adopt(next);
    else {
      await refreshReference(next);
      workflowView();
      if (flow.kind === "home")
        await library.refresh();
    }
  });
  // The shell passes only a project ID. It never reads archives or edits pack
  // state; this same guarded Go-backed workflow handles library navigation.
  function openProject(id, review = false) {
    return run(async () => {
      if (!discard())
        return;
      await adopt(await json("open", {id}));
      flow.kind = "edit";
      flow.phase = review ? "review" : "edit";
      flow.reviewPurpose = "install";
      flow.editorTab = "messages";
      workflowView();
      if (review)
        await prepareReview("install");
      window.scrollTo(0, 0);
      feedback("");
    });
  }
  window.localizationOpenProject = openProject;
  window.localizationHasEdits = hasEdits;
  document.addEventListener("workshop:closed", () => {
    imports.clearDrop();
    closed = true;
    library.close();
    playback?.invalidate();
    clearDraftEdits();
    for (const control of panel.querySelectorAll("button,input,textarea,select"))
      control.disabled = true;
  });
  workflowView();
})();
