import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WaydroidRuntime, WaydroidEmulator, waydroidPackages, findAdb, openXrName } from '../core/waydroid.mjs';
import { linuxComponents } from '../core/setup_linux.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

test('the packages the launcher installs are the ones scripts/waydroid.sh install puts on Android', async () => {
  const script = await fs.readFile(path.join(root, 'scripts/waydroid.sh'), 'utf8');
  const listed = [...script.match(/RUNTIME_APKS=\(([\s\S]*?)\)/)[1].matchAll(/\$ROOT\/(\S+?)"/g)].map(m => m[1]);
  assert.deepEqual(waydroidPackages.map(p => p.apk), listed);
  assert.deepEqual(linuxComponents(root).filter(c => c.package).map(c => c.file), listed);
});

test('Waydroid addresses adb by the address on its bridge and reports a port only as "Android answers"', () => {
  const runtime = new WaydroidRuntime(root, { sdk: '/sdk' });
  assert.equal(runtime.serial(runtime.port), '127.0.0.1:5555');
  runtime.address = '192.168.240.112:5555';
  assert.equal(runtime.serial(5555), '192.168.240.112:5555');
  assert.equal(runtime.python, 'python3');
  assert.equal(runtime.guestPackages, waydroidPackages);
});

test('adb is found from ADB, the SDK or the PATH', async () => {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'refract-adb-'));
  const adb = path.join(directory, 'platform-tools/adb');
  await fs.mkdir(path.dirname(adb), { recursive: true }); await fs.writeFile(adb, '');
  const saved = { ADB: process.env.ADB, PATH: process.env.PATH };
  try {
    delete process.env.ADB; process.env.PATH = '/nonexistent';
    assert.equal(await findAdb(directory), adb);
    process.env.PATH = path.dirname(adb);
    assert.equal(await findAdb(''), adb);
    process.env.ADB = adb;
    assert.equal(await findAdb(''), adb);
  } finally { for (const [key, value] of Object.entries(saved)) value === undefined ? delete process.env[key] : process.env[key] = value; await fs.rm(directory, { recursive: true }); }
});

test('OpenXR runtimes are named by their manifest', () => {
  assert.equal(openXrName(''), '');
  assert.equal(openXrName('/home/a/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json'), 'SteamVR');
  assert.equal(openXrName('/usr/share/openxr/1/openxr_monado.json'), 'Monado');
  assert.equal(openXrName('/usr/share/openxr/1/openxr_wivrn.json'), 'WiVRn');
});

test('a launch needs the activity of its own package', () => {
  const runtime = new WaydroidRuntime(root, { sdk: '/sdk', userId: '1234567890123456' });
  assert.throws(() => runtime.launch({ package: 'com.example.game', activity: 'com.other/Main', name: 'G' }, () => {}), /Invalid launch activity/);
  assert.throws(() => runtime.launch({ package: 'bad package', activity: 'x/y', name: 'G' }, () => {}), /Invalid Android package/);
});

test('the Android page lists the session processes of this PC', async () => {
  const emulator = new WaydroidEmulator(new WaydroidRuntime(root, { sdk: '/sdk' }), root, os.tmpdir());
  clearInterval(emulator.logcat.idle);
  const list = await emulator.processes();
  assert.ok(Array.isArray(list));
  for (const p of list) assert.ok(Number.isInteger(p.pid) && p.name && Number.isFinite(p.cpu));
  const sources = emulator.sources();
  assert.ok(sources.some(s => s.id === 'launcher') && sources.some(s => s.id === 'logcat'));
});
