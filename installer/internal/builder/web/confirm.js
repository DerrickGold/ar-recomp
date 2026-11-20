(() => {
  "use strict";
  const dialog = document.getElementById("builder-confirm-dialog");
  const ui = window.workshopI18n;
  let pending;
  dialog.addEventListener("close", () => {
    const resolve = pending;
    pending = null;
    resolve?.(dialog.returnValue === "accept");
  });
  window.workshopConfirm = (action, message, args = {}) => {
    if (pending) return Promise.resolve(false);
    ui.set(document.getElementById("builder-confirm-title"), action);
    ui.set(document.getElementById("builder-confirm-message"), message, args);
    ui.set(document.getElementById("builder-confirm-accept"), action);
    const trigger = document.activeElement;
    dialog.returnValue = "";
    dialog.showModal();
    return new Promise(resolve => {
      pending = accepted => {
        // Opening a modal closes the editor's disclosure menus. Restore a
        // visible target when the browser cannot refocus the original button.
        if (document.activeElement === document.body && trigger?.isConnected) {
          const disclosure = trigger.closest("details:not([open])");
          (disclosure?.querySelector("summary") || trigger).focus({preventScroll: true});
        }
        resolve(accepted);
      };
    });
  };
})();
