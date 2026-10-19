// Settings > Setup on Linux (the counterpart of the checks in setup.mjs): Waydroid, adb and the build outputs
// scripts/waydroid.sh play uses. Each check: { id, title, ok, detail, fix?: { action, label, note? } }.
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { run } from './runtime.mjs';
import { activeOpenXr, findAdb, openXrName, waydroidPackages } from './waydroid.mjs';
import { findAapt2, installBuildTools } from './android_sdk.mjs';

const exists = file => fs.access(file).then(() => true, () => false);
const jdk = () => process.env.JAVA_HOME || path.join(os.homedir(), 'Android/jdk-21');

async function python() {
  const check = { id: 'python', title: 'Python' };
  try { return { ...check, ok: true, detail: (await run('python3', ['--version'], { timeout: 15000 })).trim() }; }
  catch { return { ...check, ok: false, detail: 'Refract reads game packages and prepares Android with Python 3. Install python3 with your package manager.' }; }
}

async function adb(settings) {
  const check = { id: 'adb', title: 'adb' };
  const file = await findAdb(settings.sdk);
  return file ? { ...check, ok: true, detail: file }
    : { ...check, ok: false, detail: 'adb was not found. Install android-tools (Fedora), adb (Debian/Ubuntu) or the Android SDK platform-tools.' };
}

// Importing a game reads its manifest with aapt2 (launcher/inspect_apk.py).
async function buildTools(settings) {
  const check = { id: 'buildtools', title: 'Android build tools' };
  const file = await findAapt2(settings.sdk);
  return file ? { ...check, ok: true, detail: file }
    : { ...check, ok: false, detail: `Refract reads game packages with aapt2 from the Android build tools, which are not in ${settings.sdk}.`,
      fix: { action: 'buildTools', label: 'Download', note: 'Downloads Android build tools (about 64 MB) from Google. Downloading accepts the Android SDK License Agreement (developer.android.com/studio/terms).' } };
}

// scripts/waydroid.sh check lists what is missing; its "[--]" lines are information, only "[missing]" lines count.
async function waydroid(root) {
  const check = { id: 'waydroid', title: 'Waydroid and Android 16' };
  const fix = { action: 'waydroid', label: 'Set up Waydroid', note: 'Downloads Android 16 and the GPU stack, and asks for your password (pkexec). Takes a few minutes.' };
  const output = await run(path.join(root, 'scripts/waydroid.sh'), ['check'], { timeout: 60000 }).catch(error => error.message);
  const problems = [...String(output).matchAll(/^\s*\[missing\]\s*(.+)$/gm)].map(m => m[1].trim());
  const translator = String(output).match(/ARM64 translator: (.+)/)?.[1] || '';
  if (/waydroid: command not found|not found/i.test(String(output)) && !problems.length) return { ...check, ok: false, detail: 'Waydroid is not installed.', fix };
  if (problems.length) return { ...check, ok: false, detail: problems.join(' · '), fix };
  return { ...check, ok: true, detail: `Android 16 with ${translator || 'an ARM64 translator'}, drawing through ${String(output).match(/Android draws through (.+)/)?.[1] || 'the GPU'}` };
}

// The build outputs a session uses, with how to make them.
export function linuxComponents(root) {
  return [
    { label: 'Host bridge', file: 'build-linux/host-bridge/refract-host-bridge', host: true },
    { label: 'PC viewer', file: 'build-linux/refract-viewer', host: true },
    { label: 'GPU sharing layer', file: 'build-linux/gpu-layer/librefract_gpu_layer.so', host: true },
    ...waydroidPackages.map(p => ({ label: p.label, file: p.apk, package: p.package })),
    { label: 'ARM translator (Digitalis)', file: 'prebuilts/digitalis-qpr2/system' },
  ];
}
async function built(root) {
  const check = { id: 'components', title: 'Refract components' };
  const missing = [];
  for (const item of linuxComponents(root)) if (!await exists(path.join(root, item.file))) missing.push(item);
  if (!missing.length) return { ...check, ok: true, detail: 'Host bridge, PC viewer, GPU sharing layer, Android runtime and platform stand-in are built.' };
  const buildable = missing.every(m => m.host || m.package);
  return { ...check, ok: false, detail: `Not built yet: ${missing.map(m => m.label).join(', ')}.`,
    ...(buildable ? { fix: { action: 'build', label: 'Build missing parts', note: 'Needs CMake, Ninja, a C++ compiler, and for Android the SDK, NDK 27 and a JDK. Takes a few minutes.' } } : {}) };
}

