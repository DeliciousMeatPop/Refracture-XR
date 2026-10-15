REFRACT - play Quest games on your Windows PC
=============================================

What you need
- Windows 10 or 11, 64-bit
- A graphics card from the last few years (Refract is tested on NVIDIA)
- 12 GB of RAM or more
- About 10 GB of free disk space, plus room for your games
- Virtualization turned on in your PC's BIOS/UEFI (usually on already;
  it is called "Intel VT-x" or "AMD-V/SVM")
- For VR: Meta Horizon Link (Quest Link / Air Link) or SteamVR.
  Without a headset you can still play on your PC screen.

Getting started
1. Unzip this folder somewhere permanent, for example C:\Games\Refract.
   (Don't run it from inside the zip.)
2. Open Refract.exe.
   If Windows SmartScreen warns you, click "More info" > "Run anyway".
3. Choose how Refract runs games:
   - Refract Native (recommended): runs games directly on your PC, no emulator.
   - Android emulator: runs games in Android; works with the most games.
   You can switch later in Settings > Runtime.
4. Click each button in the setup list until everything is green. With Refract Native:
   - Install Python
   - Download the Android system files (about 700 MB from Google, once)
   - Download the Android build tools (about 60 MB, once)
   With the Android emulator:
   - Install Python
   - Set up Android (downloads about 2.4 GB from Google, once)
   - Turn on Windows Hypervisor Platform, if asked (then restart Windows)
5. Click "Connect Meta" at the bottom left and sign in to see the Quest games you own.
6. Open a game, Download it, Install it, then:
   - Play in VR: connect your Quest with Quest Link or Air Link (or start SteamVR) first.
   - Play on PC: the game opens in a window. Hold the right mouse button to look around,
     WASD to walk, left click = trigger. An Xbox controller works like Quest controllers.
     The full list of controls is on each game's page.

With the emulator, Android takes a minute or two to start the first time you play.
Closing the game window (or pressing Stop) saves and closes the game.

Updates
Refract checks for a new version each time it starts and asks before updating.
Updating keeps your games, settings and Meta sign-in.

If something goes wrong
- The error message says what to do in most cases.
- Settings > Setup shows anything that is missing, with a button to fix it.
- Logs: %APPDATA%\Refract\launcher-backend.log and the build-windows-* folders here.

Not every Quest game works yet. Mixed-reality games that need a room scan will not start.
