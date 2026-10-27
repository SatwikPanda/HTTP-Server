"""Controlled origin and validating client. All sockets are local and bounded."""
import asyncio
import re
import time
from urllib.parse import urlsplit

from .spec import payload

HEAD_LIMIT = 16384
CHUNK = 65536


class ResponseError(ValueError):
    pass


class Client:
    def __init__(self, endpoint, origin, seed, size, reuse=True, receive_rate=0, demo=False):
        self.endpoint, self.origin = endpoint, origin
        self.seed, self.size, self.reuse = seed, size, reuse
        self.receive_rate, self.demo = receive_rate, demo
        self.reader = self.writer = None
        self.response_wire = self.request_wire = 0

    async def close(self):
        writer, self.writer, self.reader = self.writer, None, None
        if writer:
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), 1)
            except (OSError, asyncio.TimeoutError):
                pass

    async def exact(self, count):
        try:
            value = await self.reader.readexactly(count)
        except asyncio.IncompleteReadError as error:
            self.response_wire += len(error.partial)
            raise ResponseError("truncated response") from error
        self.response_wire += len(value)
        return value

    async def line(self):
        try:
            value = await self.reader.readuntil(b"\r\n")
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError) as error:
            self.response_wire += len(getattr(error, "partial", b""))
            raise ResponseError("invalid or oversized response line") from error
        self.response_wire += len(value)
        if len(value) > HEAD_LIMIT:
            raise ResponseError("response line limit")
        return value

    async def request(self, index, method="GET", cache_only=False, expected_status=200):
        self.response_wire = self.request_wire = 0
        try:
            if not self.writer:
                self.reader, self.writer = await asyncio.open_connection(*self.endpoint, limit=HEAD_LIMIT)
            path = f"/object/{index}.bin"
            authority = f"{(self.origin or self.endpoint)[0]}:{(self.origin or self.endpoint)[1]}"
            target = f"http://{authority}{path}" if self.origin else path
            close = not self.reuse or method == "HEAD"  # EOF proves HEAD has no body.
            wire = (f"{method} {target} HTTP/1.1\r\nHost: {authority}\r\nConnection: {'close' if close else 'keep-alive'}\r\n" +
                    ("Cache-Control: only-if-cached\r\n" if cache_only else "") + "\r\n").encode("ascii")
            self.writer.write(wire)
            self.request_wire = len(wire)  # submitted bytes, not TCP acknowledgements
            await self.writer.drain()
            line = await self.line()
            match = re.fullmatch(rb"HTTP/1\.[01] ([0-9]{3}) [^\r\n]*\r\n", line)
            if not match:
                raise ResponseError("invalid status line")
            status = int(match[1])
            headers, header_bytes = {}, len(line)
            while True:
                line = await self.line()
                header_bytes += len(line)
                if header_bytes > HEAD_LIMIT:
                    raise ResponseError("response head limit")
                if line == b"\r\n":
                    break
                name, separator, value = line[:-2].partition(b":")
                if not separator or not re.fullmatch(rb"[!#$%&'*+.^_`|~0-9A-Za-z-]+", name):
                    raise ResponseError("invalid header")
                if any(byte < 32 and byte != 9 or byte == 127 for byte in value):
                    raise ResponseError("invalid header value")
                headers.setdefault(name.lower(), []).append(value.strip())
            if status != expected_status:
                raise ResponseError(f"unexpected status {status}, expected {expected_status}")
            if b"content-encoding" in headers:
                raise ResponseError("unexpected content encoding")
            length = headers.get(b"content-length", [])
            transfer = headers.get(b"transfer-encoding", [])
            if (length and transfer) or len(length) > 1 or (length and not length[0].isdigit()):
                raise ResponseError("ambiguous response framing")
            if transfer and transfer != [b"chunked"]:
                raise ResponseError("unsupported transfer encoding")
            representation = len(b"Hello, World!\r\n") if self.demo else self.size
            # Cache-only negative probes are correctness setup, never useful work.
            expected = representation if status == 200 else None
            if method == "HEAD":
                if transfer or (length and int(length[0]) != representation):
                    raise ResponseError("incorrect HEAD metadata")
                extra = await self.reader.read(1)
                self.response_wire += len(extra)
                if extra:
                    raise ResponseError("HEAD response contains a body")
                await self.close()
                return 0
            offset = 0

            async def accept(data):
                nonlocal offset
                if expected is not None:
                    wanted = b"Hello, World!\r\n"[offset:offset + len(data)] if self.demo else payload(self.seed, index, offset, len(data))
                    if offset + len(data) > expected or data != wanted:
                        raise ResponseError("representation content mismatch")
                elif offset + len(data) > HEAD_LIMIT:
                    raise ResponseError("probe error body limit")
                offset += len(data)
                if self.receive_rate:
                    await asyncio.sleep(len(data) / self.receive_rate)

            if transfer:
                while True:
                    raw = (await self.line())[:-2].split(b";", 1)[0]
                    if not re.fullmatch(rb"[0-9A-Fa-f]{1,16}", raw):
                        raise ResponseError("invalid chunk size")
                    remaining = int(raw, 16)
                    if remaining == 0:
                        trailer_bytes = 0
                        while True:
                            trailer = await self.line()
                            trailer_bytes += len(trailer)
                            if trailer_bytes > HEAD_LIMIT:
                                raise ResponseError("trailer limit")
                            if trailer == b"\r\n":
                                break
                            if b":" not in trailer:
                                raise ResponseError("invalid trailer")
                        break
                    if remaining > (expected if expected is not None else HEAD_LIMIT) - offset:
                        raise ResponseError("body exceeds expected size")
                    while remaining:
                        data = await self.exact(min(CHUNK, remaining))
                        remaining -= len(data)
                        await accept(data)
                    if await self.exact(2) != b"\r\n":
                        raise ResponseError("invalid chunk terminator")
            elif length:
                remaining = int(length[0])
                if expected is not None and remaining != expected:
                    raise ResponseError("representation length mismatch")
                if remaining > (expected if expected is not None else HEAD_LIMIT):
                    raise ResponseError("response length limit")
                while remaining:
                    data = await self.exact(min(CHUNK, remaining))
                    remaining -= len(data)
                    await accept(data)
            else:
                close = True
                while True:
                    data = await self.reader.read(CHUNK)
                    self.response_wire += len(data)
                    if not data:
                        break
                    await accept(data)
            if expected is not None and offset != expected:
                raise ResponseError("incomplete representation")
            if close or b"close" in b",".join(headers.get(b"connection", [])).lower().split(b","):
                # Receiving the body does not prove that the server finished
                # its transaction (including publishing a proxy cache fill).
                # The caller's request deadline also bounds this EOF barrier.
                extra = await self.reader.read(1)
                self.response_wire += len(extra)
                if extra:
                    raise ResponseError("unexpected bytes after response")
                await self.close()
            return offset if status == 200 else 0
        except BaseException:
            await self.close()
            raise