async function openxr() {
  const check = { id: 'openxr', title: 'PC VR runtime' };
  const name = openXrName(await activeOpenXr());
  if (!name) return { ...check, ok: false, optional: true, detail: 'No OpenXR runtime is active, so games play on this PC only. For VR, install SteamVR, Monado or WiVRn and make it the active OpenXR runtime.' };
  return { ...check, ok: true, detail: `${name} is the active OpenXR runtime.` };
}

export async function checkSetupLinux(root, settings) {
  const [py, tools, aapt2, android, parts, xr] = await Promise.all([python(), adb(settings), buildTools(settings), waydroid(root), built(root), openxr()]);
  return [py, tools, aapt2, android, parts, xr];
}

export async function fixSetupLinux(root, settings, action, progress = () => {}) {
  const long = { timeout: 60 * 60 * 1000, cwd: root, onOutput: text => { const line = text.trim().split(/\r?\n/).pop(); if (line) progress(line.slice(0, 120)); } };
  switch (action) {
    case 'buildTools':
      await installBuildTools(settings.sdk, progress); return;
    case 'waydroid':
      // pkexec shows the desktop's password dialog; setup needs to know whose Waydroid data to set up.
      await run('pkexec', ['env', `SUDO_USER=${os.userInfo().username}`, path.join(root, 'scripts/waydroid.sh'), 'setup'], long); return;
    case 'build': {
      const missing = async item => !await exists(path.join(root, item.file));
      const items = linuxComponents(root);
      if (await Promise.all(items.filter(i => i.host).map(missing)).then(m => m.some(Boolean))) { progress('Building the host programs'); await run(path.join(root, 'build_host.sh'), [], long); }
      const env = { JAVA_HOME: jdk(), ...(process.env.ANDROID_HOME ? {} : { ANDROID_HOME: settings.sdk }) };
      if (await missing(items.find(i => i.package === 'com.refract.openxrruntime')) || await missing(items.find(i => i.package === 'com.oculus.systemdriver'))) {
        progress('Building the Android runtime'); await run(path.join(root, 'android-runtime-apk/build_apk.sh'), [], { ...long, env: { ...env, ANDROID_ABI: 'arm64-v8a' } });
      }
      if (await missing(items.find(i => i.package === 'com.oculus.horizon'))) { progress('Building the Meta Platform stand-in'); await run(path.join(root, 'platform-sdk/build_apk.sh'), [], { ...long, env }); }
      return;
    }
    default: throw new Error('Unknown setup step.');
  }
}

// Whether headset play can start now, as setup.mjs headsetStatus: { connected, runtime, title?, detail? }.
export async function headsetStatusLinux(runtime) {
  const name = openXrName(await activeOpenXr());
  if (!name) return { connected: false, runtime: '', title: 'No PC VR software', detail: 'Install SteamVR, Monado or WiVRn to play in VR.' };
  if (name === 'SteamVR') {
    const running = await run('pgrep', ['-x', 'vrserver'], { timeout: 5000 }).then(() => true, () => false);
    if (!running) return { connected: false, runtime: name, title: 'SteamVR is not running', detail: 'Start SteamVR with your headset connected to play in VR.' };
  }
  try { await runtime.checkHeadset(); return { connected: true, runtime: name }; }
  catch (error) {
    const detail = error.message.replace(/ and press Play again\.$/, '.');
    return { connected: false, runtime: name, title: /^No VR headset/.test(detail) ? 'No VR headset connected' : `${name} is not ready`,
      detail: /^No VR headset/.test(detail) ? `Connect your headset to ${name}.` : detail };
  }
}
