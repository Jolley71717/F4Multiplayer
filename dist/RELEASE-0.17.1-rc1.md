## 0.17.1-rc1

A test build of 0.17.1, a fix release. Install it over 0.17.0. It uses the same protocol (76) and the same session file as 0.17.0, so it can join and host 0.17.0 players, but everyone should update to get the fixes.

Fixed:

- **A host whose first Steam lobby request is slow no longer leaves friends with no lobby.** If the first answer came back as a failure after the host had already asked again, friends had nothing to join until a third try about 30 s later. The second lobby is now kept and advertised at once.
- **A friend can pick up an item you dropped while you are standing on it.** The item lies at your feet, so their crosshair found you instead of the item and nothing happened. Pressing on your stand-in now takes the item at its feet.
- **Power armor given back no longer flashes at the spot it was lent from, and lands in the right place.** After a friend got out of your frame far away, your frame is now kept in the cell where they left it, and your friend no longer sees a copy of it appear for a moment at the old spot.
- **A friend's stand-in no longer loses its gun after a gun swap.** When a friend swapped the gun in their hand for another copy with different mods, the new gun could be taken away from their stand-in too.
- **A friend's frame copy no longer blinks in and out at a cell's edge.** Standing on the edge of a cell could place and remove the copy many times in a second. It now stays until it has been out of sight for 2 s.
- **Your friend's people no longer snap back to their own behavior as your friend's Pip-Boy closes.** People held still while your friend has a menu open could let go a moment early and be pulled back.
- **Kill XP is no longer lost when a menu opens while the enemy falls.** Opening the pause menu or the Pip-Boy right after a kill could leave the kill's XP unshared.
- **Enemies a friend's game runs now die in your game as they fall,** not 3 to 4 s later.
- **Joining or loading no longer shows a friend's hurt enemy at full health.** The copy of a creature or raider your friend had wounded was placed at full health until its health next changed. It now takes the real health.
- **An enemy of your friend's that died only in your game is brought back to life.** Before, your copy stayed dead for good while the enemy lived on in your friend's game.
- **Far-off people from your friend's game no longer use up the room for nearer ones.** They are only tracked within about 9000 units of you, so a later enemy is no longer left out because of them.
- **Settlement attacks from add-ons reach a host who lacks the add-on.** A friend whose game rolled an add-on's attack asked whose settlement it was, and a host without that add-on never answered, so the attack kept hitting the host's settlement.
- **A friend's own random attack gives way while the host's "defend the settlement" job is held at that settlement,** so there are not two attacks at once.
- **A friend's settlers dying in their own game can no longer fail the host's "defend the settlement" job there alone.** Both games now end the job the same way.
- **A friend with no settler at the host's settlement can start the host's "defend the settlement" job.** The game refused to start it when the settlement's spokesman could not be found in their game. It now starts with that spot left empty.
- **The host loading a save no longer makes a friend wait 15 s at the workbench.** A friend's build, scrap, store, move or repair now gets an answer at once ("the host isn't near the settlement") instead of waiting for the host's game that never answered.
- **A friend's list of the host's settlers and supply lines stays up to date** when a lot is going on (right after a load, for instance).
- **A saved game no longer keeps a friend's conveyor belt running** when its builder is away and the belt has no power.
- **Quests that the game starts from an event now start from a friend's step.** The Diamond City entrance and Goodneighbor quests, among others, never started in your game when a friend reached a step of them. They now start first, then take the step. A few quests the game cannot start safely this way still go their own way.
- **Goodneighbor's gate guards leave in every game.** When the guards' scene came to a game far from Goodneighbor, the timer that ends the event never ran there and the new guards stayed at the gate for good.
- **A step typed at a console for testing is never shared with a friend, nor what it sets.** This now covers more of the game's test steps, including ones in the Combat Zone, the Rail Road, Automatron, Far Harbor and Nuka-World, and a test step of the main story's "Getting a Clue" quest that could move a friend.
- **A friend's first step of a quest that begins at a step other than its first is handled by the right rule** (a few Institute and main story quests begin that way).
- **Quest rewards are no longer given twice or to the wrong character around a load.** A reward that a friend's turn-in captured before you quickloaded could be given after the load and again at a later turn-in. A friend's reward not yet given is also dropped if you load another character's save.
- **A random encounter that gives way to a friend's no longer risks a crash while it is starting.** It is now removed only once it has finished starting up.
- **Random encounters give way only to the same kind of encounter.** A checkpoint at your friend's end could be stopped for a camp 2300 units away, though in one game both would have run.
- **A friend can follow a checkpoint fight even if they have no defender yet.** Before, the run could fail to start in their game, and they saw the attackers with no objective.
- **A friend's checkpoint run no longer leaves a vertibird standing far off in your game.** A save made then could keep and share the checkpoint as your own.
- **Thrown cans and other objects stop spinning when they come to rest** in your game, not roll on for seconds from where your friend's came to rest.
- **The mod's door checks cost less of each frame in places with many doors,** such as Concord and Corvega. When there are many, they are now spread over more frames.
- **Prompts that name a key use the same names as the Controls page.** "Hold T to teleport", "Hold T to give up" and the "your mods differ" notice no longer say "key 54" when the key is rebound to a letter or a mouse button.
- **A gamepad can page a long text in the Multiplayer menu on any page.** The bumpers page the text up and down; the stick still moves between rows.
- **Several small stability fixes** in how the mod hands work between the game's threads and in how it tracks the workshop pieces a friend scrapped, so that those copies are not tracked twice.

Known issues:

- A few quests that the game starts only from an event where they happen (the Mechanist's lair move, for one) don't start in a friend's game far away. Their later stages are still shared.
- The people in a shared event don't make their gestures in your friend's game (hands up, cowering, sitting).
- Diamond City's synth accusation still plays out in each game on its own.
- If the host reloads a save from before an NPC died while you were somewhere else, the NPC can stay dead in your game until its area resets.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- Building is shared only at settlements the host owns. At a friend's own settlement, settlers' beds aren't counted yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.
- A friend who puts their weapon away while sneak-walking can still glide along crouched in your game until they stop.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
