(() => {
  "use strict";
  const glyphs = {
    keyboard: {confirm: ["Enter"], back: ["Esc"], left: ["PgUp", "page_up"], right: ["PgDn", "page_down"]},
    generic: {
      confirm: ["↓", "bottom_button"], back: ["→", "right_button"],
      left: ["L", "left_shoulder"], right: ["R", "right_shoulder"], menu: ["☰", "menu_button"],
    },
    xbox: {confirm: ["A"], back: ["B"], left: ["LB"], right: ["RB"], menu: ["☰", "menu_button"]},
    playstation: {
      confirm: ["✕", "cross_button"], back: ["○", "circle_button"],
      left: ["L1"], right: ["R1"], menu: ["☰", "options_button"],
    },
    nintendo: {confirm: ["B"], back: ["A"], left: ["L"], right: ["R"], menu: ["+", "plus_button"]},
  };

  function controllerFamily(id = "") {
    if (/playstation|dual[ -]?(?:sense|shock)|\bps[345]\b|\b054c\b/i.test(id)) return "playstation";
    if (/nintendo|joy-con|switch pro|\b057e\b/i.test(id)) return "nintendo";
    if (/x[ -]?box|xinput|steam (?:deck|virtual gamepad|controller)|\b045e\b/i.test(id)) return "xbox";
    return "";
  }

  window.workshopBigScreenNavigation = (panel, goBack, {isActive=()=>true, toggleView, navigationRoot=panel}={}) => {
    const pressed = new Map();
    const controls = "button, input[type=file], input[type=checkbox], summary";
    const hints = [...panel.querySelectorAll("[data-pad-button]")];
    const moveHint = panel.querySelector("#big-screen-move");
    const launcherHint = document.body.dataset.controllerHint;
    const hint = ["xbox", "playstation", "nintendo"].includes(launcherHint) ? launcherHint : "";
    const ui = window.workshopI18n;
    let shownFamily, activePad, sawPad = false;
    let frame = 0;

    function showPrompts(family) {
      shownFamily = family;
      if (!hints.length) return;
      panel.dataset.padFamily = family;
      ui.set(moveHint, family === "keyboard" ? "builder.big_screen.move_keyboard" : "builder.big_screen.move_gamepad");
      for (const node of hints) {
        const glyph = glyphs[family][node.dataset.padButton];
        node.hidden = !glyph;
        if (!glyph) continue;
        const [symbol, label] = glyph;
        node.textContent = symbol;
        node.setAttribute("aria-label", label ? ui.text("builder.big_screen." + label) : symbol);
      }
    }

    function move(direction) {
      const scope = document.querySelector("dialog[open]") || navigationRoot;
      const available = [...scope.querySelectorAll(controls)].filter(control =>
        !control.disabled && control.getClientRects().length > 0);
      if (!available.length) return;
      const index = available.indexOf(document.activeElement);
      const next = index < 0 ? (direction > 0 ? 0 : available.length - 1) :
        (index + direction + available.length) % available.length;
      available[next].focus();
    }

    function back() {
      const dialog = document.querySelector("dialog[open]");
      if (!dialog) goBack();
      else if (dialog.dispatchEvent(new Event("cancel", {cancelable: true}))) dialog.close();
    }

    function scrollLog(direction) {
      const scope = document.querySelector("dialog[open]") || navigationRoot;
      const log = [...scope.querySelectorAll("[data-big-scroll]:not([hidden])")].find(node=>node.getClientRects().length);
      if (log) {
        log.scrollTop += direction * Math.max(48, log.clientHeight * .8);
        return true;
      }
      const list = [...scope.querySelectorAll("[data-big-page-list]:not([hidden])")].find(node=>node.getClientRects().length);
      if (!list) return false;
      const buttons = [...list.querySelectorAll("button:not(:disabled)")];
      if (!buttons.length) return false;
      const from = buttons.indexOf(document.activeElement);
      const row = Math.max(1, buttons[0].offsetHeight);
      const page = Math.max(1, Math.floor(list.clientHeight / row) - 1);
      const index = from < 0 ? (direction < 0 ? buttons.length - 1 : 0) : from + direction * page;
      buttons[Math.min(buttons.length - 1, Math.max(0, index))].focus();
      return true;
    }

    function keydown(event) {
      if (event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey) return;
      if (event.key === "F8" && toggleView) {
        event.preventDefault();
        if (!event.repeat) toggleView();
        return;
      }
      if (!isActive() ||
          event.target.matches("input:not([type=file]):not([type=checkbox]), select, textarea, [contenteditable]")) return;
      if ((event.key === "PageUp" || event.key === "PageDown") &&
          scrollLog(event.key === "PageUp" ? -1 : 1)) {
        event.preventDefault();
        return;
      }
      const directions = {ArrowUp: -1, ArrowLeft: -1, ArrowDown: 1, ArrowRight: 1};
      if (event.key === "Escape") {
        event.preventDefault();
        if (!event.repeat) back();
      } else if (directions[event.key]) {
        event.preventDefault();
        move(directions[event.key]);
      }
    }

    function poll(now) {
      frame = requestAnimationFrame(poll);
      if (document.hidden || !document.hasFocus()) return;
      let gamepads;
      try { gamepads = Array.from(navigator.getGamepads()).filter(pad => pad?.mapping === "standard"); }
      catch {
        // A webview can block the API. Keyboard and mouse still use the same controls.
        cancelAnimationFrame(frame);
        return;
      }

      const connected = new Set();
      for (const pad of gamepads) {
        const down = index => !!pad.buttons[index]?.pressed;
        const [x = 0, y = 0] = pad.axes;
        const axis = Math.abs(x) > Math.abs(y) ? x : y;
        const direction = down(12) || down(14) ? -1 : down(13) || down(15) ? 1 :
          Math.abs(axis) >= .55 ? Math.sign(axis) : 0;
        const confirm = down(0), cancel = down(1), menu = down(9);
        const buttonActive = pad.buttons.some(button => button?.pressed);
        const scroll = down(4) ? -1 : down(5) ? 1 : 0;
        const previous = pressed.get(pad.index);
        const current = {direction, confirm, cancel, menu, scroll, buttonActive, repeatAt: previous?.repeatAt || 0};
        pressed.set(pad.index, current);
        connected.add(pad.index);
        if ((direction && direction !== previous?.direction) || (buttonActive && !previous?.buttonActive)) {
          activePad = pad.index;
        }
        // Wait for a fresh press after launch or refocus. The button that
        // opened the Builder can still be held when its window receives focus.
        if (!previous) {
          current.repeatAt = Infinity;
          continue;
        }
        if (menu && !previous.menu && toggleView) {
          toggleView();
          continue;
        }
        if (!isActive()) continue;
        if (scroll && scroll !== previous.scroll) scrollLog(scroll);
        if (cancel && !previous.cancel) back();
        else if (confirm && !previous.confirm) {
          const target = document.activeElement;
          const scope = document.querySelector("dialog[open]") || navigationRoot;
          if (scope.contains(target) && target.matches(controls) && !target.disabled &&
              target.getClientRects().length) target.click();
          else move(1);
        } else if (direction && (direction !== previous.direction || now >= current.repeatAt)) {
          move(direction);
          current.repeatAt = now + (direction === previous.direction ? 135 : 320);
        }
      }
      for (const index of pressed.keys()) {
        if (!connected.has(index)) pressed.delete(index);
      }
      const pad = gamepads.find(gamepad => gamepad.index === activePad) || gamepads[0];
      if (pad) {
        activePad = pad.index;
        sawPad = true;
      } else activePad = undefined;
      const family = pad ? controllerFamily(pad.id) || hint || "generic" : (!sawPad && hint) || "keyboard";
      if (shownFamily !== family) showPrompts(family);
    }

    showPrompts(hint || "keyboard");
    const refreshPrompts = () => showPrompts(shownFamily);
    document.addEventListener("keydown", keydown);
    document.addEventListener("workshop:language", refreshPrompts);
    const reset = () => pressed.clear();
    window.addEventListener("blur", reset);
    document.addEventListener("visibilitychange", reset);
    document.addEventListener("workshop:closed", () => {
      cancelAnimationFrame(frame);
      document.removeEventListener("keydown", keydown);
      document.removeEventListener("workshop:language", refreshPrompts);
      window.removeEventListener("blur", reset);
      document.removeEventListener("visibilitychange", reset);
    }, {once: true});
    if (typeof navigator.getGamepads === "function") frame = requestAnimationFrame(poll);
  };
})();
