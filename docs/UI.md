# The multiplayer menu and the plugin's own Flash UI

How the Multiplayer entry in the game's main menu and the menu it opens are built, the toolchain for the plugin's own
Flash movies, and what was learned about the game's menus on the way. For the F1 message-box menu see
[MENU.md](MENU.md). This is the mod's menu system: the panel shows the F1 menu's pages, plus Controls and About, in the main and pause
menus (seen so far under CrossOver only). What is still to check, and the plan, are in [FLASH.md](FLASH.md).

## The pieces

| What | Where |
|---|---|
| The MULTIPLAYER entry in the main and pause menus, after LOAD | [src/game/UiMainMenuEntry.cpp](../src/game/UiMainMenuEntry.cpp), placement rule in [UiMainMenuPlace.h](../src/game/UiMainMenuPlace.h) (unit tested) |
| Our menu (F4MPMenu in the game's UI), opened by the entry | [src/game/UiPanel.cpp](../src/game/UiPanel.cpp) |
| Its movie, `Interface/F4Multiplayer/MPMenu.swf` | [ui/MPMenu/MPMenu.as](../ui/MPMenu/MPMenu.as), compiled into `dist/movies/` and never committed |
| Pieces for other movies: look, brackets, command bar, entry list | [ui/shared/f4mp/ui/](../ui/shared/f4mp/ui/) (`Style`, `Brackets`, `CommandBar`, `EntryList`, `ScrollBar`) |
| The compiler, pinned | [tools/flash/fetch.sh](../tools/flash/fetch.sh) |
| Building the movies | [tools/flash/build.sh](../tools/flash/build.sh), [build.ps1](../tools/flash/build.ps1) |

Labels are text keys like every player-facing string (`mainmenu.entry`, `mpmenu.*`): every language file needs every
key, or `text:` tests fail.

Key names on the Controls page: `UiKeyNames::Name` (game-free) gives a letter, digit, F-key or symbol as it is, and a worded key
(Space, Left Shift, Mouse 4, the arrows) as a `key.*` text id; `UiKeyNames::Display` looks the id up in the current language
(the number pad's `key.num` takes its digit as `{n}`). A new worded key needs its `key.*` text in every language file. The command
bar's key word ("Esc)") comes from the plugin too: `SetLook`'s last argument.

## Toolchain

The game's menus are Flash movies played by Scaleform GFx 4. Ours are ActionScript 3 compiled with Apache Flex's
`mxmlc`; no Flex framework, no Adobe tool.

- `tools/flash/fetch.sh` downloads Apache Flex SDK 4.16.1 (prebuilt `mxmlc` and `compc`, Apache-2.0) and Adobe's
  `playerglobal.swc` 32.0 (the `flash.*` API), checks both against pinned SHA-256s and unpacks them into
  `~/.flex/4.16.1`. Nothing of either is committed; `playerglobal.swc` is Adobe's. The Flex tarball's checksum was
  pinned after Apache's MD5 matched and its release signature checked out against Apache Flex's KEYS. Adobe's CDN
  serves only playerglobal 32.0 now (older versions answer 403). Needs Java.
- **Movies are never committed**: a pull request holds only `ui/`, and git ignores `dist/movies/`, where they compile to. Every movie a player
  gets is compiled from source on GitHub's machines (the Package workflow, docs/RELEASING.md), and `vet.yml` checks the
  toolchain and compiles the same sources. Building the plugin and its tests needs no Java. `tools\deploy.ps1` and
  `tools/crossovermac.sh deploy` compile a movie that is missing or older than its sources when Java is there, and
  take the Package workflow's compiled `movies` instead with `-Movies <folder>` (`--movies <folder>`). A movie they
  can't have is a warning, not a failed deploy: without one the plugin falls back to the message-box menu. `dist/movies/<Name>.sources`
  records what a local movie was built from.
- `tools/flash/build.sh [Name]` builds each `ui/<Name>/<Name>.as` (with `ui/shared` on the source path) into
  `dist/movies/<Name>.swf`, uncompressed, at **SWF 15**, the game's own version (Flash Player 11.2), against playerglobal 32.0 linked
  as external, so no Adobe code is in the movie. Use only what Flash Player 11.2 had: Scaleform knows no more.
  Stage 1280x720, as the game's menus.
- `mxmlc` takes several source paths as `-compiler.source-path=a -compiler.source-path+=b`; a second bare path is an
  error ("default arguments may not be interspersed").
- On Windows the same is `tools\flash\fetch.ps1` (into `%USERPROFILE%\.flex`) and `tools\flash\build.ps1 [Name]`;
  the pins are in both fetch scripts, change them together.
- Deploying: `tools/crossovermac.sh deploy` (and `restart`) and `tools\deploy.ps1` copy `dist/movies/*.swf` into
  `Data/Interface/F4Multiplayer/`, a folder of our own so no other mod's movie clashes. `tools/package.ps1` puts them
  in the zip there, and refuses a zip without `MPMenu.swf`.

**Changing the look without restarting the game.** The panel loads its movie each time it opens, so a change only in `ui/` can
be tried in a running game: run `tools/flash/build.sh MPMenu`, copy `dist/movies/MPMenu.swf` over the one in
`Data/Interface/F4Multiplayer/`, then close the panel and open it again (`mpmenu close`, `mpmenu open` on the dev channel). Seen
under CrossOver, in the world with no other menu open: with the main or pause menu up, the panel kept showing the movie it had
already loaded, and only a restart changed it. So check it took (`gfx F4MPMenu root 1` gives the panel's place). A change in `src/`, or one that changes what the plugin passes to the movie (`SetPage`'s arguments, say), needs a
rebuild and a restart.

On a Mac (docs/misc/MAC.md): `xmake build -y F4Multiplayer` with `XMAKE_CONFIGDIR=$PWD/.xmake/win`, then
`tools/crossovermac.sh restart ''` to deploy and stop at the main menu. xmake sometimes stops at once with "please run
`xmake build [target]` to build the following targets first: -> F4MPServer"; run it again (or check the DLL's time).
The full unit test run takes about five minutes under Wine; run the tests a change can touch by name
(`tools/crossovermac.sh test "text:"`, `"log privacy"`, `"main menu entry"`).

## Reading the game's menus

The game's menus are in `Data/Fallout4 - Interface.ba2`; a BA2 tool (Bethesda Archive Extractor, or a mod manager's)
gets `Interface/MainMenu.swf` out. JPEXS FFDec (open source) shows a movie's ActionScript, classes and placed
objects. What is below was read without it, from the movie's AS3 string pool (class, member, event and instance names
and text keys, in the order the compiler met them; deduplicated, so a table of pairs reads with gaps, see the
controller glyphs). Keep what you extract out of the repo: the game's files are Bethesda's.

