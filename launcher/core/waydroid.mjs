// The Linux side of the launcher: Android runs in Waydroid (scripts/waydroid.sh) instead of the Android Emulator.
// WaydroidRuntime answers the same questions as Runtime (core/runtime.mjs), with scripts/waydroid.sh doing what the
// PowerShell scripts do on Windows, and WaydroidEmulator fills the Emulator page from Linux tools.
import { spawn } from 'node:child_process';
import { readdirSync, statSync } from 'node:fs';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { Runtime, run, sha256, readableError, headsetProblem, validPackage, validUserId, validEyeSize, validRenderScale } from './runtime.mjs';
import { Emulator, redact } from './emulator.mjs';

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const exists = file => fs.access(file).then(() => true, () => false);

// adb's port on Waydroid's network bridge. The "port" the shared code passes around only says that Android answers;
// serial() has the real address.
const ADB_PORT = 5555;

// Refract's Android packages as scripts/waydroid.sh install puts them on Android (RUNTIME_APKS there).
export const waydroidPackages = [
  { package: 'com.refract.openxrruntime', label: 'Refract OpenXR runtime', apk: 'build-android-runtime-linux-arm64-v8a/refract-openxr-runtime-debug.apk' },
  { package: 'com.oculus.systemdriver', label: 'Refract XR driver', apk: 'build-android-runtime-linux-arm64-v8a/refract-systemdriver-debug.apk' },
  { package: 'com.oculus.horizon', label: 'Meta Platform stand-in', apk: 'build-platform-sdk-linux/refract-platform-debug.apk' },
];

// Where adb is: ADB, Android's SDK, or the PATH (Fedora's android-tools).
export async function findAdb(sdk) {
  const candidates = [process.env.ADB, sdk && path.join(sdk, 'platform-tools/adb'),
    ...(process.env.PATH || '').split(':').map(dir => dir && path.join(dir, 'adb')), path.join(os.homedir(), 'Android/Sdk/platform-tools/adb')].filter(Boolean);
  for (const file of candidates) if (await exists(file)) return file;
  return '';
}

// The PC's active OpenXR runtime manifest (what headset play goes through), or ''.
export async function activeOpenXr() {
  const files = [process.env.XR_RUNTIME_JSON, path.join(process.env.XDG_CONFIG_HOME || path.join(os.homedir(), '.config'), 'openxr/1/active_runtime.json'),
    '/etc/xdg/openxr/1/active_runtime.json'].filter(Boolean);
  for (const file of files) if (await exists(file)) return file;
  return '';
}
export function openXrName(file) {
  const target = file ? path.basename(file).toLowerCase() + ' ' + path.dirname(file).toLowerCase() : '';
  return !file ? '' : /steam/.test(target) ? 'SteamVR' : /monado/.test(target) ? 'Monado' : /wivrn/.test(target) ? 'WiVRn' : path.basename(file);
}

export class WaydroidRuntime extends Runtime {
  constructor(root, settings) { super(root, settings); this.address = ''; this.script = path.join(root, 'scripts/waydroid.sh'); this.adbPath = ''; }
  get python() { return 'python3'; }
  get guestPackages() { return waydroidPackages; }
  adbExe() { return this.adbPath || path.join(this.settings.sdk || '', 'platform-tools/adb'); }
  serial() { return this.address || `127.0.0.1:${ADB_PORT}`; }
  get port() { return ADB_PORT; }

