#Requires -Version 5.1
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    # Optional explicit Qt install prefix, for example: C:\Qt\6.9.3\msvc2022_64.
    [string]$QtDir,

    # Optional explicit Qt MSBuild directory that contains qt.props and qt.targets.
    [string]$QtMsBuild,

    # Do not persist environment variables to the current Windows user profile.
    [switch]$NoUserEnvironment,

    # Do not create or update the repo-local Directory.Build.props helper file.
    [switch]$NoDirectoryBuildProps,

    # Do not patch .vcxproj files; only discover and export Qt paths.
    [switch]$SkipProjectFilePatch,

    # Search broader Visual Studio and drive roots when normal discovery fails.
    [switch]$DeepSearch
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Use the script location instead of the caller's current directory so the script
# behaves the same from PowerShell, Visual Studio, CLion, or a build terminal.
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Step {
    # Inputs: one human-readable progress message.
    # Processing: writes a stable prefix so script output is easy to scan.
    # Returns: no return value; the message is written to the host stream.
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Host "[Setup-QtPaths] $Message"
}

function Resolve-DirectoryOrNull {
    # Inputs: one path that may be empty, relative, or absolute.
    # Processing: resolves the path only when it points to an existing directory.
    # Returns: the absolute path, or $null when the input cannot be used.
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    try {
        $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop | Select-Object -First 1
        if ($null -ne $resolved -and (Test-Path -LiteralPath $resolved.ProviderPath -PathType Container)) {
            return $resolved.ProviderPath
        }
    }
    catch {
        return $null
    }

    return $null
}

function Add-CandidateDirectory {
    # Inputs: a list, a duplicate guard, and a candidate path.
    # Processing: resolves the directory and appends it once in discovery order.
    # Returns: no return value; the list is mutated when the path is valid.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen,
        [string]$Path
    )

    $resolved = Resolve-DirectoryOrNull -Path $Path
    if ($null -eq $resolved) {
        return
    }

    $key = $resolved.ToLowerInvariant()
    if (-not $Seen.ContainsKey($key)) {
        $Seen[$key] = $true
        [void]$List.Add($resolved)
    }
}

function Add-QtTreeCandidates {
    # Inputs: a potential Qt root such as C:\Qt or .deps\Qt.
    # Processing: adds root, children, and grandchildren because Qt commonly uses
    # Qt\6.x.y\msvc2022_64 or a similar compiler-specific directory layout.
    # Returns: no return value; candidate directories are appended to the list.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen,
        [string]$Root
    )

    $rootPath = Resolve-DirectoryOrNull -Path $Root
    if ($null -eq $rootPath) {
        return
    }

    Add-CandidateDirectory -List $List -Seen $Seen -Path $rootPath
    foreach ($firstLevel in Get-ChildItem -LiteralPath $rootPath -Directory -ErrorAction SilentlyContinue) {
        Add-CandidateDirectory -List $List -Seen $Seen -Path $firstLevel.FullName
        foreach ($secondLevel in Get-ChildItem -LiteralPath $firstLevel.FullName -Directory -ErrorAction SilentlyContinue) {
            Add-CandidateDirectory -List $List -Seen $Seen -Path $secondLevel.FullName
        }
    }
}

function Add-QtCandidateFromQMake {
    # Inputs: a discovered qmake.exe path plus the shared candidate list.
    # Processing: converts ...\bin\qmake.exe to the Qt install prefix and adds it.
    # Returns: no return value; valid parent directories are appended to the list.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen,
        [string]$QMakePath
    )

    if ([string]::IsNullOrWhiteSpace($QMakePath)) {
        return
    }

    $normalized = $QMakePath.Trim('"').Replace('/', '\')
    if (-not (Test-Path -LiteralPath $normalized -PathType Leaf)) {
        return
    }

    $qmakeFile = Get-Item -LiteralPath $normalized -ErrorAction SilentlyContinue
    if ($null -eq $qmakeFile -or $qmakeFile.Name -ieq 'qmake.exe' -eq $false) {
        return
    }

    $binDir = $qmakeFile.Directory
    if ($null -eq $binDir) {
        return
    }

    if ($binDir.Name -ieq 'bin' -and $null -ne $binDir.Parent) {
        Add-CandidateDirectory -List $List -Seen $Seen -Path $binDir.Parent.FullName
    }
    else {
        Add-CandidateDirectory -List $List -Seen $Seen -Path $binDir.FullName
    }
}

