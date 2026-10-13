# Long soak with a crowd of fake players (F4MPBot) on this machine, and a server restart halfway.
#
#   tools\botsoak.ps1 [-Minutes 3] [-Port N] [-Movers 2] [-RoundSeconds 40] [-ChurnSeconds 12]
#                     [-WarmupSeconds 30] [-MaxGrowthMB 32] [-NoRestart] [-AllowHardStop] [-KeepLogs]
#                     [-Mode releasedbg]
#
# Starts F4MPServer.exe on 127.0.0.1:Port (this machine only; a free port unless -Port is given, so
# soaks can run side by side) with a session file in a temp folder,
# then keeps these bots playing for -Minutes, each for about -RoundSeconds before a fresh one with the
# same identity takes its place:
#   Downed   status says it is down (it joins first, so it is player 1)
#   Reviver  helps player 1 up
#   MoverN   walk and jump in circles
#   Shooter  fires a weapon and sets the time of day
#   Looter   takes from a container, picks up, opens a door, finds a map marker, pings
#   Owner    runs an NPC companion and has it talk
#   Quester  sets a quest stage, completes the quest, gains XP
#   Voice    talks (Steam voice packets of silence)
#   Churn    is killed every -ChurnSeconds and restarted with the same identity (a crash)
# Every process it starts ends with it, however it ends (they are in a job that closes with it).
# Halfway, the server is stopped with Ctrl+C (it saves the session) and started again from the same
# session file. At the end it is stopped the same way.
#
# Checks: the server never exits unless asked; asked with Ctrl+C, it stops within 10 s (killing it is
# a failure unless -AllowHardStop), exits 0, logs stopping and saves its session file; no error, assert
# or failure in its log; it loads the saved session after the restart; every bot run exits as expected (0 at the end
# of its round, 4 when the server stops under it; killed churn runs aside); each bot keeps its player ID
# across rounds and the restart; the server's working set grows by at most -MaxGrowthMB after
# -WarmupSeconds. Prints a one-line verdict and exits 1 on failure. Logs go to a temp folder (kept on
# failure or with -KeepLogs). Nothing opens a window.
param(
	[double]$Minutes = 3,
	[int]$Port = 0,
	[int]$Movers = 2,
	[int]$RoundSeconds = 40,
	[int]$ChurnSeconds = 12,
	[int]$WarmupSeconds = 30,
	[int]$MaxGrowthMB = 32,
	[switch]$NoRestart,
	[switch]$AllowHardStop,
	[switch]$KeepLogs,
	[string]$Mode = 'releasedbg'
)

$ErrorActionPreference = 'Stop'

if ($Minutes -lt 1) { throw '-Minutes must be at least 1' }
if ($Port -eq 7779) { throw 'Port 7779 is for real games; pick another' }
# Leftovers of an interrupted soak are only looked for on a port that was asked for.
$cleanPort = $Port -ne 0
if ($Port -eq 0) {
	$probe = New-Object Net.Sockets.UdpClient (New-Object Net.IPEndPoint ([Net.IPAddress]::Loopback, 0))
	$Port = $probe.Client.LocalEndPoint.Port
	$probe.Close()
}
if ($RoundSeconds -lt 10 -or $ChurnSeconds -lt 5) { throw '-RoundSeconds must be at least 10 and -ChurnSeconds at least 5' }

$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\windows\x64\$Mode"
$serverExe = Join-Path $bin 'F4MPServer.exe'
$botExe = Join-Path $bin 'F4MPBot.exe'
foreach ($exe in $serverExe, $botExe) {
	if (-not (Test-Path $exe)) { throw "$exe not found (build it first)" }
}

