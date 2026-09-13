/* Local, opt-in diagnostics. Never collect request bodies, ROMs, editor text,
   cookies or URLs. Reports contain only the supplied error and optional log. */
(() => {
  "use strict";
  const cards=new WeakMap();
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
  async function readJSON(response){
    let body;
    try { body=await response.json();if(body===null||typeof body!=="object")throw new Error("invalid response"); }
    catch {
      const error=new Error(text("unreadable","The Builder returned an unreadable response. Check the details below before retrying."));
      error.code="AR_RESPONSE";error.status=response.status;throw error;
    }
    if(!response.ok){
      const error=new Error(typeof body?.error==="string"?body.error:text("failed","The operation could not be completed."));
      error.code=body?.errorCode||"AR_HTTP_"+response.status;error.status=response.status;throw error;
    }
    return body;
  }
  function clear(target){
    const prior=cards.get(target);if(prior){prior.card.remove();cards.delete(target);}
  }
  function show(target,error,options={}){
    if(!target)return;
    const network=error instanceof TypeError||error?.code==="AR_NETWORK";
    const detail=sanitize(error?.uiArgs?.detail||error?.message||error||"Unknown error");
    const code=error?.code||error?.status&&"AR_HTTP_"+error.status||(network?"AR_NETWORK":"AR_OPERATION");
    const version=document.querySelector('meta[name="workshop-version"]')?.content;
    const report=["ActRaiserRecomp Workshop", "Operation: "+sanitize(options.operation||"Workshop"),
      version?"Builder version: "+sanitize(version):"",
      "Code: "+sanitize(code), error?.status?"HTTP status: "+error.status:"",
      "Client: "+sanitize(navigator.userAgent||"unavailable"), "Details: "+detail,
      options.log?"Recent build log (tail):\n"+sanitize(String(options.log).slice(-16000)):""].filter(Boolean).join("\n");
    if(cards.get(target)?.report===report)return;
    clear(target);
    const card=document.createElement("section");card.className="workshop-feedback";
    const hint=document.createElement("p");label(hint,network?"connection":"help",network
      ?"The connection to the local Builder was interrupted. Check status again; save any open edits before closing or restarting."
      :"Review the details below. If you need help, copy the report and include what you were trying to do. Save open edits before restarting.");
    const details=document.createElement("details"),summary=document.createElement("summary"),reportBox=document.createElement("textarea");
    label(summary,"details","Error details");reportBox.readOnly=true;reportBox.rows=8;reportBox.value="Recorded: "+new Date().toISOString()+"\n"+report;
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
    card.append(hint,details,note,actions,status);target.after(card);cards.set(target,{card,report});
  }
  window.workshopFeedback=Object.freeze({sanitize,readJSON,show,clear});
})();
