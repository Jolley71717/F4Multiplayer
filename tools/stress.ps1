# The menu stress loop (tools/menustress.sh) from PowerShell: workshop mode, the Pip-Boy and a conversation
# over and over on this PC's game, the VM's or both, with a hang caught in the act on this PC (every thread's
# stack and a minidump, through tools/hangcatch.ps1). menustress.sh's header says what it does and counts as
# stuck; its settings (WS_SCRAP, PICK, HANGCATCH and the rest) are read from the environment as they are.
#
#   tools\stress.ps1 [-Minutes 60] [-Games host|vm|both] [-Steps wpd] [-Npc 0001CA7D|none] [-OutDir <folder>]
#
# It runs the script with Git for Windows' bash, never WSL's (System32\bash.exe): WSL's has its own
# 127.0.0.1, where the game's dev channel isn't, and no cygpath or powershell.exe for the catch. The exit
# code is the loop's: 1 when anything got stuck or hung, else 0.
param(
	[int]$Minutes = 60,
	[ValidateSet('host', 'vm', 'both')][string]$Games = 'both',
	[string]$Steps = 'wpd',
	[string]$Npc = '0001CA7D',
	[string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'

# Git's bash.exe is in <Git>\bin; git.exe on the PATH is in <Git>\cmd (or <Git>\mingw64\bin).
$candidates = @()
$git = (Get-Command git.exe -ErrorAction SilentlyContinue).Source
if ($git) {
	$folder = Split-Path $git
	for ($up = 0; $up -lt 3 -and $folder; $up++) {
		$candidates += Join-Path $folder 'bin\bash.exe'
		$folder = Split-Path $folder
	}
}
$candidates += @(
	(Join-Path $env:ProgramFiles 'Git\bin\bash.exe'),
	(Join-Path $env:LOCALAPPDATA 'Programs\Git\bin\bash.exe'))
$bash = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $bash) { throw 'No Git for Windows bash.exe found: install Git for Windows (winget install Git.Git)' }

# The repository checks text files out with Windows line ends, which bash reads as part of each command
# unless told to drop them (igncr, an option of Git's bash).
$bashArgs = @()
$ErrorActionPreference = 'Continue'  # Windows PowerShell turns a native program's error output into errors
& $bash -o igncr -c true 2>$null
$igncr = $LASTEXITCODE -eq 0
$ErrorActionPreference = 'Stop'
if ($igncr) { $bashArgs += @('-o', 'igncr') }

$script = (Join-Path $PSScriptRoot 'menustress.sh') -replace '\\', '/'
$bashArgs += @($script, '-m', $Minutes, '-g', $Games, '-s', $Steps, '-n', $Npc)
if ($OutDir) {
	# Whole, since bash's working folder isn't necessarily PowerShell's.
	$full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
	$bashArgs += @('-o', ($full -replace '\\', '/'))
}
& $bash @bashArgs
exit $LASTEXITCODE
