/* The service supplies the import catalog and owns each content format. */
(() => {
  "use strict";
  const ui = window.workshopI18n;
  const form = document.getElementById("game-imports-form");
  const input = document.getElementById("game-imports-file");
  const status = document.getElementById("game-imports-status");
  const refresh = document.getElementById("game-imports-refresh");
  const choice = document.getElementById("game-imports-content");
  const help = document.getElementById("game-imports-help");
  const next = document.getElementById("game-imports-next");
  const category = document.querySelector('[data-asset-category="imports"]');
  let items = [], busy = false, closed = false;
  function selection() {
    const item = items.find(row => row.id === choice.value);
    if (!item) return;
    ui.set(help, item.helpKey);
    ui.set(next, item.nextKey);
    const state = ["missing", "installed", "invalid"].includes(item.status) ? item.status : "invalid";
    ui.set(status, "builder.imports." + state);
  }
  choice.addEventListener("change", selection);
  const request = (...args) => window.workshopFeedback?.request ?
    window.workshopFeedback.request(...args) : fetch(...args);
  async function read(response) {
    if (window.workshopFeedback?.readJSON) return window.workshopFeedback.readJSON(response);
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || String(response.status));
    return body;
  }
  async function run(action, retry) {
    if (busy || closed) return;
    busy = true;
    for (const node of form.querySelectorAll("input,select,button")) node.disabled = true;
    window.workshopFeedback?.clear(status);
    ui.set(status, "builder.imports.working");
    try { await action(); }
    catch (error) {
      ui.set(status, "builder.imports.failed", {detail: error.message});
      window.workshopFeedback?.show(status, error, {operation: "Import game content", retry});
    } finally {
      busy = false;
      for (const node of form.querySelectorAll("input,select,button")) node.disabled = closed;
    }
  }
  function load() {
    return run(async () => {
      const data = await read(await request("game-imports"));
      const selected = choice.value;
      items = data.imports;
      category.hidden = items.length === 0;
      if (category.hidden && !form.hidden) {
        document.querySelector('[data-asset-category="music"]').click();
        form.hidden = true;
      }
      choice.replaceChildren(...items.map(item => {
        const option = document.createElement("option");
        option.value = item.id;
        ui.set(option, item.labelKey);
        return option;
      }));
      choice.value = items.some(item => item.id === selected) ? selected : items[0]?.id || "";
      selection();
    }, load);
  }
  function install() {
    const file = input.files?.[0];
    if (!file || !choice.value) return;
    return run(async () => {
      const body = new FormData();
      body.append("rom", file);
      body.append("content", choice.value);
      await read(await request("game-imports", {method: "POST", body}));
      const item = items.find(row => row.id === choice.value);
      if (item) item.status = "installed";
      selection();
      input.value = "";
      window.workshopFileInputs?.refresh(input);
      document.dispatchEvent(new Event("workshop:content-imported"));
    }, install);
  }
  form.addEventListener("submit", event => { event.preventDefault(); return install(); });
  refresh.addEventListener("click", load);
  category.addEventListener("click", load);
  document.addEventListener("workshop:navigate", () => {
    if (!document.getElementById("panel-assets").hidden) return load();
  });
  document.addEventListener("workshop:closed", () => {
    closed = true;
    for (const node of form.querySelectorAll("input,select,button")) node.disabled = true;
  });
})();
