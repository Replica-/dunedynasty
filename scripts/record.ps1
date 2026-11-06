<#
  Records a game: the Dune Dynasty window only (not the desktop) and the game's own
  sound only (not the rest of the computer).  It starts once the window is up, and
  stops a second and a half after the game is closed.

    .\scripts\record.ps1                                   # the main menu
    .\scripts\record.ps1 --house atreides --mission 5      # any game arguments

  The video goes to Videos\Dune Dynasty\dune_<date>_<time>.mp4.

  How: ffmpeg grabs the window (gdigrab) to an H.264 file, and the game writes the
  sound it mixes to a WAV file (--record-audio).  When the game has closed the two
  are lined up by the clock times they started at and put together.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
	[Parameter(ValueFromRemainingArguments = $true)]
	[string[]] $GameArgs = @(),
	[int] $Fps = 30,
	[double] $Tail = 1.5
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path "$PSScriptRoot\.."
$exe = Join-Path $root "dist\dunedynasty.exe"
$title = "Dune Dynasty"

$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
$ffprobe = (Get-Command ffprobe -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg -or -not $ffprobe) { throw "ffmpeg and ffprobe are needed on the PATH (winget install Gyan.FFmpeg)." }

$outDir = Join-Path ([Environment]::GetFolderPath("MyVideos")) "Dune Dynasty"
New-Item -ItemType Directory -Force $outDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$out = Join-Path $outDir "dune_$stamp.mp4"

$work = Join-Path $env:TEMP "dune-record-$stamp"
New-Item -ItemType Directory -Force $work | Out-Null
$wav = Join-Path $work "audio.wav"
$video = Join-Path $work "video.mkv"

# Start the game, with its sound going to the WAV file too.
$gameArgList = @("--windowed") + $GameArgs + @("--record-audio", $wav)
$game = Start-Process $exe -ArgumentList $gameArgList -WorkingDirectory (Join-Path $root "dist") -PassThru
Write-Host "Game started (pid $($game.Id)).  Waiting for its window..." -ForegroundColor Cyan

# The window: the game's process may hand over to another, so look for it by title.
$deadline = (Get-Date).AddSeconds(60)
$window = $null
while ((Get-Date) -lt $deadline -and $null -eq $window) {
	Start-Sleep -Milliseconds 250
	$window = Get-Process dunedynasty -ErrorAction SilentlyContinue |
		Where-Object { $_.MainWindowTitle -eq $title } | Select-Object -First 1
}
if ($null -eq $window) { throw "The game window did not appear." }

# Record the window.  Timestamps are the wall clock (gdigrab's own), kept with -copyts
# so the video can be lined up with the sound afterwards.
$ffArgs = @("-hide_banner", "-loglevel", "fatal", "-y",   # it complains when the window closes: that is how it ends
	"-f", "gdigrab", "-framerate", "$Fps", "-draw_mouse", "1", "-i", "title=$title",
	"-copyts", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "18", "-pix_fmt", "yuv420p",
	$video)

$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $ffmpeg
$psi.Arguments = ($ffArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.CreateNoWindow = $true
$rec = [Diagnostics.Process]::Start($psi)
Write-Host "Recording.  Close the game to stop." -ForegroundColor Green

# Until the game is closed, then a little more.
$window.WaitForExit()
Start-Sleep -Milliseconds ([int]($Tail * 1000))

if (-not $rec.HasExited) {
	$rec.StandardInput.Write("q")
	$rec.StandardInput.Close()
	if (-not $rec.WaitForExit(15000)) { $rec.Kill() }
}

if (-not (Test-Path $video)) { throw "No video was recorded." }

# Line up the sound with the video.
$videoStart = [double](& $ffprobe -v error -select_streams v:0 -show_entries stream=start_time -of csv=p=0 $video)
$audioStartFile = "$wav.start"
$haveAudio = (Test-Path $wav) -and (Test-Path $audioStartFile) -and ((Get-Item $wav).Length -gt 1000)

if ($haveAudio) {
	$audioStart = [double](Get-Content $audioStartFile | Select-Object -First 1) / 1000.0
	$lead = $videoStart - $audioStart   # how much of the sound came before the first frame

	$audioIn = if ($lead -ge 0) {
		@("-ss", ("{0:F3}" -f $lead), "-i", $wav)
	} else {
		@("-itsoffset", ("{0:F3}" -f (-$lead)), "-i", $wav)
	}

	& $ffmpeg -hide_banner -loglevel error -y -i $video @audioIn `
		-map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k -af "apad=pad_dur=$Tail" `
		-avoid_negative_ts make_zero -movflags +faststart $out 2>$null
	if ($LASTEXITCODE -ne 0) {
		& $ffmpeg -hide_banner -loglevel error -y -i $video @audioIn `
			-map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k `
			-avoid_negative_ts make_zero -movflags +faststart $out
	}
	Write-Host ("Sound lined up: {0:F3} s of it before the first frame." -f $lead)
} else {
	Write-Warning "No sound was recorded (is audio on in the game?).  Saving the video alone."
	& $ffmpeg -hide_banner -loglevel error -y -i $video -c:v copy -avoid_negative_ts make_zero -movflags +faststart $out
}

if (Test-Path $out) {
	Remove-Item -Recurse -Force $work
	Write-Host "Saved: $out" -ForegroundColor Green
} else {
	Write-Warning "Putting it together failed; the pieces are in $work"
	exit 1
}
