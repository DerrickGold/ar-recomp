/* Moving sidebar controls into a modal must preserve their identity, handlers,
 * draft values and sidebar order, even when a tool opens another workspace. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const manifest=fs.readFileSync(new URL('../tools/action_editor/editor.body.html',import.meta.url),'utf8');
const sections=[];
const shortcuts=[],popovers=[];
const elements=new Map();
let documentHost;
class Element {
  constructor(id='',tagName='DIV') {
    this.id=id;this.tagName=tagName;this.children=[];this.dataset={};this.listeners={};
    this.attributes={};this.style={};this.open=false;this.hidden=false;this.value='';this.listenerOptions={};
    const classes=new Set();
    this.classList={toggle(name,on){if(on)classes.add(name);else classes.delete(name);},contains:name=>classes.has(name)};
  }
  addEventListener(name,callback,options){this.listeners[name]=callback;this.listenerOptions[name]=options;}
  setAttribute(name,value){this.attributes[name]=value;}
  append(node){node.remove();node.parentElement=this;this.children.push(node);}
  remove(){if(this.parentElement)this.parentElement.children.splice(this.parentElement.children.indexOf(this),1);this.parentElement=null;}
  before(node){node.remove();const parent=this.parentElement;node.parentElement=parent;parent.children.splice(parent.children.indexOf(this),0,node);}
  replaceWith(node){this.before(node);this.remove();}
  querySelector(selector){return selector==='summary'?this.children.find(node=>node.tagName==='SUMMARY'):null;}
  contains(node){return node===this||this.children.some(child=>child.contains(node));}
  closest(selector){
    if(selector.split(',').some(part=>part.toUpperCase()===this.tagName||
      part==='[data-settings]'&&this.dataset.settings||part==='[data-keep-open]'&&this.dataset.keepOpen!==undefined||
      part==='details.toolbar-popover'&&this.tagName==='DETAILS'&&this.classList.contains('toolbar-popover')||
      part===`#${this.id}`))return this;
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
for(const match of manifest.matchAll(/<details\b([^>]*)><summary>([^<]+)<\/summary>/g)) {
  if(!/class="[^"]*\bside-section\b/.test(match[1]))continue;
  const id=match[1].match(/data-settings="([a-z]+)"/);if(!id)continue;
  const node=new Element('', 'DETAILS'),summary=new Element('', 'SUMMARY');
  node.dataset.settings=id[1];node.open=/(?:^|\s)open(?:\s|$)/.test(match[1]);
  node.hidden=/(?:^|\s)hidden(?:\s|$)/.test(match[1]);
  summary.textContent=match[2].replaceAll('&amp;','&');node.append(summary);
  sidebar.append(node);sections.push(node);
}
for(const id of ['paint','policy','effects','framing','camera']) {
  const button=new Element('', 'BUTTON');button.dataset.openSettings=id;shortcuts.push(button);
}
for(let i=0;i<2;++i) {
  const details=new Element(`popover${i}`,'DETAILS'),summary=new Element('', 'SUMMARY');
  details.classList.toggle('toolbar-popover',true);
  details.append(summary);element('#editorApp').append(details);popovers.push(details);
}
documentHost={activeElement:element('#editorSettingsOpen'),querySelector:element,
  querySelectorAll:selector=>selector==='#editorSidebar > [data-settings]'?sections:
    selector==='[data-open-settings]'?shortcuts:selector==='details.toolbar-popover'?popovers:[],
  createElement:tag=>new Element('',tag.toUpperCase())};
element('#settingsDlg').tagName='DIALOG';
const events={},eventOptions={};
const context=vm.createContext({document:documentHost,window:{innerWidth:960,innerHeight:640,
  addEventListener(name,callback,options){
    const previous=events[name];events[name]=previous?event=>{previous(event);callback(event);}:callback;
    eventOptions[name]=options;
  }},$:element,cvs:element('#map2d'),mode:'2d',
  closeTileMenu(){}});
vm.runInContext(fs.readFileSync(new URL('../tools/action_editor/editor_layout.js',import.meta.url),'utf8'),context);
const run=code=>vm.runInContext(code,context);
assert.equal(sections.length,18);
assert(sections.some(section=>section.dataset.settings==='palette'));
assert.equal(element('#settingsNav').children.length,sections.length);
assert.equal(element('#settingsMenu').children.length,sections.length);
const originalOrder=[...sidebar.children];
const originalOpen=sections.map(node=>node.open);
// Every section is reachable, including with the sidebar and tools hidden.
element('#sidebarToggle').click();element('#toolbarToggle').click();
assert.equal(sidebar.hidden,true);
assert.equal(element('#editorApp').classList.contains('tools-hidden'),true);
assert.equal(element('#sidebarToggle').attributes['aria-expanded'],'false');
assert.equal(element('#sidebarToggle').textContent,'Room');
assert.equal(element('#toolbarToggle').textContent,'Show editing toolbar');
for(const section of sections) {
  if(section.hidden)continue;
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
// Voxel pointer gestures prevent compatibility mousedown. Pointerdown alone
// must dismiss the menu, while still letting that click reach the destination.
let swallowed=false;const destination=element('#sharedGl');destination.focus();
events.pointerdown({target:menu.children[0],preventDefault(){swallowed=true;},stopPropagation(){swallowed=true;}});
assert.equal(menu.hidden,false,'inside pointerdown preserves the chosen menu action');
events.pointerdown({target:destination,preventDefault(){swallowed=true;},stopPropagation(){swallowed=true;}});
assert.equal(menu.hidden,true,'outside pointerdown dismisses without a compatibility mousedown');
assert.equal(swallowed,false);assert.equal(documentHost.activeElement,destination,'dismissal does not steal destination focus');
app.listeners.contextmenu(rightClick(destination));
const key=key=>({key,preventDefault(){},stopPropagation(){}});
menu.listeners.keydown(key('End'));assert.equal(documentHost.activeElement,menu.children.at(-1));
menu.listeners.keydown(key('Escape'));assert.equal(menu.hidden,true);
app.listeners.contextmenu(rightClick(input));
assert.equal(documentHost.activeElement,menu.children[sections.indexOf(policy)]);
documentHost.activeElement.click();assert.equal(element('#settingsTitle').textContent,'Background policy');
element('#settingsDlg').close();assert.deepEqual(sidebar.children,originalOrder);
element('#sidebarToggle').click();element('#toolbarToggle').click();
assert.equal(sidebar.hidden,false);assert.equal(element('#editorApp').classList.contains('tools-hidden'),false);
assert.equal(element('#sidebarToggle').textContent,'Room');
assert.equal(element('#toolbarToggle').textContent,'Hide editing toolbar');

// Semantic search filters navigation without recreating or losing controls.
const search=element('#settingsSearch'),empty=element('#settingsEmpty'),nav=element('#settingsNav');
const navButton=id=>nav.children[sections.findIndex(s=>s.dataset.settings===id)];
const filter=text=>{search.value=text;search.oninput();};
run("EditorLayout.openSettings('policy')");
filter('sprites');assert.equal(navButton('receivers').hidden,false);assert.equal(navButton('policy').hidden,true);
search.focus();search.listeners.keydown(key('ArrowDown'));assert.equal(documentHost.activeElement,navButton('receivers'));
documentHost.activeElement.click();assert.equal(element('#settingsTitle').textContent,'Player & receiver preview');
assert.equal(search.value,'');
filter('lighting');assert.equal(navButton('effects').hidden,false);assert.equal(navButton('receivers').hidden,false);
nav.listeners.keydown(key('End'));assert.equal(documentHost.activeElement,navButton('effects'));
nav.listeners.keydown(key('Home'));assert.equal(documentHost.activeElement,navButton('nebula'));
filter('nothing matches this');assert(nav.children.every(button=>button.hidden));assert.equal(empty.hidden,false);
search.listeners.keydown(key('Escape'));assert.equal(search.value,'');assert.equal(empty.hidden,true);
assert.equal(element('#settingsDlg').open,true);
filter('tile depth');assert.equal(navButton('paint').hidden,false);assert.equal(navButton('camera').hidden,true);
element('#settingsClose').click();assert.deepEqual(sidebar.children,originalOrder);

// Room-specific sections remain absent until applicable; an active section
// retains its navigation entry if context changes while its controls are open.
const trunk=sections.find(s=>s.dataset.settings==='trunk');assert.equal(trunk.hidden,true);
run("EditorLayout.openSettings('room')");assert.equal(navButton('trunk').hidden,true);
element('#settingsClose').click();trunk.hidden=false;
run("EditorLayout.openSettings('trunk')");assert.equal(element('#settingsContent').children[0],trunk);
trunk.hidden=true;filter('');assert.equal(navButton('trunk').hidden,false);
run("EditorLayout.openSettings('room')");assert.equal(navButton('trunk').hidden,true);
element('#settingsClose').click();

// Sidebar shortcuts and project/View controls operate their existing owners.
for(const shortcut of shortcuts) {
  shortcut.focus();shortcut.click();assert.equal(element('#settingsContent').children[0].dataset.settings,shortcut.dataset.openSettings);
  element('#settingsClose').click();assert.equal(documentHost.activeElement,shortcut);
}
let projectLoads=0;element('#projectLoad').onclick=()=>++projectLoads;element('#projectOpen').click();assert.equal(projectLoads,1);
const info=element('#showSceneInfo');info.checked=true;info.onchange();assert.equal(app.classList.contains('show-scene-info'),true);
info.checked=false;info.onchange();assert.equal(app.classList.contains('show-scene-info'),false);

// Native toolbar details allow one open panel, inputs remain usable, and
// outside dismissal never steals focus. Escape precedes global editor keys.
const [first,second]=popovers,open=p=>{p.open=true;p.listeners.toggle();};
open(first);open(second);assert.equal(first.open,false);assert.equal(second.open,true);
const action=new Element('', 'BUTTON'),link=new Element('', 'A'),select=new Element('', 'SELECT');
second.append(action);second.append(link);second.append(select);
second.listeners.click({target:select});assert.equal(second.open,true);
action.disabled=true;second.listeners.click({target:action});assert.equal(second.open,true);
action.disabled=false;action.dataset.keepOpen='';second.listeners.click({target:action});assert.equal(second.open,true);
delete action.dataset.keepOpen;second.listeners.click({target:action});assert.equal(second.open,false);
open(second);second.listeners.click({target:link});assert.equal(second.open,false);
open(first);first.querySelector('summary').focus();events.pointerdown({target:first.querySelector('summary')});assert.equal(first.open,true);
search.focus();events.pointerdown({target:search});assert.equal(first.open,false);assert.equal(documentHost.activeElement,search);
open(second);let stopped=false;events.keydown({...key('Escape'),stopImmediatePropagation(){stopped=true;}});
assert.equal(second.open,false);assert.equal(documentHost.activeElement,second.querySelector('summary'));assert(stopped);
assert.equal(eventOptions.keydown,true);assert.equal(second.listenerOptions.click,true);
// Modal handoff from a closing toolbar returns to the visible summary.
const settingsAction=shortcuts[0];second.append(settingsAction);open(second);settingsAction.focus();
second.listeners.click({target:settingsAction});settingsAction.click();
assert.equal(second.open,false);assert.equal(element('#settingsDlg').open,true);
element('#settingsClose').click();assert.equal(documentHost.activeElement,second.querySelector('summary'));
assert.deepEqual(sidebar.children,originalOrder);
console.log('Editor layout: live controls/drafts, searchable contextual settings, shortcuts, return focus, keyboard navigation and toolbar popover dismissal passed');
