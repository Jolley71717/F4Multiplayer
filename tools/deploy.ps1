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

# The texts in every language (always replaced: they belong to the build, unlike the ini).
$lang = Join-Path $plugins 'F4Multiplayer\lang'
New-Item -ItemType Directory -Force $lang | Out-Null
Copy-Item (Join-Path $root 'dist\lang\*.txt') $lang -Force
# The plugin's own menu movies, committed in ui\<Name>\.
$interface = Join-Path $GamePath 'Data\Interface\F4Multiplayer'
New-Item -ItemType Directory -Force $interface | Out-Null
Get-ChildItem (Join-Path $root 'ui') -Directory | ForEach-Object { Get-ChildItem $_.FullName -Filter '*.swf' -File } | Copy-Item -Destination $interface -Force

"Deployed to $plugins"
