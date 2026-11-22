(() => {
  "use strict";

  window.workshopModPreview = (request, requests = window.workshopModPacks.createRequests(request)) => {
    const ui = window.workshopI18n;
    const dialog = document.getElementById("mod-preview-dialog");
    const title = document.getElementById("mod-preview-title");
    const status = document.getElementById("mod-preview-status");
    const content = document.getElementById("mod-preview-content");
    const sourceDialog = document.getElementById("mod-source-dialog");
    const sourceTitle = document.getElementById("mod-source-title");
    const sourceList = document.getElementById("mod-source-list");
    const paging = document.getElementById("mod-source-paging");
    function fitSourceList() {
      paging.hidden = sourceList.scrollHeight <= sourceList.clientHeight;
      if (sourceList.contains(document.activeElement)) document.activeElement.scrollIntoView({block:"nearest"});
    }
    sourceDialog.addEventListener("close", () => {
      if (sourceDialog.open) return;
      window.removeEventListener("resize", fitSourceList);
      sourceList.replaceChildren();
    });
    const close = document.getElementById("mod-preview-close");
    const save = document.getElementById("mod-preview-save");
    const prev = document.getElementById("mod-preview-prev");
    const next = document.getElementById("mod-preview-next");
    const image = document.createElement("img");
    let manual, page, activeArchive, choices, attempt = 0;

    function assetChoices(packID, initial, onChanged) {
      const mounts = new Map();
      const collator = new Intl.Collator(ui.locale, {numeric:true});
      let packs, slots, saving = false;

      function read(catalog) {
        packs = new Map((catalog.packs || []).filter(pack => !pack.error).map(pack => [pack.id, pack]));
        slots = new Map((catalog.slots || []).map(slot => [slot.id, slot]));
        for (const mount of mounts.values()) mount.refresh();
      }
      read(initial);

      async function change(action, pack, slot, progress, actions) {
        if (saving) return;
        saving = true;
        const version = attempt;
        actions.append(progress);
        progress.hidden = false;
        content.setAttribute("aria-busy", "true");
        window.workshopModPacks.markMutations(content, true);
        try {
          await requests.write(async () => {
            const next = await request("mods/activation", window.workshopModPacks.jsonOptions({action, pack, slot}));
            onChanged(next);
            if (dialog.open) choices?.read(next);
          });
        } catch (error) {
          window.workshopFeedback?.show(status, error, {
            operation: "Change mod asset",
            messageKey: error.uiKey || (error.code === "AR_NETWORK" ? "builder.feedback.connection" : "builder.mods.asset_change_failed"),
          });
        } finally {
          saving = false;
          progress.hidden = true;
          if (attempt === version) {
            content.removeAttribute("aria-busy");
            window.workshopModPacks.markMutations(content, false);
          }
        }
      }

      function sourcePicker(id, name, progress) {
        const controls = document.createElement("div");
        controls.className = "mod-source-controls";
        const big = document.body.dataset.bigScreen === "true";
        const trigger = document.createElement(big ? "button" : "select");
        trigger.dataset.modMutation = "";
        ui.attribute(trigger, "aria-label", "builder.mods.source_for", {target:name});
        controls.append(trigger);
        const apply = big ? null : translatedElement("button", "builder.mods.apply_source");
        let staged = "", previousWinner, options = [];

        function saveSource(provider) {
          const slot = slots.get(id);
          if (!saving && slot?.candidates.includes(provider) && (slot.needsChoice || provider !== slot.winner)) {
            return change("select", provider, id, progress, controls);
          }
        }
        function openChoices() {
          if (saving || sourceDialog.open) return;
          const slot = slots.get(id);
          ui.set(sourceTitle, "builder.mods.source_for", {target:name});
          const rows = options.map(({provider, label}) => {
            const row = document.createElement("button");
            row.type = "button";
            row.className = "secondary";
            const current = provider === slot.winner;
            row.setAttribute("aria-pressed", String(current));
            row.textContent = current && slot.needsChoice ? ui.text("builder.mods.keep_source", {name:label}) : label;
            row.addEventListener("click", () => {
              sourceDialog.close();
              if (apply) {
                trigger.value = provider;
                updateApply();
              }
              return saveSource(provider);
            });
            return row;
          });
          sourceList.replaceChildren(...rows);
          trigger.focus();
          sourceDialog.showModal();
          (rows.find(row => row.getAttribute("aria-pressed") === "true") || rows[0]).focus();
          fitSourceList();
          window.addEventListener("resize", fitSourceList);
        }
        function updateApply() {
          const slot = slots.get(id);
          staged = trigger.value;
          apply.disabled = !staged || (!slot.needsChoice && staged === slot.winner);
        }
        if (big) {
          trigger.type = "button";
          trigger.className = "secondary";
          trigger.setAttribute("aria-haspopup", "dialog");
          ui.set(trigger, "builder.mods.choose_source");
          trigger.addEventListener("click", openChoices);
        } else {
          trigger.dataset.padPicker = "";
          trigger.addEventListener("workshop:pick", openChoices);
          trigger.addEventListener("change", () => {
            if (saving) trigger.value = staged;
            else updateApply();
          });
          apply.type = "button";
          apply.className = "secondary";
          apply.dataset.modMutation = "";
          apply.addEventListener("click", () => saveSource(trigger.value));
          controls.append(apply);
        }
        function update(slot) {
          const counts = new Map();
          options = slot.candidates.map(provider => {
            const label = packs.get(provider)?.name || provider;
            counts.set(label, (counts.get(label) || 0) + 1);
            return {provider, label};
          });
          for (const entry of options) {
            if (counts.get(entry.label) > 1) entry.label += " · " + entry.provider;
          }
          options.sort((a,b) => collator.compare(a.label, b.label));
          if (big) return;
          const items = options.map(({provider, label}) => {
            const option = document.createElement("option");
            option.value = provider;
            option.textContent = label;
            return option;
          });
          if (!slot.winner) {
            const prompt = translatedElement("option", "builder.mods.choose_source");
            prompt.value = "";
            prompt.disabled = true;
            items.unshift(prompt);
          }
          trigger.replaceChildren(...items);
          trigger.value = staged !== previousWinner && slot.candidates.includes(staged) ? staged : slot.winner || "";
          previousWinner = slot.winner;
          const hadFocus = document.activeElement === apply;
          updateApply();
          if (hadFocus && apply.disabled) trigger.focus();
        }
        return {controls, trigger, update};
      }

      function add(card, heading, id, name) {
        const choicesRow = document.createElement("div"), description = document.createElement("div");
        let picker;
        const include = document.createElement("label"), check = document.createElement("input");
        check.type = "checkbox";
        check.dataset.modMutation = "";
        check.dataset.choice = "include";
        const label = document.createElement("span");
        include.className = "mod-asset-include";
        include.append(check, label);
        const disabled = translatedElement("span", "builder.mods.disabled");
        disabled.className = "muted";
        const progress = translatedElement("span", "builder.assets.saving");
        progress.className = "muted";
        progress.setAttribute("role", "status");
        progress.hidden = true;
        choicesRow.className = "mod-asset-choice";
        description.className = "mod-source-status";
        const state = document.createElement("strong"), source = document.createElement("span");
        description.append(state, source);
        choicesRow.append(description);
        heading.append(include, disabled);
        card.append(choicesRow);
        check.addEventListener("change", () => {
          const excluded = !!packs.get(packID)?.excludedTargets?.includes(id);
          check.checked = !excluded;
          return change(excluded ? "include-target" : "exclude-target", packID, id, progress, heading);
        });
        check.addEventListener("keydown", event => {
          if (event.key === "Enter") {
            event.preventDefault();
            if (!event.repeat) check.click();
          }
        });

        function refresh() {
          const pack = packs.get(packID), slot = slots.get(id);
          const candidates = slot?.candidates || [];
          const excluded = !!pack?.excludedTargets?.includes(id);
          const shared = candidates.length > 1 && candidates.includes(packID);
          check.checked = !excluded;
          ui.set(label, excluded ? "builder.mods.asset_excluded_short" : "builder.mods.asset_included_short");
          check.setAttribute("aria-label", label.textContent + " · " + name);
          include.hidden = !pack?.enabled || !(pack.targets?.length > 1 || excluded);
          disabled.hidden = !!pack?.enabled;
          choicesRow.hidden = !shared;
          if (!shared) {
            card.removeAttribute("data-source-state");
            if (picker) {
              choicesRow.replaceChildren(description);
              picker = null;
            }
            return;
          }

          const current = packs.get(slot.winner);
          const active = slot.winner === packID;
          card.dataset.sourceState = slot.needsChoice ? "conflict" : active ? "active" : "alternate";
          ui.set(state, slot.needsChoice ? "builder.mods.conflict_badge" : active ? "builder.mods.active_pack" : "builder.mods.overridden");
          const sourceName = current ? ui.text("builder.mods.asset_source", {name: current.name || current.id}) : ui.text("builder.mods.choose_pack");
          const count = ui.text("builder.mods.asset_sources", {count: candidates.length});
          source.textContent = active && !slot.needsChoice ? count : sourceName + " · " + count;
          if (!picker) {
            picker = sourcePicker(id, name, progress);
            choicesRow.append(picker.controls);
          }
          picker.update(slot);
        }
        mounts.set(id, {refresh, focusTarget:() => picker?.trigger});
        refresh();
      }
      return {add, focusTarget:target => mounts.get(target)?.focusTarget(), read};
    }

    function assetHeading(name) {
      const heading = document.createElement("div"), title = document.createElement("strong");
      heading.className = "mod-asset-heading";
      title.textContent = name;
      heading.append(title);
      return heading;
    }

    function music(pack, choices, trackIndex) {
      const list = document.createElement("ul");
      list.className = "mod-preview-tracks";
      content.append(list);
      let first, selected;
      for (const [index, track] of (pack.tracks || []).entries()) {
        const id = track.region ? track.track + "-" + track.region : track.track;
        const name = window.workshopModPacks.targetName(id), url = pack.previews?.[index];
        const row = document.createElement("li");
        row.className = "mod-preview-asset";
        const heading = assetHeading(name);
        list.append(row);
        if (!url) {
          row.append(heading, translatedElement("small", "builder.big_screen.no_audio"));
        } else {
          const play = window.workshopAudioTransport(content, row, {url, name, heading}, error => {
            if (dialog.open) window.workshopFeedback?.show(status, error, {operation: "Play mod preview"});
          });
          first ||= play;
          if (index === trackIndex) selected = play;
        }
        choices?.add(row, heading, id, name);
      }
      if (!list.children.length) ui.set(status, "builder.big_screen.no_audio");
      return selected || first || close;
    }

    function manualPage(number) {
      page = number;
      ui.attribute(image, "alt", "builder.mods.manual_page", {page, total: manual.manualPages});
      ui.set(status, "builder.mods.manual_preview_status", {page, total: manual.manualPages});
      image.src = manual.previews[0] + "?" + new URLSearchParams({page});
      prev.disabled = page <= 1;
      next.disabled = page >= manual.manualPages;
      if (document.activeElement === prev && prev.disabled) next.focus();
      if (document.activeElement === next && next.disabled) prev.focus();
    }

    function translatedElement(tag, key, args) {
      const node = document.createElement(tag);
      ui.set(node, key, args);
      return node;
    }

    function languageCoverage(coverage) {
      const section = document.createElement("section");
      section.className = "language-preview-coverage";
      const heading = translatedElement("h3", "builder.mods.language_text_coverage");
      const percent = document.createElement("strong");
      percent.className = "language-preview-percentage";
      percent.textContent = ui.number(coverage.provided / coverage.total, {style: "percent", maximumFractionDigits: 1});
      const progress = document.createElement("progress");
      progress.max = coverage.total;
      progress.value = coverage.provided;
      ui.attribute(progress, "aria-label", "builder.mods.language_text_coverage");
      section.append(heading, percent, progress, translatedElement("p", "builder.mods.language_entries_count", coverage));
      const fallback = coverage.total - coverage.provided;
      if (fallback) section.append(translatedElement("p", "builder.mods.language_entries_fallback", {count: fallback}));

      const breakdown = document.createElement("dl");
      breakdown.className = "language-preview-breakdown";
      for (const surface of coverage.surfaces) {
        const label = translatedElement("dt", "builder.coverage.surface." + surface.surface);
        breakdown.append(label, translatedElement("dd", "builder.coverage.supplied", surface));
      }
      section.append(breakdown);
      return section;
    }

    function languageDetails(metadata, version, required) {
      const section = document.createElement("section");
      section.className = "language-preview-details";
      section.append(translatedElement("h3", "builder.mods.language_pack_details"));
      const details = document.createElement("dl");
      for (const [key, value] of [
        ["builder.language.autonym", [metadata.autonym, metadata.locale].filter(Boolean).join(" · ")],
        ["builder.mods.author", metadata.author],
        ["builder.mods.version", version],
        ["builder.mods.license", metadata.license],
        ["builder.editor.package_id", metadata.id],
      ]) {
        if (!value) continue;
        const field = document.createElement("dd");
        field.dir = "auto";
        field.textContent = value;
        details.append(translatedElement("dt", key), field);
      }
      section.append(details);
      if (required) section.append(translatedElement("p", "builder.mods.required_language"));
      if (metadata.description) {
        const description = document.createElement("p");
        description.dir = "auto";
        description.textContent = metadata.description;
        section.append(description);
      }
      return section;
    }

    function languageSamples(samples, metadata) {
      if (!samples?.length) return;
      const examples = document.createElement("details");
      examples.className = "language-preview-samples";
      examples.append(translatedElement("summary", "builder.mods.language_text_samples"));
      for (const sample of samples) {
        const heading = document.createElement("h3");
        heading.textContent = sample.key ? ui.text(sample.key, {}, sample.label) : sample.label;
        const quote = document.createElement("blockquote");
        quote.lang = metadata.locale;
        quote.dir = metadata.direction === "rtl" ? "rtl" : "ltr";
        quote.textContent = sample.text;
        examples.append(heading, quote);
      }
      content.append(examples);
    }

    async function language(pack, release, version) {
      ui.set(status, "builder.big_screen.loading");
      try {
        const query = new URLSearchParams({mod: pack.modId || "", release: release || "", key: pack.key});
        const result = await request("mods/language-preview?" + query);
        if (attempt !== version || !dialog.open) return;
        ui.unbind(status);
        status.textContent = "";
        const overview = document.createElement("div");
        overview.className = "language-preview-overview";
        overview.append(languageCoverage(result.coverage), languageDetails(result.metadata, result.version, pack.required));
        content.append(overview);
        languageSamples(result.samples, result.metadata);
      } catch (error) {
        if (attempt === version && dialog.open) {
          ui.unbind(status);
          status.textContent = "";
          window.workshopFeedback?.show(status, error, {operation: "Preview language mod"});
        }
      }
    }

    prev.addEventListener("click", () => manualPage(page - 1));
    next.addEventListener("click", () => manualPage(page + 1));
    close.addEventListener("click", () => dialog.close());
    save.addEventListener("click", async () => {
      const archive = activeArchive;
      if (!archive || save.getAttribute("aria-disabled") === "true") return;
      save.setAttribute("aria-disabled", "true");
      try {
        await window.workshopSaveArchive(archive);
      } catch (error) {
        if (dialog.open && activeArchive === archive) window.workshopFeedback?.show(save, error, {operation: "Save mod archive"});
      } finally {
        if (activeArchive === archive) save.removeAttribute("aria-disabled");
      }
    });
    image.addEventListener("error", () => {
      if (dialog.open) window.workshopFeedback?.show(status,
        new Error(ui.text("builder.mods.preview_unavailable")), {operation: "Preview mod"});
    });
    dialog.addEventListener("close", () => {
      if (sourceDialog.open) sourceDialog.close();
      attempt++;
      content.removeAttribute("aria-busy");
      manual = null;
      choices = null;
      activeArchive = null;
      window.workshopStopAudio(content);
      image.removeAttribute("src");
      content.replaceChildren();
    });
    return ({pack, archive = pack, type, release, name, catalog, onChanged, trackIndex, target}) => {
      if (dialog.open) return;
      const version = ++attempt;
      title.textContent = name || pack.installedName || pack.name || pack.filename;
      ui.unbind(status);
      status.textContent = "";
      activeArchive = archive.archiveURL && document.body.dataset.bigScreen !== "true" ?
        {url: archive.archiveURL, name: archive.filename} : null;
      save.hidden = !activeArchive;
      save.removeAttribute("aria-disabled");
      const hasPages = type === "actraiser-manual" && pack.manualPages > 0 && !!pack.previews?.[0];
      prev.hidden = next.hidden = !hasPages;
      dialog.showModal();
      if (pack.bundled) content.append(translatedElement("p", "builder.mods.bundled_fixed"));
      choices = catalog && onChanged && type !== "actraiser-language" ? assetChoices(pack.id, catalog, onChanged) : null;
      let focus = close;
      let media = content;
      if (choices && (type === "actraiser-title" || type === "actraiser-manual") && pack.targets?.length) {
        const name = window.workshopModPacks.modSlotName({type});
        const heading = assetHeading(name);
        media = document.createElement("section");
        media.className = "mod-preview-asset";
        media.append(heading);
        content.append(media);
        choices.add(media, heading, pack.targets[0], name);
      }
      if (type === "actraiser-music") {
        focus = music(pack, choices, trackIndex);
      } else if (hasPages) {
        manual = pack;
        media.append(image);
        manualPage(1);
      } else if (type === "actraiser-title" && pack.previews?.[0]) {
        ui.set(status, "builder.mods.title_surfaces");
        ui.attribute(image, "alt", "builder.mods.title_preview", {name: title.textContent});
        image.src = pack.previews[0];
        media.append(image);
      } else if (type === "actraiser-language") {
        void language(pack, release, version);
      } else ui.set(status, "builder.mods.preview_unavailable");
      (choices?.focusTarget(target) || focus).focus();
    };
  };
})();
