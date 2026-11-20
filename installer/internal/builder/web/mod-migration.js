(() => {
  "use strict";

  window.workshopLegacyMods = (request, onConverted) => {
    const ui = window.workshopI18n;
    const trigger = document.getElementById("legacy-mod-review");
    const dialog = document.getElementById("legacy-mod-dialog");
    const items = document.getElementById("legacy-mod-items");
    const status = document.getElementById("legacy-mod-status");
    const apply = document.getElementById("legacy-mod-apply");
    const later = document.getElementById("legacy-mod-later");
    let review, busy = false, converted = false;

    function paint(result, done = false) {
      review = result;
      converted = done;
      items.replaceChildren();
      for (const item of result.items || []) {
        const row = document.createElement("li");
        const title = {"actraiser-hd-title-en": "builder.big_screen.hd_title_en",
          "actraiser-hd-title-ja": "builder.big_screen.hd_title_ja"}[item.pack];
        const target = item.type === "actraiser-music" ? window.workshopModPacks.targetName(item.target) :
          ui.text(item.type === "actraiser-manual" ? "builder.mods.manual_slot" : title || "builder.assets.title_art");
        const state = document.createElement("small");
        row.append(document.createTextNode(target + " · " + item.file), state);
        ui.set(state, "builder.migration." + (item.reason || (done ? "converted" : "convertible")), {detail: item.detail});
        if (item.existing) {
          const active = document.createElement("small");
          ui.set(active, "builder.migration.existing", {name: item.existing});
          row.append(active);
        }
        items.append(row);
      }
      apply.hidden = done || result.rebuild || !result.items?.some(item => !item.reason);
      ui.set(later, done ? "builder.migration.done" : "builder.migration.later");
      if (done && result.backup) ui.set(status, "builder.migration.backup", {path: result.backup});
      else if (result.rebuild) ui.set(status, "builder.migration.rebuild");
      else if (!result.items?.length) ui.set(status, "builder.migration.empty");
      else {
        ui.unbind(status);
        status.textContent = "";
      }
    }

    async function load(open = false) {
      if (busy) return;
      busy = true;
      try {
        const result = await request(open ? "mods/migration" : "mods/migration?summary=true", {cache: "no-store"});
        const hidden = !result.items?.length;
        if (hidden && document.activeElement === trigger) document.getElementById("tab-mods").focus();
        trigger.hidden = hidden;
        if (open && !document.querySelector("dialog[open]")) {
          paint(result);
          dialog.showModal();
        }
      } catch (error) {
        trigger.hidden = false;
        if (open) window.workshopFeedback?.show(status, error, {operation: "Review legacy customizations"});
      } finally {
        busy = false;
      }
    }

    trigger.addEventListener("click", () => load(true));
    later.addEventListener("click", () => dialog.close());
    dialog.addEventListener("close", () => {
      trigger.focus();
      if (converted) {
        load();
        onConverted([...new Set(review.items.filter(item => !item.reason).map(item => item.type))]);
      }
    });
    dialog.addEventListener("cancel", event => {
      if (busy) event.preventDefault();
    });
    apply.addEventListener("click", async () => {
      if (busy) return;
      busy = later.disabled = apply.disabled = true;
      ui.set(status, "builder.big_screen.loading");
      try {
        paint(await request("mods/migration", window.workshopModPacks.jsonOptions({revision: review.revision})), true);
        later.focus();
      } catch (error) {
        ui.unbind(status);
        status.textContent = "";
        window.workshopFeedback?.show(status, error, {operation: "Convert legacy customizations"});
      } finally {
        busy = later.disabled = apply.disabled = false;
      }
    });
    return {load};
  };
})();
