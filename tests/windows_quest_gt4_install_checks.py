"""Exercise the real Quest installer against an ADB test double; no device access."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from gt4_pack_install_checks import bundle

ROOT = Path(__file__).resolve().parents[1]
FAKE_ADB = r'''
import hashlib,json,os,sys,tarfile
from pathlib import Path
base=Path(os.environ['GT2_ADB_FIXTURE'])
args=sys.argv[1:]
with (base/'calls.jsonl').open('a') as f:f.write(json.dumps(args)+'\n')
fault=os.environ.get('GT2_ADB_FAULT','')
if args==['devices']:
    print('List of devices attached')
    if fault!='disconnected':print('TESTQUEST\tdevice')
    sys.exit(0)
assert args[:2]==['-s','TESTQUEST'],args
args=args[2:]
if args[:3]==['shell','pm','path']:
    print('package:/data/app/fixture/base.apk')
elif args[:3]==['shell','am','force-stop']:pass
elif args[:3]==['shell','am','broadcast']:
    if fault=='access' and any('VERIFY_DATA' in a for a in args):print('Broadcast completed: result=1')
    else: print('Broadcast completed: result=0, data="/sdcard/Android/data/io.github.gt2pc.quest/files"')
elif args[0]=='push':
    if fault=='transfer':sys.exit(12)
    (base/'payload.tar').write_bytes(Path(args[1]).read_bytes())
elif args[:2]==['shell','sha256sum']:
    digest=hashlib.sha256((base/'payload.tar').read_bytes()).hexdigest()
    print(('0'*64 if fault=='hash' else digest)+'  '+args[2])
elif args[:2]==['shell','tar']:
    with tarfile.open(base/'payload.tar') as archive:archive.extractall(base/'remote',filter='data')
elif args[0]=='shell' and 'sha256sum -s -c' in args[1]:
    if fault=='verify':sys.exit(13)
    inventory=base/'remote/modelsets/gt4/quest-install.sha256'
    for row in inventory.read_text().splitlines():
        digest,name=row.split('  ',1)
        assert hashlib.sha256((base/'remote'/name).read_bytes()).hexdigest()==digest
elif args[:2]==['shell','rm']:(base/'payload.tar').unlink()
else:raise RuntimeError('Unexpected ADB operation: '+repr(args))
'''


@unittest.skipUnless(os.name=='nt','Windows PowerShell Quest installer')
class QuestGt4InstallChecks(unittest.TestCase):
    def run_install(self,fault='',corrupt=False):
        temp=tempfile.TemporaryDirectory(prefix='gt2 quest transfer ')
        self.addCleanup(temp.cleanup)
        base=Path(temp.name)
        scripts=base/'scripts'
        scripts.mkdir()
        for name in ('install-quest.ps1','package-quest-gt4.py'):
            shutil.copy2(ROOT/'scripts'/name,scripts/name)
        (scripts/'windows-prerequisites.ps1').write_text('function Get-Gt2Python([string]$Python) { return $Python }\n')
        archive=base/'gt4-models.tar'
        bundle(archive,corrupt=corrupt)
        (base/'fake-adb.py').write_text('import shutil\n'+FAKE_ADB)
        adb=base/'adb.cmd'
        adb.write_text(f'@echo off\r\n"{sys.executable}" "{base / "fake-adb.py"}" %*\r\nexit /b %errorlevel%\r\n')
        save=base/'remote/saves/memory-card.mcd'
        save.parent.mkdir(parents=True)
        save.write_bytes(b'keep this save')
        env=os.environ.copy()
        env['PSModulePath'] = str(Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/Modules')
        env['GT2_ADB_FIXTURE']=str(base)
        env['GT2_ADB_FAULT']=fault
        run=subprocess.run([r'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(scripts/'install-quest.ps1'),'-Gt4Only','-Gt4Bundle',str(archive),'-Python',sys.executable,'-Adb',str(adb)],env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
        log=run.stdout+run.stderr
        calls=[json.loads(line) for line in (base/'calls.jsonl').read_text().splitlines()]
        self.assertEqual(save.read_bytes(),b'keep this save')
        self.assertFalse(any('install' in call or 'launch-mode.txt' in ' '.join(call) or 'disc.raw2352' in ' '.join(call) for call in calls))
        return base,run,log,calls

    def test_transfers_extracts_and_verifies_without_reinstalling_game(self):
        base,run,log,calls=self.run_install()
        self.assertEqual(run.returncode,0,log)
        self.assertIn('GT4 INSTALLATION COMPLETE ON QUEST',log)
        self.assertTrue((base/'remote/modelsets/gt4/native-v16/wheels.json').is_file())
        self.assertTrue(any('sha256sum -s -c' in ' '.join(call) for call in calls))

    def test_failed_transfer_or_verification_never_reports_success(self):
        for fault in ('transfer','hash','verify','access'):
            with self.subTest(fault=fault):
                base,run,log,calls=self.run_install(fault)
                self.assertNotEqual(run.returncode,0,log)
                self.assertNotIn('GT4 INSTALLATION COMPLETE',log)
                if fault in ('transfer','hash'):
                    self.assertFalse(any('tar' in call for call in calls))

    def test_disconnected_quest_does_not_transfer(self):
        base,run,log,calls=self.run_install('disconnected')
        self.assertNotEqual(run.returncode,0,log)
        self.assertEqual(calls,[['devices']])

    def test_corrupt_bundle_does_not_touch_installed_game(self):
        base,run,log,calls=self.run_install(corrupt=True)
        self.assertNotEqual(run.returncode,0,log)
        self.assertFalse(any('push' in call or 'force-stop' in call for call in calls))


if __name__=='__main__':
    unittest.main()
