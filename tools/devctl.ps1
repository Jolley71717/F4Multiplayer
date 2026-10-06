# Sends commands to the F4Multiplayer developer control channel and prints the replies.
# Requires bDevChannel = true in Data/F4SE/Plugins/F4Multiplayer.ini and a running game.
#
#   .\tools\devctl.ps1 status
#   .\tools\devctl.ps1 pos
#   .\tools\devctl.ps1 "console coc SanctuaryExt"
#   .\tools\devctl.ps1 status pos        # several commands in one session
#
# Set $env:F4MP_DEV_PORT to use a port other than 7790.

$ErrorActionPreference = 'Stop'

$Commands = $args
if ($Commands.Count -eq 0) {
	throw 'Usage: devctl.ps1 <command> [<command> ...]'
}
$Port = if ($env:F4MP_DEV_PORT) { [int]$env:F4MP_DEV_PORT } else { 7790 }

$tokenPath = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Fallout4\F4SE\F4Multiplayer_dev.token'
if (-not (Test-Path $tokenPath)) {
	throw "Token not found at $tokenPath. Is the game running with bDevChannel = true?"
}
$token = (Get-Content $tokenPath -Raw).Trim()

$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port)
try {
	$stream = $client.GetStream()
	$stream.ReadTimeout = 10000
	$writer = New-Object System.IO.StreamWriter($stream)
	$writer.NewLine = "`n"
	$writer.AutoFlush = $true
	$reader = New-Object System.IO.StreamReader($stream)

	$writer.WriteLine("auth $token")
	$reply = $reader.ReadLine()
	if ($reply -ne 'ok') {
		throw "Authentication failed: $reply"
	}

	foreach ($command in $Commands) {
		$writer.WriteLine($command)
		"> $command"
		$reader.ReadLine()
	}
}
finally {
	$client.Close()
}
