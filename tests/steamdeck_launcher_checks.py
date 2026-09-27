"""Run the desktop launch script with a fake file picker and Flatpak process."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASH = os.environ.get('GT2_TEST_BASH') or shutil.which('bash')

class LauncherChecks(unittest.TestCase):
    def setUp(self):
        if not BASH:
            self.skipTest('bash not found')
        self.temp = tempfile.TemporaryDirectory(prefix='gt2 launcher ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'bin').mkdir()
        for name in ['PLAY-STEAMDECK.sh', 'IMPORT-DISC-STEAMDECK.sh']:
            shutil.copy2(ROOT / name, self.root / name)
        (self.root / 'disc with spaces.iso').write_text('synthetic image')
        self.env = dict(os.environ, GT2_TEST_ROOT=self.root.as_posix())
        self.tool('kdialog', '''case "$*" in
  *--getopenfilename*)
    printf 'picker\\n' >> "$GT2_TEST_ROOT/picker"
    [ "${TEST_CANCEL:-0}" = 1 ] && exit 1
    [ "${TEST_EMPTY:-0}" = 1 ] && exit 0
    printf '%s/disc with spaces.iso\\n' "$GT2_TEST_ROOT" ;;
  *--error*) printf '%s\\n' "$*" >> "$GT2_TEST_ROOT/errors" ;;
esac
exit 0''')
        self.tool('flatpak', '''[ "$1" = run ] || exit 99
number=0
[ -f "$GT2_TEST_ROOT/count" ] && read -r number < "$GT2_TEST_ROOT/count"
number=$((number+1))
printf '%s\\n' "$number" > "$GT2_TEST_ROOT/count"
printf '%s\\n' "$@" > "$GT2_TEST_ROOT/call-$number"
if [ "$number" = 1 ] && [ "${TEST_REQUEST:-0}" = 1 ]; then exit 42; fi
exit "${TEST_STATUS:-0}"''')

    def tool(self, name, body):
        p = self.root / 'bin' / name
        p.write_text('#!/bin/bash\n' + body + '\n', encoding='utf-8', newline='\n')
        p.chmod(0o755)

    def run_launcher(self, import_disc=False):
        bootstrap = '''if command -v cygpath >/dev/null; then
export GT2_TEST_ROOT="$(cygpath -u "$GT2_TEST_ROOT")"
fi
export PATH="$GT2_TEST_ROOT/bin:$PATH"
exec /bin/bash "$GT2_TEST_ROOT/$1"'''
        script = 'IMPORT-DISC-STEAMDECK.sh' if import_disc else 'PLAY-STEAMDECK.sh'
        self.result = subprocess.run([BASH, '--noprofile', '--norc', '-c', bootstrap, 'test', script],
                                     env=self.env, text=True, capture_output=True, timeout=20)
        return self.result.returncode

    def test_installed_game_runs_without_picker(self):
        self.assertEqual(self.run_launcher(), 0, self.result.stderr)
        self.assertFalse((self.root / 'picker').exists())
        args = (self.root / 'call-1').read_text().splitlines()
        self.assertEqual(args, ['run', '--user', '--env=GT2_HOST_DISC_PICKER=1', '--command=gt2launcher', 'io.github.gt2pc.GT2'])

    def test_first_launch_or_overlay_request_uses_host_picker(self):
        self.env['TEST_REQUEST'] = '1'
        self.assertEqual(self.run_launcher(), 0, self.result.stderr)
        self.assertTrue((self.root / 'picker').exists())
        args = (self.root / 'call-2').read_text().splitlines()
        self.assertEqual(args[-2], '--import')
        self.assertTrue(args[-1].endswith('/disc with spaces.iso'))
        self.assertIn('--filesystem=' + args[-1] + ':ro', args)
        self.assertEqual(len(args), 8)

    def test_explicit_import_skips_initial_game_launch(self):
        self.assertEqual(self.run_launcher(True), 0, self.result.stderr)
        self.assertIn('--import', (self.root / 'call-1').read_text().splitlines())
        self.assertFalse((self.root / 'call-2').exists())

    def test_cancel_never_starts_import(self):
        self.env['TEST_CANCEL'] = '1'
        self.assertEqual(self.run_launcher(True), 0)
        self.assertFalse((self.root / 'call-1').exists())

    def test_empty_accepted_path_shows_error(self):
        self.env['TEST_EMPTY'] = '1'
        self.assertNotEqual(self.run_launcher(True), 0)
        self.assertIn('empty path', (self.root / 'errors').read_text())
        self.assertFalse((self.root / 'call-1').exists())

    def test_game_failure_is_visible(self):
        self.env['TEST_STATUS'] = '17'
        self.assertNotEqual(self.run_launcher(), 0)
        self.assertIn('error 17', (self.root / 'errors').read_text())

if __name__ == '__main__':
    unittest.main()
