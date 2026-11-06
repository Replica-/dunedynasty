# Busy lines for the radio voices: what a unit says when it is selected in the
# middle of a fight.  Same voices and radio effect as the other lines (gen_voices.ps1).
param([string]$Tools, [string]$Out)

$src = Get-Content "$Tools\radio_voice.ps1" -Raw
$m = [regex]::Match($src, '(?s)Add-Type -TypeDefinition @"\r?\n(.*?)\r?\n"@')
Add-Type -TypeDefinition $m.Groups[1].Value

$piper = "$Tools\piper_dl\bin\piper\piper.exe"
$models = "$Tools\piper_dl\voices"

# A little quicker than their other lines: they are in a hurry.
$voices = @(
	@{ n = "sergeant";  m = "en_US-ryan-high";                    s = 0.88 },
	@{ n = "trooper";   m = "en_US-joe-medium";                   s = 0.92 },
	@{ n = "officer";   m = "en_US-hfc_male-medium";              s = 0.88 },
	@{ n = "veteran";   m = "en_US-john-medium";                  s = 1.00 },
	@{ n = "rookie";    m = "en_US-bryce-medium";                 s = 0.84 },
	@{ n = "captain";   m = "en_GB-alan-medium";                  s = 0.92 },
	@{ n = "grunt";     m = "en_GB-northern_english_male-medium"; s = 0.92 }
)

# Every voice has the first two; the other three come from the rest, different for each.
$common = @(
	@("bsy_01_under_attack",   "Under attack!"),
	@("bsy_02_bit_busy_here",  "A bit busy here!")
)
$pool = @(
	@("taking_fire",       "Taking fire!"),
	@("engaging",          "Contact! Engaging!"),
	@("not_now",           "Not now, we're under fire!"),
	@("little_busy_sir",   "Little busy, sir!"),
	@("heavy_fire",        "Taking heavy fire!"),
	@("can_it_wait",       "Can it wait? We're fighting!"),
	@("hostiles_on_us",    "Hostiles on us!"),
	@("in_the_thick",      "We're in the thick of it!")
)

$rnd = New-Object System.Random 777
$total = 0
$i = 0

foreach ($v in $voices) {
	$dir = Join-Path $Out $v.n
	$raw = Join-Path $dir "_raw"
	New-Item -ItemType Directory -Force $dir, $raw | Out-Null

	$lines = @() + $common
	for ($k = 0; $k -lt 3; $k++) {
		$p = $pool[($i * 3 + $k) % $pool.Count]
		$lines += ,@(("bsy_{0:D2}_{1}" -f ($k + 3), $p[0]), $p[1])
	}
	$i++

	$json = foreach ($l in $lines) {
		(@{ text = $l[1]; output_file = (Join-Path $raw "$($l[0]).wav") } | ConvertTo-Json -Compress)
	}
	$json | & $piper --model "$models\$($v.m).onnx" --length_scale $v.s --json-input 2>&1 | Out-Null

	foreach ($l in $lines) {
		$in = Join-Path $raw "$($l[0]).wav"
		if (-not (Test-Path $in)) { Write-Host "MISSING $($v.n) $($l[0])"; continue }

		$rate = 0
		$s = [RadioFx]::Read($in, [ref]$rate)
		$hiss = 0.10 + 0.08 * $rnd.NextDouble()          # a touch more static: it is a fight
		$crackle = 0.0006 + 0.0005 * $rnd.NextDouble()
		$fx = [RadioFx]::Process($s, $rate, $rnd.Next(1, 100000), $hiss, $crackle)
		[RadioFx]::Write((Join-Path $dir "$($l[0]).wav"), $fx, $rate)
		$total++
		Write-Host ("{0,-9} {1,-28} {2}" -f $v.n, $l[0], $l[1])
	}
	Remove-Item $raw -Recurse
}
Write-Host "Wrote $total files to $Out"
