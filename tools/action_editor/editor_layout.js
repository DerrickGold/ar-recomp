/* Sidebar and modal share the actual controls, including their handlers, draft
 * state and generated lists. Reparenting avoids a second authoring interface. */
const EditorLayout=(()=>{
  const app=$('#editorApp'),sidebar=$('#editorSidebar'),dialog=$('#settingsDlg');
  const content=$('#settingsContent'),nav=$('#settingsNav'),menu=$('#settingsMenu');
  menu.hidden=true;
  const sections=[...document.querySelectorAll('#editorSidebar > [data-settings]')]
    .map(node=>({node,id:node.dataset.settings,title:node.querySelector('summary').textContent,
      button:null,menuButton:null}));
  let active=null,placeholder=null,wasOpen=false,returnFocus=null,menuFocus=null;
  let sidebarHidden=false,toolsHidden=false;
  function toggleSidebar() {
    sidebarHidden=!sidebarHidden;sidebar.hidden=sidebarHidden;
    app.classList.toggle('sidebar-hidden',sidebarHidden);
    $('#sidebarToggle').textContent=sidebarHidden?'Show sidebar':'Hide sidebar';
    $('#sidebarToggle').setAttribute('aria-expanded',String(!sidebarHidden));
  }
  function toggleTools() {
    toolsHidden=!toolsHidden;app.classList.toggle('tools-hidden',toolsHidden);
    $('#toolbarToggle').textContent=toolsHidden?'Show tools':'Hide tools';
    $('#toolbarToggle').setAttribute('aria-expanded',String(!toolsHidden));
  }
  function restoreSection() {
    if(!active)return;
    placeholder.replaceWith(active.node);active.node.open=wasOpen;
    active=null;placeholder=null;
  }
  function selectSection(id) {
    const section=sections.find(item=>item.id===id)||sections[0];
    if(!section||section===active)return;
    restoreSection();active=section;wasOpen=section.node.open;
    placeholder=document.createElement('span');placeholder.hidden=true;
    section.node.before(placeholder);section.node.open=true;content.append(section.node);
    $('#settingsTitle').textContent=section.title;
    for(const item of sections) {
      item.button.classList.toggle('on',item===section);
      item.button.setAttribute('aria-current',String(item===section));
    }
    content.scrollTop=0;
  }
  function closeMenu(focus=false) {
    menu.hidden=true;
    if(focus)menuFocus?.focus({preventScroll:true});
  }
  function openSettings(id='room') {
    const focus=menu.hidden?document.activeElement:menuFocus;
    closeTileMenu();closeMenu();
    if(!dialog.open)returnFocus=focus;
    selectSection(id);
    if(!dialog.open)dialog.showModal();
    active?.button.focus({preventScroll:true});
  }
  function finishSettings(focus=true) {
    restoreSection();
    if(dialog.open)dialog.close();
    if(focus)returnFocus?.focus({preventScroll:true});
  }
  function openMenu(event) {
    if(dialog.open||event.target.closest('dialog')||event.target.closest('#tileMenu')||
      event.target.closest('#settingsMenu'))return;
    // The map retains its tile/effect actions and its Settings entry. All other
    // views, and the sidebar itself, offer the complete settings list directly.
    if(event.target===cvs&&mode==='2d')return;
    event.preventDefault();closeTileMenu();
    menuFocus=document.activeElement;menu.hidden=false;
    const rect=menu.getBoundingClientRect();
    menu.style.left=`${Math.max(8,Math.min(event.clientX,window.innerWidth-rect.width-8))}px`;
    menu.style.top=`${Math.max(8,Math.min(event.clientY,window.innerHeight-rect.height-8))}px`;
    const id=event.target.closest('[data-settings]')?.dataset.settings;
    (sections.find(section=>section.id===id)||sections[0])?.menuButton.focus();
  }
  for(const section of sections) {
    for(const [container,key] of [[nav,'button'],[menu,'menuButton']]) {
      const button=document.createElement('button');button.textContent=section.title;
      if(key==='menuButton'){button.setAttribute('role','menuitem');button.tabIndex=-1;}
      button.onclick=()=>openSettings(section.id);container.append(button);section[key]=button;
    }
  }
  $('#sidebarToggle').onclick=toggleSidebar;
  $('#toolbarToggle').onclick=toggleTools;
  $('#editorSettingsOpen').onclick=()=>openSettings();
  $('#settingsClose').onclick=()=>finishSettings();
  $('#collapseSidebarSections').onclick=()=>{
    const collapse=sections.some(section=>section.node.open);
    for(const section of sections)section.node.open=!collapse;
    $('#collapseSidebarSections').textContent=collapse?'Expand sections':'Collapse sections';
  };
  dialog.addEventListener('cancel',event=>{event.preventDefault();finishSettings();});
  dialog.addEventListener('close',()=>{if(!dialog.open)restoreSection();});
  // Let tools return to the canvas, or open their dedicated inspector, with one
  // click. Ordinary setting edits keep the dialog open.
  const leaveSettings=new Set(['pixelPick','framingAdjust','coverageFit','bgPolicyPreview',
    'previewProbesHere','previewEventHere','effectInspectorOpen','emitterAdd','emitterMapEdit',
    'emitterMapPlace','particleAreaDraw','floorMistDraw','floorMistErase','export',
    'effectDocumentOpen','bClass','bCell','bRect','bSelect','bPan','bSelectRect','stampTool']);
  content.addEventListener('click',event=>{
    const button=event.target.closest('button');
    if(button&&!button.disabled&&leaveSettings.has(button.id))finishSettings(false);
  },true);
  app.addEventListener('contextmenu',openMenu);
  for(const canvas of [cvs,$('#gl'),$('#sharedGl')])canvas.addEventListener('keydown',event=>{
    if(canvas===cvs&&mode==='2d')return;
    if(!(event.key==='ContextMenu'||event.key==='F10'&&event.shiftKey))return;
    event.preventDefault();event.stopPropagation();openSettings();
  });
  menu.addEventListener('keydown',event=>{
    if(event.key==='Escape'||event.key==='Tab') {
      if(event.key==='Escape'){event.preventDefault();event.stopPropagation();}
      closeMenu(true);return;
    }
    const buttons=sections.map(section=>section.menuButton),index=buttons.indexOf(document.activeElement);
    let next;
    if(event.key==='ArrowDown')next=(index+1)%buttons.length;
    else if(event.key==='ArrowUp')next=(index+buttons.length-1)%buttons.length;
    else if(event.key==='Home')next=0;
    else if(event.key==='End')next=buttons.length-1;
    else return;
    event.preventDefault();event.stopPropagation();buttons[next]?.focus();
  });
  window.addEventListener('mousedown',event=>{if(!menu.contains(event.target))closeMenu();},true);
  window.addEventListener('wheel',event=>{if(!menu.contains(event.target))closeMenu();},true);
  window.addEventListener('resize',()=>closeMenu());
  window.addEventListener('blur',()=>closeMenu());
  return {openSettings,finishSettings,toggleSidebar,toggleTools,
    modalOpen:()=>dialog.open||!menu.hidden};
})();
