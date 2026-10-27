"""The catalogue gate must reject omitted/duplicated views and partial imports."""
import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('windows',Path(__file__).resolve().parents[1]/'scripts/check_cockpit_windows.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
class CoverageChecks(unittest.TestCase):
    def setUp(self):
        self.rows=[dict(car='fixture',path='gt4',yaw=y,pitch=p) for y,p in runner.VIEWS]
        self.expected={('fixture','gt4')}
    def test_complete_views(self):
        runner.coverage(self.rows,self.expected,set())
    def test_missing_and_duplicate_views_fail(self):
        for rows in (self.rows[:-1],self.rows+[self.rows[0]],self.rows[:-1]+[self.rows[0]]):
            with self.assertRaises(RuntimeError):runner.coverage(rows,self.expected,set())
    def test_missing_model_is_accounted_for_but_has_no_fake_views(self):
        runner.coverage([],self.expected,self.expected)
        with self.assertRaises(RuntimeError):runner.coverage([],self.expected,set())
        with self.assertRaises(RuntimeError):runner.coverage(self.rows[:1],self.expected,self.expected)
    def test_unexpected_paths_fail(self):
        with self.assertRaises(RuntimeError):runner.coverage(self.rows,set(),set())
if __name__=='__main__':unittest.main()
