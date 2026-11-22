import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {runInNewContext} from "node:vm";
import {Node, setup} from "./interface_dom.mjs";

const script = readFileSync(new URL("../../workshopui/big-screen.js", import.meta.url), "utf8");

test("a catalog read stays distinct from pending mod writes", async () => {
  const {context} = setup();
  runInNewContext(readFileSync(new URL("../web/mod-packs.js", import.meta.url), "utf8"), context);
  const requests = context.window.workshopModPacks.createRequests();
  let releaseRead, releaseWrite, writeStarted;
  const reading = requests.run(() => new Promise(resolve => { releaseRead = resolve; }));
  assert.equal(requests.isBusy(), true);
  assert.equal(requests.isWriting(), false);
  const started = new Promise(resolve => { writeStarted = resolve; });
  const writing = requests.write(() => new Promise(resolve => { releaseWrite = resolve; writeStarted(); }));
  assert.equal(requests.isWriting(), true, "a queued user write still needs protecting");
  await Promise.resolve();
  releaseRead();
  await reading;
  await started;
  assert.equal(requests.isWriting(), true);
  releaseWrite();
  await writing;
  assert.equal(requests.isWriting(), false);
});

test("install feedback directs new overlaps to Preview and respects settled choices", () => {
  const {context,ui} = setup();
  runInNewContext(readFileSync(new URL("../web/mod-packs.js", import.meta.url), "utf8"), context);
  const {musicType, renderInstallFeedback} = context.window.workshopModPacks;
  const status = new Node("p");
  const catalog = {
    installedId: "second", installedName: "New Fillmore", installedType: musicType,
    packs: [{id: "second", enabled: true}],
    slots: [{id: "song-09-fillmore", track: "song-09", region: "fillmore", type: musicType,
      candidates: ["first", "second"], winner: "first"}],
  };
  catalog.slots[0].needsChoice = true;
  renderInstallFeedback(status, catalog);
  assert.equal(status.textContent, ui.text("builder.mods.installed_conflict", {name: "New Fillmore"}));
  catalog.slots[0].needsChoice = false;
  renderInstallFeedback(status, catalog);
  assert.equal(status.textContent, ui.text("builder.mods.installed_alternate", {name: "New Fillmore"}));
  assert.equal(catalog.slots[0].winner, "first");
});

class DialogObserver {
  constructor(callback) { this.callback = callback; }
  observe(body) { body.dialogChanged = target => this.callback([{target}]); }
  disconnect() {}
}

