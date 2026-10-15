REFRACT - play Quest games on your Linux PC
===========================================

What you need
- A 64-bit Linux desktop whose kernel has Android binder (binderfs): Fedora, Arch, Ubuntu
  and Debian kernels have it
- Waydroid 1.6.3 or newer (on Fedora, Refract's setup installs it)
- WebKitGTK 4.1 (Fedora: webkit2gtk4.1, Ubuntu/Debian: libwebkit2gtk-4.1-0), Python 3 and adb
  (Fedora: android-tools, Ubuntu/Debian: adb)
- A graphics card from the last few years (Refract is tested on NVIDIA)
- 12 GB of RAM or more, and about 10 GB of free disk space plus room for your games
- For VR: SteamVR, Monado or WiVRn. Without a headset you can still play on your PC screen.

Getting started
1. Unpack this folder somewhere permanent, for example ~/Games/Refract:
     tar -xzf Refract-*-linux-x86_64.tar.gz -C ~/Games
2. Start ./Refract in that folder.
3. Settings > Setup lists anything missing, with a button or the command to fix it.
   Setting up Waydroid needs root once:
     sudo scripts/waydroid.sh setup
4. Click "Connect Meta" at the bottom left and sign in to see the Quest games you own.
5. Open a game, Download it, Install it, then Play in VR or Play on PC.

Details, and running Refract from the command line: docs/linux_waydroid.md.

Updates
Refract checks for a new version each time it starts and asks before updating.
Updating keeps your games, settings and Meta sign-in.

Not every Quest game works yet. Mixed-reality games that need a room scan will not start.
