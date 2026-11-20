(() => {
  "use strict";

  window.workshopSaveArchive = async ({url, name}, browserDownload) => {
    const ui = window.workshopI18n, feedback = window.workshopFeedback;
    const response = await feedback.request("__shell/save-archive", {
      method: "POST", headers: {"Content-Type": "application/json"},
      body: JSON.stringify({url, name, title: ui.text("builder.file.save_archive")}),
    });
    if (response.status !== 404) {
      return (await feedback.readJSON(response)).saved ? "saved" : "cancelled";
    }
    if (browserDownload) {
      browserDownload();
    } else {
      const link = document.createElement("a");
      link.href = url;
      link.download = name;
      document.body.append(link);
      link.click();
      link.remove();
    }
    return "browser";
  };
})();