test("controller input follows focus and requires a new press after refocus", () => {
  let callback, back = 0, focused = true, bigScreen = true;
  const document = Object.assign(new EventTarget(), {
    body: {dataset: {}}, hidden: false, hasFocus: () => focused, querySelectorAll: () => [],
  });
  const window = new EventTarget();
  const pad = {index: 0, mapping: "standard", axes: [0, 0], buttons: []};
  runInNewContext(script, {
    document, window, Event, MutationObserver: DialogObserver, navigator: {getGamepads: () => [pad]},
    requestAnimationFrame: next => { callback = next; return 1; },
    cancelAnimationFrame: () => { callback = undefined; },
  });
  function button(disabled = false) {
    return {
      disabled, clicks: 0,
      focus() { document.activeElement = this; },
      click() { this.clicks++; },
      getClientRects: () => [1], matches: () => true,
    };
  }
  function scope(...buttons) {
    return Object.assign(new EventTarget(), {
      querySelectorAll: () => buttons, contains: node => buttons.includes(node),
    });
  }
  const play = button(), unavailable = button(true), exit = button(), rebuild = button();
  const panel = scope(play, unavailable, exit);
  const log = {scrollTop: 160, clientHeight: 100, getClientRects: () => [1]};
  let paging = false;
  const rows = Array.from({length: 8}, () => Object.assign(button(), {offsetHeight: 40}));
  const list = {clientHeight: 160, getClientRects: () => [1], querySelectorAll: () => rows};
  panel.querySelector = () => null;
  const query = panel.querySelectorAll;
  panel.querySelectorAll = selector => {
    if (selector === "[data-pad-button]") return [];
    if (selector.startsWith("[data-big-scroll]")) return paging ? [] : [log];
    if (selector.startsWith("[data-big-page-list]")) return paging ? [list] : [];
    return query(selector);
  };
  window.workshopBigScreenNavigation(panel, () => back++, {
    isActive: () => bigScreen,
    toggleView: () => { bigScreen = !bigScreen; },
  });
  play.focus();

  let time = 0;
  function tick(index) {
    pad.buttons = Array.from({length: 16}, (_, i) => ({pressed: i === index}));
    callback(time += 20);
  }
  tick(0);
  tick(0);
  assert.equal(play.clicks, 0, "Play held during startup must not activate the Builder");
  tick();
  tick(13);
  assert.equal(document.activeElement, exit, "skip disabled actions");
  tick();
  pad.axes = [0, -.8];
  tick();
  assert.equal(document.activeElement, play, "left stick moves focus");
  pad.axes = [0, 0];

  tick(0);
  tick(0);
  assert.equal(play.clicks, 1, "a held confirm activates once");
  function openDialog(...buttons) {
    const dialog = Object.assign(scope(...buttons), {localName: "dialog", open: true, isConnected: true});
    dialog.close = () => {
      dialog.open = false;
      document.body.dialogChanged(dialog);
    };
    buttons.forEach(control => {
      control.closest = selector => selector === "dialog[open]" && dialog.open ? dialog : null;
    });
    document.body.dialogChanged(dialog);
    return dialog;
  }
  const modal = openDialog(rebuild);
  modal.addEventListener("cancel", event => { event.preventDefault(); modal.close(); });
  tick();
  tick(0);
  assert.equal(document.activeElement, rebuild, "focus stays inside the open dialog");
  const dismissError = button(), hiddenReport = button(), copyError = button();
  const errorDialog = openDialog(dismissError, hiddenReport, copyError);
  hiddenReport.closest = selector => selector === "details:not([open])" ? {} : null;
  tick();
  tick(0);
  assert.equal(document.activeElement, dismissError, "a newer dialog takes precedence over focus in the older dialog");
  assert.equal(rebuild.clicks, 0);
  tick();
  tick(13);
  assert.equal(document.activeElement, copyError, "skip controls inside collapsed details even when they have layout boxes");
  tick();
  tick(1);
  assert.equal(errorDialog.open, false);
  assert.equal(modal.open, true, "Back dismisses only the topmost dialog");
  tick();
  tick(1);
  assert.equal(modal.open, false);
  assert.equal(back, 0, "Back must dismiss the dialog before exiting the Builder");

  focused = false;
  window.dispatchEvent(new Event("blur"));
  tick(1);
  focused = true;
  tick(1);
  assert.equal(back, 0, "Back held as focus returns must not exit");
  tick();
  tick(1);
  assert.equal(back, 1);

  tick();
  tick(4);
  assert.equal(log.scrollTop, 80, "left shoulder scrolls the log up");
  tick();
  tick(5);
  assert.equal(log.scrollTop, 160, "right shoulder scrolls the log down");
  paging = true;
  rows[2].focus();
  tick();
  tick(5);
  assert.equal(document.activeElement, rows[5], "the shoulder button pages through a visible list");
  tick();
  tick(4);
  assert.equal(document.activeElement, rows[2], "paging back restores the preceding row");
  // Expanded track rows stay in the DOM after their parent collapses.
  rows[5].getClientRects = () => [];
  tick();
  tick(5);
  assert.equal(document.activeElement, rows[6], "paging skips collapsed track buttons");
  paging = false;
  tick();
  tick(9);
  tick(9);
  assert.equal(bigScreen, false, "a held Menu press toggles once");
  play.focus();
  tick();
  tick(13);
  assert.equal(document.activeElement, play, "leave desktop navigation to the desktop view");
  const keepEditing = button(), discard = button();
  const desktopClose = openDialog(keepEditing, discard);
  keepEditing.focus();
  tick();
  tick(15);
  assert.equal(document.activeElement, discard, "a controller can navigate a desktop quit dialog");
  tick();
  tick(0);
  assert.equal(discard.clicks, 1, "confirm activates the focused desktop dialog button");
  tick();
  tick(1);
  assert.equal(desktopClose.open, false, "Back dismisses the desktop dialog");
  assert.equal(back, 1, "Back does not leave the desktop page");
  tick();
  tick(9);
  assert.equal(bigScreen, true, "Menu also returns from the desktop view");

  function modRow(id, actions) {
    const row = {};
    return actions.map(action => Object.assign(button(), {
      dataset: {modControl: action + ":" + id},
      closest: selector => selector.includes("[data-big-row]") ? row : null,
    }));
  }
  const [firstToggle, firstPreview] = modRow("first", ["pack", "details"]);
  const [secondToggle, secondConflict, secondPreview] = modRow("second", ["pack", "conflict", "details"]);
  panel.querySelectorAll = () => [firstToggle, firstPreview, secondToggle, secondConflict, secondPreview, exit];
  firstToggle.focus();
  tick();
  tick(13);
  assert.equal(document.activeElement, secondToggle, "Down skips the current pack's preview");
  tick();
  tick(15);
  assert.equal(document.activeElement, secondConflict, "Right reaches the next action on the same pack");
  tick();
  tick(15);
  assert.equal(document.activeElement, secondPreview);
  tick();
  tick(12);
  assert.equal(document.activeElement, firstPreview, "Up keeps the preview action despite different columns");
  tick();
  tick(15);
  assert.equal(document.activeElement, firstToggle, "Right wraps within the row");

  const preview = button(), actions = button(), up = button();
  for (const control of [preview, actions, up]) control.getAttribute = () => null;
  panel.querySelector = selector => selector.includes("west") ? preview : actions;
  tick();
  tick(2);
  tick(2);
  assert.equal(preview.clicks, 1, "the west button previews once without moving focus off a mod row");
  assert.equal(document.activeElement, firstToggle);
  tick();
  tick(3);
  assert.equal(actions.clicks, 1, "the north button opens actions for the selection");
  const picker = openDialog(up);
  picker.querySelector = selector => selector.includes("west") ? up : null;
  up.focus();
  tick();
  tick(2);
  assert.equal(up.clicks, 1, "the active file dialog owns its Up shortcut");
  assert.equal(preview.clicks, 1, "shortcuts do not reach the mod list behind a dialog");
  picker.close();

  document.dispatchEvent(new Event("workshop:closed"));
  assert.equal(callback, undefined);
});

