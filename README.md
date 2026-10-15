<img src="img/refract_banner.png" alt="Refract" width="100%">

[![Support Refract on Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20Refract-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/mixid_mbb)

Refract runs Android (Meta Quest) OpenXR apps on a Windows or Linux PC and shows them in a PC VR
headset. On Windows the app runs in the Android Emulator; on Linux it runs in Waydroid, a container
that shares the PC's kernel and GPU instead of emulating a machine. Refract provides an Android
OpenXR runtime inside Android, which sends frames to the PC and receives headset and controller
poses back.

> **Status:** experimental. It works for the setups documented below, but compatibility with arbitrary games is unfinished.

## Support Refract

I'm a student, and I build Refract in my spare time. If you'd like to see more Quest games running
on PC, a coffee on Ko-fi helps a lot.

[![Buy me a coffee on Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20me%20a%20coffee-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/mixid_mbb)

Not in a position to donate? Starring the repo, trying Refract with your games and reporting what
breaks helps too.

## How it works

```text
Android OpenXR APK (in the Android Emulator, or Waydroid on Linux)
  -> Refract Android OpenXR runtime (com.refract.openxrruntime)
  -> Refract protocol (poses, frames, input)
  -> Refract viewer / host bridge (Windows: D3D11, Linux: Vulkan)
  -> PC OpenXR runtime (SteamVR, or Monado/WiVRn on Linux)
  -> headset
```

The eye images are shared on the GPU through a Vulkan layer in the host process that renders for Android:
the emulator on Windows, the Venus renderer on Linux. A translator runs ARM64-only games on x86_64 Android.

## Repository layout

```text
android-runtime/      Android OpenXR runtime (native)
android-runtime-apk/  Installable runtime APK and runtime broker
platform-sdk/         Meta Platform SDK stand-in (package com.oculus.horizon)
protocol/             Pose, image and input protocol and transports
viewer/               Windows viewer: receives the shared eye textures and shows them
host-bridge/          Host bridge to a PC OpenXR runtime (Windows and Linux)
launcher/             Desktop launcher (Tauri + Node backend, GPL-3.0)
scripts/              Start the emulator and launch a game; waydroid.sh on Linux
tools/                GPU layer, GLES layer, translator patches, probes, analysis
prebuilts/digitalis/  Prebuilt ARM64-to-x86_64 translator (Digitalis/Berberis)
tests/                Smoke tests and loader probes
docs/                 Setup guides and design notes
```

## Getting started

### Linux

Requirements: Waydroid 1.6.3 or newer and a GPU (Nvidia's own driver works through Venus; AMD and Intel
use Mesa). To build: CMake, Ninja, a C++20 compiler, the Android SDK and NDK, and a JDK. `setup` installs
Android 16, the GPU stack and the Digitalis translator. [docs/linux_waydroid.md](docs/linux_waydroid.md)
has the details.

```sh
./build_host.sh
ANDROID_ABI=arm64-v8a android-runtime-apk/build_apk.sh && platform-sdk/build_apk.sh
sudo scripts/waydroid.sh setup
waydroid session start
scripts/waydroid.sh install path/to/game.apk
scripts/waydroid.sh play com.example.game     # in a window on the PC; --headset for SteamVR/Monado/WiVRn
```

The Linux path is new. It's tested on Fedora 44 with an RTX 3080: North Star, Yeeps and Batman: Arkham
Shadow run in the PC viewer. No Unreal game has been tested yet.

### Windows

Requirements: Windows 11, an Nvidia GPU, the Android SDK with the Android Emulator and NDK,
CMake and Ninja, and SteamVR.

1. Build and set up the emulator path. See
   [docs/windows_nvidia_emulator.md](docs/windows_nvidia_emulator.md).
2. Build the viewer with `viewer\build.bat`.
3. Start a game. [docs/yeeps_setup.md](docs/yeeps_setup.md) walks through a full working
   configuration:

   ```powershell
   .\scripts\start_emulator.ps1 -Cores 6 -Hidden
   .\scripts\ensure_tsc.ps1
   .\scripts\launch.ps1 -Run my-run -TranslatorMode two-gear
   ```

   Logs for each run are written to `runs\<name>\`.

Architecture, status and build details are in [docs/overview.md](docs/overview.md).

> **Note:** several scripts still hardcode the author's Android SDK path
> (`C:\Users\mixid\Android\Sdk`). Change it to match your machine.

## Not included

This repository contains no game APKs, Meta SDKs or Horizon OS files. You need to provide your own:

- The APKs of games you own.
- The OpenXR loader AAR, under `third_party/`.

The ARM64 translator is included prebuilt in [prebuilts/digitalis/](prebuilts/digitalis/)
(Digitalis/Berberis, Apache-2.0, with Refract's performance patch), so you don't have to build
AOSP. `scripts\translator.ps1 -Use digitalis` installs it into the emulator.

To pass the entitlement check with the Meta Platform SDK stand-in, list the games you own in
`scripts/owned_games.txt`. [scripts/owned_games.example.txt](scripts/owned_games.example.txt) shows the format.

Refract is meant for testing apps you own and open samples. Do not use it to bypass DRM,
anti-cheat, platform security, store restrictions or application license terms.

## Credits and license

Refract started as a fork of [AXRB](https://github.com/Android-XR-Bridge/AXRB) by Feline Reintgen.
It has since grown well beyond it: Refract adds its own Windows launcher, the Vulkan and GLES
layers, the translator performance work, the Platform SDK stand-in and much more. See
[CREDITS.md](CREDITS.md).

Refract is licensed under the **GNU General Public License v3.0 or later** ([LICENSE](LICENSE)),
with one additional term for visible credit ([NOTICE](NOTICE)):

- **Projects built on Refract must stay open source.** If you distribute Refract, a modified
  version or a project that includes Refract code, you must release its full source code under
  GPL-3.0-or-later as well.
- **Using Refract code requires visible credit.** You must credit Refract and link to
  <https://github.com/MIXIDtheSilly/Refract> where users can see it (a README, an About or
  credits page, or release notes). See [NOTICE](NOTICE) for the exact wording.

Code that still comes from AXRB was released under the MIT License; its copyright notice is kept
in [NOTICE](NOTICE).
