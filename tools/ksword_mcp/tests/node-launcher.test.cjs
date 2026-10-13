'use strict';
// 解释器选择与完成标记验证，不安装依赖或调用产品。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const {pythonCandidates, findPython, runtimePaths, isReady} = require('../bin/ksword-mcp.cjs');
const {version} = require('../package.json');

test('explicit Python is not split into shell arguments', () => {
  const executable = path.resolve('Python with spaces', 'python.exe');
  assert.deepEqual(pythonCandidates({KSWORD_MCP_PYTHON: executable}, 'win32'), [[executable, []]]);
  assert.deepEqual(pythonCandidates({}, 'win32')[0], ['py', ['-3']]);
});

test('probe checks actual executable and minimum Python version', () => {
  let count = 0;
  const actual = path.resolve('Python', 'python.exe');
  const run = (command, args, options) => {
    count++;
    assert.equal(options.windowsHide, true);
    return {status: 0, stdout: JSON.stringify({executable: actual, version: [3, count === 1 ? 9 : 10]})};
  };
  assert.equal(findPython(run, {}, 'win32'), actual);
  assert.equal(count, 2);
  assert.throws(() => findPython(() => ({status: 0, stdout: 'bad JSON'}), {}), /Python 3.10/);
});

test('runtime cache requires an absolute path and keeps versions isolated', () => {
  assert.throws(() => runtimePaths({KSWORD_MCP_CACHE_DIR: 'relative'}), /absolute/);
  const cache = path.resolve('runtime-cache');
  const runtime = runtimePaths({KSWORD_MCP_CACHE_DIR: cache}, 'win32');
  assert.equal(runtime.root, path.join(cache, version));
  assert.equal(runtime.python, path.join(cache, version, 'Scripts', 'python.exe'));
});

test('only a complete matching marker accepts a runtime', t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ksword-npm-tests-'));
  t.after(() => fs.rmSync(root, {recursive: true}));
  const runtime = {python: path.join(root, 'python.exe'), ready: path.join(root, 'ready.json')};
  fs.writeFileSync(runtime.python, 'test placeholder');
  assert.equal(isReady(runtime), false);
  fs.writeFileSync(runtime.ready, 'invalid');
  assert.equal(isReady(runtime), false);
  fs.writeFileSync(runtime.ready, JSON.stringify({version: 'other'}));
  assert.equal(isReady(runtime), false);
  fs.writeFileSync(runtime.ready, JSON.stringify({version}));
  assert.equal(isReady(runtime), true);
});

test('version uses clean stdout without bootstrapping', () => {
  const result = spawnSync(process.execPath, [path.resolve(__dirname, '../bin/ksword-mcp.cjs'), '--version'],
                           {encoding: 'utf8', env: {...process.env, KSWORD_MCP_PYTHON: 'must-not-execute'}});
  assert.equal(result.status, 0);
  assert.equal(result.stdout, version + '\n');
  assert.equal(result.stderr, '');
});
