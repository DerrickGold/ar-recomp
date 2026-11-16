(() => {
  "use strict";

  window.workshopBigScreenCustomize = (request, openDesktop, onClose) => {
    const ui = window.workshopI18n;
    const $ = id => document.getElementById(id);
    const section = $("big-screen-customize"), menu = $("big-screen-menu");
    const choices = $("big-customize-menu"), titleOptions = $("big-customize-title-options");
    const titleButtons = [...titleOptions.querySelectorAll("[data-big-title]")];
    const rows = $("big-customize-rows"), report = $("big-customize-status");
    const paging = $("big-customize-paging");
    const desktop = $("big-customize-desktop"), hint = $("big-customize-desktop-hint");
    const backButton = $("big-customize-back"), extract = $("big-music-extract"), audio = $("big-music-player");
    const headings = {
      menu: "builder.big_screen.customize", title: "builder.assets.title_art",
      languages: "builder.nav.languages", music: "builder.assets.music",
    };
    let page = "", stamp = 0, saving = false, timer = 0;
    let artwork = {}, music = [], originals = {}, trackID = "";

    function status(key, args) {
      report.hidden = !key;
      if (key) ui.set(report, key, args);
    }

    function action(text, description, handler) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "secondary big-screen-row";
      button.textContent = text;
      if (description) {
        const small = document.createElement("small");
        small.textContent = description;
        button.append(small);
      }
      button.addEventListener("click", handler);
      rows.append(button);
      return button;
    }

    function paintTitle() {
      const selection = artwork.enabled ? (artwork.variant === "ja" ? "ja" : "en") : "original";
      for (const button of titleButtons) {
        button.disabled = false;
        button.setAttribute("aria-pressed", String(button.dataset.bigTitle === selection));
      }
      const image = $("big-title-preview");
      image.hidden = selection === "original";
      if (!image.hidden) image.src = selection === "ja" ? "title-logo-ja.png" : "title-logo.png";
    }

    function paintLanguages(catalog) {
      rows.replaceChildren();
      const installed = catalog.filter(pack => pack.installed);
      if (!installed.length) status("builder.big_screen.no_packs");
      for (const pack of installed) {
        const label = pack.installedName || pack.name;
        const state = pack.error ? "builder.language.needs_attention" :
          pack.enabled ? "builder.language.enabled_game" : "builder.language.disabled_game";
        const button = action(label, ui.text(state), () => save(async () => {
          try {
            await request("localization/set-enabled", {
              method: "POST", headers: {"Content-Type": "application/json"},
              body: JSON.stringify({id: pack.id, directory: pack.key, expected: pack.installedRevision, enabled: !pack.enabled}),
            });
          } finally {
            paintLanguages(await request("localization/catalog"));
            [...rows.children].find(row => row.dataset.packKey === pack.key)?.focus();
          }
          status("builder.language.availability_saved");
        }));
        button.dataset.packKey = pack.key;
        button.disabled = !pack.installedRevision || (!pack.enabled && !!pack.error);
        button.setAttribute("aria-pressed", String(pack.enabled));
      }
    }

    function trackName(track) {
      return ui.text("builder.assets.track." + track.id.replaceAll("-", "_"), {}, track.name);
    }

    function paintMusic() {
      rows.replaceChildren();
      for (const track of music) {
        const installed = track.configured || track.variants?.some(variant => variant.configured);
        const button = action(trackName(track), ui.text(installed ?
          "builder.assets.installed_replacement" : "builder.assets.using_original"), () => {
          trackID = track.id;
          render("track");
        });
        button.dataset.trackId = track.id;
      }
    }

    function paintPlayer() {
      for (const button of rows.querySelectorAll("[data-preview-source]")) {
        const playing = !audio.paused && !audio.ended && audio.getAttribute("src") === button.dataset.previewSource;
        button.setAttribute("aria-pressed", String(playing));
        button.firstChild.nodeValue = (playing ? "⏸ " : "▶ ") + button.dataset.previewLabel;
      }
    }

    function preview(label, description, url) {
      const button = action(label, description, async () => {
        if (audio.getAttribute("src") === url && !audio.paused && !audio.ended) {
          audio.pause();
          return;
        }
        if (audio.getAttribute("src") !== url) audio.src = url;
        try { await audio.play(); }
        catch (error) {
          if (page === "track" && audio.getAttribute("src") === url) {
            status("builder.big_screen.audio_error", {detail: error.message});
          }
        }
      });
      button.dataset.previewSource = url;
      button.dataset.previewLabel = label;
      button.setAttribute("aria-pressed", "false");
    }

    function paintTrack() {
      const track = music.find(item => item.id === trackID);
      const selected = document.activeElement?.dataset.previewSource;
      rows.replaceChildren();
      if (!track) return;
      $("big-customize-title").textContent = trackName(track);
      const original = originals.tracks?.find(item => item.id === track.id);
      if (original?.url) preview(ui.text("builder.assets.original_rom"), "", original.url);
      if (track.url) preview(ui.text("builder.assets.installed_replacement"), track.file, track.url);
      for (const variant of track.variants || []) {
        if (variant.configured && variant.url) preview(variant.name, variant.file, variant.url);
      }
      const generating = originals.state === "generating";
      extract.hidden = !!original?.url || !originals.romAvailable;
      extract.disabled = generating;
      status(generating ? "builder.assets.preview_progress" :
        originals.state === "failed" ? "builder.assets.preview_failed_detail" :
        !rows.children.length ? "builder.big_screen.no_audio" : "",
        {count: originals.completed || 0, total: originals.total || 0, detail: originals.error || ""});
      paintPlayer();
      if (selected) [...rows.children].find(button => button.dataset.previewSource === selected)?.focus();
    }

    async function readOriginals(version) {
      try {
        const data = await request("audio-previews");
        if (stamp !== version) return;
        originals = data;
        paintTrack();
        if (originals.state === "generating") timer = setTimeout(() => readOriginals(version), 500);
        else if (document.activeElement === report) (rows.querySelector("button") || extract).focus();
      } catch (error) {
        if (stamp === version) status("builder.request_failed", {detail: error.message});
      }
    }

    function render(next, focus) {
      const version = ++stamp;
      clearTimeout(timer);
      audio.pause();
      audio.removeAttribute("src");
      page = next;
      menu.hidden = !!page;
      section.hidden = !page;
      choices.hidden = page !== "menu";
      titleOptions.hidden = page !== "title";
      rows.hidden = !["languages", "music", "track"].includes(page);
      paging.hidden = !["languages", "music"].includes(page);
      rows.replaceChildren();
      extract.hidden = true;
      hint.hidden = desktop.hidden = !["music", "track", "languages"].includes(page);
      status("");
      if (!page) return;
      ui.set($("big-customize-title"), headings[page] || headings.music);
      if (!hint.hidden) ui.set(hint, page === "languages" ? "builder.big_screen.desktop_languages" : "builder.big_screen.desktop_music");
      if (page === "menu") {
        (choices.querySelector(`[data-big-page="${focus || "title"}"]`)).focus();
        return;
      }
      if (page === "track") {
        ui.unbind($("big-customize-title"));
        paintTrack();
        (rows.querySelector("button") || (!extract.hidden && !extract.disabled ? extract : desktop)).focus();
        readOriginals(version);
        return;
      }
      if (page === "title") titleButtons.forEach(button => { button.disabled = true; });
      status("builder.big_screen.loading");
      report.focus();
      Promise.all(page === "music" ? [request("assets"), request("audio-previews")] :
        [request(page === "title" ? "assets" : "localization/catalog")]).then(([data, previews]) => {
        if (stamp !== version) return;
        const focusFirst = document.activeElement === report;
        status("");
        if (page === "title") {
          artwork = data.title || {};
          paintTitle();
        } else if (page === "languages") paintLanguages(data);
        else {
          music = data.tracks || [];
          originals = previews;
          paintMusic();
        }
        if (focusFirst) {
          const first = page === "title" ? titleOptions.querySelector('[aria-pressed="true"]') :
            [...rows.children].find(button => button.dataset.trackId === focus) || rows.querySelector("button:not(:disabled)");
          (first || backButton).focus();
        }
      }).catch(error => {
        if (stamp !== version) return;
        status("builder.request_failed", {detail: error.message});
        backButton.focus();
      });
    }

    async function save(change) {
      if (saving) return;
      saving = true;
      section.setAttribute("aria-busy", "true");
      try { await change(); }
      catch (error) { status("builder.request_failed", {detail: error.message}); }
      finally {
        saving = false;
        section.removeAttribute("aria-busy");
      }
    }

    function back() {
      if (saving) return;
      if (page === "track") render("music", trackID);
      else if (page && page !== "menu") render("menu", page);
      else { render(""); onClose(); }
    }

    for (const button of choices.querySelectorAll("button")) button.addEventListener("click", () => render(button.dataset.bigPage));
    for (const button of titleButtons) button.addEventListener("click", () => {
      if (button.getAttribute("aria-pressed") === "true") return;
      save(async () => {
        const choice = button.dataset.bigTitle;
        const form = new FormData();
        form.set("title-change", "1");
        form.set("title-variant", choice === "original" ? artwork.variant || "en" : choice);
        if (choice !== "original") form.set("title", "on");
        const result = await request("assets", {method: "POST", body: form});
        artwork = result.config.title;
        paintTitle();
        status("builder.assets.saved");
      });
    });
    for (const event of ["play", "pause", "ended"]) audio.addEventListener(event, paintPlayer);
    extract.addEventListener("click", () => {
      const version = stamp;
      save(async () => {
        originals = await request("audio-previews", {method: "POST"});
        paintTrack();
        (rows.querySelector("button") || backButton).focus();
        readOriginals(version);
      });
    });
    backButton.addEventListener("click", back);
    desktop.addEventListener("click", () => openDesktop(page === "languages" ? "languages" : "music"));
    document.addEventListener("workshop:closed", () => render(""));
    return {open: () => render("menu"), close: () => render(""), back, isOpen: () => !!page, isBusy: () => saving};
  };
})();
