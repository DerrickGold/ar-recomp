// Reuse the editor's scene encoder and bundled level assets; no gameplay capture
// is needed. Generated .arscene files contain game artwork and stay in runs/.
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';

const [bundle, output, ...requested] = process.argv.slice(2);
if (!bundle || !output) {
  console.error('usage: node tools/ppu_gpu/export_fixtures.mjs editor.html output-dir [0101 0102 ...]');
  process.exit(2);
}
const payload = fs.readFileSync(bundle, 'utf8').match(/window\.__ACTION_BG__=(.*?);<\/script>/s);
if (!payload) throw new Error('Editor bundle has no action room data');
const data = JSON.parse(payload[1]);
const context = vm.createContext({window: {}, Uint8Array, Uint32Array, DataView});
const encoder = fileURLToPath(new URL('../action_editor/shared_preview.js', import.meta.url));
vm.runInContext(fs.readFileSync(encoder, 'utf8'), context);
const encode = vm.runInContext('SharedActionPreview.encode', context);
const blobs = data.blobs.map(blob => Uint8Array.from(Buffer.from(blob, 'base64')));
fs.mkdirSync(output, {recursive: true});
for (const id of requested.length ? requested : ['0101', '0102', '0201', '0202', '0404']) {
  if (!/^[0-9a-f]{4}$/i.test(id)) throw new Error(`Invalid room ID: ${id}`);
  const group = parseInt(id.slice(0, 2), 16), map = parseInt(id.slice(2), 16);
  const base = data.rooms.find(room => room.group === group && room.map === map);
  const terrain = base?.terrainVariants.find(item => item.profile === 0);
  if (!terrain) throw new Error(`Missing room/terrain: ${id}`);
  const room = {...base, bg: [terrain.bg1, base.bg[1]], terrainProfile: 0};
  const bytes = encode(data, blobs, room);
  fs.writeFileSync(path.join(output, `room-${id.toLowerCase()}.arscene`), bytes);
  console.log(`${id}: ${bytes.length} bytes`);
}
