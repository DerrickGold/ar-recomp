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

TOP_POLICIES = {'default': (0, 0), 'own16': (1, 0), 'own16_donor': (3, 0),
                'masonry_cap16': (4, 0), 'forest_turf8': (8, 1),
                'forest_turf_right8': (8, 2), 'forest_turf_left8': (8, 3),
                'forest_turf8_donor': (16, 1), 'forest_turf_right8_donor': (16, 2),
                'forest_turf_left8_donor': (16, 3), 'forest_soil8': (32, 0),
                'family_cap16': (64, 0), 'family_donor16': (128, 0),
                'own_top_band5': (256, 0), 'own_top_band4': (512, 0)}
# Values are shared with the compiler's reviewed donor index; zero denotes
# ordinary masonry and1..3 are the established forest turf band families.
# Explicit names prevent an arbitrary material/palette ID becoming a family.
SURFACE_FAMILIES = {'mountain_rock': 4, 'ice_rock': 5, 'jungle_stone': 6,
                    'temple_masonry': 7}


def top_contract(manifest, record):
    name = record.get('top_surface', 'default')
    if name not in TOP_POLICIES:
        raise ValueError(f'{manifest}: invalid top surface policy for {record["key"]}')
    if name == 'own_top_band5' and (manifest != 'castle-materials.json' or
            record.get('kinds') != [1, 1, 1, 1] or record.get('flags') != 0 or
            any(record.get(k, [0, 0, 0, 0]) != [0, 0, 0, 0]
                for k in ('rear_colors', 'gate_colors', 'face_colors'))):
        raise ValueError(f'{manifest}: native top band requires reviewed plain solid artwork for {record["key"]}')
    if name == 'own_top_band4' and (manifest != 'bloodpool-marsh-materials.json' or
            record.get('role') != 'beam_deck' or record.get('kinds') != [1, 1, 4, 4] or
            record.get('flags') != 1048576 or any(record.get(k, [0, 0, 0, 0]) != [0, 0, 0, 0]
                for k in ('rear_colors', 'gate_colors', 'face_colors'))):
        raise ValueError(f'{manifest}: four-row native top band requires reviewed marsh beam artwork for {record["key"]}')
    policy, family = TOP_POLICIES[name]
    if name in ('family_cap16', 'family_donor16'):
        if record.get('surface_family') not in SURFACE_FAMILIES:
            raise ValueError(f'{manifest}: missing or unknown surface family for {record["key"]}')
        family = SURFACE_FAMILIES[record['surface_family']]
    elif 'surface_family' in record:
        raise ValueError(f'{manifest}: surface family requires a family policy for {record["key"]}')
    return policy, family

NATIVE_SURFACE_FAMILIES = {'desert_rock': 1, 'cavern_rock': 2,
                           'temple_column': 3, 'temple_ledge': 4, 'ice_crystal': 5, 'marsh_rock': 6, 'cavern_head_crown': 7, 'desert_sand_live': 8,
                           'osseous_stone': 9, 'lava_stone_cap': 11}
NATIVE_SURFACE_PROFILES = {'desert': 'KassandoraDesert', 'caverns': 'AitosCaverns',
                           'marahna_temple': 'MarahnaTemple', 'death_heim': 'DeathHeim',
                           'ice': 'NorthwallIce', 'marsh': 'BloodpoolMarsh'}
