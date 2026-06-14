# Spatial Canvas - release paketleme
# Release x64 derler, exe + LICENSE + README'yi dist\SpatialCanvas-v<ver>.zip'e koyar.
# Exe statik CRT + OS WinRT ile baglidir; VC redist GEREKMEZ (tek dosya dagitim).
# Kullanim:  pwsh -File package.ps1 [-Version 0.58.0] [-SkipBuild]
# -Version verilmezse VERSION dosyasindan okunur (tek dogruluk kaynagi).
param(
    [string]$Version = "",
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $Version) {
    $vf = Join-Path $root "VERSION"
    if (Test-Path $vf) { $Version = (Get-Content $vf -Raw).Trim() }
    else { throw "VERSION dosyasi yok ve -Version verilmedi" }
}
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
$exe = Join-Path $root "x64\Release\SpatialCanvas.exe"

if (-not $SkipBuild) {
    Write-Host "[1/3] Release x64 derleniyor..." -ForegroundColor Cyan
    Get-Process -Name SpatialCanvas -EA SilentlyContinue | Stop-Process -Force
    & $msbuild (Join-Path $root "Win32CaptureSample.sln") -p:Configuration=Release -p:Platform=x64 -m -v:m
    if ($LASTEXITCODE -ne 0) { throw "Build basarisiz (exit $LASTEXITCODE)" }
}
if (-not (Test-Path $exe)) { throw "exe yok: $exe" }

# exe surum damgasi paket surumuyle uyusuyor mu (sessiz tutarlilik kontrolu)
$fv = (Get-Item $exe).VersionInfo.ProductVersion
if ($fv -and $fv -notmatch [regex]::Escape($Version)) {
    Write-Warning "exe surumu ($fv) paket surumuyle ($Version) uyusmuyor - app.rc guncel mi?"
}

Write-Host "[2/3] Paket hazirlaniyor..." -ForegroundColor Cyan
$stage = Join-Path $root "dist\SpatialCanvas-v$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $exe (Join-Path $stage "SpatialCanvas.exe")
Copy-Item (Join-Path $root "LICENSE") $stage
Copy-Item (Join-Path $root "README.md") $stage
Copy-Item (Join-Path $root "README.tr.md") $stage
Copy-Item (Join-Path $root "CHANGELOG.md") $stage

Write-Host "[3/3] Zip olusturuluyor..." -ForegroundColor Cyan
$zip = Join-Path $root "dist\SpatialCanvas-v$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

$exeKb = [math]::Round((Get-Item $exe).Length / 1kb)
$zipKb = [math]::Round((Get-Item $zip).Length / 1kb)
$sha = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host "`nTAMAM:" -ForegroundColor Green
Write-Host "  exe : $exeKb KB (tek dosya, redist gerektirmez)"
Write-Host "  zip : $zip ($zipKb KB)"
Write-Host "  SHA256: $sha"
