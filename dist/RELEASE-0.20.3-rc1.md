## 0.20.3-rc1

A test build of 0.20.3. It uses the same protocol as 0.20.0, 0.20.1 and 0.20.2 (84), so it can play with them. Install it over 0.20.2.

Fixed:

- **Downed mode works again after DiMA's memories.** In Far Harbor, after a player went into one of DiMA's memories, every lethal hit killed that player outright instead of downing them, in the session and after any later load. The memory left the player marked "not essential" on the way out. Downed mode now takes that mark away a few seconds after it appears, and your save is mended the next time you load it in a session.
