(() => {
  "use strict";
  const glyphs = {
    keyboard: {confirm: ["Enter"], back: ["Esc"], west: ["X"], north: ["Y"], left: ["PgUp", "page_up"], right: ["PgDn", "page_down"]},
    generic: {
      confirm: ["↓", "bottom_button"], back: ["→", "right_button"],
      west: ["←", "left_button"], north: ["↑", "top_button"],
      left: ["L", "left_shoulder"], right: ["R", "right_shoulder"], menu: ["☰", "menu_button"],
    },
    xbox: {confirm: ["A"], back: ["B"], west: ["X"], north: ["Y"], left: ["LB"], right: ["RB"], menu: ["☰", "menu_button"]},
    playstation: {
      confirm: ["✕", "cross_button"], back: ["○", "circle_button"],
      west: ["□", "square_button"], north: ["△", "triangle_button"],
      left: ["L1"], right: ["R1"], menu: ["☰", "options_button"],
    },
    nintendo: {confirm: ["B"], back: ["A"], west: ["Y"], north: ["X"], left: ["L"], right: ["R"], menu: ["+", "plus_button"]},
  };

  function controllerFamily(id = "") {
    if (/playstation|dual[ -]?(?:sense|shock)|\bps[345]\b|\b054c\b/i.test(id)) return "playstation";
    if (/nintendo|joy-con|switch pro|\b057e\b/i.test(id)) return "nintendo";
    if (/x[ -]?box|xinput|steam (?:deck|virtual gamepad|controller)|\b045e\b/i.test(id)) return "xbox";
    return "";
  }

  window.workshopBigScreenNavigation = (panel, goBack, {isActive=()=>true, toggleView, navigationRoot=panel}={}) => {
    const pressed = new Map();
    const controls = "button, a[href], input[type=file], input[type=checkbox], input[type=radio], input[type=range], select[data-pad-picker], summary, [data-big-scroll][tabindex='0']";
    const hints = [...panel.querySelectorAll("[data-pad-button]"), ...document.querySelectorAll("dialog [data-pad-button]")];
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
      document.body.dataset.padFamily = family;
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

    // The topmost dialog can be earlier in DOM order or have no focused button.
    // Keep the order they opened so a controller never activates a dialog behind it.
    const openDialogs = new Set(document.querySelectorAll("dialog[open]"));
    const dialogObserver = new MutationObserver(records => {
      for (const {target} of records) {
        if (target.localName !== "dialog") continue;
        openDialogs.delete(target);
        if (target.open) openDialogs.add(target);
      }
    });
    dialogObserver.observe(document.body, {attributes:true, subtree:true, attributeFilter:["open"]});

    function availableControl(control) {
      if (control.disabled || !control.getClientRects().length) return false;
      // Collapsed details can still report layout boxes for their hidden controls.
      const collapsed = control.closest?.("details:not([open])");
      return !collapsed || (control.localName === "summary" && collapsed.firstElementChild === control);
    }
    function activeDialog() {
      let latest;
      for (const dialog of openDialogs) {
        if (dialog.open && dialog.isConnected) latest = dialog;
        else openDialogs.delete(dialog);
      }
      const focused = document.activeElement?.closest?.("dialog[open]");
      return focused && !openDialogs.has(focused) ? focused : latest;
    }
    const rowSelector = "[data-big-row], [data-big-band]";
    const rowOf = control => control.closest?.(rowSelector) || control;
    const actionOf = control => control?.dataset?.romAction || control?.dataset?.audioControl || control?.dataset?.modControl?.split(":", 1)[0];

    function controlRows(available) {
      const rows = [];
      for (const control of available) {
        const row = rows.at(-1);
        if (row && rowOf(row[0]) === rowOf(control)) row.push(control);
        else rows.push([control]);
      }
      return rows;
    }

    function matchingAction(source, from, to) {
      const action = actionOf(source);
      return (action && to.find(control => actionOf(control) === action)) ||
        to[Math.min(Math.max(0, from.indexOf(source)), to.length - 1)];
    }

    function move(direction) {
      const scope = activeDialog() || navigationRoot;
      const available = [...scope.querySelectorAll(controls)].filter(availableControl);
      if (!available.length) return;
      const focused = document.activeElement;
      const index = available.indexOf(focused);
      const step = direction === "up" || direction === "left" ? -1 : 1;
      const vertical = direction === "up" || direction === "down";
      if (index >= 0 && !vertical && focused.type === "range") {
        const value = focused.value;
        if (step < 0) focused.stepDown();
        else focused.stepUp();
        if (focused.value !== value) {
          focused.dispatchEvent(new Event("input", {bubbles: true}));
          focused.dispatchEvent(new Event("change", {bubbles: true}));
        }
        return;
      }
      if (!vertical && focused?.dataset?.bigSide) {
        const id = focused.dataset.bigSide.split(" ")[step < 0 ? 0 : 1];
        const target = available.find(control => control.id === id || control.closest?.("#" + id));
        if (target) {
          target.focus();
          return;
        }
      }
      const group = focused?.closest?.(rowSelector);
      if (index >= 0 && group) {
        if (vertical) {
          const rows = controlRows(available);
          const current = rows.findIndex(row => row.includes(focused));
          const next = rows[(current + step + rows.length) % rows.length];
          matchingAction(focused, rows[current], next).focus();
        } else {
          const row = available.filter(control => rowOf(control) === group);
          row[(row.indexOf(focused) + step + row.length) % row.length].focus();
        }
        return;
      }
      const next = index < 0 ? (step > 0 ? 0 : available.length - 1) :
        (index + step + available.length) % available.length;
      available[next].focus();
    }

    function back() {
      const dialog = activeDialog();
      if (!dialog) goBack();
      else if (dialog.dispatchEvent(new Event("cancel", {cancelable: true}))) dialog.close();
    }

    function shortcut(action) {
      const scope = activeDialog() || navigationRoot;
      const control = scope.querySelector(`[data-pad-action="${action}"]`);
      if (!control || !availableControl(control) || control.getAttribute("aria-disabled") === "true") return false;
      control.click();
      return true;
    }

    function scrollPage(direction) {
      const dialog = activeDialog();
      const scope = dialog || navigationRoot;
      const scrollable = [...scope.querySelectorAll("[data-big-scroll]:not([hidden])")].filter(node=>node.getClientRects().length);
      const log = scrollable.find(node=>node===document.activeElement) ||
        scrollable.find(node=>!node.hasAttribute?.("data-big-scroll-when-focused"));
      if (log) {
        log.scrollTop += direction * Math.max(48, log.clientHeight * .8);
        return true;
      }
      const list = [...scope.querySelectorAll("[data-big-page-list]:not([hidden])")].find(node=>node.getClientRects().length);
      const buttons = list ? [...list.querySelectorAll(controls)].filter(availableControl) : [];
      if (!buttons.length) {
        if (!dialog || dialog.scrollHeight <= dialog.clientHeight) return false;
        dialog.scrollTop += direction * Math.max(48, dialog.clientHeight * .8);
        return true;
      }
      const rows = controlRows(buttons);
      const focused = document.activeElement;
      const from = rows.findIndex(row => row.includes(focused));
      const height = Math.max(1, rowOf(buttons[0]).offsetHeight);
      const page = Math.max(1, Math.floor(list.clientHeight / height) - 1);
      const index = from < 0 ? (direction < 0 ? rows.length - 1 : 0) : from + direction * page;
      const next = rows[Math.min(rows.length - 1, Math.max(0, index))];
      matchingAction(focused, rows[from] || [], next).focus();
      return true;
    }

    function keydown(event) {
      if (event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey) return;
      if (event.key === "F8" && toggleView) {
        event.preventDefault();
        if (!event.repeat) toggleView();
        return;
      }
      if (!isActive()) return;
      const editable = event.target.matches("input:not([type=file]):not([type=checkbox]):not([type=radio]), select, textarea, [contenteditable]");
      if (editable && !activeDialog()) return;
      const action = {x: "west", y: "north"}[event.key.toLowerCase()];
      if (action && !editable && !event.repeat && shortcut(action)) {
        event.preventDefault();
        return;
      }
      if (event.key === "Escape") {
        event.preventDefault();
        if (!event.repeat) back();
        return;
      }
      if ((event.key === "PageUp" || event.key === "PageDown") &&
          scrollPage(event.key === "PageUp" ? -1 : 1)) {
        event.preventDefault();
        return;
      }
      const directions = {ArrowUp: "up", ArrowLeft: "left", ArrowDown: "down", ArrowRight: "right"};
      if (!editable && directions[event.key]) {
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
        const horizontal = Math.abs(x) > Math.abs(y);
        const axis = horizontal ? x : y;
        const stick = Math.abs(axis) < .55 ? "" :
          horizontal ? (x < 0 ? "left" : "right") : (y < 0 ? "up" : "down");
        const direction = ["up", "down", "left", "right"].find((_, i) => down(i + 12)) || stick;
        const confirm = down(0), cancel = down(1), menu = down(9);
        const west = down(2), north = down(3);
        const buttonActive = pad.buttons.some(button => button?.pressed);
        const scroll = down(4) ? -1 : down(5) ? 1 : 0;
        const previous = pressed.get(pad.index);
        const current = {direction, confirm, cancel, west, north, menu, scroll, buttonActive, repeatAt: previous?.repeatAt || 0};
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
        // Desktop keeps its usual page navigation, but modal choices can
        // still be made with a controller.
        if (!isActive() && !activeDialog()) continue;
        if (scroll && scroll !== previous.scroll) scrollPage(scroll);
        if (cancel && !previous.cancel) back();
        else if (confirm && !previous.confirm) {
          const target = document.activeElement;
          const scope = activeDialog() || navigationRoot;
          if (scope.contains(target) && target.matches(controls) && availableControl(target)) {
            if (target.type !== "range" && target.getAttribute?.("aria-disabled") !== "true") {
              if (target.hasAttribute?.("data-pad-picker")) target.dispatchEvent(new Event("workshop:pick"));
              else target.click();
            }
          }
          else move("down");
        } else if (isActive() && (west && !previous.west || north && !previous.north)) {
          shortcut(west && !previous.west ? "west" : "north");
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
      dialogObserver.disconnect();
      document.removeEventListener("keydown", keydown);
      document.removeEventListener("workshop:language", refreshPrompts);
      window.removeEventListener("blur", reset);
      document.removeEventListener("visibilitychange", reset);
    }, {once: true});
    if (typeof navigator.getGamepads === "function") frame = requestAnimationFrame(poll);
  };
})();
