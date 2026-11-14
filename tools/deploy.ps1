# Copies the built plugin into the game's Data/F4SE/Plugins folder.
# The ini is only copied if the game does not already have one, so local settings survive.
param(
	[string]$GamePath = 'F:\SteamLibrary\steamapps\common\Fallout 4',
	[string]$Mode = 'releasedbg',
	[string]$Movies = ''  # the compiled menu movies, as package.yml's "movies" artifact holds them; without it, dist\movies, compiled here when stale
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
# This version's release notes, for the About page's link, if it has them yet.
$version = (Select-String -Path (Join-Path $root 'xmake.lua') -Pattern 'set_version\("([^"]+)"\)').Matches[0].Groups[1].Value
$releaseNotes = Join-Path $root "dist\RELEASE-$version.md"
$deployedNotes = Join-Path $plugins 'F4Multiplayer\release-notes.md'
if (Test-Path $releaseNotes) { Copy-Item $releaseNotes $deployedNotes -Force } else { Remove-Item $deployedNotes -Force -ErrorAction SilentlyContinue }
# The plugin's own menu movies (git holds none). From -Movies when given; otherwise from dist\movies, compiled from
# ui\<Name>\ when one is missing or older than its sources and this PC can compile (Java). A movie that can't be had is
# a warning, not a failure: without it the plugin falls back to its message box.
$interface = Join-Path $GamePath 'Data\Interface\F4Multiplayer'
New-Item -ItemType Directory -Force $interface | Out-Null
$ui = Join-Path $root 'ui'
$sources = @(Get-ChildItem $ui -Recurse -File -Filter '*.as')
$newest = ($sources | Measure-Object LastWriteTimeUtc -Maximum).Maximum
$canCompile = [bool](Get-Command java -ErrorAction SilentlyContinue)
Get-ChildItem $ui -Directory | Where-Object { $_.Name -ne 'shared' } | ForEach-Object {
	$name = $_.Name
	if ($Movies) {
		$movie = Join-Path $Movies "$name.swf"
	} else {
		$movie = Join-Path $root "dist\movies\$name.swf"
		if (-not (Test-Path $movie) -or (Get-Item $movie).LastWriteTimeUtc -lt $newest) {
			if ($canCompile) {
				try { & (Join-Path $PSScriptRoot 'flash\build.ps1') -Names $name }
				catch { Write-Warning "${name}.swf did not compile: $_" }
			} else {
				Write-Warning "${name}.swf is missing or older than ui\: java is not on PATH to compile it (or pass -Movies)"
			}
		}
	}
	if (Test-Path $movie) {
		Copy-Item $movie $interface -Force
	} else {
		Write-Warning "No $movie to deploy: the game keeps the $name.swf it has, if any (without one the plugin shows its message box)"
	}
}

"Deployed to $plugins"
