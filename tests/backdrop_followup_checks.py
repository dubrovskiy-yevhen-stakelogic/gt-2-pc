"""Bounded GPU follow-up must never cherry-pick a passing repetition."""
import importlib.util
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import recheck_backdrop_gpu as followup

class FollowupChecks(unittest.TestCase):
    def test_retained_capture_matches_corpus_layout(self):
        self.assertEqual(followup.retained_capture(Path('corpus/report.json'), 56, 'no_name_dirt', 0),
                         Path('corpus/056-no_name_dirt/capture-0/0.gt2cap.gz'))

    def test_requires_three_successes(self):
        self.assertTrue(followup.disposition([{'passed':True}]*3))
        for runs in ([],[{'passed':True}]*2,[{'passed':True}]*4,
                     [{'passed':True},{'passed':False},{'passed':True}]):
            self.assertFalse(followup.disposition(runs))

    def test_tail_regression_fails_even_when_mean_improves(self):
        samples=[dict(deferred_sky=v,mean_ms=6 if v else 7,p95_ms=10 if v else 7.1) for v in [False,True,True,False]]
        self.assertFalse(followup.summarize(samples)['passed'])
        with self.assertRaises(RuntimeError):followup.summarize(samples[:3])

if __name__=='__main__':unittest.main()