  // `waydroid status` as { running, address }: the session's state and Android's address on the waydroid0 bridge.
  async session() {
    const text = await run('waydroid', ['status'], { timeout: 10000 }).catch(() => '');
    const field = name => text.match(new RegExp(`^${name}:\\s*(.+)$`, 'm'))?.[1].trim() || '';
    const address = field('IP address');
    return { running: field('Session') === 'RUNNING', address: /^[\d.]+$/.test(address) ? address : '' };
  }
  async findPort() {
    this.adbPath = await findAdb(this.settings.sdk);
    if (!this.adbPath) return null;
    const { running, address } = await this.session();
    if (!running || !address) return null;
    this.address = `${address}:${ADB_PORT}`;
    // adb does not retry a connection that failed once, so ask again each time.
    await run(this.adbExe(), ['connect', this.address], { timeout: 5000 }).catch(() => {});
    try { return (await run(this.adbExe(), ['-s', this.address, 'get-state'], { timeout: 3000 })).trim() === 'device' ? ADB_PORT : null; }
    catch { return null; }
  }
  async locate() { this.found = null; return this.findPort(); }
  // A session that is up while adb is not answering yet (Android still booting, or frozen: scripts/waydroid.sh start thaws it).
  async avdProcessPort() { return (await this.session()).running ? ADB_PORT : null; }
  async waitBoot(port, timeout = 240000) {
    for (const end = Date.now() + timeout; Date.now() < end; await sleep(2000)) {
      if ((await this.adbAt(port, ['shell', 'getprop', 'sys.boot_completed'], { timeout: 5000 }).catch(() => '')).trim() === '1') return;
    }
    throw new Error('Android did not finish starting. Run scripts/waydroid.sh check.');
  }
  async checkHeadset() {
    const bridge = path.join(this.root, 'build-linux/host-bridge/refract-host-bridge');
    if (!await exists(bridge)) throw new Error('The host bridge is not built. Open Settings > Setup.');
    try { await run(bridge, ['--probe-openxr'], { timeout: 60000 }); }
    catch (error) {
      if (/timed out/.test(error.message)) throw new Error('The PC VR runtime did not respond. Restart SteamVR, Monado or WiVRn and press Play again.');
      throw new Error(headsetProblem(error.message) || error.message);
    }
  }
  async ensure(update = () => {}) {
    update('Starting Android');
    this.adbPath = await findAdb(this.settings.sdk);
    if (!this.adbPath) throw new Error('adb was not found. Install android-tools (Fedora) or the Android SDK platform-tools.');
    this.started = true;
    const output = await run(this.script, ['start'], { timeout: 6 * 60 * 1000, env: { ADB: this.adbPath } });
    const address = output.trim().split(/\r?\n/).pop().trim();
    if (!/^[\d.]+:\d+$/.test(address)) throw new Error('Waydroid did not report an address for adb.');
    this.address = address;
    await this.prepare(update);
  }
  // Refract's packages on the running Android, the ones this checkout built (scripts/waydroid.sh install does the same).
  async prepare(update = () => {}) {
    for (const item of waydroidPackages) {
      const apk = path.join(this.root, item.apk);
      const local = await sha256(apk).catch(() => { throw new Error(`${item.label} is not built (${item.apk}). Open Settings > Setup.`); });
      const installed = (await this.adb(['shell', 'pm', 'path', item.package]).catch(() => '')).match(/^package:(\S*base\.apk)/m)?.[1];
      const remote = installed && (await this.adb(['shell', 'sha256sum', installed]).catch(() => '')).split(/\s/)[0];
      if (remote === local) continue;
      update(`Installing ${item.label}`);
      await this.installGuestPackage(item.package, apk);
    }
  }
  // scripts/waydroid.sh play: runs the session until the window closes. Its output is the log shown on failure.
  launch(game, onExit, mode = 'vr') {
    if (this.child) throw new Error('A game is already running.');
    validPackage(game.package);
    if (!/^[A-Za-z0-9_./]+$/.test(game.activity || '') || game.activity.split('/')[0] !== game.package) throw new Error('Invalid launch activity. Refresh installed games.');
    const args = ['play', game.package, '--activity', game.activity, '--name', game.name,
      ...(game.owned ? ['--owned'] : []),
      ...(validUserId(this.settings.userId) ? ['--user-id', this.settings.userId] : []),
      ...(mode === 'pc' ? (validEyeSize(this.settings.pcEyeSize) ? ['--eye-size', String(this.settings.pcEyeSize)] : [])
        : ['--headset', ...(validRenderScale(this.settings.vrRenderScale) ? ['--render-scale', String(this.settings.vrRenderScale)] : [])])];
    const child = spawn(this.script, args, { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ADB: this.adbExe() } });
    this.child = child; this.game = game.id; this.mode = mode;
    let tail = '';
    for (const stream of [child.stdout, child.stderr]) stream.on('data', b => { tail = (tail + b).slice(-4000); });
    let finished = false;
    const end = (code, error) => {
      if (finished) return; finished = true; this.child = null; this.game = null; this.mode = null;
      // A session the player stopped ended as asked (the script exits 130 on SIGINT).
      if (this.stopping) { this.stopping = false; code = 0; error = ''; }
      // The script's last "waydroid.sh: ..." line says why it stopped.
      const reason = tail.trim().split(/\r?\n/).filter(l => /^(waydroid\.sh|error)/i.test(l)).pop()?.replace(/^[^:]*:\s*/, '');
      // The whole output goes to launcher-backend.log (stderr), which diagnostics exports include.
      if (code) console.warn(`scripts/waydroid.sh play ${game.package} exited with code ${code}${error ? `: ${error}` : ''}\n${tail.trim()}`);
      onExit(code, error || (code ? reason || readableError(tail) || 'The game session stopped unexpectedly.' : tail));
    };
    child.on('error', e => end(1, e.message)); child.on('exit', code => end(code));
  }
  // Waydroid is the desktop's own session: leave it running when the launcher closes.
  async shutdown() {}
  async stop() {
    if (!this.child) return;
    this.stopping = true;
    this.child.kill('SIGINT');  // The script stops the game, the viewer and the bridge on INT.
  }
  async stopAndroid() { await run(this.script, ['stop'], { timeout: 60000 }); this.address = ''; }
}

