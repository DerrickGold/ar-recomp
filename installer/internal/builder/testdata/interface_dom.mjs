import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";

const source=readFileSync(new URL("../web/i18n.js",import.meta.url),"utf8");
const entries=JSON.parse(readFileSync(new URL("../../interfacecatalog/messages.json",import.meta.url),"utf8"));
export const messages=Object.fromEntries(entries.map(entry=>[entry.key,entry.text]));
const bindingNames=["data-i18n","data-i18n-title","data-i18n-placeholder","data-i18n-aria","data-i18n-alt"];
export class Node {
  constructor(tag,text="",attributes={}) {
    this.tagName=tag.toUpperCase();this.textContent=text;this.children=[];
    this.attributes=new Map(Object.entries(attributes));this.listeners=new Map();
    this.value="";this.disabled=false;this.hidden=false;
    const dataName=name=>"data-"+name.replace(/[A-Z]/g,c=>"-"+c.toLowerCase());
    this.dataset=new Proxy({}, {get:(_,name)=>this.getAttribute(dataName(name)),set:(_,name,value)=>{this.setAttribute(dataName(name),value);return true;}});
  }
  hasAttribute(name){return this.attributes.has(name);}
  getAttribute(name){return this.attributes.get(name)??null;}
  setAttribute(name,value){this.attributes.set(name,String(value));}
  removeAttribute(name){this.attributes.delete(name);}
  matches(){return bindingNames.some(name=>this.hasAttribute(name));}
  querySelectorAll(){return this.children.flatMap(child=>[...(child.matches()?[child]:[]),...child.querySelectorAll()]);}
  addEventListener(name,fn){const list=this.listeners.get(name)||[];list.push(fn);this.listeners.set(name,list);}
  async fire(name,event={}){for(const fn of this.listeners.get(name)||[]) await fn({type:name,...event});}
}
export function setup(initial="en") {
  const picker=new Node("select"),status=new Node("p"),label=new Node("span","Save",{"data-i18n":"common.save"});
  const input=new Node("input","",{"data-i18n":"common.save"}); input.value="Sir ÉLISE — {name}";
  const textarea=new Node("textarea","Native {name}",{"data-i18n":"common.save"});textarea.value="Unsaved 日本語";
  textarea.selectionStart=4;textarea.selectionEnd=9;
  const localeField=new Node("input");localeField.value="en-CA";
  const files=new Node("input");files.files=[{name:"my-font.ttf"}];
  const parent=new Node("button","",{"data-i18n":"common.save"});parent.children=[new Node("span","✦"),new Node("span","Icon button")];
  const raw=new Node("span","common.save"); // A package name, deliberately not bound.
  const attr=new Node("input","",{"data-i18n-placeholder":"builder.nav.home","placeholder":"Home"});attr.value="Leave me alone";
  const payload=new Node("script",JSON.stringify({locale:initial,locales:["en","fr","de","ja"],messages}));
  const nodes={"interface-catalog":payload,"interface-language":picker,"interface-language-status":status};
  const doc=new Node("document");doc.children=[picker,status,label,input,textarea,localeField,files,parent,raw,attr];
  doc.getElementById=id=>nodes[id];doc.documentElement={lang:"en"};doc.activeElement=textarea;
  doc.dispatchEvent=event=>{void doc.fire(event.type);};
  const requests=[];let responder=async options=>({ok:true,json:async()=>JSON.parse(options.body)});
  const window={};
  const context={document:doc,window,Intl,setTimeout,clearTimeout,Event:class{constructor(type){this.type=type;}},fetch:async(path,options)=>{requests.push({path,options});return responder(options);}};
  runInNewContext(source,context);
  return {picker,status,label,input,textarea,localeField,files,parent,raw,attr,doc,context,ui:window.workshopI18n,requests,setResponder(fn){responder=fn;}};
}
