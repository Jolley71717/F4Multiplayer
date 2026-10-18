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
# computer name. (Not the git author: xmake.lua puts it in the DLL's version block on purpose.) A
# short user name or one that is a common word ("dev", "game") can turn up in a binary by chance; the
# scan then refuses the package, and the fix is to package from an account with a distinctive name.
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

# A version as xmake.lua, the tags and dist\RELEASE-*.md name it ("0.8.9" or "0.8.9-rc2") as four
# numbers to compare, or $null for anything else. A release counts as the rc after all of its rcs.
function ConvertTo-ReleaseVersion([string]$Text) {
	if (-not ($Text -match '^(\d+)\.(\d+)\.(\d+)(-rc(\d+))?$')) { return $null }
	$rc = [int]::MaxValue
	if ($Matches[5]) { $rc = [int]$Matches[5] }
	return ,@([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], $rc)
}

# -1, 0 or 1 as version $A is older than, the same as or newer than $B, by number and not as text:
# 0.8.10 is newer than 0.8.9, and 0.8.9-rc2 is newer than 0.8.9-rc1 and older than 0.8.9.
function Compare-ReleaseVersion([string]$A, [string]$B) {
	$left = ConvertTo-ReleaseVersion $A
	$right = ConvertTo-ReleaseVersion $B
	if ($null -eq $left) { throw "'$A' is not a version like 0.8.9 or 0.8.9-rc2" }
	if ($null -eq $right) { throw "'$B' is not a version like 0.8.9 or 0.8.9-rc2" }
	for ($i = 0; $i -lt 4; $i++) {
		if ($left[$i] -lt $right[$i]) { return -1 }
		if ($left[$i] -gt $right[$i]) { return 1 }
	}
	return 0
}

# The names of the notes in $Dir (RELEASE-<version>.md) for versions newer than $Version. Files there
# named for no version are left out.
function Get-NewerReleaseNotes([string]$Dir, [string]$Version) {
	foreach ($note in Get-ChildItem -LiteralPath $Dir -Filter 'RELEASE-*.md' -File) {
		$noteVersion = $note.BaseName.Substring('RELEASE-'.Length)
		if ($null -ne (ConvertTo-ReleaseVersion $noteVersion) -and (Compare-ReleaseVersion $noteVersion $Version) -gt 0) {
			$note.Name
		}
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
