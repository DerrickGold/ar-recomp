/* Installation migration belongs to the same session-scoped Workshop in both
   browsers and desktop webviews. Paths and filenames are always text, not HTML. */
(() => {
  "use strict";
  const fetchResponse = (...args) => window.workshopFeedback?.request ?
    window.workshopFeedback.request(...args) : fetch(...args);
  const ui=window.workshopI18n, node=id=>document.getElementById("import-install"+id);
  const dialog=node(""), open=node("-open"), source=node("-source"), candidates=node("-candidates"), status=node("-status");
  const confirm=node("-confirm"), apply=node("-apply"), details=node("-details"), files=node("-files"), reload=node("-reload");
  let plan=null, busy=false, exact=false, imported=false;
  async function request(endpoint,body){
    const response=await fetchResponse("installation-import/"+endpoint,body===undefined?{}:{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
    if(window.workshopFeedback)return window.workshopFeedback.readJSON(response);
    const value=await response.json(); if(!response.ok) throw new Error(value.error||String(response.status)); return value;
  }
  function invalidate(){ plan=null; apply.disabled=true; confirm.checked=false; details.hidden=true; }
  function choices(paths){
    candidates.replaceChildren(...(paths||[]).map(path=>{const option=document.createElement("option"); option.value=path; option.textContent=path; return option;}));
    candidates.hidden=!paths?.length;
    if(paths?.length){ source.value=paths[0]; exact=true; }
  }
  async function run(action){
    if(busy)return; busy=true;
    window.workshopFeedback?.clear(status);
    for(const control of dialog.querySelectorAll("button,input,select"))control.disabled=true;
    ui.set(status,"builder.import.working");
    try { await action(); } catch(error){ invalidate(); ui.unbind(status); status.textContent=error.message; window.workshopFeedback?.show(status,error,{operation:"Import previous installation"}); }
    finally { busy=false; for(const control of dialog.querySelectorAll("button,input,select"))control.disabled=imported; reload.disabled=false; node("-skip").disabled=false; apply.disabled=imported||!plan||plan.alreadyImported||!confirm.checked; }
  }
  source.addEventListener("input",()=>{exact=false; invalidate();});
  candidates.addEventListener("change",()=>{source.value=candidates.value; exact=true; invalidate();});
  confirm.addEventListener("change",()=>{apply.disabled=!plan||plan.alreadyImported||!confirm.checked;});
  node("-choose").addEventListener("click",()=>run(async()=>{
    invalidate(); const result=await request("choose",{});
    if(result.directory){source.value=result.directory; exact=false;} ui.set(status,"builder.import.review_hint");
  }));
  node("-preview").addEventListener("click",()=>run(async()=>{
    invalidate(); const result=await request("preview",{directory:source.value,exact});
    if(result.candidates){choices(result.candidates); ui.set(status,"builder.import.multiple"); return;}
    plan=result; source.value=result.source; exact=true;
    ui.set(status,result.alreadyImported?"builder.import.already":"builder.import.summary",{copy:result.copy,conflicts:result.conflicts,identical:result.identical});
    files.textContent=result.files.map(file=>ui.text("builder.import.action_"+file.action.replaceAll("-","_"))+" · "+file.path).join("\n"); details.hidden=false;
  }));
  apply.addEventListener("click",()=>run(async()=>{
    const result=await request("apply",{revision:plan.revision,confirm:confirm.checked});
    imported=true; reload.hidden=false;
    apply.hidden=true;node("-skip").hidden=true;
    ui.set(status,"builder.import.done",{copy:result.copy,conflicts:result.conflicts});
  }));
  const close=()=>{if(busy)return; dialog.close(); if(imported)location.reload();};
  node("-skip").addEventListener("click",close);
  dialog.addEventListener("cancel",event=>{event.preventDefault(); close();});
  reload.addEventListener("click",()=>location.reload());
  open.addEventListener("click",()=>{dialog.showModal(); source.focus();});
  request("state").then(state=>{
    if(!state.enabled)return; open.hidden=false;
    node("-destination").textContent=state.destination; choices(state.candidates);
    node("-detected").hidden=!state.candidates?.length;
    if(state.warning){ui.unbind(status); status.textContent=state.warning;}
    // Folder selection already determines whether to start fresh or reuse an
    // existing game in place. Nearby installations are suggestions for manual
    // import, not a second onboarding step or permission to copy their data.
  }).catch(error=>{open.hidden=false; ui.unbind(status); status.textContent=error.message;window.workshopFeedback?.show(status,error,{operation:"Detect previous installation"});});
})();
