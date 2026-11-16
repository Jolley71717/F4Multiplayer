## 0.19.0-rc1

A test build of 0.19.0. It uses a new protocol (83) and a new session file, so everyone in a session must run it: it cannot play with 0.17.x. Install it over 0.17.x. There was no 0.18.0 release: its work (a friend's shot trails among it) is folded into this one.

New:

- **A friend's shots go where they aimed, and look like their gun's.** A friend who turned and fired at once used to shoot where they had been looking a moment before. Their tracer is now the one their gun fires (another caliber, a laser's colour or a mod's own round), not the stock one.
- **A friend sees the changes you make to a robot at the workbench.** Arms, legs, head, torso, armor and paint on Ada or another robot now show on your friend's copy of it. Brahmin horns and heads too.
- **A friend sees the weapons on your weapon racks** (Contraptions Workshop) near you, instead of empty racks.
- **The Multiplayer menu keeps your own switches and the host's rules apart.** Settings holds your own switches; friendly fire, shared story and more quests are on a "Session options" page at the end of Settings.
- **The Controls page tells you when a key you just bound is one the game uses too,** so you know it will do both.

Fixed:

- **One player going into Kellogg's memory no longer pulls the other in.** A friend standing near the lounger was taken into the memory too and could be stuck there, with no Pip-Boy, no weapons and no talking, even after reloading. The way in is now only for the player who sits down, and the way out works in any game whose player is in the memory. Nick's talk after the memory still reaches a friend who did not go along.
- **Curie's new body now survives saves, menus and loading screens in a friend's game.** Her items could be lost if you opened the Pip-Boy or took a door while she changed, and a save made then kept her half changed. A friend left alone after the first talk with Amari now gets her new body a game day later, and can heal her with a stimpak, and her last steps find her.
- **Curie is no longer dismissed when two games' quests both hold her.** After the owner came back, the other game could show a "recruit refused" notice and send her away.
- **A companion quest's story now shows in a friend's Pip-Boy.** The quest sat in the journal with objectives but no text.
- **Mass Fusion takes the same side in both games after a console quickstart.**
- **A friend's quest step that turns one person on you waits until that person is not near you.** Before, the person attacked you the moment the friend's step arrived. Kick-outs that turn several groups (Nuka-World, the Combat Zone, Children of Atom, Vault 118) now wait for all of them, not only the first.
- **Developer warps and quickstarts typed at a console no longer move a friend or offer them a trip.** Twenty more are covered.
- **A friend's ordinary Minutemen or ending step no longer waits for Institute members to leave** or says the Institute turned on you.
- **A friend's next quest step is no longer lost when the quest went back a stage first.** Detective jobs in the Diamond City office and Getting a Clue are among them.
- **A friend's copy of a Minutemen settlement job knows who gave it,** Preston or the radio. Before, a job Preston had still to offer could shut itself down, and a finished job could ask for the next one in more than one game.
- **A friend's Minutemen job counts its days and its happiness only for a settlement that is the friend's own.**
- **A person that a friend's game cannot run no longer stands still a third of the time and blinks.** A Mister Gutsy on a road is one.
- **A friend's marker for a Railroad courser job leads to where your courser stands.**
- **A friend's kill is named in the kill feed for an NPC your game runs.** Before, nobody was shown as the killer.
- **A friend on Jet no longer glides across your screen at the wrong pace.** Their stand-in's legs now keep pace with them, and so do their people.
- **A friend's stand-in steps aside when you walk into it,** so a friend standing in a doorway no longer shuts it.
- **A friend who holsters their weapon while sneak-walking has a stand-in whose legs move again.**
- **A friend at a water pump or a vault door's console stands where they stand,** instead of being held in a loop.
- **A friend's face is sculpted as strongly as in their own game.** Friends' faces had looked narrower.
- **Settlement attacks tell a friend "under attack" and how it ended,** even when the attack began and ended between two looks.
- **A friend's kill of their own enemy is reported by their game alone.**
- **A can that only a friend's grenade moved goes back where it lay,** and a pile both players blast ends alike in both games.
- **Shared workshop: a firing turret is no longer shown twice,** and a friend sees the machinegun turrets you built before the session.
- **Shared workshop: a crop, bed or pump the host stores or scraps goes from a friend's game too.** A friend's Store or Scrap of a piece that came with the host's settlement is done by the host's game, and a settlement the host loses stops being the shared pile at once.
- **Shared workshop: a friend's build that the shared pile cannot pay for is taken back with a refund and a notice.**
- **A second character's settlement piece no longer replaces the first character's piece with the same number.**
- **A crash while the mod read the workshop's links is fixed.** It showed on a scan near Sanctuary, and the workbench lookups behind it now read safely too.
- **The `highest` NPC level rule tells the host how to start it again.**
- **Several small log fixes:** a hurt copy that regenerates writes one line instead of one a second, a worn item a script made is named in the log, a copy whose health maximum differs from its NPC's is named, and a repeating sound is counted for each stand-in.

Known issues:

- A character in power armor that is not a player (Piper, say) shows to a friend only as a flag; the frame itself is not shown yet.
- If both players go into Kellogg's memory at once, the second one's scenes can run ahead.
- A weapon on a weapon rack that the friend's world doesn't have can show on a rack nearby.
- Friends' saves made by older versions that are already past Kellogg's memory may keep Virgil hidden.
- A few quests that the game starts only from an event where they happen (the Mechanist's lair move, for one) don't start in a friend's game far away. Their later stages are still shared.
- The people in a shared event don't make their gestures in your friend's game (hands up, cowering, sitting).
- Diamond City's synth accusation still plays out in each game on its own.
- If the host reloads a save from before an NPC died while you were somewhere else, the NPC can stay dead in your game until its area resets.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- Building is shared only at settlements the host owns. At a friend's own settlement, settlers' beds aren't counted yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
