/* Copy one complete room section for quick game testing. The dialog is a
 * snapshot: opening or closing it never claims that anything was exported. */
const exportDialog=$('#exportDlg'),exportText=$('#exportText');
let roomExportSnapshot=null,copyingExport=false;
function selectExportText() {
  exportText.focus();exportText.select();
}
function exportedRoomCopied(snapshot) {
  captureRoomSavepoint(snapshot.key,snapshot.canonical);
  if(roomExportSnapshot===snapshot)
    $('#exportStatus').textContent='Copied. Replace this section in diorama-layers.ini, save it, and restart the game.';
}
function openRoomExport() {
  closeTileMenu();
  $('#exportTitle').textContent=`Export ${roomSectionHeader(room)}`;
  $('#exportInstructions').textContent=`Replace the existing ${roomSectionHeader(room)} section with this block, up to the next [section] header. If repeated, replace all copies with one block; if missing, append it. Keep camera-specific sections.`;
  $('#exportStatus').textContent='';
  $('#exportCopy').disabled=$('#exportSelect').disabled=false;
  try {
    const text=roomSectionIni(room);
    roomExportSnapshot={key:roomKey(room),canonical:roomIniLines(room).join('\n'),text};
    exportText.value=text;
  } catch(error) {
    roomExportSnapshot=null;exportText.value='';
    $('#exportCopy').disabled=$('#exportSelect').disabled=true;
    $('#exportStatus').textContent=`Cannot export this level: ${error.message}`;
  }
  exportDialog.showModal();
  if(roomExportSnapshot)selectExportText();
}
async function copyRoomExport() {
  const snapshot=roomExportSnapshot;
  if(!snapshot)return;
  try {
    await navigator.clipboard.writeText(snapshot.text);
    exportedRoomCopied(snapshot);
  } catch {
    /* Some file:// browsers disable the async clipboard. Keep a selectable
     * native text field and a keyboard-copy path when both APIs are blocked. */
    if(roomExportSnapshot!==snapshot)return;
    selectExportText();
    try {
      copyingExport=true;
      if(document.execCommand('copy')) {exportedRoomCopied(snapshot);return;}
    } catch { /* The selected text remains available for keyboard copying. */ }
    finally {copyingExport=false;}
    $('#exportStatus').textContent='Clipboard access is blocked. The text is selected; press Ctrl/Cmd-C to copy it.';
  }
}
function downloadFullIni() {
  try {
    const text=mergeDioramaIni();
    const url=URL.createObjectURL(new Blob([text],{type:'text/plain;charset=utf-8'}));
    const a=document.createElement('a');
    a.href=url;a.download=sourceIniName||'diorama-layers.ini';
    document.body.appendChild(a);a.click();a.remove();
    setTimeout(()=>URL.revokeObjectURL(url),1000);
    captureEditorSavepoint('exported');
    $('#exportStatus').textContent='Full INI download started, including edits in every room.';
  } catch(error) {
    $('#exportStatus').textContent=`Cannot download INI: ${error.message}`;
  }
}
/* Native keyboard Copy only exports the room if the entire block is selected. */
exportText.addEventListener('copy',event=>{
  if(!copyingExport&&!event.defaultPrevented&&roomExportSnapshot&&
      exportText.selectionStart===0&&exportText.selectionEnd===exportText.value.length)
    exportedRoomCopied(roomExportSnapshot);
});
$('#export').onclick=openRoomExport;
$('#exportCopy').onclick=copyRoomExport;
$('#exportSelect').onclick=selectExportText;
$('#exportDownload').onclick=downloadFullIni;
$('#exportClose').onclick=()=>exportDialog.close();
