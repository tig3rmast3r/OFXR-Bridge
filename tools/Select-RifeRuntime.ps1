param(
 [Parameter(Mandatory=$true)][ValidateSet('nvidia','vulkan')][string]$Backend,
 [string]$RuntimeDir,
 [string]$SettingsPath=(Join-Path $env:LOCALAPPDATA 'ofxr-bridge/ofxr-bridge.ini'),
 [string]$ResultJson,
 [switch]$AllowRunningTray
)
$ErrorActionPreference='Stop'
# Explicit selection only. Never discover development directories or move runtimes.
$managed=[IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'ofxr-bridge/RIFE/runtime'))
if(-not $RuntimeDir){$RuntimeDir=$managed}
if(-not [IO.Path]::IsPathRooted($RuntimeDir)){throw 'RuntimeDir must be an explicit absolute path.'}
$RuntimeDir=[IO.Path]::GetFullPath($RuntimeDir).TrimEnd('\','/')
if($RuntimeDir -match '[\r\n]'){throw 'Invalid runtime path.'}
if(-not (Test-Path -LiteralPath (Join-Path $RuntimeDir 'python/python.exe'))){throw 'The selected portable runtime is missing.'}
$native=Test-Path -LiteralPath (Join-Path $RuntimeDir 'vulkan/backend.json')
if($Backend -eq 'vulkan'){
 if(-not $native -or -not (Test-Path -LiteralPath (Join-Path $RuntimeDir 'vulkan/ofxr_rife_vulkan.exe'))){throw 'The selected directory is not a Radeon Vulkan runtime.'}
}else{
 if($native -or -not (Test-Path -LiteralPath (Join-Path $RuntimeDir 'app/worker.py'))){throw 'The selected directory is not an NVIDIA CUDA runtime. Use an explicit saved NVIDIA runtime path.'}
}
if(-not $AllowRunningTray -and (Get-Process -Name OFXRBridgeTray -ErrorAction SilentlyContinue)){
 throw 'Close the OFXR tray before switching. Restart the matching tray and game afterwards. No process was stopped.'
}
$SettingsPath=[IO.Path]::GetFullPath($SettingsPath)
$utf8=[Text.UTF8Encoding]::new($false,$true)
$before=if(Test-Path -LiteralPath $SettingsPath){[IO.File]::ReadAllBytes($SettingsPath)}else{[byte[]]@()}
$text=$utf8.GetString($before).TrimStart([char]0xFEFF)
$lines=$text -split '\r?\n'
$output=[Collections.Generic.List[string]]::new()
$inTray=$false;$seenTray=$false;$written=$false
$value=if($RuntimeDir -eq $managed){''}else{$RuntimeDir.Replace('\','/')}
foreach($line in $lines){
 if($line -match '^\s*\[([^\]]+)\]\s*$'){
  if($inTray -and -not $written -and $value){$output.Add('rife_runtime_dir='+$value);$written=$true}
  $inTray=$Matches[1] -ieq 'tray'
  if($inTray){if($seenTray){throw 'Duplicate tray sections; the original settings were preserved.'};$seenTray=$true}
 }
 if($inTray -and $line -match '^\s*rife_runtime_dir\s*='){
  if(-not $written -and $value){$output.Add('rife_runtime_dir='+$value)}
  $written=$true
  continue
 }
 $output.Add($line)
}
if(-not $seenTray -and $value){$output.Add('[tray]');$output.Add('rife_runtime_dir='+$value)}
elseif($inTray -and -not $written -and $value){$output.Add('rife_runtime_dir='+$value)}
$updated=($output -join "`n").TrimEnd("`n")+"`n"
$parent=[IO.Path]::GetDirectoryName($SettingsPath)
[IO.Directory]::CreateDirectory($parent)|Out-Null
$status=Join-Path $parent 'RIFE/status/switch-backups'
[IO.Directory]::CreateDirectory($status)|Out-Null
$token=[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N')
$backup=Join-Path $status ($token+'.ini')
$temporary=Join-Path $parent ('.rife-select-'+[Guid]::NewGuid().ToString('N')+'.tmp')
[IO.File]::WriteAllText($temporary,$updated,$utf8)
try{
 if(Test-Path -LiteralPath $SettingsPath){
  # Detect changes since reading; replacing a stale whole INI could lose settings.
  $current=[IO.File]::ReadAllBytes($SettingsPath)
  if([Convert]::ToBase64String($current) -ne [Convert]::ToBase64String($before)){throw 'Settings changed while preparing the switch; retry after closing the tray.'}
  [IO.File]::Replace($temporary,$SettingsPath,$backup)
 }else{[IO.File]::Move($temporary,$SettingsPath)}
}finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
$result=[ordered]@{schema=1;backend=$Backend;runtime=$RuntimeDir;managed_default=($value -eq '');settings=$SettingsPath;backup=$backup;restart_required=$true}
if($ResultJson){[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($ResultJson)))|Out-Null;[IO.File]::WriteAllText([IO.Path]::GetFullPath($ResultJson),($result|ConvertTo-Json),$utf8)}
Write-Host ('Selected RIFE '+$Backend+': '+$RuntimeDir)
Write-Host 'Restart the matching OFXR tray and VR game to use the selected runtime.'
