#!/usr/bin/env python3
"""Audit dedup may ignore unused tables, never used art, geometry or collision."""
import copy,importlib.util,tempfile,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('visual_plan',Path(__file__).resolve().parents[1]/'tools/voxel_audit/plan-visual.py')
plan=importlib.util.module_from_spec(spec);spec.loader.exec_module(plan)
capture_spec=importlib.util.spec_from_file_location('visual_captures',Path(__file__).resolve().parents[1]/'tools/voxel_audit/run-captures.py')
captures=importlib.util.module_from_spec(capture_spec);capture_spec.loader.exec_module(captures)

def scene():
    return {'room':'0101','section':'room','terrain':0,'profile':1,'depth':.24,'bounds':[0,0,1,1],'frame':{'bgsc':[1,2]},
      'cells':[{'x':0,'y':0,'id':5,'native_id':5,'words':[1]*4,'art':'provenance','visual_art':'nativevisual','collision':1,'front':[0]*4,'back':[.24]*4}],
      'surface_audit':{'materials':[{'key':'provenance','words':[1]*4,'pixels_hex':'1'+'0'*255,'rgba555_hex':'8000'+'0000'*255,'collision':1,'flags':0,'top_policy':0}],
      'source_cells':[[0,0,5,0,0,-1,-1]],'owned_art':['8000'*256],'pixel_edits':[],
      'edge_fields':['x','y','length','side','owned_art'],'edges':[[0,0,16,0,1]],'authored':[],
      'profile_surfaces':[],'patches':[],'water':[],'waterfalls':[],'rear_fills':[],
      'origin':[0,0],'size':[16,16],'front_rows_hex':['01'*16]*16}}

class VisualPlan(unittest.TestCase):
    def test_unused_table_identity_does_not_force_regional_duplicate(self):
        a=scene();b=copy.deepcopy(a);b['terrain']=2;b['surface_audit']['materials'].append({'unused':True});b['surface_audit']['owned_art'].append('ffff'*256)
        b['surface_audit']['materials'][0]['key']='different provenance';b['surface_audit']['materials'][0]['words']=[7]*4
        b['cells'][0].update(id=77,native_id=77,words=[7]*4,art='different provenance')
        self.assertEqual(plan.canonical(a),plan.canonical(b))
    def test_used_art_collision_and_geometry_never_reuse(self):
        a=scene()
        for kind in ('native_art','collision','priority','owned_art','edge','background_register'):
            with self.subTest(kind=kind):
                b=copy.deepcopy(a)
                if kind=='native_art':b['surface_audit']['materials'][0]['rgba555_hex']='8001'+'0000'*255
                if kind=='collision':b['surface_audit']['materials'][0]['collision']=0
                if kind=='priority':b['surface_audit']['materials'][0]['words'][0]|=0x2000
                if kind=='owned_art':b['surface_audit']['owned_art'][0]='8002'*256
                if kind=='edge':b['surface_audit']['edges'][0][2]=8
                if kind=='background_register':b['frame']['bgsc'][1]=3
                self.assertNotEqual(plan.canonical(a),plan.canonical(b))
    def test_no_policy_black_ink_and_authored_width_are_inventoried(self):
        a=scene();a['surface_audit']['authored']=[{'x':32,'y':16,'width':8,'offset_x':0}]
        boxes=list(plan.source_bounds(a))
        self.assertIn((0,0,16,16),boxes) # Native opaque black is not empty.
        self.assertIn((36,16,8,16),boxes)
        extra,_=plan.add_coverage([],boxes,0,2000)
        self.assertTrue(all(any(plan.contains(w,b)for w in extra)for b in boxes))
    def test_underside_family_keeps_fold_taper_and_placement_flags(self):
        s=scene()['surface_audit'];cells={(0,0):s['source_cells'][0]}
        edge={'x':0,'y':0,'length':16,'side':2,'sample_x':0,'sample_y':0,'sample_size':16,'owned_art':1}
        baseline=plan.bottom_family(edge,s,cells)
        for field in ('fold_top','fold_bottom','flat_rear_join','column','trunk','thin_trim','rear_mounted','raised_scenery','native_actor_priority'):
            with self.subTest(field=field):self.assertNotEqual(baseline,plan.bottom_family(dict(edge,**{field:1}),s,cells))

    def test_long_generated_surface_requires_full_extent_coverage(self):
        extra,_=plan.add_coverage([],[(0,64,1024,1)],0,2000)
        self.assertTrue(all(any(plan.contains(w,(x,64,16,1))for w in extra)for x in range(0,1024,16)))
        self.assertGreater(len(extra),1)

class CaptureResume(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.directory=Path(self.temp.name)
        self.room={'id':'01-01','group':1,'map':1,'section':'','windows':[{'id':0,'bounds':{'x':0,'y':0,'width':16,'height':16},'angles':['front']}]}
        self.result={'passed':True,'html_sha256':'editor','room':'01-01','profiles':[0],'plan_sha256':captures.room_plan_hash(self.room),
          'views':[{'id':0,'bounds':self.room['windows'][0]['bounds'],'native':{'native':'native.png','effective':'effective.png'},'views':[{'angle':'front','pitch':0,'image':'front.png','observed':'front.json.gz'}]}]}
        for name in ('native.png','effective.png','front.png','front.json.gz'):(self.directory/name).write_bytes(b'fixture')
    def reusable(self):return captures.complete_capture(self.result,self.directory,self.room,0,'editor')
    def test_front_camera_effective_pitch_resumes(self):self.assertTrue(self.reusable())
    def test_new_bg2_or_room_identity_needs_new_capture(self):
        for change in ('bg2','group','section'):
            with self.subTest(change=change):
                room=copy.deepcopy(self.room)
                if change=='bg2':room['windows'][0]['bg2']=True
                else:room[change]=2 if change=='group' else 'completion'
                self.assertFalse(captures.complete_capture(self.result,self.directory,room,0,'editor'))
    def test_missing_png_or_observer_cannot_resume(self):
        for name in ('native.png','front.json.gz'):
            with self.subTest(name=name):
                (self.directory/name).unlink();self.assertFalse(self.reusable());(self.directory/name).write_bytes(b'fixture')

if __name__=='__main__':unittest.main()
