## 0.12.0

Shared workshop materials, a friend's settlement pieces counting for both of you, more quests that can be shared, and
seated friends shown sitting. Install it over 0.11.2. (This is the same file as the 0.12.0-rc1 test build, so seeing
"rc1" in your log is normal.) It uses a new protocol (60), so it doesn't play with 0.11.2 or anything older: everyone
in a session needs it, and a friend still on 0.11.2 can't join. Update together.
New:

- **Shared workshop materials.** With the host's `bSharedStorage` on, the workbench at each settlement the host owns
  is one shared pile of building materials for both players. Junk either of you scraps or stores there is there for
  both in workshop mode and at that settlement's crafting stations, and a build or craft uses it up once. Each game's
  daily food and water, and each player's own weapons and armor, stay their own. While the host is away or has just
  loaded a save, a friend builds from their own materials and a notice says so.
- **Settlement resources count for both players.** A friend's water pumps, crops, generators and defenses now count
  in your settlement's numbers too (the Pip-Boy's workshop page and the workshop menu), not only in theirs. They come
  off when the friend leaves, starts a new game or loads a save without those pieces, and they are never written
  into your save.
- **More quests can be shared.** With the host's `sStory = shared`, the new `sQuests = safe` (or "More quests" in
  the host's F1 menu) also shares 92 more quests that a check finds safe to share: Diamond City's and Goodneighbor's
  errands, the "talk to" pointers, most of Far Harbor's own quests, and two of Nuka-World's. Quests that move the
  player, change companions, turn a faction hostile, change a settlement's owner or empty your inventory are left
  out. The default stays `story`: nothing changes unless the host turns it on.

Fixed:

- A friend who sits down no longer shows standing beside the seat when you load, join or come back into the cell
  while they're already sitting.

Known issues:

- Supply lines aren't shared yet: the host's supply lines feed only the host's builds.
- The robot workbench isn't shared yet.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or
  food in your workbench: they count in the numbers only.
- If a friend also owns the host's settlement in their own save, the host's pieces count in both games' production
  there.
- Players whose mods change the leveled lists differently can still see different enemies.

