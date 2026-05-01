<#
.SYNOPSIS
    为 KswordARK.sys 生成/复用本机测试签名证书，并对开发输出目录中的驱动进行测试签名。

.DESCRIPTION
    Windows x64 的测试模式不是“完全放行未签名驱动”。
    即使已经执行 bcdedit /set testsigning on，内核仍然要求 .sys 至少带有测试签名，
    并且签名证书需要被本机信任。否则 StartServiceW/NtLoadDriver 仍会返回 577。

    本脚本默认处理仓库中常见的 KswordARK.sys 输出位置：
    - KswordARKDriver\x64\Release\KswordARK.sys
    - KswordARKDriver\x64\Debug\KswordARK.sys
    - Ksword5.1\x64\Release\KswordARK.sys
    - Ksword5.1\x64\Debug\KswordARK.sys
    - Ksword5.1\x64\Release\KswordARKDriver\KswordARK.sys
    - Ksword5.1\x64\Debug\KswordARKDriver\KswordARK.sys

    也可以通过 -TargetPath 显式指定主程序、DLL 或其它最终产物。
    该模式复用同一份 .cert\KswordARK-TestSigning.pfx/.cer，不会生成第二套签名证书。

    推荐用管理员 PowerShell 运行。管理员运行时会把测试证书导入 LocalMachine
    的 Root 和 TrustedPublisher；非管理员运行时只能导入 CurrentUser，文件会被签名，
    但内核加载仍可能因为机器级信任缺失而报 577。
#>

[CmdletBinding()]
param(
    # DriverPath：显式指定一个或多个 .sys 路径；不指定时自动签名仓库内已存在的 KswordARK.sys。
    [Parameter(ValueFromPipeline = $true, ValueFromPipelineByPropertyName = $true)]
    [string[]] $DriverPath,

    # TargetPath：显式指定一个或多个待签名文件；主程序最终签名通过该参数复用驱动测试证书。
    [string[]] $TargetPath,

    # Subject：测试证书主题；保持稳定可让后续构建复用同一证书。
    [string] $Subject = 'CN=KswordARK Test Signing Certificate',

    # PfxPassword：保护本地 .pfx 文件的密码；默认值仅用于本仓库开发测试证书。
    [string] $PfxPassword = 'KswordARK-TestSigning-LocalOnly',

    # EnableTestSigning：顺手打开 Windows 测试签名模式；需要管理员并且重启后生效。
    [switch] $EnableTestSigning,

    # SkipMachineTrust：即使当前是管理员也不导入 LocalMachine 信任区，仅用于排查证书污染。
    [switch] $SkipMachineTrust,

    # NonFatal：给 VS/MSBuild 后置构建使用；签名失败只输出警告并返回 0，保证编译不被签名环境阻断。
    [switch] $NonFatal
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1 在部分机器上创建文件型自签证书会走旧 CSP，并触发 NTE_NOT_FOUND。
# PowerShell 7 使用新版 .NET 证书 API 更稳定；如果可用，就自动转交给 pwsh 执行同一个脚本。
if ($PSVersionTable.PSVersion.Major -lt 7 -and -not $env:KSWORD_SIGN_SCRIPT_PWSH_BOOTSTRAPPED) {
    $pwshCommand = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if ($pwshCommand -and (Test-Path -LiteralPath $pwshCommand.Source)) {
        $env:KSWORD_SIGN_SCRIPT_PWSH_BOOTSTRAPPED = '1'
        $forwardArgs = @(
            '-NoProfile',
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            $PSCommandPath,
            '-Subject',
            $Subject,
            '-PfxPassword',
            $PfxPassword
        )
        if ($DriverPath) {
            foreach ($pathItem in $DriverPath) {
                $forwardArgs += @('-DriverPath', $pathItem)
            }
        }
        if ($TargetPath) {
            foreach ($pathItem in $TargetPath) {
                $forwardArgs += @('-TargetPath', $pathItem)
            }
        }
        if ($EnableTestSigning) {
            $forwardArgs += '-EnableTestSigning'
        }
        if ($SkipMachineTrust) {
            $forwardArgs += '-SkipMachineTrust'
        }
        if ($NonFatal) {
            $forwardArgs += '-NonFatal'
        }

        & $pwshCommand.Source @forwardArgs
        $exitCode = $LASTEXITCODE
        Remove-Item Env:\KSWORD_SIGN_SCRIPT_PWSH_BOOTSTRAPPED -ErrorAction SilentlyContinue
        exit $exitCode
    }

    Write-Warning '未找到 pwsh.exe，将继续使用 Windows PowerShell；如果证书生成失败，请安装 PowerShell 7 后重试。'
}

# Resolve-RepoRoot：
# - 输入：无；
# - 处理：从脚本路径向上定位仓库根目录；
# - 返回：仓库根目录的绝对路径。
function Resolve-RepoRoot {
    $scriptDirectory = Split-Path -Parent $PSCommandPath
    return (Resolve-Path (Join-Path $scriptDirectory '..')).Path
}

# Test-Admin：
# - 输入：无；
# - 处理：检查当前令牌是否属于 Administrators；
# - 返回：true 表示管理员 PowerShell，false 表示普通权限。
function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Find-SignTool：
# - 输入：无；
# - 处理：优先使用 KSWORD_SIGNTOOL/PATH，再扫描常见 Windows Kits 目录；
# - 返回：signtool.exe 的绝对路径。
function Find-SignTool {
    if ($env:KSWORD_SIGNTOOL -and (Test-Path -LiteralPath $env:KSWORD_SIGNTOOL)) {
        return (Resolve-Path -LiteralPath $env:KSWORD_SIGNTOOL).Path
    }

    $pathCommand = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($pathCommand -and (Test-Path -LiteralPath $pathCommand.Source)) {
        return $pathCommand.Source
    }

    $candidateRoots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin",
        'E:\Windows Kits\10\bin'
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    $candidateTools = foreach ($root in $candidateRoots) {
        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                $x64Tool = Join-Path $_.FullName 'x64\signtool.exe'
                $x86Tool = Join-Path $_.FullName 'x86\signtool.exe'
                if (Test-Path -LiteralPath $x64Tool) { Get-Item -LiteralPath $x64Tool }
                elseif (Test-Path -LiteralPath $x86Tool) { Get-Item -LiteralPath $x86Tool }
            }
    }

    $bestTool = $candidateTools | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $bestTool) {
        throw '未找到 signtool.exe。请安装 Windows SDK/WDK，或设置环境变量 KSWORD_SIGNTOOL。'
    }

    return $bestTool.FullName
}

