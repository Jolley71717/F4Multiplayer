## 0.21.2

It uses the same protocol as 0.20.0 to 0.21.1 (84), so it can play with them, but the fixes below need everyone in the session on 0.21.2. Install it over 0.21.1.

Fixed:

- **The host's game no longer crashes when enemies down a friend's figure.** In Nuka-World's RobCo Battlezone, the host's game could crash the moment the robots took down a friend's figure. The same could happen anywhere a friend's figure was replaced in the middle of a fight.
- **A friend's figure no longer dies in your game while your friend is alive.** Enemies, explosions, falls and traps used to kill a friend's figure in the host's game, and the game then removed it and put a new one in its place. Now it stays up, and the hits still reach your friend as before.
- **RobCo Battlezone's waves wake in both games.** With the default quest setting, a wave started by one player left the robots asleep in the other player's game, and the friend's shots couldn't hurt them. Now a wave wakes in both games, whoever starts it. The same goes for the other fights that come in waves at one place, such as the Castle's mirelurks, Rotten Landfill and Far Harbor's docks.
- **Quest enemies are the same ones in both games.** Some quests make their enemies separately in each game: a group of enemies, raiders a quest's script puts down, or the enemies of a quest your session doesn't share. Each game used to keep its own, so a friend saw them twice or not at all, and their shots missed. Now each enemy is the same one in both games, and both players' hits land.
- **An enemy that can't die stays alive in both games.** An NPC with endless health used to drop to no health in a friend's game, and the friend could kill it there and in the host's game too. Now it stays standing in both.


This build is identical to 0.21.2-rc1: its log still names rc1.