function Add-QtCandidatesFromEnvironmentBlock {
    # Inputs: the shared candidate list and duplicate guard.
    # Processing: scans all Qt-looking environment variables and PATH entries, not
    # just KSWORD_QT_DIR, because Qt installers and IDEs use different names.
    # Returns: no return value; candidate directories are appended to the list.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen
    )

    foreach ($envVar in Get-ChildItem Env: -ErrorAction SilentlyContinue) {
        if ($envVar.Name -match 'QT|QTDIR|QMAKE') {
            foreach ($part in ($envVar.Value -split ';')) {
                $cleanPart = $part.Trim('"')
                Add-CandidateDirectory -List $List -Seen $Seen -Path $cleanPart
                Add-CandidateDirectory -List $List -Seen $Seen -Path (Split-Path -Parent $cleanPart -ErrorAction SilentlyContinue)
                if ((Split-Path -Leaf $cleanPart) -ieq 'qmake.exe') {
                    Add-QtCandidateFromQMake -List $List -Seen $Seen -QMakePath $cleanPart
                }
            }
        }
    }

    foreach ($pathPart in (($env:Path -as [string]) -split ';')) {
        if ([string]::IsNullOrWhiteSpace($pathPart)) {
            continue
        }
        $candidateQMake = Join-Path ($pathPart.Trim('"')) 'qmake.exe'
        Add-QtCandidateFromQMake -List $List -Seen $Seen -QMakePath $candidateQMake
    }

    foreach ($qmakeCommand in Get-Command qmake.exe -All -ErrorAction SilentlyContinue) {
        Add-QtCandidateFromQMake -List $List -Seen $Seen -QMakePath $qmakeCommand.Source
    }
}

function Add-QtCandidatesFromText {
    # Inputs: text content that may contain Qt/qmake paths.
    # Processing: extracts qmake.exe paths and compiler-kit directories such as
    # C:\Qt\6.9.3\msvc2022_64, including forward-slash variants from generated files.
    # Returns: no return value; extracted candidates are appended to the list.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen,
        [string]$Text
    )

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return
    }

    $qmakePattern = '(?i)([A-Z]:[/\\][^\r\n"''<>|;]*?[/\\]bin[/\\]qmake\.exe)'
    foreach ($match in [regex]::Matches($Text, $qmakePattern)) {
        Add-QtCandidateFromQMake -List $List -Seen $Seen -QMakePath $match.Groups[1].Value
    }

    $kitPattern = '(?i)([A-Z]:[/\\][^\r\n"''<>|;]*?[/\\]msvc[0-9_]+)'
    foreach ($match in [regex]::Matches($Text, $kitPattern)) {
        Add-CandidateDirectory -List $List -Seen $Seen -Path ($match.Groups[1].Value.Replace('/', '\'))
    }
}

function Add-QtCandidatesFromKnownTextFiles {
    # Inputs: repository root and the shared candidate list.
    # Processing: reads only known small project and IDE configuration files where
    # Qt paths are likely to appear. It deliberately avoids recursive repository
    # scans because this project has many resources and that made normal startup slow.
    # Returns: no return value; extracted candidates are appended to the list.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $relativeFiles = @(
        'Ksword5.1\Ksword5.1\Ksword5.1.vcxproj',
        'Ksword5.1\Ksword5.1\Ksword5.1.vcxproj.user',
        'Ksword5.1\Ksword5.1\Ksword5.vcxproj',
        'Taskbar\Taskbar.vcxproj',
        'Taskbar\Taskbar.vcxproj.user',
        'KswordHUD\KswordHUD.vcxproj',
        'KswordHUD\KswordHUD.vcxproj.user',
        'Directory.Build.props'
    )

    foreach ($relativeFile in $relativeFiles) {
        $filePath = Join-Path $Root $relativeFile
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            continue
        }
        $file = Get-Item -LiteralPath $filePath -ErrorAction SilentlyContinue
        if ($null -eq $file -or $file.Length -gt 8MB) {
            continue
        }
        try {
            Add-QtCandidatesFromText -List $List -Seen $Seen -Text (Get-Content -LiteralPath $file.FullName -Raw -ErrorAction Stop)
        }
        catch {
            continue
        }
    }

    foreach ($qtConfigRoot in @(
        (Join-Path $env:APPDATA 'QtProject'),
        (Join-Path $env:LOCALAPPDATA 'QtProject'),
        (Join-Path $env:APPDATA 'QtCreator'),
        (Join-Path $env:LOCALAPPDATA 'QtCreator')
    )) {
        $resolvedConfigRoot = Resolve-DirectoryOrNull -Path $qtConfigRoot
        if ($null -eq $resolvedConfigRoot) {
            continue
        }

        $configFiles = Get-ChildItem -LiteralPath $resolvedConfigRoot -File -ErrorAction SilentlyContinue
        $configFiles += Get-ChildItem -LiteralPath $resolvedConfigRoot -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -File -ErrorAction SilentlyContinue }

        foreach ($file in $configFiles | Select-Object -First 200) {
            if ($file.Length -gt 4MB) {
                continue
            }
            try {
                Add-QtCandidatesFromText -List $List -Seen $Seen -Text (Get-Content -LiteralPath $file.FullName -Raw -ErrorAction Stop)
            }
            catch {
                continue
            }
        }
    }
}