native_surfaces = {}
ice_tip_sources = {}
for manifest, symbol, suffix in TABLES:
    document = json.loads((ROOT / "assets/environments" / manifest).read_text())
    records = document["materials"]
    profiles = {}
    top_profiles = {}
    contour_profiles = {}
    contour_surface_profiles = {}
    rear_ink_profiles = {}
    rear_donor_profiles = {}
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
        complete_matte = record.get('complete_rear_matte', False)
        if type(complete_matte) is not bool or (complete_matte and not record['flags'] & 8388608):
            raise ValueError(f'{manifest}: complete rear matte requires opaque rear backing for {record["key"]}')
        portal_jamb = record.get('portal_jamb', False)
        if type(portal_jamb) is not bool or (portal_jamb and
                any(kind not in (1, 2, 4) for kind in record['kinds'])):
            raise ValueError(f'{manifest}: invalid portal jamb for {record["key"]}')
        door_closure = record.get('door_closure', False)
        if type(door_closure) is not bool or (door_closure and record['kinds'] != [1, 1, 1, 1]):
            raise ValueError(f'{manifest}: invalid solid door closure for {record["key"]}')
        key = (int(record["key"], 16), tuple(record["kinds"]), record["flags"], rear_colors,
               gate_colors, face_colors, rear_fold, complete_matte, portal_jamb, door_closure)
        record_rooms = 0
        for room, terrain, metatile in record["origins"]:
            record_rooms |= 1 << room
        contour_surface = record.get('contour_surface', 'default')
        contour_surface_modes = {'default': 0, 'stone_top16': 1, 'stone_bottom16': 2,
                                 'native_snow_crest16': 3, 'native_snow_body': 4}
        if contour_surface not in contour_surface_modes:
            raise ValueError(f'{manifest}: invalid contour surface for {record["key"]}')
        if contour_surface != 'default':
            snow = contour_surface in ('native_snow_crest16', 'native_snow_body')
            if snow:
                expected_room = 8 if suffix == 'snow' else 7
                if (suffix not in ('snow', 'death_heim') or
                    record_rooms != 1 << expected_room or
                    record['kinds'] != [1, 1, 1, 1] or
                    record['flags'] not in (20484, 20486) or
                    any(rear_colors + gate_colors + face_colors)):
                    raise ValueError(f'{manifest}: snow crest requires reviewed summit source for {record["key"]}')
            elif (suffix != 'tomb' or record['kinds'] != [1, 1, 1, 1] or
                  record['flags'] != 4 or rear_colors != (32768,) * 4 or
                  any(gate_colors + face_colors)):
                raise ValueError(f'{manifest}: contour skin requires reviewed diagonal stone for {record["key"]}')
            skin_key = (int(record['key'], 16), contour_surface_modes[contour_surface])
            contour_surface_profiles[skin_key] = contour_surface_profiles.get(skin_key, 0) | record_rooms
        rear_donor = record.get('rear_backing_donor', False)
        if type(rear_donor) is not bool:
            raise ValueError(f'{manifest}: invalid rear backing donor for {record["key"]}')
        if rear_donor:
            if (suffix != 'jungle' or record['kinds'] != [4, 4, 4, 4] or
                record['flags'] != 32776 or any(rear_colors + gate_colors + face_colors)):
                raise ValueError(f'{manifest}: rear donor requires reviewed plain masonry for {record["key"]}')
            donor_key = int(record['key'], 16)
            rear_donor_profiles[donor_key] = rear_donor_profiles.get(donor_key, 0) | record_rooms
        native_slope = record.get('native_slope', False)
        native_foot = record.get('native_slope_foot', False)
        native_support = record.get('native_slope_support', False)
        if (any(type(value) is not bool for value in (native_slope, native_foot, native_support)) or
            sum((native_slope, native_foot, native_support)) > 1):
            raise ValueError(f'{manifest}: invalid native slope contract for {record["key"]}')
        if native_slope or native_foot or native_support:
            expected_kind = 1 if native_slope else (4 if suffix == 'snow' else 7)
            if (suffix not in ('mountains', 'snow') or
                (native_support and suffix != 'mountains') or
                any(kind != expected_kind for kind in record['kinds']) or
                (native_foot and suffix == 'snow' and not record['flags'] & 32768)):
                raise ValueError(f'{manifest}: native slope requires reviewed terrain for {record["key"]}')
            contour_key = (int(record['key'], 16), 1 if native_slope else 2 if native_foot else 3)
            contour_profiles[contour_key] = contour_profiles.get(contour_key, 0) | record_rooms
        if 'opaque_rear_ink' in record:
            ink = record['opaque_rear_ink']
            if (suffix != 'mountains' or not record['flags'] & 8388608 or
                not isinstance(ink, dict) or set(ink) != {'palette', 'index'} or
                type(ink['palette']) is not int or not 0 <= ink['palette'] <= 7 or
                type(ink['index']) is not int or not 1 <= ink['index'] <= 15):
                raise ValueError(f'{manifest}: invalid native rear ink for {record["key"]}')
            ink_key = (int(record['key'], 16), ink['palette'] * 16 + ink['index'])
            rear_ink_profiles[ink_key] = rear_ink_profiles.get(ink_key, 0) | record_rooms
        rooms = profiles.get(key, 0) | record_rooms
        profiles[key] = rooms
        contract = top_contract(manifest, record)
        if contract[0]:
            top_key = (int(record['key'], 16), *contract)
            top_profiles[top_key] = top_profiles.get(top_key, 0) | record_rooms
    # Closed Ice-only mixed-source masks. A zero row names a real native
    # stone/matte endpoint; it is not permission to sample or paint that ink.
    for record in document.get('ice_tip_sources', []):
        if suffix != 'ice': raise ValueError(f'{manifest}: ice tip source outside Ice')
        key = int(record['key'], 16)
        rows = record.get('ice_rows')
        if not isinstance(rows, list) or len(rows) != 16 or any(
                not isinstance(v, str) or len(v) != 4 or any(c not in '0123456789abcdef' for c in v) for v in rows):
            raise ValueError(f'{manifest}: invalid exact ice pixel mask')
        rooms = 0
        for room, terrain, metatile in record['origins']:
            if room != 4: raise ValueError(f'{manifest}: unreviewed ice tip room')
            rooms |= 1 << room
        if not rooms or not any(int(r['key'],16)==key for r in records):
            raise ValueError(f'{manifest}: ice tip source lacks reviewed material')
        if key in ice_tip_sources: raise ValueError(f'{manifest}: duplicate ice tip source')
        ice_tip_sources[key] = (rooms, tuple(int(v,16) for v in rows))
    # An inward surface is artwork-only and starts at an actual emitted edge.
    # An empty face list explicitly allows continuation, never a new receiver.
    for record in document.get('native_surfaces', []):
        if suffix not in NATIVE_SURFACE_PROFILES or record.get('family') not in NATIVE_SURFACE_FAMILIES:
            raise ValueError(f'{manifest}: unknown native surface family/profile')
        faces = record.get('faces')
        names = ('top', 'right', 'bottom', 'left')
        if not isinstance(faces, list) or len(set(faces)) != len(faces) or any(v not in names for v in faces):
            raise ValueError(f'{manifest}: invalid native surface faces')
        limit = record.get('max_side_length', 0)
        if type(limit) is not int or not 0 <= limit <= 16:
            raise ValueError(f'{manifest}: invalid native side span limit')
        rooms = 0
        for room, terrain, metatile in record['origins']:
            if not 0 <= room < 16: raise ValueError(f'{manifest}: invalid native surface room')
            rooms |= 1 << room
        key = int(record['key'], 16)
        if not rooms or not any(int(r['key'],16)==key for r in records):
            raise ValueError(f'{manifest}: native surface lacks reviewed material')
        identity = (NATIVE_SURFACE_PROFILES[suffix], key,
                    sum(1 << names.index(v) for v in faces),
                    NATIVE_SURFACE_FAMILIES[record['family']], limit)
        for old, old_rooms in native_surfaces.items():
            if old[:2] == identity[:2] and old[2:] != identity[2:] and old_rooms & rooms:
                raise ValueError(f'{manifest}: conflicting native surface contract')
        native_surfaces[identity] = native_surfaces.get(identity, 0) | rooms
    # A face-art contract may review an otherwise unchanged native fallback
    # material. Keep it separate so artwork cannot alter structural roles.
    for record in document.get('top_surfaces', []):
        contract = top_contract(manifest, record)
        if not contract[0]:
            raise ValueError(f'{manifest}: invalid standalone top policy for {record["key"]}')
        rooms = 0
        for room, terrain, metatile in record['origins']:
            rooms |= 1 << room
        top_key = (int(record['key'], 16), *contract)
        top_profiles[top_key] = top_profiles.get(top_key, 0) | rooms
    for (key, policy, family), rooms in top_profiles.items():
        for (other_key, other_policy, other_family), other_rooms in top_profiles.items():
            if key == other_key and (policy, family) != (other_policy, other_family) and rooms & other_rooms:
                raise ValueError(f'{manifest}: conflicting top surface policies for {key:016x}')
    lines = [f"/* Generated reviewed artwork roles. See assets/environments/{manifest}. */",
             f"static const VoxelMaterialProfile k{symbol}Materials[] = {{"]
    for (key, kinds, flags, rear_colors, gate_colors, face_colors, rear_fold, complete_matte, portal_jamb, door_closure), rooms in sorted(profiles.items()):
        gate_text = ",{%s}" % ",".join(map(str, gate_colors))
        face_text = ",{%s}" % ",".join(map(str, face_colors))
        # Keep compact profiles: bits24..27 carry an explicitly reviewed band
        # height, separate from the lower material-role flags.
        flags |= rear_fold << 24
        matte_text = ",.complete_rear_matte=true" if complete_matte else ""
        if portal_jamb:
            matte_text += ",.portal_jamb=true"
        if door_closure:
            matte_text += ",.door_closure=true"
        lines.append("  {UINT64_C(0x%016x),0x%04x,{%s},%d,{%s}%s%s%s}," %
                     (key, rooms, ",".join(map(str, kinds)), flags, ",".join(map(str, rear_colors)),
                      gate_text, face_text, matte_text))
    lines.append("};")
    if rear_donor_profiles:
        lines.append(f"static const VoxelRearDonorProfile k{symbol}RearDonors[] = {{")
        for key, rooms in sorted(rear_donor_profiles.items()):
            lines.append("  {UINT64_C(0x%016x),0x%04x}," % (key, rooms))
        lines.append("};")
    if contour_profiles:
        lines.append(f"static const VoxelContourProfile k{symbol}Contours[] = {{")
        for (key, contour), rooms in sorted(contour_profiles.items()):
            lines.append("  {UINT64_C(0x%016x),0x%04x,%u}," % (key, rooms, contour))
        lines.append("};")
    if rear_ink_profiles:
        lines.append(f"static const VoxelRearInkProfile k{symbol}RearInks[] = {{")
        for (key, ink), rooms in sorted(rear_ink_profiles.items()):
            lines.append("  {UINT64_C(0x%016x),0x%04x,%u}," % (key, rooms, ink))
        lines.append("};")
    if contour_surface_profiles:
        lines.append(f"static const VoxelContourSurfaceProfile k{symbol}ContourSurfaces[] = {{")
        for (key, orientation), rooms in sorted(contour_surface_profiles.items()):
            lines.append("  {UINT64_C(0x%016x),0x%04x,%u}," % (key, rooms, orientation))
        lines.append("};")
    if top_profiles:
        lines.append(f"static const VoxelTopSurfaceProfile k{symbol}TopSurfaces[] = {{")
        for (key, policy, family), rooms in sorted(top_profiles.items()):
            lines.append("  {UINT64_C(0x%016x),0x%04x,%d,%d}," % (key, rooms, policy, family))
        lines.append("};")
    output = ROOT / f"src/action/action_voxel_{suffix}_materials.inc"
    text = "\n".join(lines) + "\n"
    if args.check:
        if not output.exists() or output.read_text() != text:
            raise SystemExit(f'{output}: stale; run tools/generate_voxel_materials.py')
    else:
        output.write_text(text)
    print(manifest, len(profiles), "semantic material profiles")

