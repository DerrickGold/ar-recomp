(() => {
  "use strict";

  window.workshopPublishing = (request, requests, onInstalled) => {
    const ui = window.workshopI18n;
    const {musicType, titleType, manualType, jsonOptions, targetName} = window.workshopModPacks;
    const key = name => "builder.mods." + name;
    const exportDialog = document.getElementById("asset-pack-export");
    const exportForm = document.getElementById("asset-pack-export-form");
    const metadata = [...exportForm.querySelectorAll(".pack-metadata input")];
    const selections = document.getElementById("asset-pack-export-tracks");
    const exportStatus = document.getElementById("asset-pack-export-status");
    const exportSubmit = document.getElementById("asset-pack-export-submit");
    const exportInstall = document.getElementById("asset-pack-export-install");
    const artworkGuide = document.getElementById("asset-pack-artwork-guide");
    const artworkStatus = document.getElementById("asset-pack-artwork-status");
    const exportClose = document.getElementById("asset-pack-export-close");
    const discard = document.getElementById("asset-pack-export-discard");
    const trackCount = document.getElementById("asset-pack-track-count");
    const exportHeading = document.getElementById("asset-pack-export-heading");
    const musicFields = document.getElementById("asset-pack-export-music");
    const titleField = document.getElementById("asset-pack-export-image");
    const artworkInput = document.getElementById("asset-pack-artwork");
    const manualField = document.getElementById("asset-pack-export-pdf");
    const manualInput = document.getElementById("asset-pack-pdf");
    const manualStatus = document.getElementById("asset-pack-manual-status");
    let exportType = musicType, exportBusy = false, tracksLoaded = false, exportSequence = 0;
    let draftKey;
    const drafts = new Map();
    const fileChecks = new WeakMap();

    function currentDraft() {
      if (!draftKey) return;
      const values = metadata.map(field => field.value);
      let targets = drafts.get(draftKey)?.targets;
      if (exportType === musicType && tracksLoaded) {
        const boxes = [...selections.querySelectorAll("input")].filter(box => !box.disabled);
        targets = boxes.some(box => box.checked !== box.defaultChecked)
          ? new Set(boxes.filter(box => box.checked).map(box => box.value)) : undefined;
      }
      const file = exportType === titleType ? artworkInput.files[0] :
        exportType === manualType ? manualInput.files[0] : undefined;
      if (targets || file || metadata.some((field, i) => values[i] !== field.defaultValue))
        return {values, targets};
    }

    function hasEdits() {
      return !!currentDraft() || [...drafts.keys()].some(key => key !== draftKey);
    }

    function updateExportButtons() {
      const input = exportType === titleType ? artworkInput : manualInput;
      const check = fileChecks.get(input);
      const count = selections.querySelectorAll("input:checked").length;
      const valid = exportType === musicType ? tracksLoaded && count > 0 :
        check?.valid && check.file === input.files[0];
      for (const field of metadata) field.readOnly = exportBusy;
      musicFields.disabled = exportBusy || exportType !== musicType;
      artworkInput.disabled = exportBusy || exportType !== titleType;
      manualInput.disabled = exportBusy || exportType !== manualType;
      exportClose.disabled = exportBusy;
      discard.hidden = !currentDraft();
      discard.disabled = exportBusy;
      exportSubmit.disabled = exportInstall.disabled = exportBusy || !valid;
      trackCount.hidden = exportType !== musicType || !tracksLoaded;
      if (!trackCount.hidden) ui.set(trackCount, key(count ? "tracks_selected" : "choose_export_tracks"), {count});
    }

    async function validateExportFile(input, status, kind) {
      const file = input.files[0];
      const check = {file, valid: false};
      fileChecks.set(input, check);
      updateExportButtons();
      status.hidden = !file;
      status.removeAttribute("data-invalid");
      if (!file) return;
      ui.set(status, key(kind === "title" ? "artwork_checking" : "manual_checking"));
      try {
        const result = await request("mods/validate-" + kind, {method: "POST", body: file});
        if (fileChecks.get(input) !== check) return;
        check.valid = result.valid;
        if (!check.valid) status.dataset.invalid = "true";
        ui.set(status, key(kind === "title" ? "artwork_valid" : check.valid ? "manual_valid" : "manual_incompatible"), result);
      } catch (error) {
        if (fileChecks.get(input) !== check) return;
        status.dataset.invalid = "true";
        ui.unbind(status);
        status.textContent = error.message;
      } finally {
        if (fileChecks.get(input) === check) updateExportButtons();
      }
    }

    artworkInput.addEventListener("change", () => validateExportFile(artworkInput, artworkStatus, "title"));
    manualInput.addEventListener("change", () => validateExportFile(manualInput, manualStatus, "manual"));
    selections.addEventListener("change", updateExportButtons);
    exportForm.addEventListener("input", updateExportButtons);

    async function openExport(type, trackID) {
      if (exportDialog.open || exportBusy) return;
      const previous = currentDraft();
      if (previous) drafts.set(draftKey, previous);
      else drafts.delete(draftKey);
      draftKey = type === musicType && trackID ? type + ":" + trackID : type;
      const draft = drafts.get(draftKey);
      metadata.forEach((field, index) => { field.value = draft?.values[index] ?? field.defaultValue; });
      const sequence = ++exportSequence;
      exportType = type;
      const titlePack = exportType === titleType;
      const manualPack = exportType === manualType;
      ui.set(exportHeading, key(titlePack ? "export_title" : manualPack ? "export_manual" : "pack_music"));
      musicFields.hidden = musicFields.disabled = exportType !== musicType;
      titleField.hidden = artworkInput.disabled = artworkGuide.hidden = !titlePack;
      if (!titlePack) artworkStatus.hidden = true;
      if (!manualPack) manualStatus.hidden = true;
      manualField.hidden = manualInput.disabled = !manualPack;
      exportStatus.hidden = true;
      tracksLoaded = false;
      exportDialog.showModal();
      if (exportType !== musicType) {
        if (titlePack) validateExportFile(artworkInput, artworkStatus, "title");
        else validateExportFile(manualInput, manualStatus, "manual");
        return;
      }
      exportStatus.hidden = false;
      ui.set(exportStatus, "builder.big_screen.loading");
      selections.replaceChildren();
      updateExportButtons();
      try {
        const tracks = await request("mods/export-draft", {cache: "no-store"});
        if (sequence !== exportSequence || !exportDialog.open) return;
        for (const track of tracks) {
          const label = document.createElement("label");
          const selection = document.createElement("input");
          selection.type = "checkbox";
          selection.name = "target";
          selection.value = track.id;
          selection.disabled = !!track.error;
          selection.defaultChecked = !selection.disabled && !!trackID && track.track === trackID;
          selection.checked = draft?.targets ? !selection.disabled && draft.targets.has(track.id) : selection.defaultChecked;
          label.append(selection, document.createTextNode(track.error ? track.id + " · " + track.error : targetName(track.id)));
          selections.append(label);
        }
        tracksLoaded = tracks.some(track => !track.error);
        updateExportButtons();
        exportStatus.hidden = tracksLoaded;
        if (!tracksLoaded) ui.set(exportStatus, key("no_export"));
      } catch (error) {
        if (sequence !== exportSequence || !exportDialog.open) return;
        exportStatus.hidden = true;
        window.workshopFeedback?.show(exportStatus, error, {operation: "List exportable tracks"});
      }
    }

    function clearDraftAndClose() {
      drafts.delete(draftKey);
      draftKey = undefined;
      if (exportType !== musicType) {
        const input = exportType === titleType ? artworkInput : manualInput;
        input.value = "";
        fileChecks.delete(input);
        window.workshopFileInputs?.refresh(input);
      }
      exportDialog.close();
    }

    exportClose.addEventListener("click", () => exportDialog.close());
    discard.addEventListener("click", async () => {
      if (!exportBusy && await window.workshopConfirm(key("discard_draft"), key("discard_pack_prompt")))
        clearDraftAndClose();
    });
    exportDialog.addEventListener("cancel", event => { if (exportBusy) event.preventDefault(); });
    exportForm.addEventListener("submit", async event => {
      event.preventDefault();
      if (exportSubmit.disabled) return;
      const install = event.submitter === exportInstall;
      const form = new FormData(exportForm);
      const payload = Object.fromEntries(form);
      delete payload.target;
      payload.targets = form.getAll("target");
      exportBusy = true;
      updateExportButtons();
      const submitter = install ? exportInstall : exportSubmit;
      submitter.setAttribute("aria-busy", "true");
      ui.set(submitter, key(install ? "installing" : "export_pending"));
      exportStatus.classList.add("sr-only");
      exportStatus.hidden = false;
      ui.set(exportStatus, key(install ? "installing_named" : "exporting_named"), {name: payload.name});
      try {
        const endpoint = exportType === titleType ? "mods/export-title" :
          exportType === manualType ? "mods/export-manual" : "mods/export-draft";
        const options = exportType === musicType ? jsonOptions(payload) : {method: "POST", body: form};
        if (install) {
          const result = await requests.write(() => request(endpoint + "?install=true", options));
          clearDraftAndClose();
          await onInstalled(result);
          return;
        }
        const result = await request(endpoint, options);
        const location = await window.workshopSaveArchive(result);
        if (location !== "cancelled") clearDraftAndClose();
      } catch (error) {
        window.workshopFeedback?.show(exportStatus, error, {operation: install ? "Install mod pack" : "Export mod pack"});
      } finally {
        exportStatus.hidden = true;
        exportStatus.classList.remove("sr-only");
        exportBusy = false;
        submitter.removeAttribute("aria-busy");
        ui.set(submitter, install ? "builder.language.install_game" : key("download"));
        updateExportButtons();
        if (exportDialog.open && document.activeElement === document.body) submitter.focus();
      }
    });
    return {
      openMusic: trackID => openExport(musicType, trackID),
      openTitle: () => openExport(titleType),
      openManual: () => openExport(manualType),
      hasEdits,
    };
  };
})();
