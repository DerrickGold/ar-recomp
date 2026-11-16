(() => {
  "use strict";

  window.workshopROMChecklist = (request, browse, onImported, onUploadsChanged) => {
    const rows = document.querySelector("#rom-checklist-rows");
    const report = document.querySelector("#rom-checklist-feedback");
    const unavailableNote = document.querySelector("#rom-checklist-unavailable");
    const importButton = document.querySelector("#rom-import-selected");
    const ui = window.workshopI18n;
    let inventory = [], unavailable = [], uploaded = [], unavailableUploads = [], operationStatus = null, inventoryError = null;
    let loading = 0, inspecting = 0, canPlay = false, busy = false, importing = false;

    function browseEntry(entry) {
      browse({...entry, label: ui.text("builder.roms.name_" + entry.id.replaceAll("-", "_"))});
    }

    function updateActions() {
      for (const row of rows.querySelectorAll("button")) row.disabled = busy || importing;
      const pending = inventory.some(entry => entry.selected && entry.id !== "us" &&
        (entry.id === "ar2-us" ? entry.world !== "installed" || entry.master !== "installed" :
          entry.media !== "installed" || !entry.textReference));
      importButton.hidden = !canPlay || !pending;
      importButton.disabled = busy || importing;
    }

    function render() {
      const focused = document.activeElement?.dataset.romSlot;
      const buttons = inventory.map(entry => {
        const label = ui.text("builder.roms.name_" + entry.id.replaceAll("-", "_"));
        const purpose = entry.id === "us" ? "main" : entry.id === "ar2-us" ? "sequel" : "region";
        const button = document.createElement("button");
        button.type = "button";
        button.className = "secondary rom-slot";
        button.id = "rom-slot-" + entry.id;
        button.dataset.romSlot = entry.id;
        const title = document.createElement("strong");
        title.textContent = label;
        const help = document.createElement("small");
        help.textContent = ui.text("builder.roms.purpose_" + purpose);
        const status = document.createElement("small");
        status.className = "rom-slot-status";
        const states = [];
        const uploads = uploaded.filter(file => file.id === entry.id);
        for (const file of uploads) states.push(ui.text("builder.roms.selected_for_build", {name: file.name}));
        if (entry.selected) states.push(ui.text(entry.origin === "launcher" ?
          "builder.roms.from_launcher" : "builder.roms.selected", {name: entry.selected}));
        if (entry.found && entry.found !== entry.selected) states.push(ui.text("builder.roms.found", {name: entry.found}));
        if (entry.saved) states.push(ui.text("builder.roms.saved"));
        if (entry.media === "installed" && entry.id !== "us") states.push(ui.text("builder.roms.media_ready"));
        if (entry.textReference) states.push(ui.text("builder.roms.text_ready"));
        if (entry.master === "installed") states.push(ui.text("builder.roms.master_ready"));
        if (entry.world === "installed") states.push(ui.text("builder.roms.world_ready"));
        const invalidImport = [entry.media, entry.master, entry.world].includes("invalid");
        if (invalidImport) states.push(ui.text("builder.roms.invalid_import"));
        status.textContent = states.length ? states.join(" · ") :
          ui.text(entry.id === "us" ? "builder.roms.required" : "builder.roms.optional");
        button.dataset.available = String(states.length > 0);
        button.dataset.invalid = String(invalidImport);
        button.append(title, help, status);
        button.addEventListener("click", () => browseEntry(entry));
        return button;
      });
      rows.replaceChildren(...buttons);
      updateActions();
      if (focused) buttons.find(button => button.dataset.romSlot === focused && !button.disabled)?.focus({preventScroll: true});
      const invalid = [...unavailable, ...unavailableUploads];
      unavailableNote.hidden = !invalid.length;
      if (invalid.length) ui.set(unavailableNote, "builder.roms.unavailable", {names: invalid.join(", ")});
      const feedback = inventoryError || operationStatus;
      report.hidden = !feedback;
      if (feedback) ui.set(report, feedback.key, feedback.values);
    }

    async function load() {
      const requestID = ++loading;
      try {
        const data = await request("rom-checklist");
        if (requestID !== loading) return;
        inventory = data.roms || [];
        unavailable = data.unavailable || [];
        inventoryError = null;
      } catch (error) {
        if (requestID !== loading) return;
        inventoryError = {key: "builder.request_failed", values: {detail: error.message}};
      }
      render();
    }

    importButton.addEventListener("click", async () => {
      if (importing || busy) return;
      importing = true;
      updateActions();
      try {
        await request("rom-import-selected", {method: "POST"});
        operationStatus = {key: "builder.roms.imported"};
        await onImported();
      } catch (error) {
        operationStatus = {key: "builder.request_failed", values: {detail: error.message}};
      } finally {
        importing = false;
        await load();
        if (importButton.hidden && (document.activeElement === importButton || document.activeElement === document.body)) {
          rows.querySelector("button")?.focus();
        }
      }
    });
    document.addEventListener("workshop:language", render);
    document.addEventListener("workshop:content-imported", () => { if (!importing) load(); });
    return {
      load,
      chooseCore() {
        browseEntry(inventory.find(entry => entry.id === "us") || {id: "us"});
      },
      async inspect(files) {
        const requestID = ++inspecting;
        uploaded = [];
        unavailableUploads = [];
        onUploadsChanged({checking: files.length !== 0});
        render();
        if (!files.length) {
          return;
        }
        const body = new FormData();
        for (const file of files) body.append("rom", file);
        try {
          const data = await request("rom-inspect", {method: "POST", body});
          if (requestID !== inspecting) return;
          uploaded = data.selected;
          unavailableUploads = data.unavailable;
        } catch (error) {
          if (requestID !== inspecting) return;
          unavailableUploads = Array.from(files, file => file.name);
        }
        render();
        onUploadsChanged({
          core: uploaded.find(file => file.id === "us")?.name,
          invalid: unavailableUploads.length !== 0,
        });
      },
      availability(play, locked) { canPlay = play; busy = locked; updateActions(); },
    };
  };
})();
