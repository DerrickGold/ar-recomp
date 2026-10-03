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
fs.mkdirSync(output, {recursive: true});
for (const base of data.rooms) {
  const terrain = base.terrainVariants.find(t => t.profile === profile);
  if (!terrain) throw Error(`Missing terrain profile ${profile} for ${base.group}:${base.map}`);
  const room = {...base, bg: [terrain.bg1, base.bg[1]], terrainProfile: profile};
  const bytes = encode(data, blobs, room);
  const file = [base.group, base.map].map(n => n.toString(16).padStart(2, '0')).join('') + '.arscene';
  fs.writeFileSync(path.join(output, file), bytes);
  manifest.rooms.push({file, bytes: bytes.length, sha256: hash(bytes)});
}
fs.writeFileSync(path.join(output, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
console.log(`Exported ${manifest.rooms.length} rooms, terrain profile ${profile}, to ${output}`);
