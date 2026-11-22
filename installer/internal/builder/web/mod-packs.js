(() => {
  "use strict";

  const ui = window.workshopI18n;
  const musicType = "actraiser-music";
  const titleType = "actraiser-title";
  const manualType = "actraiser-manual";
  const languageType = "actraiser-language";
  const allTypes = "all";
  const typeTitles = {
    [titleType]: "builder.assets.title_art",
    [musicType]: "builder.mods.title",
    [languageType]: "builder.nav.languages",
    [manualType]: "builder.mods.manual_packs",
  };
  const key = name => "builder.mods." + name;
  const canInstall = file => /\.(o2r|arlang|pdf)$/i.test(file.name);
  const jsonOptions = body => ({
    method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(body),
  });
  function createRequests(request) {
    let tail = Promise.resolve(), count = 0, writes = 0;
    const listeners = [];
    const notify = () => listeners.forEach(listener => listener(count !== 0));
    const catalog = () => request("mods/catalog", {cache: "no-store"});
    function run(operation) {
      count++;
      notify();
      const task = tail.then(operation).finally(() => {
        count--;
        notify();
      });
      tail = task.catch(() => {});
      return task;
    }
    return {
      run,
      write(operation) {
        writes++;
        return run(operation).finally(() => { writes--; });
      },
      catalog,
      async changeLanguage(endpoint, payload) {
        try {
          if (endpoint === "mods/activation") return await request(endpoint, jsonOptions(payload));
          if (payload.modId) {
            const action = endpoint === "select-language-release" ? "select" : payload.enabled ? "enable" : "disable";
            return await request("mods/activation", jsonOptions({action, pack: payload.modId, release: payload.release}));
          }
          await request("localization/" + endpoint, jsonOptions(payload));
        } catch (error) {
          error.catalog = await catalog().catch(() => null);
          throw error;
        }
        return catalog();
      },
      subscribe(listener) { listeners.push(listener); },
      isBusy: () => count !== 0,
      isWriting: () => writes !== 0,
    };
  }

  function markMutations(container, busy) {
    for (const control of container.querySelectorAll("[data-mod-mutation]")) {
      control.setAttribute("aria-disabled", String(busy));
    }
  }

  function slotName(track, region) {
    const song = ui.text("builder.assets.track." + track.replaceAll("-", "_"), {}, track);
    return region ? ui.text(key("regional_slot"), {
      song, region: ui.text("builder.assets.region." + region.replaceAll("-", "_"), {}, region),
    }) : song;
  }

  function targetName(target) {
    const [, track, region] = /^(song-\d+)(?:-(.+))?$/.exec(target) || [];
    return target ? slotName(track || target, region) : target;
  }

  function modSlotName(slot) {
    if (slot.type === titleType) return ui.text("builder.assets.title_art");
    if (slot.type === manualType) return ui.text(key("manual_slot"));
    return slotName(slot.track, slot.region);
  }

  function button(parent, label, action, options = {}) {
    const control = document.createElement("button");
    control.type = "button";
    control.className = options.quiet ? "quiet" : "secondary";
    const caption = document.createElement("span");
    if (options.message) ui.set(caption, options.message, options.args);
    else caption.textContent = label;
    control.append(caption);
    if (options.pressed !== undefined) control.setAttribute("aria-pressed", String(options.pressed));
    if (options.id) control.dataset.modControl = options.id;
    control.disabled = !!options.disabled;
    if (options.mutation) control.dataset.modMutation = "";
    control.addEventListener("click", () => {
      if (control.getAttribute("aria-disabled") !== "true") return action();
    });
    parent.append(control);
    return control;
  }

  function confirmUninstall(name, prompt = "uninstall_prompt") {
    return window.workshopConfirm(key("uninstall"), key(prompt), {name});
  }

  function uninstallAction(parent, item, name, change, legacy) {
    if (item.bundled || (!item.uninstallRevision && !legacy)) return;
    button(parent, "", async () => {
      if (await confirmUninstall(name, legacy ? "uninstall_legacy" : undefined)) {
        await change({action: "uninstall", filename: item.filename, revision: item.uninstallRevision});
      }
    }, {message: key("uninstall"), id: "uninstall:" + (item.filename || item.key), mutation: true, quiet: true});
  }

  function renderLegacyManual(parent, remove) {
    const name = ui.text(key("legacy_manual"));
    const note = document.createElement("p");
    ui.set(note, key("legacy_manual"));
    const preview = document.createElement("a");
    preview.className = "linkbtn";
    preview.href = "manual.pdf?legacy=true";
    preview.target = "_blank";
    preview.rel = "noreferrer";
    ui.set(preview, "builder.help.open_manual");
    parent.append(note, preview);
    button(parent, "", async () => {
      if (await confirmUninstall(name, "uninstall_manual")) await remove();
    }, {message: key("uninstall"), id: "uninstall:legacy-manual", mutation: true, quiet: true});
  }

  function packTable(container, label) {
    const table = document.createElement("table");
    table.className = "mod-table";
    ui.attribute(table, "aria-label", label);
    const header = document.createElement("thead");
    const columns = document.createElement("tr");
    for (const column of ["enabled", "mod", "actions"]) {
      const cell = document.createElement("th");
      cell.scope = "col";
      ui.set(cell, key(column));
      columns.append(cell);
    }
    header.append(columns);
    const rows = document.createElement("tbody");
    table.append(header, rows);
    container.append(table);
    return rows;
  }

  function packRow(rows, {name, details, status, statusArgs, kind, enabled, id, disabled, toggle}) {
    const row = document.createElement("tr");
    row.dataset.bigRow = "";
    const check = document.createElement("td");
    const input = document.createElement("input");
    input.type = "checkbox";
    input.className = "mod-checkbox";
    input.checked = !!enabled;
    input.disabled = !!disabled;
    input.dataset.modControl = id;
    input.dataset.modMutation = "";
    ui.attribute(input, "aria-label", key(enabled ? "disable_action" : "enable_action"), {name});
    input.addEventListener("click", async event => {
      if (input.getAttribute("aria-disabled") === "true") {
        event.preventDefault();
        return;
      }
      const next = input.checked;
      try { await toggle(next); }
      finally { input.checked = !!enabled; }
    });
    input.addEventListener("keydown", event => {
      if (event.key === "Enter") {
        event.preventDefault();
        if (!event.repeat) input.click();
      }
    });
    check.append(input);

    const mod = document.createElement("th");
    mod.scope = "row";
    const title = document.createElement("span");
    title.textContent = name;
    const heading = document.createElement("div");
    heading.className = "mod-row-title";
    heading.append(title);
    mod.append(heading);
    const metadata = details.filter(Boolean).join(" · ");
    if (metadata) {
      const detail = document.createElement("small");
      detail.textContent = metadata;
      mod.append(detail);
    }
    if (status) {
      const badge = document.createElement("span");
      badge.className = "mod-status";
      badge.dataset.state = kind;
      ui.set(badge, key(status), statusArgs);
      heading.append(badge);
    }

    const actions = document.createElement("td");
    const controls = document.createElement("div");
    controls.className = "mod-row-actions";
    actions.append(controls);
    row.append(check, mod, actions);
    rows.append(row);
    return controls;
  }

  function previewAction(parent, name, id, open) {
    const control = button(parent, "", () => {
      if (control.isConnected) control.focus();
      open();
    }, {message: key("preview_button"), id});
    control.dataset.modPreview = "";
    ui.attribute(control, "aria-label", key("open_preview"), {name});
    control.setAttribute("aria-haspopup", "dialog");
  }

  function emptyList(container, message) {
    const empty = document.createElement("p");
    empty.className = "mod-empty";
    ui.set(empty, message);
    container.append(empty);
  }

  function choiceRow(parent, name, action, {id, pressed, message, args, details = []}, controller) {
    if (controller) {
      const row = controller.row(parent, {
        name, details, id, status: pressed ? "active_pack" : "", action, actionLabel: "use_pack",
      });
      row.control.setAttribute("aria-pressed", String(pressed));
      return row;
    }
    const row = document.createElement("div");
    row.className = "mod-pack-candidate";
    row.dataset.bigRow = "";
    parent.append(row);
    return {actions: row, control: button(row, [name, ...details].join(" · "), action, {id, pressed, message, args, mutation: true})};
  }

  function listPack(container, pack, controller) {
    if (!controller) return packRow(container, pack);
    const {name, details, status, statusArgs, kind, enabled, id, disabled, toggle} = pack;
    const {actions} = controller.row(container, {
      name, details, status, statusArgs, kind, enabled, id, action: disabled ? undefined : () => toggle(!enabled),
      actionLabel: enabled ? "disable_pack" : "enable_pack",
    });
    return actions;
  }

  function unclassifiedPacks(catalog) {
    return (catalog.packs || []).filter(pack => !Object.hasOwn(typeTitles, pack.type) && pack.error);
  }

  function renderPackIssues(container, catalog, change, controller) {
    container.replaceChildren();
    const packs = unclassifiedPacks(catalog);
    if (!packs.length) {
      emptyList(container, key("no_archive_issues"));
      return;
    }
    const list = document.createElement("ul");
    list.className = "mod-archive-issues";
    for (const pack of packs) {
      if (controller) {
        const {actions} = controller.row(container, {
          name: pack.filename, details: [pack.error], id: "issue:" + pack.filename,
        });
        uninstallAction(actions, pack, pack.filename, change);
        continue;
      }
      const row = document.createElement("li");
      row.dataset.bigRow = "";
      const details = document.createElement("div");
      const name = document.createElement("strong");
      const error = document.createElement("p");
      name.textContent = pack.filename;
      error.textContent = pack.error;
      details.append(name, error);
      row.append(details);
      uninstallAction(row, pack, pack.filename, change);
      list.append(row);
    }
    if (!controller) container.append(list);
  }

  function renderRows(container, catalog, change, {type = musicType, preview, controller} = {}) {
    container.replaceChildren();
    const conflictsOnly = type === "conflicts";
    const slots = (catalog.slots || []).filter(slot => conflictsOnly || slot.type === type);
    const pending = new Map(), active = new Map();
    for (const slot of slots) {
      if (slot.winner) active.set(slot.winner, (active.get(slot.winner) || 0) + 1);
      if (!slot.needsChoice) continue;
      for (const id of slot.candidates) {
        if (!pending.has(id)) pending.set(id, []);
        pending.get(id).push(slot);
      }
    }
    const packs = (catalog.packs || []).filter(pack => conflictsOnly ?
      pack.enabled && !pack.error && pending.has(pack.id) : pack.type === type);
    if (controller) {
      const names = new Intl.Collator(ui.locale, {numeric: true});
      packs.sort((a, b) => names.compare(a.name || a.filename, b.name || b.filename));
    }
    if (type === titleType) {
      for (const id of catalog.titleBundleConflicts || []) {
        const warning = document.createElement("p");
        warning.className = "mod-warning";
        ui.set(warning, key("bundled_title_conflict"), {id});
        container.append(warning);
      }
    }
    if (!packs.length) {
      emptyList(container, key(conflictsOnly ? "no_conflicting_mods" : type === titleType ? "empty_title" : type === manualType ? "empty_manual" : "empty"));
      return;
    }
    const rows = controller ? container : packTable(container, key("installed_packs"));
    for (const pack of packs) {
      const name = pack.name || pack.filename;
      const source = ui.text(key(pack.bundled ? "included" : pack.external ? "external" : "local"));
      const unresolved = pending.get(pack.id) || [];
      const problem = pack.enabled && unresolved.length > 0;
      const used = active.get(pack.id) || 0, total = pack.targets?.length || 1;
      const activity = !pack.enabled ? "" : !used ? "inactive" : used < total ? "partly_active" : "active_pack";
      const status = pack.error ? "needs_attention" : problem ? (unresolved.length === 1 ? "conflict_badge" : "choices_needed") :
        activity;
      const controls = listPack(rows, {
        name, details: [conflictsOnly && ui.text(typeTitles[pack.type]), pack.version, pack.author, source, pack.error], enabled: pack.enabled,
        id: "pack:" + (pack.id || pack.filename), disabled: pack.error,
        status, statusArgs: {count: unresolved.length, used, total}, kind: problem || pack.error ? "problem" : used ? "enabled" : "inactive",
        toggle: enabled => change({action: enabled ? "enable" : "disable", pack: pack.id}),
      }, controller);
      if (preview && !pack.error) previewAction(controls, name, "details:" + pack.id, () => preview({pack, type: pack.type, target: unresolved[0]?.id}));
      uninstallAction(controls, pack, name, change);
    }
  }

  const modControls = container =>
    [...container.querySelectorAll("[data-mod-control]")].filter(control => !control.disabled);

  function captureFocus(container) {
    const keys = modControls(container);
    const index = keys.indexOf(document.activeElement);
    if (index < 0) return;
    // Keep the same control, then try the next surviving item before looking back.
    return [...keys.slice(index), ...keys.slice(0, index).reverse()].map(control => control.dataset.modControl);
  }

  function restoreFocus(container, keys) {
    if (!keys) return false;
    const controls = modControls(container);
    const byKey = new Map(controls.map(control => [control.dataset.modControl, control]));
    const target = keys.map(key => byKey.get(key)).find(Boolean) || controls[0];
    target?.focus();
    return !!target;
  }

  function renderLanguageRows(container, catalog, mutate, {preview, controller, search = "", counts} = {}) {
    container.replaceChildren();
    const needsRelease = pack => !!pack.choices?.length && !pack.release;
    const query = search.trim().toLocaleLowerCase();
    const installed = catalog.filter(pack => {
      if (!pack.installed || !query) return pack.installed;
      const fields = [pack.name, pack.installedName, pack.locale, pack.id, pack.modId, pack.key, pack.filename];
      if (pack.required) fields.push(ui.text(key("native_us")));
      for (const releases of [pack.choices, pack.localAlternates]) {
        for (const choice of releases || []) fields.push(choice.name, choice.filename, choice.sourcePath);
      }
      return fields.some(field => field?.toLocaleLowerCase().includes(query));
    }).sort((a, b) => Number(needsRelease(b)) - Number(needsRelease(a)));
    if (counts) {
      const enabled = installed.filter(pack => pack.enabled).length;
      ui.set(counts, "builder.language.activation_counts", {enabled, disabled: installed.length - enabled});
      counts.hidden = !installed.length;
    }
    if (!installed.length) {
      emptyList(container, query ? "builder.language.no_packages" : "builder.big_screen.no_packs");
      return;
    }
    const rows = controller ? container : packTable(container, key("installed_packs"));
    for (const pack of installed) {
      const name = pack.required ? ui.text(key("native_us")) : pack.installedName || pack.name;
      const problem = pack.error || needsRelease(pack);
      const controls = listPack(rows, {
        name, details: [pack.locale, pack.version, !needsRelease(pack) && ui.text(key(pack.required ? "local_rom" : pack.external ? "external" : "local")),
          !pack.modId && pack.key !== pack.id && pack.key !== name && pack.key, pack.error],
        status: pack.required ? "required_language" : problem ? "needs_attention" : "",
        kind: problem ? "problem" : "enabled",
        enabled: pack.enabled, id: "language:" + pack.key,
        disabled: pack.required || !pack.modId && (!pack.installedRevision || !pack.enabled && !!pack.error),
        toggle: enabled => mutate("set-enabled", {
          id: pack.id, directory: pack.key, expected: pack.installedRevision, modId: pack.modId, enabled,
        }),
      }, controller);
      if (preview && !pack.error && (!pack.modId || pack.release)) {
        previewAction(controls, name, "language-preview:" + pack.key, () => preview({pack, name, type: languageType, release: pack.release}));
      }
      if (pack.required) continue;
      const uninstall = payload => mutate("mods/activation", payload);
      if (!pack.choices?.length) {
        uninstallAction(controls, pack, name, uninstall);
        if (!pack.modId && !pack.external && pack.installedRevision && pack.id) {
          uninstallAction(controls, pack, name, () => mutate("uninstall", {
            id: pack.id, directory: pack.key, expected: pack.installedRevision, confirmUninstall: true,
          }), true);
        }
        if (!pack.localAlternates?.length) continue;
      }

      const group = document.createElement("div");
      group.className = "mod-pack-conflict";
      if (controller) rows.append(group);
      else {
        const row = document.createElement("tr");
        row.className = "mod-release-row";
        const cell = document.createElement("td");
        cell.colSpan = 3;
        row.append(cell);
        rows.append(row);
        cell.append(group);
      }
      const heading = document.createElement("h4");
      ui.set(heading, key("releases_for"), {name});
      group.append(heading);
      if (needsRelease(pack)) {
        const warning = document.createElement("p");
        ui.set(warning, key("unresolved_language"));
        group.append(warning);
      }
      for (const choice of pack.choices || []) {
        const details = [choice.version || ui.text("builder.language.legacy_release"), choice.sourcePath || choice.filename].filter(Boolean);
        const label = [choice.name, ...details].join(" · ");
        const {actions} = choiceRow(group, choice.name, () => mutate("select-language-release", {
          id: pack.id, modId: pack.modId, release: choice.key,
        }), {
          details, id: "language-release:" + pack.key + ":" + choice.key, pressed: pack.release === choice.key,
        }, controller);
        if (preview) previewAction(actions, label, "language-release-preview:" + pack.key + ":" + choice.key,
          () => preview({pack, archive: choice, type: languageType, release: choice.key, name: choice.name}));
        uninstallAction(actions, choice, label, uninstall);
      }
      for (const choice of pack.localAlternates || []) {
        const details = [choice.version, choice.filename].filter(Boolean);
        const label = [choice.name, ...details].join(" · ");
        let release;
        if (controller) release = controller.row(group, {
          name: choice.name, details, status: "inactive_pack", id: "inactive-language-release:" + pack.key + ":" + choice.key,
        }).actions;
        else {
          release = document.createElement("div");
          release.className = "mod-pack-candidate";
          release.dataset.bigRow = "";
          group.append(release);
          const text = document.createElement("span");
          ui.set(text, key("inactive_release"), {name: label});
          release.append(text);
        }
        if (preview) previewAction(release, label, "language-release-preview:" + pack.key + ":" + choice.key,
          () => preview({pack, archive: choice, type: languageType, release: choice.key, name: choice.name}));
        uninstallAction(release, choice, label, uninstall);
      }
    }
  }

  function renderLanguages(container, catalog, mutate, options) {
    renderLanguageRows(container, catalog.languages || [], mutate, options);
    if (catalog.languageError) {
      const error = document.createElement("p");
      error.className = "mod-warning";
      ui.set(error, "builder.request_failed", {detail: catalog.languageError});
      container.prepend(error);
    }
  }

  function renderGroups(rows, catalog, change, changeLanguage, options) {
    rows.replaceChildren();
    for (const [type, label] of Object.entries(typeTitles)) {
      const hasPacks = type === languageType ? (catalog.languages?.length || catalog.languageError) :
        catalog.packs?.some(pack => pack.type === type);
      if (!hasPacks && !(type === titleType && catalog.titleBundleConflicts?.length)) continue;
      const group = document.createElement("section");
      group.className = "mod-type-group";
      const heading = document.createElement("h2");
      ui.set(heading, label);
      const content = document.createElement("div");
      group.append(heading, content);
      rows.append(group);
      if (type === languageType) renderLanguages(content, catalog, changeLanguage, options);
      else renderRows(content, catalog, change, {...options, type});
    }
    if (unclassifiedPacks(catalog).length) {
      const group = document.createElement("section");
      group.className = "mod-type-group";
      const heading = document.createElement("h2");
      ui.set(heading, key("needs_attention"));
      const content = document.createElement("div");
      renderPackIssues(content, catalog, change, options?.controller);
      group.append(heading, content);
      rows.append(group);
    }
    if (!rows.children.length && !catalog.legacyManual) emptyList(rows, key("empty_all"));
  }


  function uploadOutcomeKey(outcome) {
    if (outcome.detail) return key("invalid");
    if (outcome.legacyManual) return key("file_installed_fallback");
    return key(outcome.converted ? "file_converted" : "file_installed");
  }

  function renderUploadResults(list, outcomes) {
    list.replaceChildren(...outcomes.map(outcome => {
      const item = document.createElement("li");
      if (outcome.detail) item.dataset.failed = "true";
      ui.set(item, uploadOutcomeKey(outcome), outcome);
      return item;
    }));
  }

  function renderInstallFeedback(status, catalog, install = catalog) {
    const {installedId: id, installedName: name, installedType: type, installedLegacyManual} = install;
    const pack = (type === languageType ? catalog.languages : catalog.packs)
      ?.find(item => (type === languageType ? item.modId : item.id) === id);
    const slots = (catalog.slots || []).filter(slot => slot.candidates.includes(id));

    let message = "installed_named";
    if (installedLegacyManual) message = "fallback_installed";
    else if (!name) message = "installed";
    else if (pack?.enabled === false) message = "installed_disabled";
    else if (slots.some(slot => slot.needsChoice)) message = "installed_conflict";
    else if (type === languageType) {
      message = pack?.choices?.length && !pack.release ? "installed_language_release" : "installed_language";
    } else if (slots.length && !slots.some(slot => slot.winner === id)) message = "installed_alternate";
    status.hidden = false;
    ui.set(status, key(message), {name});

  }

  window.workshopModPacks = {renderRows, renderGroups, renderLanguages, renderLanguageRows, jsonOptions, targetName, modSlotName, createRequests, markMutations,
    musicType, titleType, manualType, languageType, allTypes, canInstall,
    captureFocus, restoreFocus, unclassifiedPacks, renderPackIssues, renderUploadResults, renderInstallFeedback};

  window.workshopAssetPackPanel = (request, onInstalled, openPreview, requests = createRequests(request)) => {
    const panel = document.getElementById("asset-packs");
    const rows = document.getElementById("asset-pack-list");
    const status = document.getElementById("asset-pack-status");
    const batch = document.getElementById("asset-pack-batch");
    const batchList = document.getElementById("asset-pack-batch-list");
    const batchDismiss = document.getElementById("asset-pack-batch-dismiss");
    const input = document.getElementById("asset-pack-file");
    const install = document.getElementById("asset-pack-install");
    const filter = document.getElementById("asset-pack-type");
    const languageSearchField = document.getElementById("asset-pack-language-search-field");
    const languageSearch = document.getElementById("asset-pack-language-search");
    const languageCounts = document.getElementById("asset-pack-language-counts");
    const clearFilter = document.getElementById("asset-pack-clear-filter");
    const manualReader = document.getElementById("asset-pack-manual-reader");
    const legacyManual = document.getElementById("asset-pack-legacy-manual");
    const refresh = document.getElementById("asset-pack-refresh");
    let viewType = allTypes, catalog;
    const preview = openPreview && function(options) {
      const saved = () => {
        show(key("settings_saved"));
        paint();
      };
      openPreview({...options, catalog, onChanged: next => {
        catalog = next;
        const dialog = document.getElementById("mod-preview-dialog");
        if (dialog.open) dialog.addEventListener("close", saved, {once: true});
        else saved();
      }});
    };
    let pendingLoad, focusFilterAfterPaint = false;
    let uploadQueue = Promise.resolve();

    function show(message, args = {}) {
      status.hidden = !message;
      if (message) ui.set(status, message, args);
    }
    function fail(error, operation) {
      show();
      window.workshopFeedback?.show(status, error, {operation});
    }

    function restoreFilterFocus() {
      focusFilterAfterPaint = false;
      if (document.activeElement === document.body && filter.getClientRects().length) filter.focus();
    }

    function paint() {
      if (!catalog) return;
      // A scan can finish after a preview opens. Keep the original control
      // attached until the dialog has restored focus to it.
      const dialog = document.querySelector("dialog[open]");
      if (dialog) {
        dialog.addEventListener("close", paint, {once: true});
        return;
      }
      const focus = captureFocus(panel);
      if (viewType === languageType) renderLanguages(rows, catalog, changeLanguage, {preview, search: languageSearch.value, counts: languageCounts});
      else if (viewType === allTypes) renderGroups(rows, catalog, change, changeLanguage, {preview});
      else renderRows(rows, catalog, change, {type: viewType, preview});
      manualReader.hidden = viewType !== manualType || !catalog.slots?.some(slot => slot.type === manualType && slot.winner);
      legacyManual.hidden = !catalog.legacyManual || (viewType !== manualType && viewType !== allTypes);
      legacyManual.replaceChildren();
      if (!legacyManual.hidden) renderLegacyManual(legacyManual, () => update(requests.write, async () => {
        await request("manual", {method: "DELETE"});
        catalog = await requests.catalog();
      }, key("uninstalled")));
      markMutations(legacyManual, requests.isBusy());
      markMutations(rows, requests.isBusy());
      if (focus && !restoreFocus(panel, focus)) {
        focusFilterAfterPaint = true;
        if (!requests.isBusy()) restoreFilterFocus();
      }
    }

    function setType(type) {
      if (type !== allTypes && type !== "conflicts" && !Object.hasOwn(typeTitles, type)) return;
      languageSearch.value = "";
      languageSearchField.hidden = type !== languageType;
      languageCounts.hidden = true;
      if (type !== viewType) {
        viewType = filter.value = type;
        clearFilter.hidden = type === allTypes;
        legacyManual.hidden = true;
        manualReader.hidden = true;
        rows.replaceChildren();
      }
    }

    requests.subscribe(busy => {
      install.disabled = input.disabled = filter.disabled = clearFilter.disabled = busy;
      refresh.setAttribute("aria-disabled", String(busy));
      if (busy) panel.setAttribute("aria-busy", "true");
      else panel.removeAttribute("aria-busy");
      markMutations(rows, busy);
      markMutations(legacyManual, busy);
      if (!busy && focusFilterAfterPaint) restoreFilterFocus();
    });

    function update(run, operation, message) {
      return run(async () => {
        show("builder.big_screen.loading");
        try {
          await operation();
          paint();
          show(message);
          return null;
        } catch (error) {
          fail(error, "Manage mods");
          return error.message;
        }
      });
    }

    function change(value) {
      return update(requests.write, async () => {
        catalog = await request("mods/activation", jsonOptions(value));
      }, value.action === "uninstall" ? key("uninstalled") : "builder.assets.saved");
    }

    function changeLanguage(endpoint, payload) {
      return update(requests.write, async () => {
        try {
          catalog = await requests.changeLanguage(endpoint, payload);
        } catch (error) {
          if (error.catalog) catalog = error.catalog;
          paint();
          throw error;
        }
      }, payload.action === "uninstall" || endpoint === "uninstall" ? key("uninstalled") : "builder.language.availability_saved");
    }

    function load() {
      if (pendingLoad) return pendingLoad;
      pendingLoad = update(requests.run, async () => {
        try {
          catalog = await requests.catalog();
          ui.set(refresh, key("refresh"));
        } catch (error) {
          ui.set(refresh, key("retry_scan"));
          throw error;
        }
      }).finally(() => { pendingLoad = undefined; });
      return pendingLoad;
    }
    function upload(files) {
      uploadQueue = uploadQueue.then(async () => {
        batch.hidden = true;
        const outcomes = [];
        let last;
        show("builder.big_screen.loading");
        await requests.write(async () => {
          for (const file of files) {
            if (!canInstall(file)) {
              outcomes.push({name: file.name, detail: ui.text(key("legacy_types"))});
              continue;
            }
            try {
              const format = file.name.slice(file.name.lastIndexOf(".")).toLowerCase();
              last = await request("mods/install?format=" + format, {method: "POST", body: file});
              outcomes.push({
                name: last.installedName || file.name, type: last.installedType, legacyManual: last.installedLegacyManual,
                converted: format !== ".o2r" && !last.installedLegacyManual,
              });
            } catch (error) {
              outcomes.push({name: file.name, detail: error.message});
            }
          }
          if (last) catalog = last;
        });
        show();
        const types = new Set(outcomes.map(item => item.type).filter(Boolean));
        const type = types.size > 1 ? allTypes : last?.installedType;
        const shown = last && onInstalled({type, outcomes, legacyManual: last.installedLegacyManual, catalog: last});
        if (shown) paint();
        if (outcomes.length > 1) {
          batch.hidden = false;
          renderUploadResults(batchList, outcomes);
        }
        if (outcomes.some(item => item.detail)) {
          const report = outcomes.map(item => ui.text(uploadOutcomeKey(item), item)).join("\n");
          fail(new Error(report), "Install mods");
        } else if (shown && outcomes.length === 1) renderInstallFeedback(status, catalog);
      });
      return uploadQueue;
    }

    batchDismiss.addEventListener("click", () => {
      batch.hidden = true;
      filter.focus();
    });
    refresh.addEventListener("click", () => { if (!requests.isBusy()) load(); });
    function filterRows(type) {
      show();
      setType(type);
      if (catalog) paint();
      else return load();
    }
    clearFilter.addEventListener("click", async () => {
      await filterRows(allTypes);
      filter.focus();
    });
    filter.addEventListener("change", () => filterRows(filter.value));
    languageSearch.addEventListener("input", paint);
    document.addEventListener("workshop:language", paint);
    install.addEventListener("click", () => input.click());
    input.addEventListener("change", async () => {
      if (input.files.length) await upload([...input.files]);
      input.value = "";
      window.workshopFileInputs?.refresh(input);
    });

    function showInstalled(next) {
      catalog = next;
      paint();
      renderInstallFeedback(status, catalog);
    }

    return {load, upload, setType, showInstalled};
  };
})();
