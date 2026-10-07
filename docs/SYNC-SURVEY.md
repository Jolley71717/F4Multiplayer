# What else should be synced: a survey (2026-10-07)

What a player sees of a friend today: position and movement, sneaking, jumping, weapon drawn,
armor and weapons, gunshots (animation and sound), name and health, their face and body shape,
going down and being helped up, where they are on the map, what they said (subtitles) and their
voice. Of the world: NPCs' positions, gear, deaths and health; loot, pickups, doors, locks; side
quests; map discoveries; time and weather.

Candidates, by how often a player would notice the gap in a normal session:

| # | Gap | Noticed when | Effort | Verdict |
|---|---|---|---|---|
| 1 | Melee swings and punches | Any melee fight; the friend just stands there | small once the graph's event is known | next |
| 2 | Reloading, aiming down sights, VATS pose | Every firefight; the stand-in never reloads or aims | small (animation events "reloadStart", "sightedStateEnter") | next |
| 3 | Power armor | A friend in power armor is a settler in a vault suit | medium (the frame is a furniture the stand-in must "use") | later |
| 4 | Death animations and ragdolls of shared NPCs | Every kill: the copy drops flat instead of flying | medium | later |
| 5 | Companions following the right player | Dogmeat follows nobody in your game | medium (ownership exists; the follow package doesn't) | later |
| 6 | Emotes (wave, point, sit) | Only if we add keys for them | small | later |
| 7 | Facial expression, head tracking (looking at who talks) | Conversations feel flat | small-medium | later |
| 8 | Workshop building | Any settlement work: a friend's walls are missing | large | not yet |
| 9 | Misc/radiant quests | Settlement requests differ per game | large, by design | no |
| 10 | Pip-Boy light | Night play: the friend's light is invisible | small | next |
| 11 | Crouch-sneak attack crits, VATS slow-mo for others | Rare; VATS can't be shared sensibly | n/a | no |
| 12 | Grenades and mines | A friend's thrown grenade doesn't exist in your game | medium (shared projectile spawn) | later |

| 13 | Fast travel with a friend | Each player can fast travel alone today; the other sees them vanish and reappear. To verify both ways, and consider "travel together" (the friend gets a prompt) | small to verify; medium for travel-together | verify next |
| 14 | Workshop building shared | The big one: a friend's walls, turrets and crops don't exist in your game | large (placed/moved/scrapped references, snapping, power, settlers assigned) | planned: start with placing and scrapping |

Next up, in order: 1, 2, 10, 13. Then 14 (building), 3 and 12 for fights, 5 for companions.
