import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";

const source=["../../workshopui/interface-text.js", "../web/i18n.js"].map(path=>readFileSync(new URL(path,import.meta.url),"utf8")).join("\n");
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
export function setup(initial="en",{bigScreen=false}={}) {
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
  if(bigScreen){
    doc.createElement=tag=>{
      const node=new Node(tag);
      node.append=child=>node.children.push(child);
      node.focus=()=>{doc.activeElement=node;};
      return node;
    };
    const dialog=new Node("dialog");
    dialog.showModal=()=>{dialog.open=true;};
    dialog.close=()=>{dialog.open=false;};
    Object.assign(nodes,{
      "big-screen-language":doc.createElement("button"),
      "big-screen-language-name":new Node("span"),
      "big-screen-language-dialog":dialog,
      "big-screen-language-options":doc.createElement("div"),
      "big-screen-language-status":new Node("p"),
    });
    picker.options=[{value:"en",textContent:"English"},{value:"fr",textContent:"Français"},
      {value:"de",textContent:"Deutsch"},{value:"ja",textContent:"日本語"}];
  }
  const requests=[];let responder=async options=>({ok:true,json:async()=>JSON.parse(options.body)});
  const window={};
  const context={document:doc,window,Intl,setTimeout,clearTimeout,Event:class{constructor(type){this.type=type;}},fetch:async(path,options)=>{requests.push({path,options});return responder(options);}};
  runInNewContext(source,context);
  return {picker,status,label,input,textarea,localeField,files,parent,raw,attr,doc,context,ui:window.workshopI18n,requests,setResponder(fn){responder=fn;}};
}
