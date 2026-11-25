import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
import json
from types import SimpleNamespace
from gt4_pack_install_checks import bundle as full_fixture

spec = importlib.util.spec_from_file_location('quest_gt4_bundle', Path(__file__).resolve().parents[1]/'scripts/package-quest-gt4.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleChecks(unittest.TestCase):
    def test_selected_interior_metadata_is_packaged_and_bound(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);full_fixture(root/'fixture.tar')
            with tarfile.open(root/'fixture.tar') as archive:
                files={m.name:archive.extractfile(m).read() for m in archive if m.name!='modelsets/gt4/quest-install.sha256'}
            # Include all reviewed family IDs while retaining the complete 1110-car contract.
            rename=dict(zip([f'z{i:04d}' for i in range(5)],['nn32n','nr02n','nr32n','nv12n','nv22n']))
            for name,raw in list(files.items()):
                if name.endswith('.json'):
                    value=json.loads(raw)
                    if isinstance(value.get('cars'),dict):value['cars']={rename.get(k,k):v for k,v in value['cars'].items()}
                    elif isinstance(value.get('cars'),list):
                        for car in value['cars']:car['id']=rename.get(car['id'],car['id'])
                    files[name]=json.dumps(value).encode()
            for name,raw in files.items():
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(raw)
            pack=root/'modelsets/gt4';native=pack/'native-v16'
            args=SimpleNamespace(pack=pack,native=native,arcade_expansion=None,simulation_expansion=None,output=root/'models.tar')
            bundle.build(args)
            with tarfile.open(args.output) as archive:
                names=archive.getnames()
                self.assertIn('modelsets/gt4/native-v16/interiors.json',names)
                self.assertEqual(sum(n.startswith('modelsets/gt4/authored/') and n.endswith('/manifest.json') for n in names),12)
                for name in names:
                    if name.startswith('modelsets/gt4/authored/') and name.endswith('.glb'):bundle.validate_glb(archive.extractfile(name).read())
            cabins=json.loads((native/'interiors.json').read_text());cabins['cars']['nn32n']['mesh']='other.gltf'
            (native/'interiors.json').write_text(json.dumps(cabins));args.output=root/'bad.tar'
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
            path=Path(root)/'valid.tar';full_fixture(path);bundle.verify(path)

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
