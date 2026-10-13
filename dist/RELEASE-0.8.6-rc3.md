## 0.8.6-rc3 (release candidate)

A candidate for 0.8.6. Install a crash logger (Buffout 4 or Addictol) before trying it, and report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as "player N" and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.

Protocol 28: everyone needs this build (0.8.5 and the earlier 0.8.6 rcs can't join).

Coming from 0.8.5 or rc2:

- Each character now has its own session file (`F4Multiplayer.<character>.session`). The old `F4Multiplayer.session` is taken over only by the first character that hosts with a save played in that session. A solo save, or a co-op save made before you ever hosted, starts a fresh session and leaves the old file where it is. If last session's kills and loot are gone, see "Coming from 0.8.5 or earlier" in INSTALL.md.
- Join with the character you will play: a friend who loads another character's save leaves the session. Load the co-op save before accepting an invite.
- With `bHost = true`, hosting starts once you are in the world and out of Vault 111; a friend who tries earlier is told "The host isn't in the game yet" and tries again a few seconds later.
- The session file keeps the one before it as `.bak`, and a damaged one is renamed `.corrupt`.
- `bRunInBackground` is on by default: in a session the game keeps running when you alt-tab, and you can be hurt meanwhile. Alone, alt-tab pauses as before.
- The last 3 plugin logs are kept, and `sLogLevel = debug` makes them more detailed.

What changed:

- Weapons, armor, notes, keys, magazines, vendors, workbench storage, power armor frames and what living NPCs carry stay in each player's own game; ammo, chems, food and junk are shared as before.
- Loot is no longer lost or repeated when changes arrive out of order, around a save, a reload or a host's crash.
- NPCs killed in a session come back when their cell resets in the host's game. Enemies dead in the host's world stay dead for friends.
- Quest NPCs a friend has moved on (Preston's group in Sanctuary) show up for you even if your copy of that quest hasn't.
- Shared quests move on stage by stage in order, with each stage's own scripts and achievements, and wait while you are loading, paused, talking or haven't started the quest. A friend's stage never takes your caps, and moves you only when you stand with them (the Institute teleport warns you first, then takes you along). Nothing from a session is set in your game after you leave, except finishing a teleport already under way, and your solo progress never reaches a session you join later.
- Settlement junk the host scrapped is gone for friends too (never a container you keep things in).
- A friend's grenade kills the enemies beside you with friendly fire off too, and hurts you if it lands at your feet.
- Leaving and rejoining no longer gives a joiner a fresh budget of NPC deaths and explosions.
- Power armor: friends show in their suit again, and a suit taken from the world isn't seen twice. A friend's Pip-Boy light follows them.
- Friends no longer walk on the spot while standing; a friend's Dogmeat gets up and follows.
- A button, switch or sink a friend presses is pressed by their stand-in, not by you.
- Saves and quicksaves no longer crash on a friend's stand-in; saves already affected heal.
- No game work during saves and loads (fixes crashes on load). Quitting the game no longer crashes on the way out.
- Semi-auto pistols sound like single shots.
- A freeze writes a short report; slow saves and loads no longer count as freezes.
- NPC voice lines now need `bExperimentalVoiceLines = true` in the ini.
