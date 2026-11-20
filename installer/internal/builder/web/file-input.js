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
  const checking=new WeakMap(), validated=new WeakSet();
  const fileExt=name=>name.slice(name.lastIndexOf(".")).toLowerCase();
  const startsWith=(bytes,signature,offset=0)=>
    Array.from(signature).every((character,i)=>bytes[offset+i]===character.charCodeAt(0));
  const vorbisExtension=ext=>ext===".ogg"||ext===".oga";
  function vorbisHeader(bytes){
    const segments=bytes[26], packet=27+segments;
    return segments>0&&bytes.length>=packet+30&&startsWith(bytes,"OggS")&&
      bytes[4]===0&&(bytes[5]&3)===2&&bytes[27]===30&&startsWith(bytes,"\x01vorbis\0\0\0\0",packet)&&
      bytes[packet+11]>0&&bytes.subarray(packet+12,packet+16).some(Boolean)&&(bytes[packet+29]&1)===1;
  }
  const headers={
    ".ogg": vorbisHeader,
    ".oga": vorbisHeader,
    ".png": bytes=>startsWith(bytes,"\x89PNG\r\n\x1a\n"),
    ".pdf": bytes=>bytes.some((byte,offset)=>byte===37&&startsWith(bytes,"%PDF-",offset)),
    ".o2r": bytes=>startsWith(bytes,"PK\x03\x04"),
  };

  function reject(input,file,types,key){
    input.value="";
    const args={name:file.name,types};
    window.workshopFeedback?.show(input,new Error(ui.text(key,args)),{
      operation:"Choose file",messageKey:key,messageArgs:args,
    });
  }

  document.addEventListener("change",event=>{
    const input=event.target;
    if(input.type!=="file"||validated.has(event)||input.hasAttribute?.("data-validate-batch"))return;
    checking.delete(input);
    input.setCustomValidity("");
    window.workshopFeedback?.clear(input);
    const extensions=input.accept.toLowerCase().split(",").map(type=>type.trim()).filter(type=>type.startsWith("."));
    const files=[...input.files];
    const wrongType=extensions.length&&files.find(file=>!extensions.includes(fileExt(file.name)));
    if(wrongType){
      const key=extensions.some(vorbisExtension)?"builder.file.invalid_vorbis":"builder.file.wrong_type";
      reject(input,wrongType,extensions.join(", "),key);
      return;
    }
    const toCheck=files.filter(file=>headers[fileExt(file.name)]);
    if(!toCheck.length)return;
    event.stopImmediatePropagation();
    const pending={};
    checking.set(input,pending);
    input.setCustomValidity(ui.text("builder.file.checking"));
    void (async()=>{
      for(const file of toCheck){
        const ext=fileExt(file.name);
        const ok=await file.slice(0,1024).arrayBuffer()
          .then(bytes=>headers[ext](new Uint8Array(bytes)),()=>false);
        if(checking.get(input)!==pending)return;
        if(!ok){
          reject(input,file,ext,vorbisExtension(ext)?"builder.file.invalid_vorbis":"builder.file.invalid_content");
          break;
        }
      }
      checking.delete(input);
      input.setCustomValidity("");
      const change=new Event("change",{bubbles:true});
      validated.add(change);
      input.dispatchEvent(change);
    })();
  },true);

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
    if(!input || input.type!=="file" || input.hidden || views.has(input)) return input;
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
