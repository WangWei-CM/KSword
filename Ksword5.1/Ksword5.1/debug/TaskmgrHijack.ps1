#requires -Version 5.1

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ifeoKeyPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe"
$debuggerValueName = "Debugger"

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Install-TaskmgrHijack {
    $exePath = Join-Path -Path (Get-Location).Path -ChildPath "Ksword5.1.exe"
    if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
        throw "Ksword5.1.exe was not found in the current directory: $((Get-Location).Path)"
    }

    $debuggerValue = '"' + $exePath + '"'
    New-Item -Path $ifeoKeyPath -Force | Out-Null
    New-ItemProperty -Path $ifeoKeyPath -Name $debuggerValueName -Value $debuggerValue -PropertyType String -Force | Out-Null

    Write-Host "Install complete."
    Write-Host "taskmgr.exe IFEO Debugger -> $debuggerValue"
}

function Uninstall-TaskmgrHijack {
    if (-not (Test-Path -LiteralPath $ifeoKeyPath)) {
        Write-Host "No IFEO key found for taskmgr.exe."
        return
    }

    Remove-ItemProperty -Path $ifeoKeyPath -Name $debuggerValueName -ErrorAction SilentlyContinue

    $remainingValues = (Get-Item -LiteralPath $ifeoKeyPath).Property
    if (-not $remainingValues -or $remainingValues.Count -eq 0) {
        Remove-Item -LiteralPath $ifeoKeyPath -Force
    }

    Write-Host "Uninstall complete."
    Write-Host "taskmgr.exe IFEO Debugger removed."
}

if (-not (Test-Administrator)) {
    Write-Error "Run this script as Administrator."
    exit 1
}

Write-Host "Select action:"
Write-Host "  [I] Install"
Write-Host "  [U] Uninstall"
$choice = (Read-Host "Enter I or U").Trim().ToUpperInvariant()

switch ($choice) {
    "I" { Install-TaskmgrHijack }
    "U" { Uninstall-TaskmgrHijack }
    default {
        Write-Error "Invalid selection: $choice"
        exit 1
    }
}
