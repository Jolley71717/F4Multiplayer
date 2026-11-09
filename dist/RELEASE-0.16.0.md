## 0.16.0

Shared random events. Install it over 0.15.1. (This is the same file as the 0.16.0-rc1 test build, so seeing "rc1" in your log is normal.) It uses a new protocol (63): everyone in the session needs 0.16.0, and a friend on 0.15 or older can't join.

New:

- **Random events that aren't fights are shared.** A man held at gunpoint and called a synth, a trader on the road, people talking by the roadside: whichever game started the event, its people now stand and move in your friend's game too, and your friend hears what they say. Only the player whose game started it can talk to them.

Fixed:

- **Reloading an older save as host no longer kills people again.** A host who loaded a save from before killing someone had that NPC drop dead on every load, and a cleared interior that reset could have its people die as you walked in. Now they are alive again, as in the save, for everyone. A kill your friend was near enough to see stays shared, so a host who loads after going down mid-fight doesn't bring the fight back.
- **The first thing said in a shared scene is heard.** Your friend's game sometimes stayed silent for the first line, because it came before the speaker was there.

Changed:

- **A session file saved by 0.16.0 can't be read by 0.15 or older.** An older version sets it aside (renamed `.corrupt`) and starts over, so don't go back to 0.15 in the middle of a session played in 0.16. To recover after going back, rename the `.corrupt` file back to `.session` and play in 0.16 again.

Known issues:

- The people in a shared event don't make their gestures in your friend's game (hands up, cowering, sitting).
- Diamond City's synth accusation still plays out in each game on its own.
- If the host reloads a save from before an NPC died while you were somewhere else, the NPC can stay dead in your game until its area resets.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log.
