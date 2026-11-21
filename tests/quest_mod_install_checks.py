"""Run the player/Quest installers with prepared datasets and a device-free ADB double."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ADB = r'''import hashlib,json,os,sys,shutil
from pathlib import Path
root=Path(os.environ['GT2_TEST_REMOTE'])
a=sys.argv[1:]
if a==['devices']:
 print('List of devices attached\nTESTQUEST\tdevice');sys.exit(0)
assert a[:2]==['-s','TESTQUEST'],a
a=a[2:]
def remote(s):
 prefix='/sdcard/Android/data/io.github.gt2pc.quest/files/'
 assert s.startswith(prefix),s
 return root/s[len(prefix):]
if a[0]=='install':pass
elif a[:3]==['shell','am','broadcast']:
 print('Broadcast completed: result=0, data="files"')
elif a[0]=='push':
 if a[1]=='--sync':a.pop(1)
 src=Path(a[1]);dst=remote(a[2])
 if src.is_dir():shutil.copytree(src,dst/src.name,dirs_exist_ok=True)
 else:
  dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
elif a[:2]==['shell','sha256sum']:
 print(hashlib.sha256(remote(a[2]).read_bytes()).hexdigest()+'  '+a[2])
elif a[:2]==['shell','dumpsys']:pass
else:raise RuntimeError(a)
'''

@unittest.skipUnless(os.name == 'nt', 'Windows player installer')
class QuestModInstallChecks(unittest.TestCase):
    def install(self, folders, direct=None):
        with tempfile.TemporaryDirectory(prefix='gt2 mod install ') as temp:
            base=Path(temp); package=base/'package'; scripts=package/'scripts'
            scripts.mkdir(parents=True); (package/'tools').mkdir()
            data=base/'runtime'; data.mkdir(); remote=base/'remote'; remote.mkdir()
            for folder in folders:
                target=data/folder; (target/'assets').mkdir(parents=True)
                (target/'disc.raw2352').write_bytes(folder.encode())
                (target/'assets/.carcolor').write_bytes(b'asset')
            save=remote/'saves/simulation-plus/card1.mcd'
            save.parent.mkdir(parents=True); save.write_bytes(b'existing save')
            for name in ('install-player.ps1','install-quest.ps1'):
                shutil.copy2(ROOT/'scripts'/name,scripts/name)
            (scripts/'install.ps1').write_text('param($DiscImage,$InstallDir,$BuildDir,[switch]$NoBuild,$HdMedia,$Bios,$CaptureCore)\n')
            (scripts/'platform-tools.ps1').write_text('function Get-Adb { return $Adb }\n')
            for name in ('launch.ps1','existing-install.ps1','transfer-saves.ps1'):
                (scripts/name).write_text('')
            for name in ('gt2game.exe','openxr_loader.dll','gt2install.exe','gt2checks.exe'):
                (package/'tools'/name).write_bytes(b'fixture')
            apk=package/'GT2-VR-0.9.2.apk'; apk.write_bytes(b'apk')
            files=[{'path':p.relative_to(package).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in package.rglob('*') if p.is_file()]
            (package/'release-manifest.json').write_text(json.dumps({'version':'0.9.2','files':files}))
            (base/'adb.py').write_text(ADB)
            adb=base/'adb.cmd'; adb.write_text(f'@echo off\n"{sys.executable}" "{base / "adb.py"}" %*\nexit /b %errorlevel%\n')
            env=os.environ.copy(); env['GT2_TEST_REMOTE']=str(remote)
            env['PSModulePath']=str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/Modules')
            command=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File']
            if direct is None:
                command += [str(scripts/'install-player.ps1'),'-Target','Quest','-InstallDir',str(data),'-NoBios','-HdMedia','Original']
            else:
                command += [str(scripts/'install-quest.ps1'),'-Runtime',str(data),'-Apk',str(apk),*direct]
            run=subprocess.run(command+['-Adb',str(adb)],env=env,capture_output=True,text=True,timeout=90)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            for folder in folders:
                self.assertEqual((remote/folder/'disc.raw2352').read_bytes(),folder.encode())
                self.assertEqual((remote/folder/'assets/.carcolor').read_bytes(),b'asset')
            self.assertEqual(save.read_bytes(),b'existing save')
            return (remote/'launch-mode.txt').read_text()

    def test_mod_only_player_install(self):
        self.assertEqual(self.install(['simulation-aspec']), 'simulation-aspec')

    def test_retail_and_mod_player_install(self):
        self.assertEqual(self.install(['arcade','simulation','combined-plus','simulation-aspec']), 'combined-plus:simulation')

    def test_combined_only_player_install(self):
        self.assertEqual(self.install(['combined-plus']), 'combined-plus:simulation')

    def test_legacy_both_discs(self):
        self.assertEqual(self.install(['arcade','simulation'],['-BothDiscs']), 'arcade')

    def test_direct_mod_install(self):
        self.assertEqual(self.install(['combined-plus'],['-Mode','combined-plus']), 'combined-plus:simulation')

if __name__ == '__main__':
    unittest.main()
