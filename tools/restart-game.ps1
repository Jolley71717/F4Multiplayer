# Closes Fallout 4, deploys the latest build, relaunches through F4SE and (optionally)
# loads straight into a cell using the dev channel. Requires bDevChannel = true.
#
#   .\tools\restart-game.ps1                  # load into SanctuaryExt
#   .\tools\restart-game.ps1 -Cell QASmoke
#   .\tools\restart-game.ps1 -Cell ''         # stop at the main menu
param(
	[string]$GamePath = 'F:\SteamLibrary\steamapps\common\Fallout 4',
	[string]$Cell = 'SanctuaryExt',
	[bool]$GodMode = $true,
	[int]$TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'

Get-Process Fallout4 -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
Start-Sleep -Seconds 3

& (Join-Path $PSScriptRoot 'deploy.ps1') -GamePath $GamePath
Start-Process -FilePath (Join-Path $GamePath 'f4se_loader.exe') -WorkingDirectory $GamePath

$log = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Fallout4\F4SE\F4Multiplayer.log'
$launched = Get-Date
$deadline = $launched.AddSeconds($TimeoutSeconds)

# Wait for the plugin to report that game data is loaded (main menu is up).
while ($true) {
	if ((Get-Date) -gt $deadline) { throw 'Timed out waiting for the game to reach the main menu' }
	Start-Sleep -Seconds 2
	if (-not (Get-Process Fallout4 -ErrorAction SilentlyContinue)) { continue }
	if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $launched -and (Select-String -Path $log -Pattern 'Game data ready' -Quiet)) { break }
}
'Main menu reached'

if ($Cell) {
	Start-Sleep -Seconds 3
	$loadTimer = [Diagnostics.Stopwatch]::StartNew()
	& (Join-Path $PSScriptRoot 'devctl.ps1') "console coc $Cell"
	while ($true) {
		if ((Get-Date) -gt $deadline) { throw "Timed out loading $Cell" }
		Start-Sleep -Milliseconds 500
		$status = & (Join-Path $PSScriptRoot 'devctl.ps1') status | Select-Object -Last 1
		if ($status -like 'ingame=true*') { "$status (cell load took $([int]$loadTimer.Elapsed.TotalSeconds)s)"; break }
	}
	if ($GodMode) {
		& (Join-Path $PSScriptRoot 'devctl.ps1') 'console tgm' | Out-Null
	}
}
