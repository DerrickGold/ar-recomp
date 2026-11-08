"""Synthetic observer/framing checks: no game artwork or renderer required."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('coverage_audit',ROOT/'tools/voxel_audit/coverage.py')
C=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(C)
FIELDS=['x','y','length','side','priority','sample_x','sample_y','sample_size','derived','column','trunk','fold_top','fold_bottom','behind_span_valid','rear_span_valid','physical_span_valid']

def scene(edges=None,collision=1,authored=False):
    return dict(room='0201',terrain=0,section='room',surface_audit=dict(edge_fields=FIELDS,
        edges=edges if edges is not None else [[0,0,8,0,0,0,0,0,0,0,0,0,0,False,False,True]],
        cell_fields=['x','y','material_id','authored_index','native_material_id'],source_cells=[[0,0,0,0 if authored else -1,0]],
        materials=[dict(collision=collision)],routes=[dict(route='ground_cover',status='pending')]))

def face(kind=1,screen=None,world=None,**kwargs):
    return dict(kind=kind,priority=0,plane=11,cell_x=-1,cell_y=-1,owner=0,authored=0,
        art_source_x=0,art_source_y=0,art_source_size=0,
        world_xy=world or [0,.01,8,.01,8,.01,0,.01],
        screen_xy=screen or [10,10,18,10,18,20,10,20],clip_w=[1,1,1,1],**kwargs)

def view(*faces,viewport=(0,0,100,100),frame=0,complete=True):
    fields=list(faces[0]) if faces else []
    return dict(fields=fields,faces=[[f[k] for k in fields]for f in faces],viewport=viewport,frame=frame,complete=complete,evidence={'image':'synthetic'})

class SurfaceCoverageTests(unittest.TestCase):
    def test_empty_specialized_observer_without_fields_is_not_floor_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'observed.json').write_text(json.dumps({'faces':[]}))
            report=root/'result.json'
            report.write_text(json.dumps({'room':'02-01','passed':True,'html_sha256':'approved',
                'views':[{'profile':0,'frame':0,'id':1,'views':[{'observed':'observed.json','complete':True,'image':'view.png','angle':'left','measure':{'viewport':dict(x=0,y=0,w=100,h=100)}}]}]}))
            views=list(C.load_views([report],'0201',0,'room',['approved']))
            r=C.reconcile(scene(),views)
            self.assertEqual(r['native_floor_witnesses']['statuses'],{'unseen':1})
            self.assertFalse(r['complete'])

    def test_separately_allowed_editor_builds_remain_identified(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);reports=[]
            for sha in ('before','after','unknown'):
                folder=root/sha;folder.mkdir()
                (folder/'observed.json').write_text(json.dumps({'fields':[],'faces':[]}))
                report=folder/'result.json';reports.append(report)
                report.write_text(json.dumps({'room':'02-01','passed':True,'html_sha256':sha,
                    'views':[{'profile':0,'frame':0,'id':1,'views':[{'observed':'observed.json','complete':True,'image':'view.png','angle':'left','measure':{'viewport':dict(x=0,y=0,w=100,h=100)}}]}]}))
            views=list(C.load_views(reports,'0201',0,'room',['before','after']))
            self.assertEqual([v['evidence']['html_sha256'] for v in views],['before','after'])
            self.assertTrue(all(v['evidence']['editor_hash_verified'] for v in views))

    def test_stamp_collision_does_not_rewrite_native_floor_denominator(self):
        d=scene(collision=1)
        d['surface_audit']['materials'].append(dict(collision=0))
        d['surface_audit']['source_cells'][0][2]=1
        r=C.reconcile(d,[])
        self.assertEqual(r['native_floor_witnesses']['items'],1)
        self.assertEqual(r['effective_floor_witnesses']['items'],0)
        self.assertEqual(r['floor_witnesses'][0]['material_field'],'native_material_id')

    def test_authored_extension_has_no_invented_original_floor(self):
        d=scene(collision=1)
        d['surface_audit']['source_cells'][0][-1]=-1
        r=C.reconcile(d,[])
        self.assertEqual(r['native_floor_witnesses']['items'],0)
        self.assertEqual(r['effective_floor_witnesses']['items'],1)

    def test_backdrop_does_not_certify_floor(self):
        r=C.reconcile(scene(),[view(face(3))])
        self.assertEqual(r['generic']['statuses'],{'unseen':1})
        self.assertEqual(r['native_floor_witnesses']['statuses'],{'unseen':1})
        self.assertEqual(r['ignored_observations'],{'not_a_generic_edge':1})

    def test_bg2_surface_does_not_certify_bg1_floor(self):
        f=face();f['plane']=12
        r=C.reconcile(scene(),[view(f)])
        self.assertEqual(r['generic']['statuses'],{'unseen':1})
        self.assertEqual(r['ignored_observations'],{'non_bg1_source_plane':1})

    def test_full_source_frame_is_not_visual_acceptance(self):
        r=C.reconcile(scene(),[view(face())])
        self.assertEqual(r['generic']['covered_source_pixels']['fully_framed'],8)
        self.assertFalse(r['complete'])
        self.assertEqual(r['source_view_correspondence_status'],'requires_build_and_override_state_evidence')
        self.assertEqual(r['fragments'][0]['disposition'],'pending')
        self.assertTrue(r['floor_witnesses'][0]['unresolved_geometry'])

    def test_source_window_cannot_certify_projected_offscreen_deck(self):
        r=C.reconcile(scene(),[view(face(screen=[10,627,18,627,18,655,10,655]),viewport=(0,1,960,597))])
        self.assertEqual(r['fragments'][0]['coverage_status'],'submitted_offscreen')
        self.assertEqual(r['generic']['covered_source_pixels']['onscreen'],0)
        self.assertTrue(r['supplements'])

    def test_partial_horizontal_clip_retains_partial_source_interval(self):
        r=C.reconcile(scene(),[view(face(screen=[-4,10,4,10,4,20,-4,20]))])
        self.assertEqual(r['generic']['covered_source_pixels']['onscreen'],4)
        self.assertEqual(r['generic']['covered_source_pixels']['fully_framed'],0)

    def test_perspective_clip_uses_reciprocal_w(self):
        f=face(screen=[-4,10,4,10,4,20,-4,20]);f['clip_w']=[1,2,2,1]
        interval,full=C.clipped_interval(f,(0,0,100,100),0)
        self.assertAlmostEqual(interval[0],8/3)
        self.assertFalse(full)

    def test_clipped_depth_is_not_fully_framed(self):
        r=C.reconcile(scene(),[view(face(),viewport=(0,0,100,15))])
        self.assertEqual(r['generic']['covered_source_pixels']['onscreen'],8)
        self.assertEqual(r['generic']['covered_source_pixels']['fully_framed'],0)

    def test_duplicate_views_do_not_inflate_denominator(self):
        r=C.reconcile(scene(),[view(face()),view(face())])
        self.assertEqual(r['generic']['covered_source_pixels']['submitted'],8)
        self.assertEqual(r['generic']['source_pixels'],8)

    def test_wrong_phase_and_overflow_are_not_evidence(self):
        r=C.reconcile(scene(),[view(face(),frame=1),view(face(),complete=False)])
        self.assertEqual(r['generic']['statuses'],{'unseen':1})
        self.assertEqual(len(r['excluded_views']),2)

    def test_curves_and_authored_owners_remain_physically_unresolved(self):
        d=scene(authored=True);d['surface_audit']['edges'][0][9]=1
        r=C.reconcile(d,[view(face())]);fragment=r['fragments'][0]
        self.assertEqual(fragment['coverage_status'],'fully_framed_source_submission')
        self.assertEqual(len(fragment['unresolved_geometry']),2)
        self.assertEqual(r['generic']['unresolved_geometry_items'],1)

    def test_high_deck_behind_actor_pass_retains_native_source_match(self):
        d=scene();d['surface_audit']['edges'][0][4]=1;d['surface_audit']['edges'][0][13]=True
        r=C.reconcile(d,[view(face())])
        self.assertEqual(r['generic']['covered_source_pixels']['onscreen'],8)

    def test_side_orientation_comes_from_native_winding(self):
        d=scene([[7,0,8,1,0,0,0,0,0,0,0,0,0,False,False,True]])
        f=face(2,world=[7.99,0,7.99,8,7.99,8,7.99,0])
        r=C.reconcile(d,[view(f)])
        self.assertEqual(r['by_orientation']['right']['covered_source_pixels']['onscreen'],8)
        self.assertEqual(r['native_floor_witnesses']['statuses'],{'unseen':1})

    def test_opaque_actor_or_authored_quad_does_not_impersonate_generic_top(self):
        f=face();f['authored']=1;f['cell_x']=0
        r=C.reconcile(scene(),[view(f)])
        self.assertEqual(r['generic']['statuses'],{'unseen':1})

    def test_independent_floor_witness_survives_absent_emitted_geometry(self):
        r=C.reconcile(scene(edges=[]),[])
        self.assertEqual(r['generic']['items'],0)
        self.assertEqual(r['native_floor_witnesses']['items'],1)
        self.assertFalse(r['complete'])

    def test_donor_conflict_remains_unseen(self):
        d=scene();d['surface_audit']['edges'][0][7]=16
        f=face();f['art_source_size']=16;f['art_source_x']=16
        r=C.reconcile(d,[view(f)])
        self.assertEqual(r['generic']['statuses'],{'unseen':1})

if __name__=='__main__':unittest.main()
