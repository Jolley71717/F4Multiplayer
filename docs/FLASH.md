# Flash UI

The plan for the multiplayer menu. How it's built and what was learned about the game's menus is in [UI.md](UI.md).

## One menu, one movie

Every page below is a page of `F4MPMenu` and `MPMenu.swf`: the same menu, input and lifetime in every session state. A
second movie is only for something non-modal with a life of its own, such as a HUD party strip or a hosting badge.

The pages stay in `src/game/Menu.cpp`, which already builds them from `Session::GetStatus()` and hands each to one
function, `Show(body, buttons)`. That function gets a second output, the Flash panel:

- The movie takes a whole page: `SetPage(serial, body, rows, hints, keepRow, heading)`. Its list scrolls, so `More` goes away
  from lists.
- A page's one line of status goes across the top, with room kept for it, so rows start at the same place on every page. Rows
  take the full width, or the left 2/3 with what the chosen row does in the right 1/3 on pages that say so.
- Long text: a faint frame whose right side is one line, with a handle on its left, sized to the part shown. Lists: the game's
  double chevrons where there is more above or below, and Up and Down stop at the ends.
- **About**, the main page's last row: the mod's name and version, and `dist/about.txt` (English).
- **Controls**, on the main page: the plugin's keys, changed by pressing the row and then the key. They take effect at once and
  go to the ini. Each key is in a column on its row, `W)`, as the game's own Controls page has it. A key another bind has, or
  that the game or menus need, is refused and the status line says why.
