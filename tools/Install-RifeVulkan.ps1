param(
 [string]$Root=(Join-Path $env:LOCALAPPDATA 'ofxr-bridge/RIFE/runtime'),
 [string]$ResultJson,
 [string]$CacheRoot,
 [string]$CrtSource
)
$ErrorActionPreference='Stop'
$setup=Join-Path $PSScriptRoot 'Setup-RifeRuntime.ps1'
if(-not (Test-Path -LiteralPath $setup)){throw 'Extract the complete RIFE Vulkan installer ZIP first.'}
$arguments=@{Backend='vulkan';Root=$Root}
if($ResultJson){$arguments.ResultJson=$ResultJson}
if($CacheRoot){$arguments.CacheRoot=$CacheRoot}
if($CrtSource){$arguments.CrtSource=$CrtSource}
Write-Host 'Installing experimental Radeon Vulkan RIFE (CPU-only preparation tools).'
Write-Host 'The staged runtime is validated before activation; an existing runtime is retained.'
& $setup @arguments
exit $LASTEXITCODE
