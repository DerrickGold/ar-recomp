(() => {
  "use strict";

  window.workshopModPreview = request => {
    const ui = window.workshopI18n;
    const dialog = document.getElementById("mod-preview-dialog");
    const title = document.getElementById("mod-preview-title");
    const status = document.getElementById("mod-preview-status");
    const content = document.getElementById("mod-preview-content");
    const close = document.getElementById("mod-preview-close");
    const save = document.getElementById("mod-preview-save");
    const prev = document.getElementById("mod-preview-prev");
    const next = document.getElementById("mod-preview-next");
    const image = document.createElement("img");
    let manual, page, activeArchive, attempt = 0;

    function music(pack, slots, trackIndex) {
      const bySlot = new Map(slots.map(slot => [slot.id, slot]));
      const list = document.createElement("ul");
      list.className = "mod-preview-tracks";
      content.append(list);
      let first, selected;
      for (const [index, track] of (pack.tracks || []).entries()) {
        const id = track.region ? track.track + "-" + track.region : track.track;
        const name = window.workshopModPacks.targetName(id), url = pack.previews?.[index];
        const row = document.createElement("li");
        list.append(row);
        if (!url) {
          const label = document.createElement("strong");
          const detail = document.createElement("small");
          label.textContent = name;
          ui.set(detail, "builder.big_screen.no_audio");
          row.append(label, detail);
          continue;
        }
        const slot = bySlot.get(id);
        const description = !pack.enabled ? "builder.mods.disabled" : slot ?
          slot.winner === pack.id ? "builder.mods.active" :
            slot.winner ? "builder.mods.alternate" : "builder.mods.choose_pack" : "";
        const play = window.workshopAudioTransport(content, row, {url, name, description}, error => {
          if (dialog.open) window.workshopFeedback?.show(status, error, {operation: "Play mod preview"});
        });
        first ||= play;
        if (index === trackIndex) selected = play;
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
      attempt++;
      manual = null;
      activeArchive = null;
      window.workshopStopAudio(content);
      image.removeAttribute("src");
      content.replaceChildren();
    });
    return ({pack, archive = pack, type, release, name, slots = [], trackIndex}) => {
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
      let focus = close;
      if (type === "actraiser-music") {
        focus = music(pack, slots, trackIndex);
      } else if (hasPages) {
        manual = pack;
        content.append(image);
        manualPage(1);
      } else if (type === "actraiser-title" && pack.previews?.[0]) {
        ui.set(status, "builder.mods.title_surfaces");
        ui.attribute(image, "alt", "builder.mods.title_preview", {name: title.textContent});
        image.src = pack.previews[0];
        content.append(image);
      } else if (type === "actraiser-language") {
        void language(pack, release, version);
      } else ui.set(status, "builder.mods.preview_unavailable");
      focus.focus();
    };
  };
})();
