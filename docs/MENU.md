# The in-game menu

Press **F1** while playing (no menu or console open) for the multiplayer menu. Nexus users asked
for a way to host, join and change settings without editing the ini or using the Steam overlay;
this is it. The key is `iKeyMenu` in `F4Multiplayer.ini` (a Windows virtual-key code in hex;
`70` = F1, `0` turns it off). F1 is not bound by Fallout 4 itself; if another mod uses it, change
the key.

## What it's made of

The game ships no menu of ours and we add no Flash file, so the menu is the engine's own message
box: the "Are you sure you want to quit?" dialog (`MessageBoxMenu`), created from C++ through
`MessageMenuManager::Create` with our header, body text and up to **four buttons**. Each page of
the menu is one box; pressing a button closes it, and on the next frame the button's action runs
and usually opens the next page. Lists longer than a page (friends, settings) get a `More (1/3)`
button that turns the page, and every page but the main one has `Back`.

The box pauses the game as the quit confirmation does; the session keeps running underneath (the
plugin's frame task runs anyway), so the status on the main page is what was true when the page
opened. Close and reopen the menu to refresh it. If the box goes away without a button (the game
closes dialogs for some events), nothing runs; the next F1 starts at the main page again.

Four buttons is the engine's limit for this dialog, which is why the pages are small and paged
rather than one screen with everything on it. Scrolling, text entry (a name, an address) and a
free layout aren't possible with it; the address to join still comes from the ini.

## Pages

**Main.** The first line is the session: not in one, connecting to (or refused by) a host, in a
host's game with the ping, or hosting (through Steam with the lobby open, or on a UDP port). Then
who is with you, one line each as the F6 player list shows them. Buttons depend on the state:

| State | Buttons |
|---|---|
| Not in a session | Host a session, Join a friend, Settings, Close |
| Joining or joined | Leave, Join someone else, Settings, Close |
| Hosting | Invite friends (Steam lobby open), Host controls, Stop hosting, Settings, Close (with More) |

- *Host a session* starts the session server in your game, as `bHost = true` does at startup, with
  the transport and settings from the ini, and connects your game to it. It also writes
  `bHost = true` to the ini, so the next time the game starts it hosts again; *Stop hosting* writes
  `bHost = false`. If the port can't be bound, the main page says so.
- *Invite friends* opens the Steam overlay's invite dialog for your lobby (what the overlay's
  "Invite to Game" does).
- *Stop hosting* disconnects everyone and closes the lobby.
- *Leave* disconnects from the host and leaves their Steam lobby; the game stops reconnecting.
- *Compare mods (N)* comes first, once the host's mod list has come and yours differs from it (N:
  the plugins only one of you has; plugins the host lets differ count only where they move another
  plugin's form IDs, so a join they alone differ in has none). With it the
  main page has more buttons than a box holds, so they turn over with *More*.

**Host controls** (hosting only). The first lines say whether the session is open or locked, the
password (as your ini has it) or that there is none, and how many players are banned. Buttons:

- *Kick or ban (N)* lists everyone else in the session; pressing a name gives *Kick* and *Ban*.
  A kicked player is dropped with "Multiplayer: the host removed you from the session" and their
  game stops reconnecting; they can join again. A banned one hears "the host banned you", and every
  later join is refused with "The host has banned you from this session". The HUD says what was
  done ("Sam was kicked"), or why not (the player left meanwhile; no Steam account known, so they
  were kicked instead; the ban list could not be saved).
- *Lock the session* / *Unlock the session*: while locked, nobody new is let in ("The session is
  locked: the host isn't letting anyone new in"; their game tries again each minute). Those in it
  stay, a game that restarts while still in the session gets back in, and so does a player who was
  in it and dropped out (their game crashed, their connection broke or timed out) since the server
  last started: the server knows them by the identity it gave their old player number. A player
  the host kicked or banned is not taken back while it is locked. The host's own game is never
  locked out. The lock isn't saved: hosting again starts open.
- *Password*: the box can't take typing, so *New password* makes up eight letters and digits (none
  that look alike), writes it to `sPassword` in the ini and tells the server; the page shows it to
  pass on. *Remove the password* clears both. Players already in stay. The log says only that the
  password was changed or removed, never what it is.
- *Banned (N)*: the bans, newest first, by name while the server that banned them still runs, and
  by a short alias (`~3fa2c1`) and date after; pressing one lifts it, and *Unban everyone* lifts all.

Each command goes to your own server (`kHostCommand`, protocol 42) and the page after it waits for
its answer (`kHostInfo`), up to a second, so it shows what is true. The server takes these only
from the host's own game (the local connection whose identity is the host's) and refuses and logs
anyone else's ("player 2 is not the host: their host command was refused", at most once every ten
seconds per player). Bans are kept by Steam account and by the game's own identity, both as salted
hashes, in `F4Multiplayer.bans` beside the session files (`F4Multiplayer.dev.bans` with the dev
channel); the file never has a name or a Steam ID, and the log names a banned player only by their
alias. The dedicated server takes the same commands at its console (see INSTALL.md).

