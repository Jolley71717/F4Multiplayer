## 0.13.1-rc1

A test build of 0.13.1. Install it over 0.13.0. It uses the same protocol (62), so it plays with 0.13.0: friends
don't all need to update at once.

New:

- **VATS skips friends.** VATS no longer picks a friend's stand-in as a target, so in a group it locks on to the
  enemies instead of the friend standing in front of you. With only friends nearby, VATS has nothing to target. Each
  player can turn it off in their own `F4Multiplayer.ini` (`bVatsSkipsFriends = false`). Shooting a friend without
  VATS is unchanged.

Known issues:

- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded
  game crashes there too. Quitting from the pause menu is fine.
- Workshop conveyor belts (Contraptions) run in the builder's game but not a friend's. A fix is waiting for a test.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or
  food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Worth trying: VATS in a group fight, with friends standing between you and the enemies.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