At run time:

- The dev channel's `gfx <menu> <path> [depth]` logs a menu's display objects, `gfxset` sets a property.

## The game's main menu (MainMenu.swf, 1.11.240)

- One movie for both menus: there is no PauseMenu.swf; the pause menu is MainMenu.swf with `PauseMode`.
- The menu object is `root.Menu_mc` (class `MainMenu`). Its list: `root.Menu_mc.MainPanel_mc.List_mc`, a
  `BSScrollingList`. Its entries are `entryList`, objects `{ text: "$LOAD", index: LOAD_INDEX }`; `InvalidateData()`
  redraws them. A press is the list's `"BSScrollingList::itemPress"` event; the menu's `onMainListItemPress` acts on
  `selectedEntry.index`. The menu rebuilds the list now and then, so the entry is put back from the menu's update.
- Our entry has index 900, which the menu's handler ignores.
- A state machine (`currentState`, `StartState`, `EndState`, `UpdateStateFocus`, `ReturnToMainState`; states
  `MAIN_STATE`, `SETTINGS_CATEGORY_STATE`, `SAVE_LOAD_STATE`, …) shows each panel and moves the focus. Input comes
  two ways: `ProcessUserEvent` from the game, and a focused list's own Flash key listener (`onKeyDown`;
  `disableInput`, `disableSelection` turn a list off).
- The back hint is `ButtonHintBar_mc.SetButtonHintData(Vector.<BSButtonHintData>)`.
- It loads `Main_LoginHelper.swf` into its own class domain (`LoaderContext`, `ApplicationDomain`): a movie can be
  loaded beside the menu's classes.
