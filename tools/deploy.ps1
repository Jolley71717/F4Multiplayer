# Copies the built plugin into the game's Data/F4SE/Plugins folder.
# The ini is only copied if the game does not already have one, so local settings survive.
param(
	[string]$GamePath = 'F:\SteamLibrary\steamapps\common\Fallout 4',
	[string]$Mode = 'releasedbg'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build\windows\x64\$Mode"
$plugins = Join-Path $GamePath 'Data\F4SE\Plugins'

New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item (Join-Path $build 'F4Multiplayer.dll'), (Join-Path $build 'F4Multiplayer.pdb') $plugins -Force

$ini = Join-Path $plugins 'F4Multiplayer.ini'
if (-not (Test-Path $ini)) {
	Copy-Item (Join-Path $root 'dist\F4Multiplayer.ini') $ini
}

"Deployed to $plugins"
