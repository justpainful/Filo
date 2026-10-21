param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Config = 'Debug',
    [string]$Target = '',
    [string]$Suffix = '',
    [switch]$NoEngines,
    [string]$ToolEngines = '',
    [string]$ExcludeSrc = '',
    [switch]$Reconfigure
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\env.ps1"
$root = Split-Path $PSScriptRoot
$bd = Join-Path $root "build\$Config$Suffix"
if ($Reconfigure -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
if (-not (Test-Path (Join-Path $bd 'build.ninja'))) {
    $extra = @(); if ($NoEngines) { $extra += '-DFILO_WITH_ENGINES=OFF' }; if ($ExcludeSrc) { $extra += "-DFILO_EXCLUDE_SRC=$ExcludeSrc" }; if ($ToolEngines) { $extra += "-DFILO_TOOL_ENGINES=$ToolEngines" }
    cmake -S $root -B $bd -G Ninja @extra "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$args2 = @('--build', $bd)
if ($Target) { $args2 += @('--target', $Target) }
cmake @args2
exit $LASTEXITCODE