# Keep the new optional policy separate: unrelated material tables stay exact.
lines = ['/* Generated reviewed native inward surfaces; IDs are provenance only. */',
         'static const VoxelNativeSurfaceProfile kVoxelNativeSurfaces[] = {']
for (profile, key, faces, family, limit), rooms in sorted(native_surfaces.items()):
    lines.append('  {UINT64_C(0x%016x),0x%04x,kActionVoxelProfile_%s,%u,%u,%u},' %
                 (key, rooms, profile, faces, family, limit))
lines.append('};')
output = ROOT / 'src/action/action_voxel_native_surfaces.inc'
text = '\n'.join(lines) + '\n'
if args.check:
    if not output.exists() or output.read_text() != text:
        raise SystemExit(f'{output}: stale; run tools/generate_voxel_materials.py')
else:
    output.write_text(text)

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

# Exact mixed Ice continuation sources; no structural flags or receiver faces.
lines = ['/* Generated native Ice/stone ownership; IDs are provenance only. */',
         'static const VoxelIceTipSourceProfile kVoxelIceTipSources[] = {']
for key, (rooms, rows) in sorted(ice_tip_sources.items()):
    lines.append('  {UINT64_C(0x%016x),0x%04x,{%s}},' %
                 (key, rooms, ','.join('0x%04x' % r for r in rows)))
lines.append('};')
output = ROOT / 'src/action/action_voxel_ice_tip_sources.inc'
text = '\n'.join(lines) + '\n'
if args.check:
    if not output.exists() or output.read_text() != text:
        raise SystemExit(f'{output}: stale; run tools/generate_voxel_materials.py')
else:
    output.write_text(text)