- Geometry on the stage: the main panel's box is `MainPanelBackground_mc` (getBounds against root); the second column
  (Settings) starts about 10 units of 652 (the main panel's height) to its right, 40 lower, bottom level with it; its
  rows are 58.5 apart, bars 52 high, text 52 in from the side (MPMenu.as keeps these as constants in those units).
- Rendering: `FlatScreenModel`, flags 0x80C8AC9 (custom rendering, no cursor flag), depth 14, 40 shader targets.
  Content (`MainPanel_mc`, `List_mc`, texts) is HUD color type 3 (`kPlayerSetColor`); panel backgrounds
  (`MainPanelBackground_mc` and the like) are color type 0 with a shaded background of type 2
  (`kGameplayHUDColor`). In the movie every shape is plain white: the color and the panels' dark translucent ground
  are the renderer's.

## Our menu (MPMenu)

- A `GameMenuBase` registered as `F4MPMenu` at game-data-ready, shown with `UIMessageQueue::AddMessage(kShow)`, above
  the main and pause menus (`kMessage` depth). It loads its movie with `BSScaleformManager::LoadMovieEx` and `"root"`
  as the menu object.
- **Calls from the movie.** The code object the game makes for a menu (`RegisterCodeObject` / `MapCodeObjectFunctions`)
  came out empty for ours: `BGSCodeObj` existed with no functions, and every call failed silently. So the plugin sets
  its own `BGSCodeObj`: an object of functions made with `Movie::CreateFunction` on a handler of its own, the function's
  ID in `userData`. Not the menu itself as handler: a function the movie holds keeps its handler alive, and the menu
  holds the movie. AS3's `in` and `hasOwnProperty` don't see these functions; just call them (in a try).
- **Input.** Keys and the gamepad come as user events to the movie's `ProcessUserEvent(name, justPressed)`
  (`Up`, `Down`, `Accept`, `Click`, Tab as `Cancel`, and gameplay names). Esc arrives as `Start`, so `OnButtonEvent`
  reads the way back out (Esc, Tab, the gamepad's B) by code and closes the menu itself. The codes are
  **Windows virtual keys** (Esc 0x1B, Tab 0x09, Enter 0x0D, Down 0x28), not DirectInput scan codes; the gamepad's are
  XInput's (B 0x2000).
- **Keyboard or gamepad.** `IMenu::RefreshPlatform` calls the movie's `SetPlatform(platform, psn)`; 0 is the keyboard
  and mouse. Call it again after setting the code object if the movie answers through it.
- **The look.** The HUD color from the game's `iHUDColorR/G/B:Interface` settings; the game's `$MAIN_Font`; the hard
  shadow (black, no blur, equal down and right); panel brackets as 1.5-pixel lines (`Brackets.THICK`: one looks too thin through the renderer). Controller glyphs are letters in the game's `$Controller_buttons` font:
  `Shared.AS3.BSButtonHint`'s `NameToTextMap` gives Xenon_A "A", Xenon_B "B", Xenon_X "C", Xenon_Y "D",
  Xenon_Select "E", Xenon_LS "F", Xenon_L1 "G", Xenon_L3 "H", Xenon_L2 "I", … Xenon_Start "O" (PlayStation has a map of
  its own, not read yet).
- **Drawing through the game's renderer.** Set `kCustomRendering` (the renderer is `GameMenuBase`'s `FlatScreenModel`). The game's
  menus, cursor and message boxes all draw that way, and a menu that doesn't draws in a later pass, over all of them. What follows is what
  was seen under CrossOver, not how the renderer is known to work: ours drew only where its `shaderFXObjects` (`BSGFxShaderFXTarget`s)
  gave a quad, though the main menu draws far more than its few cached quads, so there is more to it:
  - With no target, or one with no filter (it makes no quad), a sliver of the movie shows. A target on the movie's root has an empty
    quad. So the targets go on objects with a size: `Content_mc` and `CommandBar_mc`. (F4SE's `CustomMenu` shows where to look: its
    `filterHolder`, a target on the root.)
  - A target needs a filter to make a quad. The no-color one (`kNoColorMultiplier`) draws the movie's colors black; a white one
    (`CreateAndSetFiltersToColor(255, 255, 255, 1)`) leaves them as they are.
  - The panel's see-through ground is the renderer's, not the movie's: anything the movie draws there comes out opaque. So
    `Background_mc` has a nearly invisible shape, for its size, and a target as the main menu's backgrounds: no-color, with
    `EnableShadedBackground(kGameplayHUDColor)`.
  - The content keeps the HUD color the movie is told. The player's exact color would be `kPlayerSetColor` with white in the movie (not
    in use).
