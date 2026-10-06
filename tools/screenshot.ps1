# Captures the primary monitor to a PNG (downscaled) for quick visual checks.
param(
	[Parameter(Mandatory = $true)]
	[string]$OutFile,
	[int]$Width = 1280
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$full = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($full)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)

$height = [int]($bounds.Height * $Width / $bounds.Width)
$scaled = New-Object System.Drawing.Bitmap $full, $Width, $height
$scaled.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)

$graphics.Dispose()
$full.Dispose()
$scaled.Dispose()
$OutFile
