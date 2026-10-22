#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Capture')][string]$Action = 'Plan',
    [Parameter(Mandatory)][ValidateRange(1, 2147483647)][int]$TargetProcessId,
    [Parameter(Mandatory)][string]$PresentMonPath,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(5, 180)][int]$Seconds = 30,
    [string]$KernelRecorderPath = (Join-Path $PSScriptRoot '../../.codex-build-logs/ui-perf-tools/bin/KernelTraceCapture.exe'),
    [string]$TracePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-TargetIdentity {
    $targetProcess = Get-Process -Id $TargetProcessId -ErrorAction Stop
    try {
        $image = $targetProcess.MainModule.FileName
        if (!$image) { throw 'Target image path is unavailable.' }
        [ordered]@{
            pid = $TargetProcessId
            image_path = $image
            exe_sha256 = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant()
            creation_time_utc = $targetProcess.StartTime.ToUniversalTime().ToString('o')
        }
    } finally { $targetProcess.Dispose() }
}

function Start-OwnedChild([string]$Executable, [string[]]$Arguments, [string]$LogPrefix) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $startInfo.ArgumentList.Add($argument) }
    $child = [Diagnostics.Process]::new()
    $child.StartInfo = $startInfo
    if (!$child.Start()) { $child.Dispose(); throw "Could not start $Executable" }
    # Drain both pipes immediately; neither child can block on a full console pipe.
    [pscustomobject]@{
        Process = $child
        StandardOutput = $child.StandardOutput.ReadToEndAsync()
        StandardError = $child.StandardError.ReadToEndAsync()
        LogPrefix = $LogPrefix
    }
}

function Save-ChildLogs($Child) {
    $Child.StandardOutput.GetAwaiter().GetResult() | Set-Content -LiteralPath ($Child.LogPrefix + '.stdout.txt') -Encoding utf8
    $Child.StandardError.GetAwaiter().GetResult() | Set-Content -LiteralPath ($Child.LogPrefix + '.stderr.txt') -Encoding utf8
}

$presentMon = (Resolve-Path -LiteralPath $PresentMonPath).Path
$kernelRecorder = (Resolve-Path -LiteralPath $KernelRecorderPath).Path
if ($TracePath) { $TracePath = (Resolve-Path -LiteralPath $TracePath).Path }
$root = [IO.Path]::GetFullPath($OutputDirectory)
$captureId = [guid]::NewGuid().ToString()
$captureRoot = Join-Path $root $captureId
$presentMonCsv = Join-Path $captureRoot 'presentmon.csv'
$kernelEtl = Join-Path $captureRoot 'system.etl'
$kernelReceipt = Join-Path $captureRoot 'kernel-receipt.json'
$kernelReady = Join-Path $captureRoot 'kernel-ready.json'
$target = Read-TargetIdentity
$presentMonArguments = @('--process_id', "$TargetProcessId", '--qpc_time', '--v2_metrics',
    '--write_display_metadata', '--output_file', $presentMonCsv, '--no_console_stats',
    '--session_name', "KSwordUI-Present-$captureId", '--timed', "$Seconds", '--terminate_after_timed')
$kernelArguments = @('--capture', '--seconds', "$($Seconds + 5)", '--owner-pid', "$PID",
    '--output', $kernelEtl, '--receipt', $kernelReceipt, '--ready', $kernelReady)
$plan = [ordered]@{
    action = $Action; capture_id = $captureId; directory = $captureRoot; target = $target
    presentmon = @{ executable = $presentMon; arguments = $presentMonArguments }
    kernel = @{ executable = $kernelRecorder; arguments = $kernelArguments }
    trace_source = $TracePath
    scope = 'Bounded system CPU/context-switch stacks plus target PresentMon; no KSword launch, stop, input injection or settings changes.'
}
if ($Action -eq 'Plan') { $plan | ConvertTo-Json -Depth 8; return }

