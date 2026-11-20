/* Local, opt-in diagnostics. Never collect request bodies, ROMs, editor text,
   cookies or URLs. Reports contain only the supplied error and optional log. */
(() => {
  "use strict";
  const cards=new WeakMap();
  const pending=[];
  let active, nextID=0, closed=false;
  document.addEventListener("workshop:closed",()=>{
    closed=true;
    pending.length=0;
    active?.dialog.close();
  },{once:true});
  const text=(key,fallback)=>window.workshopI18n?.text("builder.feedback."+key,{},fallback)||fallback;
  const label=(node,key,fallback)=>{if(window.workshopI18n)window.workshopI18n.set(node,"builder.feedback."+key);else node.textContent=fallback;};
  function sanitize(value){
    return String(value??"").replace(/\x1b\[[0-9;]*[A-Za-z]/g,"")
      .replace(/https?:\/\/[^\s<>"']+/gi,"[address omitted]")
      .replace(/\b[a-f0-9]{36}\b/gi,"[session token]")
      .replace(/\/(?:Users|home)\/[^/\s]+/g,"~")
      .replace(/[A-Z]:\\Users\\[^\\\r\n]+/gi,"~")
      .replace(/\b(?:authorization|cookie|token|password|secret)\s*[:=]\s*[^\r\n]+/gi,"[sensitive field omitted]");
  }
  // Classify only a failed fetch as a connection problem. A TypeError raised
  // while rendering or processing a response is an application error.
  async function request(input,options){
    try { return await fetch(input,options); }
    catch(error){
      if(error?.name==="TypeError"){
        const failure=new Error(text("connection","The connection to the local Builder was interrupted. Check its status before retrying."));
        failure.code="AR_NETWORK";failure.detail=error.message;throw failure;
      }
      throw error;
    }
  }
  function responseError(body,status){
    const summary=typeof body?.error==="string"?body.error:text("failed","The operation could not be completed.");
    const detail=typeof body?.detail==="string"?body.detail:summary;
    const code=typeof body?.errorCode==="string"?body.errorCode:"AR_HTTP_"+status;
    const known=code.startsWith("builder.")&&window.workshopI18n?.text(code,{},"");
    const error=new Error(known?window.workshopI18n.text(code,{detail}):summary);
    error.code=code;error.status=status;error.detail=detail;
    if(known){error.uiKey=code;error.uiArgs={detail};}
    if(typeof body?.recoveryKey==="string"&&body.recoveryKey.startsWith("builder.recovery."))
      error.recoveryKey=body.recoveryKey;
    return error;
  }
  async function readJSON(response,acceptedStatuses=[]){
    let body;
    try { body=await response.json();if(body===null||typeof body!=="object")throw new Error("invalid response"); }
    catch {
      const error=new Error(text("unreadable","The Builder returned an unreadable response. Check the details below before retrying."));
      error.code="AR_RESPONSE";error.status=response.status;throw error;
    }
    if(!response.ok&&!acceptedStatuses.includes(response.status))throw responseError(body,response.status);
    return body;
  }
  function present(entry){
    if(closed||active===entry||pending.includes(entry))return;
    if(active){pending.push(entry);return;}
    active=entry;
    document.body.append(entry.dialog);
    entry.dialog.showModal();
    entry.close.focus();
  }
  function finish(entry){
    if(active!==entry)return;
    entry.dialog.remove();
    active=undefined;
    if(pending.length)present(pending.shift());
  }
  function clear(target){
    const prior=cards.get(target);
    if(!prior)return;
    cards.delete(target);
    const index=pending.indexOf(prior);
    if(index>=0)pending.splice(index,1);
    if(active===prior)prior.dialog.close();
  }
  function reopen(target){
    const entry=cards.get(target);
    if(closed||!entry)return false;
    present(entry);
    return true;
  }
  function show(target,error,options={}){
    if(closed||!target)return;
    const network=error?.code==="AR_NETWORK";
    const detail=sanitize(error?.detail||error?.uiArgs?.detail||error?.message||error||"Unknown error");
    const code=error?.code||error?.status&&"AR_HTTP_"+error.status||(network?"AR_NETWORK":"AR_OPERATION");
    const version=document.querySelector('meta[name="workshop-version"]')?.content;
    const report=["ActRaiserRecomp Workshop", "Operation: "+sanitize(error?.operation||options.operation||"Workshop"),
      error?.outcome?"Outcome: "+sanitize(error.outcome):"",
      version?"Builder version: "+sanitize(version):"",
      "Code: "+sanitize(code), error?.status?"HTTP status: "+error.status:"",
      "Client: "+sanitize(navigator.userAgent||"unavailable"), "Details: "+detail,
      options.log?"Recent build log (tail):\n"+sanitize(String(options.log).slice(-16000)):""].filter(Boolean).join("\n");
    const prior=cards.get(target);
    if(prior?.report===report){
      if(!options.deduplicate)present(prior);
      return;
    }
    clear(target);
    const dialog=document.createElement("dialog");dialog.className="workshop-feedback";
    const title=document.createElement("h2"), message=document.createElement("p");
    title.id="workshop-feedback-title-"+(++nextID);
    message.id=title.id+"-message";
    dialog.setAttribute("aria-labelledby",title.id);
    dialog.setAttribute("aria-describedby",message.id);
    label(title,"failed","Action needed");
    if(window.workshopI18n&&(options.messageKey||error?.uiKey))
      window.workshopI18n.set(message,options.messageKey||error.uiKey,options.messageArgs||error.uiArgs);
    else message.textContent=error?.message||detail;
    const hint=document.createElement("p");label(hint,network?"connection":"help",network
      ?"The connection to the local Builder was interrupted. Check status again; save any open edits before closing or restarting."
      :"Keep your current work open. If the cause is unclear, copy this report for support and describe the action you were taking.");
    if(error?.recoveryKey&&window.workshopI18n)
      window.workshopI18n.set(hint,error.recoveryKey);
    const details=document.createElement("details"),summary=document.createElement("summary"),reportBox=document.createElement("textarea");
    label(summary,"details","Error details");reportBox.readOnly=true;reportBox.rows=8;reportBox.value="Recorded: "+new Date().toISOString()+"\n"+report;
    reportBox.tabIndex=0;
    reportBox.setAttribute("data-big-scroll","");
    reportBox.setAttribute("data-big-scroll-when-focused","");
    if(window.workshopI18n)window.workshopI18n.attribute(reportBox,"aria-label","builder.feedback.report");
    else reportBox.setAttribute("aria-label",text("report","Error report"));
    reportBox.spellcheck=false;
    details.append(summary,reportBox);
    const note=document.createElement("p");note.className="feedback-note";
    label(note,"review","Review before sharing: reports may include local file paths. No ROM files or editor drafts are attached.");
    const actions=document.createElement("div");actions.className="feedback-actions";
    const copy=document.createElement("button");copy.type="button";copy.className="secondary";label(copy,"copy","Copy error details");
    const status=document.createElement("p");status.className="feedback-copy-status";status.setAttribute("role","status");
    copy.addEventListener("click",async()=>{
      copy.disabled=true;
      try {
        // Copy exactly the previewed report, not additional hidden diagnostics.
        await navigator.clipboard.writeText(reportBox.value);
        label(status,"copied","Error details copied.");
      } catch {
        details.open=true;reportBox.focus();reportBox.select();
        label(status,"manual","Automatic copying is unavailable. The report is selected; use your system’s Copy command.");
      } finally {copy.disabled=false;}
    });
    actions.append(copy);
    if(options.retry){
      const retry=document.createElement("button");retry.type="button";retry.className="secondary";label(retry,"retry","Check status again");
      retry.addEventListener("click",async()=>{retry.disabled=true;try{await options.retry();}catch(error){show(target,error,options);}finally{retry.disabled=false;}});
      actions.append(retry);
    }
    const close=document.createElement("button");close.type="button";label(close,"close","Close");
    close.addEventListener("click",()=>dialog.close());
    actions.append(close);
    dialog.append(title,message,hint,details,note,actions,status);
    const entry={dialog,close,report};
    dialog.addEventListener("close",()=>finish(entry));
    cards.set(target,entry);
    present(entry);
  }
  window.workshopFeedback=Object.freeze({sanitize,request,responseError,readJSON,show,clear,reopen});
})();
