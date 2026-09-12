(() => {
  "use strict";
  const $=id=>document.getElementById(id);
  let plan=null, busy=false, saved=false, current="";
  async function request(endpoint, body) {
    const response=await fetch(endpoint,body===undefined?{cache:"no-store"}:{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
    const value=await response.json(); if(!response.ok)throw Error(value.error||String(response.status)); return value;
  }
  function invalidate(){plan=null;$("review").hidden=true;$("confirm").checked=false;$("apply").disabled=true;}
  function controls(){
    for(const node of document.querySelectorAll("button,input"))node.disabled=busy||saved;
    $("apply").disabled=busy||saved||!plan||(plan.existing&&!$("confirm").checked);
  }
  async function run(action){
    if(busy||saved)return;busy=true;controls();$("status").textContent="Working…";
    try{await action();}catch(error){invalidate();$("status").textContent=error.message;}
    finally{busy=false;controls();}
  }
  $("directory").addEventListener("input",invalidate);
  $("confirm").addEventListener("change",controls);
  $("browse").addEventListener("click",()=>run(async()=>{
    invalidate();const result=await request("choose",{});if(result.directory)$("directory").value=result.directory;
    $("status").textContent="Review the exact destination before continuing.";
  }));
  $("folder-form").addEventListener("submit",event=>{event.preventDefault();return run(async()=>{
    invalidate();plan=await request("review",{directory:$("directory").value});$("directory").value=plan.directory;
    $("review-path").textContent=plan.directory;
    $("review-title").textContent=plan.game?"Update existing installation":plan.existing?"Non-empty destination":"New game folder";
    $("review-description").textContent=plan.game?"An existing ActRaiserRecomp installation was detected. Its game files can be rebuilt in place.":plan.existing?"This folder contains files but is not a recognized game installation. A dedicated game subfolder is recommended. Existing unrelated files will be left alone.":"This destination is empty or does not exist yet. The Builder will create a portable game installation here.";
    $("entries").replaceChildren(...(plan.entries||[]).map(name=>{const li=document.createElement("li");li.textContent=name;return li;}));
    $("confirm-label").hidden=!plan.existing;$("review").hidden=false;
    $("apply").textContent=current?"Save for next launch":plan.game?"Update existing installation":"Use this game folder";
    $("status").textContent=current?"This session will keep using its current game folder. The new choice takes effect after you close and reopen the Builder.":"Ready to continue. No game files have been changed.";
  });});
  $("apply").addEventListener("click",()=>run(async()=>{
    const result=await request("apply",{revision:plan.revision,confirm:$("confirm").checked});saved=true;
    $("status").textContent=result.restartRequired?"Saved. Close this dialog, finish or save your work, then close and reopen the Builder to use: "+result.directory:"Starting the Workshop…";
    if(!result.restartRequired)location.replace("../../");
  }));
  run(async()=>{
    const state=await request("state");current=state.current;$("directory").value=state.next||state.candidate||"";
    $("current").hidden=!current;$("current").textContent="Current game folder: "+current;
    if(current)$("intro").textContent="Choose a game folder for the next launch. Your current build, editor and files will keep using the folder shown below until you close and reopen the Builder. Changing folders does not copy data; use Import previous installation if needed.";
    $("status").textContent=state.warning||"";
  });
})();
