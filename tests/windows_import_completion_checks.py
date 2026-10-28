"""Check Windows import completion/error handling without converting game data."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'nt', 'Windows importer launcher')
class ImportCompletionChecks(unittest.TestCase):
    def run_import(self, prepare_code=0, package_code=0, create_bundle=True):
        temp = tempfile.TemporaryDirectory(prefix="gt2 player's import ")
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        scripts = root/'scripts'
        scripts.mkdir()
        shutil.copy2(ROOT/'IMPORT-GT4-MODELS.bat', root/'IMPORT-GT4-MODELS.bat')
        shutil.copy2(ROOT/'scripts/import-gt4-models.ps1', scripts/'import-gt4-models.ps1')
        executable = "'"+sys.executable.replace("'", "''")+"'"
        # Keep the real wrapper and subprocess exit codes; isolate expensive
        # conversion and bootstrap, which have separate integration checks.
        (scripts/'windows-prerequisites.ps1').write_text(
            'function Get-Gt2Python { return '+executable+' }\n'
            'function Get-Gt2DotNet { return '+executable+' }\n')
        (scripts/'prepare-gt4-release.py').write_text(
            'from pathlib import Path\nimport sys\n'
            'print("PREPARE_STAGE", flush=True)\n'
            'print("PREPARE_STDERR", file=sys.stderr, flush=True)\n'
            f'if {prepare_code}: sys.exit({prepare_code})\n'
            'Path(sys.argv[sys.argv.index("--output")+1]).mkdir(parents=True)\n')
        (scripts/'package-quest-gt4.py').write_text(
            'from pathlib import Path\nimport sys\n'
            'print("PACKAGE_STAGE", flush=True)\n'
            f'if {package_code}: sys.exit({package_code})\n'
            f'if {create_bundle!r}: Path(sys.argv[sys.argv.index("--output")+1]).write_bytes(b"fixture")\n')
        iso = root/'disc.iso'
        iso.touch()
        simulation = root/'runtime/simulation'
        simulation.mkdir(parents=True)
        (simulation/'disc.raw2352').touch()
        env = os.environ.copy()
        env['PATH'] = r'C:\Windows\System32;C:\Windows;C:\Windows\System32\WindowsPowerShell\v1.0'
        env['GT2_IMPORT_NO_PAUSE'] = '1'
        run = subprocess.run([r'C:\Windows\System32\cmd.exe', '/d', '/c', 'call',
            str(root/'IMPORT-GT4-MODELS.bat'), '-Iso', str(iso), '-Simulation', str(simulation),
            '-Build', str(root), '-Output', 'work/completed'], env=env, capture_output=True,
            text=True, encoding='utf-8', errors='replace', timeout=30)
        logs = list((root/'work/import-logs').glob('*.log'))
        self.assertEqual(len(logs), 1, run.stdout+run.stderr)
        log = logs[0].read_text(encoding='utf-8-sig')
        return root, run, log

    def test_success_keeps_result_and_quest_command(self):
        root, run, log = self.run_import()
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        result = (root/'work/completed/INSTALL-GT4.txt').read_text(encoding='utf-8-sig')
        self.assertIn('INSTALLATION IS STILL REQUIRED', result)
        self.assertIn('-Target Quest -Gt4Bundle', result)
        self.assertIn("player''s", result)
        self.assertIn('PREPARE_STAGE', log)
        self.assertIn('PREPARE_STDERR', log)
        self.assertIn('PACKAGE_STAGE', log)
        self.assertIn('INSTALLATION IS STILL REQUIRED', log)

    def test_conversion_failure_preserves_exit_and_log(self):
        root, run, log = self.run_import(prepare_code=17)
        self.assertEqual(run.returncode, 17, run.stdout+run.stderr)
        self.assertNotIn('PACKAGE_STAGE', log)
        self.assertIn('exit code 17', log)
        self.assertFalse((root/'work/completed/INSTALL-GT4.txt').exists())

    def test_package_failure_is_not_success(self):
        root, run, log = self.run_import(package_code=23)
        self.assertEqual(run.returncode, 23, run.stdout+run.stderr)
        self.assertIn('PACKAGE_STAGE', log)
        self.assertIn('exit code 23', log)
        self.assertFalse((root/'work/completed/INSTALL-GT4.txt').exists())

    def test_missing_bundle_is_not_success(self):
        root, run, log = self.run_import(create_bundle=False)
        self.assertEqual(run.returncode, 1, run.stdout+run.stderr)
        self.assertIn('bundle is missing', log)
        self.assertFalse((root/'work/completed/INSTALL-GT4.txt').exists())


if __name__ == '__main__':
    unittest.main()
