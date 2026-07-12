<#
.SYNOPSIS
    Applies the KswordARK driver post-build signing chain.

.DESCRIPTION
    The driver build leaves an unsigned KswordARK.sys at the MSBuild target
    path.  This script signs that file in-place with .cert\CSignTool.exe first,
    immediately runs CSignTool a second time with /ac to add the Microsoft
    kernel-mode cross-certificate, then uses .cert\AuthenticodeVariantGUI.exe to
    generate the final Authenticode variant.  The final file replaces the
    original driver path.

    The script intentionally keeps all repository-relative tool paths anchored
    to the repository root, so Visual Studio/MSBuild can call it from any current
    directory.  CSignTool and AuthenticodeVariantGUI are launched with .cert as
    their working directory because both tools may load sibling data files.
#>

[CmdletBinding()]
param(
    # DriverPath is the generated KswordARK.sys path that must be modified in-place.
    [Parameter(Mandatory = $true)]
    [string] $DriverPath,

    # SignToolPath optionally overrides the Windows SDK signtool.exe used by AuthenticodeVariantGUI.
    [string] $SignToolPath,

    # NonFatal lets exploratory/manual callers preserve a zero exit code while still logging failures.
    [switch] $NonFatal
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Resolve-RepoRoot:
# - Inputs: none.
# - Processing: walks from this script's directory to the repository root.
# - Returns: absolute repository root path.
function Resolve-RepoRoot {
    $scriptDirectory = Split-Path -Parent $PSCommandPath
    return (Resolve-Path -LiteralPath (Join-Path $scriptDirectory '..')).Path
}

# Resolve-RequiredFile:
# - Inputs: a path and a human-readable description.
# - Processing: verifies the path points to an existing file.
# - Returns: absolute file path when present; throws when missing.
function Resolve-RequiredFile {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description not found: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

# Resolve-DriverPath:
# - Inputs: a driver path that may be absolute or relative.
# - Processing: resolves relative paths from the repository root and validates it is a .sys file.
# - Returns: absolute driver file path.
function Resolve-DriverPath {
    param(
        [Parameter(Mandatory = $true)]
        [string] $RepoRoot,

        [Parameter(Mandatory = $true)]
        [string] $RequestedDriverPath
    )

    $candidate = if ([System.IO.Path]::IsPathRooted($RequestedDriverPath)) {
        $RequestedDriverPath
    }
    else {
        Join-Path $RepoRoot $RequestedDriverPath
    }

    $resolved = Resolve-RequiredFile -Path $candidate -Description 'Driver output'
    if ([System.IO.Path]::GetExtension($resolved) -ine '.sys') {
        throw "DriverPath must point to a .sys file: $resolved"
    }

    return $resolved
}

# Get-WindowsKitSignToolCandidates:
# - Inputs: none.
# - Processing: scans standard Windows Kits locations for x64 signtool.exe.
# - Returns: zero or more FileInfo objects sorted by path by the caller.
function Get-WindowsKitSignToolCandidates {
    $candidateRoots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) }

    foreach ($root in $candidateRoots) {
        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                $x64Tool = Join-Path $_.FullName 'x64\signtool.exe'
                if (Test-Path -LiteralPath $x64Tool -PathType Leaf) {
                    Get-Item -LiteralPath $x64Tool
                }
            }
    }
}

# Resolve-SignTool:
# - Inputs: optional explicit signtool path and the .cert directory.
# - Processing: prefers explicit/env Windows SDK tools, then PATH, then bundled fallback.
# - Returns: absolute signtool.exe path for AuthenticodeVariantGUI.
function Resolve-SignTool {
    param(
        [string] $ExplicitSignToolPath,

        [Parameter(Mandatory = $true)]
        [string] $CertificateDirectory
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitSignToolPath)) {
        return Resolve-RequiredFile -Path $ExplicitSignToolPath -Description 'Explicit signtool.exe'
    }

    if ($env:KSWORD_SIGNTOOL) {
        return Resolve-RequiredFile -Path $env:KSWORD_SIGNTOOL -Description 'KSWORD_SIGNTOOL signtool.exe'
    }

    $windowsKitTool = Get-WindowsKitSignToolCandidates |
        Sort-Object -Property FullName -Descending |
        Select-Object -First 1
    if ($windowsKitTool) {
        return $windowsKitTool.FullName
    }

    $pathCommand = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($pathCommand -and (Test-Path -LiteralPath $pathCommand.Source -PathType Leaf)) {
        return $pathCommand.Source
    }

    $bundledTool = Join-Path $CertificateDirectory 'signtool.exe'
    return Resolve-RequiredFile -Path $bundledTool -Description 'Bundled signtool.exe'
}

