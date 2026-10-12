param(
    [string]$Path = "preview\\injector.png",
    [int]$Cols = 78
)
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Path))
$w = $bmp.Width; $h = $bmp.Height
$cw = [math]::Max(1, [int]($w / $Cols))
$ch = [int]($cw * 2.0)               # character cells are ~2x tall
$rows = [int]($h / $ch)

$sb = New-Object System.Text.StringBuilder
for ($ry = 0; $ry -lt $rows; $ry++) {
    for ($rx = 0; $rx -lt $Cols; $rx++) {
        $r = 0; $g = 0; $b = 0; $n = 0
        for ($y = $ry * $ch; $y -lt [math]::Min(($ry + 1) * $ch, $h); $y += 2) {
            for ($x = $rx * $cw; $x -lt [math]::Min(($rx + 1) * $cw, $w); $x += 2) {
                $c = $bmp.GetPixel($x, $y); $r += $c.R; $g += $c.G; $b += $c.B; $n++
            }
        }
        if ($n -eq 0) { [void]$sb.Append(' '); continue }
        $r = $r / $n; $g = $g / $n; $b = $b / $n
        $lum = 0.299 * $r + 0.587 * $g + 0.114 * $b
        $mx = [math]::Max($r, [math]::Max($g, $b))
        $mn = [math]::Min($r, [math]::Min($g, $b))
        $sat = if ($mx -gt 0) { ($mx - $mn) / $mx } else { 0 }

        $ch2 = ' '
        if ($sat -gt 0.30 -and $lum -gt 75) {
            if ($b -ge $r -and $b -ge $g -and $r -gt $g) { $ch2 = 'V' }        # violet
            elseif ($b -ge $g -and $g -gt $r)          { $ch2 = 'C' }          # cyan
            elseif ($g -ge $r -and $g -ge $b)          { $ch2 = 'G' }
            elseif ($r -gt $g -and $r -gt $b)          { $ch2 = 'R' }
        } else {
            if ($lum -gt 200)     { $ch2 = '#' }
            elseif ($lum -gt 140) { $ch2 = 'O' }
            elseif ($lum -gt 90)  { $ch2 = 'o' }
            elseif ($lum -gt 55)  { $ch2 = '+' }
            elseif ($lum -gt 32)  { $ch2 = '-' }
            else                  { $ch2 = '.' }
        }
        [void]$sb.Append($ch2)
    }
    [void]$sb.AppendLine()
}
$bmp.Dispose()
Write-Output $sb.ToString()
