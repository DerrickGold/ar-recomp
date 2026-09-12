/* Desktop-only destination settings. An iframe keeps the active Workshop and
   unsaved drafts intact; the shell saves the choice for the NEXT launch. */
(() => {
  "use strict";
  const open=document.getElementById("game-folder-open"), dialog=document.getElementById("game-folder-dialog");
  const frame=document.getElementById("game-folder-frame");
  let closed=false;
  fetch("__shell/output/state",{cache:"no-store"}).then(response=>{
    if(response.ok&&!closed)open.hidden=false;
  }).catch(()=>{}); // Generic browser/CLI builds have no desktop selector.
  open.addEventListener("click",()=>{
    if(closed)return;
    frame.src="__shell/output/";dialog.showModal();
  });
  document.getElementById("game-folder-dismiss").addEventListener("click",()=>dialog.close());
  document.addEventListener("workshop:closed",()=>{closed=true;dialog.close();open.disabled=true;});
})();
