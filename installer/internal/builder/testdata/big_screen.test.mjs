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

test("an installed alternative offers a shortcut to its slot without changing the winner", () => {
  const {context,ui} = setup();
  runInNewContext(readFileSync(new URL("../web/mod-packs.js", import.meta.url), "utf8"), context);
  const {musicType, renderInstallFeedback} = context.window.workshopModPacks;
  const status = new Node("p"), review = new Node("button"), choice = new Node("button");
  let focused = false;
  choice.dataset.modControl = "select:song-09-fillmore:second";
  choice.focus = () => { focused = true; };
  const rows = {querySelectorAll: () => [choice]};
  const catalog = {
    installedId: "second", installedName: "New Fillmore", installedType: musicType,
    packs: [{id: "second", enabled: true}],
    slots: [{id: "song-09-fillmore", track: "song-09", region: "fillmore", type: musicType,
      candidates: ["first", "second"], winner: "first"}],
  };
  renderInstallFeedback(status, review, rows, catalog);
  assert.equal(status.textContent, ui.text("builder.mods.installed_alternate", {name: "New Fillmore"}));
  assert.match(review.textContent, /Fillmore/);
  assert.equal(review.hidden, false);
  review.onclick();
  assert.equal(focused, true);
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

test("reviewing a pack with multiple conflicts focuses its candidate for the chosen region", async () => {
  const {doc,context,rows,make}=setupConflictRows();
  const {musicType:type,renderRows}=context.window.workshopModPacks;
  const dialog=make("dialog"),list=make("div"),heading=make("h2");
  doc.getElementById=id=>({"mod-conflict-review":dialog,"mod-conflict-targets":list,"mod-conflict-heading":heading})[id];
  dialog.showModal=()=>{dialog.open=true;};
  dialog.close=()=>{dialog.open=false;};
  const packs=["first","second"].map(id=>({id,name:id,type,enabled:true}));
  const slots=["","fillmore","bloodpool"].map(region=>({
    id:region || "song-09", type, track:"song-09", region, candidates:["first","second"], winner:region==="bloodpool"?"first":"",
  }));
  renderRows(rows,{packs,slots},()=>assert.fail("review must preserve activation"),{type});
  const review=rows.querySelectorAll("[data-mod-control]").find(button=>button.dataset.modControl==="conflict:pack:second");
  assert.equal(review.textContent,"2 unresolved conflicts");
  await review.fire("click");
  assert.equal(dialog.open,true);
  assert.equal(list.children.length,2);
  assert.match(list.children[1].children[0].textContent,/Fillmore/);
  await list.children[1].fire("click");
  assert.equal(dialog.open,false);
  assert.equal(doc.activeElement.dataset.modControl,"select:fillmore:second");
});

for (const [typeKey, slot] of [
  ["titleType", "title-artwork"],
  ["manualType", "in-game-manual"],
]) test(`${slot} conflicts use one controller-selectable slot and restore focus`, async () => {
  const {doc,context,rows}=setupConflictRows();
  const actions=[];
  const {renderRows, musicType, captureFocus, restoreFocus} = context.window.workshopModPacks;
  const type = context.window.workshopModPacks[typeKey];
  const packs = [
    {type, id:"first", name:"First", enabled:true, previews:["mods/image/first.png"]},
    {type, id:"second", name:"Second", enabled:true, previews:["mods/image/second.png"]},
    {type:musicType, id:"music", name:"Music", enabled:true},
  ];
  const choice = {type, id:slot, candidates:["first","second"], winner:""};
  const music = {type:musicType, id:"title-theme", track:"title-theme", candidates:["music"]};
  const previews = [];
  const paint = () => {
    const focus = captureFocus(rows);
    renderRows(rows, {packs, slots:[choice,music]}, action => actions.push(action), {
      big: true, type, preview: ({pack}) => previews.push(pack.id),
    });
    restoreFocus(rows, focus);
  };
  paint();
  assert.equal(rows.children[0].tagName, "TABLE");
  assert.equal(rows.children[1].className, "mod-conflicts");
  const find = id => rows.querySelectorAll("[data-mod-control]").find(control => control.dataset.modControl === id);
  assert.equal(find("pack:music"), undefined);
  assert.equal(rows.querySelectorAll("img").length, 0, "pack artwork is only shown in the modal");
  assert.equal(find("pack:first").checked, true);
  await find("details:first").fire("click");
  assert.equal(previews[0], "first");
  const selection = `select:${slot}:second`;
  const use = find(selection);
  use.focus();
  await use.fire("click");
  assert.equal(actions[0].action, "select");
  assert.equal(actions[0].pack, "second");
  assert.equal(actions[0].slot, slot);
  choice.winner = "second";
  paint();
  assert.equal(doc.activeElement, find(selection));
  assert.equal(doc.activeElement.getAttribute("aria-pressed"), "true");
  const enabled = find("pack:first");
  enabled.checked = false;
  await enabled.fire("click");
  assert.deepEqual({...actions[1]}, {action: "disable", pack: "first"});
});

function setupModPanel() {
  const {doc, context} = setup();
  let openDialog;
  doc.querySelector = selector => selector === "dialog[open]" ? openDialog : null;
  const nodes = new Map();
  function make(tag) {
    const node = new Node(tag);
    node.append = (...children) => node.children.push(...children);
    node.replaceChildren = () => { node.children = []; };
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
