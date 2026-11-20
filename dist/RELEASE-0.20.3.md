## 0.20.3

It uses the same protocol as 0.20.0, 0.20.1 and 0.20.2 (84), so it can play with them. Install it over 0.20.2.

Fixed:

- **Downed mode works again after DiMA's memories.** In Far Harbor, after a player went into one of DiMA's memories, every lethal hit killed that player outright instead of downing them, in the session and after any later load. The memory left the player marked "not essential" on the way out. Downed mode now takes that mark away a few seconds after it appears, and your save is mended the next time you load it in a session.

Known issues:

- A lethal hit inside one of DiMA's memories puts you down where no friend can reach you, and you stay down until you load a save.
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
