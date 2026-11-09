param(
 [string]$Root=(Join-Path $env:LOCALAPPDATA 'ofxr-bridge/RIFE/runtime'),
 [int]$Width=64,[int]$Height=64,
 [string]$ResultJson=(Join-Path $env:TEMP 'OFXR-Radeon-Vulkan-test.json')
)
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root)
if(-not (Test-Path -LiteralPath (Join-Path $Root 'vulkan/ofxr_rife_vulkan.exe'))){throw 'Install the Radeon Vulkan RIFE option first.'}
& (Join-Path $Root 'python/python.exe') -I -B (Join-Path $PSScriptRoot 'test_rife_vulkan.py') --root $Root --width $Width --height $Height --result $ResultJson
if($LASTEXITCODE -ne 0){throw 'Radeon Vulkan test failed. See the report and cache preparation log.'}
