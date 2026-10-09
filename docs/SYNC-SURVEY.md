# What else should be synced: a survey (2026-10-07)

What a player sees of a friend today: position and movement, sneaking, jumping, weapon drawn,
armor and weapons, gunshots (animation and sound), name and health, their face and body shape,
going down and being helped up, where they are on the map, what they said (subtitles) and their
voice. Of the world: NPCs' positions, gear, deaths and health; loot, pickups, doors, locks; side
quests; map discoveries; time and weather.

Candidates, by how often a player would notice the gap in a normal session:

| # | Gap | Noticed when | Effort | Verdict |
|---|---|---|---|---|
| 1 | Melee swings and punches | Any melee fight; the friend just stands there | done (protocol 21): the stand-in performs the engine's attack action | done |
| 2 | Reloading, aiming down sights | Every firefight; the stand-in never reloads or aims | done (protocol 22): "reloadStateEnter" and "sightedStateEnter/Exit" are reported; the stand-in takes "reloadStart" and the engine's ActionSighted/ActionSightedRelease | done |
| 3 | Power armor | A friend in power armor is a settler in a vault suit | medium (the frame is a furniture the stand-in must "use") | later |
| 4 | Death animations and ragdolls of shared NPCs | Every kill: the copy drops flat instead of flying | medium | later |
| 5 | Companions following the right player | Dogmeat follows nobody in your game | medium (ownership exists; the follow package doesn't) | later |
| 6 | Emotes (wave, point, sit) | Only if we add keys for them | small | later |
| 7 | Facial expression, head tracking (looking at who talks) | Conversations feel flat | small-medium | later |
| 8 | Workshop building | Any settlement work: a friend's walls are missing | placed, moved and scrapped objects are copied (protocol 22) | partly |
| 9 | Misc/radiant quests | Settlement requests differ per game | large, by design | no |
| 10 | Pip-Boy light | Night play: the friend's light is invisible | done (protocol 22): a light follows the stand-in while their light is on | done |
| 11 | Crouch-sneak attack crits, VATS slow-mo for others | Rare; VATS can't be shared sensibly | n/a | no |
| 12 | Grenades and mines | A friend's thrown grenade doesn't exist in your game | medium (shared projectile spawn) | later |

| 13 | Fast travel with a friend | Each player can fast travel alone today; the other sees them vanish and reappear. To verify both ways, and consider "travel together" (the friend gets a prompt) | small to verify; medium for travel-together | verify next |
| 14 | Workshop building shared | The big one: a friend's walls, turrets and crops don't exist in your game | first step done (protocol 22): what a player places, moves or scraps is copied into the others' games (plain copies: not scrappable or powerable by them); the rest is planned in docs/WORKSHOP-POWER.md | partly |

Next up, in order: 1, 2, 10, 13. Then 14 (building), 3 and 12 for fights, 5 for companions.

## Second pass: locked areas, keys and story gates (2026-10-09)

The question: how does a locked mission area work when two people play together: doors that lock and
unlock, keys and keycards, terminals, elevators, scenes that gate a door. What the mod does today, by
case, and where the holes are.

**What exists.** Doors and locks a player *activates* are watched for a while and their open/lock state
is sent to the others (`WorldSync`: "pendingDoors"), so a door you unlock with a key or a bobby pin
opens in your friend's game too. Keys, notes, bobbleheads and magazines are "everyone takes their own":
the pickup is not removed from the friend's world, they get their own copy. Side quests share stages
(`QuestSync`): when you move a side quest on, the friend's game is set to the same stage once they have
the quest, and the quest's own script does the unlocking there (a door that opens at stage 30 opens in
both games). The main story and the faction quests are per player unless the host sets
`sStory = shared` (INSTALL.md): with "own", each of you walks the main quest yourself.

