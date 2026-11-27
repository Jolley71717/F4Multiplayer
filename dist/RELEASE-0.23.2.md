It uses the same protocol as 0.23.0 and 0.23.1 (86), so it can play with them. Install it over 0.23.1.

Fixed:

- **The Brotherhood of Steel quests no longer split between players.** In Fire Support and Call to Arms, the scenes with Haylen and Rhys at the Cambridge Police Station ran separately in each game, so the friend's game fell behind and Danse had nothing to say to the friend when it was time to head to ArcJet Systems. Those scenes are now shared like the rest of the quest. Other quests built the same way are covered too.
- **A crash when a friend joins is fixed.** The host's game could crash while the friend's loot was being caught up, because the game was changed before the looted character had finished loading. Every change the mod makes to a character's items now waits until that character is ready, on both sides.
- **A version mismatch now says who needs to update.** When the host and the joining player run different versions, the message names both versions (for example "The host has F4Multiplayer 0.22 and you have 0.23.2") and says who should update, instead of showing protocol numbers.
