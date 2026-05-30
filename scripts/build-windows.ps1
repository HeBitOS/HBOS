[CmdletBinding()]
param(
    [ValidateSet("all", "iso", "bios-iso", "uefi-iso", "clean")]
    [string]$Target = "all",

    [switch]$Clean,

    [string]$Distro = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Convert-ToWslPath {
    param([string]$Path)

    $resolved = (Resolve-Path $Path).Path
    $converted = & wsl.exe wslpath -a -u "$resolved"
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($converted)) {
        throw "Failed to convert Windows path to WSL path: $resolved"
    }
    return $converted.Trim()
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw "wsl.exe was not found. Install WSL first, then run this script again."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$wslRepo = Convert-ToWslPath $repoRoot

$makeTarget = $Target
if ($Clean -and $Target -ne "clean") {
    $makeTarget = "clean $Target"
}

$deps = "command -v make >/dev/null && command -v gcc >/dev/null && command -v nasm >/dev/null && command -v grub-mkrescue >/dev/null && command -v xorriso >/dev/null && command -v mkfs.fat >/dev/null && command -v mcopy >/dev/null"
$build = "cd '$wslRepo' && $deps && make $makeTarget"

$wslArgs = @()
if ($Distro -ne "") {
    $wslArgs += @("-d", $Distro)
}
$wslArgs += @("--", "bash", "-lc", $build)

Write-Host "[HBOS] Building in WSL: make $makeTarget"
& wsl.exe @wslArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Missing dependencies inside WSL can be installed with:"
    Write-Host "  sudo apt update"
    Write-Host "  sudo apt install build-essential nasm grub-pc-bin grub-efi-amd64-bin xorriso mtools dosfstools python3 python3-pil"
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "[HBOS] Build complete:"
Write-Host "  build/hbos-bios.iso"
Write-Host "  build/hbos-uefi.iso"
