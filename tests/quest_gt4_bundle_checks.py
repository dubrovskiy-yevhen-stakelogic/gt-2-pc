import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
import json
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('quest_gt4_bundle', Path(__file__).resolve().parents[1]/'scripts/package-quest-gt4.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleChecks(unittest.TestCase):
    def test_selected_interior_metadata_is_packaged_and_bound(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);pack=root/'pack';native=root/'native';pack.mkdir();native.mkdir()
            def write(path,value):path.write_text(json.dumps(value))
            write(pack/'manifest.json',{'format':'gt2-model-set','version':1,'id':'gt4','cars':{'car':{}}})
            write(native/'manifest.json',{'format':'gt4-native-overlay','version':1,'cars':{'car':'body.gltf'}})
            write(native/'wheels.json',{})
            write(native/'body.gltf',{'buffers':[],'images':[]})
            write(native/'paints.json.gz',{'colors':[]})
            # document() treats .gz as gzip; this fixture uses the real encoding.
            import gzip
            (native/'paints.json.gz').write_bytes(gzip.compress(b'{"colors":[]}'))
            cabins={'format':'gt4-native-interiors','version':1,'cars':{'car':{'mesh':'body.gltf','nativeInterior':True,'openTop':False}}}
            write(native/'interiors.json',cabins)
            args=SimpleNamespace(pack=pack,native=native,arcade_expansion=None,simulation_expansion=None,output=root/'models.tar')
            bundle.build(args)
            with tarfile.open(args.output) as archive:self.assertIn('modelsets/gt4/native-v16/interiors.json',archive.getnames())
            cabins['cars']['car']['mesh']='other.gltf';write(native/'interiors.json',cabins);args.output=root/'bad.tar'
            with self.assertRaisesRegex(ValueError,'Interior source differs'):bundle.build(args)

    def archive(self, root, extra=None, corrupt=False):
        path = Path(root)/'bundle.tar'
        files = {name: b'{}' for name in ('modelsets/gt4/manifest.json',
                 'modelsets/gt4/native-v16/manifest.json', 'modelsets/gt4/native-v16/wheels.json')}
        sums = ''.join(hashlib.sha256(data).hexdigest()+'  '+name+'\n' for name, data in files.items())
        files['modelsets/gt4/quest-install.sha256'] = sums.encode()
        if corrupt:
            files['modelsets/gt4/manifest.json'] = b'changed'
        with tarfile.open(path, 'w') as out:
            for name, data in files.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                out.addfile(entry, io.BytesIO(data))
            if extra:
                out.addfile(extra)
        return path

    def test_valid_inventory(self):
        with tempfile.TemporaryDirectory() as root:
            bundle.verify(self.archive(root))

    def test_payload_corruption(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                bundle.verify(self.archive(root, corrupt=True))

    def test_save_overwrite_and_path_aliases_rejected(self):
        for name in ('saves/card1.mcd', 'modelsets/gt4/../../saves/card1.mcd',
                     '/modelsets/gt4/manifest.json', 'modelsets/gt4/./manifest.json'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as root:
                with self.assertRaisesRegex(ValueError, 'Unsafe'):
                    bundle.verify(self.archive(root, tarfile.TarInfo(name)))

    def test_links_and_duplicate_members_rejected(self):
        for link in (False, True):
            with self.subTest(link=link), tempfile.TemporaryDirectory() as root:
                entry = tarfile.TarInfo('modelsets/gt4/manifest.json')
                if link:
                    entry.type = tarfile.SYMTYPE
                    entry.linkname = '../../saves/card1.mcd'
                with self.assertRaisesRegex(ValueError, 'Unsafe'):
                    bundle.verify(self.archive(root, entry))


if __name__ == '__main__':
    unittest.main()
