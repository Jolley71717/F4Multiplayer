# The multiplayer menu and the plugin's own Flash UI

How the Multiplayer entry in the game's main menu and the menu it opens are built, the toolchain for the plugin's own
Flash movies, and what was learned about the game's menus on the way. For the F1 message-box menu see
[MENU.md](MENU.md). Status: a first version. The entry and the panel work at the main menu (seen in one game under
CrossOver); each panel entry still opens the F1 pages; not yet tried in the pause menu, with a gamepad or on Windows.

## The pieces

| What | Where |
|---|---|
| The MULTIPLAYER entry in the main and pause menus, after LOAD | [src/game/MainMenuEntry.cpp](../src/game/MainMenuEntry.cpp), placement rule in [MainMenuPlace.h](../src/game/MainMenuPlace.h) (unit tested) |
| Our menu (F4MPMenu in the game's UI), opened by the entry | [src/game/MPMenu.cpp](../src/game/MPMenu.cpp) |
| Its movie, `Interface/F4Multiplayer/MPMenu.swf` | [ui/MPMenu/MPMenu.as](../ui/MPMenu/MPMenu.as) |
| Pieces for other movies: look, brackets, back hint, entry list | [ui/shared/f4mp/ui/](../ui/shared/f4mp/ui/) (`Style`, `Brackets`, `BackHint`, `EntryList`) |
| The compiler, pinned | [tools/flash/fetch.sh](../tools/flash/fetch.sh) |
| Building the movies | [tools/flash/build.sh](../tools/flash/build.sh) |
| Reading the game's own movies | [tools/flash/ba2extract.py](../tools/flash/ba2extract.py), [tools/flash/swfinfo.py](../tools/flash/swfinfo.py) |

Labels are text keys like every player-facing string (`mainmenu.entry`, `mpmenu.*`): every language file needs every
key, or `text:` tests fail.

## Toolchain

The game's menus are Flash movies played by Scaleform GFx 4. Ours are ActionScript 3 compiled with Apache Flex's
`mxmlc`; no Flex framework, no Adobe tool.

- `tools/flash/fetch.sh` downloads Apache Flex SDK 4.16.1 (prebuilt `mxmlc` and `compc`, Apache-2.0) and Adobe's
  `playerglobal.swc` 32.0 (the `flash.*` API), checks both against pinned SHA-256s and unpacks them into
  `~/.flex/4.16.1`. Nothing of either is committed; `playerglobal.swc` is Adobe's. The Flex tarball's checksum was
  pinned after Apache's MD5 matched and its release signature checked out against Apache Flex's KEYS. Adobe's CDN
  serves only playerglobal 32.0 now (older versions answer 403). Needs Java.
- `tools/flash/build.sh [Name]` builds each `ui/<Name>/<Name>.as` (with `ui/shared` on the source path) into
  `build/ui/<Name>.swf`, at **SWF 15**, the game's own version (Flash Player 11.2), against playerglobal 32.0 linked
  as external, so no Adobe code is in the movie. Use only what Flash Player 11.2 had: Scaleform knows no more.
  Stage 1280x720, as the game's menus.
- `mxmlc` takes several source paths as `-compiler.source-path=a -compiler.source-path+=b`; a second bare path is an
  error ("default arguments may not be interspersed").
- Deploying: `tools/crossovermac.sh deploy` (and `restart`) copies `build/ui/*.swf` into
  `Data/Interface/F4Multiplayer/`, a folder of our own so no other mod's movie clashes. `tools/package.ps1` and the
  Windows deploy don't include the movies yet: to do before this ships.

On a Mac (docs/misc/MAC.md): `xmake build -y F4Multiplayer` with `XMAKE_CONFIGDIR=$PWD/.xmake/win`, then
`tools/crossovermac.sh restart ''` to deploy and stop at the main menu. xmake sometimes stops at once with "please run
`xmake build [target]` to build the following targets first: -> F4MPServer"; run it again (or check the DLL's time).
The full unit test run takes about five minutes under Wine; run the tests a change can touch by name
(`tools/crossovermac.sh test "text:"`, `"log privacy"`, `"main menu entry"`).

## Reading the game's menus

Without a decompiler (JPEXS FFDec would show the AS3 itself; it was not used: downloading and running it needs the
maintainer's or contributor's OK):

```bash
python3 -I tools/flash/ba2extract.py "<game>/Data/Fallout4 - Interface.ba2" <out> 'mainmenu\.swf$'
python3 -I tools/flash/swfinfo.py <out>/Interface/MainMenu.swf | less
```

`swfinfo.py` prints the movie's version and stage, its exported classes and its AS3 string pool: class, member, event
and instance names and text keys, in the order the compiler met them. The pool is deduplicated, so a table of pairs
reads with gaps (see the controller glyphs below). Keep what you extract out of the repo.

At run time:

- The dev channel's `gfx <menu> <path> [depth]` logs a menu's display objects, `gfxset` sets a property.
- `MainMenuEntry` logs, once per launch, the main menu's layout (place, size, alpha, color transform, text font and
  size, three levels down) and how it renders (renderer, flags, every shader target). Search the log for
  `MainMenuEntry: layout` and `shader target`. Drop these logs once the look is settled.

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
  shadow (black, no blur, equal down and right); brackets as one-pixel lines on the pixel's middle (else they blur to
  two pixels and their shadow grows). Controller glyphs are letters in the game's `$Controller_buttons` font:
  `Shared.AS3.BSButtonHint`'s `NameToTextMap` gives Xenon_A "A", Xenon_B "B", Xenon_X "C", Xenon_Y "D",
  Xenon_Select "E", Xenon_LS "F", Xenon_L1 "G", Xenon_L3 "H", Xenon_L2 "I", … Xenon_Start "O" (PlayStation has a map of
  its own, not read yet).
- **The game's tint, tried.** With `kCustomRendering` (through `FlatScreenModel`) and `BSGFxShaderFXTarget`s on the
  movie's layers set as the main menu's (above), the content takes the player's color exactly, but our translucent
  ground came out opaque, and `EnableShadedBackground` on a layer of ours drew nothing (white; black with the no-color
  filter). `SHADED` in MPMenu.cpp keeps that path, off. The movie draws in the HUD color itself.
- **Threads.** Flash objects are written only inside a menu's own update (`AdvanceMovie`, hooked in its vtable for
  the main and pause menus, as the compass does for the HUD) or by the menu's own movie; anything that touches the
  game goes to the main thread (`MainThread::Post`).

## Gotchas

- `GFx::Value(const char*)` keeps the pointer; it is copied into an AS3 string when it is set as a member or an
  argument, so a temporary `std::string` is fine there and nowhere else.
- The log privacy test refuses a log line that formats anything ending in `.name`.
- A display object's `x`, `y` are its parent's; the game's panels are drawn at offsets inside `Menu_mc`, itself placed
  to the safe area. Use `getBounds(root)` for where something is on the stage.
- `GetVariable` paths use instance names; objects made at run time get them from `.name`.

## Next

- Pause menu: check the entry and the panel there (Esc closes the pause menu's own way).
- What the entries do: Host (choose Continue, Load or New, then host on load), Join (friends hosting, then a save),
  Direct connect (a text field), Options. With them, hosting by the menu for this launch only, instead of `bHost`.
- Package the movies (`tools/package.ps1`) and the Windows deploy.
- The PlayStation glyphs; the hint following a device change while the menu is open.
- Inserting into the main menu instead, with its real panels, list and hint bar: possible (MCM does as much in the
  pause menu), at the cost of sharing its state machine and input. Kept in mind; the current menu is its fallback.
