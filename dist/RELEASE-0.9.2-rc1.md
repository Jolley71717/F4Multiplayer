## 0.9.2-rc1

A test build of 0.9.2. Install it over 0.9.1. It uses a new protocol (50), so it doesn't play with 0.9.1:
everyone in a session needs it.

New:

- Friends see missiles, Fat Man shots, shells and explosive rounds fly. Each still explodes once.
- Doors that chain to others, doors a script opens quietly, two-state gates and doors NPCs open are shared.
- World traps are shared: mines, tripwires, bear traps and grenade bouquets go off once, in every game.
- Cars, barrels and fuel tanks a player shoots are wrecked in every game and explode once in each.
- Objects an NPC or an explosion knocks about move in every game.
- A friend's frame copy of power armor can't be entered, opened with Transfer, or given to a companion. Borrowing a
  friend's power armor is coming in a later version.
- A friend somewhere your game doesn't have (a DLC you don't own) shows as out of reach: the player list says so, their
  compass marker goes, and teleporting to them is refused until they're back.
- The host can allow more than 4 players (`iMaxPlayers` in `F4Multiplayer.ini`). More than two has been tested less.

Fixed:

- Power armor frames no longer duplicate. A friend's frame copy, or the copy of a friend's character, could survive
  loading a save and end up in your next save; now a load removes every copy the mod placed.
- Loading a save twice in one run could crash when the save held a leftover copy of a friend's character. New saves no
  longer get one. If an older save has one, the mod says so after it loads: save, restart the game and play from the
  new save.
- Every explosion hits once: missiles, grenades thrown at a friend, cluster rockets, a robot's self-destruct, a
  suicider's nuke.
- A friend's shot no longer hits you on your screen: their figure in your game fired real shots at you. Their shots now
  follow where they actually aim. A gun bash is no longer heard as a gunshot, and the clicking that could start after a
  join or a leave is gone.
- A hit that takes an enemy over from a friend's game is no longer healed back by a late health update from that game.
- An enemy is the same in every game: no more legendary in one game and plain in the other, or a settler in one and a
  scavenger in the other.
- Settlers can be given jobs during a session.
- An item a friend dropped and you scrapped in workshop mode no longer comes back (a dupe).
- Taking the last item from a container in a home no longer empties a friend's own stash in it.
- Radiant quests are remembered across loads, restarts and sessions, and yours is no longer stopped when a friend's
  can't start in your game.
- A friend's Codsworth search reaches the host's story in order.
- A player who loads a save in power armor is seen in it.
- A locked session takes back players whose game crashed or whose connection dropped; new, kicked and banned players
  still can't join.
- Friends' figures no longer stutter while someone uses Jet or VATS.
- The revive count and cooldown carry through a reconnect, and pings pressed a second apart all go through.

Known issues:

- Extra power armor frames that 0.9.1 left in your world stay. Strip them, or remove one in the console: click it,
  then `disable` and `markfordelete`.
- In rare cases an explosion can still hit twice, or not at all.
- A missile a friend sees in flight can drift a little from the real one, and vanishes if you save while it flies.
- A save made in the few seconds a companion is stepping out of a friend's frame can leave one extra frame.
- Enemies from a DLC you don't own aren't shown in your game, but their explosions can still reach you.
- Two players using the same door at the same moment can see it differently.
- Storage isn't one shared copy yet: what you put in or take out is sent to friends' games as before. Shared storage is
  planned.
- Giving a friend an item is still not possible.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
