"""Exercise the real shell entry points with local stand-ins for desktop/Flatpak tools."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASH = os.environ.get('GT2_TEST_BASH') or shutil.which('bash')


class InstallerChecks(unittest.TestCase):
    def setUp(self):
        if not BASH:
            self.skipTest('bash is not installed')
        self.tmp = tempfile.TemporaryDirectory(prefix='gt2 installer ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for name in ('INSTALL-STEAMDECK.sh', 'BUILD-STEAMDECK.sh', 'PLAY-STEAMDECK.sh', 'IMPORT-DISC-STEAMDECK.sh'):
            shutil.copy2(ROOT / name, self.root / name)
        self.bin = self.root / 'test-bin'
        self.bin.mkdir()
        self.env = dict(os.environ, GT2_TEST_ROOT=self.root.as_posix(),
                        PATH=str(self.bin) + os.pathsep + os.environ['PATH'])
        # Do not change the real user's HOME or invoke system Flatpak.
        self.tool('uname', 'if [ "$1" = -s ]; then echo Linux; else echo x86_64; fi')
        self.tool('xdg-user-dir', 'printf "%s/Desktop\\n" "$GT2_TEST_ROOT"')
        self.tool('kdialog', 'printf "%s\\n" "$*" >> "$GT2_TEST_ROOT/dialogs"')
        self.tool('konsole', 'printf "%s\\n" "$@" > "$GT2_TEST_ROOT/terminal-args"')
        self.tool('flatpak-builder', 'exec flatpak run org.flatpak.Builder "$@"')
        self.tool('flatpak', '''printf '%s\n' "$*" >> "$GT2_TEST_ROOT/flatpak-calls"
case "$1" in
  remote-add) exit "${GT2_TEST_REMOTE_STATUS:-0}" ;;
  info)
    case "$*" in
      *org.flatpak.Builder*) exit "${GT2_TEST_BUILDER_INFO_STATUS:-0}" ;;
      *) exit 0 ;;
    esac ;;
  install)
    case "$*" in
      *org.flatpak.Builder*) exit "${GT2_TEST_SDK_STATUS:-0}" ;;
      *) exit "${GT2_TEST_INSTALL_STATUS:-0}" ;;
    esac ;;
  build-bundle)
    if [ "${GT2_TEST_NO_BUNDLE:-0}" = 0 ]; then printf 'test package' > "$4"; fi ;;
  run)
    case " $* " in
      *' --disable-rofiles-fuse '*) exit "${GT2_TEST_BUILD_STATUS:-0}" ;;
      *) echo 'rofiles-fuse mounting forbidden' >&2; exit 31 ;;
    esac ;;
  *) exit 97 ;;
esac''')

    def tool(self, name, body):
        path = self.bin / name
        path.write_text('#!/bin/bash\nset -eu\n' + body + '\n', encoding='utf-8', newline='\n')
        path.chmod(0o755)

    def run_installer(self, *args, terminal=True):
        bootstrap = '''if command -v cygpath >/dev/null; then
  export GT2_TEST_ROOT="$(cygpath -u "$GT2_TEST_ROOT")"
fi
export PATH="$GT2_TEST_ROOT/test-bin:$PATH"
export XDG_CACHE_HOME="$GT2_TEST_ROOT/internal cache"
export XDG_DATA_HOME="$GT2_TEST_ROOT/user data"
exec /bin/bash "$@"'''
        command = [BASH, '--noprofile', '--norc', '-c', bootstrap, 'test', str(self.root / 'INSTALL-STEAMDECK.sh')]
        if terminal:
            command.append('--gt2-terminal')
        result = subprocess.run(command + list(args), env=self.env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        self.output = result.stdout
        return result.returncode

    def calls(self):
        path = self.root / 'flatpak-calls'
        return path.read_text(encoding='utf-8') if path.exists() else ''

    def test_execute_opens_persistent_terminal_without_starting_hidden_install(self):
        self.assertEqual(self.run_installer(terminal=False), 0, self.output)
        args = (self.root / 'terminal-args').read_text(encoding='utf-8').splitlines()
        self.assertEqual(args[:4], ['--separate', '--hold', '-e', '/bin/bash'])
        self.assertTrue(args[4].endswith('/INSTALL-STEAMDECK.sh'))
        self.assertEqual(args[5], '--gt2-terminal')
        self.assertEqual(self.calls(), '')

    def test_source_install_downloads_dependencies_builds_and_creates_shortcut(self):
        self.assertEqual(self.run_installer(), 0, self.output)
        calls = self.calls()
        self.assertIn('org.freedesktop.Sdk//25.08', calls)
        self.assertIn('run --user --filesystem=', calls)
        self.assertIn('--disable-rofiles-fuse', calls)
        self.assertIn('build-bundle', calls)
        self.assertIn('install --user --noninteractive --assumeyes ', calls)
        self.assertIn('GT2 INSTALLED', self.output)
        self.assertIn('Exec=/bin/bash', (self.root / 'Desktop/GT2.desktop').read_text())
        self.assertEqual((self.root / 'user data/gt2-launcher/PLAY-STEAMDECK.sh').read_bytes(), (ROOT / 'PLAY-STEAMDECK.sh').read_bytes())
        self.assertTrue((self.root / 'GT2-SteamDeck-install.log').exists())

    def test_sandbox_builder_uses_internal_cache_without_fuse(self):
        self.assertEqual(self.run_installer(), 0, self.output)
        calls = self.calls()
        self.assertIn('/internal cache/gt2/steamdeck-build/builder-state', calls)
        self.assertIn('/internal cache/gt2/steamdeck-build/repo', calls)
        self.assertIn('--filesystem=', calls)
        self.assertIn('--disable-rofiles-fuse', calls)
        self.assertFalse((self.root / 'work').exists())
        self.assertTrue(list((self.root / 'internal cache/gt2/steamdeck-build').glob('run.*/GT2-0.7.0-steamdeck.flatpak')))

    def test_native_builder_fallback_also_disables_fuse(self):
        self.env['GT2_TEST_BUILDER_INFO_STATUS'] = '1'
        self.assertEqual(self.run_installer(), 0, self.output)
        self.assertIn('run org.flatpak.Builder --disable-rofiles-fuse', self.calls())

    def test_existing_bundle_skips_sdk_and_compilation(self):
        (self.root / 'GT2-0.7.0-steamdeck.flatpak').write_bytes(b'test package')
        self.assertEqual(self.run_installer(), 0, self.output)
        self.assertNotIn('org.flatpak.Builder', self.calls())
        self.assertNotIn('build-bundle', self.calls())

    def test_download_failure_is_visible_and_stops_build(self):
        self.env['GT2_TEST_SDK_STATUS'] = '23'
        self.assertEqual(self.run_installer(), 23, self.output)
        self.assertIn('INSTALLATION STOPPED', self.output)
        self.assertNotIn('build-bundle', self.calls())
        self.assertFalse((self.root / 'Desktop/GT2.desktop').exists())

    def test_build_failure_never_installs_old_package(self):
        self.env['GT2_TEST_BUILD_STATUS'] = '24'
        self.assertEqual(self.run_installer(), 24, self.output)
        self.assertIn('Compiling and checking GT2', self.output)
        self.assertNotIn('GT2 INSTALLED', self.output)
        self.assertFalse((self.root / 'Desktop/GT2.desktop').exists())

    def test_missing_build_output_is_not_success(self):
        self.env['GT2_TEST_NO_BUNDLE'] = '1'
        self.assertNotEqual(self.run_installer(), 0, self.output)
        self.assertIn('INSTALLATION STOPPED', self.output)
        self.assertNotIn('GT2 INSTALLED', self.output)

    def test_install_failure_never_creates_shortcut(self):
        (self.root / 'GT2-0.7.0-steamdeck.flatpak').write_bytes(b'test package')
        self.env['GT2_TEST_INSTALL_STATUS'] = '25'
        self.assertEqual(self.run_installer(), 25, self.output)
        self.assertIn('Installing GT2', self.output)
        self.assertNotIn('GT2 INSTALLED', self.output)
        self.assertFalse((self.root / 'Desktop/GT2.desktop').exists())


if __name__ == '__main__':
    unittest.main()
