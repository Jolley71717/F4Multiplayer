# F4Multiplayer: install and play

Up to 4 players. Everyone needs:

- Fallout 4 **1.11.240** on Steam
- [F4SE 0.7.9](https://www.nexusmods.com/fallout4/mods/42147?tab=files): copy `f4se_loader.exe` and `f4se_1_11_240.dll` next to `Fallout4.exe`
- [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327?tab=files) (1.11.240 file)
- This mod: install the zip with Vortex or Mod Organizer 2 (or extract it into `Fallout 4\Data`)
- **The same mods in the same load order** as everyone else (the server refuses mismatches)

**Everyone must use the same version of this mod.** Back up your saves (or start a fresh one)
before playing; this is an early alpha.

Always start the game with F4SE (`f4se_loader.exe`, or your mod manager's F4SE launcher).

## Settings

Edit `Data\F4SE\Plugins\F4Multiplayer.ini` (in Vortex: the mod's staging folder, or use
"Open in File Manager").

### The host (one player)

```ini
sPlayerName = YourName
bHost = true
```

The host's game runs the server. Friends connect to the host's IP address on UDP port 7779:

- **Same house / LAN:** give friends your local IP (run `ipconfig`, look for IPv4 Address).
- **Over the internet, easiest:** everyone installs [Tailscale](https://tailscale.com) (free) and
  signs into the same network; give friends your Tailscale IP (100.x.x.x).
- **Over the internet, port forwarding:** forward UDP 7779 on your router to your PC and give
  friends your public IP.

When Windows Firewall asks about Fallout 4, allow it on private networks (and public, if you use
port forwarding).

### Everyone else

```ini
sPlayerName = YourName
bHost = false
sServerAddress = 100.101.102.103
```

Optionally set the same `sPassword` for everyone.

## Playing

1. The host loads a save. Then friends load theirs.
2. Once you're in the world you connect automatically; you'll see "Multiplayer: connected" and
   "<name> joined" messages.
3. Players appear to each other when they are in the same interior, or near each other outside.

Each player keeps their own save, character and inventory. Meet up by travelling to the same place.

## What is shared

- **Players:** position, walking/running/sneaking/jumping, your armor and weapon (drawn or
  holstered), and your name when someone looks at you.
- **NPCs and combat:** each NPC is run by one player's game and the others copy it, so everyone sees
  the same raider in the same place. Enemies can attack any player, and the damage reaches that
  player. Killing or hurting an NPC counts for everyone.
- **World:** deaths, loot taken from or put into containers and bodies, items picked up off the
  ground, doors opened/closed and locks picked.
- **Quests:** when a story, faction or side quest moves forward in one game, it moves forward in
  the others (never backwards). Turn this off with `bSyncQuests = false` if a quest misbehaves.
- Players who join later catch up on everything that already happened this session. So does anyone
  who loads a save or dies and reloads.

For the closest shared world, start from similar saves (for example, everyone at the same point in
the story). Things that were already different between your saves before the session stay different.

Make big story choices together. Quest progress only moves forward, so if one player sides with
one faction and another player with a rival one, both games get both results.

What happens in a session is saved into your game when you save. Use a separate save for co-op if you
want to keep your solo playthrough untouched.

## Dedicated server (optional)

`F4Multiplayer\F4MPServer.exe [--port 7779] [--max-players 4] [--password X]` runs a server
without playing. Everyone (including you) then joins with `sServerAddress`.

## Known limitations (early version)

- Other players appear as a settler of the same sex wearing their gear: their face, hair and
  weapon mods aren't copied.
- NPCs run by another player aim at you but don't show their firing animation in your game (their
  damage still applies).
- Misc and radiant quests (e.g. Minutemen settlement requests) are not shared.
- Workshop building is not shared.

## Problems

- Logs: `Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`
- "load order is different": compare your mod lists; the order must match exactly.
- "Version mismatch": everyone needs the same version of this mod.
