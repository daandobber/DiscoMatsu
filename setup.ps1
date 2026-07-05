param(
    [switch]$SetupSdk,
    [switch]$SetupBadgeLink
)

# One-time setup for a fresh clone of this repo: fetches the ESP-IDF SDK
# and/or BadgeLink tooling alongside the source, so .\install-badgelink.ps1
# has everything it needs to build and install Disc-O-Matsu.

$ErrorActionPreference = "Stop"

function Invoke-Step {
    param([string]$Title, [scriptblock]$Body)
    Write-Host ""
    Write-Host "==> $Title"
    & $Body
}

if ($SetupSdk) {
    Invoke-Step "Clone and install ESP-IDF v5.5.1" {
        $env:IDF_TOOLS_PATH = Join-Path (Get-Location).Path "esp-idf-tools"
        if (Test-Path ".\esp-idf") {
            Write-Host "esp-idf already exists."
        } else {
            git clone --recursive --branch v5.5.1 --depth=1 --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf
        }
        if (Test-Path ".\esp-idf\install.ps1") {
            powershell -ExecutionPolicy Bypass -File ".\esp-idf\install.ps1" all
        } else {
            Write-Host "ESP-IDF cloned. Run the platform-specific install script in .\esp-idf manually."
        }
    }
}

if ($SetupBadgeLink) {
    Invoke-Step "Clone BadgeLink tooling" {
        if (Test-Path ".\badgelink_v020\tools\badgelink.py") {
            Write-Host "BadgeLink tooling already exists."
        } else {
            git clone https://github.com/badgeteam/esp32-component-badgelink.git badgelink_v020
        }
    }
}

Write-Host ""
Write-Host "Ready."
Write-Host ""
Write-Host "Next commands:"
Write-Host "  `$env:IDF_TOOLS_PATH = `"`$(Get-Location)\esp-idf-tools`""
Write-Host "  . .\esp-idf\export.ps1"
Write-Host "  idf.py --no-ccache -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS='sdkconfigs/general;sdkconfigs/tanmatsu' -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0"
Write-Host "  .\install-badgelink.ps1 -NoBuild"
