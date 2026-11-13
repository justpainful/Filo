# Stages a Release build and compiles the installer.
#   pwsh scripts\package.ps1 [-Config Release] [-Suffix _rel] [-Version 1.0.0] [-FfmpegDir <folder with ffmpeg.exe + ffprobe.exe>]
# Needs: a finished build (scripts\build.ps1 -Config Release -Suffix _rel), Inno Setup 6, the FFmpeg "full" GPL build.
param(
  [string]$Config = "Release",
  [string]$Suffix = "_rel",
  [ValidatePattern("^[0-9]+[.][0-9]+[.][0-9]+$")][string]$Version = "0.4.0",
  [string]$FfmpegDir = "",
  [string]$YtDlpExe = "",                     # the real yt-dlp.exe (the standalone build, not a shim); found automatically when it is in scoop or on PATH
  [string]$GalleryDlExe = "",                 # the standalone gallery-dl.exe (GPL-2.0, reads Instagram); found in third_party\gallery-dl, scoop or on PATH
  [switch]$NoGalleryDl                        # build an installer without it (Instagram then goes through yt-dlp only)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config$Suffix\bin"
$stage = Join-Path $root "build\stage-$Version"
$dist = Join-Path $root "dist"

if (-not (Test-Path (Join-Path $bin "Filo.exe"))) { throw "Filo.exe not found in $bin - build it first." }
if (-not $FfmpegDir) {
  $cmd = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
  if ($cmd) { $FfmpegDir = Split-Path -Parent $cmd.Source }
}
if (-not $FfmpegDir -or -not (Test-Path (Join-Path $FfmpegDir "ffmpeg.exe")) -or -not (Test-Path (Join-Path $FfmpegDir "ffprobe.exe"))) { throw "ffmpeg.exe / ffprobe.exe not found. Pass -FfmpegDir." }
if (-not $YtDlpExe) {
  $cands = @("$env:USERPROFILE\scoop\apps\yt-dlp\current\yt-dlp.exe")
  $cmd = Get-Command yt-dlp.exe -ErrorAction SilentlyContinue
  if ($cmd) { $cands += $cmd.Source }
  $YtDlpExe = $cands | Where-Object { (Test-Path $_) -and ((Get-Item $_).Length -gt 5MB) } | Select-Object -First 1   # shims are tiny launchers: skip them
}
if (-not $YtDlpExe -or -not (Test-Path $YtDlpExe)) { throw "The standalone yt-dlp.exe was not found (link downloads need it). Pass -YtDlpExe." }
if (-not $NoGalleryDl) {
  if (-not $GalleryDlExe) {
    $cands = @((Join-Path $root "third_party\gallery-dl\gallery-dl.exe"), "$env:USERPROFILE\scoop\apps\gallery-dl\current\gallery-dl.exe")
    $cmd = Get-Command gallery-dl.exe -ErrorAction SilentlyContinue
    if ($cmd) { $cands += $cmd.Source }
    $GalleryDlExe = $cands | Where-Object { (Test-Path $_) -and ((Get-Item $_).Length -gt 5MB) } | Select-Object -First 1
  }
  if (-not $GalleryDlExe -or -not (Test-Path $GalleryDlExe)) { throw "The standalone gallery-dl.exe was not found (Instagram needs it). Pass -GalleryDlExe, or -NoGalleryDl to leave it out." }
}
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found." }

$resolvedStage = [IO.Path]::GetFullPath($stage)
$expectedStage = [IO.Path]::GetFullPath((Join-Path $root "build\stage-$Version"))
if ($resolvedStage -ne $expectedStage -or -not $resolvedStage.StartsWith([IO.Path]::GetFullPath($root) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to clear a staging folder outside this workspace.' }
$runningStage = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($resolvedStage + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) }
if ($runningStage) { throw "The versioned staging directory is in use; refusing to clean it: $resolvedStage" }
if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\tools\exiftool", "$stage\licenses\ffmpeg", "$stage\licenses\pdfium", "$stage\licenses\exiftool" | Out-Null

Copy-Item (Join-Path $bin "Filo.exe") $stage
Copy-Item (Join-Path $root "assets\icon-packs") $stage -Recurse
Copy-Item (Join-Path $root "src\app\filo.ico") $stage
Copy-Item (Join-Path $root "third_party\pdfium\bin\pdfium.dll") $stage
Copy-Item (Join-Path $FfmpegDir "ffmpeg.exe") "$stage\tools"
Copy-Item (Join-Path $FfmpegDir "ffprobe.exe") "$stage\tools"
# link downloads: yt-dlp plus Filo's own plugin (photos of TikTok slideshows and X posts)
Copy-Item $YtDlpExe "$stage\tools\yt-dlp.exe"
Copy-Item (Join-Path $root "assets\yt-dlp-plugins") "$stage\tools\yt-dlp-plugins" -Recurse
New-Item -ItemType Directory -Force "$stage\licenses\yt-dlp" | Out-Null
Copy-Item (Join-Path $root "assets\licenses\yt-dlp-UNLICENSE.txt") "$stage\licenses\yt-dlp\LICENSE.txt"
# Instagram: gallery-dl, a separate GPL-2.0 program (see docs\LICENSES.md for the obligations that come with shipping it)
if (-not $NoGalleryDl) {
  Copy-Item $GalleryDlExe "$stage\tools\gallery-dl.exe"
  New-Item -ItemType Directory -Force "$stage\licenses\gallery-dl" | Out-Null
  Copy-Item (Join-Path $root "assets\licenses\gallery-dl-GPL-2.0.txt") "$stage\licenses\gallery-dl\LICENSE.txt"
  Set-Content -Path "$stage\licenses\gallery-dl\SOURCE.txt" -Encoding utf8 -Value "gallery-dl is free software under the GNU GPL v2 (LICENSE.txt).`r`nThe source code of the bundled version is at https://codeberg.org/mikf/gallery-dl (the release tag of the same version)."
}

$et = Get-ChildItem (Join-Path $root "third_party\exiftool") -Directory | Select-Object -First 1
Copy-Item (Join-Path $et.FullName "exiftool(-k).exe") "$stage\tools\exiftool\exiftool.exe"
Copy-Item (Join-Path $et.FullName "exiftool_files") "$stage\tools\exiftool\exiftool_files" -Recurse
Copy-Item (Join-Path $et.FullName "README.txt") "$stage\licenses\exiftool\README.txt"
$lz = Join-Path $et.FullName "exiftool_files\Licenses_Strawberry_Perl.zip"
if (Test-Path $lz) { Copy-Item $lz "$stage\licenses\exiftool\" }

$ffRoot = Split-Path -Parent $FfmpegDir
foreach ($f in "LICENSE", "README.txt") { if (Test-Path (Join-Path $ffRoot $f)) { Copy-Item (Join-Path $ffRoot $f) "$stage\licenses\ffmpeg\$f" } }
Copy-Item (Join-Path $root "third_party\pdfium\LICENSE") "$stage\licenses\pdfium\LICENSE"
if (Test-Path (Join-Path $root "third_party\pdfium\licenses")) { Copy-Item (Join-Path $root "third_party\pdfium\licenses\*") "$stage\licenses\pdfium" -Recurse }
Copy-Item (Join-Path $root "docs\LICENSES.md") "$stage\licenses\THIRD-PARTY-NOTICES.md"

New-Item -ItemType Directory -Force $dist | Out-Null
& $iscc "/DStageDir=$stage" "/DAppVersion=$Version" "/DOutDir=$dist" (Join-Path $root "installer\filo.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
Get-ChildItem $dist -Filter "Filo-Setup-*.exe" | Select-Object Name, @{n = "MB"; e = { [math]::Round($_.Length / 1MB, 1) } }
