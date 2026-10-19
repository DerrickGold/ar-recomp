#!/usr/bin/env node
/* Export the editor's complete room assets for check_native_projection.c.
 * Assets remain local; the manifest pins their provenance without shipping ROM art. */
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';

const [htmlFile, output, profileText = '0'] = process.argv.slice(2);
const profile = Number(profileText);
if (!htmlFile || !output || !Number.isInteger(profile) || profile < 0 || profile > 2) {
  console.error('usage: node export_gpu_rooms.mjs editor.html output-directory [terrain-profile: 0|1|2]');
  process.exit(2);
}
const html = fs.readFileSync(htmlFile, 'utf8');
const data = JSON.parse(html.match(/window\.__ACTION_BG__=(.*?);<\/script>/s)[1]);
const context = vm.createContext({window: {}, Uint8Array, Uint32Array, DataView});
const encoderPath = path.join(path.dirname(fileURLToPath(import.meta.url)), 'shared_preview.js');
vm.runInContext(fs.readFileSync(encoderPath, 'utf8'), context);
const encode = vm.runInContext('SharedActionPreview.encode', context);
const blobs = data.blobs.map(s => Uint8Array.from(Buffer.from(s, 'base64')));
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const manifest = {editor: path.resolve(htmlFile), editor_sha256: hash(html), profile, rooms: []};
const roomKey = room => [room.group, room.map].map(n => n.toString(16).padStart(2, '0')).join('');
const totals = new Map(), seen = new Map();
for (const room of data.rooms) totals.set(roomKey(room), (totals.get(roomKey(room)) || 0) + 1);
fs.mkdirSync(output, {recursive: true});
if (data.player) {
  const art=Buffer.concat([data.player.characters,data.player.palette,data.player.picture].map(i=>Buffer.from(blobs[i])));
  fs.writeFileSync(path.join(output,'player.bin'),art);
  manifest.player={file:'player.bin',bytes:art.length,sha256:hash(art)};
}
for (const base of data.rooms) {
  const terrain = base.terrainVariants.find(t => t.profile === profile);
  if (!terrain) throw Error(`Missing terrain profile ${profile} for ${base.group}:${base.map}`);
  const room = {...base, bg: [terrain.bg1, base.bg[1]], terrainProfile: profile};
  const bytes = encode(data, blobs, room);
  // The DeathHeim hub has two catalogue states with the same room ID. Keep
  // both instead of silently overwriting one; retain the historical canonical
  // filename for the last state used by named-room cross references.
  const key = roomKey(base), index = seen.get(key) || 0;
  seen.set(key, index + 1);
  const file = key + (index + 1 < totals.get(key) ? `-variant${index}` : '') + '.arscene';
  fs.writeFileSync(path.join(output, file), bytes);
  manifest.rooms.push({file, bytes: bytes.length, sha256: hash(bytes)});
}
fs.writeFileSync(path.join(output, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
console.log(`Exported ${manifest.rooms.length} rooms, terrain profile ${profile}, to ${output}`);
