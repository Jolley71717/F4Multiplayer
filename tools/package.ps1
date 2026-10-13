# Builds a release from a committed tree, runs the unit tests and packages it for players.
#
# Produces dist\out\F4Multiplayer-<version>[-<label>].zip, laid out like a Data folder so Vortex or
# Mod Organizer 2 can install it directly, plus the standalone server. Beside it go a manifest (the
# commit, the build label and the DLL's SHA-256, which tools\promote.ps1 checks) and, under symbols\,
# the .pdb for reading crash logs from exactly this build. docs\RELEASING.md has the order: package,
# then tag the commit the manifest names, then publish.
param(
	[string]$Mode = 'releasedbg',
	[string]$Label = '',  # "rc3" for a release candidate: named in the zip, and with the commit in the plugin log
	[switch]$Nexus,  # the Nexus upload: without F4MPServer.exe (only dedicated servers need it, and it is one more compiled file for their scanner); the GitHub release carries the full zip
	[string]$OutDir = '',  # where the zip goes (dist\out); a scratch folder for trying the script out
	[switch]$AllowDirty  # local experiments only: packages uncommitted changes, the build calls itself "-dirty" and promote.ps1 refuses it
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
. (Join-Path $PSScriptRoot 'release-lib.ps1')

$env:Path = "C:\Program Files\xmake;C:\Program Files\Git\cmd;" + $env:Path
if (-not $OutDir) { $OutDir = Join-Path $root 'dist\out' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath

# The label goes into a C++ string literal and into file names.
if ($Label -notmatch '^[A-Za-z0-9.-]*$') { throw "Label '$Label' may hold only letters, digits, '.' and '-'" }
$version = (Select-String -Path xmake.lua -Pattern 'set_version\("([^"]+)"\)').Matches[0].Groups[1].Value

# What is packaged is a commit: no changes to tracked files, and an rc's notes are part of it.
$changed = @(git status --porcelain --untracked-files=no)
Assert-Native 'git status'
$untracked = @(git status --porcelain | Where-Object { $_.StartsWith('?? ') } | ForEach-Object { $_.Substring(3) } |
	Where-Object { -not $_.StartsWith('.claude/') -and -not $_.StartsWith('dist/out/') })
if ($untracked) { Write-Warning "Untracked files (not packaged, but check they belong nowhere): $($untracked -join ', ')" }
$problems = @()
if ($changed) { $problems += "uncommitted changes:`n  $($changed -join "`n  ")" }
if ($Label) {
	$notes = "dist/RELEASE-$version-$Label.md"
	if (-not (git ls-files -- $notes)) { $problems += "$notes is not committed (the rc's commit carries its notes)" }
}
if ($problems) {
	if (-not $AllowDirty) { throw "Not packaging: $($problems -join '; ')" }
	Write-Warning "Packaging anyway (-AllowDirty): $($problems -join '; ')"
}

# Text that has gone stale in what players read before.
foreach ($doc in 'dist\INSTALL.md', 'dist\NEXUS.md') {
	if (Select-String -LiteralPath $doc -Pattern 'F9 point' -SimpleMatch -Quiet) { throw "$doc still says the emotes are on F9 (F9 is the game's quickload)" }
}

# The plugin's first log lines name the build: "F4Multiplayer build rc3 1a2b3c4".
$commit = (git rev-parse HEAD).Trim()
Assert-Native 'git rev-parse'
$sha = $commit.Substring(0, 7)
if ($problems) { $sha += '-dirty' }
$env:F4MP_LABEL = "$Label $sha".Trim()

# -P .: in a worktree nested inside another checkout, xmake would otherwise build the outer one.
xmake config -P . -m $Mode -y | Out-Null
Assert-Native 'xmake config'
xmake build -P . -y  # every default target, the unit tests included
Assert-Native 'xmake build'

$build = Join-Path $root "build\windows\x64\$Mode"
$testOutput = & (Join-Path $build 'F4MPTests.exe')
$testsFailed = $LASTEXITCODE
$testOutput | Where-Object { -not $_.StartsWith('ok ') }
if ($testsFailed) { throw "F4MPTests: $testsFailed test(s) failed" }

$suffix = ''
if ($Label) { $suffix += "-$Label" }
if ($Nexus) { $suffix += '-nexus' }
$name = "F4Multiplayer-$version$suffix"
$zip = Join-Path $OutDir "$name.zip"
$stage = Join-Path $OutDir 'stage'

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'F4SE\Plugins') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'F4Multiplayer') | Out-Null

Copy-Item (Join-Path $build 'F4Multiplayer.dll') (Join-Path $stage 'F4SE\Plugins')
Copy-Item (Join-Path $root 'dist\F4Multiplayer.ini') (Join-Path $stage 'F4SE\Plugins')
if (-not $Nexus) { Copy-Item (Join-Path $build 'F4MPServer.exe') (Join-Path $stage 'F4Multiplayer') }
Copy-Item (Join-Path $root 'dist\INSTALL.md') (Join-Path $stage 'F4Multiplayer')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'F4Multiplayer')

# Nothing in the zip may name this PC or its user.
Assert-LeakScanWorks
$patterns = @(Get-LeakPatterns)
$leaks = foreach ($file in Get-ChildItem $stage -Recurse -File) {
	$found = @(Find-Leaks ([IO.File]::ReadAllBytes($file.FullName)) $patterns)
	if ($found) { "$($file.Name) ($($found.Count) pattern(s))" }
}
if ($leaks) {
	Remove-Item -Recurse -Force $stage
	throw "Build paths or this PC's names leaked into: $($leaks -join ', ')"
}

New-Zip $stage $zip
$dllHash = Get-Sha256 (Join-Path $stage 'F4SE\Plugins\F4Multiplayer.dll')
Remove-Item -Recurse -Force $stage

# The .pdb stays out of the zip (it is full of build paths, so it is never uploaded anywhere); it is
# kept here, named for this exact build, for reading crash logs from it.
$symbols = Join-Path $OutDir 'symbols'
New-Item -ItemType Directory -Force $symbols | Out-Null
$pdb = "$name-$sha.pdb"
Copy-Item (Join-Path $build 'F4Multiplayer.pdb') (Join-Path $symbols $pdb)

$enet = (Select-String -Path xmake.lua -Pattern 'add_requires\("enet ([^"]+)"\)').Matches[0].Groups[1].Value
$manifest = Join-Path $OutDir "$name.manifest.txt"
@(
	"version=$version"
	"label=$env:F4MP_LABEL"
	"commit=$commit"
	"dirty=$([bool]$problems)".ToLowerInvariant()
	"zip=$name.zip"
	"zip_sha256=$(Get-Sha256 $zip)"
	"dll_sha256=$dllHash"
	"pdb=symbols/$pdb"
	"enet=$enet"
) | Set-Content -LiteralPath $manifest -Encoding ASCII

"Packaged $zip"
"  build $env:F4MP_LABEL, DLL sha256 $dllHash"
"  manifest $manifest"
if ($Label -and -not $problems) { "Next: git tag v$version-$Label $commit, then the release (docs\RELEASING.md)." }
