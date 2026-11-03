# questscan.ps1 [-Data <game Data folder>] [-SetsOther | -Moves | -Rules [-Files 2,3] | -PerGame]: reads the game's own
# files (Fallout4.esm, DLCCoast.esm, DLCRobot.esm, DLCNukaWorld.esm and their script archives; nothing is written there)
# and lists every quest stage whose own fragment takes something from the player, unequips them or empties their
# inventory by a direct call on Game.GetPlayer() or a PlayerRef property, with whether a session shares the quest (what
# it does not see, such as calls through kmyQuest or a player alias, is listed in src/game/StageRules.h).
# src/game/StageRules.h's kPays rules (stages a friend's game never sets here) were checked against it;
# tests/StageRulesTests.cpp and QuestFilterTests.cpp hold what it found. With -SetsOther: the shared quests' stages whose
# fragment sets a stage of another quest, what src/game/StageRules.h's SETS_ANOTHER was made from. With -Moves: the
# shared quests' stages that move the player, by their own fragment, a script it calls or a stage of any quest it sets
# or starts (MQ202 300 sets MQ203 10, Kellogg's memories), what src/game/StageRules.h's kMoves is checked against. With
# -Rules: src/game/StageRules.h's RULES rows for the masters given by their File index (0 Fallout4.esm, 1 DLCCoast.esm,
# 2 DLCRobot.esm, 3 DLCNukaWorld.esm; the Automatron and Nuka-World rows were made so). With -PerGame: the quests shared
# by their type that the story manager starts with targets each game picks itself, src/game/QuestFilter.h's PER_GAME.
# The index of each master is src/game/StageRules.h's File. Windows PowerShell 5.1 compiles the reader
# (tools/questscan.cs) on the fly.
param([string]$Data = 'F:\SteamLibrary\steamapps\common\Fallout 4\Data', [switch]$SetsOther, [switch]$Moves, [switch]$Rules, [int[]]$Files = @(2, 3), [switch]$PerGame)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot 'questscan.cs')) -ReferencedAssemblies System.Core -Language CSharp
if ($Moves) { [MoveScan]::Run($Data) }
elseif ($SetsOther) { [CrossScan]::Run($Data) }
elseif ($Rules) { [RuleScan]::Run($Data, $Files) }
elseif ($PerGame) { [PerGameScan]::Run($Data, (Join-Path $PSScriptRoot '..\src\game\RadiantRules.h')) }
else { [QuestScan]::Run($Data) }
