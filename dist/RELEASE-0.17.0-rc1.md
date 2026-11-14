A test build of 0.17.0: shared settlements, shared settlement attacks and many more shared quests. Install it over 0.16.1. It uses a new protocol (76): everyone in the session needs 0.17.0, and a friend on 0.16 or older can't join.

New:

- **Build together in the host's settlements.** At a settlement the host owns, what a friend builds or wires in workshop mode is built in the host's game, so you both see the same settlement, with one shared workbench pile. Electricity works across both players' pieces. A friend can also scrap, store, move and repair the host's pieces (host setting `bSharedWorkshopEdit`, on by default). A friend's workshop bar shows the host's settlement figures (food, water, defense, happiness, power).
- **Settlers are shared too.** A friend sees the settlers the host has recruited, can give them jobs, and can set up supply lines with the host's Local Leader. The host's game carries each order out, so both games agree.
- **Settlement attacks on the host's settlements are both players'.** The host's game decides once whether an attack happens (the add-ons' attacks too), and both players fight the same attackers. Preston's "defend the settlement" jobs are shared the same way. Host setting `bSharedAttacks`, on by default.
- **More quests are shared** (with the host's `sQuests = safe`): the companions' own quests (Curie, MacCready, Nick, Cait, Danse; romance and affinity stay each player's own), Nick's post-quests, Sheffield's Nuka-Cola, the Detective Case Files, A Loose End's postquest, Atom Cats Garage, Vault 75, the Wasteland Refrigerator, Combat Zone, the end of The Big Dig, the teleport and timer quests, and the add-ons' endings and kick-outs.
- **Faction choices are shared.** When one player's choice gets you kicked out of a faction or turns it hostile, both games follow, companion included as in the game, with a HUD warning first. A faction that turns hostile waits until none of its members are near the other player before it does. Power Play's gang choice is the chooser's for both.
- **More radiant jobs are shared:** the Railroad's jobs, the Institute's Appropriation and Reclamation, the Brotherhood's squire and scribe jobs, and Preston's synth boss job. The job's enemies are the same actors in both games.
- **Random fights are shared.** A fight that starts at a spot is one fight for both players, not one in each game.
- **Quest rewards reach everyone once.** A turn-in's reward reaches every player, even one whose game was closed or lost the link when it was turned in.
- **Achievements from a friend's quest stages** are awarded only when the stage happened while you played, not when your game catches up later.
- **VATS:** you see who is in VATS. A new host option, `bGroupVatsSlowdown` (off by default), slows everyone's game together while anyone is in VATS.

Fixed:

- **A host whose Steam connection drops briefly keeps the session.** The game now makes a new lobby once Steam is back, and friends can rejoin it.
- **A friend's stand-in no longer glides** while your own game runs slowed.
- **NPCs in interiors stay in step** with the game that runs them, across the whole cell.
- **The mod's crash report also covers crashes that used to leave nothing behind:** a game that stops itself (an "abort", which crash loggers often miss) now writes `F4Multiplayer-crash-<date>_<time>.txt` next to the plugin log, saying what stopped it.

Changed:

- **A session file saved by 0.17.0 can't be read by 0.16 or older.** An older version sets it aside (renamed `.corrupt`) and starts over, so don't go back to 0.16 in the middle of a session played in 0.17. To recover after going back, rename the `.corrupt` file back to `.session` and play in 0.17 again.

Known issues:

- A few quests that the game starts only from an event where they happen (the Mechanist's lair move, for one) don't start in a friend's game far away. Their later stages are still shared.
- The people in a shared event don't make their gestures in your friend's game (hands up, cowering, sitting).
- Diamond City's synth accusation still plays out in each game on its own.
- If the host reloads a save from before an NPC died while you were somewhere else, the NPC can stay dead in your game until its area resets.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- Building is shared only at settlements the host owns. At a friend's own settlement, settlers' beds aren't counted yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol) before testing, then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
