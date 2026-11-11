# questscan.ps1 [-Data <game Data folder>] [-SetsOther | -Moves | -Rules [-Files 2,3] | -PerGame | -DisabledParents | -Companion]
# [-Safe]: reads the game's own files (Fallout4.esm, DLCCoast.esm, DLCRobot.esm, DLCNukaWorld.esm and their script
# archives; nothing is written there) and lists every quest stage whose own fragment takes something from the player,
# unequips them or empties their inventory by a direct call on Game.GetPlayer() or a PlayerRef property, with whether a
# session shares the quest (what it does not see, such as calls through kmyQuest or a player alias, is listed in
# src/game/StageRules.h). src/game/StageRules.h's kPays rules (stages a friend's game never sets here) were checked
# against it; tests/StageRulesTests.cpp and QuestFilterTests.cpp hold what it found. With -SetsOther: the shared quests'
# stages whose fragment sets a stage of another quest, what src/game/StageRules.h's SETS_ANOTHER was made from. With
# -Moves: the shared quests' stages that move the player, by their own fragment, a script it calls or a stage of any
# quest it sets or starts (MQ202 300 sets MQ203 10, Kellogg's memories), what src/game/StageRules.h's kMoves is checked
# against. With -Rules: src/game/StageRules.h's RULES rows for the masters given by their File index (0 Fallout4.esm,
# 1 DLCCoast.esm, 2 DLCRobot.esm, 3 DLCNukaWorld.esm; the Automatron and Nuka-World rows were made so). With -PerGame:
# the quests shared by their type that the story manager starts with targets each game picks itself,
# src/game/QuestFilter.h's PER_GAME. With -Safe: src/game/QuestFilter.h's SAFE (the quests the host's sQuests = safe
# shares, by the rule there; the quests in ALLOW_OUTCOMES in tools/questscan.cs pass it with stages that turn a faction
# on the player or give a settlement, by the maintainer's choice), and what was left out and why; with -Safe -Rules, src/game/StageRules.h's RULES rows for
# those quests alone (-Files 0,1,2,3; a stage of a SAFE or SILENT quest that makes the player an enemy is kMoves, set
# only with the friend near); with -Safe -SetsOther or -Safe -Moves, those scans with them taken for shared.
# -Safe also lists src/game/QuestFilter.h's SILENT after SAFE (the quests the rule passes but for their objectives,
# less the ones denied by name, DENY_PATTERNS in tools/questscan.cs, or by hand, DENY_SILENT, each listed with why;
# -Rules and -SetsOther take them for shared too), and ends with the payments reached through a stage of any quest a
# friend's stage is set on, SAFE's, SILENT's or the story's, the dialogue quests' and the radiants' (a stage it sets
# that has a kPays row in src/game/StageRules.h, or caps taken or the inventory emptied on the way; its
# SHARED_REACHES_PAYS is that count, which tests/StageRulesTests.cpp checks is 0).
# The index of each master is src/game/StageRules.h's File. With -DisabledParents: writes src/game/DisabledParents.h (or
# -Out), the enable parents that a script of the game's own turns off somewhere, read from Fallout4.esm and every
# official add-on installed and their script archives (named in it: COVERED; ParentScan in tools/questscan.cs says how),
# and prints what it read; tests/EnableChainTests.cpp checks the table against what this scan found. Windows PowerShell
# 5.1 compiles the reader (tools/questscan.cs) on the fly. With -Companion: the companions' quests a session shares as side
# quests (src/game/QuestFilter.h's COMPANION_QUESTS), every stage of theirs with what its fragment does and whether it
# is its companion owner's (kEach), and src/game/StageRules.h's RULES rows for them; its EACH rows are written by hand
# from the calls it lists (CompanionScan in tools/questscan.cs).
param([switch]$Companion, [string]$Data = 'F:\SteamLibrary\steamapps\common\Fallout 4\Data', [switch]$SetsOther, [switch]$Moves, [switch]$Rules,
	[int[]]$Files = @(2, 3), [switch]$PerGame, [switch]$Safe, [switch]$DisabledParents,
	[string]$Out = (Join-Path $PSScriptRoot '..\src\game\DisabledParents.h'))
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot 'questscan.cs')) -ReferencedAssemblies System.Core -Language CSharp
if ($DisabledParents) {
	$result = [ParentScan]::Run($Data)
	[IO.File]::WriteAllText([IO.Path]::GetFullPath($Out), $result.Header)  # ASCII, CRLF
	$result.Report
	"# wrote $($result.Count) entries to $([IO.Path]::GetFullPath($Out))"
	return
}
$filter = Join-Path $PSScriptRoot '..\src\game\QuestFilter.h'
$radiants = Join-Path $PSScriptRoot '..\src\game\RadiantRules.h'
$stageRules = Join-Path $PSScriptRoot '..\src\game\StageRules.h'
if ($Companion) { [CompanionScan]::Run($Data, $filter); return }
if ($Safe -and -not ($Rules -or $SetsOther -or $Moves)) { [SafeScan]::Run($Data, $filter, $radiants, $stageRules); return }
if ($Safe) { [void][SafeScan]::Load($Data, $filter, $radiants) }
if ($Moves) { [MoveScan]::Run($Data) }
elseif ($SetsOther) { [CrossScan]::Run($Data) }
elseif ($Rules) { [RuleScan]::Run($Data, $Files, [bool]$Safe) }
elseif ($PerGame) { [PerGameScan]::Run($Data, $radiants) }
else { [QuestScan]::Run($Data) }
