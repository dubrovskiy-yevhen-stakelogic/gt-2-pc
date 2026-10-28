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
    def run_import(self, prepare_code=0, package_code=0, create_bundle=True, install_failure=False, existing_bundle=False, interactive=False, target='Quest'):
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
            'if \"--verify\" in sys.argv: sys.exit(0)\n'
            f'if {create_bundle!r}: Path(sys.argv[sys.argv.index("--output")+1]).write_bytes(b"fixture")\n')
        (scripts/'platform-tools.ps1').write_text("function Get-Adb { return 'fixture-adb' }\n")
        (scripts/'install-quest.ps1').write_text(
            "param([switch]$Gt4Only,[string]$Gt4Bundle,[string]$Python,[string]$Adb,[string]$Serial)\n"
            "if(!$Gt4Only -or !(Test-Path -LiteralPath $Gt4Bundle)){throw 'Bad installation arguments'}\n"
            "Write-Host 'QUEST_TRANSFER_CALLED'\n" +
            ("throw 'fixture transfer failed'\n" if install_failure else "Write-Host 'QUEST_VERIFIED'\n"))
        (scripts/'install-gt4-pack.py').write_text(
            'from pathlib import Path\nimport sys\n'
            'runtime=Path(sys.argv[sys.argv.index("--runtime")+1])\n'
            'assert (runtime/"simulation/disc.raw2352").is_file()\n'
            'assert Path(sys.argv[sys.argv.index("--bundle")+1]).is_file()\n'
            '(runtime/"PC-INSTALL-CALLED").touch()\n'
            'print("PC_INSTALL_CALLED", flush=True)\n')
        iso = root/'disc.iso'  
        iso.touch()
        simulation = root/'runtime/simulation'
        simulation.mkdir(parents=True)
        (simulation/'disc.raw2352').touch()
        (root/'install-location.txt').write_text(str(simulation.parent))
        env = os.environ.copy()
        env['PSModulePath'] = str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/Modules')
        env['PATH'] = r'C:\Windows\System32;C:\Windows;C:\Windows\System32\WindowsPowerShell\v1.0'
        env['GT2_IMPORT_NO_PAUSE'] = '1'
        args = [r'C:\Windows\System32\cmd.exe', '/d', '/c', 'call', str(root/'IMPORT-GT4-MODELS.bat')]
        if existing_bundle:
            prepared = root/('work/gt4-import-existing/gt4-models.tar' if existing_bundle == 'discover' else 'work/completed/gt4-models.tar')
            prepared.parent.mkdir(parents=True)
            prepared.write_bytes(b'fixture')
            if existing_bundle != 'discover':
                args += ['-Bundle', str(prepared)]
        else:
            args += ['-Iso', str(iso), '-Simulation', str(simulation), '-Build', str(root), '-Output', 'work/completed']
        if not interactive:
            args += ['-Target', target]
        run = subprocess.run(args, env=env, capture_output=True, input='\n' if interactive or existing_bundle == 'discover' else None,
            text=True, encoding='utf-8', errors='replace', timeout=30)
        logs = list((root/'work/import-logs').glob('*.log'))
        self.assertEqual(len(logs), 1, run.stdout+run.stderr)
        log = logs[0].read_text(encoding='utf-8-sig')
        return root, run, log

    def test_success_transfers_to_quest_and_keeps_result(self):
        root, run, log = self.run_import()
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        result = (root/'work/completed/INSTALL-GT4.txt').read_text(encoding='utf-8-sig')
        self.assertIn('INSTALLATION COMPLETE ON QUEST', result)
        self.assertIn('QUEST_TRANSFER_CALLED', log)
        self.assertIn('QUEST_VERIFIED', log)
        self.assertIn('PREPARE_STAGE', log)
        self.assertIn('PREPARE_STDERR', log)
        self.assertIn('PACKAGE_STAGE', log)
        self.assertIn('INSTALLATION COMPLETE ON QUEST', log)

    def test_pc_and_pcvr_install_automatically_after_conversion(self):
        root, run, log = self.run_import(target='PC')
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('PC_INSTALL_CALLED', log)
        self.assertIn('INSTALLATION COMPLETE ON PC / PCVR', log)
        self.assertNotIn('QUEST_TRANSFER_CALLED', log)
        self.assertTrue((root/'runtime/PC-INSTALL-CALLED').exists())

    def test_pc_reuse_finds_installed_runtime_without_commands(self):
        root, run, log = self.run_import(target='PC', existing_bundle='discover')
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertNotIn('PREPARE_STAGE', log)
        self.assertTrue((root/'runtime/PC-INSTALL-CALLED').exists())

    def test_transfer_failure_never_reports_installed(self):
        root, run, log = self.run_import(install_failure=True)
        self.assertEqual(run.returncode, 1, run.stdout+run.stderr)
        self.assertIn('fixture transfer failed', log)
        self.assertNotIn('INSTALLATION COMPLETE', log)
        self.assertTrue((root/'work/completed/gt4-models.tar').exists())
        self.assertIn('not completed', (root/'work/completed/INSTALL-GT4.txt').read_text(encoding='utf-8-sig'))

    def test_existing_bundle_installs_without_conversion_or_dotnet(self):
        root, run, log = self.run_import(existing_bundle=True)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertNotIn('PREPARE_STAGE', log)
        self.assertNotIn('NET SDK ready:', log)
        self.assertIn('QUEST_TRANSFER_CALLED', log)

    def test_existing_conversion_is_discovered_without_manual_paths(self):
        root, run, log = self.run_import(existing_bundle='discover')
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertNotIn('PREPARE_STAGE', log)
        self.assertIn('Prepared GT4 bundle found:', log)
        self.assertIn('QUEST_TRANSFER_CALLED', log)

    def test_default_interactive_destination_is_quest(self):
        root, run, log = self.run_import(interactive=True)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('QUEST_TRANSFER_CALLED', log)

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
