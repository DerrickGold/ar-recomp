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
      onPreviewChanged,
      onAccepted
    }) {
      let preview = null;
      function render() {
        if (!preview) {
          $("accept-import").disabled = true;
          return;
        }
        const copying = $("import-keep-both").checked;
        $("project-conflict").hidden = !preview.existingProject;
        $("replace-project").closest("label").hidden = copying;
        label("accept-import", kind() === "clone" ? "builder.language.import_clone" : "builder.language.import_edit");
        $("accept-import").disabled = isBusy() || !!preview.existingProject && !copying && !$("replace-project").checked;
      }
      async function previewImport(endpoint, data) {
        preview = null;
        onPreviewChanged();
        feedbackKey("builder.language.loading_pack");
        preview = await json(endpoint, data);
        $("replace-project").checked = false;
        $("import-keep-both").checked = false;
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
        label("import-warning", "builder.language.import_kept");
        onPreviewChanged();
        feedback("");
        $("import-preview").scrollIntoView({block: "nearest"});
        $("import-name").focus({preventScroll: true});
      }
      async function loadDirectory(directory) {
        if (!directory.trim() || !await discard())
          return;
        await previewImport("directory", {directory, previewImport: true});
      }
      $("pick-directory").addEventListener("click", () => run(async () => {
        const result = await json("choose-directory", {});
        if (result.cancelled) return;
        $("directory").elements.directory.value = result.directory;
        await loadDirectory(result.directory);
      }));
      submit("directory", data => loadDirectory(data.get("directory")));
      $("directory").elements.directory.addEventListener("change", event => {
        const path = event.target.value;
        run(() => loadDirectory(path));
      });
      function importFiles(files, backup) {
        if (isClosed() || isBusy() || !files.length) return Promise.resolve();
        const file = files[0], extension = file.name.split(".").pop().toLowerCase();
        return run(async () => {
          if (files.length !== 1) throw failure("builder.language.one_archive");
          if (!(backup ? ["arproject", "zip"] : ["o2r", "arlang"]).includes(extension))
            throw failure(backup ? "builder.language.backup_types" : "builder.language.package_types");
          if (!file.size || file.size > 257 * 1024 * 1024)
            throw failure("builder.language.archive_size");
          if (!await discard()) return;
          const data = new FormData();
          data.set("file", file);
          data.set("intent", "preview");
          await previewImport("import", data);
        });
      }
      for (const id of ["import", "import-backup"]) {
        const form = $(id), input = form.elements.file;
        form.addEventListener("submit", event => event.preventDefault());
        input.addEventListener("change", () => importFiles([...input.files], id === "import-backup").finally(() => {
          input.value = "";
          window.workshopFileInputs?.refresh(input);
        }));
      }
      const authorImport = $("import-source");
      authorImport.addEventListener("dragover", event => event.preventDefault());
      authorImport.addEventListener("drop", event => {
        if (event.target?.closest?.("input[type=file],.file-control")) return;
        const files = [...(event.dataTransfer?.files || [])];
        event.preventDefault();
        return importFiles(files, /\.(arproject|zip)$/i.test(files[0]?.name));
      });
      for (const id of ["replace-project", "import-keep-both"])
        $(id).addEventListener("input", render);
      async function acceptImport() {
        if (!preview)
          return;
        const incoming = preview, keepBoth = $("import-keep-both").checked;
        if (incoming.existingProject && !keepBoth && !$("replace-project").checked)
          throw failure("builder.language.choose_conflict");
        const next = await json("accept-import", {
          importToken: incoming.token,
          keepBoth,
          replace: !keepBoth && $("replace-project").checked,
          expected: incoming.existingProject?.revision || ""
        });
        await onAccepted(next);
      }
      $("accept-import").addEventListener("click", () => run(acceptImport));
      return {
        render,
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
