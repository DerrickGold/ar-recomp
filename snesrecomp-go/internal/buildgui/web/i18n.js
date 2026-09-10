/* Host interface text only. Explicit bindings never inspect or translate game
   scripts, user-entered values, package names, IDs or author credits. */
(() => {
  "use strict";
  const bootstrap = JSON.parse(document.getElementById("interface-catalog").textContent);
  const locales = Object.freeze([...bootstrap.locales]);
  const messages = bootstrap.messages;
  let locale = locales.includes(bootstrap.locale) ? bootstrap.locale : "en";
  let numberFormatter = new Intl.NumberFormat(locale);
  const argumentsByNode = new WeakMap();
  const attributeArgumentsByNode = new WeakMap();
  const originalByNode = new WeakMap();
  const attributes = Object.freeze({"data-i18n-title":"title", "data-i18n-placeholder":"placeholder", "data-i18n-aria":"aria-label", "data-i18n-alt":"alt"});

  function text(key, args = {}, fallback = key) {
    const entry = Object.hasOwn(messages,key) ? messages[key] : null;
    const template = entry?.[locales.indexOf(locale)] ?? entry?.[0] ?? fallback ?? key;
    return template.replace(/\{([a-z_][a-z0-9_]*)\}/g, (match,name) => Object.hasOwn(args,name)
      ? (typeof args[name]==="number" ? numberFormatter.format(args[name]) : String(args[name])) : match);
  }
  function applyNode(node) {
    // Bind text only to a dedicated leaf. Never replace controls or icon trees.
    if (node.hasAttribute("data-i18n") && !node.children.length && !["INPUT","TEXTAREA","SELECT"].includes(node.tagName)) {
      if (!originalByNode.has(node)) originalByNode.set(node,node.textContent);
      node.textContent = text(node.dataset.i18n,argumentsByNode.get(node),originalByNode.get(node));
    }
    for (const [binding,attribute] of Object.entries(attributes)) {
      if (node.hasAttribute(binding)) node.setAttribute(attribute,text(node.getAttribute(binding),attributeArgumentsByNode.get(node)?.[attribute],node.getAttribute(attribute)));
    }
  }
  function apply(root = document) {
    const selector = "[data-i18n],[data-i18n-title],[data-i18n-placeholder],[data-i18n-aria],[data-i18n-alt]";
    if (root.matches?.(selector)) applyNode(root);
    root.querySelectorAll(selector).forEach(applyNode);
  }
  function set(node, key, args = {}) {
    if (!node || node.children.length || ["INPUT","TEXTAREA","SELECT"].includes(node.tagName)) throw new Error("interface binding requires a text leaf");
    argumentsByNode.set(node,Object.freeze({...args}));
    node.dataset.i18n=key;
    applyNode(node);
  }
  function unbind(node) {
    node.removeAttribute("data-i18n");
    argumentsByNode.delete(node);
    originalByNode.delete(node);
  }
  function attribute(node, name, key, args = {}) {
    const binding=Object.keys(attributes).find(binding=>attributes[binding]===name);
    if(!node || !binding) throw new Error("unsupported interface attribute");
    attributeArgumentsByNode.set(node,{...attributeArgumentsByNode.get(node),[name]:Object.freeze({...args})});
    node.setAttribute(binding,key); applyNode(node);
  }
  const api = Object.freeze({text,apply,set,unbind,attribute,get locale(){ return locale; },
    number(value,options){ return (options?new Intl.NumberFormat(locale,options):numberFormatter).format(value); }});
  window.workshopI18n = api;
  document.documentElement.lang=locale;
  apply();

  const picker=document.getElementById("interface-language");
  const status=document.getElementById("interface-language-status");
  picker.value=locale;
  if(bootstrap.preferenceError){ status.hidden=false; set(status,"builder.interface.read_failed"); }
  let saving=false,closed=false,statusTimer=0;
  document.addEventListener("workshop:closed",()=>{ closed=true; picker.disabled=true; clearTimeout(statusTimer); });
  picker.addEventListener("change",async()=>{
    if(saving||closed){ picker.value=locale; return; }
    const requested=picker.value;
    if(!locales.includes(requested)){ picker.value=locale; return; }
    clearTimeout(statusTimer);
    saving=true; picker.disabled=true; picker.setAttribute("aria-busy","true");
    status.hidden=false; set(status,"builder.interface.saving");
    try {
      const response=await fetch("interface/preferences",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({language:requested})});
      const result=await response.json();
      if(!response.ok) throw new Error(result.errorCode||"builder.interface.save_failed");
      if(result.language!==requested) throw new Error("builder.interface.save_failed");
      if(closed) return;
      locale=requested; document.documentElement.lang=locale;
      numberFormatter=new Intl.NumberFormat(locale);
      // No navigation, editor reload, or form replacement. Focus, selection,
      // unsaved messages, file inputs and game-pack locale all stay untouched.
      set(status,"builder.interface.saved"); apply();
      document.dispatchEvent(new Event("workshop:language"));
      statusTimer=setTimeout(()=>{ if(!closed&&locale===requested) status.hidden=true; },3500);
    } catch(error) {
      if(!closed){ picker.value=locale; set(status,Object.hasOwn(messages,error.message)?error.message:"builder.interface.save_failed"); }
    } finally { saving=false; picker.disabled=closed; picker.removeAttribute("aria-busy"); }
  });
})();
