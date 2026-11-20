(() => {
  "use strict";
  const fetchResponse = (...args) => window.workshopFeedback?.request ?
    window.workshopFeedback.request(...args) : fetch(...args);
  const $=id=>document.getElementById(id);
  const ui = window.workshopI18n;
  const set = (id, key, args) => ui.set($(id), "builder.output." + key, args);
  function status(message = "") {
    ui.unbind($("status"));
    $("status").textContent = message;
  }
  if (window.parent && window.parent !== window) document.body.dataset.embedded = "true";
  let plan=null, busy=false, saved=false, current="", suggested="", bigScreen=false, focusAfter;
  async function request(endpoint, body) {
    const response=await fetchResponse(endpoint,body===undefined?{cache:"no-store"}:{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
    if(window.workshopFeedback)return window.workshopFeedback.readJSON(response);
    const value=await response.json(); if(!response.ok)throw Error(value.error||String(response.status)); return value;
  }
  function invalidate(){status();plan=null;$("review").hidden=true;$("confirm").checked=false;$("apply").disabled=true;}
  function controls(){
    for(const node of document.querySelectorAll("button,input"))node.disabled=busy||saved;
    $("apply").disabled=busy||saved||!plan||(plan.existing&&!$("confirm").checked);
  }
  async function run(action){
    if(busy||saved)return;busy=true;window.workshopFeedback?.clear($("status"));controls();set("status","working");
    try{await action();}catch(error){invalidate();status(error.message);window.workshopFeedback?.show($("status"),error,{operation:ui.text("builder.output.choose")});}
    finally{busy=false;controls();if(focusAfter&&!saved)focusAfter.focus();focusAfter=null;}
  }
  $("directory").addEventListener("input",invalidate);
  $("confirm").addEventListener("change",controls);
  $("browse").addEventListener("click",()=>run(async()=>{
    invalidate();const result=await request("choose",{});if(result.directory)$("directory").value=result.directory;
    set("status","review_first");
  }));
  async function review(directory){
    invalidate();plan=await request("review",{directory});$("directory").value=plan.directory;
    $("review-path").textContent=plan.directory;
    set("review-title", plan.game ? "existing_game" : plan.existing ? "occupied" : "new_folder");
    set("review-description", plan.game ? "existing_help" : plan.existing ? "occupied_help" : "new_help");
    $("entries").replaceChildren(...(plan.entries||[]).map(name=>{const li=document.createElement("li");li.textContent=name;return li;}));
    $("confirm-label").hidden=!plan.existing;$("review").hidden=false;
    set("apply", current ? "save_next" : plan.game ? "open_existing" : "use_folder");
    if (current && !document.body.dataset.embedded) set("status","next_launch");
    else status();
    if(bigScreen)focusAfter=plan.existing?$("confirm"):$("apply");
  }
  async function apply(){
    const result=await request("apply",{revision:plan.revision,confirm:$("confirm").checked});saved=true;
    set("status", result.restartRequired ? "saved" : "starting", {path:result.directory});
    if(!result.restartRequired)location.replace("../../");
  }
  $("folder-form").addEventListener("submit",event=>{
    event.preventDefault();return run(()=>review($("directory").value));
  });
  $("apply").addEventListener("click",()=>run(apply));
  $("use-suggested").addEventListener("click",()=>run(async()=>{
    focusAfter=$("use-suggested");
    await review(suggested);
    if(!plan.existing){focusAfter=$("use-suggested");await apply();}
  }));
  run(async()=>{
    const state=await request("state");current=state.current;$("directory").value=state.next||state.candidate||"";
    $("current").hidden=!current;
    if (current) {
      set("current","current",{path:current});
      set("intro",document.body.dataset.embedded ? "import_hint" : "next_launch");
    }
    if (state.warning) set("status",state.warning,{detail:state.warningDetail});
    else status();
    bigScreen=!!state.bigScreen&&!current;
    if(bigScreen){
      document.body.dataset.bigScreen="true";
      suggested=state.suggested;
      $("suggested-folder").hidden=!suggested;
      $("suggested-path").textContent=suggested;
      focusAfter=suggested?$("use-suggested"):$("preview");
      window.workshopBigScreenNavigation(document.body,()=>{
        if(!plan||busy||saved)return;
        invalidate();
        (suggested?$("use-suggested"):$("preview")).focus();
      });
    }
  });
})();
