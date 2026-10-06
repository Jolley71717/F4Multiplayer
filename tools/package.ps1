# Builds a release and packages it for players.
#
# Produces dist\out\F4Multiplayer-<version>.zip, laid out like a Data folder so Vortex or
# Mod Organizer 2 can install it directly, plus the standalone server.
param(
	[string]$Mode = 'releasedbg'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$env:Path = "C:\Program Files\xmake;C:\Program Files\Git\cmd;" + $env:Path
xmake config -m $Mode -y | Out-Null
xmake build -y
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$version = (Select-String -Path xmake.lua -Pattern 'set_version\("([^"]+)"\)').Matches[0].Groups[1].Value
$build = Join-Path $root "build\windows\x64\$Mode"
$stage = Join-Path $root 'dist\out\stage'
$zip = Join-Path $root "dist\out\F4Multiplayer-$version.zip"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'F4SE\Plugins') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'F4Multiplayer') | Out-Null

Copy-Item (Join-Path $build 'F4Multiplayer.dll'), (Join-Path $build 'F4Multiplayer.pdb') (Join-Path $stage 'F4SE\Plugins')
Copy-Item (Join-Path $root 'dist\F4Multiplayer.ini') (Join-Path $stage 'F4SE\Plugins')
Copy-Item (Join-Path $build 'F4MPServer.exe') (Join-Path $stage 'F4Multiplayer')
Copy-Item (Join-Path $root 'dist\INSTALL.md') (Join-Path $stage 'F4Multiplayer')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'F4Multiplayer')

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage

"Packaged $zip"