function Get-ObjectPropertyValueOrNull {
    # Inputs: an object and one property name.
    # Processing: reads the property through PSObject metadata so StrictMode does
    # not throw when registry entries omit optional values such as InstallLocation.
    # Returns: the property value, or $null when the property is missing.
    param(
        [object]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object -or $null -eq $Object.PSObject -or $null -eq $Object.PSObject.Properties) {
        return $null
    }

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }

    return $property.Value
}

function Add-QtCandidatesFromRegistry {
    # Inputs: the shared candidate list and duplicate guard.
    # Processing: reads common Windows uninstall registry keys for Qt install roots.
    # Returns: no return value; install roots and their Qt-like subtrees are scanned.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen
    )

    $uninstallRoots = @(
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )

    foreach ($uninstallRoot in $uninstallRoots) {
        if (-not (Test-Path -LiteralPath $uninstallRoot)) {
            continue
        }
        foreach ($entry in Get-ChildItem -LiteralPath $uninstallRoot -ErrorAction SilentlyContinue) {
            try {
                $props = Get-ItemProperty -LiteralPath $entry.PSPath -ErrorAction Stop
            }
            catch {
                continue
            }

            $displayName = Get-ObjectPropertyValueOrNull -Object $props -Name 'DisplayName'
            if (($displayName -as [string]) -notmatch 'Qt') {
                continue
            }

            $installLocation = Get-ObjectPropertyValueOrNull -Object $props -Name 'InstallLocation'
            $displayIcon = Get-ObjectPropertyValueOrNull -Object $props -Name 'DisplayIcon'
            $uninstallString = Get-ObjectPropertyValueOrNull -Object $props -Name 'UninstallString'
            foreach ($pathValue in @($installLocation, $displayIcon, $uninstallString)) {
                if ([string]::IsNullOrWhiteSpace($pathValue)) {
                    continue
                }
                $candidate = (($pathValue -as [string]) -replace '^"([^" ]+).*$', '$1').Trim('"')
                if ((Split-Path -Leaf $candidate) -match '\.exe$') {
                    $candidate = Split-Path -Parent $candidate
                }
                Add-QtTreeCandidates -List $List -Seen $Seen -Root $candidate
            }
        }
    }
}

function Add-QtCandidatesFromQMakeSearchRoot {
    # Inputs: a root directory to search for qmake.exe.
    # Processing: recursively finds qmake.exe and converts each hit to a Qt prefix.
    # Returns: no return value; candidate prefixes are appended to the list.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen,
        [string]$Root
    )

    $rootPath = Resolve-DirectoryOrNull -Path $Root
    if ($null -eq $rootPath) {
        return
    }

    foreach ($qmake in Get-ChildItem -LiteralPath $rootPath -Recurse -Filter qmake.exe -File -ErrorAction SilentlyContinue) {
        Add-QtCandidateFromQMake -List $List -Seen $Seen -QMakePath $qmake.FullName
    }
}

function Get-FixedDriveRoots {
    # Inputs: none.
    # Processing: enumerates usable local filesystem drive roots without assuming
    # fixed drive letters; offline/removable drives are ignored by Test-Path.
    # Returns: an array of drive roots such as C:\ and D:\.
    $roots = @()
    foreach ($drive in Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue) {
        if ($drive.Root -match '^[A-Z]:\\$' -and (Test-Path -LiteralPath $drive.Root)) {
            $roots += $drive.Root
        }
    }
    return $roots
}

function Test-QtInstallDirectory {
    # Inputs: one candidate Qt installation directory.
    # Processing: checks qmake.exe, moc.exe, and at least one core header/lib marker.
    # Returns: $true when the directory looks usable for Qt VS Tools projects.
    param([string]$Path)

    $resolved = Resolve-DirectoryOrNull -Path $Path
    if ($null -eq $resolved) {
        return $false
    }

    $qmake = Join-Path $resolved 'bin\qmake.exe'
    $moc = Join-Path $resolved 'bin\moc.exe'
    $qtCoreHeader = Join-Path $resolved 'include\QtCore'
    $qt6CoreLib = Join-Path $resolved 'lib\Qt6Core.lib'
    $qt5CoreLib = Join-Path $resolved 'lib\Qt5Core.lib'

    return ((Test-Path -LiteralPath $qmake -PathType Leaf) -and
            (Test-Path -LiteralPath $moc -PathType Leaf) -and
            ((Test-Path -LiteralPath $qtCoreHeader -PathType Container) -or
             (Test-Path -LiteralPath $qt6CoreLib -PathType Leaf) -or
             (Test-Path -LiteralPath $qt5CoreLib -PathType Leaf)))
}

