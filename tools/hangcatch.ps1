# Catches a frozen Fallout 4 in the act. Once a second it asks Windows whether the game's window answers
# (IsHungAppWindow: what makes Windows call a window Not Responding). When it hasn't for -Seconds in a
# row, the console debugger attaches without taking the game over (cdb -pv: it stops the threads, reads
# them and lets them go), prints the engine's texture-wait state (the counters the position job waits on,
# docs/HANG-20261017.md), writes a full-memory dump (.dump /ma: the heap and the game's globals too, so
# the counters and batches can be read from it later) and every thread's stack (~*kn) into -OutDir, and
# detaches (qd). A full dump is the game's whole memory, several GB: with less than -MinFreeGB free on the
# drive (or with -Mini) it writes the stacks-only minidump (.dump /mt) instead, and only the newest
# -KeepDumps dumps are kept. One catch per freeze: the next comes only after the game answered again.
# The plugin's own hang watch (iHangSeconds) logs where the main thread is; this one sees every thread,
# from outside, so it works even when the plugin's thread is stuck with the rest.
#
#   tools\hangcatch.ps1 [-OutDir <folder>] [-Seconds 10] [-Minutes 0] [-Cdb <path>] [-Keep 20] [-KeepDumps 2] [-Mini]
#   tools\hangcatch.ps1 -Now [-OutDir <folder>]   # one catch, at once (menustress.sh's on a hung game thread)
#   tools\hangcatch.ps1 -Now -ProcessId <pid>     # try the debugger once on another program's window
#
# The debugger is cdbX64 from WinDbg (the Microsoft Store's, on the PATH as cdbX64) or cdb from the Windows
# SDK's Debugging Tools; -Cdb names another. -Minutes 0 watches until stopped (Ctrl+C). The game's own
# loading screens can keep the window from answering for a while: a catch then is a slow load, which the
# report's main thread shows (raise -Seconds if they come often). Each catch is hang-<time>.dmp and
# hang-<time>.txt (the stacks, and the debugger's output); the newest -Keep of each are kept.
param(
	[string]$OutDir = '',
	[int]$Seconds = 10,
	[int]$Minutes = 0,
	[string]$Cdb = '',
	[int]$Keep = 20,
	[string]$ProcessName = 'Fallout4',
	[int]$ProcessId = 0,  # this process instead of one by name
	[int]$KeepDumps = 2,  # full dumps are several GB each
	[int]$MinFreeGB = 30,
	[switch]$Mini,        # the stacks-only minidump (.dump /mt), as before
	[switch]$Now
)

$ErrorActionPreference = 'Stop'

if ($Seconds -lt 1) { throw '-Seconds must be at least 1' }
if ($Keep -lt 1) { throw '-Keep must be at least 1' }
if ($KeepDumps -lt 1) { throw '-KeepDumps must be at least 1' }
if (-not $OutDir) {
	$OutDir = if ($env:F4MP_STAGE) { Join-Path $env:F4MP_STAGE 'hangs' } else { Join-Path ([IO.Path]::GetTempPath()) 'f4mp-hangs' }
}
New-Item -ItemType Directory -Force $OutDir | Out-Null

