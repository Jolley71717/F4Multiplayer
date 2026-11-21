## 0.21.0-rc1

A test build of 0.21.0. It uses the same protocol as 0.20.0 to 0.20.4 (84), so it can play with them, but the fixes below need everyone in the session on 0.21.0. Install it over 0.20.4.

New:

- **Friends on the Pip-Boy's map.** Each friend is marked on your world map and local map, in their compass color with their name beside it, and the mark follows them while the map is open. A friend inside a building shows on the world map at its door out, faded, as the game shows you. Nothing is added to your load order or your save. To turn it off, set `bFriendsOnMap = false` in `F4Multiplayer.ini` (or `bFriendsOnMapNames = false` to hide just the names).

Fixed:

- **Dismissing a companion no longer traps a friend in a conversation.** While one player had the settlement list open to send a companion away, a friend's game could start that companion's first meeting on its own, a conversation with no way out. Friends now hold still while that list is up, as they do while you're paused.
- **Diamond City's synth accusation plays once for everyone.** The scene with Kyle and Riley at the market used to play separately in each game, so a friend who came later saw it start over or found the two already dead. Now the first player to walk into the market runs it, and everyone else sees that same scene.
- **Your own save no longer keeps the host's settlement pieces.** After building or playing at the host's settlement (with Sim Settlements 2's city planner's desk, for one), a save you made could keep copies of the host's pieces that nobody could use or scrap once you loaded it alone. Saves made with 0.21.0 now clear those copies when you load them alone. Copies already in an older save stay until you join the host there in a session and leave again.
