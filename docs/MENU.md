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
| Hosting | Invite friends (Steam lobby open), Stop hosting, Settings, Close |

- *Host a session* starts the session server in your game, as `bHost = true` does at startup, with
  the transport and settings from the ini, and connects your game to it. It also writes
  `bHost = true` to the ini, so the next time the game starts it hosts again; *Stop hosting* writes
  `bHost = false`. If the port can't be bound, the main page says so.
- *Invite friends* opens the Steam overlay's invite dialog for your lobby (what the overlay's
  "Invite to Game" does).
- *Stop hosting* disconnects everyone and closes the lobby.
- *Leave* disconnects from the host and leaves their Steam lobby; the game stops reconnecting.

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

Friends hosting with "private" game details in their Steam profile don't show up: Steam shares
rich presence and lobby membership only for friends whose game details are visible. Their invite
still works. A lobby search (`RequestLobbyList`) is not an alternative: the lobby is friends-only,
and Steam leaves friends-only lobbies out of searches.

**Settings.** Five switches, each shown with its current value and written to the ini when
toggled (only that line changes; comments and the rest of the file stay):

| Setting | Ini key | Takes effect |
|---|---|---|
| Friendly fire | `bFriendlyFire` | the next session you host (it's sent to joiners in the welcome) |
| Shared story | `sStory` (`shared`/`own`) | the next session you host |
| Voice chat | `bVoiceChat` | at once |
| Compass markers | `bCompassMarkers` | at once |
| Fast loading | `bFastLoading` | the next loading screen |

## Testing it

The dev channel drives it without the keyboard: `mpmenu open` shows the main page, `mpmenu press
<0-3>` presses a button of the page showing, `mpmenu` reports the page's buttons, counters and body
text (line breaks shown as `/`; the channel's reply is a single line, as are all of these).
`hotkeys` says why a key may be doing nothing: focus, `menuMode`, the console, which chord keys
(Alt, Ctrl, Win) are down, each hotkey's code and whether it's down, and how many presses were
blocked. `steam friends` lists what Steam says about each friend (state, game, lobby, "connect"
presence); `steam refresh` asks for their presence, as the join page does.
Things to check in a real two-game session: the menu opens in gameplay only (not in the Pip-Boy or
over the console); hosting from the menu lets a friend's "Join Game" work; the friend's game lists
the host on the join page and joining from there connects; Stop hosting disconnects the friend
cleanly; Leave stops the reconnect loop; each settings toggle changes the ini line and nothing else.
