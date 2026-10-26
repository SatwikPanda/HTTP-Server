import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "benchmarks"), str(ROOT / "tools")]
sys.dont_write_bytecode = True
from harness.report import merged_latency, summarize, write_report
from harness.spec import SpecError
from qualify import check_junit, check_inventory


def fixture(index=1):
    histogram = dict(upper_bounds_us=[1, 10, 100, None], counts=[0, 1, 0, 0], count=1)
    phase = dict(counts=dict(attempted=1, issued=1, successful=1, errors=0, timeouts=0,
        overload_dropped=0, useful_bytes=1024, response_wire_bytes=1200, request_wire_bytes=60),
        duration_seconds=1, success_latency=copy.deepcopy(histogram), dispatch_latency=copy.deepcopy(histogram),
        queue_delay=copy.deepcopy(histogram), requested_issue_window_seconds=1)
    return dict(schema_version=1, run_id=f"{index:032x}", repeat=index, evidence="candidate", state="complete",
        scenario=dict(id="hot-file", driver="event-loop", workers=1, concurrency=1, object_bytes=1024, reuse=True),
        provenance=dict(candidate_source_sha256="source", binary_sha256="binary"), environment={}, processes=[],
        phases={"measurement": phase}, timing=dict(warmup_seconds=0, measurement_seconds=1), diagnostics=[])


class Results(unittest.TestCase):
    def test_percentiles_pool_counts_instead_of_averaging_percentiles(self):
        first, second = fixture(), fixture(2)
        phase = second["phases"]["measurement"]
        phase["success_latency"].update(counts=[0, 0, 99, 0], count=99)
        merged = merged_latency([first["phases"]["measurement"], phase])
        self.assertEqual(merged["count"], 100)
        self.assertEqual(merged["percentile_upper_bounds_us"], dict(p50=100, p95=100, p99=100))
        phase["success_latency"]["upper_bounds_us"] = [1, 11, 100, None]
        with self.assertRaises(SpecError): merged_latency([first["phases"]["measurement"], phase])

    def test_failure_and_incomplete_repeat_sets_are_not_qualified(self):
        repeats = [fixture(i) for i in range(1, 6)]
        repeats[-1]["state"] = "failed"
        repeats[-1]["phases"] = {}  # Startup failures belong to the intended configuration.
        rows = summarize(repeats)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["failed_repeats"], 1)
        self.assertFalse(rows[0]["comparison_eligible"])
        with self.assertRaises(SpecError): summarize([fixture(), fixture()])

    def test_incompatible_settings_and_sources_stay_separate(self):
        records = [fixture(i) for i in range(1, 5)]
        records[1]["scenario"]["workers"] = 2
        records[2]["provenance"]["binary_sha256"] = "other"
        records[3]["timing"]["measurement_seconds"] = 2
        self.assertEqual(len(summarize(records)), 4)

    def test_raw_files_remain_immutable_and_report_is_reproducible(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for i in range(1, 6):
                destination = root / "raw" / str(i)
                destination.mkdir(parents=True)
                (destination / "result.json").write_text(json.dumps(fixture(i)), encoding="utf-8")
            first = write_report(root / "raw", root / "first", plots=False)
            second = write_report(root / "raw", root / "second", plots=False)
            self.assertEqual(first, second)
            self.assertTrue(first["provisional"])
            self.assertTrue((root / "first/repeats.csv").is_file())

    def test_missing_inventory_or_skipped_runtime_cannot_pass(self):
        with self.assertRaises(ValueError): check_inventory({"tests": []})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.xml"
            for content in ('<testsuite/>', '<testsuite><testcase name="real" status="notrun"><skipped/></testcase></testsuite>',
                            '<testsuite><testcase name="real" status="run"><failure/></testcase></testsuite>'):
                path.write_text(content)
                with self.assertRaises(ValueError): check_junit(path, ["real"])
            path.write_text('<testsuite><testcase name="real" status="run" time="1.2"/></testsuite>')
            self.assertTrue(check_junit(path, ["real"])[0]["passed"])


if __name__ == "__main__": unittest.main()