function Get-QtInstallScore {
    # Inputs: one already validated Qt install path.
    # Processing: ranks MSVC x64 and newer Qt versions higher for automatic choice.
    # Returns: an integer score used only for sorting candidates.
    param([Parameter(Mandatory = $true)][string]$Path)

    $score = 0
    $leaf = Split-Path -Leaf $Path
    $parent = Split-Path -Leaf (Split-Path -Parent $Path)

    if ($leaf -match 'msvc\d+_64') { $score += 1000000 }
    elseif ($leaf -match 'msvc') { $score += 500000 }
    if ($leaf -match '_64$') { $score += 100000 }
    if (Test-Path -LiteralPath (Join-Path $Path 'lib\Qt6Core.lib') -PathType Leaf) { $score += 50000 }

    $versionText = $parent
    if ($Path -match '([0-9]+)\.([0-9]+)\.([0-9]+)') {
        $versionText = $Matches[0]
    }
    if ($versionText -match '^([0-9]+)\.([0-9]+)\.([0-9]+)$') {
        $score += ([int]$Matches[1] * 10000) + ([int]$Matches[2] * 100) + [int]$Matches[3]
    }

    return $score
}

function Find-QtInstallDirectory {
    # Inputs: optional explicit Qt path, repository root, and deep-search preference.
    # Processing: first checks fast sources such as environment variables, PATH,
    # Qt Creator settings, registry install locations, and common Qt roots. If that
    # fails, it automatically scans fixed drives for qmake.exe because Qt is often
    # installed under custom folders like D:\Dev\Qt or E:\SDK\Qt.
    # Returns: the best Qt install path, or throws with a detailed action message.
    param(
        [string]$ExplicitQtDir,
        [Parameter(Mandatory = $true)][string]$Root,
        [switch]$UseDeepSearch
    )

    $candidates = New-Object System.Collections.Generic.List[string]
    $seen = @{}

    Add-CandidateDirectory -List $candidates -Seen $seen -Path $ExplicitQtDir
    Add-CandidateDirectory -List $candidates -Seen $seen -Path $env:KSWORD_QT_DIR
    Add-CandidateDirectory -List $candidates -Seen $seen -Path $env:QTDIR
    Add-CandidateDirectory -List $candidates -Seen $seen -Path $env:QT_DIR
    Add-QtCandidatesFromEnvironmentBlock -List $candidates -Seen $seen
    Add-QtCandidatesFromKnownTextFiles -List $candidates -Seen $seen -Root $Root
    Add-QtCandidatesFromRegistry -List $candidates -Seen $seen

    Add-QtTreeCandidates -List $candidates -Seen $seen -Root (Join-Path $Root '.deps\Qt')
    Add-QtTreeCandidates -List $candidates -Seen $seen -Root (Join-Path $env:USERPROFILE 'Qt')
    foreach ($driveRoot in Get-FixedDriveRoots) {
        Add-QtTreeCandidates -List $candidates -Seen $seen -Root (Join-Path $driveRoot 'Qt')
        foreach ($topLevel in Get-ChildItem -LiteralPath $driveRoot -Directory -ErrorAction SilentlyContinue) {
            if ($topLevel.Name -match '^(Qt|QT|qt|Dev|SDK|Tools|Libraries|Libs)$') {
                Add-QtTreeCandidates -List $candidates -Seen $seen -Root $topLevel.FullName
            }
        }
    }

    $valid = @()
    foreach ($candidate in $candidates) {
        if (Test-QtInstallDirectory -Path $candidate) {
            $valid += [pscustomobject]@{
                Path = $candidate
                Score = Get-QtInstallScore -Path $candidate
            }
        }
    }

    if ($valid.Count -eq 0 -and $UseDeepSearch) {
        Write-Step 'Fast Qt search did not find a valid MSVC kit; -DeepSearch is enabled, so fixed drives will be scanned for qmake.exe. This can take a while.'
        foreach ($driveRoot in Get-FixedDriveRoots) {
            Add-QtCandidatesFromQMakeSearchRoot -List $candidates -Seen $seen -Root $driveRoot
        }

        foreach ($candidate in $candidates) {
            if (Test-QtInstallDirectory -Path $candidate) {
                $valid += [pscustomobject]@{
                    Path = $candidate
                    Score = Get-QtInstallScore -Path $candidate
                }
            }
        }
    }

    if ($valid.Count -eq 0) {
        $searched = (Get-FixedDriveRoots | ForEach-Object { $_ + 'Qt, ' + $_ + '<top-level Qt-like folders>' }) -join '; '
        throw "Qt was not found by the fast search. Checked environment/PATH, Qt Creator settings, registry, repository files, and common roots: $searched. If Qt is in a custom folder, rerun with -QtDir '<path-to-msvc2022_64>'. To allow slow full-drive qmake.exe scanning, rerun with -DeepSearch."
    }

    return ($valid | Sort-Object Score, Path -Descending | Select-Object -First 1).Path
}

