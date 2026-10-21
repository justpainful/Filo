# Loads the MSVC x64 developer environment into the current PowerShell session.
$ErrorActionPreference = 'Stop'
if ($env:FILO_ENV_LOADED -eq '1') { return }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 with the C++ x64 toolset was not found.' }
$bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$ninjaDir = Split-Path (Get-Command ninja -ErrorAction Stop).Source
$env:PATH = "$ninjaDir;$env:PATH"
$env:FILO_ENV_LOADED = '1'
