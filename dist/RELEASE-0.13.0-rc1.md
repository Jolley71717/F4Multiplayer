## 0.13.0-rc1

A test build of 0.13.0. Install it over 0.12.1. It uses a new protocol (62), so it doesn't play with 0.12.1 or
anything older: everyone in a session needs it, and a friend still on 0.12.1 can't join. Update together.

New:

- **Supply lines are shared.** With the host's `bSharedStorage` on, a friend building at one of the host's
  settlements now also uses the materials at the host's settlements that the host's supply lines reach, the same way
  the host's own builds do. Before, a friend's game followed its own save's supply lines and took from its own stash.
- **The robot workbench is shared.** Automatron's robot workbench reads the shared pile like the other crafting
  stations.
- **Gun Run (Salem).** The three mirelurks are now the same creatures in both games: shooting them in either game
  counts, and the quest moves on when all three are dead. Before, a friend's game had idle copies that took no damage.
- **Easy City Downs.** Choices made at the robot race's terminals (start or stop the race, targeting, reserves and
  the rest) are shared with friends in the session, with `bSyncQuests` on.

Fixed:

- A workshop piece built just before loading another world could be removed for everyone. It is now kept.
- A placed item (a weapon on a table, say) that you picked up and later dropped again could vanish from your own world
  when you rejoined. It now stays where you dropped it.
- The host's open workshop list now updates at once when a friend takes materials from a far settlement linked by a
  supply line. Before, it kept counting them until the host left workshop mode.

Known issues:

- Workshop conveyor belts (Contraptions) run in the builder's game but not a friend's. A fix is waiting for a test.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or
  food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Worth trying: building together at a settlement linked to others by supply lines (both of you in workshop mode),
  the robot workbench, Gun Run with a friend, and the Easy City Downs race.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
