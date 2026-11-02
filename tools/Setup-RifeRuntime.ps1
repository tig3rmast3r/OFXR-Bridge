param(
 [string]$Root=(Join-Path $env:LOCALAPPDATA 'ofxr-bridge/RIFE/runtime'),
 [switch]$Check,[string]$ResultJson,[string]$CrtSource,[string]$CacheRoot,[switch]$SkipTensorRT
)
$ErrorActionPreference='Stop'
$runtimeRoot=[IO.Path]::GetFullPath($Root)
$sourceRoot=Split-Path -Parent $PSScriptRoot
$parentRoot=Split-Path -Parent $runtimeRoot
if(-not $ResultJson){$ResultJson=Join-Path $parentRoot "status/result-$PID.json"}
$ResultJson=[IO.Path]::GetFullPath($ResultJson)
$result=[ordered]@{schema=1;status='error';message='';root=$runtimeRoot;backend='';installed_bytes=7900000000;required_free_bytes=14000000000;log_path=''}
function Write-Result([string]$Status,[string]$Message){
 $result.status=$Status;$result.message=$Message
 [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($ResultJson))|Out-Null
 [IO.File]::WriteAllText($ResultJson,($result|ConvertTo-Json -Compress),[Text.UTF8Encoding]::new($false))
 Write-Host $Message
}
function Quote-Argument([string]$Value){
 if($Value.Contains('"')){throw 'Invalid quote in installer argument.'}
 return '"'+[regex]::Replace($Value,'(\\+)$','$1$1')+'"'
}
function Recover-Runtime {
 $journal=Join-Path $parentRoot ('.'+[IO.Path]::GetFileName($runtimeRoot)+'.transaction.json')
 if(-not (Test-Path -LiteralPath $journal)){return}
 $lease=Join-Path $parentRoot ('.'+[IO.Path]::GetFileName($runtimeRoot)+'.lease')
 try{$gate=[IO.File]::Open($lease,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}catch{throw 'RIFE_BUSY: RIFE is in use. Close the game and try again.'}
 try{
  $data=Get-Content -LiteralPath $journal -Raw|ConvertFrom-Json
  if($data.schema -ne 1 -or [IO.Path]::GetFullPath($data.root) -ne $runtimeRoot -or $data.phase -notin @('prepared','committed')){throw 'Invalid RIFE recovery journal.'}
  foreach($field in @('stage','previous')){
   $path=[IO.Path]::GetFullPath($data.$field);$kind=if($field -eq 'stage'){'stage'}else{'previous'}
   $pattern='^'+[regex]::Escape('.'+[IO.Path]::GetFileName($runtimeRoot)+'.'+$kind+'-')+'[0-9a-f]{32}$'
   if([IO.Path]::GetDirectoryName($path) -ne $parentRoot -or [IO.Path]::GetFileName($path) -notmatch $pattern){throw 'Recovery path escapes the managed runtime parent.'}
   if((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Recovery refuses a linked runtime directory.'}
  }
  if((Test-Path -LiteralPath $data.previous) -and ($data.phase -eq 'prepared' -or -not (Test-Path -LiteralPath $runtimeRoot))){
   if(Test-Path -LiteralPath $runtimeRoot){
    if(Test-Path -LiteralPath $data.stage){throw 'Ambiguous recovery directories; previous runtime preserved.'}
    [IO.Directory]::Move($runtimeRoot,$data.stage)
   }
   [IO.Directory]::Move($data.previous,$runtimeRoot)
  }
  if(Test-Path -LiteralPath $data.stage){Remove-Item -LiteralPath $data.stage -Recurse -Force}
  if($data.phase -eq 'committed' -and (Test-Path -LiteralPath $runtimeRoot) -and (Test-Path -LiteralPath $data.previous)){Remove-Item -LiteralPath $data.previous -Recurse -Force}
  Remove-Item -LiteralPath $journal -Force
 }finally{$gate.Dispose()}
}
function Remove-OwnedDownloadCache([bool]$Owned) {
 if(-not $Owned){return}
 $expectedCache=[IO.Path]::GetFullPath((Join-Path $parentRoot 'download-cache'))
 if($CacheRoot -eq $expectedCache -and (Test-Path -LiteralPath $CacheRoot) -and -not ((Get-Item -LiteralPath $CacheRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){
  Remove-Item -LiteralPath $CacheRoot -Recurse -Force
 }
}
function Invoke-OwnedPython([string]$Python,[string[]]$Arguments,[int]$TimeoutMilliseconds,[bool]$Visible){
 $start=[Diagnostics.ProcessStartInfo]::new()
 $start.FileName=$Python;$start.Arguments=(($Arguments|ForEach-Object{Quote-Argument $_}) -join ' ')
 $start.UseShellExecute=$false;$start.CreateNoWindow=(-not $Visible);$start.WorkingDirectory=$sourceRoot
 $start.EnvironmentVariables.Clear()
 foreach($name in @('SystemRoot','WINDIR','LOCALAPPDATA','TEMP','TMP')){
  $value=[Environment]::GetEnvironmentVariable($name);if($value){$start.EnvironmentVariables[$name]=$value}
 }
 $pythonDirectory=Split-Path -Parent $Python
 $start.EnvironmentVariables['PATH']=$pythonDirectory+';'+(Join-Path $pythonDirectory 'Lib/site-packages/torch/lib')+';'+(Join-Path $env:SystemRoot 'System32')
 $process=[Diagnostics.Process]::Start($start)
 if(-not $process.WaitForExit($TimeoutMilliseconds)){
  # Only this invocation's child and descendants, never game processes.
  & (Join-Path $env:SystemRoot 'System32/taskkill.exe') /PID $process.Id /T /F|Out-Null
  $process.WaitForExit(5000)|Out-Null
  throw 'The RIFE helper timed out. Its child processes were stopped; the working runtime was not replaced.'
 }
 $code=$process.ExitCode;$process.Dispose();return $code
}
try{
 $lock=Get-Content -LiteralPath (Join-Path $sourceRoot 'python/rife/runtime-lock.json') -Raw|ConvertFrom-Json
 Recover-Runtime
 if($Check){
  $python=Join-Path $runtimeRoot 'python/python.exe'
  if(-not (Test-Path -LiteralPath $python)){
   if(-not (Test-Path -LiteralPath (Join-Path $env:SystemRoot 'System32/nvcuda.dll'))){Write-Result 'unsupported' 'No NVIDIA CUDA driver was found. This release supports NVIDIA only.';exit 12}
   Write-Result 'missing' 'The portable RIFE runtime is not installed.';exit 10
  }
  $arguments=@('-I','-B',(Join-Path $sourceRoot 'python/rife/check_runtime.py'),'--root',$runtimeRoot,'--package',$sourceRoot,'--result',$ResultJson)
  $code=Invoke-OwnedPython $python $arguments 115000 $false
  if(-not (Test-Path -LiteralPath $ResultJson)){Write-Result 'repair' 'Portable Python could not start. RIFE needs repair.';exit 11}
  exit $code
 }
 if(-not (Test-Path -LiteralPath (Join-Path $env:SystemRoot 'System32/nvcuda.dll'))){Write-Result 'unsupported' 'No NVIDIA CUDA driver was found. This release supports NVIDIA only.';exit 12}
 $ownedCache=(-not $CacheRoot)
 if(-not $CacheRoot){$CacheRoot=Join-Path $parentRoot 'download-cache'}
 $CacheRoot=[IO.Path]::GetFullPath($CacheRoot)
 if(-not $CrtSource){$CrtSource=Join-Path $PSScriptRoot 'rife-msvc-crt'}
 $CrtSource=[IO.Path]::GetFullPath($CrtSource)
 foreach($item in $lock.msvc_crt.files){
  $path=Join-Path $CrtSource $item.name
  if(-not (Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256){throw "The release is missing its pinned portable CRT payload: $($item.name)"}
 }
 $pythonAsset=$lock.assets|Where-Object{$_.path -like '*python-3.12.10-embed-amd64.zip'}
 $cachePath=Join-Path $CacheRoot $pythonAsset.path
 [IO.Directory]::CreateDirectory((Split-Path -Parent $cachePath))|Out-Null
 if(-not (Test-Path -LiteralPath $cachePath) -or (Get-Item -LiteralPath $cachePath).Length -ne $pythonAsset.bytes -or (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pythonAsset.sha256){
  Write-Host 'Downloading the pinned portable Python bootstrap...'
  $partial=$cachePath+'.partial';Invoke-WebRequest -Uri $pythonAsset.url -OutFile $partial -UseBasicParsing
  if((Get-Item -LiteralPath $partial).Length -ne $pythonAsset.bytes -or (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pythonAsset.sha256){throw 'Portable Python SHA256 mismatch.'}
  Move-Item -LiteralPath $partial -Destination $cachePath -Force
 }
 $bootstrap=Join-Path $parentRoot ('bootstrap/python-3.12.10-'+$PID)
 [IO.Directory]::CreateDirectory($bootstrap)|Out-Null
 try{
  Expand-Archive -LiteralPath $cachePath -DestinationPath $bootstrap
  foreach($item in $lock.msvc_crt.files){Copy-Item -LiteralPath (Join-Path $CrtSource $item.name) -Destination (Join-Path $bootstrap $item.name)}
  $arguments=@('-I','-B',(Join-Path $PSScriptRoot 'install_rife_runtime.py'),'--root',$runtimeRoot,'--cache',$CacheRoot,'--crt-source',$CrtSource,'--result',$ResultJson)
  if($SkipTensorRT){$arguments+='--skip-tensorrt'}
  $code=Invoke-OwnedPython (Join-Path $bootstrap 'python.exe') $arguments 7200000 $true
  if(-not (Test-Path -LiteralPath $ResultJson)){throw 'The RIFE installer produced no result.'}
  if($code -eq 0 -and $ownedCache){
   Remove-OwnedDownloadCache $true
  }
 }finally{
  $full=[IO.Path]::GetFullPath($bootstrap);$allowed=[IO.Path]::GetFullPath((Join-Path $parentRoot 'bootstrap'))+[IO.Path]::DirectorySeparatorChar
  if($full.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)){Remove-Item -LiteralPath $full -Recurse -Force}
 }
 exit $code
}catch{
 if($_.Exception.Message.StartsWith('RIFE_BUSY:')){Write-Result 'busy' $_.Exception.Message;exit 13}
 Write-Result 'error' ('RIFE setup failed: '+$_.Exception.Message);exit 1
}
