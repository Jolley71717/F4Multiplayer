## 0.8.6

- Weapons and armor are no longer shared between games: a gun your friend loots stays on your copy of the corpse, and a gun or armor piece put in a chest stays out of your friend's chest. Only the item's base type travelled, so your friend got a plain copy, and taking one could remove a legendary or modded piece from their game instead of the plain one.
- Also each player's own: paper notes, holotapes, keys, bobbleheads and magazines (a note your friend takes stays for you to read), vendors and caps (selling to a vendor leaves your friend's vendor with the same caps and stock), what companions, settlers and any living NPC carry, settlement workbench storage and power armor frames. Shared as before: ammo, chems, food, junk and other loot in containers and on corpses.
- Loot is no longer lost or repeated when changes arrive out of order: a friend's loot made while your save loads lands after what they took before it, a change that went missing is asked for again, and after a host's crash the next loot by either player still shows up in the other game.
- Loading a save made alone, or in another session, while in a session no longer brings back the whole session's deposits on every later reload.
- Loot taken in the moment before a save or a reload reaches your friend instead of being forgotten.
- A button, switch or sink your friend presses is pressed by their stand-in in your game, not by you: drinking at a sink no longer gives you the rads.
- A friend on an earlier 0.8.6 test build with the same protocol can still join; the weapon, armor, vendor, workbench and power armor frame moves their game still reports are ignored by this one.
- Nothing is written to the HUD for six seconds after a game load, when the engine rebuilds its menus. The host crashed in the HUD text layout three seconds after loading a save; the loading-screen guard from 0.8.5 ended too early for that case.
- The F1 menu's first button is always Close or Back. Tab presses a message box's first button, so leaving the menu with Tab could start hosting, leave the game or stop hosting by accident, and a host who did that lost shooting, the Pip-Boy and the map until a restart.
- Protocol 26, as 0.8.0 to 0.8.5. Newer builds are on GitHub first; Nexus follows less often.
