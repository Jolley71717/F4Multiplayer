# Roadmap

The aim is a co-op game that's fun to play together, not just synced. Ideas are ordered by how much
they add for the effort. Status: done, next, later, or no (with the reason).

## 20 ideas

| # | Idea | What it gives players | Status |
|---|---|---|---|
| 1 | Visible weapon fire | You see and hear friends and their NPCs shoot (animation and the gun's own sound) | done (protocol 13; sound in 20) |
| 2 | Hotkeys | Multiplayer actions without the console; keys set in the ini | done (protocol 14) |
| 3 | Player list | Each friend's location name, distance and direction, and health | done (protocol 14) |
| 4 | Teleport to a friend | Regroup after splitting up or dying; tap to choose, hold to go | done (protocol 14) |
| 5 | Kill feed and named messages | "Sam killed Raider", "Sam died", "Sam left" | done (protocol 14) |
| 6 | Friends' health on their name | See at a glance who needs help | done (protocol 14) |
| 7 | Shared time of day and weather | Night falls and storms roll in for everyone at once | done (protocol 14) |
| 8 | Pings ("over here") | Point a friend to something without voice chat | done (protocol 14) |
| 9 | Shared kill XP | Friends get part of the XP for your kills, so nobody falls behind | done (protocol 14) |
| 10 | Connection warnings | "Connection to the host is unstable" instead of silent rubber-banding | done (protocol 14) |
| 11 | Friendly fire setting | Host chooses whether players can hurt each other (off by default) | done (protocol 15) |
| 12 | Emotes | Wave, point, cheer on hotkeys, played on your character for others | later: idles exist (IdlePointing 0013A445, IdleCheeringStanding 0010BA3A, IdleClapping 00141F3C), but stand-ins ignore `playidle` while the puppet driver animates them |
| 13 | Session saved on the host's disk | The shared world state survives the host quitting | later |
| 14 | Shared map discoveries | A location one player finds shows up on everyone's map | done (protocol 18) |
| 15 | Waiting and sleeping together | Waiting moves time for everyone | partly: the host's waiting moves everyone's clock; others snap back |
| 16 | Quest completion messages | "Sam completed Out of Time" | done (protocol 15) |
| 17 | Gunshot sound and muzzle flash | Fights sound and look complete | partly: sound done (protocol 20); muzzle flash later |
| 18 | Player markers on the compass | Find friends without opening the list | done (protocol 20): a colored shape per player |
| 19 | Remote players' faces | Friends look like their own character | later (hard: face morphs) |
| 20 | Spectating a friend after death | Watch while you reload | no (camera control is fragile) |

## 5 wild ideas

| Idea | Feasible? | Notes |
|---|---|---|
| Proximity voice chat | Done (protocol 17) | Steam's voice API (`ISteamUser::StartVoiceRecording`/`GetVoice`/`DecompressVoice`) is in the game's own steam_api64.dll; send it over the session, play it with volume by distance. |
| Downed and revive | Done (protocol 16) | The player's character is essential while friends are in the session; a lethal hit knocks them down for 45 s, and a friend who stays next to them for 2 s helps them up; otherwise the normal death. |
| Horde mode at a settlement | Partly | Spawned enemies are runtime references with different IDs in each game, so the host's game would have to run them and send "spawn this" with its own ID mapping. A bigger change to NpcSync. |
| Real faces for friends | Hard | Needs the player's face morphs, head parts and tints copied to the stand-in; FO4's face data is large and poorly documented. |
| Shared settlements (workshop) | Not now | Every placed object is a runtime reference; it would need a full object replication system. |

## Beyond the list (protocol 19-20)

| Feature | What it gives players |
|---|---|
| Overheard conversations | When a friend or an NPC near them talks within 14 m of you, their line shows on your screen ("Preston: ...") |
| Talking to the same NPC | Both players can talk to one NPC; nobody takes it over mid-conversation |
| Personal items | Bobbleheads, magazines, keys, notes and quest items are collected by each player separately |
| Fair hits | No damage from enemies that don't exist in your game, nor while you're paused; your difficulty and VATS scale the damage |
| NPCs stay put | Companions aren't dragged off by a friend's game; NPCs fall back to their own AI when their owner is paused, far or gone |
| NPCs go to the nearest player | An enemy run by a far-away friend is handed to the player standing next to it |
| Safe teleport | No teleporting out of conversations, scenes, furniture or Survival |
| Safe quest sharing | A friend's quest progress reaches you once you've started that quest, never mid-conversation |
| Reload keeps your loot | Dying and reloading doesn't take back loot you'd picked up after your save |

## Next

- Remote players' faces (#19).
- Emotes (#12), muzzle flash (#17).
- Session saved on the host's disk (#13).
- Donation links (Ko-fi, Patreon, Nexus Donation Points): last, before the release.
