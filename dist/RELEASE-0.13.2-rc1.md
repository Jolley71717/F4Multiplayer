## 0.13.2-rc1

A test build of 0.13.2. Install it over 0.13.1. It uses the same protocol (62), so it plays with 0.13.0 and 0.13.1, but
each player needs 0.13.2-rc1 to see the changes in their own game.

New:

- **See your friends' shots.** A friend's laser beams and their guns' tracers now fly across your screen, as they do
  for your own shots. They are only a picture: they can't hurt you, your companions, settlers or anyone else, and the
  hit still comes from your friend's game as before. Shots from legendary guns still show only the muzzle flash, to be
  safe. Each player can turn it off in their own `F4Multiplayer.ini` (`bFriendShotTrails = false`).
- **Conveyor belts run for everyone.** A friend's powered conveyor belt (Contraptions Workshop) now runs in your game
  too, and stops when its power goes.

Fixed:

- **Buying Home Plate charged your friends too.** When one player bought Home Plate in Diamond City, every friend in
  the session paid the 2,000 caps as well. Now only the buyer pays, and everyone in the session gets the key and owns
  the house, so you can all get in. Two other quest steps that could take a friend's caps or empty their inventory are
  no longer shared.
- **The camera could stop following you** after being downed and helped up several times in quick succession (the
  Arc Jet engine fire, say), until you loaded a save. The mod now checks the camera all the time and frees it if it
  gets stuck. These checks also run when you play alone with the mod installed.

Known issues:

- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded
  game crashes there too. Quitting from the pause menu is fine.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or
  food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Worth trying: shooting with a friend watching (laser beams and tracers), a powered conveyor belt at a friend's
  settlement, buying Home Plate, and being downed and helped up several times in a row.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
