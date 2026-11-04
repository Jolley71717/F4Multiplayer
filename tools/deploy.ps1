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
Copy-Item (Join-Path $root 'dist\about.txt') (Join-Path $plugins 'F4Multiplayer') -Force
# The plugin's own menu movies, compiled from ui\<Name>\ when one is missing or older than its sources (git holds no movies).
$interface = Join-Path $GamePath 'Data\Interface\F4Multiplayer'
New-Item -ItemType Directory -Force $interface | Out-Null
$ui = Join-Path $root 'ui'
$sources = @(Get-ChildItem $ui -Recurse -File -Filter '*.as')
$newest = ($sources | Measure-Object LastWriteTimeUtc -Maximum).Maximum
Get-ChildItem $ui -Directory | Where-Object { $_.Name -ne 'shared' } | ForEach-Object {
	$movie = Join-Path $root "dist\movies\$($_.Name).swf"
	if (-not (Test-Path $movie) -or (Get-Item $movie).LastWriteTimeUtc -lt $newest) {
		& (Join-Path $PSScriptRoot 'flash\build.ps1') -Names $_.Name
	}
	Copy-Item $movie $interface -Force
}

"Deployed to $plugins"
