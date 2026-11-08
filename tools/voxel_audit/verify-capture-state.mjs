#!/usr/bin/env node
/* Prove the offline editor's immutable source assets and INI correspond to the
 * exported production inventory. This is provenance, not visual acceptance. */
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
import {fileURLToPath} from 'node:url';
const [htmlFile, exportFile, output] = process.argv.slice(2);
if (!htmlFile || !exportFile || !output) throw Error('usage: verify-capture-state.mjs editor.html export-receipt.json output.json');
const html = fs.readFileSync(htmlFile, 'utf8'), exported = JSON.parse(fs.readFileSync(exportFile));
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const embedded = name => JSON.parse(html.match(new RegExp('window\\.' + name + '=(.*?);(?:window\\.|<\\/script>)', 's'))[1]);
const data = embedded('__ACTION_BG__'), layers = embedded('__DIORAMA_LAYERS__'), effects = embedded('__ACTION_EFFECTS__');
const encoderFile = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../action_editor/shared_preview.js');
const encoder = fs.readFileSync(encoderFile, 'utf8');
assert(html.includes('<script>' + encoder + '</script>'), 'Encoder must be byte-identical to the source embedded in the captured editor');
const context = vm.createContext({window: {}, Uint8Array, Uint32Array, DataView});
vm.runInContext(encoder, context);
const encode = vm.runInContext('SharedActionPreview.encode', context);
const blobs = data.blobs.map(s => Uint8Array.from(Buffer.from(s, 'base64')));
const key = room => [room.group,room.map].map(n=>n.toString(16).padStart(2,'0')).join('');
const totals = new Map();
for (const room of data.rooms) totals.set(key(room),(totals.get(key(room))||0)+1);
const records=[];
for (let profile=0;profile<3;++profile) {
 const seen=new Map();
 for (const base of data.rooms) {
  const terrain=base.terrainVariants.find(t=>t.profile===profile);assert(terrain,'All terrain sources must exist');
  const room={...base,bg:[terrain.bg1,base.bg[1]],terrainProfile:profile};
  const encoded=Buffer.from(encode(data,blobs,room));
  const roomKey=key(base),index=seen.get(roomKey)||0;seen.set(roomKey,index+1);
  const scene=roomKey+(index+1<totals.get(roomKey)?`-variant${index}`:'')+'.arscene';
  const expected=exported.records.find(r=>r.mode==='shipped'&&r.terrain===profile&&r.scene===scene);
  assert(expected,'Shipped export must include '+profile+'/'+scene);
  records.push({room:[base.group,base.map].map(n=>n.toString(16).padStart(2,'0')).join('-')+(base.section?'-'+base.section:''),group:base.group,map:base.map,section:base.section||'room',terrain:profile,scene,editor_source_sha256:hash(encoded),inventory_source_sha256:expected.source_sha256,match:hash(encoded)===expected.source_sha256,bytes:encoded.length,inventory:expected.output});
 }
}
const report={schema:1,editor:path.resolve(htmlFile),editor_sha256:hash(html),export_receipt:path.resolve(exportFile),export_receipt_sha256:hash(fs.readFileSync(exportFile)),embedded_layers_sha256:hash(layers),inventory_ini_sha256:exported.ini_sha256,embedded_layers_match:hash(layers)===exported.ini_sha256,embedded_effects_sha256:hash(effects),encoder_sha256:hash(encoder),encoder_embedded_exact:true,records,source_matches:records.filter(r=>r.match).length,source_total:records.length,character_phase:0,frame_mapping:'Encoded sources contain immutable phase-zero character bytes; capture nativeFrame0 matches. Stored golden request frame37 is not applied to the source bytes.',limits:['Embedded file equality proves immutable inputs; capture Project unchanged checks and project INI hashes separately establish no authored edits.', 'This does not establish visual correctness, GPU visibility, actor gameplay or hardware performance.'],passed:records.length===150&&records.every(r=>r.match)&&hash(layers)===exported.ini_sha256};
fs.mkdirSync(path.dirname(output),{recursive:true});fs.writeFileSync(output,JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify({passed:report.passed,source_matches:report.source_matches,source_total:report.source_total,embedded_layers_match:report.embedded_layers_match}));
if(!report.passed)process.exitCode=1;
