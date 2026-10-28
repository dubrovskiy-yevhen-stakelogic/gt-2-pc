"""Exercise dependency extraction in Windows PowerShell 5.1 with legacy path limits."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
POWERSHELL = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


@unittest.skipUnless(os.name == 'nt', 'Windows PowerShell extraction regression')
class DependencyZipChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='gt2-zip-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)

    def extract(self, archive, destination, move_to=None):
        script = self.base / 'extract.ps1'
        # These switches affect this child process only; no registry or machine
        # policy is changed. Test the path-limited .NET Framework configuration.
        code = """$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5){throw 'Windows PowerShell 5.1 required'}
$switches=[IO.Path].Assembly.GetType('System.AppContextSwitches')
foreach($field in @('_useLegacyPathHandling','_blockLongPaths')) {
    $switches.GetField($field,[Reflection.BindingFlags]'Static,NonPublic').SetValue($null,1)
}
"""
        code += '. ' + quote(ROOT/'scripts/windows-prerequisites.ps1') + '\n'
        code += 'Expand-Gt2Dependency ' + quote(archive) + ' ' + quote(destination) + '\n'
        if move_to:
            code += '[Gt2DependencyZip]::MoveDirectory(' + quote(destination) + ', ' + quote(move_to) + ')\n'
        code += '$PSVersionTable.PSVersion.ToString()\n'
        script.write_text(code, encoding='utf-8-sig')
        return subprocess.run([str(POWERSHELL), '-NoProfile', '-ExecutionPolicy', 'Bypass',
                               '-File', str(script)], capture_output=True)

    def test_long_sdk_paths_and_unicode(self):
        relative = ('sdk/8.0.408/DotnetTools/dotnet-watch/8.0.408-servicing.25168.25/tools/net8.0/any/'
                    'BuildHost-net472/Microsoft.CodeAnalysis.Workspaces.MSBuild.BuildHost.exe.config')
        destination = self.base / ('profile with spaces-' + 'x'*35) / ('dotnet.preparing-' + 'a'*32)
        archive = self.base/'sdk.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr(relative, b'long SDK file')
            z.writestr('unicode-\u00fc/hello.txt', b'unicode filename')
        self.assertGreater(len(str(destination/relative)), 260)
        installed = self.base / ('profile with spaces-' + 'x'*35) / 'dotnet-8.0.408'
        run = self.extract(archive, destination, installed)
        self.assertEqual(run.returncode, 0, run.stderr.decode(errors='replace'))
        self.assertIn(b'5.1.', run.stdout)
        self.assertFalse(destination.exists())
        self.assertEqual(Path('\\\\?\\' + str(installed/relative).replace('/', '\\')).read_bytes(), b'long SDK file')
        self.assertEqual((installed/'unicode-\u00fc/hello.txt').read_bytes(), b'unicode filename')

    def test_reject_traversal_before_any_write(self):
        for index, bad in enumerate(('../escape.txt', '/absolute.txt', 'C:/escape.txt', 'ok/../../escape.txt')):
            with self.subTest(path=bad):
                archive = self.base/f'unsafe{index}.zip'
                destination = self.base/f'unsafe{index}'
                with zipfile.ZipFile(archive, 'w') as z:
                    z.writestr('first.txt', b'must not be written')
                    z.writestr(bad, b'unsafe')
                self.assertNotEqual(self.extract(archive, destination).returncode, 0)
                self.assertFalse((destination/'first.txt').exists())
                self.assertFalse((self.base/'escape.txt').exists())

    def test_does_not_overwrite_existing_files(self):
        archive = self.base/'existing.zip'
        destination = self.base/'existing'
        destination.mkdir()
        (destination/'file.txt').write_bytes(b'keep this')
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('file.txt', b'replacement')
        self.assertNotEqual(self.extract(archive, destination).returncode, 0)
        self.assertEqual((destination/'file.txt').read_bytes(), b'keep this')


if __name__ == '__main__':
    unittest.main()
