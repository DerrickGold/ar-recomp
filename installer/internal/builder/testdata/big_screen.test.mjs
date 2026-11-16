import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {runInNewContext} from "node:vm";

const script = readFileSync(new URL("../../workshopui/big-screen.js", import.meta.url), "utf8");

test("controller input follows focus and requires a new press after refocus", () => {
  let dialog, callback, back = 0, focused = true, bigScreen = true;
  const document = Object.assign(new EventTarget(), {
    body: {dataset: {}}, hidden: false, hasFocus: () => focused, querySelector: () => dialog,
  });
  const window = new EventTarget();
  const pad = {index: 0, mapping: "standard", axes: [0, 0], buttons: []};
  runInNewContext(script, {
    document, window, Event, navigator: {getGamepads: () => [pad]},
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
  const modal = scope(rebuild);
  dialog = modal;
  modal.close = () => { dialog = undefined; };
  modal.addEventListener("cancel", event => { event.preventDefault(); modal.close(); });
  tick();
  tick(0);
  assert.equal(document.activeElement, rebuild, "focus stays inside the open dialog");
  tick();
  tick(1);
  assert.equal(dialog, undefined);
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
  paging = false;
  tick();
  tick(9);
  tick(9);
  assert.equal(bigScreen, false, "a held Menu press toggles once");
  play.focus();
  tick();
  tick(13);
  assert.equal(document.activeElement, play, "leave desktop navigation to the desktop view");
  tick();
  tick(9);
  assert.equal(bigScreen, true, "Menu also returns from the desktop view");

  document.dispatchEvent(new Event("workshop:closed"));
  assert.equal(callback, undefined);
});

test("launcher hints are replaced by the controller in use, and disconnects restore keyboard glyphs", () => {
  let callback, pads = [];
  const document = Object.assign(new EventTarget(), {
    body: {dataset: {controllerHint: "playstation"}},
    hidden: false, hasFocus: () => true, querySelector: () => null,
  });
  const window = Object.assign(new EventTarget(), {
    workshopI18n: {
      set: (node, key) => { node.textContent = key; },
      text: key => key.replace("builder.big_screen.", ""),
    },
  });
  runInNewContext(script, {
    document, window, Event, navigator: {getGamepads: () => pads},
    requestAnimationFrame: next => { callback = next; return 1; },
    cancelAnimationFrame() {},
  });
  const names = ["confirm", "back", "left", "right", "menu"];
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
  assert.deepEqual(hints.map(node => node.textContent), ["✕", "○", "L1", "R1", "☰"]);
  assert.equal(hints[4]["aria-label"], "options_button");

  const pad = (index, id, pressed) => ({
    index, id, mapping: "standard", axes: [0, 0],
    buttons: Array.from({length: 16}, (_, n) => ({pressed: pressed === n})),
  });
  pads = [pad(0, "XInput STANDARD GAMEPAD"), pad(1, "Wireless Controller (Vendor: 054c Product: 0ce6)")];
  callback(16);
  assert.equal(panel.dataset.padFamily, "xbox");
  assert.deepEqual(hints.map(node => node.textContent), ["A", "B", "LB", "RB", "☰"]);

  pads[1] = pad(1, pads[1].id, 0);
  callback(32);
  assert.equal(panel.dataset.padFamily, "playstation");
  assert.equal(hints[0]["aria-label"], "cross_button");
  pads[1] = null;
  callback(48);
  assert.equal(panel.dataset.padFamily, "xbox");

  pads = [pad(2, "Nintendo Switch Pro Controller")];
  callback(64);
  assert.deepEqual(hints.map(node => node.textContent), ["B", "A", "L", "R", "+"]);
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
