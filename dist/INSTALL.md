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

Each player keeps their own save, quests and inventory. Meet up by travelling to the same place.

## Dedicated server (optional)

`F4Multiplayer\F4MPServer.exe [--port 7779] [--max-players 4] [--password X]` runs a server
without playing. Everyone (including you) then joins with `sServerAddress`.

## Known limitations (early version)

- Other players use a stand-in character model.
- Combat, items, quests, containers and NPCs are not shared yet; each player's world is their own.

## Problems

- Logs: `Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`
- "load order is different": compare your mod lists; the order must match exactly.
- "Version mismatch": everyone needs the same version of this mod.
