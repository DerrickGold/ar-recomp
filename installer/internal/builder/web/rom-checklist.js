(() => {
  "use strict";

  window.workshopROMChecklist = (request, browse, {onImported, onSelectionChanged, onSelecting}) => {
    const rows = document.querySelector("#rom-checklist-rows");
    const report = document.querySelector("#rom-checklist-feedback");
    const unavailableNote = document.querySelector("#rom-checklist-unavailable");
    const importButton = document.querySelector("#rom-import-selected");
    const picker = document.querySelector("#rom-picker");
    const checksumsButton = document.getElementById("rom-checksums-open");
    const checksumsDialog = document.getElementById("rom-checksums-dialog");
    const ui = window.workshopI18n;
    let inventory = [], unavailable = [], operationStatus = null;
    let loading = 0, canPlay = false, busy = false, importing = false, selecting = false, slot = "";
    const bigScreen = () => document.body.dataset.bigScreen === "true";

    const releaseLabel = id => "builder.roms.name_" + id.replaceAll("-", "_");

    checksumsButton.addEventListener("click", () => {
      const list = document.getElementById("rom-checksums-list");
      list.replaceChildren();
      for (const entry of inventory) {
        const name = document.createElement("dt");
        ui.set(name, releaseLabel(entry.id));
        const value = document.createElement("dd");
        const digest = document.createElement("code");
        digest.textContent = entry.sha256;
        value.append(digest);
        list.append(name, value);
      }
      checksumsDialog.showModal();
      document.getElementById("rom-checksums-content").scrollTop = 0;
    });

    function browseEntry(entry) {
      if (busy || importing || selecting) return;
      if (bigScreen()) {
        browse({...entry, label: ui.text(releaseLabel(entry.id))});
      } else {
        slot = entry.id;
        picker.value = "";
        picker.click();
      }
    }

    async function select(options) {
      if (busy || importing || selecting) return;
      selecting = true;
      report.hidden = false;
      ui.set(report, "builder.roms.checking_files");
      onSelecting();
      try {
        const result = await request("rom-selection", options);
        operationStatus = null;
        await Promise.all([load(), onSelectionChanged()]);
        if (result.unavailable?.length) {
          const args = {names: result.unavailable.join(", ")};
          fail(new Error(ui.text("builder.roms.unavailable", args)), "Select ROMs");
        }
        return result;
      } finally {
        selecting = false;
        onSelecting();
        render();
      }
    }

    async function selectAndReport(action, id) {
      try {
        await action();
      } catch (error) {
        fail(error, "Select ROMs");
      } finally {
        if (id && document.activeElement === document.body) document.getElementById("rom-slot-" + id)?.focus();
      }
    }

    const selectLocal = values => select({
      method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(values),
    });

    function fail(error, operation) {
      operationStatus = null;
      window.workshopFeedback?.show(report, error, {operation});
    }

    function upload(files, id = "") {
      if (!files.length) return;
      const body = new FormData();
      if (id) body.set("id", id);
      for (const file of files) body.append("rom", file);
      return selectAndReport(() => select({method: "POST", body}), id);
    }
    picker.addEventListener("change", () => upload(picker.files, slot));

    function updateActions() {
      for (const control of rows.querySelectorAll("button")) {
        control.disabled = busy || importing;
        if (selecting) control.setAttribute("aria-disabled", "true");
        else control.removeAttribute("aria-disabled");
      }
      const pending = inventory.some(entry => entry.selected && entry.id !== "us" &&
        (entry.id === "ar2-us" ? entry.world !== "installed" || entry.master !== "installed" :
          entry.media !== "installed" || !entry.languagePack));
      importButton.hidden = !canPlay || !pending;
      importButton.disabled = busy || importing || selecting;
    }

    function render() {
      const focused = document.activeElement;
      const restoreFocus = focused && rows.contains(focused);
      const browseID = restoreFocus && focused.closest(".rom-slot-card")?.querySelector(".rom-slot").id;
      const cards = inventory.map(entry => {
        const label = ui.text(releaseLabel(entry.id));
        const purpose = entry.id === "us" ? "main" : entry.id === "ar2-us" ? "sequel" : "region";
        const card = document.createElement("div");
        card.className = "rom-slot-card";
        card.dataset.bigRow = "";
        const button = document.createElement("button");
        button.type = "button";
        button.className = "rom-slot";
        button.id = "rom-slot-" + entry.id;
        button.dataset.romAction = "browse";
        const title = document.createElement("strong");
        title.textContent = label;
        const help = document.createElement("small");
        help.textContent = ui.text("builder.roms.purpose_" + purpose);
        const status = document.createElement("small");
        status.className = "rom-slot-status";
        const states = [];
        if (entry.selected) states.push(ui.text(entry.origin === "launcher" ?
          "builder.roms.from_launcher" : "builder.roms.selected", {name: entry.selected}));
        if (entry.found && entry.found !== entry.selected) states.push(ui.text("builder.roms.found", {name: entry.found}));
        if (entry.saved) states.push(ui.text("builder.roms.saved"));
        if (entry.media === "installed" && entry.id !== "us") states.push(ui.text("builder.roms.media_ready"));
        if (entry.languagePack) states.push(ui.text("builder.roms.language_ready"));
        else if (entry.textReference) states.push(ui.text("builder.roms.text_ready"));
        if (entry.master === "installed") states.push(ui.text("builder.roms.master_ready"));
        if (entry.world === "installed") states.push(ui.text("builder.roms.world_ready"));
        const invalidImport = [entry.media, entry.master, entry.world].includes("invalid");
        if (invalidImport) states.push(ui.text("builder.roms.invalid_import"));
        status.textContent = states.length ? states.join(" · ") :
          ui.text(entry.id === "us" ? "builder.roms.required" : "builder.roms.optional");
        button.dataset.available = String(states.length > 0);
        button.dataset.invalid = String(invalidImport);
        button.append(title, help, status);
        button.addEventListener("click", () => {
          if (bigScreen()) button.focus();
          browseEntry(entry);
        });
        card.append(button);
        const actions = document.createElement("div");
        actions.className = "rom-slot-actions";
        const action = (kind, key, values) => {
          const control = document.createElement("button");
          control.type = "button";
          control.className = "quiet";
          control.id = button.id + "-" + kind;
          control.dataset.romAction = kind;
          ui.set(control, key, values);
          control.setAttribute("aria-label", label + " · " + control.textContent);
          control.addEventListener("click", () => selectAndReport(
            () => selectLocal({id: entry.id, [kind]: true}), entry.id,
          ));
          actions.append(control);
        };
        if (!bigScreen() && entry.found && entry.found !== entry.selected) action("found", "builder.roms.use_found", {name: entry.found});
        if (entry.selected) action("clear", "builder.roms.clear");
        if (actions.childElementCount) card.append(actions);
        return card;
      });
      rows.replaceChildren(...cards);
      checksumsButton.disabled = !inventory.length;
      updateActions();
      if (restoreFocus) {
        const target = document.getElementById(focused.id) || (browseID && document.getElementById(browseID));
        if (target && !target.disabled) target.focus({preventScroll: true});
      }
      unavailableNote.hidden = !unavailable.length;
      if (unavailable.length) ui.set(unavailableNote, "builder.roms.unavailable", {names: unavailable.join(", ")});
      report.hidden = !operationStatus;
      if (operationStatus) ui.set(report, operationStatus.key, operationStatus.values);
    }

    async function load() {
      const requestID = ++loading;
      try {
        const data = await request("rom-checklist");
        if (requestID !== loading) return;
        inventory = data.roms || [];
        unavailable = data.unavailable || [];
        render();
        window.workshopFeedback?.clear(rows);
      } catch (error) {
        if (requestID !== loading) return;
        window.workshopFeedback?.show(rows, error, {
          operation: "Load ROM checklist", retry: load, deduplicate: true,
        });
      }
    }

    importButton.addEventListener("click", async () => {
      if (importing || busy || selecting) return;
      importing = true;
      updateActions();
      try {
        await request("rom-import-selected", {method: "POST"});
        operationStatus = {key: "builder.roms.imported"};
        await onImported();
      } catch (error) {
        fail(error, "Import selected ROMs");
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
      upload,
      selectLocal,
      render,
      isSelecting: () => selecting,
      availability(play, locked) { canPlay = play; busy = locked; updateActions(); },
    };
  };
})();
