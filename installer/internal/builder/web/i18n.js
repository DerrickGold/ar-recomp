(() => {
  "use strict";
  const bootstrap = JSON.parse(document.getElementById("interface-catalog").textContent);
  const {locales, messages} = bootstrap;
  const ui = window.workshopI18n;
  const {set} = ui;
  const picker=document.getElementById("interface-language");
  const status=document.getElementById("interface-language-status");
  const languageButton=document.getElementById("big-screen-language");
  const languageName=document.getElementById("big-screen-language-name");
  const languageDialog=document.getElementById("big-screen-language-dialog");
  const languageStatus=document.getElementById("big-screen-language-status");
  const languageChoices=new Map();
  const closeOnLaunch=document.getElementById("close-on-launch");
  if(closeOnLaunch) closeOnLaunch.checked=bootstrap.closeOnLaunch===true;
  if(bootstrap.preferenceError){ status.hidden=false; set(status,"builder.interface.read_failed"); }
  let saving=false,closed=false,statusTimer=0;

  function syncLanguage(){
    picker.value=ui.locale;
    if(!languageButton) return;
    languageName.textContent=languageChoices.get(ui.locale)?.textContent || ui.locale;
    languageName.lang=ui.locale;
    for(const [choice,button] of languageChoices) button.setAttribute("aria-pressed",String(choice===ui.locale));
  }

  function syncSaving(){
    picker.disabled=saving||closed;
    if(saving) picker.setAttribute("aria-busy","true");
    else picker.removeAttribute("aria-busy");
    if(languageButton) languageButton.disabled=closed;
    for(const button of languageChoices.values()) button.setAttribute("aria-disabled",String(saving||closed));
  }

  async function saveLanguage(requested, feedback){
    if(saving||closed||!locales.includes(requested)){
      syncLanguage();
      return false;
    }
    clearTimeout(statusTimer);
    saving=true;
    syncSaving();
    status.hidden=feedback!==status;
    if(feedback===status) status.className="sr-only";
    feedback.hidden=false;
    set(feedback,"builder.interface.saving");
    try {
      const response=await fetch("interface/preferences",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({language:requested})});
      const result=await response.json();
      if(!response.ok) throw new Error(result.errorCode||"builder.interface.save_failed");
      if(result.language!==requested) throw new Error("builder.interface.save_failed");
      if(closed) return false;
      ui.setLocale(requested);
      // No navigation, editor reload, or form replacement. Focus, selection,
      // unsaved messages, file inputs and game-pack ui.locale all stay untouched.
      syncLanguage();
      set(feedback,"builder.interface.saved");
      document.dispatchEvent(new Event("workshop:language"));
      statusTimer=setTimeout(()=>{ if(!closed&&ui.locale===requested) feedback.hidden=true; },3500);
      return true;
    } catch(error) {
      if(!closed){
        syncLanguage();
        if(feedback===status) status.className="";
        set(feedback,Object.hasOwn(messages,error.message)?error.message:"builder.interface.save_failed");
      }
      return false;
    } finally {
      saving=false;
      syncSaving();
      if(feedback===status&&languageStatus) languageStatus.hidden=true;
    }
  }

  picker.addEventListener("change",()=>saveLanguage(picker.value,status));
  if(languageButton){
    const options=document.getElementById("big-screen-language-options");
    let row;
    for(const option of picker.options){
      if(!locales.includes(option.value)) continue;
      if(languageChoices.size%2===0){
        row=document.createElement("div");
        row.dataset.bigBand="";
        options.append(row);
      }
      const button=document.createElement("button");
      button.type="button"; button.className="secondary"; button.lang=option.value;
      button.textContent=option.textContent;
      button.addEventListener("click",async()=>{
        if(saving||closed) return;
        if(option.value===ui.locale||await saveLanguage(option.value,languageStatus)){
          if(languageDialog.open) languageDialog.close();
        }
      });
      languageChoices.set(option.value,button);
      row.append(button);
    }
    languageButton.addEventListener("click",()=>{
      if(closed||languageDialog.open) return;
      languageStatus.hidden=!saving;
      if(saving) set(languageStatus,"builder.interface.saving");
      languageButton.focus();
      languageDialog.showModal();
      languageChoices.get(ui.locale)?.focus();
    });
  }
  syncLanguage();
  document.addEventListener("workshop:closed",()=>{
    closed=true;
    clearTimeout(statusTimer);
    syncSaving();
  });
})();
