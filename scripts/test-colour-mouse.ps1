# Changes the house colour in the in-game settings by mouse clicks (injected
# events), then screenshots the game, to look for corrupted menubar buttons.
# Usage: scripts\test-colour-mouse.ps1 [-NoGlPost]
param([switch]$NoGlPost)

$exe = Join-Path $PSScriptRoot "..\dist\dunedynasty.exe"
$data = Join-Path $env:APPDATA "Dune Dynasty"
$start = Get-Date

$args = @("--windowed", "--house", "atreides", "--mission", "1", "--test-colour-mouse")
if ($NoGlPost) { $args += "--no-gl-post" }

$p = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory (Split-Path $exe) -PassThru
if (-not $p.WaitForExit(180000)) { $p.Kill(); Write-Host "TIMEOUT" }

Get-Content (Join-Path $data "output.log") | Select-String "test:"
Write-Host "Screenshots:"
Get-ChildItem (Join-Path $data "screenshot_*.png") |
    Where-Object { $_.LastWriteTime -ge $start } |
    Sort-Object LastWriteTime | ForEach-Object { "  " + $_.FullName }
