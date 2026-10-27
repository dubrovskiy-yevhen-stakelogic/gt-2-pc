"""Synthetic regression for native body/wheel registration, without retail data."""
import copy
import math
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_gt4_wheels import aligned_pose


def source(pitch=0):
    angle=math.radians(pitch);c=math.cos(angle);s=math.sin(angle);height=.08
    matrix=[1,0,0,0,0,c,s,0,0,-s,c,0,0,height,0,1]
    wheels=[]
    for z,radius in [(-1.3,.34),(1.3,.35)]:
        y=(radius-height+s*z)/c
        for x in [-.75,.75]:
            wheels.append(dict(position=[x,y,z],dimensions=[.25,radius,.12],angles=[0,0,0],
                               donorBasis=[matrix[4*c+r] for c in range(3) for r in range(3)]))
    return dict(body=[height,pitch],bodyMatrix=matrix,wheels=wheels,sourceWheel=[0,1,2,3])


class WheelPoseTests(unittest.TestCase):
    def setUp(self):
        self.alignment=dict(nativeScale=1.2,nativeOffset=[.1,.02,-.03])

    def test_body_low_point_never_moves_wheel_centres(self):
        # A deep underbody/exhaust can change the AABB while suspension is fixed.
        first=dict(self.alignment,bboxGround=-.08)
        second=dict(self.alignment,bboxGround=-.40)
        actual=aligned_pose(first,source())
        self.assertEqual(actual,aligned_pose(second,source()))
        self.assertAlmostEqual(actual['wheels'][0][1],.26*1.2+.02)
        old_bbox_y=(second['bboxGround']+.34)*1.2+.02
        self.assertGreater(abs(actual['wheels'][0][1]-old_bbox_y),.3)

    def test_pitch_keeps_both_axles_at_ground_after_rotation(self):
        pose=aligned_pose(self.alignment,source(-1.2));r=pose['bodyRotation'];pivot=pose['bodyPivot']
        self.assertEqual(pose['bodyPitchDegrees'],-1.2)
        errors=[];unrotated=[]
        for wheel in pose['wheels']:
            p=[wheel[k]-pivot[k] for k in range(3)]
            y=sum(r[k*3+1]*p[k] for k in range(3))+pivot[1]-pose['ground']
            errors.append(y-wheel[3]);unrotated.append(wheel[1]-pose['ground']-wheel[3])
        self.assertLess(max(abs(v) for v in errors),1e-12)
        self.assertGreater(max(abs(v) for v in unrotated),.03)

    def test_different_axle_radii_are_not_averaged(self):
        pose=aligned_pose(self.alignment,source())
        self.assertAlmostEqual(pose['wheels'][2][1]-pose['wheels'][0][1],.01*1.2)
        self.assertAlmostEqual(pose['ground'],-.08*1.2+.02)

    def test_raw_anchors_angles_and_source_mapping_survive(self):
        native=source();native['sourceWheel']=[0,0,1,2]
        native['wheels'][1]=copy.deepcopy(native['wheels'][0])
        pose=aligned_pose(self.alignment,native)
        self.assertEqual(pose['wheels'][0],pose['wheels'][1])
        self.assertEqual(pose['sourceWheel'],[0,0,1,2])
        self.assertEqual(pose['wheelAngles'][0],[0,0,0])
        self.assertEqual(pose['wheelBasis'][0],native['wheels'][0]['donorBasis'])

    def test_invalid_pose_is_not_silently_replaced_by_bbox(self):
        for index,value in [(5,2),(0,-1),(13,.2),(9,float('nan'))]:
            native=source();native['bodyMatrix'][index]=value
            with self.subTest(index=index),self.assertRaises(ValueError):aligned_pose(self.alignment,native)

    def test_missing_native_pose_is_an_error(self):
        with self.assertRaises(KeyError):aligned_pose(self.alignment,{})

    def test_invalid_donor_basis_is_rejected(self):
        for index,value in [(0,-1),(4,2),(8,float('nan'))]:
            native=source();native['wheels'][0]['donorBasis'][index]=value
            with self.subTest(index=index),self.assertRaises(ValueError):aligned_pose(self.alignment,native)

    def test_opposite_camber_is_preserved_without_extra_rim_flip(self):
        native=source();angle=math.radians(2);c=math.cos(angle);s=math.sin(angle)
        native['wheels'][0].update(angles=[0,0,-2],donorBasis=[c,-s,0,s,c,0,0,0,1])
        native['wheels'][1].update(angles=[0,0,-2],donorBasis=[c,s,0,-s,c,0,0,0,1])
        pose=aligned_pose(self.alignment,native)
        self.assertLess(pose['wheelBasis'][0][1],0)
        self.assertGreater(pose['wheelBasis'][1][1],0)
        self.assertGreater(pose['wheelBasis'][0][0],0)
        self.assertGreater(pose['wheelBasis'][1][0],0)


if __name__=='__main__':unittest.main()
