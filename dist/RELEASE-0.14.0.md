## 0.14.0

Go with your friend, more shared quests, and a shared quest's creatures the same in both games. Install it over 0.13.2. (This is the same file as the 0.14.0-rc2 test build, so seeing "rc2" in your log is normal.) It uses the same protocol (62) as 0.13.x, so a friend still on 0.13.2 can join, but the two versions share different quests: update together.

New:

- **Go with your friend.** When a friend's quest step takes them somewhere (into Kellogg's memory, through the Institute's teleporter, into a cave) and you are far away, you now get 90 seconds to hold the teleport key and go with them. The step happens for you when you arrive beside them. Before, a player who wasn't nearby never got that step at all. When the step starts a fight that turns a faction against you, the offer says so ("is fighting in ...: join them"); stay away and that faction stays as it was in your game.
- **More quests are shared** with the host's `sQuests = safe` (and the story shared):
  - 106 quests without objectives: a place's own events (Easy City Downs, Vault 114), the Castle's defence waves, the Prydwen's arrival, and the fight on Far Harbor's docks when you first arrive.
  - Ten side quests whose ending changes a faction or a settlement: Egret Tours Marina, the robot-run shops, Uncle Ken's settlement, Echo Lake Lumber, Dalton Farm, Acadia's Institute quest and four more in Far Harbor and Nuka-World. The outcome one player chooses is now everyone's.
  - The settlement requests for Saugus Ironworks and Weston Water Treatment, and two of Far Harbor's settlement requests, are shared as one run. The add-ons' shared requests are now matched whatever order the add-ons load in.
  - With `sQuests = story` (the default) nothing changes.
- **A shared quest's creatures are the same in both games.** Before, only Gun Run's mirelurks were; now the creatures any shared quest creates are paired, so a kill in either game counts for both.

Changed:

- Four quests that `sQuests = safe` shared in 0.13 are each player's own again, because they start another quest that each game rolls its own way: the Skylanes Flight 1665 follow-up, a Brotherhood follow-up quest, the Nuka-World Operators' boss talk, and one radio quest.
- Vertibirds are never paired across games; each game keeps its own (riding one together is planned).

Fixed:

- A friend opening the host's workbench after a load could wait up to a minute before the shared materials showed. It now takes a moment.
- A door two players used at the same moment could stay in the wrong state for the player who lost the race.
- Choices made at shared terminals (Easy City Downs) no longer carry over from an old session into a new one.
- Powered workshop pieces in a game started within a minute of the PC booting could miss their first redraw.
- A shared quest's creatures could die in one game and stay alive in the other when a quest character that isn't paired fought them (Gun Run's scavenger against its molerats). That character's hits now count in both games.

Known issues:

- Random events that aren't fights (a synth held at gunpoint, the Diamond City synth accusation) still play out in one player's game only. Sharing them is being built.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log.
