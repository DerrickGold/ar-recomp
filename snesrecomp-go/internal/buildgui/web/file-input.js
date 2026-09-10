/* Localized, browser-independent presentation for file inputs.

   Native file controls borrow their button and empty-state text from the
   browser or operating system, not from the Builder's selected language. The
   real input remains stretched over this presentation, so keyboard access,
   labels, form submission and the browser's trusted file picker all retain
   their native behavior. Only the pixels underneath it are ours. */
(() => {
  "use strict";
  const ui=window.workshopI18n;
  const views=new WeakMap();

  function refresh(input) {
    const view=views.get(input);
    if(!view) return;
    ui.set(view.action,"builder.file.choose");
    const file=input.files?.[0];
    if(file){
      ui.unbind(view.name);
      view.name.textContent=file.name;
      view.wrapper.dataset.hasFile="true";
    } else {
      ui.set(view.name,"builder.file.none");
      view.wrapper.dataset.hasFile="false";
    }
  }

  function enhance(input) {
    if(!input || input.type!=="file" || views.has(input)) return input;
    const wrapper=document.createElement("span");
    wrapper.className="file-control";
    const visual=document.createElement("span");
    visual.className="file-control-view";
    visual.setAttribute("aria-hidden","true");
    const action=document.createElement("span");
    action.className="file-control-action";
    const name=document.createElement("span");
    name.className="file-control-name";
    visual.append(action,name);
    input.before(wrapper);
    wrapper.append(input,visual);
    views.set(input,{wrapper,action,name});
    input.addEventListener("change",()=>queueMicrotask(()=>refresh(input)));
    refresh(input);
    return input;
  }

  function enhanceAll(root=document) {
    if(root.matches?.('input[type="file"]')) enhance(root);
    root.querySelectorAll?.('input[type="file"]').forEach(enhance);
  }

  enhanceAll();
  document.addEventListener("workshop:language",()=>{
    document.querySelectorAll('input[type="file"]').forEach(input=>{
      enhance(input); refresh(input);
    });
  });
  document.addEventListener("reset",event=>{
    if(event.target instanceof HTMLFormElement) setTimeout(()=>{
      event.target.querySelectorAll('input[type="file"]').forEach(input=>{
        enhance(input); refresh(input);
      });
    },0);
  },true);
  window.workshopFileInputs=Object.freeze({enhance,enhanceAll,refresh});
})();
