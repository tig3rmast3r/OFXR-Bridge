param([Parameter(Mandatory=$true)][string]$Root,[string]$CrtSource)
$ErrorActionPreference = 'Stop'
$runtimeRoot = [System.IO.Path]::GetFullPath($Root)
$sourceRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $sourceRoot 'python/rife/runtime-lock.json') -Raw | ConvertFrom-Json
$pythonAsset = $lock.assets | Where-Object { $_.path -like '*python-3.12.10-embed-amd64.zip' }
$cachePath = Join-Path $runtimeRoot $pythonAsset.path
$pythonPath = Join-Path $runtimeRoot 'python/python.exe'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $cachePath) | Out-Null
if (-not (Test-Path -LiteralPath $cachePath)) {
    Invoke-WebRequest -Uri $pythonAsset.url -OutFile $cachePath -UseBasicParsing
}
if ((Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pythonAsset.sha256) {
    throw 'Portable Python download checksum mismatch.'
}
if (-not (Test-Path -LiteralPath $pythonPath)) {
    Expand-Archive -LiteralPath $cachePath -DestinationPath (Join-Path $runtimeRoot 'python')
}
# Install app-local CRTs before starting the interpreter, which locks them.
$crtPath = if ($CrtSource) { $CrtSource } else { Join-Path $runtimeRoot 'downloads/msvc-crt' }
foreach ($file in $lock.msvc_crt.files) {
    $source = Join-Path $crtPath $file.name
    if (-not (Test-Path -LiteralPath $source) -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) {
        throw "Provide the pinned x64 Microsoft.VC143.CRT folder with -CrtSource: $($file.name)"
    }
    $target = Join-Path (Join-Path $runtimeRoot 'python') $file.name
    if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) {
        Copy-Item -LiteralPath $source -Destination $target
    }
}
$setupArguments = @('-I',(Join-Path $PSScriptRoot 'prepare_rife_runtime.py'),'--root',$runtimeRoot)
if ($CrtSource) { $setupArguments += @('--crt-source',$CrtSource) }
& $pythonPath @setupArguments
if ($LASTEXITCODE -ne 0) { throw 'Portable RIFE setup failed.' }
