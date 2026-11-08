#!/usr/bin/env python3
"""Plan additional witnesses from all source/geometry, without policy filtering.

Equality only reuses the BG1 geometry/source treatment represented in the export.
BG2/effect/runtime routes retain separate evidence requirements. A planned or
captured witness is not a reviewed disposition.
"""
from pathlib import Path
import argparse,collections,copy,gzip,hashlib,json,math,os

def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()

def load(path):return json.loads(gzip.decompress(path.read_bytes()))

def material(value):
    value=dict(value);value.pop('key',None)
    value['word_priority_palette']=[w&0x3c00 for w in value.pop('words',[])]
    return value

def canonical(data):
    s=data['surface_audit'];mats=s['materials'];art=s['owned_art']
    mat_hash=[digest(material(m))for m in mats]
    art_hash=[digest(a)for a in art]
    fields=s['edge_fields'];edges=[]
    for row in s['edges']:
        e=dict(zip(fields,row));owned=e.pop('owned_art',0)
        e['owned_art_content']=art_hash[owned-1] if owned else None
        edges.append(e)
    source=[]
    for c in s['source_cells']:
        source.append([c[0],c[1],mat_hash[c[3]]if c[3]>=0 else None,mat_hash[c[4]]if c[4]>=0 else None,s['pixel_edits'][c[5]]if c[5]>=0 else None,c[6]>=0])
    cells=[]
    for original in data['cells']:
        c={k:v for k,v in original.items()if k not in ('id','native_id','art','words')}
        cells.append(c)
    authored=copy.deepcopy(s['authored'])
    for a in authored:
        a['art_contents']=[art_hash[i]if 0<=i<len(art_hash)else None for i in a.pop('art_ids',[])]
    profile=[]
    for x,y,i in s['profile_surfaces']:profile.append([x,y,art_hash[i]if i<len(art_hash)else None])
    geometry={k:v for k,v in s.items()if k not in ('materials','source_cells','cell_fields','owned_art','edges','edge_fields','authored','profile_surfaces','pixel_edits','budgets','scope','schema','complete_visual_review','routes')}
    geometry.update(source_cells=source,cells=cells,edges=edges,authored=authored,profile_surfaces=profile)
    geometry['scene']={k:data[k]for k in ('room','section','profile','depth','bounds','frame')}
    return digest(geometry)

def contains(w,b):
    a=w['bounds'];return a['x']<=b[0] and a['y']<=b[1] and a['x']+a['width']>=b[0]+b[2] and a['y']+a['height']>=b[1]+b[3]

def source_bounds(data):
    s=data['surface_audit'];m=s['materials']
    # Opaque-black native ink is included. Only truly empty native+effective
    # material cells can omit a source-only witness; generated geometry follows.
    for c in s['source_cells']:
        if any(i>=0 and any(p!='0' for p in m[i]['pixels_hex'])for i in (c[3],c[4])):
            yield(c[0]*16,c[1]*16,16,16)
    for row in s['edges']:
        e=dict(zip(s['edge_fields'],row));vertical=e['side']&1
        for at in range(0,e['length'],16):
            n=min(16,e['length']-at)
            yield(e['x']+(0 if vertical else at),e['y']+(at if vertical else 0),1 if vertical else n,n if vertical else 1)
    for a in s['authored']:yield(a['x']+8+a['offset_x']-a['width']/2,a['y'],a['width'],16)
    for p in s['patches']:yield(p['x'],p['y'],p['width'],p['height'])
    for x,y,_ in s['profile_surfaces']:yield(x,y,16,16)
    for w in s['water']:yield(w['x'],w['y'],w['width'],1)
    for x,y,w,h,_ in s['waterfalls']:yield(x,y,w,h)
    for f in s['rear_fills']:yield(f['x'],f['y'],16,16)


def add_coverage(windows,targets,profile,next_id):
    added=[]
    for b in targets:
        # Split broad specialized objects into finite independently covered
        # cells rather than wrongly expecting one viewport to contain a level.
        for yy in range(math.floor(b[1]),math.ceil(b[1]+max(1,b[3])),16):
            for xx in range(math.floor(b[0]),math.ceil(b[0]+max(1,b[2])),16):
                t=(xx,yy,min(16,b[0]+max(1,b[2])-xx),min(16,b[1]+max(1,b[3])-yy))
                if any(contains(w,t)for w in windows+added):continue
                x=math.floor(xx/320)*320;y=math.floor(yy/192)*192
                w={'id':next_id,'bounds':{'x':x,'y':y,'width':400,'height':256},'profiles':[profile],'label':'current-inventory-extension','freeCamera':True,'distance':1.08,'invariant':'Source/compiled geometry lies beyond old coordinate seed; visual disposition remains pending.'}
                next_id+=1;added.append(w)
    return added,next_id