# Invoke-NativeTool:
# - Inputs: executable path, argument array, and working directory.
# - Processing: runs the native process, mirrors output, and checks the exit code.
# - Returns: no value; throws on non-zero exit code.
function Invoke-NativeTool {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [Parameter(Mandatory = $true)]
        [string[]] $Arguments,

        [Parameter(Mandatory = $true)]
        [string] $WorkingDirectory
    )

    $previousLocation = Get-Location
    try {
        Set-Location -LiteralPath $WorkingDirectory
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        Set-Location -LiteralPath $previousLocation
    }

    $output | ForEach-Object { Write-Host $_ }
    if ($exitCode -ne 0) {
        $joinedArguments = $Arguments -join ' '
        throw "Command failed with exit code $exitCode`: $FilePath $joinedArguments"
    }
}

# Write-SignatureSummary:
# - Inputs: a label and a file path.
# - Processing: reads the Authenticode signer with PowerShell's built-in verifier.
# - Returns: no value; writes a compact status line for build logs.
function Write-SignatureSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Label,

        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    try {
        $signature = Get-AuthenticodeSignature -FilePath $Path
    }
    catch {
        Write-Host "$Label signature summary skipped: $($_.Exception.Message)"
        return
    }

    $subject = if ($null -ne $signature.SignerCertificate) {
        $signature.SignerCertificate.Subject
    }
    else {
        '<none>'
    }

    Write-Host "$Label signature status: $($signature.Status); subject: $subject"
}

# Invoke-LegacyDriverTestSign:
# - Inputs: repository root and the driver path restored to its pre-variant contents.
# - Processing: calls the pre-existing non-fatal test-signing script exactly as the old
#   MSBuild target did, so a variant-signing failure falls back to the earlier behavior.
# - Returns: no value; logs warnings instead of throwing when the legacy fallback cannot sign.
function Invoke-LegacyDriverTestSign {
    param(
        [Parameter(Mandatory = $true)]
        [string] $RepoRoot,

        [Parameter(Mandatory = $true)]
        [string] $DriverPath,

        [Parameter(Mandatory = $true)]
        [string] $Reason
    )

    $legacyScript = Join-Path $RepoRoot 'scripts\Sign-KswordArkDriverTest.ps1'
    Write-Warning "KswordARK variant signing failed; falling back to legacy non-fatal test signing. Reason: $Reason"

    if (-not (Test-Path -LiteralPath $legacyScript -PathType Leaf)) {
        Write-Warning "Legacy test-signing script was not found: $legacyScript"
        return
    }

    $output = & powershell -ExecutionPolicy Bypass -File $legacyScript -DriverPath $DriverPath -NonFatal 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }

    if ($exitCode -ne 0) {
        Write-Warning "Legacy test-signing fallback exited with code $exitCode. The old MSBuild path was non-fatal, so the build will continue."
    }
}