test("launcher hints are replaced by the controller in use, and disconnects restore keyboard glyphs", () => {
  let callback, pads = [];
  const document = Object.assign(new EventTarget(), {
    body: {dataset: {controllerHint: "playstation"}},
    hidden: false, hasFocus: () => true, querySelectorAll: () => [],
  });
  const window = Object.assign(new EventTarget(), {
    workshopI18n: {
      set: (node, key) => { node.textContent = key; },
      text: key => key.replace("builder.big_screen.", ""),
    },
  });
  runInNewContext(script, {
    document, window, Event, MutationObserver: DialogObserver, navigator: {getGamepads: () => pads},
    requestAnimationFrame: next => { callback = next; return 1; },
    cancelAnimationFrame() {},
  });
  const names = ["confirm", "back", "left", "right", "menu", "west", "north"];
  const hints = names.map(button => ({
    dataset: {padButton: button},
    setAttribute(name, value) { this[name] = value; },
  }));
  const move = {};
  const panel = {
    dataset: {},
    querySelector: () => move,
    querySelectorAll: () => hints,
  };
  window.workshopBigScreenNavigation(panel, () => {}, {isActive: () => false});
  assert.equal(panel.dataset.padFamily, "playstation");
  assert.deepEqual(hints.map(node => node.textContent), ["✕", "○", "L1", "R1", "☰", "□", "△"]);
  assert.equal(hints[4]["aria-label"], "options_button");

  const pad = (index, id, pressed) => ({
    index, id, mapping: "standard", axes: [0, 0],
    buttons: Array.from({length: 16}, (_, n) => ({pressed: pressed === n})),
  });
  pads = [pad(0, "XInput STANDARD GAMEPAD"), pad(1, "Wireless Controller (Vendor: 054c Product: 0ce6)")];
  callback(16);
  assert.equal(panel.dataset.padFamily, "xbox");
  assert.deepEqual(hints.map(node => node.textContent), ["A", "B", "LB", "RB", "☰", "X", "Y"]);

  pads[1] = pad(1, pads[1].id, 0);
  callback(32);
  assert.equal(panel.dataset.padFamily, "playstation");
  assert.equal(hints[0]["aria-label"], "cross_button");
  pads[1] = null;
  callback(48);
  assert.equal(panel.dataset.padFamily, "xbox");

  pads = [pad(2, "Nintendo Switch Pro Controller")];
  callback(64);
  assert.deepEqual(hints.map(node => node.textContent), ["B", "A", "L", "R", "+", "Y", "X"]);
  pads = [pad(3, "Generic USB gamepad")];
  callback(80);
  assert.equal(panel.dataset.padFamily, "playstation", "an unidentified controller can use the launcher hint");
  pads = [];
  callback(96);
  assert.equal(panel.dataset.padFamily, "keyboard");
  assert.equal(hints[0].textContent, "Enter");
  assert.equal(hints[4].hidden, true);
  assert.equal(move.textContent, "builder.big_screen.move_keyboard");
});

