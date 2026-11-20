(() => {
  "use strict";

  window.workshopFileBrowser = (request, {selectROM, selected, installMod, installed}) => {
    const $ = id => document.getElementById(id);
    const dialog = $("big-rom-dialog"), entries = $("big-rom-entries");
    const location = $("big-rom-location"), heading = $("big-rom-title");
    const cancelButton = $("big-rom-back"), up = $("big-rom-up"), computer = $("big-rom-computer");
    const message = $("big-rom-message");
    const ui = window.workshopI18n;
    let sequence = 0, drives = [], current, parent, folderName, returnTo, slot, saving = false, mods;
    const endpoint = () => mods ? "mods/browse" : "rom-browser";
    const labelFor = item => item.label || ui.text("builder.big_screen.rom_" + item.id);

    function locationMessage(key) {
      location.replaceChildren();
      ui.set(location, key);
    }

    function addButton(label, key, activate, icon = "") {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "secondary";
      const caption = document.createElement("span");
      caption.textContent = label;
      if (icon) {
        const shape = document.createElement("span");
        shape.className = "browser-icon browser-icon-" + icon;
        shape.setAttribute("aria-hidden", "true");
        button.append(shape);
      }
      button.append(caption);
      button.dataset.entry = key;
      button.addEventListener("click", () => activate(button));
      entries.append(button);
    }

    function close() {
      ++sequence;
      dialog.close();
      returnTo?.focus();
    }

    async function choose(values, button) {
      if (saving) return;
      saving = true;
      ++sequence;
      button.focus();
      const others = [...dialog.querySelectorAll("button")].filter(other => other !== button && !other.disabled);
      for (const other of others) other.disabled = true;
      button.setAttribute("aria-disabled", "true");
      const progress = document.createElement("small");
      progress.className = "browser-progress";
      progress.setAttribute("role", "status");
      button.append(progress);
      ui.set(progress, mods ? "builder.mods.installing" : "builder.roms.checking_file");
      try {
        const result = mods ? await installMod(values) : await selectROM({id: slot?.id || "", ...values});
        if (!result) return;
        close();
        if (mods) installed(result);
        else selected(result.id);
      } catch (error) {
        window.workshopFeedback?.show(location, error, {operation: mods ? "Install mod" : "Select ROM"});
      } finally {
        saving = false;
        for (const other of others) other.disabled = false;
        button.removeAttribute("aria-disabled");
        progress.remove();
      }
    }

    function showComputer(focusDrive) {
      if (saving) return;
      ++sequence;
      current = parent = null;
      up.hidden = computer.hidden = true;
      message.hidden = true;
      entries.replaceChildren();
      entries.scrollTop = 0;
      locationMessage("builder.big_screen.rom_computer");
      for (const drive of drives) {
        addButton(labelFor(drive), drive.id, () => browse({location: drive.id, path: "."}), "drive");
      }
      focusEntry(focusDrive);
    }

    function focusEntry(key) {
      const match = [...entries.children].find(button => button.dataset.entry === key);
      (match || entries.firstElementChild || (!up.hidden ? up : cancelButton)).focus();
    }

    async function browse(folder, focusKey, fallback) {
      if (saving) return;
      const attempt = ++sequence;
      message.hidden = false;
      ui.set(message, "builder.big_screen.loading");
      const query = new URLSearchParams(folder);
      try {
        const reply = await request(endpoint() + "?" + query);
        if (attempt !== sequence || !dialog.open) return;
        current = folder;
        parent = reply.parent;
        folderName = reply.name;
        up.hidden = computer.hidden = false;
        entries.replaceChildren();
        entries.scrollTop = 0;
        ui.unbind(location);
        const pathLabel = document.createDocumentFragment();
        for (const part of reply.folder.split(/([/\\])/)) {
          pathLabel.append(part);
          if (part === "/" || part === "\\") pathLabel.append(document.createElement("wbr"));
        }
        location.replaceChildren(pathLabel);
        const found = slot?.found && slot.found !== slot.selected;
        if (found) {
          addButton(ui.text("builder.roms.use_found", {name: slot.found}), "found", button => choose({found: true}, button), "file");
        }
        for (const item of reply.entries) {
          const path = folder.path === "." ? item.name : folder.path + "/" + item.name;
          addButton(item.name, (item.directory ? "dir:" : "file:") + item.name, button => {
            const entry = {location: folder.location, path};
            if (item.directory) browse(entry);
            else choose(entry, button);
          }, item.directory ? "folder" : "file");
        }
        message.hidden = !!entries.children.length;
        if (!message.hidden) ui.set(message, mods ? "builder.mods.no_files" : "builder.big_screen.no_roms");
        focusEntry(focusKey);
      } catch (error) {
        if (attempt !== sequence || !dialog.open) return;
        if (fallback) return fallback();
        message.hidden = true;
        window.workshopFeedback?.show(location, error, {operation: "Browse files"});
      }
    }

    const cancel = () => { if (!saving) close(); };
    dialog.addEventListener("cancel", event => { event.preventDefault(); cancel(); });
    cancelButton.addEventListener("click", cancel);
    up.addEventListener("click", () => {
      if (!current) return;
      if (!parent) return showComputer(current.location);
      browse(parent, "dir:" + folderName);
    });
    computer.addEventListener("click", () => showComputer(current?.location));

    async function open(entry, browseMods) {
      if (dialog.open || saving) return;
      const attempt = ++sequence;
      slot = entry;
      mods = browseMods;
      drives = [];
      current = parent = null;
      returnTo = document.activeElement;
      ui.set(heading, mods ? "builder.mods.browse" : "builder.roms.select_for", {rom: slot?.label});
      up.hidden = computer.hidden = true;
      entries.replaceChildren();
      locationMessage("builder.big_screen.loading");
      message.hidden = true;
      dialog.showModal();
      cancelButton.focus();
      try {
        const reply = await request(endpoint());
        if (attempt !== sequence || !dialog.open) return;
        drives = reply.locations.filter(item => item.drive);
        const starts = ["downloads", "game", "home"].filter(id => reply.locations.some(item => item.id === id))
          .map(location => ({location, path: "."}));
        if (reply.recent?.location) starts.unshift(reply.recent);
        const openNext = () => {
          if (starts.length) return browse(starts.shift(), undefined, openNext);
          showComputer();
        };
        openNext();
      } catch (error) {
        if (attempt !== sequence || !dialog.open) return;
        showComputer();
        window.workshopFeedback?.show(location, error, {operation: "Browse files"});
      }
    }
    return {rom: entry => open(entry, false), mod: () => open(null, true)};
  };
})();
