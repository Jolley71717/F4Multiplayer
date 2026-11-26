It uses the same protocol as 0.22.0 to 0.22.2 (85), so it can play with them. Install it over 0.22.2.

Fixed:

- **Gear on a friend's dead enemies no longer disappears.** When an enemy that your friend's game runs died, its body in your game was deleted at your next save (and when you walked far away, its area unloaded, or the session ended), taking every weapon and piece of armor you hadn't looted yet with it. Now the body stays in your game as an ordinary corpse, with everything on it, through saves, loads and travel. (One player's report: looted Brotherhood armor that vanished.)
- **Quest NPCs no longer restart the host's conversation with the friend.** After the host finished a quest conversation, the same scene could start again in the friend's game, holding the friend in it, and the NPC could get stuck waiting for the friend instead of going where the host's quest needed it, which broke the quest for both. The friend's game now leaves that scene to the host's, and the NPC goes on with the quest.
- **Taking items out of the host's workbench works with shared storage on.** The friend's Transfer menu at the host's workbench showed the friend's old list, so a take moved nothing and the menu closed. It now shows the workbench's real contents as soon as they arrive.
- **A container a friend builds at the host's settlement no longer comes with extra items.** A bureau, a locker or any container that comes stocked was filled again in the host's game when a friend built it. It is now built empty, as the workshop builds it.
- **NPC gear and loot from very large modded leveled lists now match in both games.** Lists with more than 127 entries (some mods add them) were skipped when the two games shared a roll, so the players could see different gear or loot.
- When the talk partner is guessed (a forced greeting, a scene), the NPC that is actually speaking is now taken first, not just the nearest one.

Under the hood: the mod is built on the latest CommonLibF4, and the test rig's memory check now reads the right number on the second PC.

This build is identical to 0.23.0-rc1: its log still names rc1.