$logDir = Join-Path ([IO.Path]::GetTempPath()) ("f4mp-botsoak-{0}-{1}" -f $Port, (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $logDir | Out-Null
$sessionFile = Join-Path $logDir 'soak.session'

# Starts a process with no window, its output (and errors) going to a log file as it comes. Each is put
# in a job that is closed (and its processes killed) when this PowerShell ends, even if it is killed
# before the finally below runs.
if (-not ('BotSoakChild' -as [type])) {
	Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

public class BotSoakChild
{
	public Process Process;
	StreamWriter writer;
	readonly object gate = new object();

	[StructLayout(LayoutKind.Sequential)]
	struct BasicLimits
	{
		public long PerProcessUserTimeLimit;
		public long PerJobUserTimeLimit;
		public uint LimitFlags;
		public UIntPtr MinimumWorkingSetSize;
		public UIntPtr MaximumWorkingSetSize;
		public uint ActiveProcessLimit;
		public UIntPtr Affinity;
		public uint PriorityClass;
		public uint SchedulingClass;
	}

	[StructLayout(LayoutKind.Sequential)]
	struct ExtendedLimits
	{
		public BasicLimits Basic;
		public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
		public UIntPtr ProcessMemoryLimit;
		public UIntPtr JobMemoryLimit;
		public UIntPtr PeakProcessMemoryUsed;
		public UIntPtr PeakJobMemoryUsed;
	}

	[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateJobObject(IntPtr attributes, string name);
	[DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimits info, uint length);
	[DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
	[DllImport("kernel32.dll")] static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);

	static BotSoakChild()
	{
		// A process started ignoring Ctrl+C (as some hosts start PowerShell) passes that on to what it
		// starts, and the server would never hear the Ctrl+C that asks it to stop and save.
		SetConsoleCtrlHandler(IntPtr.Zero, false);
	}

	// Why a process could not be put in the job (so it could outlive an interrupted soak), or null.
	public static string JobProblem;
	// Never closed: the handle goes when this process does, and the job kills what is in it.
	static readonly IntPtr job = CreateKillOnCloseJob();

	static IntPtr CreateKillOnCloseJob()
	{
		var handle = CreateJobObject(IntPtr.Zero, null);
		if (handle == IntPtr.Zero) {
			JobProblem = "could not create a job (error " + Marshal.GetLastWin32Error() + ")";
			return handle;
		}
		var limits = new ExtendedLimits();
		limits.Basic.LimitFlags = 0x2000;  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
		if (!SetInformationJobObject(handle, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits)))) {  // JobObjectExtendedLimitInformation
			JobProblem = "could not set up the job (error " + Marshal.GetLastWin32Error() + ")";
		}
		return handle;
	}

	public static BotSoakChild Start(string exe, string arguments, string log)
	{
		var run = new BotSoakChild();
		run.writer = new StreamWriter(new FileStream(log, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete));
		run.writer.AutoFlush = true;
		var info = new ProcessStartInfo(exe, arguments);
		info.UseShellExecute = false;
		info.CreateNoWindow = true;
		info.RedirectStandardOutput = true;
		info.RedirectStandardError = true;
		run.Process = new Process();
		run.Process.StartInfo = info;
		run.Process.OutputDataReceived += (s, e) => run.Write(e.Data, "");
		run.Process.ErrorDataReceived += (s, e) => run.Write(e.Data, "stderr: ");
		run.Process.Start();
		if (job != IntPtr.Zero && !AssignProcessToJobObject(job, run.Process.Handle) && JobProblem == null) {
			JobProblem = "could not put a process in the job (error " + Marshal.GetLastWin32Error() + ")";
		}
		run.Process.BeginOutputReadLine();
		run.Process.BeginErrorReadLine();
		return run;
	}

	void Write(string line, string prefix)
	{
		if (line == null) return;
		lock (gate) { if (writer != null) writer.WriteLine(prefix + line); }
	}

	// Once the process has exited: waits for the rest of its output, then closes the log.
	public void Close()
	{
		Process.WaitForExit();
		lock (gate) { if (writer != null) { writer.Dispose(); writer = null; } }
	}
}
'@
}

# Sends Ctrl+C to a process started by BotSoakChild (it has a console of its own, with no window), from
# a helper that attaches to that console, so this script's own console is left alone.
function Send-CtrlC([int]$processId) {
	$script = @"
`$ProgressPreference = 'SilentlyContinue'
Add-Type -Name Console -Namespace BotSoak -MemberDefinition '
[DllImport("kernel32.dll")] public static extern bool FreeConsole();
[DllImport("kernel32.dll")] public static extern bool AttachConsole(uint id);
[DllImport("kernel32.dll")] public static extern bool SetConsoleCtrlHandler(System.IntPtr handler, bool add);
[DllImport("kernel32.dll")] public static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint group);'
[void][BotSoak.Console]::FreeConsole()
if (-not [BotSoak.Console]::AttachConsole($processId)) { exit 3 }
[void][BotSoak.Console]::SetConsoleCtrlHandler([IntPtr]::Zero, `$true)
if (-not [BotSoak.Console]::GenerateConsoleCtrlEvent(0, 0)) { exit 4 }
exit 0
"@
	$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
	$helper = [BotSoakChild]::Start('powershell.exe', "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded", (Join-Path $logDir 'ctrl-c.log'))
	if (-not $helper.Process.WaitForExit(30000)) {
		$helper.Process.Kill()
	}
	$helper.Close()
	return $helper.Process.ExitCode
}

