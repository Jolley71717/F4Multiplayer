## 0.11.2-rc1

A test build of 0.11.2. Install it over 0.11.1 or 0.11.0. It is a patch: it uses the same protocol (57), so it plays
with 0.11.1 and 0.11.0, and a session saved by them loads as it is. It is best if everyone in a session has it.

Fixed:

- A game could crash when an enemy's hit on a friend's character arrived just after that character's stand-in was
  replaced (seen with Nuka-World raiders firing at a friend). The game no longer runs that hit's reaction (the victim
  turning to fight back) on a stand-in that is already gone; the hit itself still lands. We couldn't make the crash
  happen on our test machines, before or after the fix, so this guards against the cause the crash log showed.
- A friend's NPC line played aloud in your game now stops when its recording ends, and at most four play at once.
  Before, a line's sound was stopped only when the same speaker spoke again, so a recording that never ended on its
  own kept playing and each new speaker added to it. One player heard a loud noise grow with every friend's line;
  this may be its cause, but we haven't confirmed it on their setup.
- An enemy that a friend's game switched on through a quest (the mirelurks at Barney Rook's bunker in Gun Run) now
  comes on in your game too. Before, it existed only in the friend's game: you couldn't see it, and they fought
  something nobody else saw. Enemies and characters that a later quest stage switches off again are left to your own
  story, and with Shared story off (the host's setting in the F1 menu) nothing changes.

Known issues:

- When both players are near a friendly NPC, the NPC still runs in one game only, and in the other it may not greet,
  idle or work (talking to it works).
- A friend's lights light in your game only once that friend has been to the settlement in this session.
- A joiner caught up past Dangerous Minds misses the companion's talk after the surgery: that stage is skipped
  because it would move the joiner into Kellogg's memory.
- Settler jobs aren't shared: each game keeps its own. A friend sees the settler go to work but not the job.
- Storing a pre-placed settlement crop during a session removes it from the host's world but not the friend's.
- Mines were tested indoors with frag mines; other mine types and outdoor mines haven't been tested yet.
- Pieces built before 0.11.0 aren't covered by the fix for built pieces a builder's save no longer has: one that is
  already stuck in a friend's game stays.
- A late joiner still sees an enemy a friend's game spawned (not one from the world) at full health until it is next
  hurt.
- An item from the world picked up before you joined, or before your last load, is not shared when you drop it.
- Players whose mods change the leveled lists differently can still see different enemies.
- Misc quests are still each player's own: only the enemies a friend's stage switches on are shared. Gun Run's first
  mirelurks and the Easy City Downs robot race are still each player's own.
- Quest enemies are shared this way only when no mod changes them or their quest marker; with an unofficial patch or
  a previs mod that does, they can still exist in one game only.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Worth trying: long fights next to a friend in a crowd of enemies, a friend's NPC conversation within earshot (with
  the F1 menu's Spoken lines on), and Gun Run from Barney Rook's first stage with a friend.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