- The panel stays open between pages. The main thread queues a page and the panel applies it in `AdvanceMovie` (Flash
  objects are written only in a menu's own update). A press carries the page's serial, so a stale one is dropped.
- Esc, Tab and the gamepad's B go back a page. The main page redraws when its status changes.
- `Menu::Press` and `Describe` work on the panel too, so the rig scripts it and reads the log, without screenshots.
- While the panel is open, the Multiplayer row under it dims as the game's own do with a submenu open.
- The menu key (F1) opens the panel in the middle of the screen, and closes it from any page. The Multiplayer entry opens it
  beside the main menu's list. The message box stays as the fallback if the movie doesn't load.

## Main menu → Multiplayer

```
MAIN MENU
  Continue · New · Load · [Multiplayer] · Settings · … · Quit
                               │
                               ▼
              ┌────────── MULTIPLAYER ──────────┐
              │  Host a game                    │
              │  Join a game                    │
              │  Settings                       │
              │  Back                           │
              └──┬──────────────┬───────────┬───┘
                 │              │           └─► your own settings (voice, keys,
                 │              │               compass, fast loading…)
                 ▼              ▼
   HOST                          JOIN
   ① Session options             ① Friends hosting now (names)
      password · max players        [Direct connect…]  ← text field: address, password
      shared/own story ·         ② Your game: Continue · Load · New
      friendly fire · revive        (the game's own; New plays Vault 111 alone,
      rules                         joins on leaving it)
   ② Your game: Continue ·                     │
      Load · New (the game's own)              ▼
        │                        loads save → in world → connects
        ▼                        ③ Mods differ? → compare page → [Join anyway] / [Leave]
   loads save → in world →       welcome → HUD: "Joined Friend A's game"
   server + Steam lobby up
   HUD: "Hosting — invite
   from the pause menu"
```

- **The game's own load screen, no picker of ours.** Host sets hosting to start after the load, and `Tracker::Check`
  already waits for the world and the end of the opening. Still to confirm: that the game's load panel can be opened
  this way. A character and save picker can come later if this feels wrong.
- **Friends' rows are names at first.** `ScanFriends` gives a name and a lobby, and the mod list only comes after
  connecting. Later the lobby can publish a mod hash, player count and lock state, for rows like
  `Friend A  2/4  mods ✓  🔒`, without changing the game protocol.
- **A Steam invite** accepted at the main menu goes to JOIN step ②.

## Pause menu → Multiplayer

The page depends on the session state, with no reload in any of them:

```
PAUSE MENU  … · [Multiplayer] · Settings · …
                     │
   ┌─────────────┬───┴──────────┬──────────────────┬──────────────┐
   ▼ DORMANT     ▼ HOST WAITING ▼ JOINING          ▼ HOSTING      ▼ JOINED
   Host this     Waiting for    Connecting to      Status: lobby, Party
     game          the world,     Friend A,          players, lock Compare mods
     → options     or out of      retrying         Party (name,   Leave
     → [Start]     Vault 111    Refused: why         health,      Settings
   Join a friend Cancel         Cancel               place, ping)
     → list →    Settings       Settings           Invite friends
     "Join as                                        (Steam overlay)
     Nora?"                                        Host controls
     → compare                                       kick · ban ·
     mods                                            lock · password
   Settings                                        Session options
                                                   Stop hosting
                                                   Settings
```

- **Join from the pause menu** joins as the character being played, after asking, as session changes get saved into
  that character. It has the same mod compare step as the main menu's route.
- **Settings** are personal and can be changed in any state. **Session options** are host rules, passed to the server
  when hosting starts: chosen before hosting, and read-only while hosting unless a rule is confirmed to apply live.
- **A Steam invite** accepted in the world asks first, as for a menu join. While hosting, it also says accepting stops
  the session.

## Session state behind both

```
                 Host (either menu) / bHost opt-in
   ┌─────────┐ ──────────► ┌──────────────┐ in the world, ┌─────────┐
   │ DORMANT │             │ HOST WAITING │ out of the    │ HOSTING │
   │(default)│ ◄────────── └──────────────┘ opening ────► └─────────┘
   └─────────┘   Cancel                                        │ ▲ loads of the same
     ▲ ▲ ◄───────────────────────────────────────────────────┘ └ character: stays
     │ │    Stop hosting · other character · quit to main menu · quit game
     │ │
     │ │  Leave · kicked/banned · other character · quit to main menu
     │ └─────────────────────────────────────────────┐
     │ Join (menu, invite, direct)                   │
     ▼                                               │
   ┌─────────┐  connected + welcome   ┌────────┐ ────┘
   │ JOINING │ ─────────────────────► │ JOINED │
   └─────────┘ ◄───────────────────── └────────┘
     retries every 5 s while   dropped: reconnect (intent kept)
     in the world; Cancel
     leaves. Refused: the page
     says why

   Always, even DORMANT: tidy co-op leftovers on load (stand-ins, frame copies, ghosts)
   Launch: always DORMANT, unless bHost / sServerAddress opt-ins are set
```

Where this differs from the code today:

- **Host lasts this launch only.** `Menu::Host` writes `bHost = true`, so the next launch hosts again. `bHost` becomes
  only the ini's opt-in.
- **Another character goes dormant.** With `bHost` set, hosting restarts under the new character today.
- **Quitting to the main menu clears a menu Host.** Otherwise the next load would silently start hosting.

## Order of work

Text entry (direct connect, typed passwords) is a spike of its own and holds nothing up: generated passwords and the
ini's address cover the gap.

1. **Pause menu and guard (done).** The entry and panel work in both menus, and `StartHosting` refuses at the main menu.
2. **Flash pages (done).** The panel shows the F1 pages through the game's renderer, with Controls and About.
   Voice and the talk icon, then the developer HUD, follow in pull requests of their own.
3. **Hosting page.** The status summary and a party list with ping and health.
4. **Main menu Host and Join.** The flows above, with the session state changes. Until then Host at the main menu gives a
   popup saying a game must be loaded first.

## Still to check

Seen only under CrossOver on a Mac: the main and pause menus, and two games in a session over a direct connection (joining, and seeing each other):

- **Windows, by someone with it:** the panel through the game's renderer (it draws, the cursor and popups are over it, and its
  ground is see-through), Controls, and the `build.ps1`, `package.ps1` and `deploy.ps1` changes (each pull request gets this on the project's rig).
- **The rest of a session:** hosting and joined pages in the pause menu, the main page's live status, host controls, hosting
  from the menu's own row, and joining over Steam with an invite.
- **A gamepad, and MCM installed.**
- **Mouse buttons in Controls, and closing the panel with a menu key bound to one:** their codes in the game's input are a guess; the log says what it heard. CrossOver can't try them.
- **Translations:** the menu's new texts are machine-written.

## To file on the tracker

- The status said "Connecting to..." at the main menu, where nothing is tried (fixed here: "You'll join ... once you've loaded a
  game"). Stop hosting writes `bHost = false`, so the ini's address is what the next launch joins (step 4 stops the write).
- Controls checks only our own keys: a key the game also uses (E, WASD) does both. The game's control map could warn of it.
- Texts that name a key ("see F1") don't follow a rebind.
- Settings mixes host rules (friendly fire, shared story) with personal ones (voice, compass): split as above.
- A gamepad can scroll a long text only on a page with no rows.