function Read-Text([string]$path) {
	try {
		$fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
		$reader = New-Object IO.StreamReader($fs)
		$text = $reader.ReadToEnd()
		$reader.Close()
		return $text
	} catch {
		return ''
	}
}

function Stop-Leftovers {
	# Anything still running from an earlier, interrupted soak on this port (one given with -Port).
	if (-not $cleanPort) { return }
	$filter = "Name = 'F4MPServer.exe' OR Name = 'F4MPBot.exe'"
	foreach ($p in @(Get-CimInstance Win32_Process -Filter $filter)) {
		if ($p.CommandLine -match "(--port $Port\b|127\.0\.0\.1:$Port\b)") {
			Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
		}
	}
}

$problems = New-Object System.Collections.ArrayList
function Problem([string]$what) {
	[void]$problems.Add($what)
	Write-Host "  FAIL  $what" -ForegroundColor Red
}

# ---- the server ---------------------------------------------------------------------------------------

$servers = New-Object System.Collections.ArrayList  # one per start: run, log, samples, stop
$server = $null

function Start-Server {
	$n = $servers.Count + 1
	$log = Join-Path $logDir "server-$n.log"
	$arguments = "--port $Port --max-players 16 --loopback-only --session-file `"$sessionFile`""
	$run = [BotSoakChild]::Start($serverExe, $arguments, $log)
	$entry = [pscustomobject]@{ N = $n; Run = $run; Log = $log; Started = Get-Date; Samples = New-Object System.Collections.ArrayList; Asked = $false; AskedAt = $null; CtrlC = $null; Hard = $false; SavedAt = $null }
	[void]$servers.Add($entry)
	$deadline = (Get-Date).AddSeconds(15)
	while ((Get-Date) -lt $deadline -and -not $run.Process.HasExited) {
		if ((Read-Text $log) -match 'listening on UDP port') { return $entry }
		Start-Sleep -Milliseconds 100
	}
	throw "server run $n did not start listening (see $log)"
}

function Stop-Server($entry) {
	$entry.Asked = $true
	$entry.AskedAt = (Get-Date).ToUniversalTime()
	$entry.CtrlC = Send-CtrlC $entry.Run.Process.Id
	if (-not $entry.Run.Process.WaitForExit(10000)) {
		Write-Host "  server run $($entry.N) ignored Ctrl+C (helper exit $($entry.CtrlC)); killing it" -ForegroundColor Yellow
		$entry.Hard = $true
		$entry.Run.Process.Kill()
		[void]$entry.Run.Process.WaitForExit(5000)
	}
	$entry.Run.Close()
	if (Test-Path $sessionFile) { $entry.SavedAt = (Get-Item $sessionFile).LastWriteTimeUtc }
}

function Sample-Server($entry) {
	try {
		$entry.Run.Process.Refresh()
		[void]$entry.Samples.Add([pscustomobject]@{ At = ((Get-Date) - $entry.Started).TotalSeconds; Bytes = $entry.Run.Process.WorkingSet64 })
	} catch {
	}
}

# ---- the bots -----------------------------------------------------------------------------------------

$contentHash = '2C42CB18'
$npc = '0001D162'
$roles = New-Object System.Collections.ArrayList
function Role([string]$name, [string[]]$extra, [int]$seconds) {
	$identity = 'B0750000000000{0:X2}' -f ($roles.Count + 1)
	[void]$roles.Add([pscustomobject]@{ Name = $name; Extra = $extra; Seconds = $seconds; Identity = $identity; Current = $null; Runs = 0 })
}
Role 'Downed' @('--downed', '1', '--status-health', '0', '--radius', '50') $RoundSeconds
Role 'Reviver' @('--revive', '1', '--radius', '120') ($RoundSeconds + 3)
for ($i = 1; $i -le $Movers; $i++) {
	Role "Mover$i" @('--radius', "$(200 + 100 * $i)", '--speed', '300', '--jump', '1') ($RoundSeconds + 3 * $i)
}
Role 'Shooter' @('--shoot', '0000463F', '--time', '14', '--time-step', '1', '--weather', '0001E0F2', '--radius', '200') ($RoundSeconds + 5)
Role 'Looter' @('--loot', '000AE12F:0000000F:-1', '--pickup', '0001F00D', '--door', '0001E0AA:1:0', '--marker', '0001ABCD', '--ping', '1', '--speed', '370') ($RoundSeconds + 7)
Role 'Owner' @('--own', $npc, '--say', $npc, '--radius', '400') ($RoundSeconds + 11)
Role 'Quester' @('--quest', '000229A5:20', '--quest-done', '000229A5', '--xp', '10') ($RoundSeconds + 13)
Role 'Voice' @('--voice-silence', '1') ($RoundSeconds + 17)
# Killed after -ChurnSeconds; if this script is gone by then, it still ends by itself a little later.
Role 'Churn' @('--radius', '250') ($ChurnSeconds + 20)

$runs = New-Object System.Collections.ArrayList  # every bot run: role, process, how it ended

function Start-Bot($role) {
	$role.Runs++
	$log = Join-Path $logDir ("{0}-{1}.log" -f $role.Name, $role.Runs)
	$arguments = @('--server', "127.0.0.1:$Port", '--name', $role.Name, '--identity', $role.Identity, '--content-hash', $contentHash,
		'--seconds', "$($role.Seconds)", '--worldspace', '0000003C', '--x', '-80352', '--y', '89600', '--z', '7790') + $role.Extra
	$run = [BotSoakChild]::Start($botExe, ($arguments -join ' '), $log)
	$entry = [pscustomobject]@{ Role = $role; N = $role.Runs; Run = $run; Log = $log; Started = Get-Date; Killed = $false; ServerStopped = $false; Code = $null }
	[void]$runs.Add($entry)
	$role.Current = $entry
}

function Finish-Bot($entry) {
	$entry.Run.Close()
	$entry.Code = $entry.Run.Process.ExitCode
	$entry.Role.Current = $null
}

function Kill-Bot($entry) {
	$entry.Killed = $true
	if (-not $entry.Run.Process.HasExited) { $entry.Run.Process.Kill() }
	[void]$entry.Run.Process.WaitForExit(5000)
	Finish-Bot $entry
}

# The server is stopping: every bot still in it should be told and leave (exit 4).
function Stop-ServerUnderBots($entry) {
	foreach ($role in $roles) {
		if ($role.Current) { $role.Current.ServerStopped = $true }
	}
	Stop-Server $entry
	$deadline = (Get-Date).AddSeconds(15)
	foreach ($role in $roles) {
		$bot = $role.Current
		if (-not $bot) { continue }
		$left = [Math]::Max(0, [int]($deadline - (Get-Date)).TotalMilliseconds)
		if (-not $bot.Run.Process.WaitForExit($left)) {
			Problem "$($role.Name) run $($role.Runs) still running 15 s after the server stopped"
			Kill-Bot $bot
		} else {
			Finish-Bot $bot
		}
	}
}

# ---- the soak -----------------------------------------------------------------------------------------

$started = Get-Date
$deadline = $started.AddMinutes($Minutes)
$restartAt = $started.AddMinutes($Minutes / 2)
$restarted = [bool]$NoRestart
$crashed = $false
Write-Host "Bot soak: port $Port, $Minutes min, $($roles.Count) bots, restart halfway: $(-not $NoRestart), logs: $logDir"

$aborted = $null
try {
	Stop-Leftovers
	if ([BotSoakChild]::JobProblem) { Write-Host "  warning: $([BotSoakChild]::JobProblem); a killed soak may leave processes behind" -ForegroundColor Yellow }
	$server = Start-Server
	# Downed first, so it is player 1 (the Reviver helps player 1 up).
	Start-Bot $roles[0]
	$joinDeadline = (Get-Date).AddSeconds(10)
	while ((Get-Date) -lt $joinDeadline -and -not ((Read-Text $server.Log) -match "'Downed' joined as player 1 ")) { Start-Sleep -Milliseconds 100 }
	if (-not ((Read-Text $server.Log) -match "'Downed' joined as player 1 ")) { throw 'Downed did not join as player 1' }

	$nextSample = Get-Date
	while ((Get-Date) -lt $deadline) {
		if ($server.Run.Process.HasExited) {
			$crashed = $true
			Problem "server run $($server.N) exited by itself with code $($server.Run.Process.ExitCode)"
			break
		}
		foreach ($role in $roles) {
			$bot = $role.Current
			if ($bot -and $bot.Run.Process.HasExited) {
				Finish-Bot $bot
				$bot = $null
			}
			if ($bot -and $role.Name -eq 'Churn' -and ((Get-Date) - $bot.Started).TotalSeconds -ge $ChurnSeconds) {
				Kill-Bot $bot  # a crash: the server still holds its connection when it comes back
				$bot = $null
			}
			if (-not $bot) { Start-Bot $role }
		}
		if ((Get-Date) -ge $nextSample) {
			Sample-Server $server
			$nextSample = (Get-Date).AddSeconds(2)
		}
		if (-not $restarted -and (Get-Date) -ge $restartAt) {
			$restarted = $true
			Write-Host "Restarting the server from its session file after $([int]((Get-Date) - $started).TotalSeconds) s"
			Stop-ServerUnderBots $server
			$server = Start-Server
		}
		Start-Sleep -Milliseconds 250
	}
	if (-not $crashed) {
		Write-Host 'Stopping the server'
		Stop-ServerUnderBots $server
	}
} catch {
	$aborted = $_.Exception.Message
	Problem "aborted: $aborted"
} finally {
	foreach ($role in $roles) {
		if ($role.Current) { Kill-Bot $role.Current }
	}
	foreach ($s in $servers) {
		if (-not $s.Run.Process.HasExited) { $s.Run.Process.Kill(); [void]$s.Run.Process.WaitForExit(5000) }
		$s.Run.Close()
	}
	Stop-Leftovers
}

# ---- checks -------------------------------------------------------------------------------------------

$growths = @()
$hard = 0
foreach ($s in $servers) {
	$log = Read-Text $s.Log
	if ($s.Hard) {
		$hard++
		# A server that will not stop loses what changed since its last save (every 15 s).
		if (-not $AllowHardStop) { Problem "server run $($s.N) ignored Ctrl+C for 10 s (helper exit $($s.CtrlC)) and was killed" }
	}
	if ($s.Asked -and -not $s.Hard) {
		if ($s.Run.Process.ExitCode -ne 0) { Problem "server run $($s.N) exited with code $($s.Run.Process.ExitCode) when stopped" }
		if (-not ($log -match 'server: stopped')) { Problem "server run $($s.N) did not log stopping" }
		if (-not $s.SavedAt -or $s.SavedAt -lt $s.AskedAt) { Problem "server run $($s.N) did not save its session file when stopped" }
	}
	foreach ($line in ($log -split "`r?`n")) {
		if ($line -match '(?i)\b(error|assert\w*|failed|could not|exception)\b') { Problem "server run $($s.N) logged: $line" }
	}
	if ($s.N -gt 1 -and -not ($log -match 'server: loaded the saved session \(')) { Problem "server run $($s.N) did not load the saved session" }
	$warm = @($s.Samples | Where-Object { $_.At -ge $WarmupSeconds })
	if ($warm.Count -ge 2) {
		$base = $warm[0].Bytes
		$peak = ($warm | Measure-Object -Property Bytes -Maximum).Maximum
		$growth = ($peak - $base) / 1MB
		$growths += ('{0:+0.0;-0.0} MB (of {1:0.0} MB)' -f $growth, ($base / 1MB))
		if ($growth -gt $MaxGrowthMB) { Problem ("server run {0}: working set grew {1:0.0} MB after warm-up (from {2:0.0} MB; limit {3} MB)" -f $s.N, $growth, ($base / 1MB), $MaxGrowthMB) }
	}
}
if (-not $NoRestart -and -not $aborted -and $servers.Count -lt 2) { Problem 'the server was never restarted' }

$ok = 0; $killed = 0; $dropped = 0
$ids = @{}
foreach ($bot in $runs) {
	$name = "$($bot.Role.Name) run $($bot.N)"
	$text = Read-Text $bot.Log
	$welcome = [regex]::Match($text, '(?m)^welcomed as player (\d+) ')
	if ($welcome.Success) {
		$id = [int]$welcome.Groups[1].Value
		if (-not $ids.ContainsKey($bot.Role.Name)) { $ids[$bot.Role.Name] = $id }
		elseif ($ids[$bot.Role.Name] -ne $id) { Problem "$name was welcomed as player $id, not $($ids[$bot.Role.Name]) as before" }
	}
	if ($bot.Killed) { $killed++; continue }
	if ($bot.ServerStopped) {
		if ($bot.Code -eq 4 -or $bot.Code -eq 0) { $dropped++ } else { Problem "$name exited with $($bot.Code) when the server stopped (want 4)" }
		continue
	}
	if ($bot.Code -eq 0 -and $welcome.Success) { $ok++ } else { Problem "$name exited with $($bot.Code) (want 0, welcomed)" }
}
if ($ids['Downed'] -ne 1) { Problem "Downed was player $($ids['Downed']), not 1" }
$all = ''
foreach ($s in $servers) { $all += Read-Text $s.Log }
$churnBack = ([regex]::Matches($all, "'Churn' came back as player")).Count
$churnRuns = @($runs | Where-Object { $_.Role.Name -eq 'Churn' }).Count
if ($churnRuns -gt 2 -and $churnBack -lt 1) { Problem 'the Churn bot never came back as itself' }

$elapsed = ((Get-Date) - $started).TotalMinutes
$restartWord = 'no restart'
if ($servers.Count -gt 1) { $restartWord = "$($servers.Count - 1) restart" + $(if ($hard) { " ($hard hard)" } else { '' }) }
$summary = '{0:0.0} min, {1} server runs ({2}), {3} bot runs: {4} ok, {5} left when the server stopped, {6} killed (churn {7}, came back {8}x); {9} problems; working set after warm-up {10}' -f `
	$elapsed, $servers.Count, $restartWord, $runs.Count, $ok, $dropped, $killed, $churnRuns, $churnBack, $problems.Count, ($growths -join ', ')
Write-Host ''
if ($problems.Count -gt 0) {
	Write-Host "BOTSOAK FAIL: $summary. Logs: $logDir" -ForegroundColor Red
	exit 1
}
Write-Host "BOTSOAK PASS: $summary." -ForegroundColor Green
if ($KeepLogs) {
	Write-Host "Logs: $logDir"
} else {
	Remove-Item -Recurse -Force $logDir
}
exit 0
