/* Desktop-only destination settings. An iframe keeps the active Workshop and
   unsaved drafts intact; the shell saves the choice for the NEXT launch. */
(() => {
  "use strict";
  const open=document.getElementById("game-folder-open"), dialog=document.getElementById("game-folder-dialog");
  const frame=document.getElementById("game-folder-frame");
  let closed=false;
  let observer;
  frame.addEventListener("load",()=>{
    observer?.disconnect();
    const body=frame.contentDocument?.body;
    if(!dialog.open||!body)return;
    // Keep short forms compact and let the iframe scroll when review details exceed the window.
    const fit=()=>{frame.style.height=(body.offsetHeight+frame.offsetHeight-frame.clientHeight)+"px";};
    observer=new ResizeObserver(fit);
    observer.observe(body);
    fit();
  });
  dialog.addEventListener("close",()=>{
    observer?.disconnect();
    frame.style.removeProperty("height");
  });
  fetch("__shell/output/state",{cache:"no-store"}).then(response=>{
    if(response.ok&&!closed)open.hidden=false;
  }).catch(()=>{}); // Generic browser/CLI builds have no desktop selector.
  open.addEventListener("click",()=>{
    if(closed)return;
    frame.src="__shell/output/?lang="+encodeURIComponent(window.workshopI18n.locale);dialog.showModal();
  });
  document.getElementById("game-folder-dismiss").addEventListener("click",()=>dialog.close());
  document.addEventListener("workshop:closed",()=>{closed=true;dialog.close();open.disabled=true;});
})();
