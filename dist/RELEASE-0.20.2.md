## 0.20.2

It uses the same protocol as 0.20.0 and 0.20.1 (84), so it can play with them. Install it over 0.20.1.

Fixed:

- **A friend can open the workbench at the host's settlements.** With shared building on, a friend pressing E at the workbench of a settlement the host owns (but the friend's own game doesn't) got "You need to clear the enemies from the location" (or "not allied") and nothing opened. It now opens workshop mode there, and everything the friend builds is the host's, as at any of the host's settlements. The workshop key works there too. The friend's own game claims nothing: the settlement stays unowned in their save.

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
