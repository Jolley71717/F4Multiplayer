# Builds a release and packages it for players.
#
# Produces dist\out\F4Multiplayer-<version>.zip, laid out like a Data folder so Vortex or
# Mod Organizer 2 can install it directly, plus the standalone server.
param(
	[string]$Mode = 'releasedbg',
	[string]$Label = '',  # "rc1" for a release candidate: named in the zip and in the plugin log
	[switch]$Nexus  # the Nexus upload: without F4MPServer.exe (only dedicated servers need it, and it is one more compiled file for their scanner); the GitHub release carries the full zip
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$env:Path = "C:\Program Files\xmake;C:\Program Files\Git\cmd;" + $env:Path
$env:F4MP_LABEL = $Label
xmake config -m $Mode -y | Out-Null
xmake build -y
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$version = (Select-String -Path xmake.lua -Pattern 'set_version\("([^"]+)"\)').Matches[0].Groups[1].Value
$build = Join-Path $root "build\windows\x64\$Mode"
$stage = Join-Path $root 'dist\out\stage'
$suffix = ''
if ($Label) { $suffix += "-$Label" }
if ($Nexus) { $suffix += '-nexus' }
$zip = Join-Path $root "dist\out\F4Multiplayer-$version$suffix.zip"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'F4SE\Plugins') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'F4Multiplayer') | Out-Null

# The .pdb stays out of the zip (it is full of build paths); it is kept next to the zip for
# reading crash logs from this version.
Copy-Item (Join-Path $build 'F4Multiplayer.dll') (Join-Path $stage 'F4SE\Plugins')
Copy-Item (Join-Path $build 'F4Multiplayer.pdb') (Join-Path $root "dist\out\F4Multiplayer-$version.pdb")
Copy-Item (Join-Path $root 'dist\F4Multiplayer.ini') (Join-Path $stage 'F4SE\Plugins')
if (-not $Nexus) { Copy-Item (Join-Path $build 'F4MPServer.exe') (Join-Path $stage 'F4Multiplayer') }
Copy-Item (Join-Path $root 'dist\INSTALL.md') (Join-Path $stage 'F4Multiplayer')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'F4Multiplayer')

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

# Nothing in the zip may name this PC or its user.
$leaks = Get-ChildItem $stage -Recurse -File | Where-Object {
	Select-String -Path $_.FullName -Pattern 'Users\\', $env:USERNAME -SimpleMatch -Quiet
}
Remove-Item -Recurse -Force $stage
if ($leaks) {
	Remove-Item -Force $zip
	throw "Build paths or the user name leaked into: $($leaks.Name -join ', ')"
}

"Packaged $zip"
