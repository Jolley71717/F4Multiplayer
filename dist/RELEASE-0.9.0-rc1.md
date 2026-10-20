## 0.9.0-rc1

A release candidate: everything since 0.8.11, built together and run through the rig's full set (movement, Corvega
combat, terminals and loot, grenades, power armor, a cell churn with save reloads, a reconnect and the fuzz pass) with
no crash on either machine. Install it over 0.8.11 and say what holds and what doesn't.

Everyone needs the same build to play together: it uses protocol 47, so a friend on 0.8.11 gets "Version mismatch"
and has to update too. A saved session from 0.8.11 or older still loads.

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
- Better pings: F8 pings what your crosshair is on (an NPC, an item, a friend or a spot), puts a ring on everyone's
  compass in your color for 15 s, and names it with its distance and direction.
- Shared Minutemen settlement requests: when Preston or a settlement hands one of you a radiant quest (raiders,
  kidnappers, ghouls, a place to clear), everyone gets the same quest for the same settlement and place, and each
  player is paid once. `bSharedRadiants` turns it off. Other factions' radiants stay each player's own for now.
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
- Dogmeat's copy runs instead of squatting along at a run's pace.
- Settlement beds and containers left mid-move when workshop mode closes are drawn where they really stand, and
  crafted containers that come stocked (a bureau) reach your friend, emptied.

Known issues:

- LooksMenu overlays follow LooksMenu's published source and haven't been tried with LooksMenu installed yet;
  reports welcome.
- Only Vault 114's gear door and the plain terminal and card-reader gates are shared. Doors whose scripts also start
  scenes or move NPCs stay each player's own (`docs/script-doors-survey.md` lists them).
- Radiant quests other than the Minutemen's settlement requests (the Brotherhood's, the Railroad's, the Institute's,
  settlement attacks) are each player's own.
- A friend's Pip-Boy doesn't show on their stand-in.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
