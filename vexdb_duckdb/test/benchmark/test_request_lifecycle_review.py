"""Checks that missing work and failed quality cannot disappear in reporting."""
import csv
import json
from pathlib import Path
import tempfile
import unittest
from review_request_lifecycle import review


class ReviewTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = self.root / "workload.cfg"
        self.config.write_text("rate=2\nseconds=1\nquery_count=2\nclients=1\nshape=broad\nprofile=0\nrequest_budget_ms=1\nout=" + str(self.root) + "\n")
        self.truth = self.root / "truth.json"
        self.truth.write_text(json.dumps({"text-new": {"broad-acl:0": {"ids": list(range(10))}}}))
        self.rows = [dict(ordinal=0, client=0, query=0, scheduled_ns=0, bind_begin_ns=1000,
                         execute_begin_ns=2000, execute_end_ns=500000, read_end_ns=600000,
                         thread_cpu_ns=400000, status="ok", ids=" ".join(map(str, range(10))), operators=0),
                     dict(ordinal=1, client=0, query=1, scheduled_ns=500000000, bind_begin_ns=502000000,
                         execute_begin_ns=0, execute_end_ns=0, read_end_ns=502000000,
                         thread_cpu_ns=0, status="expired_before_start", ids="", operators=0)]

    def write(self):
        with (self.root / "requests.tsv").open("w") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(self.rows[0]), delimiter="\t")
            writer.writeheader()
            writer.writerows(self.rows)
        (self.root / "summary.json").write_text(json.dumps(dict(offered=2, completed=1,
            expired_before_start=1, errors_or_not_started=0)))

    def test_expired_work_is_in_denominator(self):
        self.write()
        result = review(self.config, self.truth)
        self.assertEqual(result["completed_recall"], 1)
        self.assertEqual(result["offered_neighbor_delivery_ratio"], .5)
        self.assertEqual(result["deadline_miss_fraction"], .5)
        self.assertFalse(result["all_offered_completed_within_budget"])
        self.assertFalse(result["speedup_claim_allowed"])

    def test_missing_request_rejected(self):
        self.rows.pop()
        self.write()
        with self.assertRaises(AssertionError):
            review(self.config, self.truth)

    def test_wrong_ids_fail_quality(self):
        self.rows[0]["ids"] = " ".join(map(str, range(1, 11)))
        self.write()
        self.assertFalse(review(self.config, self.truth)["completed_quality_passed"])

    def test_moved_arrival_rejected(self):
        self.rows[1]["scheduled_ns"] += 1
        self.write()
        with self.assertRaises(AssertionError):
            review(self.config, self.truth)

    def test_expired_query_must_not_have_executed(self):
        self.rows[1]["execute_begin_ns"] = 501000000
        self.write()
        with self.assertRaises(AssertionError):
            review(self.config, self.truth)


if __name__ == "__main__":
    unittest.main()