function Test-QtMsBuildDirectory {
    # Inputs: one candidate Qt MSBuild directory.
    # Processing: verifies that both qt.props and qt.targets exist.
    # Returns: $true when Qt VS Tools MSBuild files are present.
    param([string]$Path)

    $resolved = Resolve-DirectoryOrNull -Path $Path
    if ($null -eq $resolved) {
        return $false
    }

    $props = Join-Path $resolved 'qt.props'
    $targets = Join-Path $resolved 'qt.targets'
    return ((Test-Path -LiteralPath $props -PathType Leaf) -and
            (Test-Path -LiteralPath $targets -PathType Leaf))
}

function Add-QtMsBuildSearchRoot {
    # Inputs: a root directory, candidate list, and duplicate guard.
    # Processing: adds direct candidates and directories containing qt.props.
    # Returns: no return value; the candidate list is mutated.
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][hashtable]$Seen,
        [string]$Root,
        [switch]$Recursive
    )

    $rootPath = Resolve-DirectoryOrNull -Path $Root
    if ($null -eq $rootPath) {
        return
    }

    Add-CandidateDirectory -List $List -Seen $Seen -Path $rootPath
    foreach ($child in Get-ChildItem -LiteralPath $rootPath -Directory -ErrorAction SilentlyContinue) {
        Add-CandidateDirectory -List $List -Seen $Seen -Path $child.FullName
    }

    if ($Recursive) {
        foreach ($props in Get-ChildItem -LiteralPath $rootPath -Recurse -Filter qt.props -File -ErrorAction SilentlyContinue) {
            Add-CandidateDirectory -List $List -Seen $Seen -Path $props.DirectoryName
        }
    }
}

function Find-QtMsBuildDirectory {
    # Inputs: optional explicit QtMsBuild path, repo root, and broad-search flag.
    # Processing: searches environment variables, repo-local .deps, and Qt VS Tools
    # extension locations. If the fast pass fails, it automatically scans Visual
    # Studio extension folders and fixed drives for qt.props/qt.targets.
    # Returns: the QtMsBuild directory, or throws with an actionable message.
    param(
        [string]$ExplicitQtMsBuild,
        [Parameter(Mandatory = $true)][string]$Root,
        [switch]$UseDeepSearch
    )

    $candidates = New-Object System.Collections.Generic.List[string]
    $seen = @{}

    Add-CandidateDirectory -List $candidates -Seen $seen -Path $ExplicitQtMsBuild
    Add-CandidateDirectory -List $candidates -Seen $seen -Path $env:QtMsBuild
    Add-CandidateDirectory -List $candidates -Seen $seen -Path $env:KSWORD_QT_MSBUILD
    Add-CandidateDirectory -List $candidates -Seen $seen -Path (Join-Path $Root '.deps\QtVsTools\msbuild')
    Add-CandidateDirectory -List $candidates -Seen $seen -Path (Join-Path $Root 'QtMsBuild')

    Add-QtMsBuildSearchRoot -List $candidates -Seen $seen -Root (Join-Path $env:LOCALAPPDATA 'QtMsBuild')
    Add-QtMsBuildSearchRoot -List $candidates -Seen $seen -Root (Join-Path $env:APPDATA 'QtMsBuild')
    Add-QtMsBuildSearchRoot -List $candidates -Seen $seen -Root (Join-Path $env:LOCALAPPDATA 'Microsoft\VisualStudio') -Recursive:$UseDeepSearch

    foreach ($candidate in $candidates) {
        if (Test-QtMsBuildDirectory -Path $candidate) {
            return (Resolve-DirectoryOrNull -Path $candidate)
        }
    }

    if ($UseDeepSearch) {
        Write-Step 'Fast QtMsBuild search did not find qt.props/qt.targets; -DeepSearch is enabled, so Visual Studio and Qt-like drive folders will be scanned.'
        Add-QtMsBuildSearchRoot -List $candidates -Seen $seen -Root (Join-Path $env:LOCALAPPDATA 'Microsoft\VisualStudio') -Recursive
        Add-QtMsBuildSearchRoot -List $candidates -Seen $seen -Root ${env:ProgramFiles(x86)} -Recursive
        Add-QtMsBuildSearchRoot -List $candidates -Seen $seen -Root $env:ProgramFiles -Recursive
        foreach ($driveRoot in Get-FixedDriveRoots) {
            foreach ($topLevel in Get-ChildItem -LiteralPath $driveRoot -Directory -ErrorAction SilentlyContinue) {
                if ($topLevel.Name -match '^(Qt|QT|qt|Dev|SDK|Tools|Libraries|Libs)$') {
                    Add-QtMsBuildSearchRoot -List $candidates -Seen $seen -Root $topLevel.FullName -Recursive
                }
            }
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-QtMsBuildDirectory -Path $candidate) {
            return (Resolve-DirectoryOrNull -Path $candidate)
        }
    }

    throw 'QtMsBuild was not found by the fast search. Install Qt VS Tools, restore .deps\QtVsTools\msbuild, run with -QtMsBuild <directory-containing-qt.props-and-qt.targets>, or rerun with -DeepSearch.'
}

