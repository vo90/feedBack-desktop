'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { loadTs, ROOT } = require('./_load-ts');

const {
    normalizeDirectoryGrantPickerOptions,
} = loadTs('src/main/directory-grant-contract.ts');


test('directory grant picker accepts the documented owner/purpose contract', () => {
    assert.deepEqual(normalizeDirectoryGrantPickerOptions({
        owner: 'hybrid_track',
        purpose: 'library-root',
        // Older renderer code may still send these fields. They are ignored:
        // an untrusted same-origin script cannot control native dialog copy or
        // make Windows resolve a UNC/device default before user confirmation.
        title: 'Spoofed title',
        defaultPath: '\\\\attacker\\share',
    }), {
        owner: 'hybrid_track',
        purpose: 'library-root',
        title: 'Choose a song library for Hybrid Track',
    });
});


test('directory grant picker rejects forged or ambiguous bindings', () => {
    for (const value of [
        null,
        {},
        { owner: '../hybrid_track', purpose: 'library-root' },
        { owner: 'hybrid_track', purpose: 'library root' },
        { owner: 'another_plugin', purpose: 'library-root' },
        { owner: 'hybrid_track', purpose: 'another-purpose' },
    ]) {
        assert.throws(() => normalizeDirectoryGrantPickerOptions(value), TypeError);
    }
});


test('renderer-controlled native dialog paths and titles are never forwarded', () => {
    const source = fs.readFileSync(path.join(ROOT, 'src/main/main.ts'), 'utf8');
    assert.doesNotMatch(source, /defaultPath:\s*options\./);
    assert.match(source, /title:\s*options\.title/);
});


test('main process keeps the grant picker top-frame/origin scoped', () => {
    const source = fs.readFileSync(path.join(ROOT, 'src/main/main.ts'), 'utf8');
    assert.match(source, /event\.sender !== mainWindow\.webContents/);
    assert.match(source, /event\.senderFrame !== mainWindow\.webContents\.mainFrame/);
    assert.match(source, /makeRendererOriginPredicate\(port\)\(event\.senderFrame\.url\)/);
});


test('backend registration secret is fresh per spawn and never preload-exposed', () => {
    const python = fs.readFileSync(path.join(ROOT, 'src/main/python.ts'), 'utf8');
    const preload = fs.readFileSync(path.join(ROOT, 'src/main/preload.ts'), 'utf8');
    assert.match(python, /randomBytes\(32\)\.toString\('base64url'\)/);
    assert.match(python, /\[DIRECTORY_GRANT_SECRET_ENV\]: grantSecretForChild/);
    assert.doesNotMatch(preload, /DIRECTORY_GRANT_SECRET_(?:ENV|HEADER)/);
});
