"""Regenerate reviewed Voxel 3D material tables; tile IDs remain provenance only."""
from pathlib import Path
import argparse
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true', help='Fail on stale generated tables without writing them.')
args = parser.parse_args()

ROOT = Path(__file__).resolve().parents[1]
TABLES = (
    ("castle-materials.json", "Castle", "castle"),
    ("fillmore-forest-materials.json", "Forest", "forest"),
    ("fillmore-cave-materials.json", "Cave", "cave"),
    ("fillmore-cave-water-materials.json", "CaveWater", "cave_water"),
    ("fillmore-temple-materials.json", "Temple", "temple"),
    ("bloodpool-marsh-materials.json", "Marsh", "marsh"),
    ("kassandora-desert-materials.json", "Desert", "desert"),
    ("kassandora-tomb-materials.json", "Tomb", "tomb"),
    ("aitos-mountains-materials.json", "Mountains", "mountains"),
    ("aitos-caverns-materials.json", "Caverns", "caverns"),
    ("marahna-jungle-materials.json", "Jungle", "jungle"),
    ("marahna-temple-materials.json", "MarahnaTemple", "marahna_temple"),
    ("northwall-snow-materials.json", "Snow", "snow"),
    ("northwall-ice-materials.json", "Ice", "ice"),
    ("northwall-ice-water-materials.json", "IceWater", "ice_water"),
    ("death-heim-materials.json", "DeathHeim", "death_heim"),
)

for manifest, symbol, suffix in TABLES:
    records = json.loads((ROOT / "assets/environments" / manifest).read_text())["materials"]
    profiles = {}
    for record in records:
        rear_colors = tuple(record.get("rear_colors", [0, 0, 0, 0]))
        if len(rear_colors) != 4 or any(type(v) is not int or not 0 <= v <= 65535 for v in rear_colors):
            raise ValueError(f'{manifest}: invalid rear color mask for {record["key"]}')
        gate_colors = tuple(record.get("gate_colors", [0, 0, 0, 0]))
        if len(gate_colors) != 4 or any(type(v) is not int or not 0 <= v <= 65535 or v & 1 for v in gate_colors):
            raise ValueError(f'{manifest}: invalid gate color mask for {record["key"]}')
        face_colors = tuple(record.get("face_colors", [0, 0, 0, 0]))
        if len(face_colors) != 4 or any(type(v) is not int or not 0 <= v <= 65535 or v & 1 for v in face_colors):
            raise ValueError(f'{manifest}: invalid face color mask for {record["key"]}')
        if any(a & (b | c) or b & c for a, b, c in zip(rear_colors, gate_colors, face_colors)):
            raise ValueError(f'{manifest}: overlapping pixel roles for {record["key"]}')
        rear_fold = record.get("rear_fold_pixels", 0)
        if type(rear_fold) is not int or not 0 <= rear_fold <= 15:
            raise ValueError(f'{manifest}: invalid rear ground band for {record["key"]}')
        if record['flags'] & 0x0f000000:
            raise ValueError(f'{manifest}: reserved ground-band bits for {record["key"]}')
        key = (int(record["key"], 16), tuple(record["kinds"]), record["flags"], rear_colors, gate_colors, face_colors, rear_fold)
        rooms = profiles.get(key, 0)
        for room, terrain, metatile in record["origins"]:
            rooms |= 1 << room
        profiles[key] = rooms
    lines = [f"/* Generated reviewed artwork roles. See assets/environments/{manifest}. */",
             f"static const VoxelMaterialProfile k{symbol}Materials[] = {{"]
    for (key, kinds, flags, rear_colors, gate_colors, face_colors, rear_fold), rooms in sorted(profiles.items()):
        gate_text = ",{%s}" % ",".join(map(str, gate_colors))
        face_text = ",{%s}" % ",".join(map(str, face_colors))
        # Keep compact profiles: bits24..27 carry an explicitly reviewed band
        # height, separate from the lower material-role flags.
        flags |= rear_fold << 24
        lines.append("  {UINT64_C(0x%016x),0x%04x,{%s},%d,{%s}%s%s}," %
                     (key, rooms, ",".join(map(str, kinds)), flags, ",".join(map(str, rear_colors)), gate_text, face_text))
    lines.append("};")
    output = ROOT / f"src/action/action_voxel_{suffix}_materials.inc"
    text = "\n".join(lines) + "\n"
    if args.check:
        if not output.exists() or output.read_text() != text:
            raise SystemExit(f'{output}: stale; run tools/generate_voxel_materials.py')
    else:
        output.write_text(text)
    print(manifest, len(profiles), "semantic material profiles")

# Transparency is a separately reviewed allowlist. Keep its native IDs in the
# manifest for art review, never in executable selection logic.
manifest = 'voxel-transparency.json'
records = json.loads((ROOT / 'assets/environments' / manifest).read_text())['rules']
rules = {}
for record in records:
    if record['background'] != 1 or record['method'] != 'edge_connected':
        raise ValueError(f'{manifest}: unsupported cutout scope/method')
    group, room = record['group'], record['room']
    if any(type(v) is not int or not 0 <= v <= 255 for v in (group, room)):
        raise ValueError(f'{manifest}: invalid room identity')
    colors = record['colors']
    if len(colors) != 4 or any(type(v) is not int or not 1 <= v <= 15 for row in colors for v in row):
        raise ValueError(f'{manifest}: invalid opaque palette indices')
    masks = tuple(sum(1 << v for v in set(row)) for row in colors)
    if not any(masks) or not record['origins'] or not record['reason']:
        raise ValueError(f'{manifest}: missing cutout review evidence')
    key = (int(record['key'], 16), group, room)
    if key in rules:
        raise ValueError(f'{manifest}: duplicate semantic cutout {key}')
    rules[key] = masks
lines = [f'/* Generated reviewed matte cutouts. See assets/environments/{manifest}. */',
         'static const VoxelTransparencyRule kVoxelTransparencyRules[] = {']
for (key, group, room), masks in sorted(rules.items()):
    lines.append('  {UINT64_C(0x%016x),%d,%d,{%s}},' %
                 (key, group, room, ','.join(map(str, masks))))
lines.append('};')
output = ROOT / 'src/action/action_voxel_transparency.inc'
text = '\n'.join(lines) + '\n'
if args.check:
    if not output.exists() or output.read_text() != text:
        raise SystemExit(f'{output}: stale; run tools/generate_voxel_materials.py')
else:
    output.write_text(text)
print(manifest, len(rules), 'semantic transparency rules')