function ConvertTo-MsBuildRelativePath {
    # Inputs: a base directory and a target directory.
    # Processing: uses System.Uri for Windows PowerShell 5.1 compatible relative paths.
    # Returns: a backslash-separated relative path for MSBuild project XML.
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$TargetDirectory
    )

    $baseFull = [System.IO.Path]::GetFullPath($BaseDirectory)
    $targetFull = [System.IO.Path]::GetFullPath($TargetDirectory)
    if (-not $baseFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $baseFull += [System.IO.Path]::DirectorySeparatorChar
    }

    $baseUri = New-Object System.Uri($baseFull)
    $targetUri = New-Object System.Uri($targetFull)
    $relative = [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString())
    return $relative.Replace('/', '\')
}

function ConvertTo-XmlText {
    # Inputs: a raw string intended for XML text content.
    # Processing: escapes XML-sensitive characters.
    # Returns: an XML-safe string.
    param([Parameter(Mandatory = $true)][string]$Text)

    return [System.Security.SecurityElement]::Escape($Text)
}

function Set-CurrentProcessQtEnvironment {
    # Inputs: detected Qt install directory and optional Qt MSBuild directory.
    # Processing: sets the variables used by this repository and Qt VS Tools; when
    # QtMsBuild is absent, only Qt install variables are set so bad paths are not created.
    # Returns: no return value; only the current PowerShell process is changed.
    param(
        [Parameter(Mandatory = $true)][string]$DetectedQtDir,
        [string]$DetectedQtMsBuild
    )

    $env:KSWORD_QT_DIR = $DetectedQtDir
    $env:QTDIR = $DetectedQtDir
    if (-not [string]::IsNullOrWhiteSpace($DetectedQtMsBuild)) {
        $env:QtMsBuild = $DetectedQtMsBuild
        $env:KSWORD_QT_MSBUILD = $DetectedQtMsBuild
    }
}

function Set-UserQtEnvironment {
    # Inputs: detected Qt install directory and optional Qt MSBuild directory.
    # Processing: persists variables for future Visual Studio/MSBuild processes; when
    # QtMsBuild is absent, existing user QtMsBuild variables are left untouched.
    # Returns: no return value; Windows user environment variables are updated.
    param(
        [Parameter(Mandatory = $true)][string]$DetectedQtDir,
        [string]$DetectedQtMsBuild
    )

    [Environment]::SetEnvironmentVariable('KSWORD_QT_DIR', $DetectedQtDir, 'User')
    [Environment]::SetEnvironmentVariable('QTDIR', $DetectedQtDir, 'User')
    if (-not [string]::IsNullOrWhiteSpace($DetectedQtMsBuild)) {
        [Environment]::SetEnvironmentVariable('QtMsBuild', $DetectedQtMsBuild, 'User')
        [Environment]::SetEnvironmentVariable('KSWORD_QT_MSBUILD', $DetectedQtMsBuild, 'User')
    }
}

function Update-DirectoryBuildProps {
    # Inputs: repo root, detected Qt install path, and optional QtMsBuild path.
    # Processing: creates or updates a generated block in Directory.Build.props so
    # MSBuild resolves Qt before project-local fallback paths are evaluated.
    # Returns: the Directory.Build.props path.
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$DetectedQtDir,
        [string]$DetectedQtMsBuild
    )

    $propsPath = Join-Path $Root 'Directory.Build.props'
    $beginMarker = '<!-- KSWORD_QT_PATHS_BEGIN -->'
    $endMarker = '<!-- KSWORD_QT_PATHS_END -->'
    $qtDirXml = ConvertTo-XmlText -Text $DetectedQtDir

    $blockLines = @(
        '  ' + $beginMarker,
        '  <!-- Generated by Setup-QtPaths.ps1. Keep machine-specific Qt paths out of .vcxproj files. -->',
        '  <PropertyGroup Label="KSword local Qt paths">',
        '    <KSWORD_QT_DIR>' + $qtDirXml + '</KSWORD_QT_DIR>',
        '    <QTDIR Condition="''$(QTDIR)'' == ''''">$(KSWORD_QT_DIR)</QTDIR>'
    )
    if (-not [string]::IsNullOrWhiteSpace($DetectedQtMsBuild)) {
        $qtMsBuildXml = ConvertTo-XmlText -Text $DetectedQtMsBuild
        $blockLines += '    <KSWORD_QT_MSBUILD>' + $qtMsBuildXml + '</KSWORD_QT_MSBUILD>'
        $blockLines += '    <QtMsBuild Condition="''$(QtMsBuild)'' == '''' or !Exists(''$(QtMsBuild)\qt.targets'')">$(KSWORD_QT_MSBUILD)</QtMsBuild>'
    }
    $blockLines += @(
        '  </PropertyGroup>',
        '  ' + $endMarker
    )
    $block = ($blockLines -join [Environment]::NewLine) + [Environment]::NewLine

    if (-not (Test-Path -LiteralPath $propsPath -PathType Leaf)) {
        $content = '<?xml version="1.0" encoding="utf-8"?>' + [Environment]::NewLine + '<Project>' + [Environment]::NewLine + $block + '</Project>' + [Environment]::NewLine
        if ($PSCmdlet.ShouldProcess($propsPath, 'create Directory.Build.props with detected Qt paths')) {
            Set-Content -LiteralPath $propsPath -Value $content -Encoding UTF8
        }
        return $propsPath
    }

    $existing = Get-Content -LiteralPath $propsPath -Raw
    $pattern = [regex]::Escape($beginMarker) + '(.|\r|\n)*?' + [regex]::Escape($endMarker)
    if ($existing -match $pattern) {
        $updated = [regex]::Replace($existing, $pattern, [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $block.TrimEnd() })
    }
    elseif ($existing -match '</Project>\s*$') {
        $updated = [regex]::Replace($existing, '</Project>\s*$', $block + '</Project>' + [Environment]::NewLine)
    }
    else {
        throw "Directory.Build.props exists but does not look like an MSBuild Project XML file: $propsPath"
    }

    if ($updated -ne $existing -and $PSCmdlet.ShouldProcess($propsPath, 'update generated Qt path block')) {
        Set-Content -LiteralPath $propsPath -Value $updated -Encoding UTF8
    }

    return $propsPath
}

