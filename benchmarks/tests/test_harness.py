import asyncio
import sys
import unittest
import copy
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from harness.spec import manifest, matrix, payload, choose_index, validate_result, SpecError
from harness.http_io import Client, Origin, ResponseError
from harness.measure import run_phase
from harness.runner import check_origin
from harness.process import OwnedProcess, profile, command


class Contracts(unittest.TestCase):
    def test_validation_conflict_counts_are_bounded(self):
        scenario = {'cache': 'revalidation', 'concurrency': 2}
        measurement = {'counts': {'successful': 4}}
        delta = dict(requests=6, responses_304=4, responses_200=2, errors=0, rejected=0)
        check_origin(scenario, measurement, delta)
        with self.assertRaises(SpecError):
            check_origin(scenario, measurement, dict(delta, requests=9, responses_200=5))
        with self.assertRaises(SpecError):
            check_origin(dict(scenario, concurrency=1), measurement, delta)
    def test_manifest_and_matrix(self):
        spec = manifest(ROOT / "scenarios.json")
        points = matrix(spec)
        self.assertTrue(any("operator connection limit" in point["excluded"] for point in points))
        self.assertEqual({point["concurrency"] for point in points}, {1, 10, 50, 100, 500, 1000})
        self.assertEqual({point["object_bytes"] for point in points if point["family"] != "demo-smoke"}, set(spec["axes"]["object_bytes"]))
        self.assertEqual({point["workers"] for point in points}, {1, 2, 4, 8, 16})
        self.assertLess(len(points), 500)

    def test_payload_and_ordering(self):
        all_bytes = payload(1729, 3, 0, 1024)
        self.assertEqual(all_bytes[27:249], payload(1729, 3, 27, 222))
        self.assertNotEqual(all_bytes, payload(1728, 3, 0, 1024))
        sequence = [choose_index(1729, index, 16, "zipf") for index in range(1000)]
        self.assertEqual(sequence, [choose_index(1729, index, 16, "zipf") for index in range(1000)])
        self.assertGreater(sequence.count(0), sequence.count(15))

    def test_unsupported_profile_is_rejected(self):
        selected = profile(ROOT / 'profiles/demo.json')
        with self.assertRaises(SpecError):
            command(Path('server'), selected, dict(application='proxy', driver='event-loop'), ROOT)


class Network(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        spec = manifest(ROOT / "scenarios.json")
        self.scenario = dict(spec["families"][0], seed=1729, object_bytes=4097, timeout=1)
        self.origin = await Origin(self.scenario, 4).start()
        self.client = Client(self.origin.endpoint, None, 1729, 4097)

    async def asyncTearDown(self):
        await self.client.close()
        await self.origin.close()
        self.assertFalse(self.origin.tasks)

    async def test_persistent_get_head_and_counts(self):
        self.assertEqual(await self.client.request(0), 4097)
        self.assertEqual(await self.client.request(0), 4097)
        self.assertEqual(await self.client.request(0, "HEAD"), 0)
        self.assertEqual(self.origin.counts["requests"], 3)

    async def test_wrong_truncated_and_head_body_rejected(self):
        for fault in ("wrong", "truncated", "head-body"):
            self.origin.fault = fault
            with self.assertRaises(ResponseError):
                await self.client.request(0, "HEAD" if fault == "head-body" else "GET")
            self.assertIsNone(self.client.writer)

    async def test_failed_responses_never_count_as_throughput(self):
        self.origin.fault = 'wrong'
        scenario = dict(self.scenario, application='server', concurrency=1, arrival='closed',
                        rate=10, queue_limit=1, reuse=True)
        result = await run_phase(scenario, self.origin.endpoint, None, .03, 4)
        self.assertEqual(result['counts']['successful'], 0)
        self.assertEqual(result['counts']['useful_bytes'], 0)
        self.assertEqual(result['counts']['errors'], result['counts']['attempted'])

    async def test_open_arrival_overload_is_recorded(self):
        self.origin.scenario['origin_delay_ms'] = 50
        scenario = dict(self.scenario, application='server', concurrency=1, arrival='open',
                        rate=1000, queue_limit=1, reuse=True)
        result = await run_phase(scenario, self.origin.endpoint, None, .04, 100)
        self.assertGreater(result['counts']['overload_dropped'], 0)
        self.assertEqual(result['counts']['attempted'], result['counts']['successful'] + result['counts']['errors'])
        self.assertLessEqual(result['peak_queued_requests'], 1)
        self.assertLessEqual(result['peak_active_requests'], 1)

    async def test_raw_schema_rejects_corrupt_counts_and_histograms(self):
        scenario = dict(self.scenario, application='server', concurrency=1, arrival='closed',
                        rate=10, queue_limit=1, reuse=True)
        phase = await run_phase(scenario, self.origin.endpoint, None, .03, 2)
        record = dict(schema_version=1, run_id='a' * 32, repeat=1, evidence='smoke', state='complete',
                      scenario=scenario, provenance={}, environment={}, processes=[], phases={'measurement': phase}, diagnostics=[])
        validate_result(record)
        corrupt = copy.deepcopy(record)
        corrupt['phases']['measurement']['counts']['successful'] += 1
        with self.assertRaises(SpecError):
            validate_result(corrupt)
        corrupt = copy.deepcopy(record)
        corrupt['phases']['measurement']['success_latency']['counts'][0] = -1
        with self.assertRaises(SpecError):
            validate_result(corrupt)

    async def test_owned_process_bounded_output_and_cleanup(self):
        child = await OwnedProcess([sys.executable, '-u', '-c',
            'import time; print("x" * 70000, flush=True); time.sleep(30)']).start()
        try:
            for _ in range(100):
                if child.truncated:
                    break
                await asyncio.sleep(.01)
            self.assertTrue(child.truncated)
            self.assertLessEqual(len(child.tail), 65536)
        finally:
            await child.stop()
        self.assertIsNotNone(child.process.returncode)

    async def test_cancellation_closes_active_clients(self):
        self.origin.scenario['origin_delay_ms'] = 500
        scenario = dict(self.scenario, application='server', concurrency=2, arrival='closed',
                        rate=10, queue_limit=1, reuse=True)
        task = asyncio.create_task(run_phase(scenario, self.origin.endpoint, None, 10, 100))
        await asyncio.sleep(.03)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        await self.origin.close()
        self.assertFalse(self.origin.writers)


if __name__ == "__main__":
    unittest.main()
