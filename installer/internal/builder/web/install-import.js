/* Installation migration belongs to the same session-scoped Workshop in both
   browsers and desktop webviews. Paths and filenames are always text, not HTML. */
(() => {
  "use strict";
  const ui=window.workshopI18n, node=id=>document.getElementById("import-install"+id);
  const dialog=node(""), open=node("-open"), source=node("-source"), candidates=node("-candidates"), status=node("-status");
  const confirm=node("-confirm"), apply=node("-apply"), details=node("-details"), files=node("-files"), reload=node("-reload");
  let plan=null, busy=false, exact=false, imported=false;
  async function request(endpoint,body){
    const response=await fetch("installation-import/"+endpoint,body===undefined?{}:{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
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
    for(const control of dialog.querySelectorAll("button,input,select"))control.disabled=true;
    ui.set(status,"builder.import.working");
    try { await action(); } catch(error){ invalidate(); ui.unbind(status); status.textContent=error.message; }
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
    ui.set(status,"builder.import.done",{copy:result.copy,conflicts:result.conflicts});
  }));
  const skip=()=>run(async()=>{if(!imported)await request("skip",{}); dialog.close(); if(imported)location.reload();});
  node("-skip").addEventListener("click",skip);
  dialog.addEventListener("cancel",event=>{event.preventDefault(); if(!busy)skip();});
  reload.addEventListener("click",()=>location.reload());
  open.addEventListener("click",()=>{dialog.showModal(); source.focus();});
  request("state").then(state=>{
    if(!state.enabled)return; open.hidden=false;
    node("-destination").textContent=state.destination; choices(state.candidates);
    node("-detected").hidden=!state.candidates?.length;
    if(state.warning){ui.unbind(status); status.textContent=state.warning;}
    if(!state.decided){dialog.showModal(); source.focus();}
  }).catch(error=>{open.hidden=false; ui.unbind(status); status.textContent=error.message;});
})();
