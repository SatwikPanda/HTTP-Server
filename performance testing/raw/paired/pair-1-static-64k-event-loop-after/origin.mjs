// Controlled, local benchmark origin. It is not part of the production server.
import http from 'node:http';

const bytes = Number(process.argv[2] ?? 65536);
if (!Number.isInteger(bytes) || bytes < 1 || bytes > 16777216) process.exit(2);
const body = Buffer.alloc(bytes, 97);
let requests = 0;
let validations = 0;
const server = http.createServer((request, response) => {
    if (request.url === '/__stats') {
        response.writeHead(200, { 'Content-Type': 'application/json' });
        response.end(JSON.stringify({ requests, validations, bytes }));
        return;
    }
    ++requests;
    const headers = {
        'Content-Type': 'application/octet-stream',
        'Content-Length': bytes,
        'Cache-Control': 'public, max-age=3600',
        'ETag': '"perf-' + bytes + '"',
    };
    if (request.headers['if-none-match'] === headers.ETag) {
        ++validations;
        delete headers['Content-Length'];
        response.writeHead(304, headers);
        response.end();
        return;
    }
    response.writeHead(200, headers);
    response.end(request.method === 'HEAD' ? undefined : body);
});
server.listen(0, '127.0.0.1', () => {
    console.log('Listening on : 127.0.0.1:' + server.address().port);
});
