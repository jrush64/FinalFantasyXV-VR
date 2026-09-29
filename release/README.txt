FFXV VR  -  FINAL FANTASY XV WINDOWS EDITION in VR, with head tracking
by Halcyon

Copyright (c) 2026 Halcyon.
Licensed under the GNU General Public License v3.0 - see LICENSE.txt. You are
free to use, modify, and redistribute this software under the terms of that
license.


INSTALL
1. Extract ALL the files into your game folder, the one with ffxv_s.exe.
   (Steam: right-click FINAL FANTASY XV > Manage > Browse local files.)
2. Double-click FFXV-VR.bat in that folder.
3. Pick an image quality. Balanced (2560x1440) is recommended.

If the installer says the game has no settings file yet, start the game once,
reach the title screen, quit, and run FFXV-VR.bat again.

The installer sets the game to borderless, TAA on (the mod turns it into DLSS 4),
motion blur off and Model LOD to its default, and puts in a newer DLSS file.
Your old game video settings and DLSS file are kept and come back when you
uninstall.


PLAY
1. Start your VR runtime (SteamVR, Virtual Desktop, Quest Link...).
   It must be the active OpenXR runtime: SteamVR > Settings > OpenXR, or
   Meta Horizon Link app > Settings > General > OpenXR Runtime.
2. Launch the game and load your save.
3. Once you are in the game, press the head tracking key (F9 by default).
   Wait for the beep: head tracking takes a few seconds.


KEYS (defaults; all can be changed in the menu, and the installer shows yours)
Insert  menu
F9      start VR and head tracking; press again if tracking is ever lost
R       recenter
End     switch to Mono (use it if the game ever hangs)
K       first person (experimental)


GOOD TO KNOW
- Press the head tracking key only once you are IN the game, not on the title
  screen, a menu or a loading screen. The mod finds the game camera then.
- No head tracking after the beep, or it stops working? Press it again.
- Keep Model LOD at its default. Raised above it, patches of grass pop in and
  out in the headset as you turn your head.
- If a conversation or scene seems stuck while the game is still running,
  press the Mono key (End by default) or pick another VR mode in the Insert
  menu, then switch back once past it.


PROBLEMS
Send ffxv-vr.log from the game folder (ffxv-vr.prev.log is the session before).


CHANGE QUALITY OR UNINSTALL
Run FFXV-VR.bat again.

Supports SDR and HDR displays with the same DLL. The installer does not change
Windows HDR.