function setupConflictRows() {
  const {doc, context} = setup();
  function make(tag) {
    const node = new Node(tag);
    node.append = (...children) => node.children.push(...children);
    node.replaceChildren = (...children) => { node.children = children; };
    node.querySelectorAll = selector => node.children.flatMap(child =>
      [...((selector === "[data-mod-control]" ? child.dataset.modControl : child.tagName.toLowerCase() === selector) ? [child] : []),
        ...child.querySelectorAll(selector)]);
    node.focus = () => { doc.activeElement = node; };
    return node;
  }
  doc.createElement = make;
  const rows = make("div");
  runInNewContext(readFileSync(new URL("../web/mod-packs.js", import.meta.url), "utf8"), context);
  return {doc,context,rows,make};
}

test("both providers get a passive flag and each Preview opens the first pending asset", async () => {
  const {context, rows} = setupConflictRows();
  const {musicType: type, renderRows} = context.window.workshopModPacks;
  const targets = ["song-09", "song-09-fillmore", "title-theme"];
  const packs = ["suite", "other"].map(id => ({id, name:id, type, enabled:true, targets}));
  const slots = targets.map(id => ({id, type, track:id, candidates:["suite","other"], winner:"suite", needsChoice:id !== "title-theme"}));
  const catalog = {packs, slots};
  let details;
  const paint = () => renderRows(rows, catalog, () => assert.fail("opening a preview changed activation"), {
    type, preview: value => { details = value; },
  });
  const flags = () => rows.querySelectorAll("span").filter(node => node.dataset.state === "problem");
  paint();
  assert.deepEqual(flags().map(flag => flag.textContent), ["2 unresolved conflicts","2 unresolved conflicts"]);
  assert.equal(rows.querySelectorAll("button").length, 2, "only the two preview buttons are needed");
  for (const pack of packs) {
    const preview = rows.querySelectorAll("button").find(button => button.dataset.modControl === "details:" + pack.id);
    await preview.fire("click");
    assert.equal(details.pack.id, pack.id);
    assert.equal(details.target, targets[0]);
  }
  slots[0].needsChoice = false;
  paint();
  assert.deepEqual(flags().map(flag => flag.textContent), ["Conflict","Conflict"]);
  slots[1].needsChoice = false;
  paint();
  assert.equal(flags().length, 0);
  assert.ok(rows.querySelectorAll("span").some(node => node.textContent === "Inactive"));
  slots[1].winner = "other";
  paint();
  assert.ok(rows.querySelectorAll("span").some(node => node.textContent === "2 of 3 assets active"));
});

for (const [typeKey, slot] of [
  ["titleType", "title-artwork"], ["manualType", "in-game-manual"],
]) test(`${slot} opens the right preview under the conflict filter`, async () => {
  const {context,rows} = setupConflictRows();
  const mods = context.window.workshopModPacks;
  const type = mods[typeKey];
  const packs = ["first", "second"].map(id => ({id, name:id, type, enabled:true, targets:[slot]}));
  const choice = {type, id:slot, candidates:["first","second"], needsChoice:true};
  let preview;
  mods.renderRows(rows, {packs, slots:[choice]}, () => assert.fail("list changed the source"),
    {type:"conflicts", preview:details => {preview = details;}});
  await rows.querySelectorAll("button").find(control => control.dataset.modControl === "details:second").fire("click");
  assert.equal(preview.type, type);
  assert.equal(preview.target, slot);
});

