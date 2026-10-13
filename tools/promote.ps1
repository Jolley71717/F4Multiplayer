# Makes the release from a release candidate that held, without building anything: the release zips
# hold the rc's own files, byte for byte, so what was soaked is what ships (its plugin log still says
# "build rcN <commit>"; the notes say the release is identical to rcN).
#
#   tools\promote.ps1 -Rc v0.8.6-rc3
#
# Needs, in dist\out: the rc's zip and manifest from tools\package.ps1 -Label rc3, and the rc's soak
# verdict (soak-verdict.txt, see docs\RELEASING.md) naming the same commit and DLL with verdict=PASS.
# The tag v0.8.6-rc3 must point at the manifest's commit. Writes F4Multiplayer-0.8.6.zip and
# F4Multiplayer-0.8.6-nexus.zip, then prints the commands that publish them.
param(
	[Parameter(Mandatory = $true)][string]$Rc,
	[string]$OutDir = '',  # where the rc's files are and the release's go (dist\out)
	[string]$Verdict = ''  # the soak verdict (<OutDir>\soak-verdict.txt)
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
. (Join-Path $PSScriptRoot 'release-lib.ps1')

$env:Path = "C:\Program Files\Git\cmd;" + $env:Path
if (-not $OutDir) { $OutDir = Join-Path $root 'dist\out' }
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath
if (-not $Verdict) { $Verdict = Join-Path $OutDir 'soak-verdict.txt' }

if ($Rc -notmatch '^v(\d+\.\d+\.\d+)-(rc\d+)$') { throw "-Rc takes an rc tag such as v0.8.6-rc3, not '$Rc'" }
$version = $Matches[1]
$label = $Matches[2]
$rcName = "F4Multiplayer-$version-$label"
$rcZip = Join-Path $OutDir "$rcName.zip"
$manifestPath = Join-Path $OutDir "$rcName.manifest.txt"
foreach ($path in $rcZip, $manifestPath, $Verdict) {
	if (-not (Test-Path -LiteralPath $path)) { throw "Missing $path" }
}

# The rc is the one that was packaged, from a commit, and that commit is the tag.
$manifest = Read-KeyValues $manifestPath
if ($manifest['dirty'] -ne 'false') { throw "$rcName was packaged from uncommitted changes (-AllowDirty); it can't be released" }
if ($manifest['version'] -ne $version) { throw "The manifest is for version $($manifest['version']), not $version" }
if ((Get-Sha256 $rcZip) -ne $manifest['zip_sha256']) { throw "$rcName.zip is not the zip its manifest describes" }
$commit = $manifest['commit']
$tagged = git rev-parse --verify --quiet "$Rc^{commit}"
if (-not $tagged) { throw "There is no tag $Rc (tag the rc's commit first: git tag $Rc $commit)" }
if ($tagged.Trim() -ne $commit) { throw "$Rc points at $($tagged.Trim()), but $rcName was built from $commit" }

# What was soaked: the verdict must be for this commit and this DLL, and a pass.
$soak = Read-KeyValues $Verdict
$soakCommit = "$($soak['commit'])"
if ($soak['verdict'] -ne 'PASS') { throw "The soak verdict in $Verdict is '$($soak['verdict'])', not PASS" }
if ($soakCommit.Length -lt 7 -or -not $commit.StartsWith($soakCommit)) { throw "The soak verdict is for commit '$soakCommit', not $commit" }
if ($soak['dll_sha256'] -ne $manifest['dll_sha256']) { throw "The soak verdict is for a different DLL ($($soak['dll_sha256'])) than $rcName.zip holds" }

$stage = Join-Path $OutDir 'promote-stage'
Expand-Zip $rcZip $stage
try {
	$dll = Join-Path $stage 'F4SE\Plugins\F4Multiplayer.dll'
	if ((Get-Sha256 $dll) -ne $manifest['dll_sha256']) { throw "The DLL in $rcName.zip is not the one its manifest names" }

	$name = "F4Multiplayer-$version"
	$zip = Join-Path $OutDir "$name.zip"
	New-Zip $stage $zip
	# The Nexus upload leaves out the dedicated server, as tools\package.ps1 -Nexus does.
	Remove-Item -LiteralPath (Join-Path $stage 'F4Multiplayer\F4MPServer.exe')
	$nexusZip = Join-Path $OutDir "$name-nexus.zip"
	New-Zip $stage $nexusZip
} finally {
	Remove-Item -Recurse -Force -LiteralPath $stage
}

# Crash logs from the release are read with the rc's .pdb, under the release's name too.
$rcPdb = Join-Path $OutDir $manifest['pdb']
$pdb = "symbols/$name-$($commit.Substring(0, 7)).pdb"
if (Test-Path -LiteralPath $rcPdb) {
	Copy-Item -LiteralPath $rcPdb (Join-Path $OutDir $pdb)
} else {
	Write-Warning "No $rcPdb on this PC: crash logs from the release can't be symbolized here"
}

@(
	"version=$version"
	"label=$($manifest['label'])"
	"commit=$commit"
	'dirty=false'
	"zip=$name.zip"
	"zip_sha256=$(Get-Sha256 $zip)"
	"dll_sha256=$($manifest['dll_sha256'])"
	"pdb=$pdb"
	"enet=$($manifest['enet'])"
	"promoted_from=$Rc"
) | Set-Content -LiteralPath (Join-Path $OutDir "$name.manifest.txt") -Encoding ASCII

"Promoted $Rc to $zip (and $nexusZip)"
"Publish it:"
"  git tag v$version $commit"
"  git push origin v$version"
"  gh release create v$version `"$zip`" --title $version --notes-file dist/RELEASE-$version.md"
