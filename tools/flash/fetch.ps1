# The ActionScript 3 compiler for the plugin's own menu movies, on Windows: what fetch.sh does (docs/UI.md). Apache
# Flex SDK's mxmlc and compc, prebuilt, and Adobe's playerglobal.swc (the flash.* API), both pinned by SHA-256 and
# unpacked outside the repo, into %USERPROFILE%\.flex\4.16.1; neither is committed (playerglobal.swc is Adobe's).
# Again only if a file is missing. Prints FLEX_HOME=<folder>. The compiler needs Java.
#
#   tools\flash\fetch.ps1
#   tools\flash\fetch.ps1 -Root D:\flex
param(
	[string]$Root = (Join-Path $env:USERPROFILE '.flex')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest's progress bar slows the download many times over

# The same pins as fetch.sh: change both together.
$flexVersion = '4.16.1'
$flexUrl = "https://archive.apache.org/dist/flex/$flexVersion/binaries/apache-flex-sdk-$flexVersion-bin.tar.gz"
$flexSha256 = '17fda7ac8d3e476cad3127f345455ef316acfb87c6f4322e5897bd8d9b09388e'
$playerglobalVersion = '32.0'
$playerglobalUrl = 'https://fpdownload.macromedia.com/get/flashplayer/updaters/32/playerglobal32_0.swc'
$playerglobalSha256 = '7d4d6168d27603cfb3b750302448e354e0bbc1bdd58f5d101c3dcf6891e9bb65'

$home_ = Join-Path $Root $flexVersion
$playerglobal = Join-Path $home_ "frameworks\libs\player\$playerglobalVersion\playerglobal.swc"
if ((Test-Path (Join-Path $home_ 'lib\mxmlc.jar')) -and (Test-Path (Join-Path $home_ 'lib\compc.jar')) -and (Test-Path $playerglobal)) {
	"FLEX_HOME=$home_"
	return
}

function Get-Pinned([string]$Url, [string]$Sha256, [string]$File) {
	Invoke-WebRequest -Uri $Url -OutFile $File -UseBasicParsing
	$got = (Get-FileHash -Algorithm SHA256 -LiteralPath $File).Hash.ToLowerInvariant()
	if ($got -ne $Sha256) { throw "checksum mismatch for ${Url}: expected $Sha256, got $got" }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("f4mp-flex-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
try {
	Get-Pinned $flexUrl $flexSha256 (Join-Path $work 'flex.tar.gz')
	Get-Pinned $playerglobalUrl $playerglobalSha256 (Join-Path $work 'playerglobal.swc')
	$sdk = Join-Path $work 'sdk'
	New-Item -ItemType Directory -Force $sdk | Out-Null
	# Windows 10 and later have bsdtar as tar.exe, which reads .tar.gz.
	& tar.exe -xzf (Join-Path $work 'flex.tar.gz') -C $sdk --strip-components 1
	if ($LASTEXITCODE -ne 0) { throw "tar could not unpack the Flex SDK (exit $LASTEXITCODE)" }
	$player = Join-Path $sdk "frameworks\libs\player\$playerglobalVersion"
	New-Item -ItemType Directory -Force $player | Out-Null
	Copy-Item (Join-Path $work 'playerglobal.swc') (Join-Path $player 'playerglobal.swc')
	New-Item -ItemType Directory -Force $Root | Out-Null
	if (Test-Path $home_) { Remove-Item -Recurse -Force $home_ }
	Move-Item $sdk $home_
} finally {
	Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
"FLEX_HOME=$home_"
