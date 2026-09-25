/* Regional media is parsed/extracted/installed by the game-owned Go service.
 * This controller only handles selection, confirmation and translated status. */
(() => {
  "use strict";
  const fetchResponse = (...args) => window.workshopFeedback?.request ?
    window.workshopFeedback.request(...args) : fetch(...args);
  const ui=window.workshopI18n;
  const form=document.getElementById("regional-media-form");
  const input=document.getElementById("regional-media-file");
  const status=document.getElementById("regional-media-status");
  const donors=document.getElementById("regional-media-donors");
  const refresh=document.getElementById("regional-media-refresh");
  const names={us:"builder.media.us",jp:"builder.media.jp","eu-en":"builder.media.eu",de:"builder.media.de",fr:"builder.media.fr"};
  const states={missing:"builder.media.missing",installed:"builder.media.installed",invalid:"builder.media.invalid"};
  let busy=false,closed=false;
  function errorMessage(error,operation){
    ui.set(status,error.uiKey||"builder.media.failed",error.uiArgs||{});
    window.workshopFeedback?.show(status,error,{operation,retry:load});
  }
  async function read(response,acceptedStatuses=[]){
    if(window.workshopFeedback?.readJSON)return window.workshopFeedback.readJSON(response,acceptedStatuses);
    const body=await response.json();
    if(!response.ok&&!acceptedStatuses.includes(response.status))throw new Error(body.error||String(response.status));
    return body;
  }
  async function inventory(){
    const data=await read(await fetchResponse("regional-media"));
    donors.replaceChildren(...data.donors.map(row=>{
      const li=document.createElement("li"),name=document.createElement("strong"),value=document.createElement("span"),detail=document.createElement("small");
      ui.set(name,names[row.release]);
      ui.set(value,row.release==="us" && row.status==="missing"?"builder.media.baseline":states[row.status]||states.invalid);
      ui.set(detail,row.release==="us"?"builder.media.us_help":row.release==="jp"?"builder.media.jp_help":"builder.media.eu_help");
      li.append(name,value,detail);return li;
    }));
  }
  async function run(action,operation){
    if(busy||closed)return;busy=true;
    for(const node of form.querySelectorAll("input,button"))node.disabled=true;
    window.workshopFeedback?.clear(status);ui.set(status,"builder.media.working");
    try { await action(); } catch(error){errorMessage(error,operation);}
    finally {busy=false;for(const node of form.querySelectorAll("input,button"))node.disabled=closed;}
  }
  function load(){return run(async()=>{await inventory();ui.set(status,"builder.media.ready");},"Read regional media status");}
  async function install(file,replace){
    const body=new FormData();body.append("donor",file);body.append("replace",String(replace));
    const response=await fetchResponse("regional-media",{method:"POST",body});
    const data=await read(response,[409]);
    if(response.status===409 && data.code==="replace_required" && !replace){
      if(!window.confirm(ui.text("builder.media.replace",{region:ui.text(names[data.release])}))){ui.set(status,"builder.media.cancelled");return null;}
      return install(file,true);
    }
    if(!response.ok)throw window.workshopFeedback?.responseError ?
      window.workshopFeedback.responseError(data,response.status) : new Error(data.error||String(response.status));
    return data;
  }
  form.addEventListener("submit",event=>{
    event.preventDefault();const file=input.files?.[0];if(!file)return;
    run(async()=>{
      const result=await install(file,false);if(!result)return;
      try { await inventory(); }
      catch(error){
        error.uiKey="builder.media.installed_refresh_failed";
        error.uiArgs={detail:error.message};
        error.outcome="Regional media installed; status refresh failed.";
        error.operation="Refresh regional media after installation";
        throw error;
      }
      ui.set(status,result.changed?"builder.media.done":"builder.media.unchanged");
      form.reset();
    },"Install regional media");
  });
  refresh.addEventListener("click",load);
  document.querySelector('[data-asset-category="regions"]').addEventListener("click",load);
  document.addEventListener("workshop:navigate",()=>{if(!form.hidden && !document.getElementById("panel-assets").hidden)load();});
  document.addEventListener("workshop:closed",()=>{closed=true;for(const node of form.querySelectorAll("input,button"))node.disabled=true;});
})();
