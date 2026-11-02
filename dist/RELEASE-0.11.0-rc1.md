## 0.11.0-rc1

A test build of 0.11.0. Install it over 0.10.1. It uses a new protocol (57), so it doesn't play with 0.10.1 or
anything older: everyone in a session needs it, and a friend still on 0.10.1 can't join. Update together. The host's
session file from 0.10.1 still loads, but a session saved by this version can't be loaded by 0.10.1 or older: keep a
copy of the session file before updating if you might go back.

New:

- Every game now builds an NPC from the same roll. Until now each game picked a leveled NPC's look, gear and
  legendary status itself, at its own player's level, so friends saw different raiders, a legendary in one game was
  plain in another, and a scene that places its NPCs at runtime could end up with a copy per player. The session now
  rolls each one once and every game builds the same NPC. It is rolled at the average level of the players there, or
  the host can choose the highest, the host's own, or each game's own (`sNpcLevel` in the ini). The chance of a
  legendary follows the players' difficulty the same way: their average by default, or the host's, or each game's own
  (`sLegendaryDifficulty`). An NPC brought back by a cell reset is rolled anew, the same for everyone.

Fixed:

- A mine a friend walks onto now gives both of you the full warning beep. Before, their copy in your game went off
  almost at once, and its report cut the warning short in theirs too.
- Catching up a friend's main quest no longer pulls a joiner into Kellogg's memory. Catch-up now never sets a quest
  stage whose scripts move the player (the memory lounger, DiMA's memories, Vault 118, the pre-war opening).
- A leveled NPC that only a friend's game has, like the Mr Gutsy a random encounter puts on the road, now shows up in
  yours, with its fight.
- A teleport to a friend far across the map now lands next to them: their cell is loaded first. The autosave after a
  teleport waits until you are standing on the ground, so it can no longer save you mid-fall and loop you into a
  fall death on every load. A fall after a teleport should do no damage.
- On some starts a player had no input at all (no walking, looking or shooting, though menus worked). The game had
  marked the player as in a synced animation with none running; the mark is now cleared at game start and on load.
- A friend who sneaks now sneak-walks instead of gliding along crouched.
- A game that joins late, rejoins after a crash or loads a save is now told how hurt the NPCs its friends run are,
  instead of showing them at full health.
- A piece a player built that their own world no longer has (they loaded a save from before they built it) is now
  taken out for everyone, instead of staying in every friend's game and coming back at every join. If a later save
  has it again, it comes back.
- Getting into a power armor frame is safer: the check of whose frame it is no longer reads game data from the wrong
  thread.

Known issues:

- A joiner caught up past Dangerous Minds misses the companion's talk after the surgery: that stage is skipped
  because it would move the joiner into Kellogg's memory.
- Settler jobs aren't shared: each game keeps its own. A friend sees the settler go to work but not the job.
- Storing a pre-placed settlement crop during a session removes it from the host's world but not the friend's.
- Mines were tested indoors with frag mines; other mine types and outdoor mines haven't been tested yet.
- A robot such as a Mr Gutsy that a friend's game runs can stay alive at low health in their game when you land the
  killing blow: its long fall outlasts the time the kill is accepted.
- Pieces built before 0.11.0 aren't covered by the fix for built pieces a builder's save no longer has: one that is
  already stuck in a friend's game stays.
- A late joiner still sees an enemy a friend's game spawned (not one from the world) at full health until it is next
  hurt.
- Players whose mods change the leveled lists differently can still see different enemies.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Worth trying: fights with leveled raiders and legendaries (both players should see the same enemy), walking onto
  mines with a friend nearby, a teleport to a friend far away, and a late join to a fight in progress.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