function setupModPanel() {
  const {doc, context} = setup();
  let openDialog;
  doc.querySelector = selector => selector === "dialog[open]" ? openDialog : null;
  const nodes = new Map();
  function make(tag) {
    const node = new Node(tag);
    node.append = (...children) => node.children.push(...children);
    node.replaceChildren = (...children) => { node.children = children; };
    node.querySelector = selector => node.querySelectorAll(selector)[0];
    node.querySelectorAll = selector => node.children.flatMap(child => {
      const match = selector === "[data-mod-mutation]" ? child.hasAttribute("data-mod-mutation") :
        selector === "[data-mod-control]" ? child.dataset.modControl : child.tagName.toLowerCase() === selector;
      return [...(match ? [child] : []), ...child.querySelectorAll(selector)];
    });
    node.focus = () => { doc.activeElement = node; };
    return node;
  }
  doc.createElement = make;
  doc.getElementById = id => {
    if (!nodes.has(id)) nodes.set(id, make("div"));
    return nodes.get(id);
  };
  runInNewContext(readFileSync(new URL("../web/mod-packs.js", import.meta.url), "utf8"), context);
  return {doc, context, nodes, make, setDialog(dialog) { openDialog = dialog; }};
}

test("the conflict filter keeps one row per affected mod and stays selected when the last flag clears", async () => {
  const {context, doc, nodes} = setupModPanel();
  const type = context.window.workshopModPacks.musicType;
  const targets = Array.from({length:24}, (_,i) => "song-" + i);
  const packs = Array.from({length:96}, (_,i) => ({id:"pack-" + i, name:"Pack " + i, enabled:true, type, targets}));
  const candidates = packs.map(pack => pack.id);
  const catalog = {packs, slots:targets.map((id,i) => ({id, track:id, type, candidates:i < 3 ? candidates.slice(i,80) : candidates, winner:candidates[i], needsChoice:i < 3}))};
  let preview;
  const panel = context.window.workshopAssetPackPanel(async () => catalog, () => {}, options => { preview = options; });
  await panel.load();
  const rows = nodes.get("asset-pack-list"), filter = nodes.get("asset-pack-type");
  const controls = () => rows.querySelectorAll("[data-mod-control]");
  assert.equal(rows.querySelectorAll("input").length, 96);
  filter.value = "conflicts";
  await filter.fire("change");
  assert.equal(rows.querySelectorAll("input").length, 80);
  for (const locale of ["de", "en"]) {
    const ui = context.window.workshopI18n;
    ui.setLocale(locale);
    await doc.fire("workshop:language");
    assert.ok(rows.querySelectorAll("small")[0].textContent.startsWith(ui.text("builder.mods.title")));
    assert.equal(filter.value, "conflicts");
  }
  assert.equal(controls().some(control => control.dataset.modControl.startsWith("select:")), false);
  await controls().find(control => control.dataset.modControl === "details:pack-0").fire("click");
  assert.equal(preview.type, type);
  assert.equal(preview.target, targets[0]);
  catalog.slots[0].needsChoice = false;
  preview.onChanged(catalog);
  assert.equal(rows.querySelectorAll("input").length, 79);
  catalog.slots.forEach(slot => { slot.needsChoice = false; });
  preview.onChanged(catalog);
  assert.equal(rows.children[0].textContent, "No mods have unresolved conflicts.");
  assert.equal(filter.value, "conflicts");
  const clear = nodes.get("asset-pack-clear-filter");
  assert.equal(clear.hidden, false);
  await clear.fire("click");
  assert.equal(rows.querySelectorAll("input").length, 96);
});

