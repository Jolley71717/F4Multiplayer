## 0.8.7

0.8.7 takes the place of 0.8.6, which was only ever out as test builds: it holds everything in them. The build is identical to 0.8.7-rc1, so its log still names `rc1`.

Everyone needs 0.8.7 to play together: it uses protocol 31, so a friend on 0.8.5 or on any 0.8.6 test build gets "Version mismatch".

Coming from 0.8.5:

- Each character now has its own session file (`F4Multiplayer.<character>.session`). The old `F4Multiplayer.session` is taken over only by the first character that hosts with a save played in that session. If you load a solo save first, or a co-op save made before you ever hosted, that character starts a fresh session and the old file is left where it is. If last session's kills and loot are gone, see "Coming from 0.8.5 or earlier" in INSTALL.md: renaming the old file by hand gives it back.
- Join with the character you will play. A friend who loads a save of another character than the one they joined with leaves the session ("you loaded another character"). Accepting an invite in a solo save and then loading the co-op save no longer works: load the co-op save first.
- The story is shared by default: when any player moves the main story or a faction quest forward, everyone else is fast-forwarded to the same point (`sStory = shared`). The host can set `sStory = own` to have each player play the story in their own game, as before. If you keep your old `F4Multiplayer.ini` instead of the new one, it still says `sStory = own`: change it to `shared`, or switch "Shared story" on in the F1 menu. The dedicated server shares the story too unless started with `--own-story`.
- With `bHost = true`, hosting starts once you are in the world and out of Vault 111. A friend who tries before that is told "The host isn't in the game yet", and their game tries again a few seconds later.
- The session file keeps the one before it as `.bak`. A damaged one is renamed `.corrupt`, and the host carries on from the backup.
- `bRunInBackground` is on by default: while you are in a session, the game keeps running when you alt-tab, so friends still see you and the NPCs your game runs. You can be hurt meanwhile. Playing alone, alt-tab pauses the game as before.
- Spoken lines are on by default (`bSpokenLines`, also "Spoken lines" in the F1 menu); the old `bExperimentalVoiceLines` setting is no longer read.
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
- Shared quests move on for everyone, whoever plays them, stage by stage in order, with each stage's own scripts and achievements; a quest a friend moves on is started in your game if you hadn't, and stages wait while you are loading, paused or talking. A friend who joins partway through is caught up to the host's story, and never set back if further along. Stages that take items (Vault 81's fusion cores) are left out. A friend's stage never takes your caps, and moves you only when you stand with them (the Institute teleport warns you first, then takes you along). Nothing from a session is set in your game after you leave, except finishing a teleport already under way, and your solo progress never reaches a session you join later. A stage you reach while your connection is being remade, a story step too, reaches your friends once you are back; after five minutes without the host you are playing alone, and nothing of it is sent when they return.
- Diamond City's gate no longer stays shut for friends: with a shared story, when one player hears Piper argue with Danny Sullivan over the intercom and the gate opens, it opens in everyone's game, and a friend who joins later finds it open.
- A friend's grenade kills the enemies beside you in your game, with friendly fire off too. It also hurts you if it lands at your feet. Nothing bigger than a thrown explosive is set off for a friend. An enemy another player's game runs is hurt in your game only by players' hits, so its own side's grenades no longer kill it in one game while it fights on in the other. Your companion's and your NPCs' hits on it count, and your kill of it counts in the other game too, with a gun, a grenade or a mine.
- A grenade goes off once in your friends' games: throwing grenades a few seconds apart could set an earlier one off a second time for them, and a grenade could go missing for them after one of theirs went off near you. A friend's thrown grenade no longer leaves a grenade warning stuck on your screen.
- Leaving and rejoining no longer gives a joiner a fresh budget of NPC deaths and explosions to report.
- Raiders and creatures a random encounter or ambush makes appear in every friend's game, and can no longer be worn down in one game by something the other game never saw, which could leave one dead for you while it kept fighting your friend. Hitting a friend's stand-in no longer turns it hostile or leaves you in combat.
- Power armor: friends show in their suit again, and a suit taken from the world isn't seen twice. A friend's Pip-Boy light follows them across cells without flickering, and a friend in power armor is drawn on the frame they wear. A friend walking in power armor no longer glides along standing still on your screen: if their suit plays no walk, it is put on them again within a couple of seconds and they walk and run as they do out of it.
- Friends no longer walk on the spot while standing, and a friend's Dogmeat gets up and follows.
- A friend who runs is shown running, not walking fast: their stand-in follows their own walk, run or sprint at the pace their game plays it, and strafes or backs up when they do. It moves evenly when their game runs slowly, and catches up within a second or two after a hitch while joining. People and creatures another player's game runs strafe and back up as they do there, and Dogmeat walks, trots and runs as he does there. A friend's stand-in aims its gun up and down where they look, and people another player's game runs aim up and down as they do there.
- A friend who sits on a chair, couch, bench or stool, or lies in a bed or sleeping bag, is shown doing the same in your game, and gets up when they do (at a terminal or a workbench they are still shown standing beside it). Their stand-in used to stand on the couch they sat on; where your game can't put it in the furniture, it waits beside it instead.
- You hear what's said near you in a friend's game, not just read it: the lines of NPCs they talk to, and their own character's lines, play in the speaker's voice from where the speaker stands, fading with distance. It is a plain sound, so it can never leave you stuck in a conversation without your weapons, Pip-Boy or workshop.
- A friend's gun shows with their own barrel, sights and receiver, and armor and power armor pieces keep their mods.
- Downed: a player knocked down with friends around waits to be helped up. A friend within a few steps helps them up after 2 seconds; after 45 seconds alone they die once and the game loads their last save. Friends never see a false "died", and pausing while you get up doesn't kill you.
- A turret or robot a friend builds in a settlement shows up in your game too. A friend's kill of an NPC that is switched off in your game leaves a corpse.
- A button, switch or sink your friend presses is pressed by their stand-in in your game, not by you: drinking at a sink no longer gives you the rads.
- Saves and quicksaves no longer crash on a friend's stand-in, and saves already affected heal. No multiplayer work runs during saves and loads (this fixes crashes on load), and quitting the game no longer crashes on the way out.
- Nothing is written to the HUD for six seconds after a game load, when the engine rebuilds its menus. The host crashed in the HUD text layout three seconds after loading a save; the loading-screen guard from 0.8.5 ended too early for that case.
- The F1 menu's first button is always Close or Back. Tab presses a message box's first button, so leaving the menu with Tab could start hosting, leave the game or stop hosting by accident, and a host who did that lost shooting, the Pip-Boy and the map until a restart.
- Semi-auto pistols sound like single shots.
- A friend calling an elevator no longer forces yours open onto an empty shaft, and doors no longer reopen themselves. The host's talk at Vault 81's intercom, and the vault door, reach friends.
- Scrapping in workshop mode, or going in and out of it, can no longer freeze the game, and leaving a settlement where a friend's stand-in stood no longer crashes the host.
- A friend's power armor stands where they parked it. Dogmeat, saved dead by mistake, comes back. A friend who shaped their body in LooksMenu has that body on their stand-in when you have LooksMenu too.
- A freeze writes a short report. Slow saves and loads no longer count as freezes.
- Newer builds are on GitHub first; Nexus follows less often.

Known issues:

- Two players opening or closing the same door at the same instant can end up seeing it differently; using the door again puts them back in step.
- Getting into power armor was refused in testing ("You cannot use this at this time") in and just outside Diamond City, for a reason not found yet. If it happens to you, say where you were and what you did just before.
- A friend's face can look a little different in your game than in theirs: skin tone and face paint aren't carried over yet.
