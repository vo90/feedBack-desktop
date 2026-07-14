// Contract-surface extraction for the audio engine TLC refactor (Phase 0.a,
// docs/audio-engine-tlc.md §4). Each extractor returns a sorted, stable JSON
// snapshot of one public surface. contract-check.test.js diffs these against
// the committed snapshots so a decomposition phase cannot silently change the
// public API. Regenerate deliberately with: node tests/contracts/extract.js
'use strict';

const fs = require('fs');
const path = require('path');

const repoRoot = path.join(__dirname, '..', '..');

// Export table of slopsmith_audio.node. Loads the real binary; returns null
// when it hasn't been built (contract-check skips with a warning then).
function extractAddonExports() {
    const addonPath = path.join(repoRoot, 'build', 'Release', 'slopsmith_audio.node');
    if (!fs.existsSync(addonPath)) return null;
    const addon = require(addonPath);
    return Object.keys(addon).sort();
}

// Every ipcMain.handle / ipcMain.on channel registered in audio-bridge.ts.
function extractIpcChannels() {
    const src = fs.readFileSync(path.join(repoRoot, 'src', 'main', 'audio-bridge.ts'), 'utf8');
    const channels = new Set();
    const re = /ipcMain\.(?:handle|on)\(\s*'([^']+)'/g;
    let m;
    while ((m = re.exec(src)) !== null) channels.add(m[1]);
    return [...channels].sort();
}

// Top-level method keys of the `audio:` and `audioEffects:` object literals in
// preload.ts — the surface the renderer (and every plugin) programs against.
// Brace-depth walk: record keys only at depth 1 inside the target literal.
function extractPreloadKeys(objectName, src) {
    const start = src.indexOf(`${objectName}: {`);
    if (start === -1) throw new Error(`preload.ts: '${objectName}: {' not found`);
    let i = src.indexOf('{', start);
    let depth = 0;
    let parenDepth = 0; // multi-line parameter lists must not yield keys
    const keys = [];
    let lineStart = i;
    for (; i < src.length; i++) {
        const c = src[i];
        if (c === '{') depth++;
        else if (c === '}') {
            depth--;
            if (depth === 0) break;
        } else if (c === '(') parenDepth++;
        else if (c === ')') parenDepth--;
        else if (c === '\n') {
            lineStart = i + 1;
        } else if (depth === 1 && parenDepth === 0) {
            // At a key position: line begins (after whitespace) with `name:`
            if (i === lineStart) {
                const line = src.slice(lineStart, src.indexOf('\n', lineStart));
                const km = line.match(/^\s*([A-Za-z_$][\w$]*)\s*:/);
                if (km) keys.push(km[1]);
            }
        }
    }
    return keys.sort();
}

function extractPreloadApi() {
    const src = fs.readFileSync(path.join(repoRoot, 'src', 'main', 'preload.ts'), 'utf8');
    return {
        audio: extractPreloadKeys('audio', src),
        audioEffects: extractPreloadKeys('audioEffects', src),
    };
}

function writeSnapshot(name, data) {
    fs.writeFileSync(path.join(__dirname, name), JSON.stringify(data, null, 2) + '\n');
}

module.exports = { extractAddonExports, extractIpcChannels, extractPreloadApi };

if (require.main === module) {
    const addonExports = extractAddonExports();
    if (addonExports) writeSnapshot('addon-exports.json', addonExports);
    else console.warn('addon not built — skipping addon-exports.json');
    writeSnapshot('ipc-channels.json', extractIpcChannels());
    writeSnapshot('preload-audio-api.json', extractPreloadApi());
    console.log('contract snapshots written to tests/contracts/');
}
