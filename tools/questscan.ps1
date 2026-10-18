# questscan.ps1 [-Data <game Data folder>]: reads the game's own files (Fallout4.esm, DLCCoast.esm and their script
# archives; nothing is written there) and lists every quest stage whose fragment takes something from the player,
# unequips them or empties their inventory, with whether a session shares the quest. src/game/StageRules.h's kPays rules
# (stages a friend's game never sets here) were checked against it; tests/StageRulesTests.cpp and QuestFilterTests.cpp
# hold what it found. Windows PowerShell 5.1 compiles the reader (tools/questscan.cs) on the fly.
param([string]$Data = 'F:\SteamLibrary\steamapps\common\Fallout 4\Data')
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot 'questscan.cs')) -ReferencedAssemblies System.Core -Language CSharp
[QuestScan]::Run($Data)
