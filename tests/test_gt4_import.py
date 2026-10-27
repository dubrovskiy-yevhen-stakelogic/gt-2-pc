"""Synthetic importer checks: no proprietary assets or rendering required."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from gt4_mesh import convert, number, shape_commands
from import_gt4_models import extract
import import_gt4_models as importer
import io
import zlib
from build_gt4_expansion import write_catalogue_version


class ImportChecks(unittest.TestCase):
    def test_catalogue_version_has_platform_independent_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            write_catalogue_version(folder)
            self.assertEqual((Path(folder)/'catalogue.version').read_bytes(), b'gt2-car-expansion-1\n')

    def test_ps2_alpha_state_is_per_draw_and_specular_is_retained(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);dump=root/'dump';dump.mkdir();(dump/'Textures').mkdir()
            (dump/'Textures/0.1.png').write_bytes(b'synthetic texture')
            (dump/'model_set.yaml').write_text('Models:\n  model0:\n          shape1: {}\n          shape2:\n            - ColorMask(0xFF000000)\n            - DisableAlphaTest\n')
            (dump/'model0.mtl').write_text('newmtl paint\nKd 1 1 1 1\nKs 1 1 1 0\nmap_Kd Textures/0.1.png\n')
            (dump/'model0.obj').write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\no shape1\nusemtl paint\nf 1/1 2/1 3/1\no shape2\nf 1/1 2/1 3/1\n')
            convert(dump,root/'output/body.gltf')
            doc=json.loads((root/'output/body.gltf').read_text())
            self.assertEqual([m['alphaMode'] for m in doc['materials']],['MASK','OPAQUE'])
            self.assertTrue(all(m['extras']['gt2pc']['reflection'] for m in doc['materials']))
            self.assertEqual([p['material'] for p in doc['meshes'][0]['primitives']],[0,1])
            with (dump/'model_set.yaml').open('a') as f:
                f.write('          shape3:\n            - DisableAlphaTest\n            - BlendFunction(0, 1, 0, 1, 0)\n')
            with (dump/'model0.obj').open('a') as f:f.write('o shape3\nf 1/1 2/1 3/1\n')
            convert(dump,root/'blended/body.gltf')
            blended=json.loads((root/'blended/body.gltf').read_text())
            self.assertEqual([p['material'] for p in blended['meshes'][0]['primitives']],[0,1,0])

    def test_uv_addresses_original_tex0_not_cropped_png(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);dump=root/'dump';dump.mkdir();(dump/'Textures').mkdir()
            (dump/'Textures/0.1.png').write_bytes(b'\x89PNG\r\n\x1a\n'+bytes(8)+struct.pack('>II',384,128))
            (dump/'texture_addressing.json').write_text(json.dumps({'0.1.png':{'width':512,'height':128}}))
            (dump/'model_set.yaml').write_text('Models:\n  model0:\n          shape1: {}\n')
            (dump/'model0.mtl').write_text('newmtl paint\nmap_Kd Textures/0.1.png\n')
            (dump/'model0.obj').write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0.6 1.1\no shape1\nusemtl paint\nf 1/1 2/1 3/1\n')
            convert(dump,root/'out/body.gltf')
            doc=json.loads((root/'out/body.gltf').read_text())
            idx=doc['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0']
            offset=doc['bufferViews'][doc['accessors'][idx]['bufferView']]['byteOffset']
            u,v=struct.unpack_from('<ff',(root/'out/body.bin').read_bytes(),offset)
            self.assertAlmostEqual(u,.8);self.assertAlmostEqual(v,-.1)

    def test_cli_donor_cache_separates_open_and_lod(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);iso=root/'disc.iso';iso.write_bytes(b'fixture')
            tool=root/'tool.dll';tool.write_bytes(b'fixture tool')
            profiles=root/'profiles.json'
            profiles.write_text(json.dumps({'discTocSha256':'fixture','cars':{
                'aaaaa':{'name':'first','code':'same0001','directory':'lod'},
                'bbbbb':{'name':'alias','code':'same0001','directory':'lod'},
                'ccccc':{'name':'open resource','code':'same0001','directory':'open'}}}))
            records=[{'kind':1,'path':'car/'+d+'/same0001','payload':d.encode()} for d in ['lod','open']]
            decoded=[];converted=[]
            def decode(command,**kwargs):
                raw=Path(command[-1]);decoded.append((raw.parent.name,raw.read_bytes()))
                dump=raw.with_name(raw.name+'_dump');dump.mkdir()
                (dump/'texture_addressing.json').write_text('{}')
            def convert(dump,output,**kwargs):
                self.assertTrue(dump.is_dir());converted.append(dump.parent.name)
                output.parent.mkdir(parents=True,exist_ok=True);output.write_text('{}')
                return {'triangles':1}
            argv=['importer','--iso',str(iso),'--tool',str(tool),'--profiles',str(profiles),'--output',str(root/'pack')]
            with patch.object(sys,'argv',argv), patch.object(importer,'inventory',return_value={'toc_fingerprint_sha256':'fixture','entries':records}), patch.object(importer,'extract',side_effect=lambda stream,entry:entry['payload']), patch.object(importer.subprocess,'run',side_effect=decode), patch.object(importer,'convert',side_effect=convert), patch('builtins.print'):
                importer.main()
            self.assertEqual(decoded,[('lod',b'lod'),('open',b'open')])
            self.assertEqual(converted,['lod']*4+['open']*2)
            self.assertEqual(len(json.loads((root/'pack/manifest.json').read_text())['cars']),3)
            self.assertEqual(json.loads((root/'pack/provenance.json').read_text())['profilesSha256'],importer.digest(profiles))

    def test_compressed_size_validation(self):
        data = b'CAR4' + bytes(range(200))
        compressor=zlib.compressobj(wbits=-15)
        packed=bytes.fromhex('c5eef7ff')+struct.pack('<I',(-len(data))&0xffffffff)+compressor.compress(data)+compressor.flush()
        entry=dict(offset=0,size=len(data),stored_size=len(packed),kind=2)
        self.assertEqual(extract(io.BytesIO(packed),entry),data)
        with self.assertRaises(ValueError): extract(io.BytesIO(packed[:-3]),entry)
        with self.assertRaises(ValueError): extract(io.BytesIO(packed),dict(entry,size=1))

    def test_decimal_locale_and_nonfinite(self):
        self.assertEqual(number('0,125'),.125)
        for value in ('nan','inf','-inf'):
            with self.assertRaises(ValueError): number(value)

    def test_highest_lod_callbacks_and_coordinates(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); dump=root/'dump'; dump.mkdir()
            (dump/'model_set.yaml').write_text('Models:\n  model0:\n          shape1: {}\n          shape2_tail_lamp_on:\n            - DisableDepthMask\n  model1:\n          shape1:\n            - DisableDepthMask\n')
            commands=shape_commands(dump/'model_set.yaml')
            self.assertNotIn('DisableDepthMask',commands['shape1'])
            (dump/'model0.mtl').write_text('newmtl plain\nKd 1 1 1 1\n')
            (dump/'model0.obj').write_text('v 0,25 1 -2\nv 1 1 -2\nv 0 2 -2\nvt 0.2 0.3\nvn 0 0 1\no shape1\nusemtl plain\nf 1/1/1 2/1/1 3/1/1\no shape2_tail_lamp_on\nf 1/1/1 2/1/1 3/1/1\n')
            stats=convert(dump,root/'out/body.gltf')
            self.assertEqual(stats['triangles'],1)
            self.assertEqual(stats['shapes'][0]['min'],[0,1,-2])
            doc=json.loads((root/'out/body.gltf').read_text()); binary=(root/'out/body.bin').read_bytes()
            self.assertEqual(struct.unpack_from('<fff',binary),(.25,1,-2))
            index=doc['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0']
            view=doc['bufferViews'][doc['accessors'][index]['bufferView']]
            u,v=struct.unpack_from('<ff',binary,view['byteOffset'])
            self.assertAlmostEqual(u,.2); self.assertAlmostEqual(v,.7)
            with self.assertRaises(ValueError): convert(dump,root/'empty/body.gltf',exclude_shapes=['shape1'])

    def test_frame_albedo_wrap_and_opaque_paint(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);dump=root/'dump';dump.mkdir();(dump/'Textures').mkdir()
            (dump/'Textures/0.64.png').write_bytes(b'synthetic copy fixture')
            (dump/'model_set.yaml').write_text('Models:\n  model0:\n          shape45:\n            - ColorMask(0x00FFFFFF)\nTextures:\n  0.64.png:\n    WrapModeS: SCE_GS_REPEAT\n')
            (dump/'model0.mtl').write_text('newmtl paint\nmap_Kd Textures/0.64.png\n')
            (dump/'model0.obj').write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nvt -1 3\no shape45\nusemtl paint\nf 1/1 2/1 3/1\n')
            with self.assertRaises(ValueError): convert(dump,root/'skip/body.gltf')
            convert(dump,root/'out/body.gltf',include_shapes=['shape45'],opaque_textures=['0.64.png'])
            doc=json.loads((root/'out/body.gltf').read_text())
            self.assertEqual(doc['samplers'],[{'wrapS':10497,'wrapT':33071}])
            self.assertEqual(doc['materials'][0]['alphaMode'],'OPAQUE')
            # Out-of-range UVs must survive: clamping happens per pixel, not
            # per vertex, or the windshield aperture is distorted.
            binary=(root/'out/body.bin').read_bytes();idx=doc['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0']
            offset=doc['bufferViews'][doc['accessors'][idx]['bufferView']]['byteOffset']
            self.assertEqual(struct.unpack_from('<ff',binary,offset),(-1,-2))
            convert(dump,root/'mirror/cockpit.gltf',cockpit=True,include_shapes=['shape45'],mirror_textures=['0.64.png'])
            mirror=json.loads((root/'mirror/cockpit.gltf').read_text())
            self.assertTrue(mirror['materials'][0]['extras']['gt2pc']['rearView'])
            self.assertNotIn('rearView',doc['materials'][0]['extras']['gt2pc'])


if __name__=='__main__': unittest.main()
