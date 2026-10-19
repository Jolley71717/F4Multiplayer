# Seamless Co-op (Yui's FromSoftware mods): licensing and ideas

Research of 2026-10-19. The question: is the source of the Seamless Co-op mods (Elden Ring, Dark Souls III, Dark Souls
Remastered, Dark Souls II, Nightreign) and the Armored Core VI co-op mod public and usable, and which of their design
ideas are worth building into F4Multiplayer.

**Rule for this document and anything built from it.** Their mods are closed and all rights reserved (below). We take
ideas only, never code, assets, UI files or text, and we do not decompile or run their binaries. Every idea below is
written from the public feature descriptions in our own words, and anything we build from it is designed from our own
code and the game, as with Commonwealth Online. Option names are mentioned only to say which feature is meant.

## Verdict

None of Yui's co-op mods is open source. They ship as binaries only, with no license, and the author asks that nobody
modify, repackage or redistribute them without permission. We cannot use or port any of it. Their design ideas are
free to reimplement.

| Project | Source public? | License | GPL-3.0 compatible? | Sources |
|---|---|---|---|---|
| Seamless Co-op (Elden Ring) | No: binaries only | None. Author's FAQ on the Nexus page: no modifying, repackaging or redistributing without permission | No (all rights reserved) | [Nexus 510](https://www.nexusmods.com/eldenring/mods/510), [GitHub mirror](https://github.com/yuiamoroll/EldenRingSeamlessCoopRelease) (README, VERSION, BLOCKSYNC only) |
| Seamless Co-op (Dark Souls III) | No | Same FAQ line | No | [Nexus 1895](https://www.nexusmods.com/darksouls3/mods/1895), [mirror](https://github.com/yuiamoroll/DarkSouls3SeamlessCoopRelease) |
| Seamless Co-op (Dark Souls Remastered) | No | Same FAQ line | No | [Nexus 899](https://www.nexusmods.com/darksoulsremastered/mods/899), [mirror](https://github.com/yuiamoroll/DarkSouls1SeamlessCoopRelease) |
| Seamless Co-op (Dark Souls II SotFS) | No | Same FAQ line | No | [Nexus 1468](https://www.nexusmods.com/darksouls2/mods/1468), [mirror](https://github.com/yuiamoroll/DarkSouls2SeamlessCoopRelease) |
| Armored Core VI co-op mod | No | Same FAQ line | No | [Nexus 3](https://www.nexusmods.com/armoredcore6firesofrubicon/mods/3), [mirror](https://github.com/yuiamoroll/ArmoredCoreVICoopRelease) |
| Seamless Co-op (Nightreign) | No | "Do not modify, redistribute, or bundle" without permission | No | [Nexus 3](https://www.nexusmods.com/eldenringnightreign/mods/3), [mirror](https://github.com/yuiamoroll/NightreignSeamlessCoopRelease) |
| SeamlessCoopGlobalSyncList | A list of banned Steam IDs, no code | None | No (and nothing to use) | [repo](https://github.com/yuiamoroll/SeamlessCoopGlobalSyncList) |
| Yui's MIT repos (Sekiro Online, DSR PvP Watchdog) | No: an MIT LICENSE file sits next to README/VERSION/DATA files, with no source | MIT (covers nothing useful) | n/a | [Sekiro-Online](https://github.com/yuiamoroll/Sekiro-Online), [PvP Watchdog](https://github.com/yuiamoroll/DarkSouls-Remastered-PvP-Watchdog) |
| launch_modded_eldenring (Yui) | Yes, C++ | MIT | Yes | [repo](https://github.com/yuiamoroll/launch_modded_eldenring). A launcher that starts Elden Ring without EAC. F4SE's loader already does what we need |
| ersc-docs (community install guide/FAQ for ERSC) | Docs only | None | No: read for facts, never copy | [site](https://ersc-docs.github.io/), [repo](https://github.com/ersc-docs/ersc-docs.github.io) |

Notes on the sources:

- The author publishes as "Yui" on Nexus (member 49594931; the Nexus mod list by that uploader holds all six co-op
  mods). Links to `github.com/LukeYui/...` resolve to `github.com/yuiamoroll/...` (account "Yui", since 2019). The
  `LukeYui` GitHub account itself is a different, empty account created 2026-06-25.
- The release repos hold only a short README, a VERSION file and a BLOCKSYNC list (read by the mod at launch for the
  latest version and for bans). Their GitHub releases carry the binaries and the changelogs. We read the changelog text
  only and downloaded nothing.
- Nexus blocks automated page fetches (HTTP 403), so the mod descriptions were read through Nexus's public GraphQL API
  (`api.nexusmods.com/v2/graphql`, no key). That API doesn't return the Permissions tab. The author's own FAQ in every
  description answers the question anyway. To see the tab itself, open the page in a browser.
- We found no statement from the author about publishing the source.

### Related open-source projects

These have real source and a license. Most target FromSoftware games, so nothing is worth porting, but reading them is
allowed and some have ideas worth noting. Several say they reimplemented Yui's behaviour from observation without using
his code. We take only ideas from them too: if code is ever taken from one, it keeps its license notice and must come
from an original part of that project.

| Project | What | License | GPL-3.0 compatible? |
|---|---|---|---|
| [zofrea1/DS1-Prepare-to-Coop-PTDE](https://github.com/zofrea1/DS1-Prepare-to-Coop-PTDE) | Seamless-style co-op for Dark Souls PTDE, up to 4 players. Very detailed public rules (team wipes, spectating, travel follows the host) | GPL-3.0 | Yes |
| [himingal/des-seamless-coop](https://github.com/himingal/des-seamless-coop) | Demon's Souls co-op on RPCS3 with a built-in server | GPL-3.0 | Yes |
| [Restezzz/Seamless-DarkSouls2](https://github.com/Restezzz/Seamless-DarkSouls2) | DS2 co-op with an in-game menu, lobby password, crash log, rejoin after a crash | MIT (uses ds3os MIT, ImGui MIT, MinHook BSD-2) | Yes |
| [scheissgeist/Seamless](https://github.com/scheissgeist/Seamless) | The DS2 co-op mod the one above grew out of | MIT | Yes |
| [micthiesen/unseamless-coop](https://github.com/micthiesen/unseamless-coop) | Rust rewrite of ERSC. Unfinished: no in-world play yet | MIT, but built on fromsoftware-rs (below) | Its own code yes; the combination no |
| [vswarte/fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs) | Rust bindings to FromSoftware games | "MIT NON-AI" / "ASL2 NON-AI": MIT/Apache with added no-machine-learning clauses | **No**: the extra restrictions conflict with GPL-3.0 section 7, and the no-AI clause is a reason to keep away entirely |
| [Dasaav-dsv/libER](https://github.com/Dasaav-dsv/libER) | Elden Ring API library | Apache-2.0 with LLVM exception | Yes (one way, into GPL-3.0) |
| [garyttierney/me3](https://github.com/garyttierney/me3) | Mod loader framework (successor to ModEngine) | MIT or Apache-2.0 | Yes |
| [soulsmods/ModEngine2](https://github.com/soulsmods/ModEngine2) | Souls mod loader | MIT | Yes |
| [TLeonardUK/ds3os](https://github.com/TLeonardUK/ds3os) | Open reimplementation of the DS3/DS2 online servers | MIT | Yes |
| [caldwell/erscom](https://github.com/caldwell/erscom) | ERSC installer/manager | GPL-3.0 | Yes (nothing to use) |
| [veeenu/eldenring-practice-tool](https://github.com/veeenu/eldenring-practice-tool) | Speedrun practice tool | AGPL-3.0 | Combinable under GPL-3.0 section 13, but its parts stay AGPL. Not needed |
| [tiltedphoques/TiltedEvolution](https://github.com/tiltedphoques/TiltedEvolution) (Skyrim Together Reborn) | Bethesda-engine co-op, already our prior art | GPL-3.0-or-later; the launcher parts are LGPL-2.1 (FiveM-derived) | Yes |
| [skyrim-multiplayer/skymp](https://github.com/skyrim-multiplayer/skymp) | Skyrim MP server and client | GPLv3 / AGPLv3, per subproject | Yes, with care for the AGPL parts |

Repos with no license (installers, updaters, extensions such as boblord14/SeamlessCoopExtension, translations) are all
rights reserved and not usable.

## What their mods do (public descriptions)

Summarised in our own words from the Nexus descriptions, the GitHub changelogs and the community FAQ. Across the six
games:

- **Session.** One player hosts, others join; players are matched by a shared password through Steam matchmaking (the
  password effectively names the lobby, and a weak one lets strangers in). Nightreign and AC6 use plain Steam
  party invites instead. Same game version and same mod version are required. The mod tells the player when a newer
  version is out. Up to 6 players.
- **The host's settings rule the world.** A settings file holds enemy scaling, invasions, death penalties and summons;
  only the password has to match everyone, and the host's values decide.
- **Scaling.** Separate per-player percentages for ordinary enemies and for bosses: health, damage dealt, and posture
  (stagger resistance). AC6 adds health and armour per extra player. Nightreign expresses it relative to the vanilla
  formula.
- **Death.** A dead player respawns at their last rest point in the same world and the session carries on. During a
  boss fight (or an invasion, or always with an option) a dead player spectates a living teammate until the fight is
  won, everyone is dead, or someone rests. AC6: spectate until a checkpoint, a full wipe restarts the mission.
- **Death penalty (optional).** Each death in a session adds a stacking debuff (up to 5) that only resting removes. The
  stated reason: stop one player dying over and over at no cost while another keeps the fight going. The DS2 version
  raises enemy difficulty instead, and too many deaths without resting trigger an event.
- **Resting and travel.** Resting resets the world for everyone. Nobody may rest while a teammate fights a boss, and a
  rest pulls everyone out of boss arenas. Fast travel is a vote: one player proposes a destination on the map, the
  others accept, everyone goes. Some dungeons' entry is a vote too.
- **Progress.** World state follows the host. Progress made in a friend's world also advances your own. Shops are
  instanced per player. Ordinary pickups are per player; one-off rewards (boss drops) go to everyone nearby. The
  community FAQ warns at length that mixed progress can set flags wrongly, and keeps a page of manual fixes.
- **Items between players.** Spells and upgrade materials can be dropped for party members.
- **Rules mid-session.** The host switches between co-op, friendly fire and team PvP with an item, and players choose
  teams. A "neutral" mode lets players heal and buff each other.
- **Invasions (optional).** Strangers can join as hostile players; while one is in the world nobody can rest or travel.
  Cooldowns, matchmaking brackets, and a public ban list downloaded at launch.
- **HUD.** Over each player's head: name, ping, level or death count (a setting). A HUD icon while hosting and while PvP
  is on. Friends on the compass. Map waypoints are shared. Messages for a party wipe.
- **Separation from vanilla.** A launcher starts the game without anti-cheat. Saves use a different file extension, so
  modded and vanilla saves never mix. The official servers are never contacted.
- **Robustness.** Crash reporting (Crashpad), a warning for irregular frame rates, guards against duplicate items when
  a disconnect happens during loading, and an enemy whose AI got confused by several players made to target only the
  host.
- **Other.** Proximity voice chat (DS2), a friend's light source lights your game too (DSR), audio heard from the
  player rather than the camera (DS2), translations and a language override, skipping the intro logos, a borderless
  window option.

From the open DS1 PTDE project (GPL-3.0), ideas Yui's pages don't spell out: when the host fast travels, the guests
follow to the same place; a death keeps the enemies the team killed dead; a full team wipe reloads every game together;
the host can't use a return-home item without taking the guests along; health bars show the scaled health; a protocol change says which versions can't
play together instead of "no sessions found".

## Mapping to F4Multiplayer

Effort: **S** about a day, **M** a few days, **L** a week or more (with live tests on the VM rig).

| Idea | F4Multiplayer today | Worth it? | Effort |
|---|---|---|---|
| Password-matched sessions | Steam invites/Join Game, optional `sPassword` | Have it (invites are friendlier than a shared lobby password) | - |
| Host kick/ban | Not yet | Open work (per the brief) | - |
| Host's settings rule the world | `bFriendlyFire`, `sStory` sent in `Welcome` | Have it; new host-side options (scaling, penalties) should go the same way | - |
| Version check | "Version mismatch", load-order check with the plugin diff | Have it | - |
| **Update check** ("0.8.12 is out") | None | **Yes**: players on different versions is a common failure. One GitHub releases API call at launch, opt-out ini key, nothing personal sent | S |
| **Enemy scaling per extra player** | None: 4 players against vanilla enemies | **Yes, the biggest balance gap**. Host's `fEnemyHealthScaling` / `fEnemyDamageScaling` per extra player, separate values for legendaries and bosses (uniques, high-level), applied the same in every game through the hit path we already own ("fair hits") | M |
| Posture scaling | FO4 has stagger but no posture bar | Maybe later: stagger/limb resistance is subtle | M |
| **Down/death penalty until rest** | Revive is free; tuning is open work | **Yes**, as part of the revive tuning: each down in a session costs e.g. 10% max health (stacking to 5), cleared by sleeping. It is exactly the "infinite revives" problem Seamless names | S-M |
| Respawn without a reload, keep the world | Dying is a normal death and reload; catch-up restores the shared world | No: FO4's death-reload is the game's own, and catch-up covers it | - |
| Spectate a friend when dead | Roadmap #20: no (camera control fragile) | Keep as no; downed/revive is our version | - |
| Team wipe message | Kill feed says who died | Small polish: "Everyone is down" | S |
| **Fast travel together** (vote or follow the host) | F7 teleports to a friend | **Yes**: one player picks a map marker, the others get "Go with Sam to Diamond City? (hold F7)", and each fast travels themselves (their own game's fast travel rules: not in combat, not overencumbered) | M |
| **No resting while a teammate fights; wait/sleep together** | Roadmap #15 partly: the host's wait moves everyone, others snap back | **Yes**: refuse Wait/Sleep while any player in the session is in combat, and let any player's wait move the session clock | S-M |
| World resets on rest | FO4 cells respawn on timers | No | - |
| Progress follows the host / your progress comes home | `sStory = shared/own`, side quests shared once started, session saved | Have it | - |
| Separate co-op saves | Same saves; INSTALL says back up | **Yes, as an automatic backup**: save to a named slot ("before co-op") when you join or host, at most once per session. Their docs' loudest warning is "back up before joining" | S |
| Repair menu for wrong flags | None | Not now: our guards (own story, quest stages only forward) aim to prevent it. Revisit if reports come in | - |
| Instanced shops and pickups | Vendors, caps, keys, weapons and armor per player | Have it | - |
| Give items to a friend | Not yet | Open work | - |
| **Heal a friend** (neutral-mode heals) | Revive only | **Yes**: use a Stimpak on a downed or hurt friend's stand-in to heal them (it costs your Stimpak), as Fallout 76 does | M |
| Mid-session rule changes | F1 menu writes settings | Have it | - |
| Team PvP | Friendly fire on/off | No: co-op is the point | - |
| Invasions, global ban list | None | No: friends only, no random matchmaking | - |
| **Overhead display options** | Name and health over the head | **Yes**: an option to add ping, level or deaths this session | S |
| **Session scoreboard** | F6 list: place, health, level | **Yes**, for fun: kills, downs, revives and deaths this session in F6 | S |
| **HUD session indicator** | None (F1 shows it) | **Yes**: a small mark while hosting / connected, with the player count and friendly fire on | S |
| Shared map waypoint | F8 ping; pings with markers are open work | Fold into the pings work: a custom map marker a friend sets shows on your map and compass | M |
| Friends on the compass | `bCompassMarkers` | Have it | - |
| Proximity voice | Steam voice, fades with distance | Have it | - |
| Shared light | Pip-Boy light on the stand-in | Have it | - |
| One player per boss AI target | NPC ownership, nearest-player hand-off | Have it | - |
| Crash reporting | Buffout 4 / Addictol; our own hang reports | Enough for now | - |
| **Low frame rate warning** | Connection warnings only | **Yes**: a game at low fps sends its player and the NPCs it runs late and jerky, which looks like lag; tell the others "Sam's game runs at 18 fps" | S |
| No anti-cheat to bypass | F4SE, no official servers | n/a | - |
| Companions in session (summons toggle) | Per-player companions | Maybe: a host option to send companions home in a session for balance; low demand | S |
| No pausing in menus | Not yet | Open work | - |
| VATS in real time | Not yet | Open work | - |

## Top 10 to add, ranked

1. **Enemy scaling per extra player** (health and damage, separate for legendaries/bosses; host's setting). M. The
   biggest balance gap with 4 players.
2. **Down penalty until you sleep** (stacking max-health loss per down, cleared by a bed). S-M. Goes into the revive
   tuning work.
3. **Fast travel together** (one proposes a marker, the others accept and travel). M.
4. **No waiting or sleeping while a teammate fights; anyone's wait moves the session clock.** S-M. Finishes roadmap #15.
5. **Update check at launch.** S.
6. **Automatic backup save when joining or hosting.** S.
7. **Heal a friend with a Stimpak.** M.
8. **Overhead options and a session scoreboard** (ping/level/deaths over heads; kills, downs, revives in F6). S.
9. **HUD session indicator** (hosting/connected, players, friendly fire). S.
10. **Low frame rate warning for friends.** S.

Next in line: a shared map waypoint (with the pings work), the "everyone is down" message, a host option to keep
companions home.