function Get-QtProjectFiles {
    # Inputs: repository root.
    # Processing: scans .vcxproj files and selects Qt VS Tools project files only.
    # Returns: an array of full project paths.
    param([Parameter(Mandatory = $true)][string]$Root)

    $projects = @()
    foreach ($project in Get-ChildItem -LiteralPath $Root -Recurse -Filter *.vcxproj -File -ErrorAction SilentlyContinue) {
        $text = Get-Content -LiteralPath $project.FullName -Raw
        if ($text -match 'QtMsBuild' -or $text -match '\$\(QtMsBuild\)\\Qt\.props') {
            $projects += $project.FullName
        }
    }

    return $projects
}

function Update-QtProjectFile {
    # Inputs: one Qt .vcxproj file and the repository root.
    # Processing: replaces project-local QtMsBuild fallback with environment and
    # repo-local fallbacks, conditionalizes Qt imports, and adds QTDIR fallback.
    # Returns: $true when the project file changed; otherwise $false.
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $original = Get-Content -LiteralPath $ProjectPath -Raw
    $updated = $original
    $projectDir = Split-Path -Parent $ProjectPath
    $repoQtMsBuild = Join-Path $Root '.deps\QtVsTools\msbuild'
    $relativeRepoQtMsBuild = ConvertTo-MsBuildRelativePath -BaseDirectory $projectDir -TargetDirectory $repoQtMsBuild

    $repoQtMsBuildExpression = '$(MSBuildThisFileDirectory)' + $relativeRepoQtMsBuild
    $repoQtTargetsExpression = $repoQtMsBuildExpression + '\qt.targets'
    $fallbackLines = @(
        '    <QtMsBuild Condition="''$(QtMsBuild)'' == '''' and ''$(KSWORD_QT_MSBUILD)'' != '''' and Exists(''$(KSWORD_QT_MSBUILD)\qt.targets'')">$(KSWORD_QT_MSBUILD)</QtMsBuild>',
        '    <QtMsBuild Condition="''$(QtMsBuild)'' == '''' and Exists(''' + $repoQtTargetsExpression + ''')">' + $repoQtMsBuildExpression + '</QtMsBuild>',
        '    <QtMsBuild Condition="''$(QtMsBuild)'' == '''' or !Exists(''$(QtMsBuild)\qt.targets'')">$(MSBuildProjectDirectory)\QtMsBuild</QtMsBuild>'
    )
    $fallbackBlock = $fallbackLines -join [Environment]::NewLine

    # Only expand the old single project-local fallback once. If the project already
    # contains the portable KSWORD_QT_MSBUILD and repo .deps fallbacks, leave it as-is
    # so repeated script runs are idempotent instead of duplicating QtMsBuild nodes.
    if ($updated -notmatch 'KSWORD_QT_MSBUILD' -or $updated -notmatch '\.deps\\QtVsTools\\msbuild') {
        $updated = [regex]::Replace(
            $updated,
            '(?m)^\s*<QtMsBuild\b[^>]*>\$\(MSBuildProjectDirectory\)\\QtMsBuild</QtMsBuild>\s*$',
            [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $fallbackBlock })
    }

    $updated = [regex]::Replace(
        $updated,
        '<Import Project="\$\(QtMsBuild\)\\Qt\.props"\s*/>',
        '<Import Project="$(QtMsBuild)\Qt.props" Condition="Exists(''$(QtMsBuild)\Qt.props'')" />')

    $updated = [regex]::Replace(
        $updated,
        '<Import Project="\$\(QtMsBuild\)\\qt\.targets"\s*/>',
        '<Import Project="$(QtMsBuild)\qt.targets" Condition="Exists(''$(QtMsBuild)\qt.targets'')" />')

    if ($updated -notmatch '<QtInstall[^>]*QTDIR') {
        $qtInstallPattern = '(?m)^(\s*)<QtInstall Condition="''\$\(KSWORD_QT_DIR\)''\s*!=\s*''''">\$\(KSWORD_QT_DIR\)</QtInstall>\r?\n\s*<QtInstall Condition="''\$\(KSWORD_QT_DIR\)''\s*==\s*''''">Qt</QtInstall>'
        $updated = [regex]::Replace(
            $updated,
            $qtInstallPattern,
            [System.Text.RegularExpressions.MatchEvaluator]{
                param($m)
                $indent = $m.Groups[1].Value
                return ($indent + '<QtInstall Condition="''$(KSWORD_QT_DIR)'' != ''''">$(KSWORD_QT_DIR)</QtInstall>' + [Environment]::NewLine +
                        $indent + '<QtInstall Condition="''$(QtInstall)'' == '''' and ''$(QTDIR)'' != ''''">$(QTDIR)</QtInstall>' + [Environment]::NewLine +
                        $indent + '<QtInstall Condition="''$(QtInstall)'' == ''''">Qt</QtInstall>')
            })
    }

    if ($updated -eq $original) {
        return $false
    }

    if ($PSCmdlet.ShouldProcess($ProjectPath, 'patch portable Qt MSBuild fallback rules')) {
        Set-Content -LiteralPath $ProjectPath -Value $updated -Encoding UTF8
    }

    return $true
}

