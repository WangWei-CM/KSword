#!/usr/bin/env node
'use strict';

// npm 与 pip 使用同一 Python 包；bootstrap 的所有输出走 stderr，stdout 专供 MCP。
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync, spawn} = require('node:child_process');
const metadata = require('../package.json');
const packageRoot = path.resolve(__dirname, '..');

function pythonCandidates(environment = process.env, platform = process.platform) {
  // 显式解释器优先，其他候选只走 PATH，不扫描磁盘或改变系统 Python。
  if (environment.KSWORD_MCP_PYTHON) return [[environment.KSWORD_MCP_PYTHON, []]];
  return platform === 'win32' ? [['py', ['-3']], ['python', []], ['python3', []]] : [['python3', []], ['python', []]];
}

function findPython(run = spawnSync, environment = process.env, platform = process.platform) {
  // 从实际运行的解释器读取完整路径，拒绝不满足 Python 版本的候选。
  const probe = 'import json,sys;print(json.dumps({"executable":sys.executable,"version":list(sys.version_info[:2])}))';
  for (const [command, prefix] of pythonCandidates(environment, platform)) {
    const result = run(command, [...prefix, '-c', probe], {encoding: 'utf8', windowsHide: true, timeout: 10000});
    if (result.error || result.status !== 0) continue;
    try {
      const value = JSON.parse(result.stdout.trim());
      if (path.isAbsolute(value.executable) && value.version[0] === 3 && value.version[1] >= 10) return value.executable;
    } catch (_) {}
  }
  throw new Error('Python 3.10+ is required. Install Python or set KSWORD_MCP_PYTHON to its full path.');
}

function runtimePaths(environment = process.env, platform = process.platform) {
  // 版本化的用户缓存仅保存本包独立虚拟环境，避免改动客户端或系统依赖。
  const cache = environment.KSWORD_MCP_CACHE_DIR || path.join(os.homedir(), '.cache', 'ksword-mcp');
  if (!path.isAbsolute(cache)) throw new Error('KSWORD_MCP_CACHE_DIR must be an absolute path.');
  const root = path.join(cache, metadata.version);
  return {root, python: path.join(root, platform === 'win32' ? 'Scripts' : 'bin', platform === 'win32' ? 'python.exe' : 'python'),
          ready: path.join(root, 'ksword-mcp-ready.json')};
}

function runSetup(command, args) {
  // 安装不能消费 MCP stdin，也不能在 stdout 混入 pip/venv 诊断。
  const result = spawnSync(command, args, {stdio: ['ignore', 2, 2], windowsHide: true, timeout: 180000});
  if (result.error || result.status !== 0) throw new Error('KSword MCP isolated runtime setup failed.');
}

function isReady(runtime) {
  // 仅采纳完成安装后写入的本版本标记，损坏或其它版本的标记不表示可用。
  try {
    return fs.statSync(runtime.python).isFile() && JSON.parse(fs.readFileSync(runtime.ready, 'utf8')).version === metadata.version;
  } catch (_) { return false; }
}

async function prepare() {
  const runtime = runtimePaths();
  if (isReady(runtime)) return runtime;
  fs.mkdirSync(path.dirname(runtime.root), {recursive: true});
  const lock = runtime.root + '.lock';
  const deadline = Date.now() + 180000;
  let descriptor;
  // 并发启动只允许一个安装者；不删除其它进程的锁或已存在的运行时。
  while (descriptor === undefined) {
    try { descriptor = fs.openSync(lock, 'wx', 0o600); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      if (isReady(runtime)) return runtime;
      if (Date.now() >= deadline) throw new Error('KSword MCP setup lock is busy. Check the installer before removing ' + lock);
      await new Promise(resolve => setTimeout(resolve, 250));
    }
  }
  try {
    if (isReady(runtime)) return runtime;
    const basePython = findPython();
    runSetup(basePython, ['-m', 'venv', runtime.root]);
    runSetup(runtime.python, ['-m', 'pip', 'install', '--disable-pip-version-check', '--no-input', packageRoot]);
    // 成功标记最后写入；业务调用不会因为安装中断而自动重发。
    fs.writeFileSync(runtime.ready, JSON.stringify({version: metadata.version}), {encoding: 'utf8', mode: 0o600});
    return runtime;
  } finally {
    fs.closeSync(descriptor);
    fs.unlinkSync(lock);
  }
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length === 1 && args[0] === '--version') {
    process.stdout.write(metadata.version + '\n');
    return;
  }
  const runtime = await prepare();
  if (args.length === 1 && args[0] === '--prepare') {
    process.stderr.write('KSword MCP isolated runtime is ready.\n');
    return;
  }
  // 不通过 shell 传递路径/参数；子进程是实际 MCP server，继承 stdio 协议通道。
  const child = spawn(runtime.python, ['-I', '-m', 'ksword_mcp', ...args], {stdio: 'inherit', windowsHide: true});
  child.on('error', () => { process.stderr.write('Could not start KSword MCP.\n'); process.exitCode = 1; });
  child.on('exit', (code) => { process.exitCode = code === null ? 1 : code; });
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => child.kill(signal));
}

module.exports = {pythonCandidates, findPython, runtimePaths, isReady};
if (require.main === module) {
  main().catch(error => { process.stderr.write(error.message + '\n'); process.exitCode = 1; });
}
