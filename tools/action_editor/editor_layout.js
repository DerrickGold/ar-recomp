/* Sidebar and modal share the actual controls, including their handlers, draft
 * state and generated lists. Reparenting avoids a second authoring interface. */
const EditorLayout=(()=>{
  const app=$('#editorApp'),sidebar=$('#editorSidebar'),dialog=$('#settingsDlg');
  const content=$('#settingsContent'),nav=$('#settingsNav'),menu=$('#settingsMenu');
  const search=$('#settingsSearch'),empty=$('#settingsEmpty');
  menu.hidden=true;
  const sections=[...document.querySelectorAll('#editorSidebar > [data-settings]')]
    .map(node=>({node,id:node.dataset.settings,title:node.querySelector('summary').textContent,
      button:null,menuButton:null}));
  let active=null,placeholder=null,wasOpen=false,returnFocus=null,menuFocus=null;
  let sidebarHidden=false,toolsHidden=false;
  const aliases={
    room:'level act map region terrain background',policy:'background layers parallax fill bands water priority',
    pixels:'tile artwork transparent erase paint pixel',trunk:'northwall tree curve depth enclosure frost bark',
    palette:'color colour saturation shadows highlights grade artwork',nebula:'death heim glow lighting stars',
    framing:'camera crop coverage margins bounds viewport position',camera:'view orbit yaw pitch animation time',
    receivers:'sprites objects actors player preview lighting',effects:'environment lighting lights particles fog mist water effects',
    files:'project import export save load ini',paint:'tile tiles material depth plane paint floor side surface cap',
    selection:'select tile tiles rectangle region',scenery:'background tile border edge space extension',
    reset:'reset clear classifications tile depth',tally:'tile count classification inventory',
    other:'layer alignment background position offset',ladder:'layer depth planes order priority notes'
  };
  function refreshNavigation() {
    const terms=(search?.value||'').trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
    let matches=0;
    for(const section of sections) {
      const available=!section.node.hidden||section===active;
      const words=`${section.title} ${aliases[section.id]||''}`.toLocaleLowerCase();
      section.button.hidden=!available||!terms.every(term=>words.includes(term));
      section.menuButton.hidden=!available;
      if(!section.button.hidden)++matches;
    }
    if(empty)empty.hidden=matches!==0;
  }
  function navigateButtons(event,buttons) {
    if(!buttons.length)return;
    const index=buttons.indexOf(document.activeElement);let next;
    if(event.key==='ArrowDown')next=(index+1)%buttons.length;
    else if(event.key==='ArrowUp')next=index<0?buttons.length-1:(index+buttons.length-1)%buttons.length;
    else if(event.key==='Home')next=0;
    else if(event.key==='End')next=buttons.length-1;
    else return;
    event.preventDefault();event.stopPropagation();buttons[next].focus({preventScroll:true});
  }
  function toggleSidebar() {
    sidebarHidden=!sidebarHidden;sidebar.hidden=sidebarHidden;
    app.classList.toggle('sidebar-hidden',sidebarHidden);
    $('#sidebarToggle').textContent='Room';
    $('#sidebarToggle').setAttribute('aria-expanded',String(!sidebarHidden));
  }
  function toggleTools() {
    toolsHidden=!toolsHidden;app.classList.toggle('tools-hidden',toolsHidden);
    $('#toolbarToggle').textContent=toolsHidden?'Show editing toolbar':'Hide editing toolbar';
    $('#toolbarToggle').setAttribute('aria-expanded',String(!toolsHidden));
  }
  function restoreSection() {
    if(!active)return;
    placeholder.replaceWith(active.node);active.node.open=wasOpen;
    active=null;placeholder=null;
  }
  function selectSection(id) {
    const section=sections.find(item=>item.id===id&&(!item.node.hidden||item===active))||
      sections.find(item=>!item.node.hidden);
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
    refreshNavigation();
  }
  function closeMenu(focus=false) {
    menu.hidden=true;
    if(focus)menuFocus?.focus({preventScroll:true});
  }
  function openSettings(id='room',source=null) {
    const owner=source||(menu.hidden?document.activeElement:menuFocus);
    // A toolbar action closes its details before opening the dialog. Its
    // summary is the visible return target, even if the browser has already
    // moved focus off the now-hidden action button.
    const focus=owner?.closest('details.toolbar-popover')?.querySelector('summary')||owner;
    closeTileMenu();closeMenu();
    for(const popover of document.querySelectorAll('details.toolbar-popover'))popover.open=false;
    if(!dialog.open)returnFocus=focus;
    if(search)search.value='';
    selectSection(id);
    refreshNavigation();
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
    menuFocus=document.activeElement;refreshNavigation();menu.hidden=false;
    const rect=menu.getBoundingClientRect();
    menu.style.left=`${Math.max(8,Math.min(event.clientX,window.innerWidth-rect.width-8))}px`;
    menu.style.top=`${Math.max(8,Math.min(event.clientY,window.innerHeight-rect.height-8))}px`;
    const id=event.target.closest('[data-settings]')?.dataset.settings;
    (sections.find(section=>section.id===id&&!section.menuButton.hidden)||
      sections.find(section=>!section.menuButton.hidden))?.menuButton.focus();
  }
  for(const section of sections) {
    for(const [container,key] of [[nav,'button'],[menu,'menuButton']]) {
      const button=document.createElement('button');button.textContent=section.title;
      button.type='button';
      if(key==='button')button.setAttribute('aria-controls','settingsContent');
      if(key==='menuButton'){button.setAttribute('role','menuitem');button.tabIndex=-1;}
      button.onclick=()=>openSettings(section.id);container.append(button);section[key]=button;
    }
  }
  $('#sidebarToggle').onclick=toggleSidebar;
  $('#toolbarToggle').onclick=toggleTools;
  $('#editorSettingsOpen').onclick=()=>openSettings();
  $('#settingsClose').onclick=()=>finishSettings();
  for(const button of document.querySelectorAll('[data-open-settings]'))
    button.onclick=()=>openSettings(button.dataset.openSettings,button);
  const projectOpen=$('#projectOpen');
  if(projectOpen)projectOpen.onclick=()=>$('#projectLoad').click();
  const sceneInfo=$('#showSceneInfo');
  if(sceneInfo)sceneInfo.onchange=()=>app.classList.toggle('show-scene-info',sceneInfo.checked);
  if(search) {
    search.oninput=refreshNavigation;
    search.addEventListener('keydown',event=>{
      if(event.key==='Escape'&&search.value) {
        event.preventDefault();event.stopPropagation();search.value='';refreshNavigation();return;
      }
      if(event.key==='ArrowDown'||event.key==='ArrowUp')
        navigateButtons(event,sections.filter(section=>!section.button.hidden).map(section=>section.button));
    });
  }
  nav.addEventListener('keydown',event=>navigateButtons(event,
    sections.filter(section=>!section.button.hidden).map(section=>section.button)));
  refreshNavigation();
  dialog.addEventListener('cancel',event=>{event.preventDefault();finishSettings();});
  dialog.addEventListener('close',()=>{if(!dialog.open)restoreSection();});
  // Let tools return to the canvas, or open their dedicated inspector, with one
  // click. Ordinary setting edits keep the dialog open.
  const leaveSettings=new Set(['pixelPick','framingAdjust','coverageFit','bgPolicyPreview','palettePreview',
    'previewProbesHere','previewEventHere','effectInspectorOpen','emitterAdd','emitterMapEdit',
    'emitterMapPlace','particleAreaDraw','lightBoxDraw','floorMistDraw','floorMistErase','export',
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
    navigateButtons(event,sections.filter(section=>!section.menuButton.hidden).map(section=>section.menuButton));
  });
  // Voxel pointer handlers preventDefault to own the drag. Browsers then
  // suppress compatibility mousedown, so dismissal must start at pointerdown.
  // Do not consume the event: that same click should reach its destination.
  for(const eventName of ['pointerdown','mousedown'])
    window.addEventListener(eventName,event=>{if(!menu.contains(event.target))closeMenu();},true);
  window.addEventListener('wheel',event=>{if(!menu.contains(event.target))closeMenu();},true);
  window.addEventListener('resize',()=>closeMenu());
  window.addEventListener('blur',()=>closeMenu());
  return {openSettings,finishSettings,toggleSidebar,toggleTools,
    modalOpen:()=>dialog.open||!menu.hidden};
})();

/* Native details retain keyboard activation and their expanded semantics.
 * Dismissing a toolbar never moves focus away from the clicked destination. */
(()=>{
  const popovers=[...document.querySelectorAll('details.toolbar-popover')];
  function closeAll(except=null) {
    for(const popover of popovers)if(popover!==except)popover.open=false;
  }
  for(const popover of popovers) {
    popover.addEventListener('toggle',()=>{if(popover.open)closeAll(popover);});
    popover.addEventListener('click',event=>{
      if(event.target.closest('input,select,textarea,[data-keep-open]'))return;
      const action=event.target.closest('button,a');
      if(action&&!action.disabled)popover.open=false;
    },true);
  }
  window.addEventListener('pointerdown',event=>{
    for(const popover of popovers)if(popover.open&&!popover.contains(event.target))popover.open=false;
  },true);
  window.addEventListener('keydown',event=>{
    if(event.key!=='Escape')return;
    const popover=popovers.find(item=>item.open);
    if(!popover)return;
    event.preventDefault();event.stopImmediatePropagation();closeAll();
    popover.querySelector('summary')?.focus({preventScroll:true});
  },true);
})();