def bottom_family(edge, surface, cells):
    """Keep every compiled treatment flag; only position/length may be reused."""
    e=dict(edge)
    cell=cells.get((e['x']//16,e['y']//16))
    source=surface['materials'][cell[4]] if cell and cell[4]>=0 else None
    donor_cell=cells.get((e['sample_x']//16,e['sample_y']//16))
    donor=surface['materials'][donor_cell[4]] if e['sample_size'] and donor_cell and donor_cell[4]>=0 else None
    owned=e.pop('owned_art')
    e['owned_art_content']=digest(surface['owned_art'][owned-1]) if owned else None
    e['source_phase']=[e.pop('x')%16,e.pop('y')%16]
    e['sample_phase']=[e.pop('sample_x')%16,e.pop('sample_y')%16]
    e.pop('length')
    e['source_material']=material(source) if source else None
    e['donor_material']=material(donor) if donor else None
    return digest(e)


def bottom_witnesses(data,windows,profile,next_id):
    s=data['surface_audit'];cells={(c[0],c[1]):c for c in s['source_cells']};families=collections.defaultdict(list)
    for row in s['edges']:
        e=dict(zip(s['edge_fields'],row))
        if e['side']!=2 or not(e.get('physical_span_valid')or e.get('native_span_valid')):continue
        families[bottom_family(e,s,cells)].append(e)
    remaining=set(families);selected=[]
    while remaining:
        choices=[]
        for w in windows:
            hits={sig for sig in remaining if any(contains(w,(e['x'],e['y'],min(e['length'],16),1))for e in families[sig])}
            if hits:choices.append((len(hits),w,hits))
        if not choices:break
        _,w,hits=max(choices,key=lambda a:a[0]);selected.append({'id':next_id,'bounds':w['bounds'],'pitch':-.3,'distance':1.12,'profiles':[profile],'label':'bottom-treatment-witness','freeCamera':True,'treatment_signatures':sorted(hits),'invariant':'Negative pitch is needed to see undersides; each recorded treatment still requires visual review.'});next_id+=1;remaining-=hits
    return selected,next_id,{'families':len(families),'planned_families':len(families)-len(remaining),'unlocated_families':sorted(remaining)}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--inventory',type=Path,required=True);p.add_argument('--seed',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    assert os.getpriority(os.PRIO_PROCESS,0)==15,'Run at nice15'
    seed=json.loads(a.seed.read_text());lookup={(f"{r['group']:02x}{r['map']:02x}",r.get('section')or'room'):r for r in seed['rooms']}
    out=[];equal=[];coverage=[]
    for file in sorted((a.inventory/'0').glob('*.json.gz')):
        base=load(file);key=(base['room'],base['section']);original=lookup[key];room={k:original[k]for k in ('id','group','map','section')};room['windows']=[];next_id=2000;base_sig=canonical(base)
        for profile in range(3):
            data=base if not profile else load(a.inventory/str(profile)/file.name)
            sig=base_sig if not profile else canonical(data);same=profile>0 and sig==base_sig
            equal.append({'room':room['id'],'profile':profile,'canonical_sha256':sig,'equal_to_profile0':same,'reuse_scope':'BG1 source and exported compiled geometry only; BG2/effects/runtime objects need separate evidence'})
            windows=copy.deepcopy(original['windows'])
            extra,next_id=add_coverage(windows,source_bounds(data),profile,next_id);windows+=extra
            if not same:
                if profile:room['windows']+=[dict(w,id=next_id+i,profiles=[profile],label='non-equivalent-region')for i,w in enumerate(original['windows'])];next_id+=len(original['windows'])if profile else 0
                room['windows']+=extra
                bottom,next_id,stats=bottom_witnesses(data,windows,profile,next_id);room['windows']+=bottom
            else:stats={'reused_from_profile0':True}
            coverage.append({'room':room['id'],'profile':profile,'source_geometry_extra_windows':len(extra),'region_equivalent':same,'bottom':stats,'authored_transform_coverage_pending':[{'x':a['x'],'y':a['y'],'reason':'The source footprint is inventoried; transformed/bent screen coverage needs actual observation or focused inspection.'}for a in data['surface_audit']['authored'] if any(a.get('bend',[0])[3:]) or any(a.get('transform',[0]*9)[3:6]) or any(v not in (0,1) for v in a.get('transform',[0]*9)[6:])]})
        out.append(room);print(room['id'],len(room['windows']),flush=True)
    a.output.mkdir(parents=True,exist_ok=True)
    (a.output/'regional-equality.json').write_text(json.dumps({'basis':'Exact canonical used native/effective BG1 art, edited source, compiled geometry and donor art; no policy-only equivalence.','rows':equal},indent=2)+'\n')
    (a.output/'visual-inventory-supplement-plan.json').write_text(json.dumps({'scope':'Current inventory extensions, non-equivalent regional maps and underside witnesses. Capturing is not visual acceptance.','rooms':out},indent=2)+'\n')
    (a.output/'visual-inventory-coverage.json').write_text(json.dumps(coverage,indent=2)+'\n')
    print('profile variants',len(equal),'reused',sum(r['equal_to_profile0']for r in equal),'supplement_windows',sum(len(r['windows'])for r in out),flush=True)

if __name__=='__main__':main()