**Compare mods.** Your load order against the host's, by file name: *missing in your game* (the
host's plugins you lack, in the host's order), *only in your game* (yours the host lacks), and *in
another place in your load order* (plugins you both have that are out of the host's order among
their kind, or light in one game and full in the other; one moved plugin is named alone, not every
plugin it passed). Since protocol 36 the moved ones are nothing to change: form IDs are translated by
plugin name (DESIGN.md, Form IDs across load orders), and only what comes from a plugin one of you
lacks is left out. The host's `sIgnoredPlugins` come with its list and are left out as the server
leaves them out: named only where one moves another plugin's form IDs (missing, extra or out of
place before it). Ten names a page, *More* to turn. In the session, *Ask the host again* asks the
host for its list; the answer goes to the HUD whatever it says. The server answers one ask in five
seconds of the last list it sent, so the game doesn't ask sooner, and says "you just asked the host;
try again in a few seconds" instead; an ask unanswered in ten seconds is over. The host's server
sends its list (`kLoadOrder`, protocol 35) before welcoming a joiner whose list differs. A joiner hears
"your mods differ from the host's (N): see F1" once, and again only when it changes; only moved
plugins are no news. A hello without a plugin list (a bot) is still refused for a load order other
than the session's, and a refused game's HUD says the same line, at every refusal. The log has
every name (`Session: mods: ...` lines); the dev channel's `mods` says what is known and `mods ask`
asks again, and `mods forms [formHex]` shows the session's plugin table and an ID in both numberings.

**Join a friend.** Steam friends who are hosting Fallout 4 co-op right now, by name: those whose
rich presence advertises a lobby (the same "connect" string the overlay's "Join Game" uses) or who
Steam reports in a lobby of this game. Steam hands out a friend's rich presence only once it has
been asked for, so pressing *Join a friend* (or *Look again*) first asks for every online friend's
(`RequestFriendRichPresence`) and shows the page when the answers have arrived
(`FriendRichPresenceUpdate_t`), or after 1.5 seconds; the screen is empty for that moment.
Pressing a name joins their lobby, and the join carries on as an accepted invite would (you see
"joining your friend's game", then "connected"). If `sServerAddress` is set, `Join <address>`
connects to it directly (the UDP/LAN case). With nobody to list, the page says how many friends
Steam sees, how many are online and how many are in Fallout 4, so you can tell where the list
stops.

A host appears on this page only if the joiner's Steam shows them as playing Fallout 4: Steam
shares rich presence and lobby membership for a friend only inside the game it sees them in. The
check is the host's own Steam profile: it must say "In-Game: Fallout 4" while they host. Two things
on the host's side decide that: how the game is started (through Steam, with F4SE set as the
launcher; a game started some other way may run with Steam unaware of it), and the profile's
"Game details" privacy, which must be Friends Only or Public. On the test rig the host's game was
started directly, and the joiner's Steam saw the host as online but not in a game
(`steam friends`: `inGame=false app=0 connect=''`), while the host's own presence was intact
(`steam`: `ownConnect='+f4mp_lobby <id>'`). The invite and "Join Game" paths still work in that
case, and the page says so. A lobby search
(`RequestLobbyList`) is not an alternative: the lobby is friends-only, and Steam leaves
friends-only lobbies out of searches.

**Settings.** Six switches, each shown with its current value and written to the ini when
toggled (only that line changes; comments and the rest of the file stay):

| Setting | Ini key | Takes effect |
|---|---|---|
| Friendly fire | `bFriendlyFire` | the next session you host (it's sent to joiners in the welcome) |
| Shared story | `sStory` (`shared`/`own`) | the next session you host |
| Voice chat | `bVoiceChat` | at once |
| Spoken lines | `bSpokenLines` | the next line |
| Compass markers | `bCompassMarkers` | at once |
| Fast loading | `bFastLoading` | the next loading screen |

Spoken lines plays a friend's lines as a plain sound at your copy of the speaker; it never starts a
conversation, a Say or a scene, so it can't leave you held in dialogue the way the old
`bExperimentalVoiceLines` (no longer read) could.

## Testing it

The dev channel drives it without the keyboard: `mpmenu open` shows the main page, `mpmenu press
<0-3>` presses a button of the page showing, `mpmenu` reports the page's buttons, counters and body
text (line breaks shown as `/`; the channel's reply is a single line, as are all of these).
`hotkeys` says why a key may be doing nothing: focus, `menuMode`, the console, which chord keys
(Alt, Ctrl, Win) are down, each hotkey's code and whether it's down, and how many presses were
blocked. `steam friends` lists what Steam says about each friend (state, game, lobby, "connect"
presence); `steam refresh` asks for their presence, as the join page does. `hostctl` says what the
host's game last heard from its server: locked, password set, the number of bans and their aliases.
Things to check in a real two-game session: the menu opens in gameplay only (not in the Pip-Boy or
over the console); hosting from the menu lets a friend's "Join Game" work; the friend's game lists
the host on the join page and joining from there connects; Stop hosting disconnects the friend
cleanly; Leave stops the reconnect loop; each settings toggle changes the ini line and nothing else.