function Update-QtProjectFiles {
    # Inputs: repository root.
    # Processing: applies Qt portability fixes to every Qt VS Tools project file.
    # Returns: the number of project files changed.
    param([Parameter(Mandatory = $true)][string]$Root)

    $changed = 0
    foreach ($project in Get-QtProjectFiles -Root $Root) {
        if (Update-QtProjectFile -ProjectPath $project -Root $Root) {
            $changed++
            Write-Step "Patched Qt project: $project"
        }
    }

    return $changed
}

Write-Step "Repository root: $RepoRoot"
$detectedQtDir = Find-QtInstallDirectory -ExplicitQtDir $QtDir -Root $RepoRoot -UseDeepSearch:$DeepSearch
Write-Step "Detected Qt install: $detectedQtDir"
$detectedQtMsBuild = $null
try {
    $detectedQtMsBuild = Find-QtMsBuildDirectory -ExplicitQtMsBuild $QtMsBuild -Root $RepoRoot -UseDeepSearch:$DeepSearch
    Write-Step "Detected QtMsBuild: $detectedQtMsBuild"
}
catch {
    Write-Warning $_.Exception.Message
    Write-Warning 'Qt project imports will be made conditional so the solution can load, but command-line/VS builds still need Qt VS Tools MSBuild files. Install Qt VS Tools or pass -QtMsBuild when available.'
}

if ($PSCmdlet.ShouldProcess('current PowerShell process', 'set Qt environment variables')) {
    Set-CurrentProcessQtEnvironment -DetectedQtDir $detectedQtDir -DetectedQtMsBuild $detectedQtMsBuild
}

if (-not $NoUserEnvironment) {
    if ($PSCmdlet.ShouldProcess('current Windows user', 'persist Qt environment variables')) {
        Set-UserQtEnvironment -DetectedQtDir $detectedQtDir -DetectedQtMsBuild $detectedQtMsBuild
        if ([string]::IsNullOrWhiteSpace($detectedQtMsBuild)) {
            Write-Step 'Persisted KSWORD_QT_DIR and QTDIR for the current user. QtMsBuild was not persisted because it was not found.'
        }
        else {
            Write-Step 'Persisted KSWORD_QT_DIR, QTDIR, QtMsBuild, and KSWORD_QT_MSBUILD for the current user.'
        }
    }
}
else {
    Write-Step 'Skipped user environment persistence.'
}

if (-not $NoDirectoryBuildProps) {
    $propsPath = Update-DirectoryBuildProps -Root $RepoRoot -DetectedQtDir $detectedQtDir -DetectedQtMsBuild $detectedQtMsBuild
    Write-Step "Updated local MSBuild props: $propsPath"
}
else {
    Write-Step 'Skipped Directory.Build.props update.'
}

if (-not $SkipProjectFilePatch) {
    $changedProjects = Update-QtProjectFiles -Root $RepoRoot
    Write-Step "Qt project files patched: $changedProjects"
}
else {
    Write-Step 'Skipped .vcxproj patching.'
}

Write-Step 'Done. Restart Visual Studio/CLion if it was already open so it reloads user environment variables.'
Write-Step 'Command-line builds launched from this PowerShell process can use the detected variables immediately.'

