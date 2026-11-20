## 0.20.3-rc2

A test build of 0.20.3. It uses the same protocol as 0.20.0, 0.20.1 and 0.20.2 (84), so it can play with them. Install it over 0.20.2.

Fixed:

- **Downed mode works again after DiMA's memories.** In Far Harbor, after a player went into one of DiMA's memories, every lethal hit killed that player outright instead of downing them, in the session and after any later load. The memory left the player marked "not essential" on the way out. Downed mode now takes that mark away a few seconds after it appears, and your save is mended the next time you load it in a session.

- **Going down inside DiMA's memories works like the unmodded game.** A lethal hit inside a memory used to leave you on the ground for good, where no friend could reach you, until you loaded a save. Now downed mode stays out of the way inside a memory: answering No on the memory's "exit the simulation?" box gets you back up with full health, as in the unmodded game, and answering Yes ends it as the unmodded game does: you die there and load your last save.
