## 0.9.0-rc3

A release candidate: everything since 0.8.11, built together and run through the rig's full set (movement, Corvega
combat, terminals and loot, grenades, power armor, a cell churn with save reloads, a reconnect and the fuzz pass) with
no crash on either machine. Install it over 0.8.11, rc1 or rc2 and say what holds and what doesn't.

Everyone needs the same build to play together: it uses protocol 49, so a friend on 0.8.11 or 0.9.0-rc1 gets
"Version mismatch" and has to update too (rc2 and rc3 can play together). A saved session from 0.8.11 or older still
loads.

New since rc2: a friend in power armor walks in it instead of vanishing out of it, a downed friend is shown lying down
until helped up, a save made with a friend's stand-in next to you no longer crashes the game, a quickload no longer restarts the session when you have mods, and taking only a weapon,
armor or key from a chest or corpse leaves it bare for friends too.

New since rc1: every faction's radiant quests shared (not only the Minutemen's), much smoother movement for friends,
a friend's Pip-Boy raised on their stand-in and menus named on their tag, host-tuned revives, VATS without slowing
your friends, tutorial hints off, looted chests and corpses bare for everyone, shared plant picking, and Linux and
Proton notes in the log and in INSTALL.md.

New:

- Different mods, or the same mods in another order, can play together. Every form the games talk about is now
  numbered by the session, so load orders no longer have to match. What only one game has is skipped, never guessed:
  the joiner is told which mods only one side has, and the rest plays normally.
- Give each other items by dropping them. Drop a weapon, armor, aid or junk and your friend sees it on the ground;
  whoever presses Take first gets it, and it exists in one place only, through saves and reloads on either side. Keys,
  notes, quest items and items made at runtime are refused, and the giver is told. Putting a weapon or armor in a
  container shows a hint to drop it instead.
- Host controls in the F1 menu: kick, ban (kept across restarts), lock the session, and make up or remove a password.
  A dedicated server has the same commands at its console (`help` lists them).
- Where is everyone: F10 (`iKeyWhere`) prints each friend's distance and direction on the HUD ("Sam: 240 m NE").
- Better pings: F8 pings what your crosshair is on (an NPC, an item, a friend or a spot), puts a ring on everyone's
  compass in your color for 15 s, and names it with its distance and direction.
- Shared radiant quests: when one of you is handed a repeatable job whose targets are the game's own places and
  people, everyone gets the same quest for the same settlement, place and target, and each player is paid once. That
  covers most of the Minutemen's settlement requests, the Brotherhood's (BoSR01, BoSR02, BoSR05), the Institute's
  (InstR01, InstR02, InstR05), the Railroad's (RRR01 to RRR06, with the HQ's job count kept right) and Diamond City's
  wanted posters. `bSharedRadiants` turns it off.
- Smoother friends: each state is stamped with the engine frame it was read in, so a friend's character moves about
  two-thirds less unevenly on every connection, and carries on through a late update instead of freezing and leaping.
  How far behind a friend is drawn adapts to the connection: on a good link about 100 to 150 ms instead of 50, more on
  a worse one, and it shrinks again when the link recovers. After a join, friends are no longer drawn 1.5 s
  behind.
- A friend in their Pip-Boy raises their wrist on their stand-in, and their name tag says "[Pip-Boy]" or "[in a menu]".
- Session rules the host sets: how long a downed player bleeds out, how long a revive takes and the health it gives,
  how many revives per fight or per window, a cooldown after getting up, and whether helpers can revive during a fight
  (`iBleedOutSeconds`, `fReviveSeconds`, `iReviveHealth`, `iMaxRevives`, `iReviveWindowMinutes`,
  `iReviveCooldownSeconds`, `bReviveOutOfCombat`). The HUD says how many revives are left.
- VATS in real time (`bRealTimeVats`, on by default): your VATS no longer slows the game for your friends. Jet still
  slows time as it does alone.
