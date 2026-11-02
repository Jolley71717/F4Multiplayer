# Builds the plugin's own menu movies on Windows: what build.sh does (docs/UI.md). Each ui\<Name>\<Name>.as,
# with the shared ui\shared, into ui\<Name>\<Name>.swf, which is committed (only a change to ui\ needs Java and this
# compiler). Uses the pinned compiler (fetch.ps1, run first if it is missing), at SWF 15 as the game's movies are,
# against playerglobal 32.0 linked as external (no Adobe code in the movie). Then the compile time is cleared from the
# movie (the same sources give the same bytes) and <Name>.sources written, which F4MPTests checks against the sources.
# Commit all three.
#
#   tools\flash\build.ps1            # every movie
#   tools\flash\build.ps1 MPMenu     # one
param(
	[string[]]$Names = @()
)

$ErrorActionPreference = 'Stop'

$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$ui = Join-Path $root 'ui'
$flex = (& (Join-Path $PSScriptRoot 'fetch.ps1') | Where-Object { $_ -like 'FLEX_HOME=*' } | Select-Object -Last 1) -replace '^FLEX_HOME=', ''
if (-not $flex) { throw 'fetch.ps1 named no FLEX_HOME' }
if (-not (Get-Command java -ErrorAction SilentlyContinue)) { throw 'java is not on PATH: the Flex compiler needs a Java runtime (8 or later)' }
$playerglobal = Join-Path $flex 'frameworks\libs\player\32.0\playerglobal.swc'

# mxmlc writes its compile time into the ProductInfo tag (code 41: ids, version and build, then the date, 8 bytes).
function Clear-CompileTime([string]$Movie) {
	$data = [IO.File]::ReadAllBytes($Movie)
	if ([Text.Encoding]::ASCII.GetString($data, 0, 3) -ne 'FWS') { throw "${Movie}: not an uncompressed movie" }
	$bits = $data[8] -shr 3
	$at = 8 + [Math]::Floor((5 + $bits * 4 + 7) / 8) + 4
	$stamped = $false
	while ($at -lt $data.Length) {
		$header = [BitConverter]::ToUInt16($data, $at)
		$at += 2
		$code = $header -shr 6
		$length = $header -band 0x3F
		if ($length -eq 0x3F) {
			$length = [BitConverter]::ToUInt32($data, $at)
			$at += 4
		}
		if ($code -eq 41 -and $length -ge 26) {
			for ($i = $at + $length - 8; $i -lt $at + $length; $i++) { $data[$i] = 0 }
			$stamped = $true
		}
		$at += $length
		if ($code -eq 0) { break }
	}
	if (-not $stamped) { throw "${Movie}: no ProductInfo tag to clear" }
	[IO.File]::WriteAllBytes($Movie, $data)
}

# <Name>.sources: each source file, by its path under ui/ in ordinal order, with the SHA-256 of its text with LF line
# endings (as build.sh writes it).
function Write-Sources([string]$Name) {
	$files = foreach ($top in @($Name, 'shared')) {
		Get-ChildItem (Join-Path $ui $top) -Recurse -File -Filter '*.as' |
			ForEach-Object { $_.FullName.Substring($ui.Length + 1).Replace('\', '/') }
	}
	$sha = [Security.Cryptography.SHA256]::Create()
	$lines = @("# The sources $Name.swf was built from (tools/flash/build.sh), each with the SHA-256 of its text with LF line endings.")
	$sorted = [string[]]@($files)
	[Array]::Sort($sorted, [StringComparer]::Ordinal)
	$latin1 = [Text.Encoding]::GetEncoding(28591)  # byte for byte: the hash is of the file's bytes, CRLF made LF
	foreach ($path in $sorted) {
		$text = $latin1.GetString([IO.File]::ReadAllBytes((Join-Path $ui $path))).Replace("`r`n", "`n")
		$hash = -join ($sha.ComputeHash($latin1.GetBytes($text)) | ForEach-Object { $_.ToString('x2') })
		$lines += "$hash  $path"
	}
	[IO.File]::WriteAllText((Join-Path $ui "$Name\$Name.sources"), (($lines -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
}

if ($Names.Count -eq 0) {
	$Names = @(Get-ChildItem $ui -Directory | Where-Object { $_.Name -ne 'shared' } | ForEach-Object { $_.Name })
}

foreach ($name in $Names) {
	$src = Join-Path $ui $name
	$movie = Join-Path $src "$name.swf"
	& java '-Dsun.io.useCanonCaches=false' '-Djava.util.Arrays.useLegacyMergeSort=true' -jar (Join-Path $flex 'lib\mxmlc.jar') `
		"+flexlib=$(Join-Path $flex 'frameworks')" '-target-player=32.0' '-swf-version=15' '-compress=false' '-static-link-runtime-shared-libraries=true' `
		'-compiler.library-path=' "-compiler.external-library-path=$playerglobal" "-compiler.source-path=$src" `
		"-compiler.source-path+=$(Join-Path $ui 'shared')" '-default-size' '1280' '720' '-default-frame-rate' '60' `
		'-default-background-color' '0x000000' '-warnings=true' '-output' $movie (Join-Path $src "$name.as")
	if ($LASTEXITCODE -ne 0) { throw "mxmlc failed on $name (exit $LASTEXITCODE)" }
	Clear-CompileTime $movie
	Write-Sources $name
}