# New-FileBackedCertificate：
# - 输入：证书主题、pfx 路径、cer 路径、pfx 密码；
# - 处理：用 .NET CertificateRequest 生成不依赖系统证书库的 Code Signing 证书；
# - 返回：带私钥的 X509Certificate2 对象。
function New-FileBackedCertificate {
    param(
        [Parameter(Mandatory = $true)]
        [string] $CertificateSubject,

        [Parameter(Mandatory = $true)]
        [string] $PfxPath,

        [Parameter(Mandatory = $true)]
        [string] $CerPath,

        [Parameter(Mandatory = $true)]
        [securestring] $SecurePassword
    )

    $rsaKey = [System.Security.Cryptography.RSA]::Create(2048)
    try {
        $hashAlgorithm = [System.Security.Cryptography.HashAlgorithmName]::SHA256
        $padding = [System.Security.Cryptography.RSASignaturePadding]::Pkcs1
        $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            $CertificateSubject,
            $rsaKey,
            $hashAlgorithm,
            $padding)

        $basicConstraints = [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new(
            $true,
            $false,
            0,
            $true)
        $request.CertificateExtensions.Add($basicConstraints)

        $keyUsage = [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature,
            $true)
        $request.CertificateExtensions.Add($keyUsage)

        $enhancedUsages = [System.Security.Cryptography.OidCollection]::new()
        [void]$enhancedUsages.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
        $enhancedKeyUsage = [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new(
            $enhancedUsages,
            $false)
        $request.CertificateExtensions.Add($enhancedKeyUsage)

        $notBefore = [System.DateTimeOffset]::Now.AddDays(-1)
        $notAfter = [System.DateTimeOffset]::Now.AddYears(10)
        $certificate = $request.CreateSelfSigned($notBefore, $notAfter)

        $pfxBytes = $certificate.Export(
            [System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx,
            $SecurePassword)
        [System.IO.File]::WriteAllBytes($PfxPath, $pfxBytes)

        $cerBytes = $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
        [System.IO.File]::WriteAllBytes($CerPath, $cerBytes)

        return [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
            $pfxBytes,
            $SecurePassword,
            [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    }
    finally {
        if ($rsaKey) {
            $rsaKey.Dispose()
        }
    }
}

# Get-OrCreate-TestCertificate：
# - 输入：证书主题、仓库根目录、pfx 密码；
# - 处理：优先复用 .cert\KswordARK-TestSigning.pfx，缺失或过期时重新生成；
# - 返回：包含证书对象、pfx 路径、cer 路径的 hashtable。
function Get-OrCreate-TestCertificate {
    param(
        [Parameter(Mandatory = $true)]
        [string] $CertificateSubject,

        [Parameter(Mandatory = $true)]
        [string] $RepoRoot,

        [Parameter(Mandatory = $true)]
        [string] $PlainPassword
    )

    $certificateDirectory = Join-Path $RepoRoot '.cert'
    New-Item -ItemType Directory -Path $certificateDirectory -Force | Out-Null

    $pfxPath = Join-Path $certificateDirectory 'KswordARK-TestSigning.pfx'
    $cerPath = Join-Path $certificateDirectory 'KswordARK-TestSigning.cer'
    $securePassword = ConvertTo-SecureString -String $PlainPassword -AsPlainText -Force

    if (Test-Path -LiteralPath $pfxPath) {
        try {
            $existingCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
                $pfxPath,
                $securePassword,
                [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
            if ($existingCertificate.Subject -eq $CertificateSubject -and
                $existingCertificate.NotAfter -gt (Get-Date).AddDays(30)) {
                if (-not (Test-Path -LiteralPath $cerPath)) {
                    $cerBytes = $existingCertificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
                    [System.IO.File]::WriteAllBytes($cerPath, $cerBytes)
                }
                Write-Host "复用文件型测试证书：$($existingCertificate.Thumbprint)"
                return @{
                    Certificate = $existingCertificate
                    PfxPath = $pfxPath
                    CerPath = $cerPath
                    Password = $PlainPassword
                }
            }
        }
        catch {
            Write-Warning "现有 PFX 无法复用，将重新生成：$($_.Exception.Message)"
        }
    }

    Write-Host "创建文件型测试证书：$CertificateSubject"
    $newCertificate = New-FileBackedCertificate `
        -CertificateSubject $CertificateSubject `
        -PfxPath $pfxPath `
        -CerPath $cerPath `
        -SecurePassword $securePassword
    Write-Host "PFX：$pfxPath"
    Write-Host "CER：$cerPath"

    return @{
        Certificate = $newCertificate
        PfxPath = $pfxPath
        CerPath = $cerPath
        Password = $PlainPassword
    }
}

# Import-TestCertificateTrust：
# - 输入：cer 路径、是否跳过机器信任；
# - 处理：导入 CurrentUser/LocalMachine 信任区；
# - 返回：无返回值。
function Import-TestCertificateTrust {
    param(
        [Parameter(Mandatory = $true)]
        [string] $CerPath,

        [Parameter(Mandatory = $true)]
        [bool] $SkipMachine
    )

    Write-Host "证书文件：$CerPath"
    try {
        Import-Certificate -FilePath $CerPath -CertStoreLocation Cert:\CurrentUser\Root | Out-Null
        Import-Certificate -FilePath $CerPath -CertStoreLocation Cert:\CurrentUser\TrustedPublisher | Out-Null
        Write-Host '已导入 CurrentUser\Root 与 CurrentUser\TrustedPublisher。'
    }
    catch {
        Write-Warning "导入 CurrentUser 证书信任失败：$($_.Exception.Message)"
    }

    if ((Test-Admin) -and -not $SkipMachine) {
        Import-Certificate -FilePath $CerPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Import-Certificate -FilePath $CerPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
        Write-Host '已导入 LocalMachine\Root 与 LocalMachine\TrustedPublisher。'
        return
    }

    if (-not $SkipMachine) {
        Write-Warning '当前不是管理员：无法导入 LocalMachine 信任区。内核加载可能仍然报 577。'
        Write-Warning '请用管理员 PowerShell 重新运行本脚本，或手工把 .cert\KswordARK-TestSigning.cer 导入“本地计算机\受信任的根证书颁发机构”和“受信任的发布者”。'
    }
}

# Resolve-SignTargets：
# - 输入：仓库根目录、显式最终产物路径、兼容旧参数的驱动路径；
# - 处理：优先归一化显式路径；未提供显式路径时回落到默认驱动输出扫描；
# - 返回：去重后的签名目标绝对路径数组。
function Resolve-SignTargets {
    param(
        [Parameter(Mandatory = $true)]
        [string] $RepoRoot,

        [string[]] $ExplicitTargetPaths,

        [string[]] $ExplicitDriverPaths
    )

    $combinedExplicitPaths = @()
    if ($ExplicitTargetPaths) {
        $combinedExplicitPaths += $ExplicitTargetPaths
    }
    if ($ExplicitDriverPaths) {
        $combinedExplicitPaths += $ExplicitDriverPaths
    }
    $combinedExplicitPaths = @($combinedExplicitPaths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($combinedExplicitPaths.Count -gt 0) {
        return Resolve-DriverTargets -RepoRoot $RepoRoot -ExplicitPaths $combinedExplicitPaths
    }

    return Resolve-DriverTargets -RepoRoot $RepoRoot
}

# Resolve-DriverTargets：
# - 输入：仓库根目录与用户显式路径；
# - 处理：显式路径优先，否则收集仓库内常见输出路径；
# - 返回：去重后的驱动绝对路径数组。
function Resolve-DriverTargets {
    param(
        [Parameter(Mandatory = $true)]
        [string] $RepoRoot,

        [string[]] $ExplicitPaths
    )

    $explicitPathList = @($ExplicitPaths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($explicitPathList.Count -gt 0) {
        return $explicitPathList |
            ForEach-Object {
                if ([System.IO.Path]::IsPathRooted($_)) { $_ }
                else { Join-Path $RepoRoot $_ }
            } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            ForEach-Object { (Resolve-Path -LiteralPath $_).Path } |
            Sort-Object -Unique
    }

    $relativeCandidates = @(
        'KswordARKDriver\x64\Release\KswordARK.sys',
        'KswordARKDriver\x64\Debug\KswordARK.sys',
        'Ksword5.1\x64\Release\KswordARK.sys',
        'Ksword5.1\x64\Debug\KswordARK.sys',
        'Ksword5.1\x64\Release\KswordARKDriver\KswordARK.sys',
        'Ksword5.1\x64\Debug\KswordARKDriver\KswordARK.sys'
    )

    return $relativeCandidates |
        ForEach-Object { Join-Path $RepoRoot $_ } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        ForEach-Object { (Resolve-Path -LiteralPath $_).Path } |
        Sort-Object -Unique
}

# Enable-TestSigningIfRequested：
# - 输入：是否请求开启；
# - 处理：执行 bcdedit /set testsigning on；
# - 返回：无返回值。
function Enable-TestSigningIfRequested {
    param(
        [Parameter(Mandatory = $true)]
        [bool] $Requested
    )

    if (-not $Requested) {
        return
    }

    if (-not (Test-Admin)) {
        throw '开启测试模式需要管理员 PowerShell。'
    }

    & bcdedit /set testsigning on
    if ($LASTEXITCODE -ne 0) {
        throw "bcdedit /set testsigning on 失败，退出码：$LASTEXITCODE"
    }

    Write-Host '已设置 testsigning on；需要重启系统后生效。'
}

# Invoke-SignTool：
# - 输入：signtool 路径、证书信息、待签名文件路径；
# - 处理：使用文件型 PFX 对主程序、DLL 或驱动进行嵌入式签名；
# - 返回：无返回值。
function Invoke-SignTool {
    param(
        [Parameter(Mandatory = $true)]
        [string] $SignToolPath,

        [Parameter(Mandatory = $true)]
        [hashtable] $CertificateInfo,

        [Parameter(Mandatory = $true)]
        [string] $TargetPath
    )

    Write-Host "签名目标：$TargetPath"
    $signOutput = & $SignToolPath sign /v /fd SHA256 /f $CertificateInfo.PfxPath /p $CertificateInfo.Password $TargetPath 2>&1
    $signExitCode = $LASTEXITCODE
    $signOutput | ForEach-Object { Write-Host $_ }
    if ($signExitCode -ne 0) {
        $joinedOutput = ($signOutput | Out-String).Trim()
        if ($joinedOutput -match 'ImportCertObject|0x80090011|NTE_NOT_FOUND|Access is denied|拒绝访问|找不到对象') {
            throw (
                "signtool 无法临时导入 PFX 私钥，通常是当前 PowerShell 没有证书存储/密钥容器写权限。" +
                "请用管理员 PowerShell 重新运行本脚本；当前目标：$TargetPath；退出码：$signExitCode")
        }
        throw "signtool sign 失败：$TargetPath，退出码：$signExitCode"
    }

    $verifyOutput = & $SignToolPath verify /pa /v $TargetPath 2>&1
    $verifyExitCode = $LASTEXITCODE
    $verifyOutput | ForEach-Object { Write-Host $_ }
    if ($verifyExitCode -ne 0) {
        Write-Warning "signtool verify /pa 未通过：$TargetPath，退出码：$verifyExitCode。"
        Write-Warning '这通常表示证书尚未导入本机信任区；签名已写入文件，但内核加载前仍需管理员导入 LocalMachine 信任区。'
    }

    $signature = Get-AuthenticodeSignature -FilePath $TargetPath
    Write-Host "签名状态：$($signature.Status)；签名者：$($signature.SignerCertificate.Subject)"
}

try {
    $repoRoot = Resolve-RepoRoot
    $isAdmin = Test-Admin
    Write-Host "仓库根目录：$repoRoot"
    Write-Host "管理员权限：$isAdmin"

    Enable-TestSigningIfRequested -Requested ([bool]$EnableTestSigning)

    $signTool = Find-SignTool
    Write-Host "signtool：$signTool"

    $certificateInfo = Get-OrCreate-TestCertificate `
        -CertificateSubject $Subject `
        -RepoRoot $repoRoot `
        -PlainPassword $PfxPassword
    Import-TestCertificateTrust -CerPath $certificateInfo.CerPath -SkipMachine ([bool]$SkipMachineTrust)

    $targets = @(Resolve-SignTargets -RepoRoot $repoRoot -ExplicitTargetPaths $TargetPath -ExplicitDriverPaths $DriverPath)
if (-not $targets -or $targets.Count -eq 0) {
        throw '未找到可签名的目标文件。请先编译驱动/主程序，或通过 -TargetPath/-DriverPath 指定路径。'
    }

    foreach ($target in $targets) {
        Invoke-SignTool -SignToolPath $signTool -CertificateInfo $certificateInfo -TargetPath $target
    }

    Write-Host ''
    Write-Host '完成。'
    if (-not $TargetPath) {
        Write-Host '若加载仍返回 577，请确认：'
        Write-Host '1. bcdedit /enum 中 testsigning 为 Yes，并且已经重启；'
        Write-Host '2. 证书已在 LocalMachine\Root 与 LocalMachine\TrustedPublisher；'
        Write-Host '3. 服务 ImagePath 指向的正是刚刚签名的 KswordARK.sys。'
    }
}
catch {
    if ($NonFatal) {
        Write-Warning "KswordARK 自动测试签名失败，但已按 NonFatal 模式继续构建：$($_.Exception.Message)"
        Write-Warning '如需生成可加载驱动，请使用管理员 PowerShell 手动运行：powershell -ExecutionPolicy Bypass -File scripts\Sign-KswordArkDriverTest.ps1 -EnableTestSigning'
        exit 0
    }

    throw
}
