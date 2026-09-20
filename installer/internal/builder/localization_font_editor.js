(() => {
  "use strict";
  // Font roles, fallback order and pending files belong to this editor. The
  // coordinator owns requests and decides when a project draft is saved.
  window.workshopLanguageFonts = {
    create({$, ui, phrase, option, keyedOption, feedbackKey, onChange, onRender}) {
      let saved = {}, role = "body", stacks = {}, stack = [], dirty = false;
      const uploads = new Map();
      function load(fonts) {
        saved = fonts;
        role = "body";
        stacks =
            Object.assign(Object.create(null), {body: [fonts.primary, ...(fonts.fallback || [])]});
        for (const fontRole of fonts.roles || [])
          stacks[fontRole.name] = [fontRole.primary, ...(fontRole.fallback || [])];
        stack = stacks.body;
        clearDirty();
        $("font-report").replaceChildren();
        renderFonts();
      }
      function clearDirty() {
        dirty = false;
        uploads.clear();
      }
      $("font-role").addEventListener("change", () => {
        role = $("font-role").value;
        stack = stacks[role];
        renderFonts();
      });
      $("font-role-add").addEventListener("click", () => {
        const name = $("font-role-name").value.trim();
        if (!/^[a-z][a-z0-9_.-]*$/.test(name) || stacks[name] || Object.keys(stacks).length >= 9) {
          feedbackKey("builder.playback.role_invalid", {}, true);
          return;
        }
        stacks[name] = [...stacks.body];
        role = name;
        stack = stacks[name];
        $("font-role-name").value = "";
        fontChanged();
      });
      $("font-role-remove").addEventListener("click", () => {
        if (role === "body")
          return;
        delete stacks[role];
        role = "body";
        stack = stacks.body;
        fontChanged();
      });
      function fontChanged() {
        dirty = true;
        for (const name of uploads.keys())
          if (!Object.values(stacks).some(stack => stack.includes(name)))
            uploads.delete(name);
        $("font-report").replaceChildren();
        renderFonts();
        onChange();
      }
      function renderFonts() {
        $("font-role").replaceChildren(...Object.keys(stacks).map(name => option(name, name)));
        $("font-role").value = role;
        $("font-role-remove").disabled = role === "body";
        const known = [...new Set([
          "builtin:actraiser-sans", saved.primary, ...(saved.fallback || []), ...uploads.keys(),
          ...Object.values(stacks).flat()
        ].filter(Boolean))];
        $("font-list").replaceChildren(...stack.map((reference, index) => {
          const row = document.createElement("li");
          row.className = "loc-font-row";
          const label = document.createElement("label");
          label.append(phrase(
              index ? "builder.editor.fallback_number" : "builder.editor.primary_font",
              {number: index}));
          const select = document.createElement("select");
          select.required = true;
          select.append(keyedOption("", "builder.editor.choose_font"));
          for (const name of known)
            select.append(
                name === "builtin:actraiser-sans" ?
                    keyedOption(name, "builder.editor.bundled_font") :
                    option(name, name));
          select.value = reference;
          select.addEventListener("change", () => {
            stack[index] = select.value;
            fontChanged();
          });
          label.append(select);
          row.append(label);
          if (uploads.has(reference))
            label.append(phrase("builder.editor.pending_font", {}, "small"));
          const fileLabel = document.createElement("label");
          fileLabel.append(phrase("builder.editor.choose_font_file"));
          const file = document.createElement("input");
          file.type = "file";
          file.accept = ".ttf,.otf";
          file.addEventListener("change", () => {
            const chosen = file.files[0];
            if (!chosen)
              return;
            if (!chosen.size || chosen.size > 64 * 1024 * 1024) {
              feedbackKey("builder.editor.font_size_error", {}, true);
              file.value = "";
              return;
            }
            const path = "fonts/" + chosen.name;
            uploads.set(path, chosen);
            stack[index] = path;
            fontChanged();
          });
          fileLabel.append(file);
          row.append(fileLabel);
          window.workshopFileInputs?.enhance(file);
          const actions = document.createElement("div");
          actions.className = "loc-row";
          for (const [key, aria, emptyAria, offset] of [
                   [
                     "builder.editor.move_up", "builder.editor.move_up_font",
                     "builder.editor.move_up_slot", -1
                   ],
                   [
                     "builder.editor.move_down", "builder.editor.move_down_font",
                     "builder.editor.move_down_slot", 1
                   ],
                   [
                     "builder.editor.remove", "builder.editor.remove_font",
                     "builder.editor.remove_slot", 0
                   ]]) {
            const button = phrase(key, {}, "button");
            button.type = "button";
            ui.attribute(
                button, "aria-label", reference ? aria : emptyAria,
                {font: reference, number: index + 1});
            button.dataset.unavailable = String(
                offset ? index + offset < 0 || index + offset >= stack.length : stack.length === 1);
            button.disabled = button.dataset.unavailable === "true";
            button.addEventListener("click", () => {
              if (offset)
                [stack[index], stack[index + offset]] = [stack[index + offset], stack[index]];
              else
                stack.splice(index, 1);
              fontChanged();
            });
            actions.append(button);
          }
          row.append(actions);
          return row;
        }));
        onRender();
      }
      $("font-add").addEventListener("click", () => {
        if (stack.length < 9) {
          stack.push("");
          fontChanged();
          $("font-list").lastElementChild.querySelector("select").focus();
        }
      });
      function draftFonts() {
        const fonts = {primary: stacks.body[0], fallback: stacks.body.slice(1)};
        const roles =
            Object.entries(stacks)
                .filter(([name]) => name !== "body")
                .map(([name, stack]) => ({name, primary: stack[0], fallback: stack.slice(1)}));
        if (roles.length)
          fonts.roles = roles;
        return fonts;
      }
      function draftFontUploads() {
        return [...uploads].filter(
            ([name]) => Object.values(stacks).some(stack => stack.includes(name)));
      }
      function fontPayload(q) {
        const pending = draftFontUploads();
        if (!q.saveFonts || !pending.length)
          return q;
        q.fontPaths = pending.map(([name]) => name);
        const payload = new FormData();
        payload.set("request", JSON.stringify(q));
        pending.forEach(([, file], i) => payload.set("font" + i, file));
        return payload;
      }
      function showCoverage(result) {
        const summary = phrase(
            result.complete ? "builder.editor.coverage_complete" :
                              "builder.editor.coverage_missing",
            {count: result.complete ? result.scalars : result.missingCount}, "p");
        const list = document.createElement("ul");
        for (const gap of result.missing) {
          const item = document.createElement("li");
          item.textContent = gap.codepoint + " “" + gap.character + "” — " +
              gap.locations
                  .map(
                      loc => loc.messageID ? loc.messageID + " · " + loc.source + ":" + loc.line :
                                             loc.source)
                  .join("; ");
          list.append(item);
        }
        const values = phrase(
            result.dynamicValues.length ? "builder.editor.unresolved_values" :
                                          "builder.editor.no_unresolved_values",
            {values: result.dynamicValues.join(", ")}, "p");
        values.className = "loc-help";
        $("font-report").replaceChildren(summary, list, values);
      }
      return {
        load,
        clearDirty,
        showCoverage,
        payload: fontPayload,
        fonts: draftFonts,
        uploads: draftFontUploads,
        get dirty() {
          return dirty;
        },
        get roles() {
          return Object.keys(stacks);
        },
        get stackLength() {
          return stack.length;
        },
      };
    }
  };
})();