// The Emulator page on Linux: Waydroid is a container, so there is no virtual machine, disk image or qemu to show.
export class WaydroidEmulator extends Emulator {
  async status() { return { ...await super.status(), adopted: false }; }
  // Processes of Refract's session and Waydroid, with CPU use since the last call.
  async processes() {
    const text = await run('ps', ['-eo', 'pid=,ppid=,times=,rss=,nlwp=,lstart=,comm=,args='], { timeout: 10000 }).catch(() => '');
    const wanted = /^(refract-viewer|refract-host-bridge|adb|waydroid|refract-venus|virgl_test_server|pose_input_serv)/;
    const list = [];
    for (const line of text.split('\n')) {
      const m = line.match(/^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\w{3} \w{3}\s+\d+ [\d:]+ \d{4})\s+(\S+)\s*(.*)$/);
      if (!m || !(wanted.test(m[7]) || /pose_input_server\.py|virgl|venus/.test(m[8]))) continue;
      list.push({ pid: Number(m[1]), parent: Number(m[2]), name: m[7], command: m[8], path: '', cpu: Number(m[3]), memory: Number(m[4]) * 1024,
        threads: Number(m[5]), started: new Date(m[6]).toISOString() });
    }
    const now = Date.now(), threads = os.cpus().length;
    for (const p of list) {
      const before = this.cpu.get(p.pid);
      p.cpuPercent = before && now > before.at ? Math.max(0, Math.min(100, (p.cpu - before.cpu) / ((now - before.at) / 1000) / threads * 100)) : null;
      this.cpu.set(p.pid, { cpu: p.cpu, at: now });
    }
    return list;
  }

  async hostInfo() {
    const read = file => fs.readFile(file, 'utf8').catch(() => '');
    const [release, meminfo, cpuinfo, gpuText, openxr, waydroidVersion, adbVersion, commit, launcherVersion, binder] = await Promise.all([
      read('/etc/os-release'), read('/proc/meminfo'), read('/proc/cpuinfo'),
      run('nvidia-smi', ['--query-gpu=name,driver_version', '--format=csv,noheader'], { timeout: 10000 }).catch(() => run('lspci', ['-mm'], { timeout: 10000 }).then(t => t.split('\n').filter(l => /VGA|3D/.test(l)).join('\n'), () => '')),
      activeOpenXr(),
      run('waydroid', ['--version'], { timeout: 5000 }).then(t => t.trim(), () => ''),
      run(this.runtime.adbExe(), ['version'], { timeout: 5000 }).then(t => t.split(/\r?\n/).slice(0, 2).join(' · '), () => ''),
      run('git', ['-C', this.root, 'log', '-1', '--format=%h %cs %s'], { timeout: 5000 }).then(t => t.trim(), () => ''),
      fs.readFile(path.join(this.root, 'launcher/package.json'), 'utf8').then(t => JSON.parse(t).version, () => ''),
      exists('/dev/binderfs/binder-control').then(ok => ok || exists('/dev/binder')),
    ]);
    const kb = name => Number(meminfo.match(new RegExp(`^${name}:\\s+(\\d+)`, 'm'))?.[1] || 0) * 1024;
    const cpus = cpuinfo.match(/^model name\s*:\s*(.+)$/m)?.[1].trim() || '';
    const gpus = gpuText.split('\n').map(l => l.trim()).filter(Boolean).map(l => l.replace(/^([^,]+), (.+)$/, '$1 (driver $2)'));
    return { os: release.match(/^PRETTY_NAME="?([^"\n]+)/m)?.[1] || 'Linux', memory: kb('MemTotal'), free: kb('MemAvailable'), cpu: cpus,
      cores: new Set(cpuinfo.match(/^core id\s*:\s*\d+/gm) || []).size || os.cpus().length, threads: os.cpus().length, gpus, openxr,
      acceleration: { ok: binder, detail: binder ? `Waydroid container (${waydroidVersion || 'installed'}), no virtual machine` : 'The kernel has no Android binder; Waydroid cannot start.' },
      sdk: this.settings.sdk, emulatorVersion: waydroidVersion, adbVersion, multicoreQemu: false, node: process.version, launcherVersion, commit, root: this.root, data: this.dataDirectory };
  }

  async components() {
    const files = [
      { label: 'Host bridge', file: 'build-linux/host-bridge/refract-host-bridge' },
      { label: 'PC viewer', file: 'build-linux/refract-viewer' },
      { label: 'GPU sharing layer', file: 'build-linux/gpu-layer/librefract_gpu_layer.so' },
      { label: 'Runtime Vulkan layer', file: 'build-vulkan-compat/libVkLayer_REFRACT_runtime.so' },
      ...waydroidPackages.map(p => ({ label: p.label, file: p.apk, package: p.package })),
    ];
    return Promise.all(files.map(async item => {
      const full = path.join(this.root, item.file);
      const stat = await fs.stat(full).catch(() => null);
      return { ...item, path: full, built: Boolean(stat), modified: stat?.mtime.toISOString() || '', size: stat?.size || 0,
        sha256: stat && item.package ? await sha256(full).catch(() => '') : '' };
    }));
  }

  // The newest session's logs (runs/<name>/, written by scripts/waydroid.sh play).
  sources() {
    const runs = path.join(this.root, 'runs');
    let newest = path.join(runs, 'waydroid-session'), at = 0;
    try {
      for (const name of readdirSync(runs)) {
        const stat = statSync(path.join(runs, name));
        if (stat.isDirectory() && name !== 'waydroid-session' && stat.mtimeMs > at) { at = stat.mtimeMs; newest = path.join(runs, name); }
      }
    } catch { /* No sessions yet. */ }
    return [
      { id: 'session', label: 'Waydroid session', file: path.join(runs, 'waydroid-session/waydroid-session.log') },
      { id: 'launcher', label: 'Launcher backend', file: path.join(this.dataDirectory, 'launcher-backend.log') },
      ...['host', 'viewer', 'input'].flatMap(name => [
        { id: `${name}-err`, label: `${{ host: 'Host bridge', viewer: 'PC viewer', input: 'PC input server' }[name]} (stderr)`, file: path.join(newest, `${name}.err`) },
        { id: `${name}-out`, label: `${{ host: 'Host bridge', viewer: 'PC viewer', input: 'PC input server' }[name]} (stdout)`, file: path.join(newest, `${name}.log`) }]),
      { id: 'logcat', label: 'Android logcat (last game)', file: path.join(newest, 'logcat.txt') },
    ];
  }

  // Android's data lives in ~/.local/share/waydroid, not in a disk image Refract manages.
  async diskInfo() { return { exists: false, directory: path.join(os.homedir(), '.local/share/waydroid') }; }
  async storage() {
    const port = await this.runtime.findPort();
    if (!port) return { collected: new Date().toISOString(), port: null, guest: null, host: await this.diskInfo() };
    const text = await this.adb(port, ['shell', 'df -k /data | tail -n 1'], { timeout: 15000 }).catch(() => '');
    const [, total, used, free] = text.trim().split(/\s+/).map(Number);
    return { collected: new Date().toISOString(), port, host: await this.diskInfo(),
      guest: { total: total * 1024 || 0, used: used * 1024 || 0, free: free * 1024 || 0, apps: [], leftovers: [], shared: [] } };
  }
  // Waydroid's adbd is not root, so the Storage tab's root tools are not available.
  asRoot() { return Promise.reject(new Error('Android in Waydroid has no root shell for adb.')); }

  async exportDiagnostics(extra = {}) {
    const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    const base = path.join(this.dataDirectory, 'diagnostics'), directory = path.join(base, `refract-diagnostics-${stamp}`);
    await fs.mkdir(path.join(directory, 'logs'), { recursive: true });
    const [info, status] = await Promise.all([this.info().catch(e => ({ error: e.message })), this.status().catch(e => ({ error: e.message }))]);
    await fs.writeFile(path.join(directory, 'diagnostics.json'), redact(JSON.stringify({ ...extra, status, info }, null, 2)));
    if (info.guest?.top) await fs.writeFile(path.join(directory, 'top.txt'), info.guest.top);
    const port = info.port || await this.runtime.findPort().catch(() => null);
    if (port) {
      await fs.writeFile(path.join(directory, 'logcat.txt'), redact(await this.adb(port, ['logcat', '-d', '-v', 'threadtime'], { timeout: 60000 }).catch(e => `logcat failed: ${e.message}`)));
      await fs.writeFile(path.join(directory, 'getprop.txt'), await this.adb(port, ['shell', 'getprop'], { timeout: 10000 }).catch(() => ''));
    }
    for (const source of this.sources()) {
      const text = await fs.readFile(source.file, 'utf8').catch(() => null);
      if (text !== null) await fs.writeFile(path.join(directory, 'logs', `${source.id}-${path.basename(source.file)}`), redact(text.slice(-16 * 1024 * 1024)));
    }
    const zip = `${directory}.zip`;
    await run('zip', ['-qr', zip, path.basename(directory)], { timeout: 120000, cwd: base }).catch(() => {});
    return { directory, zip: await exists(zip) ? zip : '', base };
  }
}
