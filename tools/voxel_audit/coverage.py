#!/usr/bin/env python3
"""Reconcile compiled source edges with current face-observer submissions.

This measures source-footprint/framing evidence, never final pixel visibility,
material correctness or acceptance. Backdrops, actor quads and opaque occluders
cannot certify a floor. Deformed/specialized geometry remains unresolved.
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
import math
from pathlib import Path

SIDES = ('top', 'right', 'bottom', 'left')
TOLERANCE = .035


def read_json(path):
    path = Path(path)
    with (gzip.open(path, 'rt') if path.suffix == '.gz' else path.open()) as source:
        return json.load(source)


def clipped_interval(face, viewport, axis):
    """Projectively correct native-source interval inside the actual viewport."""
    xy, world, weights = face['screen_xy'], face['world_xy'], face['clip_w']
    if len(xy) != 8 or len(world) != 8 or len(weights) != 4:
        raise ValueError('Invalid observer polygon dimensions')
    if not all(math.isfinite(v) for v in xy+world+weights) or min(weights) <= 0:
        raise ValueError('Invalid observer projection values')
    polygon = [[xy[i*2], xy[i*2+1], world[i*2+axis]/weights[i], 1/weights[i]] for i in range(4)]
    x0,y0,w,h = viewport
    if w <= 0 or h <= 0:
        raise ValueError('Invalid observer viewport')
    full = all(x0 <= v[0] <= x0+w and y0 <= v[1] <= y0+h for v in polygon)
    for coordinate, bound, direction in [(0,x0,1),(0,x0+w,-1),(1,y0,1),(1,y0+h,-1)]:
        if not polygon:
            break
        result=[]
        for index,b in enumerate(polygon):
            a=polygon[index-1]
            inside_a=(a[coordinate]-bound)*direction >= 0
            inside_b=(b[coordinate]-bound)*direction >= 0
            if inside_a != inside_b:
                t=(bound-a[coordinate])/(b[coordinate]-a[coordinate])
                result.append([av+(bv-av)*t for av,bv in zip(a,b)])
            if inside_b:
                result.append(b)
        polygon=result
    if len(polygon)<3:
        return None, False
    area=abs(sum(a[0]*b[1]-a[1]*b[0] for a,b in zip(polygon,polygon[1:]+polygon[:1])))/2
    if area < .01:
        return None, False
    source=[v[2]/v[3] for v in polygon]
    return [min(source),max(source)],full


def observe_face(face, viewport):
    # Only the existing generic cap observer has a stable native-source line.
    # In particular, kind3 is a backing mesh and kind0 a front face, not a top.
    if face.get('authored') or face.get('owner') or face.get('cell_x',-1)>=0:
        return None,'independent_owner_requires_separate_mapping'
    # BG1 primary/high/virtual-far ABI slots from diorama_planes.h. BG2
    # water/cloud callbacks require their independent source inventory.
    if face.get('plane') not in (0,6,11):
        return None,'non_bg1_source_plane'
    kind=face.get('kind')
    if kind not in (1,2,5,6,7):
        return None,'not_a_generic_edge'
    world=face['world_xy']
    if len(world)!=8 or not all(math.isfinite(v) for v in world):
        raise ValueError('Invalid observer world coordinates')
    if kind in (1,7):
        side=0 if kind==1 else 2;axis=0
    elif kind in (5,6):
        side=3 if kind==5 else 1;axis=1
    else:
        delta=world[3]-world[1]
        if abs(delta)<1e-5:
            return None,'ambiguous_side_orientation'
        side=1 if delta>0 else 3;axis=1
    perpendicular=world[1-axis::2]
    if max(perpendicular)-min(perpendicular)>TOLERANCE:
        return None,'nonlinear_source_footprint'
    offset=.99 if side in (1,2) else .01
    coordinate=sum(perpendicular)/4-offset
    line=round(coordinate)
    if abs(line-coordinate)>TOLERANCE:
        return None,'non_native_source_line'
    along=world[axis::2]
    interval=[min(along),max(along)]
    if interval[1]-interval[0]<1e-5:
        return None,'degenerate_source_interval'
    clipped,full=clipped_interval(face,viewport,axis)
    return dict(side=side,line=line,interval=interval,onscreen=clipped,
                fully_framed=clipped if full else None,priority=face.get('priority',0),
                donor=[face.get('art_source_x',0),face.get('art_source_y',0),face.get('art_source_size',0)]),None


def union_length(intervals,start,end):
    intervals=sorted((max(start,a),min(end,b)) for a,b in intervals if b>start and a<end)
    total=0.;last=start
    for a,b in intervals:
        if b>max(a,last):
            total+=b-max(a,last)
        last=max(last,b)
    return min(end-start,max(0.,total))


def fragment_records(data):
    a=data['surface_audit']
    cells={(v[0],v[1]):dict(zip(a['cell_fields'],v)) for v in a['source_cells']}
    for index,row in enumerate(a['edges']):
        edge=dict(zip(a['edge_fields'],row));side=edge['side']
        if side not in range(4):
            raise ValueError('Unknown compiled edge orientation')
        offset=0
        while offset<edge['length']:
            x=edge['x']+(offset if not side%2 else 0)
            y=edge['y']+(offset if side%2 else 0)
            length=min(edge['length']-offset,16-((y if side%2 else x)%16))
            reasons=[]
            if edge.get('column') or edge.get('trunk'):
                reasons.append('curved_geometry_requires_physical_mapping')
            if edge.get('fold_top') or edge.get('fold_bottom'):
                reasons.append('folded_geometry_requires_physical_mapping')
            cell=cells.get((x//16,y//16));ai=cell.get('authored_index',-1) if cell else -1
            if ai>=0:
                reasons.append('authored_owner_requires_physical_mapping')
            if edge.get('physical_span_valid') is False:
                reasons.append('no_resolved_generic_physical_span')
            yield dict(id=f'edge:{index}:{offset}',parent_edge=index,side=side,
                       orientation=SIDES[side],line=x if side%2 else y,
                       start=y if side%2 else x,length=length,bounds=[x,y,1 if side%2 else length,length if side%2 else 1],
                       priority=edge['priority'],allow_low_pass=edge.get('behind_span_valid',False) or edge.get('rear_span_valid',False),
                       donor=[edge.get('sample_x',0),edge.get('sample_y',0),edge.get('sample_size',0)],
                       derived=bool(edge.get('derived')),unresolved_geometry=reasons)
            offset+=length


def floor_witnesses(data, domain='native'):
    """Keep original map collision and effective stamp collision independent."""
    material_field={'native':'native_material_id','effective':'material_id'}[domain]
    a=data['surface_audit'];solid=set()
    for row in a['source_cells']:
        cell=dict(zip(a['cell_fields'],row));material_id=cell[material_field]
        # Authored extension cells have no original map material. In Python,
        # using their -1 sentinel as an index would invent a native floor.
        if material_id<0:
            continue
        collision=a['materials'][material_id]['collision']
        for quadrant in range(4):
            if collision&(1<<quadrant):
                solid.add((cell['x']*2+(quadrant&1),cell['y']*2+(quadrant>>1)))
    for x,y in sorted(solid,key=lambda v:(v[1],v[0])):
        if (x,y-1) not in solid:
            yield dict(id=f'{domain}_floor:{x}:{y}',side=0,orientation='source_floor',line=y*8,start=x*8,
                       collision_domain=domain,material_field=material_field,
                       length=8,bounds=[x*8,y*8,8,8],unresolved_geometry=[f'{domain}_collision_boundary_needs_context'])


def coverage(item,index):
    spans=defaultdict(list);examples=[]
    start,end=item['start'],item['start']+item['length']
    for observation in index.get((item['side'],item['line']),[]):
        a,b=observation['interval']
        if b<=start or a>=end:
            continue
        if 'priority' in item and observation['priority']!=item['priority'] and not (
                item.get('allow_low_pass') and observation['priority']==0):
            continue
        donor=item.get('donor');seen=observation.get('donor')
        if donor and seen[2] and donor[2] and not item.get('derived') and donor!=seen:
            continue
        spans['submitted'].append([a,b])
        for key in ('onscreen','fully_framed'):
            if observation[key]:
                spans[key].append(observation[key])
        if len(examples)<4:
            examples.append(observation['evidence'])
    measured={key:round(union_length(spans[key],start,end),5) for key in ('submitted','onscreen','fully_framed')}
    if measured['fully_framed']>=item['length']-1e-4:
        status='fully_framed_source_submission'
    elif measured['onscreen']>1e-4:
        status='partly_on_screen_submission'
    elif measured['submitted']>1e-4:
        status='submitted_offscreen'
    else:
        status='unseen'
    return dict(item,coverage_status=status,covered_source_pixels=measured,evidence=examples,
                disposition='pending',visibility='occlusion_and_final_pixels_not_evaluated')


def reconcile(data,views):
    index=defaultdict(list);ignored=Counter();accepted_views=[];excluded_views=[]
    for view in views:
        if not view.get('complete') or view.get('frame',0)!=0:
            excluded_views.append(dict(evidence=view.get('evidence'),reason='incomplete_or_wrong_character_phase'))
            continue
        fields=view['fields']
        for row in view['faces']:
            if len(row)!=len(fields):
                raise ValueError('Truncated observer face record')
            face=dict(zip(fields,row));observed,reason=observe_face(face,view['viewport'])
            if observed is None:
                ignored[reason]+=1;continue
            observed['evidence']=view['evidence']
            index[observed['side'],observed['line']].append(observed)
        accepted_views.append({k:view[k] for k in ('evidence','viewport','frame')})
    fragments=[coverage(item,index) for item in fragment_records(data)]
    witnesses=[coverage(item,index) for item in floor_witnesses(data,'native')]
    effective_witnesses=[coverage(item,index) for item in floor_witnesses(data,'effective')]
    def summary(items):
        result=dict(items=len(items),statuses=dict(Counter(v['coverage_status'] for v in items)),
                    source_pixels=sum(v['length'] for v in items),
                    unresolved_geometry_items=sum(bool(v['unresolved_geometry']) for v in items))
        result['covered_source_pixels']={k:round(sum(v['covered_source_pixels'][k] for v in items),3)
                                        for k in ('submitted','onscreen','fully_framed')}
        return result
    groups=defaultdict(list)
    for route,items in [('generic',fragments),('native_floor_witness',witnesses),
                        ('effective_floor_witness',effective_witnesses)]:
        for item in items:
            if item['coverage_status']=='fully_framed_source_submission' and not item['unresolved_geometry']:
                continue
            x,y,_,_=item['bounds'];key=(route,item['orientation'],math.floor(x/320),math.floor(y/224))
            groups[key].append(item)
    supplements=[]
    for (route,orientation,gx,gy),items in sorted(groups.items()):
        supplements.append(dict(route=route,orientation=orientation,bounds=dict(x=gx*320-48,y=gy*224-48,width=416,height=320),
            pitch=-.3 if orientation=='bottom' else .25,
            reason='projected_framing_or_physical_mapping_needs_review',surface_count=len(items),
            surface_ids=[v['id'] for v in items],invariant='Frame the complete exposed face; source-window inclusion alone is insufficient.'))
    return dict(schema=1,room=data['room'],terrain=data['terrain'],section=data.get('section','room'),
                complete=False,source_view_correspondence_status='requires_build_and_override_state_evidence',scope='source-footprint correspondence and projected framing, not pixel visibility or visual acceptance',
                generic=summary(fragments),native_floor_witnesses=summary(witnesses),
                effective_floor_witnesses=summary(effective_witnesses),
                by_orientation={side:summary([v for v in fragments if v['orientation']==side]) for side in SIDES},
                fragments=fragments,floor_witnesses=witnesses,effective_floor_records=effective_witnesses,supplements=supplements,
                views=accepted_views,excluded_views=excluded_views,ignored_observations=dict(ignored),
                pending_routes=data['surface_audit']['routes'],
                limits=['An editor hash alone does not prove raw/shipped/override source-state equality; separate build/state evidence is required.',
                        'No region inherits coverage without separately proven source/view equality.',
                        'A source interval can be observed at only part of its depth; no whole-face or occlusion acceptance is inferred.',
                        'Folded, curved, authored and specialized shapes need explicit physical mapping and visual review.',
                        'Native and effective-source collision witnesses use separate material identities; neither is inferred from emitted geometry.',
                        'Collision witnesses are independent candidates; slopes, pits and decorative source ink need native context.'])


def load_views(results,room,terrain,section,editor_sha=None):
    expected_room=room[:2]+'-'+room[2:]+('-completion' if section=='completion' else '')
    allowed_shas={editor_sha} if isinstance(editor_sha,str) else set(editor_sha or [])
    for path in results:
        report=read_json(path)
        if report.get('room')!=expected_room or not report.get('passed'):
            continue
        if allowed_shas and report.get('html_sha256') not in allowed_shas:
            continue
        for window in report.get('views',[]):
            if window.get('profile')!=terrain:
                continue
            for view in window['views']:
                observed=path.parent/view['observed']
                payload=read_json(observed)
                # Specialized routes can truthfully submit zero generic
                # callbacks. Older capture files omit the unused schema then.
                # A nonempty record without its schema remains malformed.
                fields=payload['fields'] if payload['faces'] else payload.get('fields',[])
                viewport=view.get('measure',{}).get('viewport')
                if not viewport:
                    continue
                yield dict(fields=fields,faces=payload['faces'],complete=view.get('complete',False),
                    frame=window.get('frame',0),viewport=[viewport[k] for k in ('x','y','w','h')],
                    evidence=dict(result=str(path),observed=str(observed),image=str(path.parent/view['image']),
                        window=window['id'],angle=view['angle'],html_sha256=report.get('html_sha256'),
                        editor_hash_verified=bool(allowed_shas)))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory',type=Path,required=True,help='One production --surfaces JSON or gzip file')
    parser.add_argument('--captures',type=Path,required=True,help='Capture tree containing result.json files; supplemental trees may be supplied repeatedly',action='append')
    parser.add_argument('--editor-sha256',action='append',help='Allow this exact editor hash; repeat for separately justified builds. Omitted means provenance remains unverified')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.resolve()==args.inventory.resolve():
        parser.error('Output must not overwrite the source inventory')
    for source in args.captures:
        protected=source.resolve() if source.is_dir() else source.resolve().parent
        if args.output.resolve().is_relative_to(protected):
            parser.error('Output must be outside every capture input directory')
    data=read_json(args.inventory)
    paths=sorted({p for root in args.captures for p in (list(root.rglob('result.json')) if root.is_dir() else [root])})
    views=load_views(paths,data['room'],data['terrain'],data.get('section','room'),args.editor_sha256)
    report=reconcile(data,views)
    report['inventory']=str(args.inventory.resolve())
    report['inventory_sha256']=hashlib.sha256(args.inventory.read_bytes()).hexdigest()
    report['required_editor_sha256']=args.editor_sha256
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('room','terrain','generic','native_floor_witnesses')},indent=2))


if __name__=='__main__':
    main()
