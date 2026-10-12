param([string]$Path = "preview\\injector.png")
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Path))
$w = $bmp.Width; $h = $bmp.Height
$violet = 0; $cyan = 0; $green = 0; $red = 0; $bright = 0; $total = 0
$colors = @{}
$sumR = 0; $sumG = 0; $sumB = 0
for ($y = 0; $y -lt $h; $y += 2) {
  for ($x = 0; $x -lt $w; $x += 2) {
    $c = $bmp.GetPixel($x, $y)
    $total++
    $sumR += $c.R; $sumG += $c.G; $sumB += $c.B
    $key = "$($c.R),$($c.G),$($c.B)"
    if ($colors.ContainsKey($key)) { $colors[$key]++ } else { $colors[$key] = 1 }
    if ($c.R -gt 100 -and $c.B -gt 170 -and $c.G -lt 150) { $violet++ }
    if ($c.B -gt 180 -and $c.G -gt 140 -and $c.R -lt 150) { $cyan++ }
    if ($c.G -gt 150 -and $c.R -lt 150 -and $c.B -lt 190) { $green++ }
    if ($c.R -gt 170 -and $c.G -lt 130 -and $c.B -lt 130) { $red++ }
    if (($c.R + $c.G + $c.B) -gt 300) { $bright++ }
  }
}
$bmp.Dispose()
"image      : ${w} x ${h}"
"sampled    : $total px"
"distinct   : $($colors.Count) colours"
"avg colour : R$([int]($sumR/$total)) G$([int]($sumG/$total)) B$([int]($sumB/$total))"
"violet px  : $violet  ($([math]::Round(100*$violet/$total,2))%)"
"cyan px    : $cyan  ($([math]::Round(100*$cyan/$total,2))%)"
"green px   : $green  ($([math]::Round(100*$green/$total,2))%)"
"red px     : $red  ($([math]::Round(100*$red/$total,2))%)"
"bright px  : $bright  ($([math]::Round(100*$bright/$total,2))%)"
