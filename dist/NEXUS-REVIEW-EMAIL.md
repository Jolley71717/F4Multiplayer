# Email to Nexus support (quarantined file review)

To: support@nexusmods.com
Subject: File review request: F4Multiplayer - Co-op over Steam (Fallout 4 mod 109812)

Hi,

The main file of my mod has been quarantined by the automated checks. It contains compiled code, so here is the source and how to build it.

- Mod page: https://www.nexusmods.com/fallout4/mods/109812
- File: F4Multiplayer-0.6.0.zip (F4SE\Plugins\F4Multiplayer.dll, F4Multiplayer\F4MPServer.exe)
- Source: https://github.com/Jolley71717/F4Multiplayer (GPL-3.0; tag v0.6.0 is the uploaded build)
- VirusTotal: 0/67 detections

What it is: an F4SE plugin (DLL) that adds co-op for up to 4 players. It talks to other players through Steam's networking (via the game's own steam_api64.dll) or UDP. F4MPServer.exe is an optional standalone relay server for people who don't want to play through Steam.

Build steps (Windows, Visual Studio 2022 with "Desktop development with C++", Git, xmake 3.0+):

    git clone --recurse-submodules https://github.com/Jolley71717/F4Multiplayer
    cd F4Multiplayer
    git checkout v0.6.0
    xmake config -m releasedbg
    xmake build

This produces build\windows\x64\releasedbg\F4Multiplayer.dll and F4MPServer.exe; tools\package.ps1 produces the exact zip that was uploaded. Dependencies are CommonLibF4 (git submodule, MIT) and ENet (fetched by xmake, MIT). No Steamworks SDK is required.

Thanks for taking a look.

Jolley71717
