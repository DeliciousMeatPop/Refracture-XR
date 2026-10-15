<img src="img/refract_banner.png" alt="Refract" width="100%">

[![Support Refract on Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20Refract-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/mixid_mbb)

Refract runs Meta Quest (Android OpenXR) games on a Windows or Linux PC, in a PC VR headset or in a
window on the screen. Games run in one of three ways:

- **Android Emulator** (Windows): the most compatible option.
- **Refract Native** (Windows, experimental): runs the game's ARM64 code directly on the PC.
- **Waydroid** (Linux): a container that shares the PC's kernel and GPU.

> **Status:** experimental. Not every game works yet.

## Support Refract

I'm a student, and I build Refract in my spare time. If you'd like to see more Quest games running
on PC, a coffee on Ko-fi helps a lot.

[![Buy me a coffee on Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20me%20a%20coffee-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/mixid_mbb)

Not in a position to donate? Starring the repo, trying Refract with your games and reporting what
breaks helps too.

## How it works

```text
Quest game (Android Emulator, Refract Native or Waydroid)
  -> Refract's Android OpenXR runtime
  -> Refract viewer / host bridge on the PC
  -> PC OpenXR runtime (SteamVR, Meta Horizon Link, Monado/WiVRn) -> headset
```

Eye images are shared on the GPU, and headset and controller poses go back to the game. ARM64-only
games run through the Digitalis translator, or Refract Native's own ARM64 JIT.

## Requirements

- Windows 10 or 11 (64-bit), or Linux with Waydroid 1.6.3 or newer
- A graphics card from the last few years
- 12 GB of RAM or more
- About 10 GB of free disk space, plus room for your games
- Virtualization turned on in the BIOS/UEFI (Windows)
- For VR: Meta Horizon Link (Quest Link / Air Link) or SteamVR; Monado or WiVRn also work on Linux.
  Without a headset you can play in a window.

## Getting started

### Windows

1. Download a build from [Releases](https://github.com/MIXIDtheSilly/Refract/releases) and unzip it,
   or double-click **Refract Launcher.cmd** in a source checkout.
2. In **Settings > Setup**, click each button until everything is green.
3. Click **Connect Meta** to see the Quest games you own, then download, install and play.

More in [launcher/README.md](launcher/README.md). To run the emulator by hand, see
[docs/windows_nvidia_emulator.md](docs/windows_nvidia_emulator.md) and
[docs/yeeps_setup.md](docs/yeeps_setup.md).

### Linux

To build: CMake, Ninja, a C++20 compiler, the Android SDK and NDK, and a JDK.

```sh
./build_host.sh
ANDROID_ABI=arm64-v8a android-runtime-apk/build_apk.sh && platform-sdk/build_apk.sh
sudo scripts/waydroid.sh setup
waydroid session start
scripts/waydroid.sh install path/to/game.apk
scripts/waydroid.sh play com.example.game     # --headset for SteamVR/Monado/WiVRn
```

The launcher also runs on Linux. Details are in [docs/linux_waydroid.md](docs/linux_waydroid.md).

## Not included

This repository contains no game APKs, Meta SDKs or Horizon OS files: bring the games you own.
The Digitalis ARM64 translator (Apache-2.0) is included prebuilt in
[prebuilts/digitalis/](prebuilts/digitalis/).

Refract is meant for apps you own and open samples. Do not use it to bypass DRM, anti-cheat,
platform security, store restrictions or license terms.

## Credits and license

Refract started as a fork of [AXRB](https://github.com/Android-XR-Bridge/AXRB) by Feline Reintgen
and has since grown well beyond it. See [CREDITS.md](CREDITS.md).

Refract is licensed under the **GNU GPL v3.0 or later** ([LICENSE](LICENSE)), with one additional
term ([NOTICE](NOTICE)): projects built on Refract must stay open source under GPL-3.0-or-later,
and must credit Refract with a visible link to <https://github.com/MIXIDtheSilly/Refract>. Code
still from AXRB is MIT-licensed; its notice is kept in [NOTICE](NOTICE).
