"""Normalize production surface exports without excluding unknown treatments.

All source data stays private. This adapter inventories emitted geometry and
source silhouettes separately: neither proves the other is complete. Art pages
are immutable compiler donors, not a replacement for actual renderer captures.
"""
from collections import Counter, defaultdict
import gzip
import hashlib
import json
from pathlib import Path

SIDES = ('top', 'right', 'bottom', 'left')
SEMANTICS = ('none', 'rear', 'foliage', 'scenery', 'platform', 'water', 'hazard',
             'gate', 'column', 'branch', 'tree_wood', 'stump', 'authored')


def read_json(path):
    path = Path(path)
    with (gzip.open(path, 'rt') if path.suffix == '.gz' else path.open()) as source:
        return json.load(source)


def decode_art(value):
    return [int(value[i:i+4], 16) for i in range(0, len(value), 4)] if value else None


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def normalize_export(data, scope='unknown'):
    a = data['surface_audit']
    scene_id = f"{scope}/{data['terrain']}/{data['room']}/{data.get('section', 'room')}"
    cells = {(v[0], v[1]): dict(zip(a['cell_fields'], v)) for v in a['source_cells']}
    shapes = {(v['x'], v['y']): v for v in data.get('cells', [])}
    materials = a['materials']
    original = [decode_art(m['rgba555_hex']) for m in materials]
    owned = [decode_art(s) for s in a['owned_art']]
    surfaces, cache = [], {}
    def source(cx, cy):
        pos = cx, cy
        if pos not in cache:
            cell = cells.get(pos)
            if not cell:
                cache[pos] = None, None
            else:
                m = materials[cell['material_id']]
                art = list(original[cell['material_id']])
                edits = a['pixel_edits'][cell['edit_id']] if cell['edit_id'] >= 0 else None
                for y in range(16):
                    for x in range(16):
                        bit = 1 << (15-x)
                        # RearMatte cutout bits describe backing ownership,
                        # not front alpha. This is the exported mask contract.
                        if not (m.get('flags',0) & 2097152) and m['cutouts'][y] & bit:
                            art[y*16+x] = 0
                        if edits and edits['transparent'][y] & bit:
                            art[y*16+x] = 0
                        if edits and edits['black'][y] & bit:
                            art[y*16+x] = 0x8000
                ai = cell.get('authored_index',-1)
                if ai >= 0 and a['authored'][ai].get('surface'):
                    art = list(owned[a['authored'][ai]['art_ids'][0]])
                cache[pos] = m, art
        return cache[pos]
    def donor(x, y, width, height):
        if not width or not height:
            return None
        pixels = []
        for yy in range(y, y+height):
            for xx in range(x, x+width):
                _, art = source(xx//16, yy//16)
                if art is None:
                    return None
                pixels.append(art[(yy%16)*16+xx%16])
        return pixels
    def append(route, identity, orientation, x, y, w, h, depth, mapping,
               art=None, art_size=None, **extra):
        sx,sy = extra.get('source_bounds',[x,y])[:2]
        m, native = source(int(sx)//16, int(sy)//16)
        if extra.get('source_basis') == 'native':
            cell = cells.get((int(sx)//16,int(sy)//16))
            mid = cell.get('native_material_id',-1) if cell else -1
            m, native = (materials[mid],original[mid]) if mid >= 0 else (None,None)
        if extra.get('source_background',0) != 0:
            m,native=None,None
        shape = shapes.get((int(sx)//16, int(sy)//16), {})
        semantic = shape.get('semantic', 0)
        item = dict(id=identity, route=route, orientation=orientation,
                    bounds=[x,y,w,h], depth=depth, mapping=mapping,
                    material_key=m['key'] if m else None,
                    top_policy=m.get('top_policy',0) if m else 0,
                    semantic=SEMANTICS[semantic] if semantic < len(SEMANTICS) else str(semantic),
                    source_art=native, source_size=[16,16] if native else None,
                    rendered_art=art, rendered_size=art_size,
                    rendered_art_scope='compiled_donor_not_final_pixels', **extra)
        # Share content reviews only for identical geometry/art treatments.
        appearance = {k:v for k,v in extra.items() if k not in
                      ('parent_edge','contour_index','source_bounds','art_id')}
        item['treatment_signature'] = digest({
            'route':route, 'orientation':orientation, 'native':native,
            'donor':art, 'depth':depth, 'local_origin':[x%16,y%16],
            'mapping':mapping, 'metadata':appearance})
        surfaces.append(item)
    for index, values in enumerate(a['edges']):
        e = dict(zip(a['edge_fields'], values))
        side = e['side']
        # Retain unexpected side values explicitly instead of discarding them.
        direction = SIDES[side] if side < 4 else f'unknown_side_{side}'
        texture = bool(e['surface_texture'] or e['sample_size'])
        live = e['surface_texture'] == 3 and not e['owned_art']
        if e['owned_art']:
            art = owned[e['owned_art']-1]
        elif live:
            art = None  # The per-fragment footprint is sampled below.
        else:
            art = donor(e['sample_x'],e['sample_y'],e['sample_size'],e['sample_size'])
        size = [16,16] if e['owned_art'] else ([e['sample_size']]*2 if art else None)
        start = 0
        # An edge can span multiple native material cells. All fragments remain
        # counted, with parent index, and adjacent comparisons cross policies.
        while start < e['length']:
            x = e['x'] + (start if side%2==0 else 0)
            y = e['y'] + (start if side%2 else 0)
            length = min(e['length']-start,16-((y if side%2 else x)%16))
            # Live inward sand keeps the actual source columns; sample_size
            # describes depth rows, not a square donor. Tiny side risers keep
            # one column. Read only this fragment's footprint so adjacent ink
            # cannot create false detail/signature evidence.
            if live:
                width = 1 if side%2 else length
                art = donor(e['sample_x']+(0 if side%2 else start),
                            e['sample_y'],width,e['sample_size'])
                size = [width,e['sample_size']] if art else None
            append('generic',f'edge:{index}:{start}',direction,x,y,
                   1 if side%2 else length,length if side%2 else 1,
                   [e.get('physical_near',e['native_near']),e.get('physical_far',e['native_far'])],
                   'assigned' if texture else 'native_extrusion',art,size,
                   priority=e['priority'],colliding=bool(e['colliding']),
                   parent_edge=index,source_length=length,
                   sample_offset=[e['sample_x']-x,e['sample_y']-y],
                   sample_size=e['sample_size'],native_x=e['native_x'],
                   curve=[e['column'],e['trunk']],fold=[e['fold_top'],e['fold_bottom'],e['flat_rear_join']],
                   native_span_valid=e['native_span_valid'], texture_mode=e['surface_texture'],
                   contour_skin=e['contour_skin'],near_rank=e['near_rank'],far_rank=e['far_rank'])
            start += length
    art_indices = [1,5,6,4]
    for index, authored in enumerate(a['authored']):
        for ci, (x,y,length,side) in enumerate(authored['contours']):
            q = (y//8)*2+x//8
            art_index = art_indices[side]
            art_id = authored['art_ids'][art_index]
            art = owned[art_id] if 0 <= art_id < len(owned) else None
            automatic = bool(authored['automatic_art'] & (1 << art_index))
            sample = authored['samples'][art_index]
            left=authored['x']+8-authored['width']/2+authored['offset_x']
            append('authored',f'authored:{index}:contour:{ci}',SIDES[side],
                   left+x*authored['width']/16,authored['y']+y,1 if side%2 else length*authored['width']/16,
                   length if side%2 else 1,[authored['front'][q],authored['back'][q]],
                   'native_extrusion' if automatic and not sample[2] else 'assigned',art,[16,16] if art else None,
                   priority=authored['priority'][q],colliding=False,
                   authored_fields=authored, contour_index=ci,
                   authored_face_visible=bool(authored['faces'] & (1 << [2,5,3,4][side])),
                   source_bounds=[authored['x']+x,authored['y']+y,1 if side%2 else length,length if side%2 else 1],
                   world_bounds_status='requires_renderer_transform_and_bend')
        # Native-transform-only cells keep the generic contours; retain the
        # authored owner even when it has no separately emitted contours.
        if not authored['contours']:
            append('authored_owner',f'authored:{index}:owner','owner',authored['x'],authored['y'],16,16,
                   [min(authored['front']),max(authored['back'])],'assigned',
                   authored_fields=authored)
    for index,(x,y,art_id) in enumerate(a['profile_surfaces']):
        art = owned[art_id]
        append('profile',f'profile:{index}','profile',x,y,16,16,[0,data['depth']],
               'assigned',art,[16,16],art_id=art_id)
    for route,records in [('patch',a['patches']),('water',a['water']),('rear_fill',a['rear_fills'])]:
        for index,record in enumerate(records):
            x,y = record['x'],record['y']
            colors=record.get('uniform_color')
            background=record.get('background',0)
            selected_color=colors[background] if colors else 0
            art=[selected_color] if selected_color else decode_art(record.get('art',''))
            size=[1,1] if art and len(art)==1 else [16,16] if art else None
            append(route,f'{route}:{index}','top' if route=='water' else route,
                   x,y,record.get('width',16),record.get('height',1 if route=='water' else 16),
                   [0,data['depth']],'assigned' if art else 'specialized',art,size,
                   specialized_record=record, source_background=background)
    for route in ('gates','waterfalls','rear_trims'):
        for index,record in enumerate(a[route]):
            x,y=record[:2]
            append(route,f'{route}:{index}',route,x,y,record[2] if route!='gates' else (record[5] or 8),
                   record[3] if route=='waterfalls' else record[2] if route=='gates' else 1,
                   [0,data['depth']],'specialized',specialized_record=record)
    if a.get('trunk'):
        trunk = a['trunk']
        for index,mask in enumerate(trunk['core_masks']+trunk['side_masks']):
            occupied = sum(v.bit_count() for v in bytes.fromhex(mask))
            append('trunk',f'trunk:mask:{index}','curved',0,0,
                   trunk.get('side_width',0),trunk['height'],[0,data['depth']],
                   'specialized',mask_pixels=occupied,mask_sha256=hashlib.sha256(bytes.fromhex(mask)).hexdigest(),
                   depth_status='renderer_owned_unresolved',
                   trunk_art_hash=digest({k:v for k,v in trunk.items() if k.endswith('rgba')}))
    # Independent source-side floor denominators. Keep original-native and
    # effective edited collision separate; neither depends on emitted faces.
    # These witnesses are not prescribed voxel caps: slopes, hazards and thin
    # one-way floors need visual/context review rather than automatic infill.
    floor_counts={}
    for basis,field in [('native','native_material_id'),('effective','material_id')]:
        solid_quarters = set()
        for (cx,cy),cell in cells.items():
            mid=cell.get(field,-1)
            collision = materials[mid]['collision'] if mid >= 0 else 0
            for q in range(4):
                if collision & (1<<q):
                    solid_quarters.add((cx*2+(q&1),cy*2+(q>>1)))
        floor_counts[basis]=0
        for qx,qy in sorted(solid_quarters,key=lambda p:(p[1],p[0])):
            if (qx,qy-1) in solid_quarters:
                continue
            append(f'{basis}_floor_witness',f'{basis}_floor:{qx}:{qy}','source_floor',
                   qx*8,qy*8,8,8,[0,data['depth']],f'{basis}_collision_boundary',
                   colliding=True,source_basis=basis,depth_status='source_witness_no_physical_extent',
                   expected_geometry_status='source_witness_requires_rendered_context')
            floor_counts[basis]+=1
    routes=[]
    for r in a['routes']:
        if r.get('applicable') is False:
            routes.append(dict(route=r['route'], count=0, status='not_applicable',reason=r['status']))
        else:
            routes.append(dict(route=r['route'],count=r.get('items'),
                               status='inventoried' if r['status']=='inventoried_not_visually_accepted' else 'pending',
                               reason=r['status']))
    for basis,count in floor_counts.items():
        routes.append(dict(route=f'{basis}_collision_boundaries',count=count,
                           status='inventoried',reason=f'Independent {basis} walking-boundary witnesses; not inferred from emitted faces'))
    # Emitted inventory is exact; expected geometry remains an independent
    # obligation and is deliberately not set equal to the emitted count.
    return dict(scene_id=scene_id,room=data['room'],terrain=data['terrain'],section=data.get('section'),scope=scope,
                surfaces=surfaces,expected_routes=routes,expected_surface_count=None,
                compiled_counts={k:len(a[k]) for k in ('source_cells','edges','authored','patches','profile_surfaces','water','gates')},
                source_inventory_hash=digest(a),budgets=a.get('budgets',{}))


def normalize_legacy(data, scope='retained_pre_fix'):
    """Use retained diagnostic exports as negative controls, never acceptance.

    Old files cannot describe all specialized/edited output. Preserve that
    limitation explicitly while checking the original reported generic faces.
    """
    layer = data['layers'][0]
    materials = {m['id']: m for m in layer['materials']}
    rows = [bytes.fromhex(row) for row in layer['map_rows_hex']]
    palette = data.get('palette_snes', [])
    def source(cx, cy):
        if not (0 <= cy < len(rows) and 0 <= cx < len(rows[cy])):
            return None, None
        m = materials[rows[cy][cx]]
        pixels = []
        for i, char in enumerate(m['pixels_hex']):
            ink = int(char,16);q=(i//16//8)*2+(i%16//8)
            slot = ((m['words'][q]>>10)&7)*16+ink
            pixels.append((0x8000|palette[slot]) if ink and slot<len(palette) else (0x8000|ink) if ink else 0)
        return m,pixels
    surfaces=[]
    for index,values in enumerate(data['edges']):
        e=dict(zip(data['edge_fields'],values));side=e['side']
        if side not in range(4):
            raise ValueError('Unknown legacy edge side')
        offset=0
        while offset<e['length']:
            x=e['x']+(offset if not side%2 else 0);y=e['y']+(offset if side%2 else 0)
            length=min(e['length']-offset,16-((y if side%2 else x)%16))
            m,art=source(x//16,y//16)
            surfaces.append(dict(id=f'edge:{index}:{offset}',route='generic',orientation=SIDES[side],
                bounds=[x,y,1 if side%2 else length,length if side%2 else 1],depth=[e.get('near_rank',0),e.get('far_rank',2)],
                source_art=art,source_size=[16,16] if art else None,rendered_art=None,rendered_size=None,
                mapping='assigned' if e.get('surface_texture') or e.get('sample_size') else 'native_extrusion',
                material_key=m['key'] if m else None,top_policy=m.get('top_policy',0) if m else 0,
                priority=e.get('priority',0),colliding=e.get('colliding',False)))
            offset+=length
    return dict(scene_id=f"{scope}/{data['room']}",room=data['room'],terrain=data.get('terrain'),scope=scope,
                surfaces=surfaces,expected_surface_count=None,
                expected_routes=[dict(route='legacy_export_limits',count=None,status='pending',
                    reason='Retained old export cannot establish complete specialized/authored/source inventory')])
