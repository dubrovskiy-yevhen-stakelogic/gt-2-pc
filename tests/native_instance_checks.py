import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from gt4_native_instances import resolve_draws
from gt4_native_cabins import cabin_draws,merge_cabin,split_exterior_glazing

class NativeInstanceChecks(unittest.TestCase):
    def draw(self,x):
        matrix=[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,0.,x,0.,0.,1.]
        return dict(shape=10,packet=0,material=0,texset=-1,texture=-1,wheel=-1,spinning=False,
                    matrix=matrix,type=1,blend=False,blendEquation=68,alphaFunc=1,alphaRef=0,
                    alphaFail=0,cull=True,depthWrite=True,vertices=[[x,0,0],[x+.2,0,0],[x,.3,0]])

    def test_two_seats_keep_distinct_transforms(self):
        left,right=self.draw(-.35),self.draw(.35)
        later=copy.deepcopy([left,right])
        for draw in later:draw['vertices']=[]
        result=resolve_draws([[left,right],later])
        self.assertEqual(len(result),2)
        self.assertEqual([d['vertices'][0][0] for d,_ in result],[-.35,.35])
        self.assertEqual([mask for _,mask in result],[3,3])
        # The original shape-only merge necessarily loses one of these seats.
        self.assertEqual(len({(d['shape'],d['packet'],d['material']) for d in (left,right)}),1)

    def test_render_state_and_multiplicity_are_not_geometry(self):
        solid=self.draw(0);blend=copy.deepcopy(solid);blend.update(vertices=[],blend=True,depthWrite=False)
        repeat=copy.deepcopy(solid);repeat['vertices']=[]
        result=resolve_draws([[solid,repeat,blend],[repeat]])
        self.assertEqual(len(result),3)
        self.assertEqual([mask for _,mask in result],[3,1,1])
        self.assertTrue(all(d['vertices']==solid['vertices'] for d,_ in result))

    def test_missing_other_instance_must_fail(self):
        other=self.draw(.4);other['vertices']=[]
        with self.assertRaises(ValueError):resolve_draws([[self.draw(-.4),other]])

    def test_donor_keeps_both_seats_and_rejects_stale_selection(self):
        meta={'states':[[self.draw(-.35),self.draw(.35)]]}
        selection={'packets':[{'shape':10,'packet':0}]}
        self.assertEqual(len(cabin_draws(meta,selection)),2)
        selection['packets'].append({'shape':11,'packet':0})
        with self.assertRaises(ValueError):cabin_draws(meta,selection)

    def test_door_component_selection_is_exact(self):
        draw=self.draw(0);draw['vertices']+=self.draw(2)['vertices']
        meta={'states':[[draw]]};selection={'packets':[{'shape':10,'packet':0,'triangles':[0]}]}
        self.assertEqual(cabin_draws(meta,selection)[0][0]['vertices'],self.draw(0)['vertices'])
        self.assertEqual(len(draw['vertices']),6)
        selection['packets'][0]['triangles']=[2]
        with self.assertRaises(ValueError):cabin_draws(meta,selection)

    def test_donor_namespaces_and_exterior_survive(self):
        def metadata(draw,value):return {'states':[[draw]],'materials':[[[value]*17]],'paintMaterials':[[[value+1]*17]]}
        body=metadata(self.draw(0),1);donor=metadata(self.draw(.35),4)
        donor['states'][0][0].update(texture=0,texset=0)
        copies=[];selection={'packets':[{'shape':10,'packet':0}],'exteriorOnlyPackets':[[10,0]]}
        merged=merge_cabin(body,donor,selection,lambda old,new:copies.append((old,new)))
        self.assertTrue(merged['states'][0][0]['exteriorOnly'])
        self.assertTrue(merged['states'][0][1]['interiorOnly'])
        self.assertEqual(merged['states'][0][1]['material'],1)
        self.assertEqual(merged['paintMaterials'][0],[body['paintMaterials'][0][0],donor['materials'][0][0]])
        self.assertEqual(len(copies),1)
        self.assertNotIn('exteriorOnly',body['states'][0][0])

    def test_canopy_split_preserves_lower_shell_and_instances(self):
        draws=[]
        for x in (-.4,.4):
            draw=self.draw(x);draw['vertices']+=self.draw(x+2)['vertices'];draws.append((draw,63))
        rule=[{'shape':10,'packet':0,'triangles':[1]}]
        result=split_exterior_glazing(draws,rule)
        self.assertEqual(len(result),4)
        self.assertEqual(sum(len(d['vertices']) for d,_ in result),12)
        self.assertEqual(sum(len(d['vertices']) for d,_ in result if not d.get('exteriorOnly')),6)
        self.assertEqual({tuple(d['matrix']) for d,_ in result},{tuple(d['matrix']) for d,_ in draws})
        self.assertTrue(all(mask==63 for _,mask in result))
        with self.assertRaises(ValueError):split_exterior_glazing(draws,[{'shape':10,'packet':0,'triangles':[2]}])
        with self.assertRaises(ValueError):split_exterior_glazing(draws,[{'shape':99,'packet':0,'triangles':[0]}])

if __name__=='__main__':unittest.main()
