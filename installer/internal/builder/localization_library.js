(() => {
  "use strict";
  // The library owns its catalog snapshot and uninstall selection. Opening a
  // project crosses back to the coordinator's unsaved-edit guard.
  window.workshopLanguageLibrary = {
    create({$, ui, label, contentLanguage, run, json, feedbackKey, isBusy, isClosed, openProject}) {
      let catalog = [];
      async function loadCatalog(focusKey) {
        catalog = await json("catalog");
        renderCatalog(focusKey);
      }
      let uninstallPack = null;
      function reviewUninstall(pack) {
        if (isBusy() || isClosed())
          return;
        uninstallPack = pack;
        label(
            "uninstall-question", "builder.language.uninstall_confirm",
            {name: pack.installedName, id: pack.id, folder: pack.key});
        // Embedded webviews do not consistently implement window.confirm().
        $("uninstall-dialog").showModal();
        $("uninstall-cancel").focus();
      }
      $("uninstall-cancel").addEventListener("click", () => $("uninstall-dialog").close());
      $("uninstall-dialog").addEventListener("close", () => {
        uninstallPack = null;
      });
      $("uninstall-confirm").addEventListener("click", async () => {
        if (!uninstallPack || isBusy() || isClosed())
          return;
        const pack = uninstallPack;
        uninstallPack = null;
        $("uninstall-dialog").close();
        let removed = false;
        await run(async () => {
          const result = await json("uninstall", {
            id: pack.id,
            directory: pack.key,
            expected: pack.installedRevision,
            confirmUninstall: true
          });
          await loadCatalog();
          removed = true;
          feedbackKey("builder.language.uninstalled", {backup: result.backup});
        });
        if (removed && !isClosed())
          $("library-search").focus({preventScroll: true});
      });
      function renderCatalog(focusKey) {
        const query = $("library-search").value.trim().toLocaleLowerCase(),
              filter = $("library-filter").value;
        const rows = catalog.filter(
            p => [p.name, p.installedName, p.locale, p.id].join(" ").toLocaleLowerCase().includes(
                     query) &&
                (filter === "all" || filter === "installed" && p.installed ||
                 filter === "workshop" && !p.installed));
        $("library-empty").hidden = rows.length > 0;
        label("library-summary", "builder.language.library_counts", {
          enabled: catalog.filter(p => p.installed && p.enabled).length,
          disabled: catalog.filter(p => p.installed && !p.enabled).length,
          projects: catalog.filter(p => p.project).length
        });
        label(
            "library-empty",
            filter === "installed" && !catalog.some(p => p.installed) ?
                "builder.language.library_empty" :
                "builder.language.no_packages");
        $("library").replaceChildren(...rows.map(p => {
          const card = document.createElement("article");
          card.className = "loc-package-card";
          card.dataset.packageId = p.id;
          card.dataset.packageKey = p.key || "";
          const title = document.createElement("h3");
          title.textContent = p.name;
          contentLanguage(title, {locale: p.locale});
          const badge = document.createElement("span");
          badge.className = "loc-package-badge";
          badge.dataset.error = String(!!p.error);
          if (p.error || !p.installed)
            ui.set(
                badge,
                p.error        ? "builder.language.needs_attention" :
                    p.readOnly ? "builder.language.read_only" :
                                 "builder.language.workshop_only");
          const info = document.createElement("p");
          info.className = "loc-help";
          info.textContent = (p.locale ? p.locale + " · " : "") + p.id;
          if (p.installed && p.installedName !== p.name) {
            const name = document.createElement("span");
            ui.set(name, "builder.language.installed_as", {name: p.installedName});
            info.append(name);
          }
          if (p.installed && p.key !== p.id) {
            const folder = document.createElement("span");
            ui.set(folder, "builder.language.folder", {folder: p.key});
            info.append(folder);
          }
          if (p.error || !p.installed)
            card.append(badge);
          card.append(title, info);
          if (p.installed) {
            const label = document.createElement("label");
            label.className = "loc-package-enable";
            const checkbox = document.createElement("input");
            checkbox.type = "checkbox";
            checkbox.checked = p.enabled;
            checkbox.disabled = !p.installedRevision || (!p.enabled && !!p.error);
            ui.attribute(
                checkbox, "aria-label", "builder.language.enable_pack", {name: p.installedName});
            const caption = document.createElement("span");
            ui.set(
                caption,
                p.enabled ? "builder.language.enabled_game" : "builder.language.disabled_game");
            label.append(checkbox, caption);
            card.prepend(label);
            checkbox.addEventListener("change", () => {
              if (isBusy()) {
                checkbox.checked = p.enabled;
                return;
              }
              const enabled = checkbox.checked;
              run(async () => {
                try {
                  await json(
                      "set-enabled",
                      {id: p.id, directory: p.key, expected: p.installedRevision, enabled});
                  feedbackKey("builder.language.availability_saved");
                } finally {
                  await loadCatalog(p.key);
                }
              });
            });
          }
          const actions = document.createElement("div");
          actions.className = "loc-row";
          const action = (key, handler) => {
            const b = document.createElement("button");
            b.type = "button";
            ui.set(b, key);
            b.addEventListener("click", handler);
            actions.append(b);
            return b;
          };
          if (p.project) {
            action(
                p.readOnly ? "builder.language.view_reference" : "builder.language.edit_project",
                () => openProject(p.id));
            if (!p.readOnly)
              action(
                  p.installed ? "builder.language.review_update" :
                                "builder.language.review_install",
                  () => openProject(p.id, true));
          }
          if (p.installed && p.installedRevision)
            action("builder.language.uninstall", () => reviewUninstall(p)).className =
                "loc-uninstall";
          if (p.error) {
            const error = document.createElement("p");
            error.className = "loc-help";
            error.textContent = p.error;
            card.append(error);
          } else if (!p.project) {
            const note = document.createElement("p");
            note.className = "loc-help";
            ui.set(note, "builder.language.no_project");
            card.append(note);
          }
          card.append(actions);
          return card;
        }));
        if (typeof focusKey === "string")
          [...$("library").children]
              .find(row => row.dataset.packageKey === focusKey)
              ?.querySelector("input[type=\"checkbox\"]")
              ?.focus({preventScroll: true});
      }
      $("library-search").addEventListener("input", renderCatalog);
      $("library-filter").addEventListener("change", renderCatalog);
      return {
        refresh: loadCatalog,
        close() {
          uninstallPack = null;
          $("uninstall-dialog").close();
        },
      };
    }
  };
})();
