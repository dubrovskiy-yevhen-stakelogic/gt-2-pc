"""Exercise the release importer outside the source checkout."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
files = re.findall(r"^    '([^']+)'", (ROOT/'scripts/gt4-import-files.ps1').read_text(), re.M)
assert len(files) == len(set(files))
with tempfile.TemporaryDirectory(prefix='gt2-import-package-') as tmp:
    package = Path(tmp)
    for name in files:
        destination = package/name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/name, destination)
    for path in (package/'scripts').glob('*.py'):
        tree = ast.parse(path.read_text(encoding='utf-8-sig'))
        for node in ast.walk(tree):
            modules = ([node.module] if isinstance(node, ast.ImportFrom) else
                       [a.name for a in node.names] if isinstance(node, ast.Import) else [])
            for module in modules:
                if module and (ROOT/'scripts'/f'{module}.py').is_file():
                    assert (package/'scripts'/f'{module}.py').is_file(), module
    subprocess.run([sys.executable, '-B', str(package/'scripts/prepare-gt4-release.py'), '--help'],
                   cwd=tmp, check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, '-B', '-c',
                   'from authored_cockpit_pack import assets; assert len(assets()) == 36'],
                   cwd=package/'scripts', check=True)
    for name in ('gt4-maker-map.json', 'gt4-expansion-models.json', 'gt4-native-interiors.json'):
        assert (package/'db'/name).is_file()
    assert (package/'tools/gt4catalog/Program.cs').is_file()
    assert (package/'scripts/gt4_vif_positions.cs').is_file()
print('GT4 packaged importer checks passed')
