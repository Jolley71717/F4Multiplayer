# Builds the plugin's own menu movies on Windows: what build.sh does (docs/UI.md). Each ui\<Name>\<Name>.as, with the
# shared ui\shared, into build\ui\<Name>.swf with the pinned compiler (fetch.ps1, run first if it is missing), at SWF
# 15 as the game's movies are, against playerglobal 32.0 linked as external (no Adobe code in the movie). Needs Java.
#
#   tools\flash\build.ps1            # every movie
#   tools\flash\build.ps1 MPMenu     # one
param(
	[string[]]$Names = @()
)

$ErrorActionPreference = 'Stop'

$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$flex = (& (Join-Path $PSScriptRoot 'fetch.ps1') | Where-Object { $_ -like 'FLEX_HOME=*' } | Select-Object -Last 1) -replace '^FLEX_HOME=', ''
if (-not $flex) { throw 'fetch.ps1 named no FLEX_HOME' }
if (-not (Get-Command java -ErrorAction SilentlyContinue)) { throw 'java is not on PATH: the Flex compiler needs a Java runtime (8 or later)' }
$playerglobal = Join-Path $flex 'frameworks\libs\player\32.0\playerglobal.swc'
$out = Join-Path $root 'build\ui'
New-Item -ItemType Directory -Force $out | Out-Null

if ($Names.Count -eq 0) {
	$Names = @(Get-ChildItem (Join-Path $root 'ui') -Directory | Where-Object { $_.Name -ne 'shared' } | ForEach-Object { $_.Name })
}

foreach ($name in $Names) {
	$src = Join-Path $root "ui\$name"
	& java '-Dsun.io.useCanonCaches=false' '-Djava.util.Arrays.useLegacyMergeSort=true' -jar (Join-Path $flex 'lib\mxmlc.jar') `
		"+flexlib=$(Join-Path $flex 'frameworks')" '-target-player=32.0' '-swf-version=15' '-static-link-runtime-shared-libraries=true' `
		'-compiler.library-path=' "-compiler.external-library-path=$playerglobal" "-compiler.source-path=$src" `
		"-compiler.source-path+=$(Join-Path $root 'ui\shared')" '-default-size' '1280' '720' '-default-frame-rate' '60' `
		'-default-background-color' '0x000000' '-warnings=true' '-output' (Join-Path $out "$name.swf") (Join-Path $src "$name.as")
	if ($LASTEXITCODE -ne 0) { throw "mxmlc failed on $name (exit $LASTEXITCODE)" }
}
