/* Moving sidebar controls into a modal must preserve their identity, handlers,
 * draft values and sidebar order, even when a tool opens another workspace. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const manifest=fs.readFileSync(new URL('../tools/action_editor/editor.body.html',import.meta.url),'utf8');
const sections=[];
const elements=new Map();
let documentHost;
class Element {
  constructor(id='',tagName='DIV') {
    this.id=id;this.tagName=tagName;this.children=[];this.dataset={};this.listeners={};
    this.attributes={};this.style={};this.open=false;this.hidden=false;
    const classes=new Set();
    this.classList={toggle(name,on){if(on)classes.add(name);else classes.delete(name);},contains:name=>classes.has(name)};
  }
  addEventListener(name,callback){this.listeners[name]=callback;}
  setAttribute(name,value){this.attributes[name]=value;}
  append(node){node.remove();node.parentElement=this;this.children.push(node);}
  remove(){if(this.parentElement)this.parentElement.children.splice(this.parentElement.children.indexOf(this),1);this.parentElement=null;}
  before(node){node.remove();const parent=this.parentElement;node.parentElement=parent;parent.children.splice(parent.children.indexOf(this),0,node);}
  replaceWith(node){this.before(node);this.remove();}
  querySelector(selector){return selector==='summary'?this.children.find(node=>node.tagName==='SUMMARY'):null;}
  contains(node){return node===this||this.children.some(child=>child.contains(node));}
  closest(selector){
    if(selector==='button'&&this.tagName==='BUTTON'||selector==='dialog'&&this.tagName==='DIALOG'||
      selector==='[data-settings]'&&this.dataset.settings||selector===`#${this.id}`)return this;
    return this.parentElement?.closest(selector)||null;
  }
  focus(){documentHost.activeElement=this;}
  showModal(){this.open=true;}
  close(){this.open=false;this.listeners.close?.();}
  getBoundingClientRect(){return {width:260,height:420};}
  click(){this.onclick?.();}
}
function element(selector) {
  if(!elements.has(selector))elements.set(selector,new Element(selector.slice(1)));
  return elements.get(selector);
}
const sidebar=element('#editorSidebar');
for(const match of manifest.matchAll(/<details class="side-section[^"]*" data-settings="([a-z]+)"( open)?><summary>([^<]+)<\/summary>/g)) {
  const node=new Element('', 'DETAILS'),summary=new Element('', 'SUMMARY');
  node.dataset.settings=match[1];node.open=!!match[2];
  summary.textContent=match[3].replaceAll('&amp;','&');node.append(summary);
  sidebar.append(node);sections.push(node);
}
documentHost={activeElement:element('#editorSettingsOpen'),querySelector:element,
  querySelectorAll:()=>sections,createElement:tag=>new Element('',tag.toUpperCase())};
element('#settingsDlg').tagName='DIALOG';
const events={};
const context=vm.createContext({document:documentHost,window:{innerWidth:960,innerHeight:640,
  addEventListener(name,callback){events[name]=callback;}},$:element,cvs:element('#map2d'),mode:'2d',
  closeTileMenu(){}});
vm.runInContext(fs.readFileSync(new URL('../tools/action_editor/editor_layout.js',import.meta.url),'utf8'),context);
const run=code=>vm.runInContext(code,context);
assert.equal(sections.length,15);
assert.equal(element('#settingsNav').children.length,sections.length);
assert.equal(element('#settingsMenu').children.length,sections.length);
const originalOrder=[...sidebar.children];
const originalOpen=sections.map(node=>node.open);
// Every section is reachable, including with the sidebar and tools hidden.
element('#sidebarToggle').click();element('#toolbarToggle').click();
assert.equal(sidebar.hidden,true);
assert.equal(element('#editorApp').classList.contains('tools-hidden'),true);
assert.equal(element('#sidebarToggle').attributes['aria-expanded'],'false');
for(const section of sections) {
  run(`EditorLayout.openSettings('${section.dataset.settings}')`);
  assert.equal(element('#settingsDlg').open,true);
  assert.equal(element('#settingsContent').children[0],section);
  assert.equal(section.open,true);
  assert.equal(documentHost.activeElement,element('#settingsNav').children[sections.indexOf(section)]);
}
element('#settingsClose').click();
assert.deepEqual(sidebar.children,originalOrder);
assert.deepEqual(sections.map(node=>node.open),originalOpen);
assert.equal(run('EditorLayout.modalOpen()'),false);
// Draft values and handlers remain on the same live element after switching.
const policy=sections.find(node=>node.dataset.settings==='policy');
const input=new Element('bgPolicyLeft','INPUT');input.value='48';policy.append(input);
const handler=()=>{input.value='64';};input.oninput=handler;
run("EditorLayout.openSettings('policy');EditorLayout.openSettings('camera');EditorLayout.openSettings('policy')");
assert.equal(policy.children.at(-1),input);assert.equal(input.oninput,handler);
input.oninput();element('#settingsDlg').listeners.cancel({preventDefault(){}});
assert.equal(input.value,'64');assert.deepEqual(sidebar.children,originalOrder);
assert.equal(policy.open,false);
// Map tools close the modal before their own handler operates on the canvas.
const tool=new Element('framingAdjust','BUTTON');sections.find(node=>node.dataset.settings==='framing').append(tool);
run("EditorLayout.openSettings('framing')");
element('#settingsContent').listeners.click({target:tool});
assert.equal(element('#settingsDlg').open,false);
assert.deepEqual(sidebar.children,originalOrder);
// Disabled tools do not close the modal.
tool.disabled=true;run("EditorLayout.openSettings('framing')");
element('#settingsContent').listeners.click({target:tool});
assert.equal(element('#settingsDlg').open,true);element('#settingsClose').click();
// Every preview offers settings; 2D retains its existing tile action menu.
const menu=element('#settingsMenu'),app=element('#editorApp');
const rightClick=target=>({target,clientX:959,clientY:639,preventDefault(){this.prevented=true;}});
const tileClick=rightClick(element('#map2d'));app.listeners.contextmenu(tileClick);
assert.equal(tileClick.prevented,undefined);assert.equal(menu.hidden,true);
const previewClick=rightClick(element('#sharedGl'));app.listeners.contextmenu(previewClick);
assert.equal(previewClick.prevented,true);assert.equal(menu.hidden,false);
assert.equal(menu.style.left,'692px');assert.equal(menu.style.top,'212px');
const key=key=>({key,preventDefault(){},stopPropagation(){}});
menu.listeners.keydown(key('End'));assert.equal(documentHost.activeElement,menu.children.at(-1));
menu.listeners.keydown(key('Escape'));assert.equal(menu.hidden,true);
app.listeners.contextmenu(rightClick(input));
assert.equal(documentHost.activeElement,menu.children[sections.indexOf(policy)]);
documentHost.activeElement.click();assert.equal(element('#settingsTitle').textContent,'Background policy');
element('#settingsDlg').close();assert.deepEqual(sidebar.children,originalOrder);
element('#sidebarToggle').click();element('#toolbarToggle').click();
assert.equal(sidebar.hidden,false);assert.equal(element('#editorApp').classList.contains('tools-hidden'),false);
element('#collapseSidebarSections').click();assert(sections.every(node=>!node.open));
element('#collapseSidebarSections').click();assert(sections.every(node=>node.open));
console.log('Editor layout: all sidebar sections, live control identity, draft retention, close/cancel, tool handoff, panel toggles and context-menu keyboard navigation passed');
