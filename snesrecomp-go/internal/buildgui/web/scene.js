/* Decorative workshop scene, independent of game state and editor models.
   The Go service extracts/validates the original sprites. This canvas only
   composes cached images; illustrated fallback works without a ROM or cache. */
import {createEncounterState,sampleEncounter} from "./encounters.mjs";

(() => {
  "use strict";
  const canvas = document.querySelector("#palace-scene");
  // One small scene buffer drives both the Home preview and the atmosphere
  // behind all menus. No game state, offscreen decoders, or full-window DPR
  // buffers. Work panels don't cause the scene clock to reset.
  const buffer=document.createElement("canvas");buffer.width=640;buffer.height=420;
  const context = buffer.getContext("2d");
  const preview=canvas.getContext("2d");
  const ambient=document.querySelector("#ambient-scene").getContext("2d");
  if (!context||!preview||!ambient) return;
  const choice = document.querySelector("#scene-motion");
  const setting = document.querySelector("#scene-setting");
  const panel = document.querySelector("#panel-home");
  const stage = canvas.parentElement;
  const reduced = matchMedia("(prefers-reduced-motion: reduce)");
  const caption = document.querySelector("#scene-caption");
  let nativeArt=null, clips=null, loading=false, closed=false, onScreen=true, retryAfter=0;
  const pending = new AbortController();
  // This is only a browser-session preference. No game settings are mutated.
  try { const saved = sessionStorage.getItem("workshop-scene"); if (["auto", "animated", "still", "off"].includes(saved)) choice.value = saved; } catch (_) { /* Storage can be disabled. */ }
  try { const saved=sessionStorage.getItem("workshop-scenery");if(["fillmore","palace"].includes(saved)) setting.value=saved; } catch (_) {}
  const art = document.createElement("canvas");
  art.width = 640; art.height = 420;
  const a = art.getContext("2d");
  if (!a) return;
  const polygon = (ctx, fill, points) => {
    ctx.fillStyle = fill; ctx.beginPath();
    points.forEach(([x,y], i) => i ? ctx.lineTo(x,y) : ctx.moveTo(x,y));
    ctx.closePath(); ctx.fill();
  };
  const rect = (color,x,y,w,h) => { a.fillStyle=color; a.fillRect(x,y,w,h); };
  // A faceted floating island and a modest marble temple; the geometry remains
  // crisp at its authored resolution and scales as a single cached layer.
  polygon(a,"#405563",[[168,260],[327,231],[490,267],[334,319],[234,300]]);
  polygon(a,"#263b4b",[[168,260],[234,300],[309,365],[270,289]]);
  polygon(a,"#344956",[[270,289],[334,319],[309,365]]);
  polygon(a,"#526365",[[334,319],[490,267],[438,317],[366,340],[309,365]]);
  polygon(a,"#71867a",[[170,255],[321,217],[489,261],[336,298]]);
  polygon(a,"#899789",[[170,255],[336,290],[489,261],[336,298]]);
  polygon(a,"#465c62",[[349,312],[397,296],[369,330],[344,336]]);
  polygon(a,"#71806d",[[205,260],[241,250],[252,269],[225,275]]);
  // Stairs leading out toward the viewer.
  for(let i=0;i<5;i++) {
    polygon(a,i%2?"#a69f87":"#cac0a0",[[292-i*8,259+i*5],[354+i*8,259+i*5],[367+i*8,266+i*5],[302-i*8,268+i*5]]);
  }
  polygon(a,"#d7c9a3",[[222,236],[323,213],[424,236],[324,267]]);
  polygon(a,"#a9a086",[[222,236],[324,260],[424,236],[424,245],[324,275],[222,245]]);
  polygon(a,"#5e747b",[[242,169],[325,153],[403,173],[403,234],[325,254],[242,231]]);
  polygon(a,"#364d5b",[[324,162],[402,174],[402,234],[324,254]]);
  // Recessed doors and shadows behind the colonnade.
  rect("#2a404d",268,186,17,46); rect("#1a303e",316,187,25,59); rect("#283f4c",365,188,15,44);
  const column = (x,y,h) => {
    rect("#9d9d8e",x+3,y+3,12,h); rect("#e0d6b6",x,y,7,h); rect("#c2bba0",x+7,y,5,h);
    rect("#f0e2ba",x-4,y-3,20,5); rect("#9c9e91",x-3,y+h-1,19,5);
    rect("#e2d2aa",x-5,y+h+3,23,4);
  };
  column(238,170,61); column(273,179,61); column(310,188,61);
  column(353,180,60); column(390,170,62);
  polygon(a,"#c2b99a",[[221,157],[323,135],[425,158],[425,174],[323,200],[221,175]]);
  polygon(a,"#f0dcb0",[[221,157],[323,184],[425,158],[323,137]]);
  polygon(a,"#a2a28e",[[218,155],[323,111],[429,157],[323,188]]);
  polygon(a,"#ead7ae",[[218,155],[323,111],[323,181]]);
  polygon(a,"#d4c39c",[[235,155],[314,123],[314,176]]);
  polygon(a,"#baa984",[[257,154],[305,134],[305,168]]);
  polygon(a,"#f2ddb2",[[218,155],[323,181],[429,157],[429,163],[323,191],[218,163]]);
  // Cypress silhouettes and warm little lanterns add life without needing ROMs.
  for(const [x,y,s] of [[208,221,1],[425,224,.9],[445,245,.65]]) {
    rect("#857e61",x,y-12,3,24);
    polygon(a,"#344f52",[[x-11*s,y-4],[x+2,y-57*s],[x+14*s,y-4],[x+3,y+3]]);
    polygon(a,"#658177",[[x-11*s,y-4],[x+2,y-57*s],[x+2,y+3]]);
  }
  rect("#ead49a",260,250,3,4); rect("#ead49a",387,252,3,4);

  let raf=0, last=0, elapsed=0;
  const cloud = (x,y,scale,opacity) => {
    context.save(); context.translate(x,y); context.scale(scale,scale);
    context.globalAlpha=opacity; context.fillStyle="#b6c9da"; context.beginPath();
    context.ellipse(0,0,86,12,0,0,Math.PI*2); context.ellipse(-30,-8,30,13,0,0,Math.PI*2);
    context.ellipse(12,-13,38,18,0,0,Math.PI*2); context.ellipse(48,-5,25,12,0,0,Math.PI*2);
    context.fill(); context.restore();
  };
  const glow=context.createRadialGradient(330,203,25,330,203,220);
  glow.addColorStop(0,"#47617680"); glow.addColorStop(.55,"#29415c50"); glow.addColorStop(1,"#111b2900");
  function drawPlaceholder(t) {
    context.clearRect(0,0,640,420);
    context.fillStyle=glow; context.fillRect(0,0,640,420);
    // Decorative chart marks, with a quietly orbiting star.
    context.strokeStyle="#bdc8d11b"; context.lineWidth=1;
    context.beginPath(); context.ellipse(327,222,236,106,-.13,0,Math.PI*2); context.stroke();
    context.fillStyle="#efd6a599";
    for(const [x,y] of [[154,119],[445,95],[515,173],[130,243]]) {
      context.fillRect(x-3,y,7,1); context.fillRect(x,y-3,1,7);
    }
    const drift=Math.sin(t/26000)*27;
    cloud(163+drift,193,1.03,.15); cloud(468-drift*.65,226,.85,.12);
    context.drawImage(art,0,Math.sin(t/2300)*3);
    cloud(152+drift*.8,318,1.15,.32); cloud(468-drift,337,.95,.24);
    cloud(355+drift*.3,384,.6,.09);
  }
  function drawPalace(t) {
    context.clearRect(0,0,640,420);
    context.fillStyle=glow; context.fillRect(0,0,640,420);
    const drift=Math.sin(t/26000)*27;
    cloud(173+drift,134,1.2,.15); cloud(455-drift,174,1,.18);
    const theta=t/14000*Math.PI*2;
    const x=320+Math.cos(theta)*190, y=224+Math.sin(theta)*83;
    const dx=-Math.sin(theta)*190, dy=Math.cos(theta)*83;
    const facing=Math.abs(dx)>Math.abs(dy)*1.4?(dx<0?"left":"right"):(dy<0?"back":"front");
    // ROM $01:A589-$A5BC: four 4-tick wing poses. $01:A7C5: two
    // 96-tick palace poses. The orbit itself is decorative, not gameplay.
    const angel=nativeArt.get("angel."+facing+"."+(Math.floor(t*60/4000)%4));
    const palace=nativeArt.get("palace.idle."+(Math.floor(t*60/96000)%2));
    const drawAngel=()=>context.drawImage(angel,Math.round(x-24),Math.round(y-27));
    if(Math.sin(theta)<0) drawAngel();
    context.drawImage(palace,200,Math.round(102+Math.sin(t/2300)*3));
    if(Math.sin(theta)>=0) drawAngel();
    cloud(130+drift*.8,340,1.1,.22); cloud(515-drift,357,.95,.23);
    cloud(335+drift*.3,390,.6,.08);
  }
  function pose(id,t,loop=true) {
    const clip=clips.get(id);
    let tick=Math.floor(t*60/1000);
    tick=loop?tick%clip.ticks:Math.min(tick,clip.ticks-1);
    for(const step of clip.poses) {
      if(tick<step.ticks) return nativeArt.get(step.frame);
      tick-=step.ticks;
    }
  }

  const encounter=createEncounterState();
  const actorOrigin={master:[96,144],bird:[96,96],club:[96,112],leaper:[96,128],centaur:[160,288]};
  function drawActor(actor) {
    if(!actor.visible) return;
    const id=(actor.kind==="master"?"master":"fillmore."+actor.kind)+"."+actor.facing+"."+actor.action;
    const sprite=pose(id,actor.phase,actor.loop),origin=actorOrigin[actor.kind];
    context.globalAlpha=actor.opacity;
    context.drawImage(sprite,Math.round(actor.x-origin[0]),Math.round(actor.y-origin[1]));
    context.globalAlpha=1;
  }

  function drawImpact(effect) {
    if(!effect.visible) return;
    const p=effect.age/(effect.boss?650:300),radius=6+p*(effect.boss?35:20);
    context.globalAlpha=(1-p)*.8;
    for(let i=0;i<6;i++) {
      const angle=i*Math.PI/3;
      context.fillStyle=i%2?"#ded6ad":"#eeb85a";
      const size=effect.boss?4:2;
      context.fillRect(Math.round(effect.x+Math.cos(angle)*radius),Math.round(effect.y+Math.sin(angle)*radius+p*p*18),size,size);
    }
    context.globalAlpha=1;
  }
  function drawFillmore(t) {
    context.clearRect(0,0,640,420);
    // Two extracted layers, scaled/cached once. Repetition and motion are a
    // decorative vignette, not a replay or a reconstruction of level physics.
    const forest=nativeArt.get("fillmore.forest"),ground=nativeArt.get("fillmore.ground");
    const scroll=Math.floor(t/110),drift=scroll%512;
    // Adjacent mirrored copies meet at matching pixels; a raw first-page
    // repeat otherwise exposes a hard seam through the tree canopy.
    for(let x=-drift,n=Math.floor(scroll/512);x<640;x+=512,n++) {
      context.save();context.translate(x+(n%2?512:0),-80);context.scale(n%2?-1:1,1);
      context.drawImage(forest,0,0);context.restore();
    }
    // A distant bird goes about its business while the bounded encounter
    // timeline owns the foreground actors. No entities accumulate offscreen.
    const bird2=pose("fillmore.bird.flipped.fly",t+160);
    context.drawImage(bird2,Math.round(-120+(t/27)%960)-96,Math.round(80+Math.sin(t/1300)*14)-96);
    for(let gx=0;gx<640;gx+=512) context.drawImage(ground,gx,326);
    sampleEncounter(t,encounter);
    drawActor(encounter.enemy);
    drawActor(encounter.master);
    drawImpact(encounter.effect);
  }
  function draw(t) {
    if(!nativeArt) drawPlaceholder(t);
    else if(setting.value==="fillmore") drawFillmore(t);
    else drawPalace(t);
    if(!panel.hidden) {preview.clearRect(0,0,640,420);preview.drawImage(buffer,0,0);}
    ambient.clearRect(0,0,640,420);ambient.drawImage(buffer,0,0);
  }
  function describe() {
    const fillmore=nativeArt&&setting.value==="fillmore";
    canvas.dataset.scene=fillmore?"fillmore":"palace";
    canvas.setAttribute("data-i18n-aria",nativeArt?(fillmore?"builder.scene.fillmore_aria":"builder.scene.palace_aria"):"builder.scene.placeholder_aria");
    window.workshopI18n.apply(canvas);
    window.workshopI18n.set(caption,nativeArt?(fillmore?"builder.scene.fillmore":"builder.scene.palace"):"builder.scene.placeholder");
  }
  async function loadArt() {
    if(nativeArt||loading||closed||document.hidden||choice.value==="off"||performance.now()<retryAfter) return;
    loading=true;retryAfter=performance.now()+1500;
    try {
      const response=await fetch("scene-assets",{cache:"no-store",signal:pending.signal});
      if(!response.ok) throw new Error("Scenery unavailable");
      const data=await response.json();
      if(closed) return;
      caption.title=data.message||"Illustrated scenery";
      if(!data.available) return;
      const url=new URL(data.atlasURL,location.href);
      if(url.origin!==location.origin||url.pathname!==new URL("scene-atlas.png",location.href).pathname) throw new Error("Unexpected scenery URL");
      const sheet=new Image();sheet.src=url.href;await sheet.decode();
      if(closed) return;
      if(data.catalog?.version!==3||sheet.width!==960||sheet.height!==1728||data.catalog.frames.length!==98) throw new Error("Unsupported scenery atlas");
      const sprites=new Map();
      for(const frame of data.catalog.frames) {
        if(![frame.x,frame.y,frame.width,frame.height].every(Number.isInteger)||frame.x<0||frame.y<0||frame.width<1||frame.width>256||frame.height<1||frame.height>256||frame.x+frame.width>sheet.width||frame.y+frame.height>sheet.height||sprites.has(frame.id)) throw new Error("Invalid scenery frame");
        const scale=frame.id.startsWith("palace.")?5:frame.id.startsWith("angel.")?3:2;
        const sprite=document.createElement("canvas");sprite.width=frame.width*scale;sprite.height=frame.height*scale;
        const ctx=sprite.getContext("2d");if(!ctx) throw new Error("Scenery canvas unavailable");
        ctx.imageSmoothingEnabled=false;
        ctx.drawImage(sheet,frame.x,frame.y,frame.width,frame.height,0,0,sprite.width,sprite.height);
        sprites.set(frame.id,sprite);
      }
      for(const [group,count] of [["palace.idle",2],["angel.front",4],["angel.back",4],["angel.left",4],["angel.right",4]]) for(let i=0;i<count;i++) if(!sprites.has(group+"."+i)) throw new Error("Missing scenery pose");
      const animations=new Map();
      if(!Array.isArray(data.catalog.animations)||data.catalog.animations.length!==28) throw new Error("Missing animation catalog");
      for(const clip of data.catalog.animations) {
        if(animations.has(clip.id)||!Array.isArray(clip.poses)||!clip.poses.length||clip.poses.length>16) throw new Error("Invalid animation");
        let ticks=0;
        for(const pose of clip.poses) {
          if(!sprites.has(pose.frame)||!Number.isInteger(pose.ticks)||pose.ticks<1||pose.ticks>256) throw new Error("Invalid animation pose");
          ticks+=pose.ticks;
        }
        animations.set(clip.id,{poses:clip.poses,ticks});
      }
      for(const facing of ["left","right"]) {
        for(const action of ["idle","walk","sword","jump","air-sword","fall"]) if(!animations.has("master."+facing+"."+action)) throw new Error("Missing Master animation");
      }
      for(const facing of ["normal","flipped"]) {
        for(const [kind,actions] of [["bird",["fly"]],["club",["walk"]],["leaper",["rest","hop"]],["centaur",["idle","walk","charge","cast"]]]) {
          for(const action of actions) if(!animations.has("fillmore."+kind+"."+facing+"."+action)) throw new Error("Missing encounter animation");
        }
      }
      for(const id of ["fillmore.forest","fillmore.ground"]) if(!sprites.has(id)) throw new Error("Missing background");
      clips=animations;nativeArt=sprites;canvas.dataset.art="native-us";
      describe();
      sync();
    } catch(error) {
      if(!closed) caption.title="Original scenery unavailable; using the illustration. Your game and tools are unaffected.";
    } finally { loading=false; }
  }
  function frame(now) {
    raf=0;
    if(last===0) last=now;
    const delta=now-last;
    if(delta>=1000/30) { elapsed+=Math.min(delta,100); last=now; draw(elapsed); }
    raf=requestAnimationFrame(frame);
  }
  function sync() {
    cancelAnimationFrame(raf); raf=0; last=0;
    const mode=choice.value;
    stage.dataset.motion=mode;
    document.body.dataset.sceneMotion=mode;
    const animate=(mode==="auto"||mode==="animated")&&!reduced.matches;
    canvas.dataset.running=String(animate&&!closed&&onScreen&&!document.hidden);
    if(closed || mode==="off" || document.hidden || !onScreen) { canvas.dataset.running="false"; return; }
    draw(elapsed);
    if(animate) raf=requestAnimationFrame(frame);
    loadArt();
  }
  choice.addEventListener("change",() => { try { sessionStorage.setItem("workshop-scene",choice.value); } catch (_) {} sync(); });
  setting.addEventListener("change",()=>{try{sessionStorage.setItem("workshop-scenery",setting.value);}catch(_){}describe();sync();});
  reduced.addEventListener("change",sync);
  document.addEventListener("visibilitychange",sync);
  document.addEventListener("workshop:navigate",sync);
  document.addEventListener("workshop:build-state",()=>{ retryAfter=0;loadArt(); });
  document.addEventListener("workshop:closed",()=>{ closed=true;pending.abort();sync(); });
  if(window.IntersectionObserver) new IntersectionObserver(entries=>{onScreen=entries[0].isIntersecting;sync();}).observe(document.querySelector("#workshop-atmosphere"));
  window.addEventListener("pagehide",() => { cancelAnimationFrame(raf); raf=0; });
  window.addEventListener("pageshow",sync);
  canvas.dataset.art="illustration";
  sync();
})();
