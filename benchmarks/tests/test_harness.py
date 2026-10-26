import asyncio
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from harness.spec import manifest, matrix, payload, choose_index, SpecError
from harness.http_io import Client, Origin, ResponseError


class Contracts(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