if (-not $Cdb) {
	foreach ($candidate in @(
			(Get-Command cdbX64.exe -ErrorAction SilentlyContinue).Source,
			(Get-Command cdb.exe -ErrorAction SilentlyContinue).Source,
			(Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Debuggers\x64\cdb.exe'),
			(Join-Path $env:ProgramFiles 'Windows Kits\10\Debuggers\x64\cdb.exe'))) {
		if ($candidate -and (Test-Path $candidate)) { $Cdb = $candidate; break }
	}
}
if (-not $Cdb -or -not (Test-Path $Cdb)) {
	throw 'No console debugger: install WinDbg (winget install Microsoft.WinDBG) or the Windows SDK Debugging Tools, or pass -Cdb'
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class F4HangCatch {
	[DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr hWnd);
	[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr processHandle);
}
'@

function Write-Line([string]$text) {
	Write-Host ("{0} {1}" -f (Get-Date -Format 'HH:mm:ss'), $text)
}

# The newest $Keep files of a kind stay; older catches go.
function Remove-Old([string]$pattern, [int]$keep = $Keep) {
	Get-ChildItem -Path $OutDir -Filter $pattern -File | Sort-Object LastWriteTime -Descending | Select-Object -Skip $keep | Remove-Item -Force -ErrorAction SilentlyContinue
}

# The debugger's commands that print the engine's texture-wait state (docs/HANG-20261017.md): pending, done and total,
# the stage, the texture update's lock (owner thread and count), the current and second batches and whether the current
# one holds requests not handed to the streamer (its list head), the main thread's own batch (a static one, set aside
# while the wait runs) with its list head, and the lock owner's stack. Offsets from the game's Address Library, by ID; none when
# it can't be read (another game version without one), and the catch goes on without them.
function Get-TextureWaitCommands([System.Diagnostics.Process]$game) {
	try {
		$exe = $game.Path
		$version = (Get-Item $exe).VersionInfo.FileVersion -replace '\.', '-'
		$bin = Join-Path (Split-Path $exe) "Data\F4SE\Plugins\version-$version.bin"
		$db = [IO.File]::ReadAllBytes($bin)
	} catch {
		Write-Line "no texture-wait counters: the Address Library couldn't be read ($($_.Exception.Message))"
		return @()
	}
	# (No key named count: $at.Count would read it instead of the table's size.)
	$wanted = @{ 2698346 = 'pending'; 2698349 = 'done'; 2698350 = 'second'; 2698351 = 'current'; 2698352 = 'total'; 2698354 = 'stage'; 2698358 = 'owner'; 2698359 = 'held';
		2698368 = 'main' }
	$at = @{}
	$count = [BitConverter]::ToUInt64($db, 0)
	for ($i = 0; $i -lt $count; $i++) {
		$id = [BitConverter]::ToUInt64($db, 8 + $i * 16)
		if ($wanted.ContainsKey([int]$id)) { $at[$wanted[[int]$id]] = 'Fallout4+0x{0:X}' -f [BitConverter]::ToUInt64($db, 16 + $i * 16) }
	}
	if ($at.Count -ne $wanted.Count) {
		Write-Line 'no texture-wait counters: an ID is missing from the Address Library'
		return @()
	}
	return @(
		'.echo ---- texture wait',
		".printf `"pending=%d done=%u total=%u stage=%u\n`", by($($at.pending)), dwo($($at.done)), dwo($($at.total)), dwo($($at.stage))",
		".printf `"lock owner=0x%x count=%u\n`", dwo($($at.owner)), dwo($($at.held))",
		".printf `"current batch=%p head=%p; second batch=%p head=%p\n`", qwo($($at.current)), qwo(qwo($($at.current))+8), qwo($($at.second)), qwo(qwo($($at.second))+8)",
		".printf `"the main thread's batch=%p head=%p (set aside while the wait runs: a head here is a request never handed over)\n`", $($at.main), qwo($($at.main)+8)",
		"r `$t0 = dwo($($at.owner))",
		'.if (@$t0 != 0) { ~~[@$t0]s; ~~[@$t0]kn 8 }',
		'~0s',
		'.echo ---- end of texture wait')
}

# Attaches, writes the dump and the stacks, detaches. The debugger gets two minutes (ten for a full dump): a stuck debugger is
# killed. With -pv it suspended every thread of the game and only its detach (qd) resumes them; a thread's
# suspension outlives the process that asked for it, so after a kill the game is resumed here, once.
function Invoke-Catch([System.Diagnostics.Process]$game, [string]$why) {
	$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
	$dump = Join-Path $OutDir "hang-$stamp.dmp"
	$text = Join-Path $OutDir "hang-$stamp.txt"
	$errors = Join-Path $OutDir "hang-$stamp.err"
	# The commands go in a file (-cf): the dump's path may have spaces, which a -c line can't quote. The dump
	# right after the counters (a few plain reads), so a catch always has it. Symbols only from a folder of our own (-y): the debugger's default is
	# Microsoft's symbol server, and downloading every module's took minutes with the game stopped. The
	# stacks name functions by the modules' exports (ntdll, kernel32) and the rest as module+offset, which is
	# what the plugin's crash and hang reports give too.
	$script = Join-Path $OutDir "hang-$stamp.cdb"
	$symbols = Join-Path $OutDir 'symbols'
	New-Item -ItemType Directory -Force $symbols | Out-Null
	$kind = '/ma'
	if ($Mini) {
		$kind = '/mt'
	} else {
		$free = (Get-PSDrive -Name ([IO.Path]::GetPathRoot((Resolve-Path $OutDir).Path).Substring(0, 1))).Free / 1GB
		if ($free -lt $MinFreeGB) {
			Write-Line ("only {0:N0} GB free for a full dump (-MinFreeGB {1}): writing the stacks-only minidump" -f $free, $MinFreeGB)
			$kind = '/mt'
		}
	}
	# The counters first (they are what a catch is for now), then the dump, then the stacks.
	Set-Content -Path $script -Encoding ascii -Value (@(".echo ---- $why", '.time') + (Get-TextureWaitCommands $game) + @(
		".dump $kind `"$dump`"",
		'.kframes 0x80',
		'~*kn',
		'qd'))
	Write-Line "catching pid $($game.Id): $why"
	$cdbProcess = Start-Process -FilePath $Cdb -ArgumentList @('-pv', '-p', $game.Id, '-y', "`"$symbols`"", '-cf', "`"$script`"") -NoNewWindow -PassThru `
		-RedirectStandardOutput $text -RedirectStandardError $errors
	$limit = if ($kind -eq '/ma') { 600 } else { 120 }
	if (-not $cdbProcess.WaitForExit($limit * 1000)) {
		Write-Line "the debugger took over $limit s: stopped"
		Stop-Process -Id $cdbProcess.Id -Force -ErrorAction SilentlyContinue
		$cdbProcess.WaitForExit(10000) | Out-Null
		try {
			$status = [F4HangCatch]::NtResumeProcess($game.Handle)
			Write-Line ("resumed the game's threads (status {0:X8})" -f $status)
		} catch {
			Write-Line "could not resume the game's threads: $($_.Exception.Message)"
		}
	}
	Remove-Item $script -Force -ErrorAction SilentlyContinue
	if ((Test-Path $errors) -and (Get-Item $errors).Length -eq 0) { Remove-Item $errors -Force }
	$dumped = Test-Path $dump
	Write-Line ("wrote {0}{1}" -f $text, $(if ($dumped) { " and $dump" } else { ' (no dump: see the text)' }))
	Remove-Old 'hang-*.dmp' $KeepDumps
	Remove-Old 'hang-*.txt'
	Remove-Old 'hang-*.err'
}

function Get-Game {
	$found = if ($ProcessId) { Get-Process -Id $ProcessId -ErrorAction SilentlyContinue } else { Get-Process -Name $ProcessName -ErrorAction SilentlyContinue }
	$found | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
}

if ($Now) {
	$game = Get-Game
	if (-not $game) { throw "$ProcessName is not running (or has no window)" }
	Invoke-Catch $game 'asked for (-Now)'
	return
}

Write-Line "watching $ProcessName (not answering for $Seconds s is a hang); catches go to $OutDir"
$deadline = if ($Minutes -gt 0) { (Get-Date).AddMinutes($Minutes) } else { [DateTime]::MaxValue }
$hungSince = $null
$caught = $false
$lastPid = 0
$catches = 0
while ((Get-Date) -lt $deadline) {
	$game = Get-Game
	if (-not $game) {
		if ($lastPid) { Write-Line "the game is gone (pid $lastPid)" }
		$lastPid = 0; $hungSince = $null; $caught = $false
		Start-Sleep -Seconds 2
		continue
	}
	if ($game.Id -ne $lastPid) {
		Write-Line "$($game.ProcessName) is running (pid $($game.Id))"
		$lastPid = $game.Id; $hungSince = $null; $caught = $false
	}
	if ([F4HangCatch]::IsHungAppWindow($game.MainWindowHandle)) {
		if (-not $hungSince) {
			# Windows calls a window hung only after it hasn't read its messages for five seconds.
			$hungSince = (Get-Date).AddSeconds(-5)
			Write-Line 'not responding'
		}
		$stalled = ((Get-Date) - $hungSince).TotalSeconds
		if (-not $caught -and $stalled -ge $Seconds) {
			Invoke-Catch $game ("not responding for {0:N0} s" -f $stalled)
			$caught = $true
			$catches++
		}
	} elseif ($hungSince) {
		Write-Line ("answers again after {0:N0} s" -f ((Get-Date) - $hungSince).TotalSeconds)
		$hungSince = $null; $caught = $false
	}
	Start-Sleep -Seconds 1
}
Write-Line "done: $catches catch(es) in $OutDir"
