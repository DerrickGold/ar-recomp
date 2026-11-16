(() => {
  "use strict";

  window.workshopROMBrowser = (request, selectROM, selected) => {
    const dialog = document.querySelector("#big-rom-dialog");
    const entries = document.querySelector("#big-rom-entries");
    const location = document.querySelector("#big-rom-location");
    const backButton = document.querySelector("#big-rom-back");
    const ui = window.workshopI18n;
    let sequence = 0, place = null, folder = ".", returnTo, slot, saving = false;

    function addButton(label, key, activate) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "secondary";
      button.textContent = label;
      button.dataset.entry = key;
      button.addEventListener("click", activate);
      entries.append(button);
    }

    async function choose(values) {
      if (saving) return;
      saving = true;
      try {
        const result = await selectROM({id: slot?.id || "", ...values});
        dialog.close();
        await selected(result.id);
      } catch (error) {
        ui.set(location, "builder.request_failed", {detail: error.message});
      } finally {
        saving = false;
      }
    }

    async function browse(nextPlace, nextFolder, focusKey) {
      if (saving) return;
      const attempt = ++sequence;
      place = nextPlace;
      folder = nextFolder;
      entries.replaceChildren();
      ui.set(location, "builder.big_screen.loading");
      location.focus();
      const query = place ? "?" + new URLSearchParams({location: place.id, path: folder}) : "";
      try {
        const reply = await request("rom-browser" + query);
        if (attempt !== sequence || !dialog.open) return;
        if (!place) {
          ui.set(location, slot ? "builder.roms.select_for" : "builder.big_screen.rom_locations", {rom: slot?.label});
          if (slot?.found && slot.found !== slot.selected) {
            addButton(ui.text("builder.roms.use_found", {name: slot.found}), "found", () => choose({found: true}));
          }
          if (slot?.selected) {
            addButton(ui.text("builder.roms.clear"), "clear", () => choose({clear: true}));
          }
          for (const item of reply.locations) {
            const label = item.label || ui.text("builder.big_screen.rom_" + item.id);
            addButton(label, item.id, () => browse({id: item.id, label}, "."));
          }
        } else {
          ui.unbind(location);
          location.textContent = folder === "." ? place.label : place.label + " / " + folder;
          for (const item of reply.entries) {
            const path = folder === "." ? item.name : folder + "/" + item.name;
            addButton((item.directory ? "▸ " : "") + item.name, item.name, () => {
              if (item.directory) browse(place, path);
              else choose({location: place.id, path});
            });
          }
          if (!reply.entries.length) ui.set(location, "builder.big_screen.no_roms", {folder: location.textContent});
        }
        const first = [...entries.children].find(button => button.dataset.entry === focusKey) || entries.firstElementChild;
        (first || backButton).focus();
      } catch (error) {
        if (attempt !== sequence || !dialog.open) return;
        ui.set(location, "builder.request_failed", {detail: error.message});
        backButton.focus();
      }
    }

    function back() {
      if (saving) return;
      if (!place) {
        dialog.close();
        returnTo?.focus();
      } else if (folder === ".") {
        browse(null, ".", place.id);
      } else {
        const parts = folder.split("/");
        const child = parts.pop();
        browse(place, parts.join("/") || ".", child);
      }
    }

    dialog.addEventListener("cancel", event => { event.preventDefault(); back(); });
    dialog.addEventListener("close", () => { sequence++; });
    backButton.addEventListener("click", back);
    return (entry) => {
      if (dialog.open) return;
      slot = entry;
      returnTo = document.activeElement;
      dialog.showModal();
      browse(null, ".");
    };
  };
})();
