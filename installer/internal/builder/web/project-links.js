(() => {
  "use strict";
  const status = document.getElementById("project-link-status");
  document.querySelectorAll("[data-project-link]").forEach(link => {
    link.addEventListener("click", async event => {
      // Preserve normal browser gestures for opening/copying an anchor.
      if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      event.preventDefault();
      status.hidden = true;
      window.workshopFeedback.clear(status);
      try {
        const response = await window.workshopFeedback.request(
          "project-links/" + link.dataset.projectLink, {method: "POST"});
        await window.workshopFeedback.readJSON(response);
      } catch (error) {
        status.hidden = false;
        window.workshopI18n.set(status, "builder.links.open_failed");
        window.workshopFeedback.show(status, error);
      }
    });
  });
})();
