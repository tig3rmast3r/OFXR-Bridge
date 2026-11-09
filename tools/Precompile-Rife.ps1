param(
 [string]$Root=(Join-Path $env:LOCALAPPDATA 'ofxr-bridge/RIFE/runtime'),
 [Parameter(Mandatory=$true)][string]$ResultJson,
 [string]$Games
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$Root=[IO.Path]::GetFullPath($Root)
$ResultJson=[IO.Path]::GetFullPath($ResultJson)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($ResultJson))|Out-Null
$progressPath=$ResultJson+'.progress.json'
$cancelPath=$ResultJson+'.cancel'
$logPath=$ResultJson+'.log'
$form=New-Object Windows.Forms.Form
$form.Text=if(Test-Path -LiteralPath (Join-Path $Root 'vulkan/backend.json')){'OFXR - Prepare Vulkan for found games'}else{'OFXR 0.3.3 - Pre-compile TensorRT for found games'}
$form.ClientSize=New-Object Drawing.Size(560,164)
$form.StartPosition='CenterScreen';$form.FormBorderStyle='FixedDialog';$form.MaximizeBox=$false;$form.MinimizeBox=$true
$label=New-Object Windows.Forms.Label
$label.Location=New-Object Drawing.Point(16,16);$label.Size=New-Object Drawing.Size(528,50)
$label.Text='Preparing recorded game resolutions...';$form.Controls.Add($label)
$bar=New-Object Windows.Forms.ProgressBar
$bar.Location=New-Object Drawing.Point(16,76);$bar.Size=New-Object Drawing.Size(528,24)
$form.Controls.Add($bar)
$button=New-Object Windows.Forms.Button
$button.Text='Cancel';$button.Location=New-Object Drawing.Point(444,120);$button.Size=New-Object Drawing.Size(100,28)
$form.Controls.Add($button)
$state=@{done=$false;cancelled=$false}
$cancel={
 if($state.done){$form.Close();return}
 $state.cancelled=$true
 [IO.File]::WriteAllText($cancelPath,'cancel')
 $button.Enabled=$false;$label.Text='Cancelling the current compilation...'
}
$button.Add_Click($cancel)
$form.Add_FormClosing({param($sender,$event)
 if(-not $state.done){$event.Cancel=$true;& $cancel}
})
function Quote-Argument([string]$Value){
 if($Value.Contains('"')){throw 'Invalid quote in pre-compile argument.'}
 return '"'+[regex]::Replace($Value,'(\\+)$','$1$1')+'"'
}
function Read-PrecompileJson([string]$Path){
 # The writer publishes by atomic rename. Reading must also allow deletion,
 # otherwise our own progress refresh can deny that rename on Windows.
 $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
 try {
  $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true)
  try {$text=$reader.ReadToEnd()}finally{$reader.Dispose()}
 }finally{$stream.Dispose()}
 # Close the handle before parsing or updating any controls.
 return ($text|ConvertFrom-Json)
}
$process=$null;$exitCode=1
try {
 $python=Join-Path $Root 'python/python.exe'
 if(-not (Test-Path -LiteralPath $python)){throw 'RIFE is not installed. Run Check RIFE first.'}
 $script=Join-Path $PSScriptRoot 'precompile_rife.py'
 $start=New-Object Diagnostics.ProcessStartInfo
 $start.FileName=$python
 $arguments=@('-I','-B',$script,'--root',$Root,'--result',$ResultJson,'--progress',$progressPath,'--cancel',$cancelPath,'--log',$logPath,'--parent-watchdog')
 if($Games){$arguments+=@('--games',[IO.Path]::GetFullPath($Games))}
 $start.Arguments=(($arguments|ForEach-Object{Quote-Argument $_}) -join ' ')
 $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardError=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardInput=$true
 $start.EnvironmentVariables.Clear()
 foreach($name in @('SystemRoot','WINDIR','LOCALAPPDATA','TEMP','TMP')){
  $value=[Environment]::GetEnvironmentVariable($name);if($value){$start.EnvironmentVariables[$name]=$value}
 }
 $start.EnvironmentVariables['PATH']=(Split-Path -Parent $python)+';'+(Join-Path $Root 'python/Lib/site-packages/torch/lib')+';'+(Join-Path $env:SystemRoot 'System32')
 $process=[Diagnostics.Process]::Start($start)
 $stderr=$process.StandardError.ReadToEndAsync();$stdout=$process.StandardOutput.ReadToEndAsync()
 $form.Show()
 while(-not $process.HasExited){
  [Windows.Forms.Application]::DoEvents()
  if((Test-Path -LiteralPath $progressPath) -and -not $state.cancelled){
   try {
    $p=Read-PrecompileJson $progressPath
    $bar.Maximum=[Math]::Max(1,[int]$p.total);$bar.Value=[Math]::Min($bar.Maximum,[int]$p.completed)
    $label.Text=('{0}/{1}   {2}' -f $p.current,$p.total,$p.message)
   }catch{} # atomic replacement may race one UI refresh
  }
  Start-Sleep -Milliseconds 100
 }
 $exitCode=$process.ExitCode
 $diagnostics=$stderr.Result+$stdout.Result
 if($diagnostics){[IO.File]::AppendAllText($logPath,$diagnostics)}
 if(-not (Test-Path -LiteralPath $ResultJson)){throw 'The pre-compiler exited without a result. See the retained log.'}
 $result=Read-PrecompileJson $ResultJson
 $label.Text=$result.message
 if($exitCode -eq 0){$bar.Value=$bar.Maximum}
}catch{
 $result=[ordered]@{schema=1;status='error';message=$_.Exception.Message;root=$Root;backend='NVIDIA / TensorRT';installed_bytes=0;required_free_bytes=0;log_path=$logPath}
 [IO.File]::WriteAllText($ResultJson,($result|ConvertTo-Json -Compress),[Text.UTF8Encoding]::new($false))
 $label.Text=$result.message
}finally{
 $state.done=$true;$button.Text='Close';$button.Enabled=$true
 if($process){$process.Dispose()}
 if(-not $form.Visible){$form.Show()}
 while($form.Visible){[Windows.Forms.Application]::DoEvents();Start-Sleep -Milliseconds 100}
 $form.Dispose()
}
exit $exitCode
