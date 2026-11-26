Everyone in a session needs 0.23.0: it can't play with 0.22.x (protocol 86). Install it over 0.22.2.

Fixed:

- **The host is no longer walled in at Grandchester Mystery Mansion (Nuka-World).** When the friend's part of the mansion's quest opened a door, the invisible wall in its doorway came down only in the friend's game, so the host was stopped at an open door. A wall a script takes down in one game now comes down in every game. (It never goes the other way, so nobody can be shut in.)
- **A settler's store sells its stock to the friend too.** A settler given a store at the host's settlement showed the friend only their own pockets and a few caps. The friend now trades from that store, and it follows the settler when they're moved to another store or let go.
- **One conversation per NPC.** If both players pressed E on the same NPC, both got a conversation, and the NPC could get stuck between them. Now whoever starts first has it; the other player is told the NPC is talking to their friend, and hears that conversation instead.
- **Quest NPCs no longer restart the host's conversation with the friend.** After the host finished a quest conversation, the same scene could start again in the friend's game and the NPC could wait for the friend instead of going on with the quest, which broke it for both. The friend's game now leaves that scene to the host's.
- **Gear on a friend's dead enemies no longer disappears.** A body your friend's game ran could be deleted in your game at your next save, load or area change, with everything on it. It now stays as an ordinary corpse until you loot it.
- **The friend can move more of the host's pieces in a shared settlement:** stacked pieces, wired and powered pieces, and turrets, and they can scrap or store a wire.
- **Taking items out of the host's workbench works with shared storage on.** The friend's Transfer menu showed an old list, so a take moved nothing. It now shows the workbench's real contents.
- **A container a friend builds at the host's settlement no longer comes with extra items.** It's built empty, as the workshop builds it.
- **Hits are no longer lost when another mod handles them first.** Some mods stop the game's hit events before this mod sees them, so damage on a friend's copy of an NPC could go missing. Every hit is now seen.
- **A crash when joining a session is guarded against.** Gear and items on an NPC are now changed only once the game has fully loaded that NPC.
- **NPC gear and loot from very large modded leveled lists now match in both games** (lists of more than 127 entries were skipped).

Under the hood: the mod is built on the latest CommonLibF4, and the test rig's memory check reads the right number on the second PC.
