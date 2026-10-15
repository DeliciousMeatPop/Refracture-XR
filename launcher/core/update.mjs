// Updates a packaged Refract (tools/package_release.ps1, tools/package_release.sh) from the latest GitHub release.
// The launcher asks on every start; Test N pre-releases are never offered (releases/latest skips pre-releases).
// A checkout (with .git) updates with git instead and is never offered one.
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createWriteStream } from 'node:fs';
import fs from 'node:fs/promises';
import path from 'node:path';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { run } from './runtime.mjs';

const REPOSITORY = 'MIXIDtheSilly/Refract';
const linux = process.platform === 'linux';
// The release asset for this platform and the folder at the top of it.
const assetName = /^Refract-(\d+\.\d+\.\d+)-(win64\.zip|linux-x86_64\.tar\.gz)$/;
const platformSuffix = linux ? 'linux-x86_64.tar.gz' : 'win64.zip';
const exists = file => fs.access(file).then(() => true, () => false);

const parse = version => String(version).replace(/^v/, '').split('.').map(Number);
export function newer(candidate, current) {
  const a = parse(candidate), b = parse(current);
  if (a.length !== 3 || b.length !== 3 || [...a, ...b].some(n => !Number.isInteger(n))) return false;
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i];
  return false;
}

export const updatable = async root => !await exists(path.join(root, '.git'));

// The newest release if it is newer than `current` and has a package for this platform, else null.
export async function checkUpdate(root, current) {
  if (!await updatable(root)) return null;
  const response = await fetch(`https://api.github.com/repos/${REPOSITORY}/releases/latest`, {
    headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'Refract-launcher' }, signal: AbortSignal.timeout(15000) });
  if (response.status === 404) return null;  // No full release yet, only pre-releases.
  if (!response.ok) throw new Error(`GitHub answered ${response.status}.`);
  const release = await response.json();
  const version = String(release.tag_name || '').replace(/^v/, '');
  if (release.draft || release.prerelease || !newer(version, current)) return null;
  const asset = (release.assets || []).find(a => a.name.endsWith(platformSuffix) && assetName.exec(a.name)?.[1] === version);
  if (!asset) return null;
  // GitHub records each uploaded asset's SHA-256; older releases list it in their notes instead.
  const sha256 = /^sha256:([0-9a-f]{64})$/i.exec(asset.digest || '')?.[1]
    || new RegExp(`${asset.name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}[^0-9a-fA-F]*([0-9a-fA-F]{64})`).exec(release.body || '')?.[1];
  if (!sha256) return null;
  return { version, name: release.name || `Refract ${version}`, notes: String(release.body || '').slice(0, 4000), page: release.html_url,
    asset: { name: asset.name, url: asset.browser_download_url, size: asset.size, sha256: sha256.toLowerCase() } };
}

async function download(url, destination, size, progress) {
  const response = await fetch(url, { headers: { 'User-Agent': 'Refract-launcher' }, signal: AbortSignal.timeout(60 * 60 * 1000) });
  if (!response.ok || !response.body) throw new Error(`The update download failed (HTTP ${response.status}).`);
  const hash = createHash('sha256');
  let received = 0, shown = -1;
  const body = Readable.fromWeb(response.body);
  body.on('data', chunk => {
    hash.update(chunk); received += chunk.length;
    const percent = size ? Math.floor(received * 100 / size) : 0;
    if (percent !== shown) { shown = percent; progress(`Downloading the update (${percent}%)`); }
  });
  await pipeline(body, createWriteStream(destination));
  return hash.digest('hex');
}

// Downloads and checks the update, unpacks it beside Refract's data, and starts a helper that waits for the
// launcher to close, copies the new files over the installation (files Refract downloaded there, like Android's
// system files, stay) and starts the new launcher. The caller then closes the launcher.
export async function installUpdate(root, dataDir, update, progress = () => {}) {
  if (!await updatable(root)) throw new Error('This copy of Refract is a git checkout; update it with git.');
  const { name, url, size, sha256 } = update.asset;
  const match = assetName.exec(name);
  if (!match || !name.endsWith(platformSuffix) || !/^https:\/\/github\.com\//.test(url)) throw new Error('Unexpected update package.');
  const folder = path.join(dataDir, 'update');
  await fs.rm(folder, { recursive: true, force: true });
  await fs.mkdir(folder, { recursive: true });
  const archive = path.join(folder, name);
  if (await download(url, archive, size, progress) !== sha256) throw new Error('The update download is damaged (checksum mismatch). Try again later.');
  progress('Unpacking the update');
  const unpacked = path.join(folder, 'files');
  await fs.mkdir(unpacked);
  // Windows 10 and 11 include bsdtar, which reads zip files.
  await run(linux ? 'tar' : path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe'), ['-xf', archive, '-C', unpacked], { timeout: 10 * 60 * 1000 });
  const top = name.replace(/\.(zip|tar\.gz)$/, '');
  const source = path.join(unpacked, top);
  const launcher = path.join(source, linux ? 'Refract' : 'Refract.exe');
  if (!await exists(launcher)) throw new Error('The update package has no Refract launcher in it.');
  await fs.rm(archive, { force: true });
  progress('Restarting Refract');
  const pids = [process.pid, process.ppid].join(' ');
  const log = path.join(folder, 'update.log');
  let child;
  if (linux) {
    const script = path.join(folder, 'apply.sh');
    await fs.writeFile(script, [
      '#!/bin/sh',
      `for pid in ${pids}; do i=0; while kill -0 "$pid" 2>/dev/null && [ $i -lt 120 ]; do sleep 0.25; i=$((i+1)); done; done`,
      'cp -a "$1/." "$2/" || exit 1',
      'rm -rf "$1"',
      'cd "$2" && exec ./Refract',
      ''].join('\n'), { mode: 0o755 });
    child = spawn('/bin/sh', [script, source, root], { detached: true, stdio: ['ignore', (await fs.open(log, 'w')).fd, 'ignore'] });
  } else {
    const script = path.join(folder, 'apply.ps1');
    await fs.writeFile(script, [
      'param([string]$Source, [string]$Target, [string]$Wait)',
      '$deadline = (Get-Date).AddSeconds(30)',
      'foreach ($id in $Wait -split ",") { while ((Get-Process -Id $id -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 } }',
      '# Files can stay locked for a moment after their process exits.',
      'for ($try = 0; $try -lt 20; $try++) {',
      '  robocopy $Source $Target /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null',
      '  if ($LASTEXITCODE -lt 8) { break }',
      '  Start-Sleep -Seconds 1',
      '}',
      'if ($LASTEXITCODE -ge 8) { Write-Output "robocopy failed ($LASTEXITCODE)" }',
      'else { Remove-Item -Recurse -Force (Split-Path $Source) -ErrorAction SilentlyContinue }',
      'Start-Process (Join-Path $Target "Refract.exe") -WorkingDirectory $Target',
      ''].join('\r\n'));
    child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', script,
      '-Source', source, '-Target', root, '-Wait', pids.replace(' ', ',')], { detached: true, windowsHide: true, stdio: ['ignore', (await fs.open(log, 'w')).fd, 'ignore'] });
  }
  child.unref();
  return { restart: true };
}