- Tutorial hints are off during a session (`bHideTutorials`) and come back afterwards.
- Linux and Steam Deck: the log says whether the game runs under Proton, whether Steam started it, why Steam isn't
  ready if it isn't, and how the hotkeys are read. INSTALL.md has a Linux section on starting F4SE through Steam.
  Accepting a friend's invite while you're hosting now tells you to set `bHost = false`, instead of doing nothing.
- Security gates opened from terminals, card readers and buttons open for everyone, including for a friend who comes
  later. Vault 114's gear door (Valentine's quest) now opens for everyone too.
- Settlement building: power wires, building done before the session started, and pieces the settlement came with
  that you move now show up for your friend. Each game shows the power its builder's game reports, and a friend's
  copies can't be wired, stored or scrapped by mistake, so no settlement grid keeps a dead reference after the session.
- Objects you pick up and move by hand (a bottle, a tire), drop or throw show up moved for your friend, and things
  you knock over land in the same place for a friend nearby.
- A friend's LooksMenu tattoos, face and body paint and skin override show on their stand-in, when both games have
  LooksMenu and the overlay mods.
- A friend's grenade, mine or Molotov shows in their stand-in's hand through the throw (`bGrenadeInHand`).
- Muzzle flash on every replayed shot of friends and mirrored NPCs.

Fixed:

- The black screen on joining: the vault's own quest stages (Vault 111's pod, lift and mirror scenes) are no longer
  shared, since every player goes through the vault on their own path, and a fade a quest left up is faded back in.
- A Molotov's fire, or a friend's grenade, no longer heals an NPC back or hits it twice: the NPC's own game shares its
  health whenever it changes, and the others report the damage they dealt.
- Crashes taking from some containers and using terminals (Red Rocket's desk among them) are hardened against, and a
  friend's looting of a container you aren't near waits until you come back.
- "You cannot use this at this time" on a power armor frame that leans, or whose seat the game still thought was
  yours.
- A chest or corpse a friend looted bare is bare in your game too, even though each game rolls its own loot. Weapons,
  armor, notes, keys and quest items stay each player's own, as before.
- A plant a friend picks is picked in your game too, and two players picking the same plant harvest it once.
- Dogmeat's copy runs instead of squatting along at a run's pace.
- Damage counts on enemies again after loading a save, and no ghost enemies after a reload.
- Alt-tabbing out of the game no longer makes your character fire or move at clicks and keys meant for another window.
- A downed player the game stood back up (in a pile of clutter, or after a save) is put down again and keeps waiting for
  help.
- No emotes in power armor (they slid the view sideways there), with a line saying why.
- A friend's gun is in one piece on their stand-in after they swap it for another copy of the same gun.
- Friend markers and pings show on the power armor compass too.
- A friend in power armor walks in it, instead of sliding along still or popping in and out of view without it.
- A downed friend's stand-in lies down until they're helped up, instead of standing.
- A crash on saving (manual, quick or auto) while a friend's stand-in was touching you.
- With a mod plugin installed, loading a quicksave (or any save made after the mod was added) could take the save for
  another character: the host restarted the session and friends had to rejoin, or a friend left it. The session file
  for your character is kept and renamed the first time you host with this version.
- Taking only a weapon, armor, a key or a note from a chest or corpse, when nothing else shared is left in it, leaves
  it bare for friends too; before, their copy kept its own junk.
- Settlement beds and containers left mid-move when workshop mode closes are drawn where they really stand, and
  crafted containers that come stocked (a bureau) reach your friend, emptied.

Known issues:

- LooksMenu overlays follow LooksMenu's published source and haven't been tried with LooksMenu installed yet;
  reports welcome.
- Only Vault 114's gear door and the plain terminal and card-reader gates are shared. Doors whose scripts also start
  scenes or move NPCs stay each player's own (`docs/script-doors-survey.md` lists them).
- Radiant quests whose enemies or helpers are made on the spot stay each player's own: settlement attacks and the
  Castle's defense, rescues and escorts (MinRecruit08/09, BoSR03/04, InstR03/04, RRR07 to RRR11), random-encounter
  quests and the DLCs' radiants.
- The Pip-Boy and other menus still pause your own game.
- The Linux notes in the log haven't been tried on Linux yet; logs from Linux players are welcome.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
