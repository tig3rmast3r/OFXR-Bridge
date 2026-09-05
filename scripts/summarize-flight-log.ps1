param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,

    [ValidateRange(1, 100)]
    [int]$Top = 20
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Average {
    param([object[]]$Values)
    [Math]::Round(($Values | Measure-Object -Average).Average, 1)
}

$resolved = (Resolve-Path -LiteralPath $LogPath).Path
$pending = @{}
$completed = [System.Collections.Generic.List[object]]::new()
$failures = [System.Collections.Generic.List[object]]::new()
$decisions = [System.Collections.Generic.List[object]]::new()
$nvidiaStages = [System.Collections.Generic.List[object]]::new()
$nvidiaTotals = [System.Collections.Generic.List[object]]::new()
$lineNumber = 0

Get-Content -LiteralPath $resolved | ForEach-Object {
    ++$lineNumber
    $line = $_
    if ($line -notmatch
        '^seq=(\d+) ms=([0-9.]+) tid=(\d+) phase=([BEI]) op=([^ ]+) result=(-?\d+) dur_us=(\d+) a=(\d+) b=(\d+) c=(\d+)') {
        return
    }

    $sequence = [UInt64]$Matches[1]
    $entry = [pscustomobject]@{
        Sequence = $sequence
        Milliseconds = [double]$Matches[2]
        Thread = [UInt32]$Matches[3]
        Phase = $Matches[4]
        Operation = $Matches[5]
        Result = [Int64]$Matches[6]
        DurationUs = [UInt64]$Matches[7]
        A = [UInt64]$Matches[8]
        B = [UInt64]$Matches[9]
        C = [UInt64]$Matches[10]
        LineNumber = $lineNumber
        Line = $line
    }

    switch ($entry.Phase) {
        'B' { $pending[$sequence] = $entry }
        'E' {
            [void]$pending.Remove($sequence)
            $completed.Add($entry)
            if ($entry.Result -lt 0) {
                $failures.Add($entry)
            }
        }
        'I' {
            if ($entry.Result -lt 0) {
                $failures.Add($entry)
            }
            if ($entry.Operation -in @(
                    'swapchain_eligibility',
                    'projection_mapping',
                    'generation_prepare')) {
                $decisions.Add($entry)
            }
            if ($entry.Operation -eq 'nvidia_gpu_stages' -and
                $entry.Result -ge 0) {
                $nvidiaStages.Add($entry)
            }
            if ($entry.Operation -eq 'nvidia_gpu_total' -and
                $entry.Result -eq 0) {
                $nvidiaTotals.Add($entry)
            }
        }
    }
}

Write-Output "OFXR flight log: $resolved"
Write-Output "Completed boundaries: $($completed.Count)"
Write-Output "Failed boundaries/events: $($failures.Count)"
Write-Output "Unmatched BEGIN boundaries: $($pending.Count)"

if ($pending.Count -gt 0) {
    Write-Output ''
    Write-Output 'Unmatched BEGIN boundaries (probable hang location):'
    $pending.Values |
        Sort-Object Sequence |
        Select-Object Sequence, Milliseconds, Thread, Operation, LineNumber, Line |
        Format-Table -AutoSize -Wrap
}

if ($failures.Count -gt 0) {
    Write-Output ''
    Write-Output 'Failures:'
    $failures |
        Select-Object Sequence, Milliseconds, Thread, Operation, Result,
            DurationUs, LineNumber |
        Format-Table -AutoSize
}

if ($decisions.Count -gt 0) {
    Write-Output ''
    Write-Output 'Eligibility decisions:'
    $decisions |
        Group-Object Operation, Result |
        Sort-Object Name |
        Select-Object Count, Name |
        Format-Table -AutoSize
}

if ($nvidiaStages.Count -gt 0 -and $nvidiaTotals.Count -gt 0) {
    Write-Output ''
    Write-Output "NVIDIA GPU timings ($($nvidiaTotals.Count) completed pairs, microseconds):"
    [pscustomobject]@{
        PackAvg = Get-Average -Values @($nvidiaStages | ForEach-Object A)
        Eye0Avg = Get-Average -Values @($nvidiaStages | ForEach-Object B)
        Eye1Avg = Get-Average -Values @($nvidiaStages | ForEach-Object C)
        CompositionAvg = Get-Average -Values @($nvidiaTotals | ForEach-Object A)
        TotalAvg = Get-Average -Values @($nvidiaTotals | ForEach-Object B)
        TotalMax = ($nvidiaTotals | Measure-Object B -Maximum).Maximum
    } | Format-Table -AutoSize
}

Write-Output ''
Write-Output "Slowest $Top completed boundaries:"
$completed |
    Sort-Object DurationUs -Descending |
    Select-Object -First $Top Sequence, Milliseconds, Thread, Operation,
        Result, DurationUs, LineNumber |
    Format-Table -AutoSize
