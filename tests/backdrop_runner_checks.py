"""Data-free checks for the owned-disc GPU regression gate."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'backdrop_runner', Path(__file__).resolve().parents[1]/'scripts/check_backdrop_regression.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class BackdropRunnerChecks(unittest.TestCase):
    def test_requires_all_ordered_phases_and_gpu_samples(self):
        log = ''.join(f'Sky pair phase {i} deferred={int(i in (1, 2))}\n'
                      'GPU avg 7.3 ms p95 7.5 p99 7.6; CPU+GPU 8.0 ms\n' for i in range(4))
        self.assertEqual(len(runner.paired_samples(log)), 4)
        for broken in (log[:log.index('Sky pair phase 3')], log.replace('phase 1', 'phase 3'),
                       log.replace('GPU avg 7.3', 'GPU avg 0.0'), log.replace('GPU avg', 'GPU unavailable')):
            with self.assertRaises(RuntimeError):
                runner.paired_samples(broken)

    def test_expensive_sky_is_rejected(self):
        self.assertTrue(runner.regression(12.32, 7.36))

    def test_improvement_and_timer_noise_pass(self):
        self.assertFalse(runner.regression(7.36, 12.32))
        self.assertFalse(runner.regression(7.45, 7.36))

    def test_absolute_and_relative_allowances_are_both_applied(self):
        self.assertFalse(runner.regression(1.30, 1.0))
        self.assertTrue(runner.regression(1.31, 1.0))
        self.assertFalse(runner.regression(11.20, 10.0))
        self.assertTrue(runner.regression(11.21, 10.0))

    def test_paired_comparison_is_stricter(self):
        self.assertTrue(runner.regression(10.80, 10.0, 1.05))
        self.assertFalse(runner.regression(10.80, 10.0))


if __name__ == '__main__':
    unittest.main()