class Origin:
    """Local async fixture with bounded connections; deliberately independent of C++ CHECKs."""
    def __init__(self, scenario, max_clients):
        self.scenario, self.max_clients = scenario, max_clients
        self.server = None
        self.tasks, self.writers = set(), set()
        self.counts = dict(requests=0, responses_200=0, responses_304=0, errors=0, rejected=0, peak_connections=0)
        self.fault = None  # tests only: wrong, truncated, head-body

    async def start(self):
        self.server = await asyncio.start_server(self.handle, "127.0.0.1", 0, limit=HEAD_LIMIT)
        self.endpoint = self.server.sockets[0].getsockname()[:2]
        return self

    def snapshot(self):
        return dict(self.counts)

    async def close(self):
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        for writer in tuple(self.writers):
            writer.close()
        for task in tuple(self.tasks):
            task.cancel()
        if self.tasks:
            await asyncio.wait_for(asyncio.gather(*tuple(self.tasks), return_exceptions=True), 3)

    async def handle(self, reader, writer):
        task = asyncio.current_task()
        self.tasks.add(task)
        if len(self.writers) >= self.max_clients:
            self.counts["rejected"] += 1
            writer.close()
            self.tasks.discard(task)
            return
        self.writers.add(writer)
        self.counts["peak_connections"] = max(self.counts["peak_connections"], len(self.writers))
        scenario = self.scenario
        try:
            while True:
                try:
                    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), scenario["timeout"])
                except asyncio.IncompleteReadError as error:
                    if not error.partial:
                        break  # readiness probe or normally closed persistent peer
                    raise
                if len(head) > HEAD_LIMIT:
                    raise ResponseError("origin request head limit")
                lines = head.decode("ascii").split("\r\n")
                method, target, version = lines[0].split(" ")
                if method not in {"GET", "HEAD"} or version != "HTTP/1.1":
                    raise ResponseError("origin request syntax")
                fields = {}
                for field in lines[1:-2]:
                    key, value = field.split(":", 1)
                    fields[key.lower()] = value.strip()
                matched = re.fullmatch(r"/object/([0-9]+)\.bin", urlsplit(target).path)
                if not matched or int(matched[1]) >= scenario["objects"]:
                    raise ResponseError("origin object path")
                index = int(matched[1])
                self.counts["requests"] += 1
                await asyncio.sleep(scenario["origin_delay_ms"] / 1000)
                tag = f'"bench-{scenario["seed"]}-{index}"'
                unchanged = fields.get("if-none-match") == tag
                from email.utils import formatdate
                control = "no-cache" if scenario["cache"] == "revalidation" else "max-age=3600"
                status = "304 Not Modified" if unchanged else "200 OK"
                close = fields.get("connection", "").lower() == "close"
                header = f"HTTP/1.1 {status}\r\nDate: {formatdate(usegmt=True)}\r\nETag: {tag}\r\nCache-Control: {control}\r\n"
                if not unchanged:
                    header += f'Content-Length: {scenario["object_bytes"]}\r\n'
                header += f"Connection: {'close' if close else 'keep-alive'}\r\n\r\n"
                writer.write(header.encode("ascii"))
                self.counts["responses_304" if unchanged else "responses_200"] += 1
                if not unchanged and (method == "GET" or self.fault == "head-body"):
                    size = scenario["object_bytes"] - (1 if self.fault == "truncated" else 0)
                    offset = 0
                    while offset < size:
                        count = min(CHUNK, size - offset)
                        data = payload(scenario["seed"], index, offset, count)
                        rate = scenario["origin_bytes_per_second"]
                        if rate:
                            await asyncio.sleep(count / rate)
                        writer.write(b"!" * count if self.fault == "wrong" else data)
                        await asyncio.wait_for(writer.drain(), scenario["timeout"])
                        offset += count
                        await asyncio.sleep(0)
                await asyncio.wait_for(writer.drain(), scenario["timeout"])
                if close or self.fault == "truncated":
                    break
        except (OSError, ValueError, asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            self.counts["errors"] += 1
        finally:
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), 1)
            except (OSError, asyncio.TimeoutError):
                pass
            self.writers.discard(writer)
            self.tasks.discard(task)
