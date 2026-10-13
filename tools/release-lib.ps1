# Helpers for package.ps1 and promote.ps1 (dot-sourced). Windows PowerShell 5.1.

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

# PowerShell does not stop when a program fails; this does.
function Assert-Native([string]$What) {
	if ($LASTEXITCODE) { throw "$What failed (exit code $LASTEXITCODE)" }
}

function Get-Sha256([string]$Path) {
	(Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

# Zips the contents of $Stage, folders first, with '/' between the parts of every name. Compress-Archive
# in Windows PowerShell 5.1 writes '\', which unzip on Linux or a Steam Deck keeps as part of a file
# name instead of making folders.
function New-Zip([string]$Stage, [string]$Zip) {
	if (Test-Path -LiteralPath $Zip) { Remove-Item -Force -LiteralPath $Zip }
	$prefix = (Resolve-Path -LiteralPath $Stage).ProviderPath.TrimEnd('\').Length + 1
	$archive = [IO.Compression.ZipFile]::Open($Zip, [IO.Compression.ZipArchiveMode]::Create)
	try {
		foreach ($dir in Get-ChildItem -LiteralPath $Stage -Recurse -Directory | Sort-Object FullName) {
			[void]$archive.CreateEntry($dir.FullName.Substring($prefix).Replace('\', '/') + '/')
		}
		foreach ($file in Get-ChildItem -LiteralPath $Stage -Recurse -File | Sort-Object FullName) {
			$name = $file.FullName.Substring($prefix).Replace('\', '/')
			[void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
		}
	} finally {
		$archive.Dispose()
	}
	$bad = @(Get-ZipEntryNames $Zip | Where-Object { $_.Contains('\') })
	if ($bad) { throw "Zip entries named with '\' in ${Zip}: $($bad -join ', ')" }
}

function Get-ZipEntryNames([string]$Zip) {
	$archive = [IO.Compression.ZipFile]::OpenRead($Zip)
	try { $archive.Entries | ForEach-Object { $_.FullName } } finally { $archive.Dispose() }
}

function Expand-Zip([string]$Zip, [string]$Destination) {
	if (Test-Path -LiteralPath $Destination) { Remove-Item -Recurse -Force -LiteralPath $Destination }
	[IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Destination)
}

# What no shipped file may contain: a Windows profile path, or this PC's user name, profile folder or
# computer name. (Not the git author: xmake.lua puts it in the DLL's version block on purpose.)
function Get-LeakPatterns {
	@('Users\', 'Users/', $env:USERNAME, (Split-Path $env:USERPROFILE -Leaf), $env:COMPUTERNAME) |
		Where-Object { $_ -and $_.Length -ge 3 } | Select-Object -Unique
}

# The patterns found in $Bytes as 8-bit text or as UTF-16LE (at either byte alignment), ignoring case.
function Find-Leaks([byte[]]$Bytes, [string[]]$Patterns) {
	$latin1 = [Text.Encoding]::GetEncoding(28591)  # one char per byte, so binaries search as text
	$text = $latin1.GetString($Bytes)
	foreach ($pattern in $Patterns) {
		$wide = $latin1.GetString([Text.Encoding]::Unicode.GetBytes($pattern))
		if ($text.IndexOf($pattern, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
			$text.IndexOf($wide, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
			$pattern
		}
	}
}

# The scan must see a profile path in both encodings, or it would pass anything.
function Assert-LeakScanWorks {
	foreach ($probe in @([Text.Encoding]::Unicode.GetBytes("MZ`0C:\Users\x\src"), [Text.Encoding]::ASCII.GetBytes("MZ`0C:\Users\x\src"))) {
		if (-not (Find-Leaks $probe @('Users\'))) { throw 'The leak scan missed a profile path in its own self-test' }
	}
}

# Reads a file of key=value lines (a manifest, a soak verdict) into a hashtable.
function Read-KeyValues([string]$Path) {
	$values = @{}
	foreach ($line in Get-Content -LiteralPath $Path) {
		if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $values[$Matches[1]] = $Matches[2] }
	}
	$values
}