# Invoke-KswordDriverVariantSign:
# - Inputs: the generated driver path and optional signtool override.
# - Processing: runs CSignTool once to write the embedded signature, runs
#   CSignTool again with /ac to add the driver cross-certificate to the existing
#   signature, then runs AuthenticodeVariantGUI into a temporary output and
#   atomically replaces the original driver file with the generated variant.
#   If any variant-signing step fails, restores the pre-signing file and falls
#   back to scripts\Sign-KswordArkDriverTest.ps1 -NonFatal.
# - Returns: no value; final driver path contains either the Authenticode variant or the legacy test signature.
function Invoke-KswordDriverVariantSign {
    param(
        [Parameter(Mandatory = $true)]
        [string] $RequestedDriverPath,

        [string] $RequestedSignToolPath
    )

    $repoRoot = Resolve-RepoRoot
    $driver = Resolve-DriverPath -RepoRoot $repoRoot -RequestedDriverPath $RequestedDriverPath
    $certificateDirectory = Join-Path $repoRoot '.cert'
    $driverDirectory = Split-Path -Parent $driver
    $driverBaseName = [System.IO.Path]::GetFileNameWithoutExtension($driver)
    $driverExtension = [System.IO.Path]::GetExtension($driver)
    $temporaryOutput = Join-Path $driverDirectory "$driverBaseName.authvariant.tmp$driverExtension"
    $preVariantBackup = Join-Path $driverDirectory "$driverBaseName.prevariant.tmp$driverExtension"

    if (Test-Path -LiteralPath $temporaryOutput) {
        Remove-Item -LiteralPath $temporaryOutput -Force
    }
    if (Test-Path -LiteralPath $preVariantBackup) {
        Remove-Item -LiteralPath $preVariantBackup -Force
    }

    try {
        Copy-Item -LiteralPath $driver -Destination $preVariantBackup -Force

        $csignTool = Resolve-RequiredFile -Path (Join-Path $certificateDirectory 'CSignTool.exe') -Description 'CSignTool.exe'
        $authVariantTool = Resolve-RequiredFile -Path (Join-Path $certificateDirectory 'AuthenticodeVariantGUI.exe') -Description 'AuthenticodeVariantGUI.exe'
        $displayInfo = Resolve-RequiredFile -Path (Join-Path $certificateDirectory 'outer_display_info.dat') -Description 'outer_display_info.dat'
        $resolvedSignTool = Resolve-SignTool -ExplicitSignToolPath $RequestedSignToolPath -CertificateDirectory $certificateDirectory

        Write-Host "KswordARK driver signing root: $repoRoot"
        Write-Host "Driver output: $driver"
        Write-Host "CSignTool: $csignTool"
        Write-Host "AuthenticodeVariantGUI: $authVariantTool"
        Write-Host "Display info: $displayInfo"
        Write-Host "signtool: $resolvedSignTool"

        Invoke-NativeTool `
            -FilePath $csignTool `
            -Arguments @('sign', '/r', '1', '/f', $driver) `
            -WorkingDirectory $certificateDirectory
        Write-SignatureSummary -Label 'After CSignTool' -Path $driver

        # CSignTool /ac:
        # - Inputs: the driver path after the first CSignTool signing pass.
        # - Processing: appends the Microsoft Code Verification Root cross-certificate
        #   used by legacy kernel-mode driver signing.  Direct /ac on an unsigned file
        #   has been observed to hang and leave the file unsigned, so keep this as a
        #   strict second pass after the ordinary embedded signature exists.
        # - Returns: no script value; the driver is modified in place.
        Invoke-NativeTool `
            -FilePath $csignTool `
            -Arguments @('sign', '/r', '1', '/f', $driver, '/ac') `
            -WorkingDirectory $certificateDirectory
        Write-SignatureSummary -Label 'After CSignTool /ac' -Path $driver

        Invoke-NativeTool `
            -FilePath $authVariantTool `
            -Arguments @('generate', '--source', $driver, '--output', $temporaryOutput, '--display-file', $displayInfo, '--signtool', $resolvedSignTool) `
            -WorkingDirectory $certificateDirectory

        if (-not (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)) {
            throw "AuthenticodeVariantGUI did not create expected output: $temporaryOutput"
        }

        Move-Item -LiteralPath $temporaryOutput -Destination $driver -Force
        Write-SignatureSummary -Label 'Final' -Path $driver
    }
    catch {
        $failureMessage = $_.Exception.Message
        if (Test-Path -LiteralPath $preVariantBackup -PathType Leaf) {
            Copy-Item -LiteralPath $preVariantBackup -Destination $driver -Force
            Write-Host "Restored pre-variant driver before legacy fallback: $driver"
        }

        Invoke-LegacyDriverTestSign -RepoRoot $repoRoot -DriverPath $driver -Reason $failureMessage
    }
    finally {
        if (Test-Path -LiteralPath $temporaryOutput) {
            Remove-Item -LiteralPath $temporaryOutput -Force
        }
        if (Test-Path -LiteralPath $preVariantBackup) {
            Remove-Item -LiteralPath $preVariantBackup -Force
        }
    }
}

try {
    Invoke-KswordDriverVariantSign -RequestedDriverPath $DriverPath -RequestedSignToolPath $SignToolPath
}
catch {
    if ($NonFatal) {
        Write-Warning "KswordARK driver variant signing failed, but NonFatal was requested: $($_.Exception.Message)"
        exit 0
    }

    throw
}
