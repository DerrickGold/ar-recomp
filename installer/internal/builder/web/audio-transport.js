(() => {
  "use strict";

  window.workshopAudioTransport = (group, parent, {url, name, description}, onError) => {
    const ui = window.workshopI18n;
    const key = name => "builder.audio." + name;
    const player = document.createElement("div");
    player.className = "audio-transport";
    player.setAttribute("role", "group");
    ui.attribute(player, "aria-label", key("preview"));

    const title = document.createElement("strong");
    title.textContent = name;
    const time = document.createElement("span");
    time.className = "audio-time";
    time.setAttribute("aria-hidden", "true");
    const timeline = document.createElement("div");
    timeline.className = "audio-timeline";
    timeline.append(title, time);
    const seek = document.createElement("input");
    seek.type = "range";
    seek.min = "0";
    seek.step = "0.1";
    ui.attribute(seek, "aria-label", key("position"));
    const controls = document.createElement("div");
    controls.className = "audio-controls";
    controls.dataset.bigBand = "";
    player.append(timeline);
    if (description) {
      const note = document.createElement("small");
      ui.set(note, description);
      player.append(note);
    }
    const audio = document.createElement("audio");
    audio.preload = "none";
    audio.src = url;
    player.append(audio, seek, controls);
    parent.append(player);

    const clock = seconds => {
      const whole = Math.max(0, Math.floor(seconds || 0));
      return Math.floor(whole / 60) + ":" + String(whole % 60).padStart(2, "0");
    };

    function button(action, click) {
      const control = document.createElement("button");
      control.className = "secondary";
      control.type = "button";
      control.dataset.audioControl = action;
      control.addEventListener("click", click);
      controls.append(control);
      return control;
    }

    function skip(seconds) {
      const duration = audio.duration;
      if (Number.isFinite(duration) && duration > 0) {
        audio.currentTime = Math.max(0, Math.min(duration, audio.currentTime + seconds));
      }
    }

    const rewind = button("rewind", () => skip(-10));
    rewind.textContent = "−10 s";
    ui.attribute(rewind, "aria-label", key("rewind"));
    const play = button("play", toggle);
    play.dataset.audioPlay = "";
    const forward = button("forward", () => skip(10));
    forward.textContent = "+10 s";
    ui.attribute(forward, "aria-label", key("forward"));
    const mute = button("mute", () => { audio.muted = !audio.muted; });
    const volume = document.createElement("input");
    volume.className = "audio-volume";
    volume.type = "range";
    volume.min = "0";
    volume.max = "1";
    volume.step = "0.05";
    ui.attribute(volume, "aria-label", key("volume"));
    controls.append(volume);
    volume.addEventListener("input", () => {
      audio.volume = Number(volume.value);
      audio.muted = false;
    });
    seek.addEventListener("input", () => {
      if (!seek.disabled) audio.currentTime = Number(seek.value);
    });

    function paintTimeline() {
      const duration = Number.isFinite(audio.duration) && audio.duration > 0 ? audio.duration : 0;
      const elapsed = clock(audio.currentTime), total = duration ? clock(duration) : "–:––";
      time.textContent = elapsed + " / " + total;
      seek.disabled = rewind.disabled = forward.disabled = !duration;
      seek.max = String(duration || 1);
      seek.value = String(duration ? Math.min(audio.currentTime, duration) : 0);
      seek.setAttribute("aria-valuetext", elapsed + " / " + total);
    }

    function paintButtons() {
      const playing = !audio.paused && !audio.ended;
      ui.set(play, playing ? "builder.mods.pause_audio" : "builder.mods.play_audio");
      ui.attribute(play, "aria-label", playing ? "builder.mods.pause_track" : "builder.mods.play_track", {name});
      ui.set(mute, key(audio.muted ? "unmute" : "mute"));
      volume.value = String(audio.muted ? 0 : audio.volume);
    }

    async function toggle() {
      if (!audio.paused && !audio.ended) {
        audio.pause();
        return;
      }
      for (const other of group.querySelectorAll("audio")) {
        if (other !== audio) other.pause();
      }
      try { await audio.play(); }
      catch (error) {
        if (player.isConnected && error.name !== "AbortError") onError(error);
      }
    }

    for (const event of ["durationchange", "timeupdate"]) {
      audio.addEventListener(event, paintTimeline);
    }
    for (const event of ["play", "pause", "ended", "volumechange"]) {
      audio.addEventListener(event, paintButtons);
    }
    paintTimeline();
    paintButtons();
    return play;
  };

  window.workshopStopAudio = group => {
    for (const audio of group.querySelectorAll("audio")) {
      audio.pause();
      audio.removeAttribute("src");
      audio.load();
    }
  };
})();
