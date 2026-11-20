(() => {
  "use strict";

  window.workshopBigScreenCustomize = (request, openDesktop, onClose, previewPack, sharedRequests) => {
    const ui = window.workshopI18n;
    const modPacks = window.workshopModPacks;
    const requests = sharedRequests || modPacks.createRequests(request);
    const $ = id => document.getElementById(id);
    const section = $("big-screen-customize"), menu = $("big-screen-menu");
    const choices = $("big-customize-menu");
    const filterDialog = $("big-mod-filter-dialog"), filterButton = $("big-mod-filter");
    const conflictButton = $("big-mod-conflicts");
    const primaryButton = $("big-mod-primary"), previewButton = $("big-mod-preview-open"), moreButton = $("big-mod-more");
    const moreDialog = $("big-mod-actions-dialog"), moreOptions = $("big-mod-options");
    const issues = choices.querySelector('[data-big-page="issues"]');
    const installResults = $("big-mod-install-results"), content = $("big-customize-content");
    const rows = $("big-customize-rows"), report = $("big-customize-status");
    const paging = $("big-customize-paging");
    const desktop = $("big-customize-desktop");
    const refresh = $("big-mod-refresh"), install = $("big-mod-install");
    const backButton = $("big-customize-back"), extract = $("big-music-extract");
    const actions = $("big-mod-actions"), soundtrack = $("big-game-music");
    const filters = [...choices.querySelectorAll("[data-big-page]")];
    const modTypes = {
      all: modPacks.allTypes, title: modPacks.titleType, packs: modPacks.musicType,
      manual: modPacks.manualType, languages: modPacks.languageType,
    };
    const desktopDestinations = {title: "title", packs: "music", languages: "languages"};
    const items = new Map();
    let selected, conflictID;
    const options = {preview: previewPack, controller: {row: modRow}};
    let page = "", filter = "all", stamp = 0, saving = false, timer = 0;
    let catalog, installResult, pendingCatalog;
    let music = [], originals = {}, trackID = "";

    const activeFilter = () => filters.find(button => button.dataset.bigPage === filter);

    function status(key, args) {
      report.hidden = !key;
      if (key) ui.set(report, key, args);
    }

    function fail(error, operation) {
      status();
      window.workshopFeedback?.show(report, error, {operation});
    }

    function syncBusy() {
      const busy = requests.isBusy();
      if (busy) section.setAttribute("aria-busy", "true");
      else section.removeAttribute("aria-busy");
      modPacks.markMutations(rows, busy);
      modPacks.markMutations(moreOptions, busy);
      primaryButton.setAttribute("aria-disabled", String(busy));
      for (const button of [refresh, extract, install]) button.setAttribute("aria-disabled", String(busy));
      for (const button of [backButton, desktop]) button.setAttribute("aria-disabled", String(saving));
    }
    requests.subscribe(syncBusy);

    function previewFor(item) {
      return item?.actions.querySelector("[data-mod-preview]");
    }

    function otherActions(item) {
      return item ? [...item.actions.children].filter(button => !button.hasAttribute("data-mod-preview")) : [];
    }

    function selectRow(item) {
      if (selected) delete selected.control.dataset.selected;
      selected = item;
      if (item) item.control.dataset.selected = "true";
      primaryButton.hidden = !item?.action;
      if (item?.action) ui.set($("big-mod-primary-label"), "builder.mods." + item.actionLabel);
      previewButton.hidden = !previewFor(item);
      moreButton.hidden = !otherActions(item).length;
    }

    // Each entry has one focus target. Supplemental controls are presented
    // in the shared action strip or its dialog for the highlighted entry.
    function modRow(container, {name, details = [], id, status, statusArgs, kind, enabled, action, actionLabel}) {
      const control = document.createElement("button"), actions = document.createElement("div");
      const labels = document.createElement("span"), title = document.createElement("strong");
      control.type = "button";
      control.className = "secondary big-mod-entry";
      control.dataset.modControl = id;
      control.dataset.bigRow = "";
      control.dataset.bigSide = "big-mod-filter big-mod-footer";
      if (action && typeof enabled === "boolean") control.setAttribute("aria-pressed", String(enabled));
      title.textContent = name;
      labels.append(title);
      const metadata = details.filter(Boolean).join(" · ");
      if (metadata) {
        const detail = document.createElement("small");
        detail.textContent = metadata;
        labels.append(detail);
      }
      control.append(labels);
      const badges = document.createElement("span");
      badges.className = "big-mod-badges";
      for (const key of [typeof enabled === "boolean" && (enabled ? "enabled" : "disabled"), status].filter(Boolean)) {
        const badge = document.createElement("span");
        badge.className = "mod-status";
        if (key === "enabled" || key === "active_pack") badge.dataset.state = "enabled";
        else if (key === status && kind) badge.dataset.state = kind;
        ui.set(badge, "builder.mods." + key, key === status ? statusArgs : undefined);
        badges.append(badge);
      }
      control.append(badges);
      const item = {name, control, actions, action, actionLabel};
      items.set(id, item);
      control.addEventListener("focus", () => {
        selectRow(item);
        // The action strip and status can change the height of the list.
        // Keep the whole focused row in view after those changes settle.
        requestAnimationFrame(() => {
          if (document.activeElement !== control) return;
          const viewport = content.getBoundingClientRect();
          const row = control.getBoundingClientRect();
          if (row.top < viewport.top + 6) content.scrollTop += row.top - viewport.top - 6;
          else if (row.bottom > viewport.bottom - 6) content.scrollTop += row.bottom - viewport.bottom + 6;
        });
      });
      control.addEventListener("click", () => {
        control.focus();
        if (action) action();
        else {
          const preview = previewFor(item);
          if (preview) preview.click();
          else if (!moreButton.hidden) moreButton.click();
        }
      });
      container.append(control);
      return {control, actions};
    }

    primaryButton.addEventListener("click", () => selected?.action?.());
    previewButton.addEventListener("click", () => previewFor(selected)?.click());
    moreButton.addEventListener("click", () => {
      const available = otherActions(selected);
      if (!available.length || moreDialog.open) return;
      $("big-mod-actions-title").textContent = selected.name;
      moreOptions.replaceChildren(...available.map(source => {
        const button = source.cloneNode(true);
        button.addEventListener("click", () => {
          if (button.getAttribute("aria-disabled") === "true") return;
          moreDialog.close();
          source.click();
        });
        return button;
      }));
      syncBusy();
      moreDialog.showModal();
    });

    function action(text, description, handler) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "secondary big-screen-row";
      button.dataset.bigRow = "";
      button.textContent = text;
      if (description) {
        const small = document.createElement("small");
        small.textContent = description;
        button.append(small);
      }
      button.addEventListener("click", handler);
      rows.append(button);
      return button;
    }

    function changeLanguage(endpoint, payload) {
      return save(async () => {
        installResult = undefined;
        try {
          catalog = await requests.changeLanguage(endpoint, payload);
        } catch (error) {
          if (error.catalog) {
            catalog = error.catalog;
            paintLibrary();
          }
          throw error;
        }
        paintLibrary();
        showInstallResult();
        status(endpoint === "uninstall" || payload.action === "uninstall" ?
          "builder.mods.uninstalled" : "builder.language.availability_saved");
      });
    }

    function paintLibrary(focusList = false) {
      if (!catalog || page !== "library") return;
      const focus = modPacks.captureFocus(rows);
      const selectionID = selected?.control.dataset.modControl;
      const scroll = content.scrollTop;
      items.clear();
      issues.hidden = !modPacks.unclassifiedPacks(catalog).length;
      if (filter === "issues" && issues.hidden) setFilter("all");
      const conflicts = filteredConflicts();
      conflictButton.hidden = !conflicts.length;
      if (conflicts.length) {
        const pending = conflicts.filter(slot => !slot.winner).length;
        ui.set(conflictButton, pending ? "builder.mods.review_conflicts" : "builder.mods.edit_choices", {count: pending || conflicts.length});
      }
      if (filter === "all") modPacks.renderGroups(rows, catalog, changePack, changeLanguage, options);
      else if (filter === "languages") modPacks.renderLanguages(rows, catalog, changeLanguage, options);
      else if (filter === "issues") modPacks.renderPackIssues(rows, catalog, changePack, options.controller);
      else modPacks.renderRows(rows, catalog, changePack, {...options, type: modTypes[filter]});
      if ((filter === "all" || filter === "manual") && catalog.legacyManual)
        action(ui.text("builder.mods.legacy_manual"), ui.text("builder.mods.desktop_manual"),
          () => openDesktop("legacy-manual")).dataset.modControl = "legacy-manual-link";
      const first = rows.querySelector("[data-mod-control]");
      selectRow(items.get(selectionID) || items.values().next().value);
      content.scrollTop = focusList ? 0 : scroll;
      if (focusList) (first || filterButton).focus();
      else if (focus && !modPacks.restoreFocus(rows, focus)) (first || filterButton).focus();
      syncBusy();
    }

    function showInstallResult() {
      const outcomes = installResult?.outcomes || [];
      installResults.hidden = outcomes.length < 2;
      if (installResults.hidden) content.removeAttribute("tabindex");
      else content.tabIndex = 0;
      modPacks.renderUploadResults(installResults, installResults.hidden ? [] : outcomes);
      if (installResult?.catalog && catalog && installResults.hidden) {
        modPacks.renderInstallFeedback(report, null, rows, catalog, installResult.catalog);
      }
    }

    function setFilter(next) {
      filter = next;
      for (const button of filters) button.setAttribute("aria-pressed", String(button.dataset.bigPage === filter));
      ui.set($("big-mod-filter-value"), activeFilter().dataset.i18n);
      soundtrack.hidden = page !== "library" || filter !== "packs";
      updateDesktopShortcut();
    }

    function updateDesktopShortcut() {
      desktop.hidden = page === "targets" || page === "candidates" || (page === "library" && !desktopDestinations[filter]);
    }

    function filteredConflicts() {
      return (catalog?.slots || [])
        .filter(slot => slot.candidates.length > 1 && (filter === "all" || slot.type === modTypes[filter]))
        .sort((a, b) => Number(!!a.winner) - Number(!!b.winner));
    }

    function paintChoices(focus) {
      const slots = filteredConflicts();
      const byID = new Map(catalog.packs.map(pack => [pack.id, pack]));
      const slot = slots.find(item => item.id === conflictID);
      if (!slots.length) return render("library", conflictButton.id);
      if (page === "candidates" && slot) {
        ui.unbind($("big-customize-title"));
        $("big-customize-title").textContent = modPacks.modSlotName(slot);
        status("builder.mods.choose_pack");
        modPacks.renderCandidates(rows, slot, byID, changePack, previewPack, options.controller);
        const winner = items.get("select:" + slot.id + ":" + slot.winner);
        (items.get(focus)?.control || winner?.control || rows.firstElementChild).focus();
        return;
      }
      page = "targets";
      ui.set($("big-customize-title"), slots.some(target => !target.winner) ? "builder.mods.conflicts" : "builder.mods.selected_packs");
      for (const target of slots) {
        const state = target.winner ? ui.text("builder.mods.current", {name: byID.get(target.winner)?.name || target.winner}) :
          ui.text("builder.mods.no_choice");
        const button = action(modPacks.modSlotName(target), state, () => {
          conflictID = target.id;
          render("candidates");
        });
        button.classList.add("big-conflict-target");
        button.dataset.target = target.id;
      }
      ([...rows.children].find(button => button.dataset.target === focus) || rows.firstElementChild).focus();
    }

    function loadLibrary(quiet = false, focusList = false) {
      const version = stamp;
      if (pendingCatalog?.version === version) return pendingCatalog.task;
      if (!quiet) status("builder.big_screen.loading");
      const task = requests.run(requests.catalog).then(data => {
        if (stamp !== version) return;
        catalog = data;
        ui.set(refresh, "builder.mods.refresh");
        const dialog = document.querySelector("dialog[open]");
        if (dialog) {
          dialog.addEventListener("close", refreshOnFocus, {once: true});
          return;
        }
        if (page === "targets" || page === "candidates") {
          const current = document.activeElement;
          render(page, current.dataset.target || current.dataset.modControl);
          if (current.matches("button") && current.getClientRects().length) current.focus();
          return;
        }
        paintLibrary(focusList && document.activeElement === filterButton);
        if (!quiet) status("");
        showInstallResult();
      }).catch(error => {
        if (stamp !== version) return;
        ui.set(refresh, "builder.mods.retry_scan");
        fail(error, "Load mod library");
      }).finally(() => {
        if (pendingCatalog?.task === task) pendingCatalog = undefined;
      });
      pendingCatalog = {version, task};
      return task;
    }

    function refreshOnFocus() {
      if (!["library", "targets", "candidates"].includes(page)) return;
      const dialog = document.querySelector("dialog[open]");
      if (dialog) {
        dialog.addEventListener("close", refreshOnFocus, {once: true});
        return;
      }
      return loadLibrary(true);
    }

    function trackName(track) {
      return ui.text("builder.assets.track." + track.id.replaceAll("-", "_"), {}, track.name);
    }

    function replacementsFor(track) {
      const replacements = new Map();
      const label = target => target === track.id ? ui.text("builder.assets.installed_replacement") : modPacks.targetName(target);
      const legacy = ui.text("builder.assets.legacy_replacement");
      if (track.url) replacements.set(track.id, {label: label(track.id), source: legacy, url: track.url});
      for (const variant of track.variants || []) {
        if (variant.url) replacements.set(variant.name, {label: label(variant.name), source: legacy, url: variant.url});
      }
      for (const slot of catalog?.slots || []) {
        if (slot.type !== modPacks.musicType || slot.track !== track.id || !slot.winner) continue;
        const pack = catalog.packs.find(item => item.id === slot.winner && item.type === modPacks.musicType && !item.error);
        const index = pack?.tracks?.findIndex(item => item.track === slot.track && (item.region || "") === (slot.region || ""));
        const url = pack?.previews?.[index];
        if (url) replacements.set(slot.id, {label: label(slot.id), source: pack.name, url});
      }
      return [...replacements.values()];
    }

    function paintMusic() {
      rows.replaceChildren();
      for (const track of music) {
        const description = track.replacements.length ? ui.text("builder.assets.installed_replacement") :
          catalog ? ui.text("builder.assets.using_original") : "";
        const button = action(trackName(track), description, () => {
          trackID = track.id;
          render("track");
        });
        button.dataset.trackId = track.id;
      }
    }

    function preview(name, source, url, first = false) {
      if ([...rows.children].some(row => row.dataset.previewSource === url)) return;
      const row = document.createElement("div");
      row.dataset.previewSource = url;
      window.workshopAudioTransport(rows, row, {url, name: source ? name + " · " + source : name},
        error => fail(error, "Play audio preview"));
      if (first) rows.prepend(row);
      else rows.append(row);
    }

    function paintTrack() {
      const track = music.find(item => item.id === trackID);
      if (!track) return;
      $("big-customize-title").textContent = trackName(track);
      const original = originals.tracks?.find(item => item.id === track.id);
      if (original?.url) preview(ui.text("builder.assets.original_rom"), "", original.url, true);
      for (const replacement of track.replacements) {
        preview(replacement.label, replacement.source, replacement.url);
      }
      const generating = originals.state === "generating";
      extract.hidden = !!original?.url || !originals.romAvailable;
      extract.disabled = generating;
      status(generating ? "builder.assets.preview_progress" :
        originals.state === "failed" ? "builder.assets.preview_failed_detail" :
        !rows.children.length ? "builder.big_screen.no_audio" : "",
        {count: originals.completed || 0, total: originals.total || 0, detail: originals.error || ""});
    }

    async function readOriginals(version) {
      try {
        const data = await request("audio-previews");
        if (stamp !== version) return;
        originals = data;
        paintTrack();
        if (originals.state === "generating") timer = setTimeout(() => readOriginals(version), 500);
        else if (document.activeElement === report) (rows.querySelector("[data-audio-play]") || extract).focus();
      } catch (error) {
        if (stamp === version) fail(error, "Read audio previews");
      }
    }

    function render(next, focus) {
      const version = ++stamp;
      clearTimeout(timer);
      window.workshopStopAudio(rows);
      page = next;
      const library = page === "library";
      document.body.dataset.bigModLibrary = String(!!page);
      menu.hidden = !!page;
      section.hidden = !page;
      actions.hidden = !library;
      conflictButton.hidden = true;
      paging.hidden = page === "track";
      rows.replaceChildren();
      items.clear();
      selectRow();
      extract.hidden = true;
      status("");
      installResults.hidden = true;
      content.removeAttribute("tabindex");
      if (!page) return;
      if (library) {
        ui.set($("big-customize-title"), "builder.nav.mods");
        setFilter(filter);
        const resume = focus === soundtrack.id && !soundtrack.hidden ? soundtrack :
          focus === conflictButton.id ? conflictButton : null;
        filterButton.focus();
        if (catalog) {
          paintLibrary(!resume);
          if (resume && !resume.hidden) resume.focus();
          showInstallResult();
        } else loadLibrary(false, true);
        return;
      }
      soundtrack.hidden = true;
      updateDesktopShortcut();
      if (page === "targets" || page === "candidates") return paintChoices(focus);
      ui.set($("big-customize-title"), "builder.mods.rom_music");
      if (page === "track") {
        ui.unbind($("big-customize-title"));
        paintTrack();
        (rows.querySelector("[data-audio-play]") || (!extract.hidden && !extract.disabled ? extract : desktop)).focus();
        readOriginals(version);
        return;
      }
      status("builder.big_screen.loading");
      report.focus();
      Promise.all([request("assets"), request("audio-previews")]).then(([data, detail]) => {
        if (stamp !== version) return;
        const focusFirst = document.activeElement === report;
        status("");
        music = (data.tracks || []).map(track => ({...track, replacements: replacementsFor(track)}));
        originals = detail;
        paintMusic();
        if (focusFirst) {
          const first = [...rows.children].find(button => button.dataset.trackId === focus) || rows.querySelector("button");
          (first || backButton).focus();
        }
      }).catch(error => {
        if (stamp !== version) return;
        backButton.focus();
        fail(error, "Load game music");
      });
    }

    async function save(change, run = requests.write) {
      if (saving || requests.isBusy()) return;
      saving = true;
      try { await run(change); }
      catch (error) { fail(error, "Manage mods"); }
      finally {
        saving = false;
        syncBusy();
      }
    }

    function changePack(value) {
      return save(async () => {
        installResult = undefined;
        catalog = await request("mods/activation", modPacks.jsonOptions(value));
        if (page === "candidates") {
          render("targets", conflictID);
          return;
        }
        paintLibrary();
        showInstallResult();
        status(value.action === "uninstall" ? "builder.mods.uninstalled" : "builder.mods.settings_saved");
      });
    }

    function back() {
      if (saving) return;
      if (page === "candidates") render("targets", conflictID);
      else if (page === "targets") render("library", conflictButton.id);
      else if (page === "track") render("music", trackID);
      else if (page === "music") render("library", soundtrack.id);
      else { render(""); onClose(); }
    }

    filterButton.addEventListener("click", () => {
      if (saving || filterDialog.open) return;
      filterDialog.showModal();
      activeFilter().focus();
    });
    conflictButton.addEventListener("click", () => {
      if (!saving && filteredConflicts().length) render("targets");
    });
    for (const button of filters) button.addEventListener("click", () => {
      if (saving) return;
      filterDialog.close();
      installResult = undefined;
      status();
      setFilter(button.dataset.bigPage);
      paintLibrary(true);
      showInstallResult();
    });
    soundtrack.addEventListener("click", () => render("music"));
    extract.addEventListener("click", () => {
      const version = stamp;
      save(async () => {
        originals = await request("audio-previews", {method: "POST"});
        paintTrack();
        (rows.querySelector("button") || backButton).focus();
        readOriginals(version);
      }, requests.run);
    });
    backButton.addEventListener("click", back);
    refresh.addEventListener("click", () => {
      if (!requests.isBusy()) loadLibrary();
    });
    desktop.addEventListener("click", () => openDesktop(page === "library" ? desktopDestinations[filter] : "music"));
    document.addEventListener("workshop:closed", () => render(""));

    function openLibrary(next, result) {
      catalog = result?.catalog;
      installResult = result;
      issues.hidden = true;
      filter = next;
      render("library");
    }

    return {
      open: () => openLibrary("all"),
      openAssetPacks: (type, result) =>
        openLibrary(Object.keys(modTypes).find(key => modTypes[key] === type) || "all", result),
      close: () => render(""), back, refreshOnFocus, isOpen: () => !!page, isBusy: () => saving,
    };
  };
})();
