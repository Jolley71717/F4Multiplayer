## 0.11.1

A patch for 0.11.0: friendly NPCs that stood silent in a friend's game when the other player was off elsewhere, a friend's lights that stayed dark, crafting that
ignored the settlement's junk, and a batch of smaller fixes. Install it over 0.11.0. (This is the same file as the
0.11.1-rc1 test build, so seeing "rc1" in your log is normal.) It uses the same protocol (57), so it plays with 0.11.0
and a session saved by 0.11.0 loads as it is. It is best if everyone in a session has it, since several of the
fixes need both games to carry them.

New:

- A friend's NPC lines now show in the game's own subtitle area, timed with the speech, instead of short notices at
  the top left that were gone before you could read them. This needs General Subtitles on in the game's settings; with
  it off, or while you are in a conversation yourself, you still get the notices.

Fixed:

- Fewer friendly NPCs standing in silence in a friend's game. An NPC runs its own AI in one game only, and until
  now it went to the nearer player only once the other was very far away, so one player often found town NPCs that
  never greeted or turned to them. A friendly NPC now goes to the game of a friend standing next to it when you are
  well off (about 2000 units away). An NPC your game takes over also drops the do-nothing behaviour its copy had and
  goes about its own business.
- Crafting at a station a friend built now counts the junk stored in your settlement's workbench, not only what you
  carry.
- A friend's lights now light in your game once they are powered. Many of the workshop's lights (the construction
  light, the light box, the high-tech lights, the strobe, the fan light and the mirror ball) stayed dark when
  they were already powered as they appeared. A friend's light bulb now also casts its light.
- A friend's built pieces stay tracked when their cell unloads and loads again: their power, scrapping and moving
  keep working, and leaving a session cleans them up. Before, a piece in a cell you had left could be lost track
  of, and a second copy made over it.
- Dropping an item you picked up from the world (a stimpak lying in Sanctuary, say) is now shared: your friend sees it
  land.
- A killing blow of yours on a friend's NPC now reaches the game that runs it, so a Mr Gutsy no longer stays alive
  on a sliver of health in its own game after a long fall.
- Two games loading the same new outdoor area at once could build a raider from different templates. Both now read
  the area's level zone itself, not the game's copy of it, which isn't set yet while the area loads.
- Catching up a friend's quests no longer leaves a menu open in a joiner's game: the settlement list a stage opens by
  itself (Getting a Clue, when Nick becomes the new companion) and the item inspection some stages open are closed
  for you.
- Side quests that the game starts with targets it picks for each player (robot assaults, Far Harbor and Nuka-World
  settlement and gang jobs, Saugus Ironworks, Weston Water Treatment) are no longer shared, so a friend's
  stage doesn't start your copy with your own targets. In the Automatron and Nuka-World story quests, a friend's
  stage no longer moves you when you aren't with them, or takes caps or items from you.
- Getting into a power armor frame is safer: a rare chance of reading the game's data at the wrong moment is gone.
- After being downed and helped up, the downed camera and the input lock are now ended if the game left them on.
  A player saw this as a camera stuck underground and a Pip-Boy that wouldn't open, after a grenade both knocked them
  over and downed them. We couldn't make it happen on our test machines, so tell us if you still see it.
- The install guide says what to check when a Steam invite arrives but joining does nothing.

Known issues:

- When both players are near a friendly NPC, the NPC still runs in one game only, and in the other it may not greet,
  idle or work.
- A friend's lights light in your game once that friend has been to the settlement in this session.

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

Reporting a problem:

- Install a crash logger (Buffout 4 or Addictol), or a crash leaves nothing to read.
- Report on GitHub with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or
  `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as `player N`
  and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
