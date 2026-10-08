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

One player hosts. Only the host changes a setting:

```ini
bHost = true
```

Everyone else keeps the defaults. Players are named after their Steam names (set `sPlayerName` to
use another name).

Friends join through Steam: no ports, IP addresses or VPNs. Traffic goes through Steam's relays,
so nobody sees anyone's IP address. Only Steam friends of the host (or players the host invites)
can join. Optionally set the same `sPassword` for everyone.

## Playing

If your Steam status hides what you play ("private" game details), friends can't see you in a game: invite them from the overlay instead of waiting for Join Game, or set your game details to visible for friends while hosting.

1. Everyone starts the game **with F4SE** and loads a save.
2. The host invites friends: open the Steam overlay (Shift+Tab), go to Friends, right-click a
   friend and pick **Invite to Game**. The friend accepts the invite in their Steam overlay.

   Or the friend joins on their own: in the Steam overlay or friends list, right-click the host
   and pick **Join Game**.
3. You'll see "Multiplayer: connected" and "<name> joined" messages.
4. Players appear to each other when they are in the same interior, or near each other outside.

Accept invites and use Join Game **while the game is already running with F4SE**. If Steam starts
Fallout 4 for you from an invite, it starts without F4SE and the mod doesn't load: close it, start
the game with F4SE, then join again.

### Without Steam (LAN or VPN)

Everyone sets `sTransport = enet`. Friends set `sServerAddress` to the host's IP address. The
host's game listens on UDP port 7779; allow it when Windows Firewall asks. Over the internet the
host also forwards UDP 7779 on their router.

Each player keeps their own save, character and inventory.

### Hotkeys

| Key | What it does |
|---|---|
| F6 | Player list: where everyone is, how far and which way, their health and level |
| F7 | Teleport to a friend: tap to choose who, then hold for a second to go there. While you're down: hold to give up |
| F8 | "Over here!": tells everyone where you are, with distance and direction |

Change them in `F4Multiplayer.ini` (`iKeyPlayerList`, `iKeyTeleport`, `iKeyPing`). They don't
work while a menu or the console is open.

### Voice chat

Hold the mouse's back side button to talk (`iKeyVoice`; set `bVoiceOpenMic = true` to talk without
a key). Friends hear you at full volume within 8 meters, fading out by 60 meters (`fVoiceRange`;
0 = everyone hears everyone). It uses the microphone chosen in Steam > Settings > Voice.

### Going down

With friends in the session, a hit that would kill you knocks you down instead. A friend who stands
next to you for 2 seconds helps you up (with 30% health). When a friend goes down, holding F7 takes
you to them. If nobody comes within 45 seconds, or you hold F7, you die as usual. Turn this off with
`bRevive = false`.

## What is shared

- **Players:** position, walking/running/sneaking/jumping, your armor and weapon (drawn or
  holstered), shooting, reloading, aiming, swinging a melee weapon or punching, your Pip-Boy light, and your name when someone looks at you (with your health when you're
  hurt). Messages tell everyone when a player kills something, dies, completes a quest, joins or
  leaves.
- **Time and weather:** everyone follows the host's time of day and weather (`bSyncTime`).
- **Map:** a location one player discovers appears on everyone's map, ready for fast travel
  (`bShareMap`).
- **XP:** you get half the XP for your friends' kills (`fXpShare`).
- **NPCs and combat:** each NPC is run by one player's game and the others copy it, so everyone sees
  the same raider in the same place. Its weapon and armor are copied from that game too, so a
  raider carries the same gun on every screen. Enemies can attack any player, and the damage reaches that
  player. Killing or hurting an NPC counts for everyone. Players can't hurt each other unless the
  host sets `bFriendlyFire = true`.
- **World:** deaths, loot taken from or put into containers and bodies, items picked up off the
  ground, doors opened/closed and locks picked.
- **Quests:** when a side quest moves forward in one game, it moves forward in the others (never
  backwards), once they have started that quest themselves (it's in their Pip-Boy). Turn this off
  with `bSyncQuests = false` if a quest misbehaves.
- **The story:** everyone plays the main story and the faction quests in their own game, and gets
  a notice when a friend finishes a story quest. Play it side by side: kills count for everyone, and
  each of you talks to the characters yourself. The host can set `sStory = shared` to move
  everyone's story along together instead (it can skip scenes you haven't seen, and a friend's
  faction choices become yours).
- **A new character:** play the opening up to leaving Vault 111 on your own; you can be connected
  meanwhile. Nothing from the session touches it, and what your friends did is applied once you're out.
- Players who join later catch up on everything that already happened this session. So does anyone
  who loads a save or dies and reloads.

For the closest shared world, start from similar saves (for example, everyone at the same point in
the story). Things that were already different between your saves before the session stay different.

With `sStory = shared`, make big story choices together. Quest progress only moves forward, so if
one player sides with one faction and another player with a rival one, both games get both results.

What happens in a session is saved into your game when you save. Use a separate save for co-op if you
want to keep your solo playthrough untouched.

## Dedicated server (optional)

`F4Multiplayer\F4MPServer.exe [--port 7779] [--max-players 4] [--password X] [--friendly-fire]` runs a server
without playing (over UDP only). Everyone, including you, then joins with `sTransport = enet` and
`sServerAddress`.

## Known limitations (early version)

- Other players appear as a settler of their sex and body shape wearing their gear: their face,
  hair and weapon mods aren't copied yet.
- Other players' and NPCs' shots show the firing animation and play the gun's sound, but without a
  muzzle flash yet.
- Misc and radiant quests (e.g. Minutemen settlement requests) are not shared.
- Workshop building is shared one way per object: what a friend builds appears in your game as a plain copy you can't scrap, wire or assign settlers to (they can, on their side). Power and snapping to a friend's pieces don't cross games yet. Only things the workshop can build are copied.

## Problems

- "Connection to the host is unstable": nothing has come back from the host for 4 seconds. It
  usually recovers ("connection restored"); after 20 seconds you're disconnected and the mod keeps
  trying to reconnect.

- Logs: `Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`
- "load order is different": compare your mod lists; the order must match exactly.
- "Version mismatch": everyone needs the same version of this mod.
- No "Invite to Game" or "Join Game" in Steam: the host must have started the game with F4SE and
  `bHost = true`; check the host's log for a "Steam: hosting lobby" line.
- "you're hosting": only the host sets `bHost = true`.
