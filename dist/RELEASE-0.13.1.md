## 0.13.1

VATS skips friends. Install it over 0.13.0. (This is the same file as the 0.13.1-rc1 test build, so seeing "rc1" in
your log is normal.) It uses the same protocol (62), so it plays with 0.13.0: friends don't all need to update at once,
but each player needs 0.13.1 for VATS to skip friends in their own game.

New:

- **VATS skips friends.** VATS no longer picks a friend's stand-in as a target, so in a group it locks on to the
  enemies instead of the friend standing in front of you. With only friends nearby, VATS has nothing to target. Each
  player can turn it off in their own `F4Multiplayer.ini` (`bVatsSkipsFriends = false`). Shooting a friend without
  VATS is unchanged.

Known issues:

- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded
  game crashes there too. Quitting from the pause menu is fine.
- After being downed and helped up several times in quick succession (the Arc Jet engine fire, say), the camera can
  stop following your character until you load a save. A fix is in the works.
- Workshop conveyor belts (Contraptions) run in the builder's game but not a friend's. A fix is waiting for a test.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or
  food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log
(`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log.