if (Test-Path -LiteralPath $captureRoot) { throw "Capture directory already exists: $captureRoot" }
New-Item -ItemType Directory -Path $captureRoot | Out-Null
$plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $captureRoot 'plan.json') -Encoding utf8
$manifest = [ordered]@{
    schema_version = 1; capture_id = $captureId; target = $target
    qpc_frequency = [Diagnostics.Stopwatch]::Frequency.ToString([Globalization.CultureInfo]::InvariantCulture)
    started_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString([Globalization.CultureInfo]::InvariantCulture)
    stopped_qpc = '0'; started_utc = [DateTime]::UtcNow.ToString('o'); stopped_utc = ''
    files = [ordered]@{}; capture_complete = $false
    tools = @{
        presentmon_sha256 = (Get-FileHash -LiteralPath $presentMon -Algorithm SHA256).Hash.ToLowerInvariant()
        kernel_sha256 = (Get-FileHash -LiteralPath $kernelRecorder -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    errors = @(); physical_photons_measured = $false; causal_action_completion_verified = $false
}
$kernel = $null
$present = $null
$captureFailure = $null
try {
    $kernel = Start-OwnedChild $kernelRecorder $kernelArguments (Join-Path $captureRoot 'kernel')
    $readyTimeout = [Diagnostics.Stopwatch]::StartNew()
    while (!(Test-Path -LiteralPath $kernelReady)) {
        if ($kernel.Process.HasExited) { throw 'Kernel recorder exited before ready; see kernel.stderr.txt.' }
        if ($readyTimeout.Elapsed.TotalSeconds -gt 20) { throw 'Kernel recorder did not become ready within 20 seconds.' }
        Start-Sleep -Milliseconds 50
    }
    $readyValue = Get-Content -LiteralPath $kernelReady -Raw | ConvertFrom-Json
    if (!$readyValue.ready -or $readyValue.qpc_frequency -ne $manifest.qpc_frequency) {
        throw 'Kernel recorder clock/ready mismatch.'
    }
    $present = Start-OwnedChild $presentMon $presentMonArguments (Join-Path $captureRoot 'presentmon')
    Write-Host "Capture started for PID $TargetProcessId, $Seconds seconds. Reproduce the UI operation manually."
    if (!$present.Process.WaitForExit(($Seconds + 20) * 1000)) {
        # Only the exact PresentMon child created here, never another trace or KSword process.
        $present.Process.Kill()
        $present.Process.WaitForExit()
        throw 'PresentMon exceeded its bounded capture time; capture is incomplete.'
    }
    if ($present.Process.ExitCode -ne 0) { throw "PresentMon failed: $($present.Process.ExitCode)." }
    if (!$kernel.Process.WaitForExit(($Seconds + 10) * 1000)) {
        throw 'Owned kernel recorder has not stopped; it has its own timeout and owner-process exit guard.'
    }
    if ($kernel.Process.ExitCode -ne 0) { throw "Kernel recorder failed/incomplete: $($kernel.Process.ExitCode)." }
    $receiptValue = Get-Content -LiteralPath $kernelReceipt -Raw | ConvertFrom-Json
    if (!$receiptValue.complete -or $receiptValue.qpc_frequency -ne $manifest.qpc_frequency) {
        throw 'Kernel capture receipt is incomplete or has a different clock.'
    }
    foreach ($artifact in @($presentMonCsv, $kernelEtl)) {
        if (!(Test-Path -LiteralPath $artifact -PathType Leaf) -or (Get-Item -LiteralPath $artifact).Length -eq 0) {
            throw "Capture artifact missing/empty: $artifact"
        }
    }
    $after = Read-TargetIdentity
    if ($target.creation_time_utc -ne $after.creation_time_utc -or $target.image_path -ne $after.image_path -or
        $target.exe_sha256 -ne $after.exe_sha256) { throw 'Target process/build identity changed during capture.' }
    $manifest.capture_complete = $true
} catch {
    $captureFailure = $_
    $manifest.errors += $_.Exception.Message
} finally {
    # No termination of a kernel recorder: allow its bounded stop/receipt path to finish.
    if ($kernel -and !$kernel.Process.HasExited) {
        if (!$kernel.Process.WaitForExit(($Seconds + 10) * 1000)) {
            $manifest.capture_complete = $false
            $manifest.errors += 'Kernel recorder did not finish; verify its printed unique session name before manual cleanup.'
        }
    }
    foreach ($child in @($present, $kernel)) {
        if (!$child) { continue }
        if ($child.Process.HasExited) { Save-ChildLogs $child }
        $child.Process.Dispose()
    }
    $manifest.stopped_qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString([Globalization.CultureInfo]::InvariantCulture)
    $manifest.stopped_utc = [DateTime]::UtcNow.ToString('o')
    if ($TracePath) {
        try {
            $traceCopy = Join-Path $captureRoot 'perf-trace.jsonl'
            # A running diagnostic writer may still append: snapshot identity is hashed,
            # but a snapshot is deliberately not advertised as a completed writer session.
            $source = [IO.File]::Open($TracePath, 'Open', 'Read', 'ReadWrite, Delete')
            try {
                $destination = [IO.File]::Open($traceCopy, 'CreateNew', 'Write', 'Read')
                try { $source.CopyTo($destination) } finally { $destination.Dispose() }
            } finally { $source.Dispose() }
        } catch { $manifest.errors += "Trace snapshot failed: $($_.Exception.Message)" }
    }
    $roles = [ordered]@{
        presentmon_csv = $presentMonCsv; system_etl = $kernelEtl
        kernel_receipt = $kernelReceipt; trace_jsonl = (Join-Path $captureRoot 'perf-trace.jsonl')
    }
    foreach ($entry in $roles.GetEnumerator()) {
        if (Test-Path -LiteralPath $entry.Value -PathType Leaf) {
            $manifest.files[$entry.Key] = @{
                path = [IO.Path]::GetFileName($entry.Value)
                sha256 = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
    }
    $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $captureRoot 'capture.json') -Encoding utf8
}
Write-Host "Capture manifest: $(Join-Path $captureRoot 'capture.json')"
if ($captureFailure) { throw $captureFailure }
if (!$manifest.capture_complete) { throw 'Capture incomplete; inspect capture.json and child logs.' }
