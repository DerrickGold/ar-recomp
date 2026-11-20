(() => {
  "use strict";

  window.workshopLanguageLibrary = {
    create({$, ui, label, contentLanguage, openProject}) {
      let projects = [];

      function render() {
        const search = $("library-search").value.trim().toLocaleLowerCase();
        const rows = projects.filter(project =>
          [project.name, project.locale, project.id].join(" ").toLocaleLowerCase().includes(search));
        $("library-empty").hidden = rows.length > 0;
        label("library-empty", search ? "builder.language.no_packages" : "builder.language.empty_projects");
        $("library").replaceChildren(...rows.map(project => {
          const card = document.createElement("article");
          card.className = "loc-package-card";
          card.dataset.packageId = project.id;
          const title = document.createElement("h3");
          title.textContent = project.name || project.id;
          contentLanguage(title, {locale: project.locale});
          const info = document.createElement("p");
          info.className = "loc-help";
          info.textContent = [project.locale, project.id].filter(Boolean).join(" · ");
          card.append(title, info);
          if (project.error) {
            const error = document.createElement("p");
            error.textContent = project.error;
            card.append(error);
          } else {
            const open = document.createElement("button");
            open.type = "button";
            ui.set(open, project.origin === "native-source" ?
              "builder.language.view_reference" : "builder.language.edit_project");
            open.addEventListener("click", () => openProject(project.id));
            card.append(open);
          }
          return card;
        }));
      }
      $("library-search").addEventListener("input", render);
      return {
        show(rows) {
          projects = rows;
          label("library-heading", "builder.language.project_count", {count: projects.length});
          render();
        },
      };
    },
  };
})();
