# Nexus Mods page

What to paste into the Nexus upload form. The file to upload is `dist\out\F4Multiplayer-<version>.zip`.

## Create draft

- **Mod name:** F4Multiplayer - Co-op over Steam
- **Category:** Gameplay
- **Short description** (350 characters max):

Co-op for up to 4 players through Steam: invite friends from the overlay, no port forwarding. Shared NPCs, combat, loot, doors, side quests, map and time of day. Voice chat, revive a downed friend, teleport to friends, player list. Everyone keeps their own save and plays their own main story. F4SE plugin, early alpha.

## Description (BBCode)

```
[size=5][b]Play Fallout 4 with up to 3 friends[/b][/size]

One player hosts, the others join through Steam: Shift+Tab, right-click a friend, [b]Invite to Game[/b] (or [b]Join Game[/b] on the host). No port forwarding, no VPN, no IP addresses. Everyone keeps their own save, character and inventory.

[size=4][b]What you share[/b][/size]
[list]
[*][b]NPCs and combat:[/b] one game runs each NPC and the others copy it, so everyone fights the same raider in the same place. Enemies attack any player; kills and damage count for everyone.
[*][b]The world:[/b] loot taken or stored, items picked up, doors and locks, deaths.
[*][b]Side quests:[/b] progress in one game moves the others forward once they have started the quest too. The main story and factions are played by each player in their own game (the host can switch to a shared story).
[*][b]Map:[/b] a location one player finds shows up on everyone's map, ready for fast travel.
[*][b]Time and weather[/b] follow the host.
[*][b]XP:[/b] half the XP for your friends' kills.
[/list]

[size=4][b]Playing together[/b][/size]
[list]
[*]Friends appear as settlers wearing their gear, with their name and health over their head. You see them aim, fire and swing.
[*][b]Voice chat[/b] through Steam, with volume that fades with distance. Hold the mouse back button to talk.
[*][b]Going down:[/b] a lethal hit knocks you down instead; a friend next to you for 2 seconds gets you up. 45 seconds alone and you die as usual.
[*][b]F6[/b] player list (where everyone is, health, level), [b]F7[/b] teleport to a friend, [b]F8[/b] "over here!" ping.
[*]Reconnecting, reloading a save or dying keeps the shared world consistent: your game catches up on what happened.
[*]A new character plays the Vault 111 opening on their own, untouched by the session, and joins the shared world when they leave the vault.
[/list]

[size=4][b]Requirements[/b][/size]
[list]
[*]Fallout 4 [b]1.11.240[/b] (Steam, current version)
[*][url=https://www.nexusmods.com/fallout4/mods/42147]F4SE[/url] 0.7.9
[*][url=https://www.nexusmods.com/fallout4/mods/47327]Address Library for F4SE Plugins[/url]
[*]Everyone needs the same version of this mod and [b]the same mods in the same load order[/b].
[/list]

[size=4][b]Install[/b][/size]
Install with Vortex or Mod Organizer 2, or extract the zip into Fallout 4\Data. The host sets [b]bHost = true[/b] in Data\F4SE\Plugins\F4Multiplayer.ini; friends change nothing. Always start the game with F4SE. The full guide is in the zip: F4Multiplayer\INSTALL.md.

[size=4][b]Early alpha[/b][/size]
Back up your saves or use a separate one for co-op. Known gaps: friends show as settlers (no faces or hair yet), no muzzle flash on their shots, misc/radiant quests and workshop building are not shared. If something breaks, the log is Documents\My Games\Fallout4\F4SE\F4Multiplayer.log; please attach it to a bug report.

[size=4][b]Open source[/b][/size]
GPL-3.0. The source will be published on GitHub.
```

## Permissions

- Other user's assets: none used.
- Upload permission: do not upload to other sites (point to this page).
- Modification permission: yes, under GPL-3.0 (source must be shared).
- Conversion permission: no.
- Asset use permission: yes, with credit, under GPL-3.0.
- Credits: F4SE and CommonLibF4 (libxse), ENet, Address Library.

## Changelog for 0.6.1

- Fixed a crash when a friend joined: their stand-in could be replaced (once their face arrived) while the game was still loading it. A stand-in now waits a moment for the face, and is never deleted mid-load.
- A friend's or NPC's automatic fire sounds like one burst, every shot heard (the stand-in used to play only the shots its animation had time for).
- A mirrored NPC takes off the armor its owner's copy doesn't wear, so both games see the same outfit; an NPC that dies at once still gets its gear reported.
- Friends' melee swings and punches play on their stand-in (a friend's swing used to show nothing).
- Friends' stand-ins are now the right sex and body shape (their full face is sent too, and will show once head generation works).
- NPC gear is shared: the game that runs an NPC reports its weapon and armor, and the others dress their copy the same (each game used to roll its own).
- Replayed automatic-weapon fire no longer loops forever on the hearing side.
- The free high-resolution texture pack no longer counts as a load-order difference.
- Protocol 21: everyone updates together.

## Changelog for 0.6.0

- First Nexus release.
- Joining through Steam (invites, Join Game, relays).
- Shared NPCs, combat, loot, doors, side quests, map, time and weather.
- Weapon fire animation, player list, teleport, ping, kill feed, XP share.
- Downed/revive, Steam voice chat.
- Each player plays their own story; the Vault 111 opening is left alone.
- Reconnecting keeps your loot and your player slot, including right after a crash.
- About 50 ms less lag.
