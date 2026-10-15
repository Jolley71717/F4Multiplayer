## 0.8.6-rc4 (release candidate)

A candidate for 0.8.6. Install a crash logger (Buffout 4 or Addictol) before trying it, and report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log. The plugin log names players as "player N" and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.

Protocol 30: everyone needs this build (0.8.5 and the earlier 0.8.6 rcs can't join).

What changed since rc3:

- Shared quests move on for everyone, whoever plays them: a stage you reach is set in your friends' games too, starting the quest there if they hadn't, and theirs in yours. A friend who joins partway through a quest is caught up to the host's story stage by stage, in order, and never set back if they are further along. What a friend did alone before joining stays in their own game. Stages that take items (Vault 81's fusion cores) are left out.
- Vault 81: the host's talk at the intercom, and the vault door it opens, now reach friends.
- Your kill of an enemy a friend's game runs counts in their game too, with a gun, a grenade or a mine; an enemy you were fighting no longer changes hands as it falls.
- Your companion's bites and your NPCs' shots on an enemy a friend's game runs hurt it in their game too, and your companion's kill is yours.
- Raiders and creatures a random encounter or ambush makes appear in every friend's game, and fighting them counts for everyone.
- Hitting a friend's stand-in no longer turns it hostile or leaves you stuck in combat.
- A friend calling an elevator no longer forces yours open onto an empty shaft, and doors stop reopening themselves once a minute.
- Scrapping in workshop mode, or going in and out of it, can no longer freeze the game.
- Leaving a settlement where a friend's stand-in stood no longer crashes the host.
- A friend who teleports onto you is drawn a body's room away, not inside your camera.
- A friend's stand-in, and a mirrored Dogmeat, move smoothly instead of a step at a time.
- A friend's power armor stands where they parked it, at a station or wherever they got out.
- A friend's shots stay gunshots with a grenade equipped, a stray shot sound no longer plays forever, and a friend in power armor is no longer shown out of it after a menu.
- Dogmeat, saved dead by mistake, comes back for everyone; going down is never taken for a death again.
- A friend who shaped their body in LooksMenu has that body on their stand-in, when your game has LooksMenu too.
- A story NPC's scene that holds you no longer freezes when a friend's game takes the NPC over. A friend whose mods would give different form IDs is told so instead of joining.
- A host with the HD texture pack and mods loaded after it keeps their saved shared world after updating.
- A missing container change no longer has the game asking the server for the whole world every minute.
- Quitting the game no longer runs multiplayer work while the game tears its world down.
- A freeze's report names the thread that is really stuck.
- A friend who runs is shown running, not walking fast: their stand-in follows their own walk, run or sprint, at the pace their own game plays it, and strafes or backs up when they do.
- A friend's stand-in moves more evenly when their game runs slowly, and no longer slows down and lurches after a stall. A friend whose game hitched while joining is drawn close behind within a second or two, not trailing for half a minute.
- People and creatures another player's game runs strafe, back up and turn the way they do in that game, instead of flipping between directions. Dogmeat walks, trots and runs as he does in that game.
- A friend's stand-in aims its gun up and down where they look. People another player's game runs aim up and down at what they shoot at there, not level.
- An enemy another player's game runs is hurt in your game only by players' hits: its own side's grenades, and the fire they leave, no longer kill your copy of it while it fights on in their game.
- A friend's gun shows on their stand-in with their own barrel, sights and receiver, not a set rolled for your level. Armor and power armor pieces keep their mods too; a mod from a plugin you don't have is left off instead of the gun being handed over again and again.
- A friend in power armor is drawn on the frame they wear (the plain one, not the AtomCat frame).
- Downed: a player knocked down with friends around waits to be helped up again (rc3 could kill them outright). A friend within a few steps helps them up after 2 seconds; after 45 seconds alone they die once and the game loads their last save. Friends see "is back up", never a false "died", and pausing while you get up doesn't kill you. A friend's stand-in no longer sits on the ground while they fight.
- A turret or robot a friend builds in a settlement shows up in your game too, on your side.
- The host's Preston, Sturges and Jun in Sanctuary show up beside them for a friend whose copies still wait in the museum.
- A friend's Pip-Boy light follows them across cells without flickering.
- A friend's kill of an NPC that is switched off in your game leaves a corpse.
- Saving and loading are a little faster.

Known issues:

- Two players opening or closing the same door at the same instant can end up seeing it differently; using the door again puts them back in step.
