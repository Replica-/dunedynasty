<#
.SYNOPSIS
  Regression tests for the fork's changes.  Each one launches the game with a
  --test-* switch, which plays the scenario by itself and quits.

.DESCRIPTION
  A test passes when the game exits with code 0 inside its time limit.  A test
  that crashes, exits with an error, or does not finish in time (a hang) fails,
  and the script goes on to the next one.  Screenshot tests also check that a
  screenshot was made, and that the right-hand side of it is not black bars.
  Screenshots and a summary are kept in %TEMP%\dd-regression for a look over.

  .\scripts\regression-tests.ps1                  everything but the slow tests
  .\scripts\regression-tests.ps1 -Slow            ... and the slow ones too
  .\scripts\regression-tests.ps1 -Only mentat     tests whose name matches
  .\scripts\regression-tests.ps1 -Build           build first
#>
param(
	[string]$Only = "",
	[switch]$Slow,
	[switch]$Build,
	[string]$Exe = "$PSScriptRoot\..\dist\dunedynasty.exe",
	[string]$Results = "$env:TEMP\dd-regression"
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$data = "$env:APPDATA\Dune Dynasty"
$mission = @("--house", "atreides", "--mission", "5")

# name, what it covers, arguments, time limit (seconds), kind.
#   kind "shot":  must leave a screenshot, with the picture filling the window.
#   kind "":      the exit code is all there is.
$tests = @(
	@{ n = "colour-menu";      t = 30;  k = "shot"; a = $mission + "--test-colour-menu";
	   d = "Game Controls menu with the house colour picker" },
	@{ n = "options-previous"; t = 30;  k = "";     a = $mission + "--test-options-previous";
	   d = "yellow preset, then Previous (hung on a stale Locate button)" },
	@{ n = "mentat";           t = 30;  k = "shot"; a = $mission + "--test-mentat";
	   d = "in-game Mentat: left-aligned, TV frame completed, wall on the right" },
	@{ n = "confirm-house";    t = 30;  k = "shot"; a = @("--test-confirm-house");
	   d = "Bene Gesserit house screen: top/bottom strips, bricks on the right" },
	@{ n = "panel";            t = 90;  k = "shot"; a = $mission + "--test-panel";
	   d = "Locate line on an enemy unit, action panel stats" },
	@{ n = "deathhand";        t = 120; k = "";     a = $mission + "--test-deathhand";
	   d = "Death Hand missile: scatter and damage near the target" },
	@{ n = "deathhand-cy";     t = 120; k = "";     a = $mission + "--test-deathhand-cy";
	   d = "Death Hand missile on the construction yard" },
	@{ n = "worm";             t = 180; k = "";     a = $mission + "--test-worm";
	   d = "worm behaviours next to stationary and moving tanks" },
	@{ n = "worm-harvesters";  t = 240; k = "";     a = $mission + "--test-worm-harvesters";
	   d = "the game's own worm logic against harvesters" },
	@{ n = "worm-respawn";     t = 240; k = "";     a = $mission + "--test-worm-respawn";
	   d = "worms coming back after being killed" },
	@{ n = "spice";            t = 600; k = "";     a = $mission + "--test-spice"; slow = $true;
	   d = "harvester spice choices over four minutes of play" }
)

if ($Build) {
	Write-Host "Building..." -ForegroundColor Cyan
	& cmd /c "D:\development\msys64\msys2_shell.cmd -ucrt64 -defterm -no-start -here -c ""cd /d/development/dunedynasty/ && bash /d/development/dunedynasty/scripts/build-debug-windows-x64.sh""" | Out-Null
}

if (-not (Test-Path $Exe)) { throw "No game at ${Exe}: build it first (-Build)." }
New-Item -ItemType Directory -Force $Results | Out-Null
Add-Type -AssemblyName System.Drawing

# The picture should fill the window: a black band down the right hand side is
# what the widescreen fixes are there to stop.
function Test-RightSideFilled($path) {
	$img = [System.Drawing.Bitmap]::FromFile($path)
	try {
		$x0 = [int]($img.Width * 0.92)
		$sum = 0; $n = 0
		for ($x = $x0; $x -lt $img.Width; $x += 6) {
			for ($y = [int]($img.Height * 0.15); $y -lt $img.Height; $y += 6) {
				$c = $img.GetPixel($x, $y)
				$sum += $c.R + $c.G + $c.B; $n++
			}
		}
		return ($sum / $n / 3) -gt 4
	} finally { $img.Dispose() }
}

$summary = @()

foreach ($test in $tests) {
	if ($Only -ne "" -and $test.n -notlike "*$Only*") { continue }
	if ($test.slow -and -not $Slow -and $Only -eq "") { continue }

	Get-Process dunedynasty -ErrorAction SilentlyContinue | Stop-Process -Force
	$before = @(Get-ChildItem $data -Filter "screenshot_*.png" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })

	$gameArgs = @("--windowed") + $test.a
	$watch = [Diagnostics.Stopwatch]::StartNew()
	$p = Start-Process $Exe -ArgumentList $gameArgs -WorkingDirectory $root -PassThru
	$finished = $p.WaitForExit($test.t * 1000)
	if (-not $finished) { Stop-Process -Id $p.Id -Force }
	$secs = [int]$watch.Elapsed.TotalSeconds

	$result = "PASS"; $why = ""
	if (-not $finished) {
		$result = "FAIL"; $why = "no exit in $($test.t)s (hung)"
	} elseif ($p.ExitCode -ne 0) {
		$result = "FAIL"; $why = "exit code $($p.ExitCode) (crashed or test failed)"
	}

	if ($test.k -eq "shot" -and $result -eq "PASS") {
		Start-Sleep -Milliseconds 500
		$new = Get-ChildItem $data -Filter "screenshot_*.png" -ErrorAction SilentlyContinue |
			Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime | Select-Object -Last 1
		if ($null -eq $new) {
			$result = "FAIL"; $why = "no screenshot was taken"
		} else {
			Copy-Item $new.FullName "$Results\$($test.n).png" -Force
			if (-not (Test-RightSideFilled $new.FullName)) {
				$result = "FAIL"; $why = "right side of the screenshot is black"
			}
		}
	}

	$colour = if ($result -eq "PASS") { "Green" } else { "Red" }
	Write-Host ("{0,-5} {1,-18} {2,4}s  {3}" -f $result, $test.n, $secs, $(if ($why) { $why } else { $test.d })) -ForegroundColor $colour
	$summary += [pscustomobject]@{ Test = $test.n; Result = $result; Seconds = $secs; Detail = $why }
}

Get-Process dunedynasty -ErrorAction SilentlyContinue | Stop-Process -Force
$summary | Format-Table -AutoSize | Out-String | Set-Content "$Results\summary.txt"

$failed = @($summary | Where-Object { $_.Result -ne "PASS" }).Count
Write-Host ""
Write-Host "$($summary.Count - $failed) passed, $failed failed.  Screenshots: $Results" -ForegroundColor $(if ($failed) { "Red" } else { "Green" })
exit $(if ($failed) { 1 } else { 0 })
