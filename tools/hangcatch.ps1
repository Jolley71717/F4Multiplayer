# Catches a frozen Fallout 4 in the act. Once a second it asks Windows whether the game's window answers
# (IsHungAppWindow: what makes Windows call a window Not Responding). When it hasn't for -Seconds in a
# row, the console debugger attaches without taking the game over (cdb -pv: it stops the threads, reads
# them and lets them go), writes a minidump with every thread (.dump /mt) and every thread's stack (~*kn)
# into -OutDir, and detaches (qd). One catch per freeze: the next comes only after the game answered again.
# The plugin's own hang watch (iHangSeconds) logs where the main thread is; this one sees every thread,
# from outside, so it works even when the plugin's thread is stuck with the rest.
#
#   tools\hangcatch.ps1 [-OutDir <folder>] [-Seconds 10] [-Minutes 0] [-Cdb <path>] [-Keep 20]
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
	[switch]$Now
)

$ErrorActionPreference = 'Stop'

if ($Seconds -lt 1) { throw '-Seconds must be at least 1' }
if ($Keep -lt 1) { throw '-Keep must be at least 1' }
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
function Remove-Old([string]$pattern) {
	Get-ChildItem -Path $OutDir -Filter $pattern -File | Sort-Object LastWriteTime -Descending | Select-Object -Skip $Keep | Remove-Item -Force -ErrorAction SilentlyContinue
}

# Attaches, writes the dump and the stacks, detaches. The debugger gets two minutes: a stuck debugger is
# killed. With -pv it suspended every thread of the game and only its detach (qd) resumes them; a thread's
# suspension outlives the process that asked for it, so after a kill the game is resumed here, once.
function Invoke-Catch([System.Diagnostics.Process]$game, [string]$why) {
	$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
	$dump = Join-Path $OutDir "hang-$stamp.dmp"
	$text = Join-Path $OutDir "hang-$stamp.txt"
	$errors = Join-Path $OutDir "hang-$stamp.err"
	# The commands go in a file (-cf): the dump's path may have spaces, which a -c line can't quote. The dump
	# first, so a catch always has it. Symbols only from a folder of our own (-y): the debugger's default is
	# Microsoft's symbol server, and downloading every module's took minutes with the game stopped. The
	# stacks name functions by the modules' exports (ntdll, kernel32) and the rest as module+offset, which is
	# what the plugin's crash and hang reports give too.
	$script = Join-Path $OutDir "hang-$stamp.cdb"
	$symbols = Join-Path $OutDir 'symbols'
	New-Item -ItemType Directory -Force $symbols | Out-Null
	Set-Content -Path $script -Encoding ascii -Value @(
		".echo ---- $why",
		'.time',
		".dump /mt `"$dump`"",
		'.kframes 0x80',
		'~*kn',
		'qd')
	Write-Line "catching pid $($game.Id): $why"
	$cdbProcess = Start-Process -FilePath $Cdb -ArgumentList @('-pv', '-p', $game.Id, '-y', "`"$symbols`"", '-cf', "`"$script`"") -NoNewWindow -PassThru `
		-RedirectStandardOutput $text -RedirectStandardError $errors
	if (-not $cdbProcess.WaitForExit(120000)) {
		Write-Line 'the debugger took over two minutes: stopped'
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
	Remove-Old 'hang-*.dmp'
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
