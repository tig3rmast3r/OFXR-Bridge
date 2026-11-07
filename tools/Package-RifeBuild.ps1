param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [int]$Version=898,
    [string]$SourceDir,
    [string]$CrtSource
)
$ErrorActionPreference='Stop'
if(-not $SourceDir){$SourceDir=Split-Path -Parent $PSScriptRoot}
$SourceDir=[IO.Path]::GetFullPath($SourceDir)
$BuildDir=[IO.Path]::GetFullPath($BuildDir)
$OutDir=[IO.Path]::GetFullPath($OutDir)
$release=Join-Path $BuildDir 'Release'
if(-not (Test-Path -LiteralPath (Join-Path $release 'OFXRBridgeTray.exe'))){$release=$BuildDir}
function Hash([string]$Path){return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function NormalizedHash([string]$Path){
    $text=[Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($Path)).Replace("`r`n","`n")
    $algorithm=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-','').ToLowerInvariant()}finally{$algorithm.Dispose()}
}
function Copy-Relative([string]$From,[string]$Relative,[string]$Stage){
    $to=Join-Path $Stage $Relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to))|Out-Null
    Copy-Item -LiteralPath $From -Destination $to
    if((Hash $From) -ne (Hash $to)){throw "Copy verification failed: $Relative"}
}
function Require-X64([string]$Path){
    $bytes=[IO.File]::ReadAllBytes($Path)
    if($bytes.Length -lt 64 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90){throw "Not a PE binary: $Path"}
    $offset=[BitConverter]::ToInt32($bytes,60)
    if($offset -lt 0 -or $offset+6 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes,$offset) -ne 0x4550 -or [BitConverter]::ToUInt16($bytes,$offset+4) -ne 0x8664){throw "Not a Windows x64 binary: $Path"}
}
$lockPath=Join-Path $SourceDir 'python/rife/runtime-lock.json'
$manifestPath=Join-Path $SourceDir 'python/rife/overlay-manifest.json'
$lock=Get-Content -LiteralPath $lockPath -Raw|ConvertFrom-Json
$overlay=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
if($overlay.schema -ne 1 -or $overlay.base_commit -ne $lock.model_source.commit){throw 'RIFE base/overlay revision mismatch.'}
foreach($asset in @($lock.assets)+@($lock.tensorrt_backend.assets)+@($lock.upstream_source)){
    if($asset.sha256 -notmatch '^[0-9a-f]{64}$' -or $asset.bytes -le 0 -or -not $asset.url.StartsWith('https://')){throw 'Every RIFE dependency needs a fixed HTTPS URL, byte size and SHA256.'}
}
$appRoot=[IO.Path]::GetFullPath((Join-Path $SourceDir 'python/rife'))
$overlayFiles=@($overlay.files.PSObject.Properties)
if($overlayFiles.Count -ne 17){throw 'Expected the complete reviewed 17-file RIFE overlay.'}
foreach($item in $overlayFiles){
    $path=[IO.Path]::GetFullPath((Join-Path $appRoot $item.Name))
    if(-not $path.StartsWith($appRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Overlay path escapes its source root.'}
    if((NormalizedHash $path) -ne $item.Value){throw "RIFE overlay source differs from its manifest: $($item.Name)"}
}
$actualOverlay=@(Get-ChildItem -LiteralPath $appRoot -Recurse -File|Where-Object{$_.FullName -notmatch '[\\/]__pycache__[\\/]' -and $_.Name -ne 'overlay-manifest.json'})
if($actualOverlay.Count -ne $overlayFiles.Count){throw 'Unlisted files exist in the RIFE overlay source.'}
$binaries=[ordered]@{
    'OFXRBridgeTray.exe'='OFXRBridgeTray.exe'
    'ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll'='XR_APILAYER_XRFrameBridge_diagnostic.dll'
    'ofxr/OFXR_vulkan_queue_layer.dll'='OFXR_vulkan_queue_layer.dll'
    'ofxr/nvngx.dll_dlssg.dll'='nvngx.dll_dlssg.dll'
    'ofxr/nvngx.dll_dlssnr.dll'='nvngx.dll_dlssnr.dll'
}
$providers=[ordered]@{
    'PDAFWPlugin.dll'='5f3cfc38903ac438241a3c47cb4bc3ac329cb39b056e33a63c20045fe15336f9'
    'nvngx_dlssg.dll'='ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82'
    'ofxr_amd_fidelityfx_framegeneration_dx12.dll'='12a5081257ec95b0b53ad51b4a87fb3c03f97fe0bbb59f9496968f8d50ef93a6'
}
foreach($name in $binaries.Values){Require-X64 (Join-Path $release $name)}
# 0.2 V576: the 32-bit layer for 32-bit games, when the build bundled one.
$x86Layer=Join-Path $release 'ofxr_x86/XR_APILAYER_XRFrameBridge_diagnostic.dll'
if(Test-Path -LiteralPath $x86Layer){
    $bytes=[IO.File]::ReadAllBytes($x86Layer);$offset=[BitConverter]::ToInt32($bytes,60)
    if([BitConverter]::ToUInt16($bytes,$offset+4) -ne 0x14c){throw 'ofxr_x86 holds a layer that is not 32-bit.'}
    $binaries['ofxr_x86/XR_APILAYER_XRFrameBridge_diagnostic.dll']='ofxr_x86/XR_APILAYER_XRFrameBridge_diagnostic.dll'
}
$versionInfo=[Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $release 'OFXRBridgeTray.exe'))
if($versionInfo.FileMajorPart -ne 0 -or $versionInfo.FileMinorPart -ne 3 -or $versionInfo.FileBuildPart -ne 3 -or $versionInfo.FilePrivatePart -ne $Version){throw 'Tray binary version does not match the requested 0.3.3 build.'}
foreach($name in $providers.Keys){
    $path=Join-Path (Join-Path $release 'ofxr') $name
    Require-X64 $path
    if((Hash $path) -ne $providers[$name]){throw "Pinned provider differs: $name"}
}
if(-not $CrtSource){
    $candidate=Join-Path $PSScriptRoot 'rife-msvc-crt'
    if(Test-Path -LiteralPath $candidate){$CrtSource=$candidate}
    else{
        $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if(Test-Path -LiteralPath $vswhere){
            $installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if($installation){$CrtSource=Join-Path $installation ('VC/Redist/MSVC/'+$lock.msvc_crt.version+'/x64/Microsoft.VC143.CRT')}
        }
    }
}
if(-not $CrtSource){throw 'Provide -CrtSource with the pinned Microsoft.VC143.CRT folder. No latest redistributable is downloaded.'}
$CrtSource=[IO.Path]::GetFullPath($CrtSource)
foreach($item in $lock.msvc_crt.files){
    $path=Join-Path $CrtSource $item.name
    if((Get-Item -LiteralPath $path).Length -ne $item.bytes -or (Hash $path) -ne $item.sha256){throw "Pinned CRT file differs: $($item.name)"}
}
$crtLicense=Join-Path $SourceDir 'licenses/MSVC_CRT_LICENSE.rtf'
if((NormalizedHash $crtLicense) -ne '8facd2af82a812f1107b5ef0362b28ff0648ff8d8eb237e888a9b1a0ed21847c'){throw 'The pinned Microsoft redistributable license is missing or modified.'}
$documents=@('README.md','LICENSE','licenses/ATTRIBUTION.md','licenses/MSVC_CRT_LICENSE.rtf','docs/RIFE.md','docs/BUILDING_RIFE.md','docs/V849_NR_CAPTURE.md','docs/V850_MSFS_NR.md','docs/V851_NR_STARTUP.md','docs/V852_NR_COLOUR.md','docs/V862_RIFE_TIMEOUT.md','docs/V863_RIFE_PRECOMPILE.md','docs/V864_RIFE_OVERLAY_UPDATE.md','docs/V865_RIFE_PIPELINE.md','docs/V866_RIFE_OUTPUT_BANKS.md','docs/V867_RIFE_INPUT_TIMING.md','docs/V868_RIFE_FRACTION_BATCH.md','docs/V886_D11_FG_CAPTURE.md','docs/V890_OSR_VISIBLE_WARP.md','docs/V891_OSR_DISOCCLUSION_DEPTH.md','docs/OSR_HISTORY.md','docs/V892_BENCHMARK_MEMORY.md','docs/V893_UI_AND_INVENTORY.md','docs/V894_UEVR_PANEL_LIFETIME.md','docs/V895_UEVR_AUXILIARY_BUDGET.md','docs/V896_SHARED_UI_ATLAS.md','docs/V897_AUTOMATIC_UI_BUDGET.md','docs/V898_BENCHMARK_RECONFIGURE.md','scripts/Set-CrashDebug.ps1')
foreach($optional in @('CREDITS.md','THIRD_PARTY.md')){if(Test-Path -LiteralPath (Join-Path $SourceDir $optional)){$documents+=$optional}}
$licenseRoot=Join-Path $SourceDir 'licenses'
foreach($file in Get-ChildItem -LiteralPath $licenseRoot -Recurse -File){$documents+='licenses/'+$file.FullName.Substring($licenseRoot.Length+1).Replace('\','/')}
$documents=@($documents|Sort-Object -Unique)
$tools=@('tools/update_rife_overlay.py','tools/Precompile-Rife.ps1','tools/precompile_rife.py','tools/Inspect-NrCapture.py','tools/Setup-RifeRuntime.ps1','tools/prepare_rife_runtime.py','tools/install_rife_runtime.py','tools/build_rife_overlay_manifest.py')
foreach($relative in $documents+$tools){if(-not (Test-Path -LiteralPath (Join-Path $SourceDir $relative))){throw "Missing distribution source file: $relative"}}
$name='OFXR-BRIDGE-0.3.3-v'+$Version
$stage=Join-Path $OutDir $name
$archive=Join-Path $OutDir ($name+'.zip')
if((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)){throw 'Existing package is preserved. Choose another empty output directory.'}
[IO.Directory]::CreateDirectory($stage)|Out-Null
foreach($relative in $binaries.Keys){Copy-Relative (Join-Path $release $binaries[$relative]) $relative $stage}
foreach($provider in $providers.Keys){Copy-Relative (Join-Path (Join-Path $release 'ofxr') $provider) ('ofxr/'+$provider) $stage}
foreach($relative in $documents+$tools){Copy-Relative (Join-Path $SourceDir $relative) $relative $stage}
Copy-Relative $manifestPath 'python/rife/overlay-manifest.json' $stage
foreach($item in $overlayFiles){Copy-Relative (Join-Path $appRoot $item.Name) ('python/rife/'+$item.Name) $stage}
foreach($item in $lock.msvc_crt.files){Copy-Relative (Join-Path $CrtSource $item.name) ('tools/rife-msvc-crt/'+$item.name) $stage}
$sourceCommit='unavailable (source archive)'
if(Test-Path -LiteralPath (Join-Path $SourceDir '.git')){
    $sourceCommit=(& git -C $SourceDir rev-parse HEAD).Trim()
    if($LASTEXITCODE){throw 'Unable to identify the source commit.'}
}
$proof=[ordered]@{schema=1;version=$Version;source_commit=$sourceCommit;runtime_lock_sha256=(NormalizedHash $lockPath);overlay_sha256=(NormalizedHash $manifestPath);overlay_files=$overlayFiles.Count;crt_version=$lock.msvc_crt.version;crt_files=@($lock.msvc_crt.files).Count;crt_license_sha256=(Hash $crtLicense);crt_license_normalized_sha256=(NormalizedHash $crtLicense);providers=$providers;runtime_or_model_weights_bundled=$false;authenticode_signing='Package only already-built/signed binaries; this helper never signs or modifies executable bytes.'}
[IO.File]::WriteAllText((Join-Path $stage 'BUILD_PROVENANCE.json'),($proof|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
$files=@(Get-ChildItem -LiteralPath $stage -Recurse -File|Sort-Object FullName)
$hashes=[ordered]@{}
foreach($file in $files){
    $relative=$file.FullName.Substring($stage.Length+1).Replace('\','/')
    if($file.Extension -in @('.ini','.pdb','.log','.csv','.whl','.pkl','.onnx','.engine','.pyc') -or $file.Name -eq 'nvngx_dlssnr.dll'){throw "Forbidden runtime/cache/model payload: $relative"}
    $hashes[$relative]=Hash $file.FullName
}
$lines=@($hashes.Keys|ForEach-Object{$hashes[$_]+'  '+$_})
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'),(($lines -join "`n")+"`n"),[Text.Encoding]::ASCII)
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$output=[IO.File]::Open($archive,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
$created=[IO.Compression.ZipArchive]::new($output,[IO.Compression.ZipArchiveMode]::Create,$false)
try{
    foreach($file in Get-ChildItem -LiteralPath $stage -Recurse -File|Sort-Object FullName){
        $relative=$file.FullName.Substring($stage.Length+1).Replace('\','/')
        $entry=$created.CreateEntry($name+'/'+$relative,[IO.Compression.CompressionLevel]::Optimal)
        $from=[IO.File]::OpenRead($file.FullName);$to=$entry.Open()
        try{$from.CopyTo($to)}finally{$from.Dispose();$to.Dispose()}
    }
}finally{$created.Dispose();$output.Dispose()}
$zip=[IO.Compression.ZipFile]::OpenRead($archive)
try{
    if(@($zip.Entries|Where-Object{-not $_.FullName.EndsWith('/')}).Count -ne $hashes.Count+1){throw 'ZIP file count differs from staged payload.'}
    foreach($relative in $hashes.Keys){
        $entry=$zip.GetEntry($name+'/'+$relative)
        if(-not $entry){throw "ZIP member missing: $relative"}
        $stream=$entry.Open();$algorithm=[Security.Cryptography.SHA256]::Create()
        try{$digest=([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','').ToLowerInvariant()}finally{$stream.Dispose();$algorithm.Dispose()}
        if($digest -ne $hashes[$relative]){throw "ZIP verification failed: $relative"}
    }
}finally{$zip.Dispose()}
$archiveHash=Hash $archive
[IO.File]::WriteAllText(($archive+'.sha256'),($archiveHash+'  '+[IO.Path]::GetFileName($archive)+"`n"),[Text.Encoding]::ASCII)
Write-Host ('Package verified: '+$archive)
Write-Host ('SHA256: '+$archiveHash)
