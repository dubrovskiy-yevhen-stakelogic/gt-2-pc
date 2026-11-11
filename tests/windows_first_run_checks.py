"""Exercise the real first-run launcher without importing user data."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(os.name == 'nt', 'Windows launcher')
class FirstRunChecks(unittest.TestCase):
    def test_first_run_bindings(self):
        signature = (ROOT/'scripts/install-player.ps1').read_text().split('$ErrorActionPreference', 1)[0]
        launcher = (ROOT/'scripts/launch.ps1').read_text()
        for redirected in (False, True):
            with self.subTest(redirected=redirected), tempfile.TemporaryDirectory(prefix='GT2 first run ') as folder:
                root = Path(folder)
                scripts = root/'scripts'
                scripts.mkdir()
                (scripts/'launch.ps1').write_text(launcher.replace('[Console]::IsInputRedirected', '$true' if redirected else '$false'))
                (scripts/'install-player.ps1').write_text(signature + '''
if ($Target -ne 'PC' -or $DiscImage -or $InstallDir -eq '-HdMedia') { throw 'Incorrect first-run parameters' }
''' + ("if ($HdMedia -ne 'Original' -or !$NoBios) { throw 'Incorrect unattended options' }\n" if redirected else "if ($NoBios) { throw 'Interactive BIOS option changed' }\n") + "Write-Output 'FIRST_RUN_BINDING_OK'; exit 0\n")
                result = subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(scripts/'launch.ps1')], capture_output=True, text=True)
                self.assertIn('FIRST_RUN_BINDING_OK', result.stdout, result.stdout + result.stderr)
                self.assertNotIn('Incorrect', result.stderr)

if __name__ == '__main__':
    unittest.main()
