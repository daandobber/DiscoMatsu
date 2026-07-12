param(
    [string]$Slug = "nl.daandobber.discomatsu",
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

Copy-Item $BuildBin (Join-Path $outDir "discomatsu.bin") -Force
Copy-Item ".\LICENSE" (Join-Path $outDir "LICENSE") -Force
Copy-Item ".\assets\icons\icon16.png" (Join-Path $outDir "icon16.png") -Force
Copy-Item ".\assets\icons\icon32.png" (Join-Path $outDir "icon32.png") -Force
Copy-Item ".\assets\icons\icon64.png" (Join-Path $outDir "icon64.png") -Force

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
            executable = "discomatsu.bin"
        }
    )
}

$metadata | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $outDir "metadata.json")

$zipPath = ".\dist\discmatsu-app-repository-package.zip"
Compress-Archive -Path $outDir -DestinationPath $zipPath -Force

Write-Host "Wrote app-repository package to $outDir"
Write-Host "Wrote zip to $zipPath"
