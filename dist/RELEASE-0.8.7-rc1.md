## 0.8.7-rc1 (release candidate)

A candidate for 0.8.7, which takes the place of 0.8.6: it holds everything in 0.8.6-rc4 and what is below. Install a crash logger (Buffout 4 or Addictol) before trying it, and report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as "player N" and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.

Protocol 31: everyone needs this build (0.8.5 and the 0.8.6 rcs can't join).

What changed since 0.8.6-rc4:

- The story is shared by default: when any player moves the main story or a faction quest forward, everyone else is fast-forwarded to the same point (`sStory = shared`). The host can set `sStory = own` to have each player play the story in their own game, as before. If you keep your old `F4Multiplayer.ini` instead of the new one, it still says `sStory = own`: change it to `shared`, or switch "Shared story" on in the F1 menu. The dedicated server shares the story too unless started with `--own-story`.
- Diamond City's gate no longer stays shut for friends: with a shared story, when one player hears Piper argue with Danny Sullivan over the intercom and the gate opens, it opens in everyone's game, and a friend who joins later finds it open.
- A friend who sits on a chair, couch, bench or stool, lies in a bed or sleeping bag, or works at a workbench, a station or a terminal is shown doing the same in your game, and gets up when they do. Their stand-in used to stand on the couch they sat on; where your game can't put it in the furniture, it waits beside it instead.
- You hear what's said near you in a friend's game, not just read it: the lines of NPCs they talk to, and their own character's lines, play in the speaker's voice from where the speaker stands, fading with distance. It is a plain sound, so it can never leave you stuck in a conversation without your weapons, Pip-Boy or workshop. It is on by default (`bSpokenLines`, also "Spoken lines" in the F1 menu); the old `bExperimentalVoiceLines` setting is no longer read.
- A friend walking in power armor no longer glides along standing still on your screen: if their suit plays no walk, it is put on them again within a couple of seconds and they walk and run as they do out of it.
- Enemies that appear mid-game (random encounters, ambushes) can no longer be worn down in one game by something the other game never saw, which could leave one dead for you while it kept fighting your friend.
- A friend's thrown grenade no longer leaves a grenade warning stuck on your screen.
- A grenade goes off once in your friends' games: throwing grenades a few seconds apart could set an earlier one off a second time for them, and a grenade could go missing for them after one of theirs went off near you.

Known issues:

- Two players opening or closing the same door at the same instant can end up seeing it differently; using the door again puts them back in step.
- Getting into power armor was refused in testing ("You cannot use this at this time") in and just outside Diamond City, for a reason not found yet. If it happens to you, say where you were and what you did just before.
- A friend's face can look a little different in your game than in theirs: skin tone and face paint aren't carried over yet.
