// Both game-art sources use the same bounded, local atlas protocol. Rasterize
// once at an integer scale; animation frames only compose the cached canvases.
export async function loadSceneAtlas(data, profile) {
  const url = new URL(data.atlasURL, location.href);
  if (url.origin !== location.origin || url.pathname !== new URL("scene-atlas.png", location.href).pathname) throw new Error("Unexpected scenery URL");
  const catalog = data.catalog;
  if (catalog?.version !== profile.version || !Array.isArray(catalog.frames) ||
      !catalog.frames.length || catalog.frames.length > 192 ||
      (profile.frames && catalog.frames.length !== profile.frames) ||
      !Array.isArray(catalog.animations) || catalog.animations.length !== profile.animations) throw new Error("Unsupported scenery atlas");
  const sheet = new Image(); sheet.src = url.href; await sheet.decode();
  if (sheet.width !== profile.width || (profile.height && sheet.height !== profile.height) || sheet.height < 1 || sheet.height > 3072) throw new Error("Invalid scenery dimensions");
  const sprites = new Map(), clips = new Map();
  for (const frame of catalog.frames) {
    if (typeof frame.id !== "string" || !frame.id || frame.id.length > 100 || sprites.has(frame.id) ||
        ![frame.x,frame.y,frame.width,frame.height].every(Number.isInteger) ||
        frame.x < 0 || frame.y < 0 || frame.width < 1 || frame.width > 256 || frame.height < 1 || frame.height > 256 ||
        frame.x+frame.width > sheet.width || frame.y+frame.height > sheet.height) throw new Error("Invalid scenery frame");
    const scale = profile.scale(frame.id);
    const sprite = document.createElement("canvas"); sprite.width = frame.width*scale; sprite.height = frame.height*scale;
    const context = sprite.getContext("2d"); if (!context) throw new Error("Scenery canvas unavailable");
    context.imageSmoothingEnabled = false;
    context.drawImage(sheet,frame.x,frame.y,frame.width,frame.height,0,0,sprite.width,sprite.height);
    sprites.set(frame.id,sprite);
  }
  for (const clip of catalog.animations) {
    if (typeof clip.id !== "string" || clips.has(clip.id) || !Array.isArray(clip.poses) || !clip.poses.length || clip.poses.length > 16) throw new Error("Invalid animation");
    let ticks = 0;
    for (const pose of clip.poses) {
      if (!sprites.has(pose.frame) || !Number.isInteger(pose.ticks) || pose.ticks < 1 || pose.ticks > 32768) throw new Error("Invalid animation pose");
      ticks += pose.ticks;
    }
    clips.set(clip.id,{poses:clip.poses,ticks});
  }
  return {sprites,clips};
}

export function samplePose(art, id, time, loop=true) {
  const clip = art.clips.get(id);
  let tick = Math.floor(Math.max(0,time)*60/1000);
  tick = loop ? tick%clip.ticks : Math.min(tick,clip.ticks-1);
  for (const step of clip.poses) {
    if (tick < step.ticks) return art.sprites.get(step.frame);
    tick -= step.ticks;
  }
}
