'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const {
    DIRECTORY_GRANT_SECRET_HEADER,
    registerDirectoryGrantWithBackend,
} = require('./_load-ts').loadTs('src/main/directory-grant-client.ts');

const SECRET = 'd'.repeat(43);

async function loopbackServer(t, handler) {
    const server = http.createServer(handler);
    await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(0, '127.0.0.1', resolve);
    });
    t.after(() => new Promise((resolve) => server.close(resolve)));
    return server.address().port;
}


test('client registers only over loopback with the private header and exact body', async (t) => {
    let observed;
    const port = await loopbackServer(t, (request, response) => {
        const chunks = [];
        request.on('data', (chunk) => chunks.push(chunk));
        request.on('end', () => {
            observed = {
                remote: request.socket.remoteAddress,
                method: request.method,
                url: request.url,
                secret: request.headers[DIRECTORY_GRANT_SECRET_HEADER.toLowerCase()],
                body: JSON.parse(Buffer.concat(chunks).toString('utf8')),
            };
            response.writeHead(201, { 'Content-Type': 'application/json' });
            response.end(JSON.stringify({
                grant: 'G'.repeat(43),
                expiresAt: Date.now() + 120_000,
            }));
        });
    });

    const result = await registerDirectoryGrantWithBackend({
        port,
        secret: SECRET,
        owner: 'hybrid_track',
        purpose: 'library-root',
        directoryPath: 'C:\\External Songs',
    });

    assert.deepEqual(result, {
        grant: 'G'.repeat(43),
        expiresAt: result.expiresAt,
    });
    assert.deepEqual(observed, {
        remote: '127.0.0.1',
        method: 'POST',
        url: '/api/desktop/directory-grants',
        secret: SECRET,
        body: {
            owner: 'hybrid_track',
            purpose: 'library-root',
            path: 'C:\\External Songs',
        },
    });
});


test('client never reflects a rejected backend body containing sensitive data', async (t) => {
    const sensitive = 'C:\\Private Songs and backend diagnostic';
    const port = await loopbackServer(t, (_request, response) => {
        response.writeHead(403, { 'Content-Type': 'text/plain' });
        response.end(sensitive);
    });

    await assert.rejects(
        registerDirectoryGrantWithBackend({
            port,
            secret: SECRET,
            owner: 'hybrid_track',
            purpose: 'library-root',
            directoryPath: 'C:\\External Songs',
        }),
        (error) => {
            assert.match(error.message, /rejected by the backend \(403\)/);
            assert.doesNotMatch(error.message, /Private Songs|diagnostic/);
            return true;
        },
    );
});


test('client rejects malformed or expired success responses', async (t) => {
    const port = await loopbackServer(t, (_request, response) => {
        response.writeHead(201, { 'Content-Type': 'application/json' });
        response.end(JSON.stringify({ grant: 'predictable', expiresAt: Date.now() - 1 }));
    });

    await assert.rejects(
        registerDirectoryGrantWithBackend({
            port,
            secret: SECRET,
            owner: 'hybrid_track',
            purpose: 'library-root',
            directoryPath: 'C:\\External Songs',
        }),
        /invalid response/,
    );
});


test('client rejects a grant when the managed backend rotates in flight', async (t) => {
    let markRequestArrived;
    const requestArrived = new Promise((resolve) => { markRequestArrived = resolve; });
    let releaseResponse;
    const responseReleased = new Promise((resolve) => { releaseResponse = resolve; });
    const port = await loopbackServer(t, async (request, response) => {
        request.resume();
        markRequestArrived();
        await responseReleased;
        response.writeHead(201, { 'Content-Type': 'application/json' });
        response.end(JSON.stringify({
            grant: 'R'.repeat(43),
            expiresAt: Date.now() + 120_000,
        }));
    });

    let currentGeneration = 1;
    const pending = registerDirectoryGrantWithBackend({
        port,
        secret: SECRET,
        owner: 'hybrid_track',
        purpose: 'library-root',
        directoryPath: 'C:\\External Songs',
        isBackendCurrent: () => currentGeneration === 1,
    });
    await requestArrived;
    currentGeneration = 2;
    releaseResponse();

    await assert.rejects(pending, /backend changed/);
});