test("both music previews switch just the shared track and preserve the mounted player", async () => {
  const {context, doc, nodes, make} = setupModPanel();
  const mods = context.window.workshopModPacks;
  const create = doc.createElement;
  doc.createElement = tag => {
    const node = create(tag);
    node.contains = child => node === child || node.children.some(item => item.contains?.(child));
    return node;
  };
  const dialog = doc.getElementById("mod-preview-dialog");
  dialog.showModal = () => { dialog.open = true; };
  dialog.close = () => { dialog.open = false; return dialog.fire("close"); };
  doc.body = make("body");
  context.window.workshopAudioTransport = (_group, row, {heading}) => {
    const player = doc.createElement("button");
    row.append(heading, player);
    return player;
  };
  context.window.workshopStopAudio = () => {};
  const type = mods.musicType, title = "title-theme", region = "song-09-fillmore";
  const solo = {id:"solo", name:"Solo", type, enabled:true, targets:[title], tracks:[{track:title}], previews:["solo.ogg"]};
  const suite = {id:"suite", name:"Suite", type, enabled:true, targets:[title,region],
    tracks:[{track:title}, {track:"song-09",region:"fillmore"}], previews:["suite.ogg","fillmore.ogg"]};
  const initial = {packs:[solo,suite], slots:[
    {id:title, type, candidates:["solo","suite"], winner:"solo", needsChoice:true},
    {id:region, type, candidates:["suite"], winner:"suite"},
  ]};
  let catalog = initial;
  const writes = [];
  let preferred = "solo";
  const request = async (path, options) => {
    assert.equal(path, "mods/activation");
    const {action,pack,slot} = JSON.parse(options.body);
    assert.equal(slot, title);
    writes.push([action, pack]);
    const excluded = action === "exclude-target";
    if (action === "select") preferred = pack;
    return {...catalog, packs:[solo,{...suite, excludedTargets:excluded ? [title] : []}], slots:[
      {...initial.slots[0], needsChoice:false, candidates:excluded ? ["solo"] : ["solo","suite"], winner:excluded ? "solo" : preferred},
      initial.slots[1],
    ]};
  };
  runInNewContext(readFileSync(new URL("../web/mod-preview.js", import.meta.url), "utf8"), context);
  const open = context.window.workshopModPreview(request);
  const preview = pack => open({pack, type, catalog, target:title, onChanged:next => { catalog = next; }});
  const body = nodes.get("mod-preview-content");
  const apply = () => body.querySelectorAll("button").find(button => button.dataset.i18n === "builder.mods.apply_source");
  async function stage(provider) {
    const field = body.querySelector("select");
    field.value = provider;
    await field.fire("change");
  }
  preview(solo);
  assert.equal(body.querySelector("select").value, "solo");
  assert.equal(apply().disabled, false, "an automatic winner can be confirmed without switching away");
  await apply().fire("click");
  assert.equal(catalog.slots[0].needsChoice, false);
  assert.equal(apply().disabled, true);
  await stage("suite");
  assert.equal(catalog.slots[0].winner, "solo", "browsing options leaves the current override intact");
  await apply().fire("click");
  assert.equal(catalog.slots[0].winner, "suite");
  await dialog.close();
  preview(suite);
  const [titleCard, regionCard] = body.querySelectorAll("li");
  const player = titleCard.querySelector("button");
  const include = titleCard.querySelector("input");
  assert.equal(titleCard.dataset.sourceState, "active");
  assert.equal(regionCard.querySelector("select"), undefined, "a unique track has no source picker");
  assert.equal(regionCard.querySelector("input").checked, true, "the unique track can still be excluded");
  await stage("solo");
  apply().focus();
  await apply().fire("click");
  assert.equal(titleCard.dataset.sourceState, "alternate");
  assert.equal(doc.activeElement, titleCard.querySelector("select"), "priority change keeps focus on the same asset");
  include.focus();
  await include.fire("change");
  assert.equal(doc.activeElement, include);
  assert.equal(include.checked, false);
  assert.match(include.getAttribute("aria-label"), /Excluded.*Title Theme/);
  assert.equal(titleCard.querySelector("select"), undefined);
  assert.equal(titleCard.hasAttribute("data-source-state"), false);
  await include.fire("change");
  assert.equal(include.checked, true);
  assert.equal(titleCard.querySelector("select").value, "solo");
  assert.deepEqual(writes, [["select","solo"],["select","suite"],["select","solo"],["exclude-target","suite"],["include-target","suite"]]);
  assert.equal(titleCard.querySelector("button"), player);
  assert.equal(catalog.slots[1].winner, "suite");
  await dialog.close();

  const alternates = Array.from({length:64}, (_, i) => ({id:"alternate-" + i, name:i < 2 ? "Remix" : "Remix " + i}));
  catalog = {packs:[solo, suite, ...alternates], slots:[
    {...initial.slots[0], winner:"", candidates:["solo","suite", ...alternates.map(pack => pack.id)]},
    initial.slots[1],
  ]};
  preview(suite);
  const field = body.querySelector("select");
  assert.equal(body.querySelectorAll("select").length, 1);
  assert.equal(field.value, "");
  assert.equal(apply().disabled, true, "opening a picker without an incumbent must not choose a winner");
  const labels = field.children.map(option => option.textContent);
  assert.equal(labels.length, 67, "one placeholder followed by all 66 sources");
  assert.equal(new Set(labels).size, labels.length, "same-name sources are distinguishable");
  assert.ok(labels.indexOf("Remix 9") < labels.indexOf("Remix 10"));
});

