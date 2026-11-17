## 0.20.0-rc1

A test build of 0.20.0. It uses a new protocol (84), so everyone in a session must run it: it cannot play with 0.19.0. Install it over 0.19.0.

Fixed:

- **Virgil is back in a friend's older save that is already past Kellogg's memory.** A save made with an older version could keep him hidden in the Glowing Sea. Joining a session with shared story now brings him back.
- **A friend's weapon on a weapon rack shows only on the rack it is really on.** A weapon on a rack your world doesn't have used to show on another rack standing nearby. It now shows only when the same rack stands where your friend's does, and it follows the rack when your friend moves it.

Known issues:

- A character in power armor that is not a player (Piper, say) shows to a friend only as a flag; the frame itself is not shown yet.
- If both players go into Kellogg's memory at once, the second one's scenes can run ahead.
- A few quests that the game starts only from an event where they happen (the Mechanist's lair move, for one) don't start in a friend's game far away. Their later stages are still shared.
- The people in a shared event don't make their gestures in your friend's game (hands up, cowering, sitting).
- Diamond City's synth accusation still plays out in each game on its own.
- If the host reloads a save from before an NPC died while you were somewhere else, the NPC can stay dead in your game until its area resets.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- Building is shared only at settlements the host owns. At a friend's own settlement, settlers' beds aren't counted yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
