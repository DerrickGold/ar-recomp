(() => {
  "use strict";
  // Owns the detached import preview, conflict controls, directory/archive
  // pickers and drag-and-drop state. Project adoption stays in the coordinator.
  window.workshopLanguageImport = {
    create({
      $,
      label,
      raw,
      feedback,
      feedbackKey,
      failure,
      json,
      run,
      submit,
      discard,
      kind,
      isBusy,
      isClosed,
      beginInstall,
      onPreviewChanged,
      onAccepted
    }) {
      let preview = null;
      function render() {
        $("import-upgrade").hidden = kind() !== "install" || !preview?.upgrade;
        if (!preview) {
          $("accept-import").disabled = true;
          return;
        }
        const copying = !!$("import-new-id").value.trim(), installing = kind() === "install";
        $("project-conflict").hidden = !preview.existingProject;
        $("replace-project").closest("label").hidden = copying;
        $("import-install-conflict").hidden = !installing || !preview.installed || copying;
        label(
            "accept-import",
            installing             ? "builder.language.import_install" :
                kind() === "clone" ? "builder.language.import_clone" :
                                     "builder.language.import_edit");
        $("accept-import").disabled = isBusy() || installing && !!preview.installError ||
            !!preview.existingProject && !copying && !$("replace-project").checked ||
            installing && !!preview.installed && !copying && !$("import-replace-installed").checked;
      }
      async function previewImport(endpoint, data) {
        preview = null;
        onPreviewChanged();
        feedbackKey("builder.language.loading_pack");
        preview = await json(endpoint, data);
        $("replace-project").checked = $("import-replace-installed").checked = false;
        $("import-new-id").value = "";
        const p = preview, m = p.metadata;
        $("import-name").textContent = m.name;
        label(
            "import-summary", "builder.language.import_summary",
            {locale: m.locale, id: m.id, count: p.messages, author: m.author, license: m.license});
        if (p.existingProject)
          label(
              "project-conflict-note", "builder.language.project_conflict",
              {id: m.id, name: p.existingProject.name});
        else
          raw("project-conflict-note", "");
        label(
            "import-warning",
            p.installError ? "builder.language.import_not_installable" :
                             "builder.language.import_kept",
            {detail: p.installError});
        onPreviewChanged();
        feedback("");
        $("import-preview").scrollIntoView({block: "nearest"});
        $("import-name").focus({preventScroll: true});
      }
      async function loadDirectory(directory) {
        if (!directory.trim() || !discard())
          return;
        await previewImport("directory", {directory, previewImport: true});
      }
      $("pick-directory").addEventListener("click", () => run(async () => {
                                                      if (!discard())
                                                        return;
                                                      const result =
                                                          await json("choose-directory", {});
                                                      if (result.cancelled)
                                                        return;
                                                      $("directory").elements.directory.value =
                                                          result.directory;
                                                      await loadDirectory(result.directory);
                                                    }));
      submit("directory", data => loadDirectory(data.get("directory")));
      $("directory").elements.directory.addEventListener("change", event => {
        const path = event.target.value;
        run(() => loadDirectory(path));
      });
      let dragDepth = 0;
      function clearDrop() {
        dragDepth = 0;
        $("drop-overlay").hidden = true;
      }
      function dropNotice(key) {
        label("drop-message", key);
        $("drop-overlay").dataset.notice = "true";
        $("dismiss-drop").hidden = false;
        $("drop-overlay").hidden = false;
      }
      $("dismiss-drop").addEventListener("click", clearDrop);
      function importFiles(files, install = false, backup = false) {
        if (isClosed() || !files.length)
          return Promise.resolve();
        if (isBusy()) {
          dropNotice("builder.language.drop_busy");
          return Promise.resolve();
        }
        // One detached server preview per session; never silently install only the
        // first item of a multi-file drop or overwrite a preview still loading.
        if (files.length !== 1) {
          dropNotice("builder.language.one_archive");
          return Promise.resolve();
        }
        const file = files[0], extension = file.name.split(".").pop().toLowerCase();
        if (!(backup ? ["arproject", "zip"] : ["arlang"]).includes(extension)) {
          dropNotice(backup ? "builder.language.backup_types" : "builder.language.package_types");
          return Promise.resolve();
        }
        if (!file.size || file.size > 257 * 1024 * 1024) {
          dropNotice("builder.language.archive_size");
          return Promise.resolve();
        }
        return run(async () => {
          if (!discard())
            return;
          if (install) {
            if (!await beginInstall()) {
              dropNotice("builder.language.build_required");
              return;
            }
          }
          clearDrop();
          const data = new FormData();
          data.set("file", file);
          data.set("intent", "preview");
          await previewImport("import", data);
        });
      }
      for (const id of ["quick-import", "import", "import-backup"]) {
        const form = $(id), input = form.elements.file;
        form.addEventListener("submit", event => event.preventDefault());
        input.addEventListener(
            "change",
            () => importFiles([...input.files], id === "quick-import", id === "import-backup")
                      .finally(() => {
                        // A failed/cancelled selection must be selectable again without first
                        // choosing a different file. The preview already owns its uploaded bytes.
                        input.value = "";
                        window.workshopFileInputs?.refresh(input);
                      }));
      }
      const fileDrag = event => [...(event.dataTransfer?.types || [])].includes("Files") ||
          event.dataTransfer?.files?.length > 0;
      const fileTarget = event => event.target?.closest?.("input[type=\"file\"],.file-control");
      document.addEventListener("dragenter", event => {
        if (!fileDrag(event) || fileTarget(event))
          return;
        event.preventDefault();
        ++dragDepth;
        if (isClosed() || isBusy())
          return;
        label("drop-message", "builder.language.drop_prompt");
        $("drop-overlay").dataset.notice = "false";
        $("dismiss-drop").hidden = true;
        $("drop-overlay").hidden = false;
      });
      document.addEventListener("dragover", event => {
        if (!fileDrag(event))
          return;
        if (fileTarget(event)) {
          clearDrop();
          return;
        }
        event.preventDefault();
        event.dataTransfer.dropEffect = isClosed() || isBusy() ? "none" : "copy";
      });
      document.addEventListener("dragleave", event => {
        if (!fileDrag(event))
          return;
        if (--dragDepth <= 0)
          clearDrop();
      });
      document.addEventListener("dragend", clearDrop);
      document.addEventListener("drop", event => {
        clearDrop();
        if (!fileDrag(event) || fileTarget(event))
          return;  // ROM/font controls keep native drop behavior.
        event.preventDefault();
        return importFiles([...(event.dataTransfer.files || [])], true);
      });
      document.addEventListener("keydown", event => {
        if (event.key === "Escape")
          clearDrop();
      });
      for (const id of ["replace-project", "import-new-id", "import-replace-installed"])
        $(id).addEventListener("input", render);
      async function acceptImport() {
        if (!preview)
          return;
        const incoming = preview, newID = $("import-new-id").value.trim();
        const replaceInstall = !newID && $("import-replace-installed").checked;
        if (incoming.existingProject && !newID && !$("replace-project").checked)
          throw failure("builder.language.choose_conflict");
        if (kind() === "install" && incoming.installed && !newID && !replaceInstall)
          throw failure("builder.language.confirm_replace");
        const next = await json("accept-import", {
          importToken: incoming.token,
          newID,
          replace: !newID && $("replace-project").checked,
          expected: incoming.existingProject?.revision || ""
        });
        await onAccepted(next, replaceInstall);
      }
      $("accept-import").addEventListener("click", () => run(acceptImport));
      return {
        render,
        clearDrop,
        reset() {
          preview = null;
        },
        get preview() {
          return preview;
        },
      };
    }
  };
})();
