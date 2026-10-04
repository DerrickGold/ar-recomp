/* ZIP interoperability is checked against Python's standard implementation,
 * including deflated input, rather than only against our own ZIP reader. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import {spawnSync} from 'node:child_process';

const context=vm.createContext({TextEncoder,TextDecoder,Blob,DecompressionStream});
vm.runInContext(fs.readFileSync(new URL('../tools/action_editor/project_archive.js',import.meta.url),'utf8')+
  '\nglobalThis.archive=ActionProjectArchive;',context);
const archive=context.archive;
const files={'diorama-layers.ini':'# 森 </script>\r\n[layers:01:01]\r\nbg1 = z:0.2\r\n',
  'action-effects.ini':'[effects]\nversion=1\n; fog and light\n'};
const encoded=archive.encode(files);
const readPython=spawnSync(process.env.PYTHON||'python3',['-c',
  'import io,json,sys,zipfile\nz=zipfile.ZipFile(io.BytesIO(sys.stdin.buffer.read()))\nassert z.testzip() is None\nprint(json.dumps({n:z.read(n).decode("utf-8") for n in z.namelist()}))'],
  {input:Buffer.from(encoded)});
assert.equal(readPython.status,0,readPython.stderr.toString());
assert.deepEqual(JSON.parse(readPython.stdout),files);
assert.deepEqual({...await archive.decode(encoded)},files);

function pythonZip(mode,entries=Object.entries(files)) {
  const result=spawnSync(process.env.PYTHON||'python3',['-c',
    'import io,json,sys,zipfile,warnings\nwarnings.simplefilter("ignore")\nb=io.BytesIO()\n'+
    'with zipfile.ZipFile(b,"w",compression='+mode+') as z:\n'+
    ' for name,text in json.load(sys.stdin): z.writestr(name,text)\n'+
    ' z.comment=b"Portable project"\nsys.stdout.buffer.write(b.getvalue())'],
    {input:JSON.stringify(entries)});
  assert.equal(result.status,0,result.stderr.toString());return new Uint8Array(result.stdout);
}
assert.deepEqual({...await archive.decode(pythonZip('zipfile.ZIP_DEFLATED'))},files);
assert.deepEqual({...await archive.decode(pythonZip('zipfile.ZIP_STORED'))},files);
const corrupt=new Uint8Array(encoded);corrupt[30+'diorama-layers.ini'.length]^=1;
await assert.rejects(archive.decode(corrupt),/Corrupt ZIP data/);
await assert.rejects(archive.decode(encoded.subarray(0,encoded.length-1)),/Invalid or incomplete/);
await assert.rejects(archive.decode(pythonZip('zipfile.ZIP_STORED',[Object.entries(files)[0]])),/must contain action-effects.ini/);
await assert.rejects(archive.decode(pythonZip('zipfile.ZIP_STORED',[
  ...Object.entries(files),['diorama-layers.ini','duplicate']])),/duplicate diorama-layers.ini/);
await assert.rejects(archive.decode(pythonZip('zipfile.ZIP_STORED',Object.entries(files)
  .map(([name,text])=>['folder/'+name,text]))),/at its root/);
const oversized=new Uint8Array(encoded),view=new DataView(oversized.buffer);
const directory=view.getUint32(oversized.length-6,true);
view.setUint32(directory+24,archive.limits['diorama-layers.ini']+1,true);
await assert.rejects(archive.decode(oversized),/size limit/);
const encrypted=new Uint8Array(encoded),encryptedView=new DataView(encrypted.buffer);
encryptedView.setUint16(directory+8,1,true);
await assert.rejects(archive.decode(encrypted),/Unsupported ZIP encoding/);
assert.throws(()=>archive.encode({'diorama-layers.ini':''}),/missing action-effects.ini/);
console.log('Project ZIPs: Python extraction, stored/deflated imports, Unicode and corrupt/missing/duplicate/oversized entry rejection passed');
