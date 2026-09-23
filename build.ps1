# Build script for CleanStart Installer.
# Generates the broom icon and app manifest, then compiles src/installer.cpp
# into bin/CleanStartInstaller.exe (statically linked, comctl32 v6 themed).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

$ErrorActionPreference = "Stop"
$src = $PSScriptRoot
# Output goes to a bin folder NEXT TO WHERE THE SCRIPT IS RUN FROM (current working directory),
# not next to the script. Run it from your repo root and you get <cwd>\bin\CleanStartInstaller.exe.
$binDir = Join-Path (Get-Location) "bin"
if (-not (Test-Path $binDir)) { New-Item -ItemType Directory -Path $binDir | Out-Null }
Write-Host "Output folder: $binDir"

# ---------------- 1. generate app.manifest from template ----------------
$manifest = Join-Path $src "app.manifest"
Copy-Item (Join-Path $src "app.template.manifest") $manifest -Force
# Keep only the Common-Controls dependency (no forced UAC) - UI stays as before
(Get-Content $manifest -Raw) -replace '(?s)<trustInfo.*?</trustInfo>\s*', '' | Set-Content $manifest -Encoding UTF8
Write-Host "Manifest: $manifest"

# ---------------- 2. generate CleanStart.ico (broom, 16/32/48/256 px) ----------------
Add-Type -AssemblyName System.Drawing

function New-BroomBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $s = $size / 256.0

    # Handle: diagonal stick from top-right to middle
    $handlePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(146, 102, 60), (22 * $s))
    $handlePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $handlePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($handlePen, (200 * $s), (28 * $s), (120 * $s), (140 * $s))

    # Broom head: trapezoid fan
    $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(232, 180, 60))
    $pts = @(
        (New-Object System.Drawing.PointF((100 * $s), (125 * $s))),
        (New-Object System.Drawing.PointF((140 * $s), (125 * $s))),
        (New-Object System.Drawing.PointF((165 * $s), (225 * $s))),
        (New-Object System.Drawing.PointF((75 * $s), (225 * $s)))
    )
    $g.FillPolygon($brush, $pts)

    # Bristle lines
    $linePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(170, 120, 30), (4 * $s))
    for ($i = 1; $i -le 3; $i++) {
        $t = $i / 4.0
        $topX = 100 + (40 * $t)
        $botX = 75 + (90 * $t)
        $g.DrawLine($linePen, ($topX * $s), (128 * $s), ($botX * $s), (222 * $s))
    }

    # Binding band
    $bandPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(120, 80, 30), (10 * $s))
    $g.DrawLine($bandPen, (95 * $s), (150 * $s), (145 * $s), (150 * $s))

    $g.Dispose()
    return $bmp
}

$sizes = @(16, 32, 48, 256)
$images = @()
foreach ($sz in $sizes) { $images += New-BroomBitmap $sz }

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([UInt16]0)      # reserved
$bw.Write([UInt16]1)      # type: icon
$bw.Write([UInt16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
$entries = @()
foreach ($bmp in $images) {
    $pngMs = New-Object System.IO.MemoryStream
    $bmp.Save($pngMs, [System.Drawing.Imaging.ImageFormat]::Png)
    $png = $pngMs.ToArray()
    $sz = $bmp.Width
    $entries += ,@($sz, $png.Length, $offset, $png)
    $offset += $png.Length
}
foreach ($e in $entries) {
    $sz = $e[0]
    $bw.Write([Byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $bw.Write([Byte]($(if ($sz -ge 256) { 0 } else { $sz })))
    $bw.Write([Byte]0); $bw.Write([Byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$e[1]); $bw.Write([UInt32]$e[2])
}
foreach ($e in $entries) { $bw.Write($e[3]) }
$bw.Flush()

$icoPath = Join-Path $src "CleanStart.ico"
[System.IO.File]::WriteAllBytes($icoPath, $ms.ToArray())
Write-Host "Icon: $icoPath ($($ms.Length) bytes)"

# ---------------- 3. locate g++ ----------------
$gppCandidates = @("C:\msys64\ucrt64\bin\g++.exe", "C:\Program Files\CodeBlocks\MinGW\bin\g++.exe")
$gpp = $gppCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $gpp) { $gpp = "g++" }
$gppDir = Split-Path (Resolve-Path ($gpp -replace '/', '\')) -Parent
$env:PATH = "$gppDir;$env:PATH"
Write-Host "Compiler: $gpp"

# ---------------- 4. compile ----------------
$exe = Join-Path $binDir "CleanStartInstaller.exe"
& $gpp -municode -mwindows -O2 -std=c++17 -static -static-libgcc -static-libstdc++ `
  -o $exe (Join-Path $src "installer.cpp") (Join-Path $src "installer.rc") `
  -lcomctl32 -lshell32 -lole32 -luuid -lshlwapi
if ($LASTEXITCODE -ne 0) { Write-Host "Build FAILED." -ForegroundColor Red; exit 1 }
Write-Host "Build OK: $exe" -ForegroundColor Green
