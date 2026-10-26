"""Bounded closed/open-loop scheduling and aggregatable latency histograms."""
import asyncio
import bisect
import math
import time

from .http_io import Client, ResponseError
from .spec import choose_index

# Eight buckets per power of two (~9.1% width, integer rounding near 1 us).
BOUNDS = sorted({math.ceil(2 ** (index / 8)) for index in range(257)})


class Histogram:
    def __init__(self):
        self.counts = [0] * (len(BOUNDS) + 1)
        self.count = 0

    def add(self, seconds):
        self.counts[bisect.bisect_left(BOUNDS, max(0, seconds) * 1e6)] += 1
        self.count += 1

    def export(self):
        percentiles = {}
        for name, fraction in (("p50", .50), ("p95", .95), ("p99", .99)):
            total = 0
            percentiles[name] = None
            if not self.count:
                continue
            for index, count in enumerate(self.counts):
                total += count
                if total >= math.ceil(self.count * fraction):
                    percentiles[name] = BOUNDS[index] if index < len(BOUNDS) else None
                    break
        return dict(unit="microseconds", precision="8 buckets per doubling; integer upper bounds; final bucket overflow",
                    upper_bounds_us=BOUNDS + [None], counts=self.counts, count=self.count,
                    percentile_upper_bounds_us=percentiles)


async def run_phase(scenario, endpoint, origin, seconds, request_limit):
    start, cpu_start = time.perf_counter(), time.process_time()
    end = start + seconds
    counts = dict(attempted=0, issued=0, successful=0, errors=0, timeouts=0, overload_dropped=0,
                  transport_errors=0, validation_errors=0, useful_bytes=0, response_wire_bytes=0, request_wire_bytes=0)
    success, dispatch, queue_delay = Histogram(), Histogram(), Histogram()
    errors = []
    peak_queued = active = peak_active = delayed = 0
    max_issue_delay = 0.0
    sequence = 0

    async def issue(client, index, scheduled):
        nonlocal active, peak_active, delayed, max_issue_delay
        actual = time.perf_counter()
        lag = max(0, actual - scheduled)
        queue_delay.add(lag)
        max_issue_delay = max(max_issue_delay, lag)
        if lag > .001:
            delayed += 1
        active += 1
        peak_active = max(peak_active, active)
        remaining = scenario["timeout"] - lag
        try:
            if remaining <= 0:
                raise asyncio.TimeoutError()
            counts["issued"] += 1
            useful = await asyncio.wait_for(client.request(index, scenario["method"]), remaining)
            counts["successful"] += 1
            counts["useful_bytes"] += useful
            success.add(time.perf_counter() - scheduled)
        except asyncio.TimeoutError:
            counts["errors"] += 1
            counts["timeouts"] += 1
            await client.close()
        except (OSError, ResponseError) as error:
            counts["errors"] += 1
            counts["validation_errors" if isinstance(error, ResponseError) else "transport_errors"] += 1
            if len(errors) < 8:
                errors.append(str(error)[:256])
        finally:
            counts["response_wire_bytes"] += client.response_wire
            counts["request_wire_bytes"] += client.request_wire
            client.response_wire = client.request_wire = 0
            dispatch.add(time.perf_counter() - scheduled)
            active -= 1

    def client():
        return Client(endpoint, origin, scenario["seed"], scenario["object_bytes"], scenario["reuse"],
                      scenario["client_bytes_per_second"], scenario["application"] == "demo")

    def object_index(serial):
        return choose_index(scenario["seed"], serial, scenario["objects"], scenario["distribution"])

    async def closed_worker():
        nonlocal sequence
        connection = client()
        try:
            while time.perf_counter() < end and sequence < request_limit:
                serial = sequence
                sequence += 1
                counts["attempted"] += 1
                await issue(connection, object_index(serial), time.perf_counter())
        finally:
            await connection.close()

    queue = asyncio.Queue(maxsize=scenario["queue_limit"])

    async def open_worker():
        connection = client()
        try:
            while True:
                item = await queue.get()
                try:
                    if item is None:
                        return
                    await issue(connection, *item)
                finally:
                    queue.task_done()
        finally:
            await connection.close()

    tasks = [asyncio.create_task(closed_worker() if scenario["arrival"] == "closed" else open_worker())
             for _ in range(scenario["concurrency"])]
    try:
        if scenario["arrival"] == "open":
            while sequence < request_limit:
                scheduled = start + sequence / scenario["rate"]
                if scheduled >= end:
                    break
                await asyncio.sleep(max(0, scheduled - time.perf_counter()))
                counts["attempted"] += 1
                item = object_index(sequence), scheduled
                sequence += 1
                try:
                    queue.put_nowait(item)
                    peak_queued = max(peak_queued, queue.qsize())
                except asyncio.QueueFull:
                    counts["errors"] += 1
                    counts["overload_dropped"] += 1
            await queue.join()
            for _ in tasks:
                await queue.put(None)
        await asyncio.gather(*tasks)
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
    elapsed = max(time.perf_counter() - start, 1e-9)
    cpu = time.process_time() - cpu_start
    return dict(counts=counts, duration_seconds=elapsed, requested_issue_window_seconds=seconds,
                tail_drain_seconds=max(0, elapsed - seconds), request_limit_reached=sequence >= request_limit,
                successful_requests_per_second=counts["successful"] / elapsed,
                useful_bytes_per_second=counts["useful_bytes"] / elapsed,
                achieved_issue_rate=counts["issued"] / min(seconds, elapsed),
                success_latency=success.export(), dispatch_latency=dispatch.export(), queue_delay=queue_delay.export(),
                peak_active_requests=peak_active, peak_queued_requests=peak_queued,
                delayed_over_1ms=delayed, max_issue_delay_seconds=max_issue_delay,
                generator_cpu_seconds=cpu, generator_core_utilization=cpu / elapsed,
                saturation_suspected=cpu / elapsed >= .8 or counts["overload_dropped"] > 0 or
                    (scenario["arrival"] == "open" and max_issue_delay > .05),
                diagnostics=errors)
