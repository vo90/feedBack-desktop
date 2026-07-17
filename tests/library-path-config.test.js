'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { loadTs, ROOT } = require('./_load-ts');

const {
    normalizeExplicitLibraryPath,
    prepareLibraryPathForPython,
} = loadTs('src/main/library-path-config.ts');

function tmpConfigDir() {
    return fs.mkdtempSync(path.join(os.tmpdir(), 'feedback-library-path-'));
}

test('normal desktop startup bootstraps the fallback into config instead of DLC_DIR', () => {
    const configDir = tmpConfigDir();
    const result = prepareLibraryPathForPython(configDir, 'C:\\Music\\fee[dB]ack');

    assert.deepEqual(result, { status: 'bootstrapped' });
    assert.deepEqual(
        JSON.parse(fs.readFileSync(path.join(configDir, 'config.json'), 'utf8')),
        { dlc_dir: 'C:\\Music\\fee[dB]ack' },
    );
    assert.equal(result.environmentDlcDir, undefined);
});

test('bootstrap merges the fallback into an existing config without losing settings', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    fs.writeFileSync(configFile, JSON.stringify({ master_difficulty: 75 }));

    const result = prepareLibraryPathForPython(configDir, 'D:\\Songs');

    assert.equal(result.status, 'bootstrapped');
    assert.deepEqual(
        JSON.parse(fs.readFileSync(configFile, 'utf8')),
        { master_difficulty: 75, dlc_dir: 'D:\\Songs' },
    );
});

test('an existing saved library stays config-owned and can change between scans', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    fs.writeFileSync(configFile, JSON.stringify({ dlc_dir: 'D:\\Saved Songs' }));

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.deepEqual(result, { status: 'configured' });
    assert.deepEqual(
        JSON.parse(fs.readFileSync(configFile, 'utf8')),
        { dlc_dir: 'D:\\Saved Songs' },
    );
    assert.equal(result.environmentDlcDir, undefined);
});

test('an explicit valid DLC_DIR remains an environment override', () => {
    const configDir = tmpConfigDir();
    const managedSongs = path.join(configDir, 'Managed Songs');
    fs.mkdirSync(managedSongs);
    const result = prepareLibraryPathForPython(
        configDir,
        'C:\\Default Songs',
        ` ${managedSongs} `,
    );

    assert.deepEqual(result, {
        status: 'explicit-override',
        environmentDlcDir: managedSongs,
    });
    assert.equal(fs.existsSync(path.join(configDir, 'config.json')), false);
});

test('an explicit DLC_DIR rejects whitespace, files, and missing paths', () => {
    const root = tmpConfigDir();
    const file = path.join(root, 'not-a-directory');
    fs.writeFileSync(file, 'x');

    assert.equal(normalizeExplicitLibraryPath('   '), undefined);
    assert.equal(normalizeExplicitLibraryPath(file), undefined);
    assert.equal(normalizeExplicitLibraryPath(path.join(root, 'missing')), undefined);
});

test('an explicit DLC_DIR is trimmed before directory validation', () => {
    const root = tmpConfigDir();
    const directory = path.join(root, 'Managed Songs');
    fs.mkdirSync(directory);

    assert.equal(normalizeExplicitLibraryPath(`  ${directory}  `), directory);
});

test('a corrupt config is never overwritten during bootstrap', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    fs.writeFileSync(configFile, '{broken');

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.equal(result.status, 'invalid-config');
    assert.match(result.error, /JSON/);
    assert.equal(fs.readFileSync(configFile, 'utf8'), '{broken');
});

test('python startup does not pin its resolved fallback as DLC_DIR', () => {
    const source = fs.readFileSync(path.join(ROOT, 'src', 'main', 'python.ts'), 'utf8');

    assert.match(source, /normalizeExplicitLibraryPath\(process\.env\.DLC_DIR\)/);
    assert.doesNotMatch(source, /existsSync\(process\.env\.DLC_DIR\)/);
    assert.match(source, /prepareLibraryPathForPython\(configDir, dlcDir, explicitDlcDir\)/);
    assert.doesNotMatch(source, /DLC_DIR:\s*dlcDir/);
    assert.match(source, /delete pythonEnv\.DLC_DIR/);
});
