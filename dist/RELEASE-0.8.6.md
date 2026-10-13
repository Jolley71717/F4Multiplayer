## 0.8.6

Everyone needs 0.8.6 to play together: it uses protocol 27, so a friend on 0.8.5 or on the 0.8.6-rc1 or rc2 test builds gets "Version mismatch".

Coming from 0.8.5:

- Each character now has its own session file (`F4Multiplayer.<character>.session`). The old `F4Multiplayer.session` is taken over only by the first character that hosts with a save played in that session. If you load a solo save first, or a co-op save made before you ever hosted, that character starts a fresh session and the old file is left where it is. If last session's kills and loot are gone, see "Coming from 0.8.5 or earlier" in INSTALL.md: renaming the old file by hand gives it back.
- Join with the character you will play. A friend who loads a save of another character than the one they joined with leaves the session ("you loaded another character"). Accepting an invite in a solo save and then loading the co-op save no longer works: load the co-op save first.
- With `bHost = true`, hosting starts once you are in the world and out of Vault 111. A friend who tries before that is told "The host isn't in the game yet", and their game tries again a few seconds later.
- The session file keeps the one before it as `.bak`. A damaged one is renamed `.corrupt`, and the host carries on from the backup.
- `bRunInBackground` is on by default: while you are in a session, the game keeps running when you alt-tab, so friends still see you and the NPCs your game runs. You can be hurt meanwhile. Playing alone, alt-tab pauses the game as before.
- The plugin log names other players as "player N", Steam accounts by a short alias and IP addresses masked. The last 3 logs are kept (`F4Multiplayer.1.log` is the one before this launch). `sLogLevel = debug` makes the log more detailed.

What changed:

- Weapons and armor are no longer shared between games: a gun your friend loots stays on your copy of the corpse, and a gun or armor piece put in a chest stays out of your friend's chest. Only the item's base type travelled, so your friend got a plain copy, and taking one could remove a legendary or modded piece from their game instead of the plain one.
- Also each player's own: paper notes, holotapes, keys, bobbleheads and magazines (a note your friend takes stays for you to read), vendors and caps (selling to a vendor leaves your friend's vendor with the same caps and stock), what companions, settlers and any living NPC carry, settlement workbench storage and power armor frames. Shared as before: ammo, chems, food, junk and other loot in containers and on corpses.
- Loot is no longer lost or repeated when changes arrive out of order: a friend's loot made while your save loads lands after what they took before it, a change that went missing is asked for again, and after a host's crash the next loot by either player still shows up in the other game.
- Loading a save made alone, or in another session, while in a session no longer brings back the whole session's deposits on every later reload.
- Loot taken in the moment before a save or a reload reaches your friend instead of being forgotten.
- NPCs killed in a session come back when their cell resets in the host's game, as they would playing alone, instead of staying dead for the whole session.
- Enemies dead in the host's world stay dead for friends. Settlement junk the host scrapped is gone for friends too (never a container you keep things in).
- Quest NPCs a friend has moved on (Preston's group in Sanctuary) show up for you even if your copy of that quest hasn't.
- A friend's grenade kills the enemies beside you in your game, with friendly fire off too. It also hurts you if it lands at your feet. Nothing bigger than a thrown explosive is set off for a friend.
- Leaving and rejoining no longer gives a joiner a fresh budget of NPC deaths and explosions to report.
- Power armor: friends show in their suit again, and a suit taken from the world isn't seen twice. A friend's Pip-Boy light follows them.
- Friends no longer walk on the spot while standing, and a friend's Dogmeat gets up and follows.
- A button, switch or sink your friend presses is pressed by their stand-in in your game, not by you: drinking at a sink no longer gives you the rads.
- Saves and quicksaves no longer crash on a friend's stand-in, and saves already affected heal. No multiplayer work runs during saves and loads (this fixes crashes on load), and quitting the game no longer crashes on the way out.
- Nothing is written to the HUD for six seconds after a game load, when the engine rebuilds its menus. The host crashed in the HUD text layout three seconds after loading a save; the loading-screen guard from 0.8.5 ended too early for that case.
- The F1 menu's first button is always Close or Back. Tab presses a message box's first button, so leaving the menu with Tab could start hosting, leave the game or stop hosting by accident, and a host who did that lost shooting, the Pip-Boy and the map until a restart.
- Semi-auto pistols sound like single shots.
- A freeze writes a short report. Slow saves and loads no longer count as freezes.
- NPC voice lines now need `bExperimentalVoiceLines = true` in the ini.
- Newer builds are on GitHub first; Nexus follows less often.