test("installed language search finds releases without scanning or changing their activation", async () => {
  const {context, nodes} = setupModPanel();
  const {languageType} = context.window.workshopModPacks;
  const choices = ["one", "two"].map(key => ({
    name: "Translation", filename: "same.o2r", version: "1.0", key, sourcePath: "/" + key + "/same.o2r",
  }));
  const languages = [
    {name: "Translation", installedName: "Traduction", locale: "fr-CA", id: "example.french", key: "french", installed: true, enabled: true, choices},
    {name: "German", locale: "de", id: "german", key: "german", installed: true},
  ];
  const requests = [];
  const panel = context.window.workshopAssetPackPanel(async endpoint => {
    requests.push(endpoint);
    return {languages};
  }, () => {});
  panel.setType(languageType);
  await panel.load();
  const rows = nodes.get("asset-pack-list"), search = nodes.get("asset-pack-language-search");
  const counts = nodes.get("asset-pack-language-counts");
  assert.equal(counts.textContent, "1 enabled · 1 disabled");
  for (const query of [" TraduCtion ", "FR-ca", "example.FRENCH", "same.o2r"]) {
    search.value = query;
    await search.fire("input");
    assert.equal(rows.querySelectorAll("input").length, 1);
    assert.equal(counts.textContent, "1 enabled · 0 disabled");
    const labels = rows.querySelectorAll("span").map(span => span.textContent);
    for (const choice of choices) assert.ok(labels.some(label => label.includes(choice.sourcePath)));
  }
  search.value = "absent";
  await search.fire("input");
  assert.equal(rows.children[0].textContent, "No matching packages.");
  assert.equal(counts.hidden, true);
  panel.setType(languageType); // Installing another language opens this view again.
  panel.showInstalled({languages});
  assert.equal(search.value, "");
  assert.equal(rows.querySelectorAll("input").length, 2);
  assert.deepEqual(requests, ["mods/catalog"]);
});

test("mod management queues refreshes and keeps pending actions visibly disabled", async () => {
  const {doc, context, nodes, make, setDialog} = setupModPanel();
  const {musicType, createRequests} = context.window.workshopModPacks;
  const catalog = {packs: ["first", "second"].map(id=>({id, name:id, type:musicType, enabled:true}))};
  const requests = [];
  let release;
  const saving = new Promise(resolve => { release = resolve; });
  const request = async (endpoint, options) => {
    requests.push(endpoint === "mods/activation" ? JSON.parse(options.body).pack : endpoint);
    if (endpoint === "mods/activation") await saving;
    return catalog;
  };
  const shared = createRequests(request);
  const panel = context.window.workshopAssetPackPanel(request, () => {}, undefined, shared);
  nodes.get("asset-packs").append(nodes.get("asset-pack-list"), nodes.get("asset-pack-legacy-manual"));
  panel.setType(musicType);
  await panel.load();
  const checkboxes = () => nodes.get("asset-pack-list").querySelectorAll("input");
  const [first] = checkboxes();
  const closing = make("dialog");
  setDialog(closing);
  await panel.load();
  assert.equal(checkboxes()[0],first,"an in-flight refresh preserves a modal's opener");
  setDialog(null);
  await closing.fire("close");
  assert.notEqual(checkboxes()[0],first);
  const [toggle, alternate] = checkboxes();
  toggle.focus();
  toggle.checked = false;
  const pending = toggle.fire("click");
  assert.equal(alternate.getAttribute("aria-disabled"), "true");
  await alternate.fire("click", {preventDefault(){}});
  const refresh = panel.load();
  catalog.packs[0].enabled = false;
  release();
  await Promise.all([pending, refresh]);
  assert.deepEqual(requests, ["mods/catalog", "mods/catalog", "first", "mods/catalog"]);
  assert.equal(doc.activeElement, checkboxes()[0]);
  assert.equal(checkboxes()[1].getAttribute("aria-disabled"), "false");
  await checkboxes()[1].fire("click");
  assert.equal(requests.at(-1), "second");
  const requestCount = requests.length;
  nodes.get("asset-pack-type").value = "all";
  await nodes.get("asset-pack-type").fire("change");
  assert.equal(checkboxes().length, 2, "the cached catalogue supports the All filter");
  assert.equal(requests.length, requestCount, "changing filters never scans a loaded catalogue");
});