| Case | Example | Today | Gap |
|---|---|---|---|
| Door with a key you both can pick up | Kellogg's house (key from Valentine's case is quest-given; the Mayor's key in Diamond City) | key-holder opens it, the open/unlock is sent, friend walks through | none |
| Key given only in dialogue, to one player | Vault 81 (Overseer), Covenant, many settlement houses | only the speaker has the key; the door still opens for the friend when the speaker opens it | the friend cannot re-lock or open it alone; fine for co-op |
| Door unlocked by a terminal in the same room | Fort Hagen, Mass Fusion, National Guard armory, Vault 75 | the terminal's lock state is sent (it was activated) but the *linked door* was never activated, so its unlock is not sent | **gap**: scan nearby doors for lock changes and send them; or send a terminal's linked refs' state when the terminal is used |
| Door that opens at a side-quest stage | Museum of Freedom (Preston), Vault 81, Cabot House, Covenant | stage is shared, both scripts unlock it | none once the friend has the quest; before that they are refused by the door until their own stage arrives |
| Door gated by a main-story stage, `sStory = own` | Fort Hagen's elevator, Institute relay, Prydwen docking, Mass Fusion's reactor | each player must reach that stage themselves; a friend ahead of you walks through, you are refused | by design; `shared` lifts it. Worth saying plainly in INSTALL.md with these examples |
| Scene that must play before the door opens | Diamond City gate (Piper), Goodneighbor's Hancock scene, Vault 81's intercom | the scene runs in the game of whoever starts it; the stage it sets is shared; the friend's door opens when their stage arrives | a friend standing next to you sees the scene's NPCs act (they are owned by the talker's game) but hears only subtitles; voiced lines now play on their copy (0.6.3) |
| Elevators (load doors) and interior transitions | any vault, Fort Hagen | load doors are per player; NPC ownership follows whoever is in the cell | none |
| Scripted transport | Vertibird, the Prydwen, the Institute relay, Vault 111's elevator | the rider is teleported by their own game; the friend sees them vanish and reappear elsewhere | a "come with me" for scripted transport is not possible; fast travel to a friend is (party teleport) |
| Timed sequences and traps | reactor countdowns, laser tripwires, turrets | run in each game; damage only from things that exist in yours | none for a 2-player co-op; turrets are world refs so both see them |
| Hand-in quest items | "bring X to Y" quests | the hander-in finishes the stage, the friend's quest advances with the stage; their copy of the item stays in their bag | cosmetic |
| Lockpick and hacking minigames | all | each player's own minigame; the result (lock state) is sent | none |
| Terminal-controlled things that are not doors | turret shutdown, spotlights, Protectron release | the terminal state is sent; what it drives is not | same gap as the linked door |

**Other things a session will meet, not yet covered** (beyond the table above and the first pass):

- Grenades, mines, Molotovs (first pass #12): a friend's throw does not exist in your game. Shared projectile spawn, medium.
- Death animations and ragdolls of shared NPCs (first pass #4).
- Power armor on the stand-in (first pass #3): in progress 2026-10-09.
- Companions: each follows its owner; the friend's game runs it as a mirror. To verify on video.
- Vertibird signal grenades and the Vertibird itself: the rider is on their own; the friend sees the stand-in in the sky.
- Settlement attacks and radiant settlement quests: per game (first pass #9).
- Trading with vendors, caps, crafting: per player, by design.
- Sleeping and waiting: the world clock is shared (clock owner), so one player sleeping moves time for both.
- Faction hostility after story choices: with `own` stories, one player can be an Institute ally while the other is at war with it; their NPCs are shared, so the friend's enemies attack you in their cell. Say so in INSTALL.md.
- Addiction, radiation, survival mode needs: per player.
- Pip-Boy radio, holotapes, perks, SPECIAL: per player.

**Order to take them on:** terminal-linked doors and nearby lock changes (small, and it is the one that
strands a friend behind a door), INSTALL.md wording for story gates and factions (small), grenades
(medium), ragdolls (medium).
