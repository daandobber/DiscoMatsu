param(
    [string]$Slug = "nl.daandobber.discmatsu",
    [string]$Version = "0.1.0",
    [int]$Revision = 1,
    [string]$BuildBin = ".\build\tanmatsu\application.bin",
    [string]$OutRoot = ".\dist\app-repository"
)

$ErrorActionPreference = "Stop"

$outDir = Join-Path $OutRoot $Slug
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if (!(Test-Path $BuildBin)) {
    throw "Build binary not found: $BuildBin"
}

Copy-Item $BuildBin (Join-Path $outDir "discmatsu.bin") -Force
Copy-Item ".\LICENSE" (Join-Path $outDir "LICENSE") -Force

Add-Type -AssemblyName System.Drawing

function New-Icon {
    param(
        [int]$Size,
        [string]$Path
    )

    $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor

        $bg = [System.Drawing.Color]::FromArgb(255, 2, 10, 7)
        $grid = [System.Drawing.Color]::FromArgb(255, 18, 86, 46)
        $green = [System.Drawing.Color]::FromArgb(255, 51, 255, 102)
        $soft = [System.Drawing.Color]::FromArgb(255, 132, 255, 166)

        $g.Clear($bg)
        $gridBrush = New-Object System.Drawing.SolidBrush($grid)
        $greenBrush = New-Object System.Drawing.SolidBrush($green)
        $softBrush = New-Object System.Drawing.SolidBrush($soft)
        $gridPen = New-Object System.Drawing.Pen($grid, 1)
        $borderPen = New-Object System.Drawing.Pen($green, [Math]::Max(1, [int]($Size / 18)))

        try {
            $step = [Math]::Max(3, [int]($Size / 8))
            for ($x = 2; $x -lt $Size - 2; $x += $step) {
                $g.DrawLine($gridPen, $x, 2, $x, $Size - 3)
            }
            for ($y = 2; $y -lt $Size - 2; $y += $step) {
                $g.DrawLine($gridPen, 2, $y, $Size - 3, $y)
            }

            $cell = [Math]::Max(1, [int]($Size / 12))
            $points = @(@(1, 1), @(2, 0), @(0, 3), @(7, 1), @(6, 5), @(2, 7), @(7, 7))
            foreach ($point in $points) {
                $px = [Math]::Min($Size - $cell - 2, 2 + $point[0] * $step)
                $py = [Math]::Min($Size - $cell - 2, 2 + $point[1] * $step)
                $g.FillRectangle($softBrush, $px, $py, $cell, $cell)
            }

            if ($Size -eq 16) {
                $fontSize = 12
                $textRect = [System.Drawing.RectangleF]::new(0, 0, $Size, $Size + 1)
            } elseif ($Size -eq 32) {
                $fontSize = 24
                $textRect = [System.Drawing.RectangleF]::new(0, -1, $Size, $Size + 2)
            } else {
                $fontSize = 48
                $textRect = [System.Drawing.RectangleF]::new(0, -2, $Size, $Size + 4)
            }

            $font = New-Object System.Drawing.Font("Consolas", $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
            $format = New-Object System.Drawing.StringFormat
            try {
                $format.Alignment = [System.Drawing.StringAlignment]::Center
                $format.LineAlignment = [System.Drawing.StringAlignment]::Center
                $g.DrawString("D", $font, $greenBrush, $textRect, $format)
            } finally {
                $format.Dispose()
                $font.Dispose()
            }

            $g.DrawRectangle($borderPen, 1, 1, $Size - 3, $Size - 3)
        } finally {
            $borderPen.Dispose()
            $gridPen.Dispose()
            $softBrush.Dispose()
            $greenBrush.Dispose()
            $gridBrush.Dispose()
        }

        $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $g.Dispose()
        $bmp.Dispose()
    }
}

New-Icon 16 (Join-Path $outDir "icon16.png")
New-Icon 32 (Join-Path $outDir "icon32.png")
New-Icon 64 (Join-Path $outDir "icon64.png")

$metadata = [ordered]@{
    name = "Disc-O-Matsu"
    description = "USB CD/DVD audio player for Tanmatsu"
    categories = @("media")
    version = $Version
    icon = [ordered]@{
        "16x16" = "icon16.png"
        "32x32" = "icon32.png"
        "64x64" = "icon64.png"
    }
    author = "Daan Dobber"
    license_type = "MIT"
    license_file = "LICENSE"
    application = @(
        [ordered]@{
            targets = @("tanmatsu", "konsool")
            revision = $Revision
            type = "appfs"
            executable = "discmatsu.bin"
        }
    )
}

$metadata | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $outDir "metadata.json")

$zipPath = ".\dist\discmatsu-app-repository-package.zip"
Compress-Archive -Path $outDir -DestinationPath $zipPath -Force

Write-Host "Wrote app-repository package to $outDir"
Write-Host "Wrote zip to $zipPath"
