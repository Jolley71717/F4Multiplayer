## 0.21.0

It uses the same protocol as 0.20.0 to 0.20.4 (84), so it can play with them, but the fixes below need everyone in the session on 0.21.0. Install it over 0.20.4.
New:

- **Friends on the Pip-Boy's map.** Each friend is marked on your world map and local map, in their compass color with their name beside it, and the mark follows them while the map is open. A friend inside a building shows on the world map at its door out, faded, as the game shows you. Nothing is added to your load order or your save. To turn it off, set `bFriendsOnMap = false` in `F4Multiplayer.ini` (or `bFriendsOnMapNames = false` to hide just the names).

Fixed:

- **Dismissing a companion no longer traps a friend in a conversation.** While one player had the settlement list open to send a companion away, a friend's game could start that companion's first meeting on its own, a conversation with no way out. Friends now hold still while that list is up, as they do while you're paused.
- **Diamond City's synth accusation plays once for everyone.** The scene with Kyle and Riley at the market used to play separately in each game, so a friend who came later saw it start over or found the two already dead. Now the first player to walk into the market runs it, and everyone else sees that same scene.
- **Your own save no longer keeps the host's settlement pieces.** After building or playing at the host's settlement (with Sim Settlements 2's city planner's desk, for one), a save you made could keep copies of the host's pieces that nobody could use or scrap once you loaded it alone. Saves made with 0.21.0 now clear those copies when you load them alone. Copies already in an older save stay until you join the host there in a session and leave again.

Known issues:

- A character in power armor that is not a player (Piper, say) shows to a friend only as a flag; the frame itself is not shown yet.
- If both players go into Kellogg's memory at once, the second one's scenes can run ahead.
- A few quests that the game starts only from an event where they happen (the Mechanist's lair move, for one) don't start in a friend's game far away. Their later stages are still shared.
- The people in a shared event don't make their gestures in your friend's game (hands up, cowering, sitting).
- If the host reloads a save from before an NPC died while you were somewhere else, the NPC can stay dead in your game until its area resets.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- Building is shared only at settlements the host owns. At a friend's own settlement, settlers' beds aren't counted yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