- **Opened in the world (the menu key).** In the middle of the stage, at the pause menu's scale (its panel box is 327 high on the
  1280x720 stage). Seen under CrossOver:
  - It never pauses the game and isn't modal, and the HUD stays up under it, so its toasts can be read. If the player wants to pause,
    they use the game's own menu.
  - `kTopmostRenderedMenu` would hide the HUD, so it isn't set. The HUD then draws over the panel, so the three things in its middle
    (crosshair, rollover, quick container) go while it's up. That's by `alpha`: the HUD sets their `visible` itself, including when the
    player gets in or out of power armor, and a `visible` saved at opening and put back left the crosshair off.
  - The player can't move: `PlayerControls::blockPlayerInput` and an input layer turning off only `kActivate`. A full layer put the gun
    away and hid the HUD. The activate key then comes to the menu as the user event `DISABLED`, and is matched against
    `ControlMap::GetMappedKey("Activate", device)`, so whatever the player has it bound to is the menu's Accept.
  - Esc never reached the menu here, so `UiPages::Frame` reads the key. It acts the frame after the key comes up: closing on the way down
    let the game's own Esc open the pause menu.
  - The press that opens the panel reaches it as well (logged on every opening), and closed the first one again: the menu key closes
    the panel only 250 ms after its first update. Hotkeys fire only as a key goes down, so the press that closes it doesn't open it again.
- **The command bar** (`ui/shared/f4mp/ui/CommandBar.as`) is the game's `ButtonHintBar_mc` of ours. It has one command, the way back. A
  page could have more, side by side as the game's do: `Show` would take a list of commands, each with its key, word and action.
- **Release notes** are a page off About: this version's `dist/RELEASE-*.md`, which the package copies to
  `F4SE/Plugins/F4Multiplayer/release-notes.md` (an rc's own notes in an rc's package; `promote.ps1` puts the release's in the release made from it). No file, no
  link. `ui/shared/f4mp/ui/Markdown.as` shows the little markdown they use: headings, lists, paragraphs, and inline code, bold and links
  as plain text. They're in English whatever the game's language.
- **Typing.** A dialog with a place to type (`Dialog.as`) gets the keys in `OnButtonEvent`, turned to characters with `ToUnicode`, and
  `ControlMap::SetTextEntryMode` keeps them from the game for as long: without it `o` worked the power armor's radio, and others the
  Pip-Boy and VATS. It holds back the menu's own events too, so the arrows or D-pad and Enter or A are read as keys while it's on. The game
  gives a menu only a key's press and release, so the key repeat and the caret are ours. Nothing typed is logged. The left stick moves no
  page of the panel with the dev channel's virtual pad; the D-pad does.
- **Threads.** Flash objects are written only inside a menu's own update (`AdvanceMovie`, hooked in its vtable for
  the main and pause menus, as the compass does for the HUD) or by the menu's own movie; anything that touches the
  game goes to the main thread (`MainThread::Post`).

## Gotchas

- `GFx::Value(const char*)` keeps the pointer; it is copied into an AS3 string when it is set as a member or an
  argument, so a temporary `std::string` is fine there and nowhere else.
- The log privacy test refuses a log line that formats anything ending in `.name`.
- A display object's `x`, `y` are its parent's; the game's panels are drawn at offsets inside `Menu_mc`, itself placed
  to the safe area. Use `getBounds(root)` for where something is on the stage.
- A display object's `mask` failed on the first page drawn (no clipping, and a block of its own) and clipped only after
  the first input (seen under CrossOver; cause not known). The list shows only the rows that fit instead.
- `GetVariable` paths use instance names; objects made at run time get them from `.name`.

## Next

The plan is in [FLASH.md](FLASH.md). Also: the PlayStation glyphs, and the hint following a device change while the menu is open.
