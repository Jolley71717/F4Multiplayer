## 0.13.2

Friends' shots in flight, belts that run for everyone, and fixes. Install it over 0.13.1. (This is the same file as the
0.13.2-rc1 test build, so seeing "rc1" in your log is normal.) It uses the same protocol (62), so it plays with 0.13.0
and 0.13.1, but each player needs 0.13.2 to see the changes in their own game.

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

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log
(`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log.
