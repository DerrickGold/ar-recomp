/* The native worker owns shaping, wrapping, reveal order and waits. This player
 * displays its images and sends the same clock/input to independent timelines. */
(() => {
  "use strict";
  window.workshopPlayback = {create};

  function create(host, capture, reportError, locateTemplate) {
    const ui = window.workshopI18n;
    const text = (key, args = {}) => ui.text("builder.playback." + key, args);
    let generation = 0;
    let pending = null;
    let result = null;
    let players = [];

    function make(tag, value, parent = host) {
      const node = document.createElement(tag);
      if (value !== undefined) node.textContent = value;
      parent.append(node);
      return node;
    }
    function bind(node, key, args = {}) {
      ui.set(node, "builder.playback." + key, args);
    }
    function localized(tag, key, parent = host, args = {}) {
      const node = make(tag, undefined, parent);
      bind(node, key, args);
      return node;
    }
    function button(key, parent, action) {
      const node = localized("button", key, parent);
      node.type = "button";
      node.dataset.playback = "true";
      node.addEventListener("click", action);
      return node;
    }
    function select(key, choices, parent) {
      const label = make("label", undefined, parent);
      localized("span", key, label);
      const input = make("select", undefined, label);
      for (const [value, caption, args] of choices) {
        const option = typeof caption === "string" && caption.startsWith("i18n:")
          ? localized("option", caption.slice(5), input, args)
          : make("option", caption, input);
        option.value = String(value);
      }
      return input;
    }

    localized("h4", "title");
    localized("p", "help").className = "loc-help";
    const status = make("p", "");
    status.setAttribute("role", "status");
    const toolbar = make("div");
    toolbar.className = "loc-row";
    const linkedLabel = make("label", "", toolbar);
    const linked = make("input", undefined, linkedLabel);
    linked.type = "checkbox";
    linked.checked = true;
    localized("span", "linked", linkedLabel);
    const speed = select("speed", [0.25, 0.5, 1, 2, 4].map(value => [value, value + "×"]), toolbar);
    speed.value = "1";
    const zoom = select("zoom", [[0, "i18n:fit"], [1, "1×"], [2, "2×"], [3, "3×"]], toolbar);
    zoom.value = "0";
    const rebuild = button("render", toolbar, () => refresh());
    const advanced = make("details");
    localized("summary", "scenario", advanced);
    localized("p", "scenario_help", advanced);
    const settings = make("div", undefined, advanced);
    settings.className = "loc-row";
    const size = select("size", Array.from({length: 13}, (_, i) => [80 + i * 5, (80 + i * 5) + "%"]), settings);
    const delay = select("delay", Array.from({length: 10}, (_, i) => [i, String(i)]), settings);
    const effectChoices = [["0:0", "i18n:none"]];
    for (const [mode, name] of [[2, "mosaic_size"], [1, "low_resolution_size"]]) {
      for (const pixels of [2, 4, 6, 8])
        effectChoices.push([mode + ":" + pixels, "i18n:" + name, {pixels}]);
    }
    const effect = select("effect", effectChoices, settings);
    const smoothing = select("sampling", [[0, "i18n:crisp"], [1, "i18n:smooth"]], settings);
    const fields = make("div", undefined, advanced);
    fields.className = "loc-comparison";
    const sourceFields = make("fieldset", undefined, fields);
    const draftFields = make("fieldset", undefined, fields);
    const paletteLabel = make("label", undefined, advanced);
    localized("span", "palette", paletteLabel);
    const palette = make("textarea", undefined, paletteLabel);
    palette.rows = 3;
    palette.spellcheck = false;
    palette.value = "{}";
    palette.setAttribute("dir", "ltr");
    const comparison = make("div");
    comparison.className = "loc-comparison loc-playback-panels";
    host.hidden = true;

    function invalidate() {
      ++generation;
      pending?.abort();
      pending = null;
      rebuild.disabled = false;
      players.forEach(player => player.pause());
      for (const control of comparison.querySelectorAll("button, input")) control.disabled = true;
      if (result) bind(status, "stale");
    }
    function scenarioFields(parent, caption, values) {
      parent.replaceChildren();
      localized("legend", caption, parent);
      for (const [name, value] of Object.entries(values)) {
        const label = make("label", name, parent);
        const input = make("input", undefined, label);
        input.name = name;
        input.value = value;
        input.maxLength = 1023;
        input.setAttribute("dir", "auto");
      }
    }
    function readScenario(which) {
      const value = structuredClone(result[which === "source" ? "sourceScenario" : "scenario"]);
      const parent = which === "source" ? sourceFields : draftFields;
      value.values = Object.fromEntries(Array.from(parent.querySelectorAll("input"), input => [input.name, input.value]));
      value.size = Number(size.value);
      value.delay = Number(delay.value);
      value.sampling = Number(smoothing.value);
      [value.pixelation, value.pixelSize] = effect.value.split(":").map(Number);
      value.inks = JSON.parse(palette.value);
      return value;
    }
    async function loadImages(movie) {
      return Promise.all(movie.sheets.map(url => new Promise((resolve, reject) => {
        const image = new Image();
        image.onload = () => resolve(image);
        image.onerror = () => reject(new Error(text("image_error")));
        image.src = url;
      })));
    }
    async function refresh(pages, preservePosition = true) {
      const retained = pages && preservePosition ? players.map((player, index) =>
        pages[index] === player.movie.page ? player.position : 0) : [0, 0];
      invalidate();
      const ownGeneration = generation;
      const abort = new AbortController();
      pending = abort;
      host.hidden = false;
      rebuild.disabled = true;
      bind(status, "rendering");
      try {
        const body = {...capture()};
        const uploads = body.fontUploads || [];
        delete body.fontUploads;
        if (result) {
          body.scenario = readScenario("draft");
          body.sourceScenario = readScenario("source");
          if (pages) [body.sourceScenario.page, body.scenario.page] = pages;
        }
        let payload = JSON.stringify(body);
        let headers = {"Content-Type": "application/json"};
        if (body.saveFonts && uploads.length) {
          body.fontPaths = uploads.map(([name]) => name);
          payload = new FormData();
          payload.set("request", JSON.stringify(body));
          uploads.forEach(([, file], i) => payload.set("font" + i, file));
          headers = {};
        }
        const response = await fetch(new URL("localization/playback", location.href), {
          method: "POST", headers, body: payload, signal: abort.signal
        });
        const next = await response.json();
        if (!response.ok) throw new Error(next.error || String(response.status));
        const images = await Promise.all([loadImages(next.source), loadImages(next.draft)]);
        if (ownGeneration !== generation || abort.signal.aborted) return;
        result = next;
        size.value = String(next.scenario.size);
        delay.value = String(next.scenario.delay);
        smoothing.value = String(next.scenario.sampling);
        effect.value = next.scenario.pixelation + ":" + next.scenario.pixelSize;
        scenarioFields(sourceFields, "source", next.sourceScenario.values);
        scenarioFields(draftFields, "draft", next.scenario.values);
        comparison.replaceChildren();
        players = [player(next.source, images[0], "source", 0), player(next.draft, images[1], "draft", 1)];
        players.forEach((player, index) => player.seek(retained[index]));
        updateZoom();
        bind(status, "ready");
      } catch (error) {
        if (ownGeneration !== generation || abort.signal.aborted) return;
        ui.unbind(status);
        status.textContent = error.message;
        reportError(error.message);
      } finally {
        if (ownGeneration === generation) {
          pending = null;
          rebuild.disabled = false;
        }
      }
    }
    function appendTrace(parent, movie, key) {
      const detail = make("details", undefined, parent);
      localized("summary", "trace", detail);
      make("p", `${movie.messageID} → ${movie.resolvedID}\n${movie.packID || ""}\n${movie.source}:${movie.line} · ${movie.layout}`, detail);
      if (locateTemplate && key === "draft" && !movie.fallback)
        button("locate", detail, () => locateTemplate(movie.messageID));
      if (movie.artworkPlaceholders) localized("p", "artwork_placeholders", detail);
      if (!movie.nativeBounds) localized("p", "generic_bounds", detail);
      if (movie.fallback) localized("p", "fallback", detail);
      if (movie.appearance) make("pre", JSON.stringify(movie.appearance, null, 2), detail);
      for (const treatment of movie.treatments || []) {
        make("p", `${treatment.sourcePath}:${treatment.sourceLine}`, detail);
        make("pre", JSON.stringify(treatment.definition, null, 2), detail);
      }
      const fonts = make("ul", undefined, detail);
      const uses = new Set();
      for (const font of movie.fonts) {
        const identity = `${font.reference} · ${font.pixels}px`;
        const key = identity + ":" + font.missing;
        if (uses.has(key)) continue;
        uses.add(key);
        const item = make("li", identity, fonts);
        if (font.missing) {
          make("span", " · ", item);
          localized("span", "missing", item);
        }
      }
      const events = make("ol", undefined, detail);
      for (const frame of movie.frames) {
        if (["wait", "control", "input", "page", "end"].includes(frame.kind))
          make("li", `${(frame.tick / 60).toFixed(2)}s · ${frame.kind}${frame.control ? " · " + frame.control : ""}`, events);
      }
    }
    function player(movie, images, key, index) {
      const section = make("section", undefined, comparison);
      const heading = make("h5", undefined, section);
      localized("span", key, heading);
      make("span", " · " + result[key === "source" ? "sourceLanguage" : "draftLanguage"], heading);
      const viewport = make("div", undefined, section);
      viewport.className = "loc-playback-viewport";
      const canvas = make("canvas", undefined, viewport);
      canvas.width = movie.width;
      canvas.height = movie.height;
      ui.attribute(canvas, "aria-label", "builder.playback." + key);
      const context = canvas.getContext("2d");
      const controls = make("div", undefined, section);
      controls.className = "loc-row";
      const toggle = button("play", controls, () => {
        const play = !p.playing;
        each(player => play ? player.play() : player.pause());
      });
      button("restart", controls, () => each(player => { player.pause(); player.seek(0); }));
      button("step", controls, () => each(player => { player.pause(); player.advanceTo(player.clock + 1); }));
      button("reveal", controls, () => each(player => player.reveal()));
      button("confirm", controls, confirm);
      const previous = button("previous", controls, () => inspectPage(-1));
      const next = button("next", controls, () => inspectPage(1));
      previous.disabled = movie.page === 0;
      next.disabled = movie.page + 1 >= movie.pages;
      const seek = make("input", undefined, section);
      seek.type = "range";
      seek.min = "0";
      seek.max = String(movie.frames.length - 1);
      seek.value = "0";
      ui.attribute(seek, "aria-label", "builder.playback.seek");
      const frameStatus = make("p", undefined, section);
      const positionStatus = make("span", "", frameStatus);
      const eventStatus = make("span", "", frameStatus);
      appendTrace(section, movie, key);

      const p = {
        movie, position: 0, clock: 0, playing: false, animation: null, origin: 0,
        seek(at) {
          p.position = Math.max(0, Math.min(movie.frames.length - 1, at));
          const frame = movie.frames[p.position];
          p.clock = frame.tick;
          context.clearRect(0, 0, canvas.width, canvas.height);
          context.drawImage(images[frame.sheet], 0, frame.index * movie.height, movie.width, movie.height, 0, 0, movie.width, movie.height);
          seek.value = String(p.position);
          bind(positionStatus, "position", {page: movie.page + 1, pages: movie.pages, seconds: (frame.tick / 60).toFixed(2)});
          eventStatus.textContent = " · " + (frame.control || frame.kind);
        },
        pause() {
          p.playing = false;
          cancelAnimationFrame(p.animation);
          bind(toggle, "play");
        },
        play() {
          if (p.position === movie.frames.length - 1 || blocked()) return;
          p.playing = true;
          bind(toggle, "pause");
          p.origin = performance.now() - p.clock / 60 * 1000 / Number(speed.value);
          p.animation = requestAnimationFrame(tick);
        },
        advanceTo(target) {
          if (blocked()) return;
          while (p.position + 1 < movie.frames.length && movie.frames[p.position + 1].tick <= target) {
            p.seek(p.position + 1);
            if (blocked()) { p.pause(); return; }
          }
          p.clock = Math.min(target, movie.frames.at(-1).tick);
        },
        reveal() {
          p.pause();
          if (blocked() || movie.frames[p.position].kind === "wait") return;
          while (p.position + 1 < movie.frames.length) {
            p.seek(p.position + 1);
            if (blocked() || movie.frames[p.position].kind === "wait") break;
          }
        }
      };
      function blocked() {
        return ["control", "input", "page", "end"].includes(movie.frames[p.position].kind);
      }
      function tick(now) {
        if (!p.playing) return;
        p.advanceTo((now - p.origin) * 60 / 1000 * Number(speed.value));
        if (p.position === movie.frames.length - 1) p.pause();
        if (p.playing) p.animation = requestAnimationFrame(tick);
      }
      function each(action) {
        (linked.checked ? players : [p]).forEach(action);
      }
      function confirm() {
        const pages = [result.source.page, result.draft.page];
        each(player => {
          player.pause();
          const kind = player.movie.frames[player.position].kind;
          if (kind === "page" && player.movie.page + 1 < player.movie.pages) {
            pages[players.indexOf(player)]++;
          } else if (kind === "control" || kind === "input") {
            player.seek(player.position + 1);
          }
        });
        if (pages[0] !== result.source.page || pages[1] !== result.draft.page) refresh(pages);
      }
      function inspectPage(delta) {
        const pages = [result.source.page, result.draft.page];
        pages[index] += delta;
        if (linked.checked) {
          const other = 1 - index;
          const count = index === 0 ? result.draft.pages : result.source.pages;
          pages[other] = Math.max(0, Math.min(count - 1, pages[other] + delta));
        }
        refresh(pages);
      }
      seek.addEventListener("input", () => {
        p.pause();
        p.seek(Number(seek.value));
        if (!linked.checked) return;
        const other = players[1 - index];
        let at = 0;
        while (at + 1 < other.movie.frames.length && other.movie.frames[at + 1].tick <= p.clock) ++at;
        other.pause();
        other.seek(at);
      });
      p.seek(0);
      return p;
    }
    function updateZoom() {
      for (const canvas of comparison.querySelectorAll("canvas"))
        canvas.style.width = zoom.value === "0" ? "100%" : canvas.width * Number(zoom.value) + "px";
    }
    speed.addEventListener("change", () => players.forEach(player => player.pause()));
    zoom.addEventListener("change", updateZoom);
    advanced.addEventListener("input", invalidate);
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) players.forEach(player => player.pause());
    });
    return {
      invalidate,
      show() {
        if (result && result.draft.messageID !== capture().id) result = null;
        return refresh([0, 0], false);
      }
    };
  }
})();
