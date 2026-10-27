"""Bundle updates preserve saves and reject corrupt or escaping payloads."""
import hashlib
import gzip
import importlib.util
import io
import json
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('gt4_install', Path(__file__).resolve().parents[1]/'scripts/install-gt4-pack.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)
spec = importlib.util.spec_from_file_location('gt4_package', Path(__file__).resolve().parents[1]/'scripts/package-quest-gt4.py')
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


def bundle(path, corrupt=False, escape=False, subset=False, missing_interior=False,
           missing_native=False, missing_mesh=False, missing_expansion_model=False):
    def encode(value):
        return json.dumps(value,separators=(',',':')).encode('utf-8')
    ids = [f'z{i:04d}' for i in range(1 if subset else 1110)]
    mesh = 'fixture/body.gltf.gz'
    cars = {car: dict(gt4Model='fixture',required=index<722) for index,car in enumerate(ids)}
    native = {car:mesh for car in ids}
    if missing_native:
        native.pop(ids[-1])
    cabins = {car:dict(mesh=mesh,nativeInterior=True) for car in ids[:89]}
    expansion = [dict(id=car) for car in ids[:722]]
    if missing_expansion_model:
        expansion[-1]['id']='x0000'
    files = {
        'modelsets/gt4/manifest.json':encode(dict(format='gt2-model-set',version=1,id='gt4',cars=cars)),
        'modelsets/gt4/native-v16/manifest.json':encode(dict(format='gt4-native-overlay',version=1,cars=native)),
        'modelsets/gt4/native-v16/interiors.json':encode(dict(format='gt4-native-interiors',version=1,cars=cabins)),
        'modelsets/gt4/native-v16/wheels.json':encode(dict(format='gt4-wheel-geometry',version=2,cars={car:{} for car in ids},missing=[])),
        'modelsets/gt4/native-v16/'+mesh:gzip.compress(encode(dict(buffers=[dict(uri='body.bin.gz')],images=[]))),
        'modelsets/gt4/native-v16/fixture/body.bin.gz':gzip.compress(b'mesh fixture'),
        'modelsets/gt4/native-v16/fixture/paints.json.gz':gzip.compress(encode(dict(format='gt4-native-paints',version=1,colors=[dict(images={})]))),
        'arcade/expansions/gt4/assets/carparam/gt4_expansion.json':encode(dict(format='gt2-car-expansion',version=1,cars=expansion))}
    if missing_interior:
        files.pop('modelsets/gt4/native-v16/interiors.json')
    if missing_mesh:
        files.pop('modelsets/gt4/native-v16/'+mesh)
    files['arcade/expansions/gt4/catalogue.version'] = b'gt2-car-expansion-1\n'
    if escape:
        files['modelsets/gt4/../../../saves/card.mcd'] = b'overwrite'
    inventory = ''.join(hashlib.sha256(data).hexdigest()+'  '+name+'\n' for name,data in files.items())
    if corrupt:
        files['modelsets/gt4/manifest.json'] = b'corrupt'
    files['modelsets/gt4/quest-install.sha256'] = inventory.encode('ascii')
    with tarfile.open(path, 'w') as archive:
        for name,data in files.items():
            entry=tarfile.TarInfo(name);entry.size=len(data)
            archive.addfile(entry,io.BytesIO(data))


class InstallChecks(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.runtime=self.root/'runtime'
        for folder in ('arcade','saves','modelsets/gt4','arcade/expansions/gt4'):
            (self.runtime/folder).mkdir(parents=True,exist_ok=True)
        (self.runtime/'arcade/disc.raw2352').write_bytes(b'disc fixture')
        (self.runtime/'saves/card.mcd').write_bytes(b'keep saved game')
        (self.runtime/'modelsets/gt4/old.bin').write_bytes(b'old model fixture')
        (self.runtime/'arcade/expansions/gt4/old.txt').write_text('old catalogue')
        self.archive=self.root/'models.tar'

    def unchanged(self):
        self.assertEqual((self.runtime/'saves/card.mcd').read_bytes(),b'keep saved game')
        self.assertEqual((self.runtime/'arcade/disc.raw2352').read_bytes(),b'disc fixture')

    def test_update_keeps_saves_and_previous_models(self):
        bundle(self.archive)
        installer.install(self.archive,self.runtime)
        self.unchanged()
        self.assertTrue((self.runtime/'modelsets/gt4/native-v16/wheels.json').is_file())
        backups=list((self.runtime/'gt4-backups').iterdir())
        self.assertEqual(len(backups),1)
        self.assertEqual((backups[0]/'modelsets/gt4/old.bin').read_bytes(),b'old model fixture')
        self.assertFalse(list(self.runtime.glob('.gt4-stage-*')))

    def test_corrupt_bundle_leaves_installation_untouched(self):
        bundle(self.archive,corrupt=True)
        with self.assertRaisesRegex(ValueError,'checksum mismatch'):
            installer.install(self.archive,self.runtime)
        self.unchanged()
        self.assertTrue((self.runtime/'modelsets/gt4/old.bin').is_file())
        self.assertFalse((self.runtime/'gt4-backups').exists())

    def test_escaping_bundle_is_rejected(self):
        bundle(self.archive,escape=True)
        with self.assertRaisesRegex(ValueError,'Unsafe'):
            installer.install(self.archive,self.runtime)
        self.unchanged()

    def test_missing_disc_is_rejected_before_asset_changes(self):
        bundle(self.archive)
        (self.runtime/'arcade/disc.raw2352').unlink()
        with self.assertRaisesRegex(ValueError,'Prepare the arcade disc'):
            installer.install(self.archive,self.runtime)
        self.assertTrue((self.runtime/'modelsets/gt4/old.bin').is_file())
        self.assertFalse((self.runtime/'gt4-backups').exists())

    def test_subset_cannot_replace_an_installed_full_pack(self):
        bundle(self.archive,subset=True)
        with self.assertRaisesRegex(ValueError,'1110 model mappings'):
            installer.install(self.archive,self.runtime)
        self.unchanged()
        self.assertTrue((self.runtime/'modelsets/gt4/old.bin').is_file())
        self.assertTrue((self.runtime/'arcade/expansions/gt4/old.txt').is_file())
        self.assertFalse((self.runtime/'gt4-backups').exists())

    def test_packager_rejects_a_diagnostic_subset(self):
        bundle(self.archive,subset=True)
        inputs=self.root/'inputs'
        with tarfile.open(self.archive) as archive:
            for entry in archive:
                target=inputs/entry.name
                target.parent.mkdir(parents=True,exist_ok=True)
                target.write_bytes(archive.extractfile(entry).read())
        output=self.root/'subset-output.tar'
        args=SimpleNamespace(pack=inputs/'modelsets/gt4',native=inputs/'modelsets/gt4/native-v16',
                             arcade_expansion=inputs/'arcade/expansions/gt4',simulation_expansion=None,output=output)
        with self.assertRaisesRegex(ValueError,'1110 model mappings'):
            packager.build(args)
        self.assertFalse(output.exists())

    def test_incomplete_native_assets_are_rejected(self):
        for option, message in [('missing_interior','Missing required GT4 asset'),
                                ('missing_native','complete model catalogue'),
                                ('missing_mesh','Missing or unsafe GT4 asset reference'),
                                ('missing_expansion_model','required GT4 models')]:
            with self.subTest(option=option):
                bundle(self.archive,**{option:True})
                with self.assertRaisesRegex(ValueError,message):
                    installer.install(self.archive,self.runtime)
                self.unchanged()
                self.assertTrue((self.runtime/'modelsets/gt4/old.bin').is_file())
                self.assertFalse((self.runtime/'gt4-backups').exists())


if __name__=='__main__':
    unittest.main()
