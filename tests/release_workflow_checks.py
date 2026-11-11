"""Regress manual release builds whose game tag predates the CI APT helper."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASH = os.environ.get('GT2_TEST_BASH') or shutil.which('bash')
WORKFLOW = (ROOT / '.github/workflows/release.yml').read_text()
JOBS = dict(re.findall(r'^  ([a-z][a-z0-9_-]*):\n(.*?)(?=^  [a-z][a-z0-9_-]*:|\Z)',
                       WORKFLOW, re.M | re.S))


class TaggedBuildChecks(unittest.TestCase):
    def test_all_apt_jobs_keep_workflow_tools_separate(self):
        jobs = {name: body for name, body in JOBS.items() if 'apt-install.sh' in body}
        self.assertEqual(set(jobs), {'linux', 'android', 'web', 'steamdeck'})
        for name, body in jobs.items():
            with self.subTest(job=name):
                workflow_ref = body.index('ref: ${{ github.workflow_sha }}')
                preserve = body.index('install -m 644 scripts/apt-install.sh')
                game_ref = body.index("ref: ${{ inputs.tag != ''")
                use = body.index('bash "$RUNNER_TEMP/gt2-apt-install.sh"')
                self.assertLess(workflow_ref, preserve)
                self.assertLess(preserve, game_ref)
                self.assertLess(game_ref, use)
                self.assertNotIn('bash scripts/apt-install.sh', body)

    def test_old_tag_without_helper_still_installs_dependencies(self):
        if not BASH or not shutil.which('git'):
            self.skipTest('bash and git are required')
        with tempfile.TemporaryDirectory(prefix='gt2 tagged build ') as temp:
            base = Path(temp)
            repo = base / 'repo'
            runner = base / 'runner temp'
            stubs = base / 'stubs'
            for folder in (repo, runner, stubs):
                folder.mkdir()

            def git(*args):
                return subprocess.run(['git', '-C', str(repo), *args], check=True,
                                      capture_output=True, text=True).stdout.strip()

            git('init')
            git('config', 'user.name', 'Release workflow test')
            git('config', 'user.email', 'workflow-test@example.invalid')
            (repo / 'game.txt').write_text('old release\n')
            git('add', '.')
            git('commit', '-m', 'old game tag without CI helper')
            git('tag', 'old-release')
            (repo / 'scripts').mkdir()
            shutil.copy2(ROOT / 'scripts/apt-install.sh', repo / 'scripts/apt-install.sh')
            (repo / 'game.txt').write_text('newer development game\n')
            git('add', '.')
            git('commit', '-m', 'workflow revision with CI helper')
            env = dict(os.environ, RUNNER_TEMP=runner.as_posix(),
                       GT2_APT_LOG=(base / 'apt.log').as_posix(),
                       PATH=str(stubs) + os.pathsep + os.environ['PATH'])
            for name, code in {
                'sudo': 'exec "$@"',
                'timeout': 'shift; exec "$@"',
                'apt-get': 'printf "%s\\n" "$*" >> "$GT2_APT_LOG"',
            }.items():
                tool = stubs / name
                tool.write_text('#!/bin/bash\n' + code + '\n', newline='\n')
                tool.chmod(0o755)
            preserve = re.search(r'^          (install -m 644 scripts/apt-install.sh .+)$',
                                 WORKFLOW, re.M)[1]
            subprocess.run([BASH, '-c', preserve], cwd=repo, env=env, check=True)
            git('checkout', '--force', 'old-release')
            self.assertFalse((repo / 'scripts/apt-install.sh').exists())
            self.assertEqual((repo / 'game.txt').read_text(), 'old release\n')
            # Exercise the real retry helper with stand-ins, never the host's APT.
            command = 'bash "$RUNNER_TEMP/gt2-apt-install.sh" cmake ninja-build'
            subprocess.run([BASH, '-c', command], cwd=repo, env=env, check=True)
            log = (base / 'apt.log').read_text().splitlines()
            self.assertEqual(log, ['-o Acquire::Retries=3 update',
                                  '-o Acquire::Retries=3 install --yes --no-install-recommends cmake ninja-build'])


if __name__ == '__main__':
    unittest.main()
