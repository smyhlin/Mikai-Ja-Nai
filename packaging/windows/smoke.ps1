#requires -Version 7.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$OutputDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Evidence = [ordered]@{
    schema = 'mikai.windows-installer-smoke/1'
    status = 'failed'
    startedUtc = [DateTime]::UtcNow.ToString('o')
    installerSha256 = $null
    installPath = $null
    startMenuGroup = $null
    registryBefore = $null
    registryAfter = $null
    checks = [ordered]@{}
    cleanupErrors = @()
    error = $null
}
$root = $null
$installAttempted = $false
$installCleaned = $false
$installPath = $null
$shortcut = $null
$exe = $null
$script:terminationFailed = $false
$processEvidence = [Collections.Generic.List[object]]::new()

function Write-Evidence {
    if ($null -eq $script:EvidencePath) { return }
    $script:Evidence.finishedUtc = [DateTime]::UtcNow.ToString('o')
    $script:Evidence.processes = @($processEvidence.ToArray())
    [IO.File]::WriteAllText($script:EvidencePath,
        ($script:Evidence | ConvertTo-Json -Depth 12) + "`n", [Text.UTF8Encoding]::new($false))
}
function Invoke-Bounded([string]$Name, [string]$File, [string[]]$Arguments, [int]$TimeoutSeconds = 120) {
    $log = Join-Path $script:OutputPath ($Name + '.log')
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -PassThru -Wait:$false `
        -RedirectStandardOutput $log -RedirectStandardError ($log + '.stderr')
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $killError = $null
        try {
            $process.Kill($true)
            if (-not $process.WaitForExit(10000)) {
                $killError = 'Process tree did not exit within 10 seconds after termination was requested.'
            }
        } catch {
            $killError = $_.Exception.Message
        }
        if ($null -ne $killError) { $script:terminationFailed = $true }
        $processEvidence.Add([ordered]@{name=$Name; status='timeout'; timeoutSeconds=$TimeoutSeconds; killError=$killError; log=(Split-Path -Leaf $log)})
        if ($null -ne $killError) {
            throw "$Name exceeded its ${TimeoutSeconds}s timeout; process-tree termination failed: $killError. See evidence log."
        }
        throw "$Name exceeded its ${TimeoutSeconds}s timeout; process tree was terminated. See evidence log."
    }
    $process.Refresh()
    $processEvidence.Add([ordered]@{name=$Name; status='exited'; exitCode=$process.ExitCode; log=(Split-Path -Leaf $log)})
    if ($process.ExitCode -ne 0) { throw "$Name failed with exit code $($process.ExitCode); see evidence log." }
}
function Assert-NoReparsePath([string]$Path) {
    $candidate = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($candidate)) {
        if (Test-Path -LiteralPath $candidate) {
            $item = Get-Item -LiteralPath $candidate -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing reparse point: $($item.FullName)"
            }
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::Equals($parent, $candidate, [StringComparison]::OrdinalIgnoreCase)) { break }
        $candidate = $parent
    }
}
function Get-IntegrationSnapshot {
    # Explicit allowlist only. Record presence/type/hash; never serialize registry contents.
    $keys = @(
        @('Software\Mikai\DefaultVideoPlayer', @('MikaiRegistrationOwner')),
        @('Software\Mikai\DefaultVideoPlayer\Capabilities', @('ApplicationName','ApplicationDescription','ApplicationIcon')),
        @('Software\Classes\Mikai.Video', @('','MikaiRegistrationOwner')),
        @('Software\Classes\Mikai.Video\DefaultIcon', @('')),
        @('Software\Classes\Mikai.Video\shell\open\command', @('')),
        @('Software\RegisteredApplications', @('Mikai')),
        @('Software\Microsoft\Windows\CurrentVersion\Run', @('Mikai'))
    )
    $result = [ordered]@{}
    foreach ($ext in @('.mkv','.mp4','.webm','.avi','.mov','.m4v','.ts','.m2ts','.mpeg','.mpg','.flv')) {
        $keys += ,@("Software\Classes\SystemFileAssociations\$ext\shell\Mikai.OpenVideo", @('','Icon','MikaiRegistrationOwner'))
        $keys += ,@("Software\Classes\SystemFileAssociations\$ext\shell\Mikai.OpenVideo\command", @(''))
    }
    foreach ($entry in $keys) {
        $keyPath = [string]$entry[0]
        $names = [string[]]$entry[1]
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath, $false)
        foreach ($name in $names) {
            $id = $keyPath + '|' + $name
            if ($null -eq $key -or -not ($key.GetValueNames() -contains $name)) { $result[$id] = 'missing'; continue }
            $value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $kind = $key.GetValueKind($name).ToString()
            $bytes = [Text.Encoding]::UTF8.GetBytes($kind + "`0" + [string]$value)
            $result[$id] = $kind + ':' + [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
        }
        if ($null -ne $key) { $key.Dispose() }
    }
    return $result
}
function Assert-SameSnapshot($Before, $After, [string]$Phase) {
    foreach ($key in $Before.Keys) {
        if ($After[$key] -cne $Before[$key]) { throw "Known integration registry value changed during ${Phase}: $key" }
    }
}
function Write-Sentinel([string]$Path, [byte[]]$Bytes) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllBytes($Path, $Bytes)
}
function Assert-Sentinel([string]$Path, [byte[]]$Bytes) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Sentinel was removed: $Path" }
    $actual = [IO.File]::ReadAllBytes($Path)
    if ([Convert]::ToBase64String($actual) -cne [Convert]::ToBase64String($Bytes)) { throw "Sentinel bytes changed: $Path" }
}
function Assert-InstalledPayload([string]$Path, [string[]]$SentinelPaths = @()) {
    $manifestPath = Join-Path $Path 'PACKAGE-CONTENTS.sha256'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'Installed payload inventory manifest is missing.'
    }
    $expected = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines($manifestPath, [Text.Encoding]::UTF8)) {
        if ($line -notmatch '^([0-9A-Fa-f]{64})[ \t]+(.+?)\s*$') {
            throw 'Installed payload inventory contains a malformed line.'
        }
        $hash = $Matches[1].ToLowerInvariant()
        $relative = $Matches[2].Replace('/', '\')
        if ([IO.Path]::IsPathRooted($relative) -or
            @($relative.Split('\') | Where-Object { $_ -eq '..' -or $_ -eq '' }).Count -gt 0 -or
            $relative -match '["\r\n;]') {
            throw 'Installed payload inventory contains an unsafe relative path.'
        }
        if ($expected.ContainsKey($relative)) { throw "Installed payload inventory repeats a path: $relative" }
        $expected.Add($relative, $hash)
    }
    $sentinels = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($sentinel in $SentinelPaths) {
        $fullPath = [IO.Path]::GetFullPath($sentinel)
        $rootPath = [IO.Path]::GetFullPath($Path).TrimEnd('\') + '\'
        if (-not $fullPath.StartsWith($rootPath, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Sentinel path is outside the owned install root.'
        }
        [void]$sentinels.Add($fullPath.Substring($rootPath.Length))
    }
    foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File -Force) {
        $relative = $file.FullName.Substring([IO.Path]::GetFullPath($Path).TrimEnd('\').Length + 1)
        if ($expected.ContainsKey($relative)) {
            $actualHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actualHash -cne $expected[$relative]) { throw "Installed payload hash differs from its inventory: $relative" }
            continue
        }
        if ($relative -ceq 'PACKAGE-CONTENTS.sha256' -or
            $relative -match '^unins\d{3}\.(exe|dat)$' -or $sentinels.Contains($relative)) {
            continue
        }
        throw "Unexpected file in the installed payload: $relative"
    }
    foreach ($relative in $expected.Keys) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relative) -PathType Leaf)) {
            throw "Installed payload file from inventory is missing: $relative"
        }
    }
    return $expected.Count
}
function Get-ShortcutMetadata([string]$Path) {
    if ($null -eq ('MikaiSmoke.ShortcutReader' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace MikaiSmoke {
    [ComImport, Guid("000214F9-0000-0000-C000-000000000046"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IShellLinkW {
        [PreserveSig] int GetPath([Out] StringBuilder path, int capacity, IntPtr findData, uint flags);
        [PreserveSig] int GetIDList(out IntPtr itemIdList);
        [PreserveSig] int SetIDList(IntPtr itemIdList);
        [PreserveSig] int GetDescription([Out] StringBuilder description, int capacity);
        [PreserveSig] int SetDescription([MarshalAs(UnmanagedType.LPWStr)] string description);
        [PreserveSig] int GetWorkingDirectory([Out] StringBuilder directory, int capacity);
        [PreserveSig] int SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string directory);
        [PreserveSig] int GetArguments([Out] StringBuilder arguments, int capacity);
        [PreserveSig] int SetArguments([MarshalAs(UnmanagedType.LPWStr)] string arguments);
        [PreserveSig] int GetHotkey(out short hotkey);
        [PreserveSig] int SetHotkey(short hotkey);
        [PreserveSig] int GetShowCmd(out int showCommand);
        [PreserveSig] int SetShowCmd(int showCommand);
        [PreserveSig] int GetIconLocation([Out] StringBuilder iconPath, int capacity, out int iconIndex);
        [PreserveSig] int SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string iconPath, int iconIndex);
        [PreserveSig] int SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string path, uint reserved);
        [PreserveSig] int Resolve(IntPtr window, uint flags);
        [PreserveSig] int SetPath([MarshalAs(UnmanagedType.LPWStr)] string path);
    }

    [ComImport, Guid("0000010B-0000-0000-C000-000000000046"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IPersistFile {
        [PreserveSig] int GetClassID(out Guid classId);
        [PreserveSig] int IsDirty();
        [PreserveSig] int Load([MarshalAs(UnmanagedType.LPWStr)] string fileName, uint mode);
        [PreserveSig] int Save([MarshalAs(UnmanagedType.LPWStr)] string fileName, bool remember);
        [PreserveSig] int SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string fileName);
        [PreserveSig] int GetCurFile([MarshalAs(UnmanagedType.LPWStr)] out string fileName);
    }

    public sealed class ShortcutMetadata {
        public string TargetPath { get; set; }
        public string WorkingDirectory { get; set; }
    }

    public static class ShortcutReader {
        public static ShortcutMetadata Read(string fileName) {
            var shellLinkType = Type.GetTypeFromCLSID(new Guid("00021401-0000-0000-C000-000000000046"), true);
            var shellLink = (IShellLinkW)Activator.CreateInstance(shellLinkType);
            try {
                var persist = (IPersistFile)shellLink;
                var hr = persist.Load(fileName, 0);
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                var target = new StringBuilder(32768);
                var workingDirectory = new StringBuilder(32768);
                hr = shellLink.GetPath(target, target.Capacity, IntPtr.Zero, 0);
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                hr = shellLink.GetWorkingDirectory(workingDirectory, workingDirectory.Capacity);
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                return new ShortcutMetadata { TargetPath = target.ToString(), WorkingDirectory = workingDirectory.ToString() };
            } finally {
                if (Marshal.IsComObject(shellLink)) Marshal.FinalReleaseComObject(shellLink);
            }
        }
    }
}
'@
    }
    return [MikaiSmoke.ShortcutReader]::Read($Path)
}

$script:EvidencePath = $null
$script:OutputPath = $null
try {
    if ($env:OS -ne 'Windows_NT') { throw 'Installer smoke is Windows-only.' }
    $out = [IO.Path]::GetFullPath($OutputDir)
    if (Test-Path -LiteralPath $out) { throw "OutputDir must be fresh and absent: $out" }
    New-Item -ItemType Directory -Path $out | Out-Null
    Assert-NoReparsePath $out
    $script:OutputPath = $out
    $script:EvidencePath = Join-Path $out 'installer-smoke.json'
    $installerPath = (Resolve-Path -LiteralPath $Installer).Path
    Assert-NoReparsePath $installerPath
    $script:Evidence.installerSha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()

    $uninstall = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
    $existing = @()
    if (Test-Path $uninstall) {
        $existing = @(Get-ChildItem $uninstall -ErrorAction Stop | Where-Object {
            $p = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction Stop
            ($null -ne $p -and $null -ne $p.PSObject.Properties['DisplayName'] -and
                ($p.DisplayName -eq 'Mikai' -or $p.DisplayName -eq 'Mikai Ja Nai' -or
                    $p.DisplayName -like 'Mikai Ja Nai *')) -or
                $_.PSChildName -like '{0D3BBE76-AC5C-48E3-9EBF-8563798364AA}*'
        })
    }
    if ($existing.Count -gt 0) { throw 'A Mikai uninstall registration already exists for this user; refusing installation/upgrade.' }
    $script:Evidence.checks.noExistingInstall = $true

    $script:Evidence.registryBefore = Get-IntegrationSnapshot
    $root = Join-Path ([IO.Path]::GetTempPath()) ('Mikai installer smoke ' + [guid]::NewGuid().ToString('N') + ' 測試')
    New-Item -ItemType Directory -Path $root | Out-Null
    Assert-NoReparsePath $root
    $script:Evidence.testRoot = $root
    $installPath = Join-Path $root 'Install with spaces 日本語'
    $group = 'Mikai Smoke ' + [guid]::NewGuid().ToString('N')
    $exe = Join-Path $installPath 'mikai.exe'
    $programs = [Environment]::GetFolderPath([Environment+SpecialFolder]::Programs)
    $groupPath = Join-Path $programs $group
    if (Test-Path -LiteralPath $groupPath) {
        throw "The expected Start Menu group is already present; refusing to alter it: $groupPath"
    }
    $shortcut = Join-Path $groupPath 'Mikai Ja Nai.lnk'
    $script:Evidence.installPath = $installPath
    $script:Evidence.startMenuGroup = $group
    $args = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-','/CURRENTUSER',
        ('/DIR="' + $installPath + '"'),('/GROUP="' + $group + '"'),
        ('/LOG="' + (Join-Path $out 'install-initial-inno.log') + '"'))
    $installAttempted = $true
    Invoke-Bounded 'install-initial' $installerPath $args
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Installer did not create mikai.exe at requested path.' }
    if (-not (Test-Path -LiteralPath $shortcut -PathType Leaf)) { throw 'Expected Start Menu shortcut is missing.' }
    Assert-NoReparsePath $shortcut
    $link = Get-ShortcutMetadata $shortcut
    if ([IO.Path]::GetFullPath($link.TargetPath) -ine [IO.Path]::GetFullPath($exe) -or
        [IO.Path]::GetFullPath($link.WorkingDirectory) -ine [IO.Path]::GetFullPath($installPath)) {
        throw "Start Menu shortcut metadata is incorrect (target='$($link.TargetPath)', workingDirectory='$($link.WorkingDirectory)')."
    }
    $script:Evidence.checks.exeInstalled = $true
    $script:Evidence.checks.shortcutMetadata = $true
    $payloadFileCount = Assert-InstalledPayload $installPath
    $script:Evidence.payloadFileCount = $payloadFileCount
    $script:Evidence.checks.installedPayloadMatchesManifest = $true
    # App-adjacent data paths, observed from NativePaths.cpp, AnimeJanaiConfig.cpp,
    # and CrashLog.cpp. Never touch QSettings: that belongs to the real user profile.
    $models = Join-Path $installPath 'native\animejanai\models\custom\smoke-unknown-model.bin'
    $config = Join-Path $installPath 'native\animejanai\animejanai_manager.conf'
    $logs = Join-Path $installPath 'mikai.log'
    $sentinels = @(
        @{path=$models; bytes=[byte[]](0,255,19,77,0,3)},
        @{path=$config; bytes=[byte[]](91,115,101,116,116,105,110,103,115,93,10,65,61,49)},
        @{path=$logs; bytes=[byte[]](0,76,79,71,255,10)}
    )
    foreach ($s in $sentinels) {
        Assert-NoReparsePath $s.path
        Write-Sentinel $s.path $s.bytes
    }

    $upgradeArgs = @($args)
    $upgradeArgs[-1] = '/LOG="' + (Join-Path $out 'install-upgrade-inno.log') + '"'
    Invoke-Bounded 'install-upgrade' $installerPath $upgradeArgs
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Upgrade removed or failed to restore executable.' }
    $upgradePayloadFileCount = Assert-InstalledPayload $installPath @($sentinels | ForEach-Object { $_.path })
    if ($upgradePayloadFileCount -ne $payloadFileCount) { throw 'Upgrade changed the installed payload inventory count.' }
    $script:Evidence.checks.upgradePayloadMatchesManifest = $true
    foreach ($s in $sentinels) { Assert-Sentinel $s.path $s.bytes }
    $snapshotUpgrade = Get-IntegrationSnapshot
    Assert-SameSnapshot $script:Evidence.registryBefore $snapshotUpgrade 'upgrade'
    $script:Evidence.checks.upgradePreservedSentinels = $true
    $script:Evidence.checks.foreignIntegrationsUnchangedAfterUpgrade = $true

    $uninstaller = Join-Path $installPath 'unins000.exe'
    if (-not (Test-Path -LiteralPath $uninstaller -PathType Leaf)) { throw 'Installer uninstaller executable is missing.' }
    Assert-NoReparsePath $uninstaller
    Invoke-Bounded 'uninstall' $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
    if (Test-Path -LiteralPath $exe) { throw 'Uninstall left mikai.exe in the owned install directory.' }
    foreach ($s in $sentinels) { Assert-Sentinel $s.path $s.bytes }
    $snapshotUninstall = Get-IntegrationSnapshot
    Assert-SameSnapshot $script:Evidence.registryBefore $snapshotUninstall 'uninstall'
    if (Test-Path -LiteralPath $shortcut) { throw 'Uninstall left its owned Start Menu shortcut.' }
    $script:Evidence.registryAfter = $snapshotUninstall
    $script:Evidence.checks.uninstallRemovedExe = $true
    $script:Evidence.checks.uninstallRemovedShortcut = $true
    $script:Evidence.checks.sentinelsSurviveUninstall = $true
    $script:Evidence.checks.foreignIntegrationsUnchangedAfterUninstall = $true
    $installCleaned = $true
    $script:Evidence.status = 'passed'
} catch {
    $script:Evidence.error = [ordered]@{
        message = $_.Exception.Message
        exceptionType = $_.Exception.GetType().FullName
        exception = $_.Exception.ToString()
        fullyQualifiedErrorId = $_.FullyQualifiedErrorId
        category = $_.CategoryInfo.Category.ToString()
    }
    throw
} finally {
    if ($installAttempted -and -not $installCleaned -and $null -ne $root -and
        $null -ne $installPath -and (Test-Path -LiteralPath $installPath -PathType Container)) {
        $cleanupErrors = [Collections.Generic.List[string]]::new()
        $uninstaller = Join-Path $installPath 'unins000.exe'
        $uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{0D3BBE76-AC5C-48E3-9EBF-8563798364AA}_is1'
        $registeredPath = $null
        try {
            if (Test-Path -LiteralPath $uninstallKey) {
                $registration = Get-ItemProperty -LiteralPath $uninstallKey -ErrorAction Stop
                if ($null -ne $registration -and $null -ne $registration.PSObject.Properties['InstallLocation']) {
                    $registeredPath = [IO.Path]::GetFullPath([string]$registration.InstallLocation).TrimEnd('\')
                }
            }
            if ($script:terminationFailed) {
                $cleanupErrors.Add('Installer process-tree termination failed; no cleanup process was started, and the owned root was retained.')
            } elseif ((Test-Path -LiteralPath $uninstaller -PathType Leaf) -and
                $null -ne $registeredPath -and
                [string]::Equals($registeredPath, [IO.Path]::GetFullPath($installPath).TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
                try {
                    Assert-NoReparsePath $uninstaller
                    Invoke-Bounded 'failure-cleanup-uninstall' $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
                } catch {
                    $cleanupErrors.Add("Owned local uninstaller failed: $($_.Exception.Message)")
                }
            } else {
                $cleanupErrors.Add('Owned install registration and local uninstaller could not both be verified; install root retained.')
            }
        } catch {
            $cleanupErrors.Add("Could not verify owned install cleanup: $($_.Exception.Message)")
        }
        if ($null -ne $shortcut -and (Test-Path -LiteralPath $shortcut -PathType Leaf) -and
            $null -ne $exe) {
            try {
                Assert-NoReparsePath $shortcut
                $link = Get-ShortcutMetadata $shortcut
                if ([string]::Equals([IO.Path]::GetFullPath($link.TargetPath),
                    [IO.Path]::GetFullPath($exe), [StringComparison]::OrdinalIgnoreCase)) {
                    Remove-Item -LiteralPath $shortcut -Force -ErrorAction Stop
                    $groupPath = Split-Path -Parent $shortcut
                    if ((Test-Path -LiteralPath $groupPath -PathType Container) -and
                        -not (Get-ChildItem -LiteralPath $groupPath -Force | Select-Object -First 1)) {
                        Remove-Item -LiteralPath $groupPath -Force -ErrorAction Stop
                    }
                } else {
                    $cleanupErrors.Add('Retaining the Start Menu shortcut because its target does not match the owned executable.')
                }
            } catch {
                $cleanupErrors.Add("Owned Start Menu shortcut cleanup failed: $($_.Exception.Message)")
            }
        }
        if ($cleanupErrors.Count -eq 0 -and
            -not (Test-Path -LiteralPath $exe) -and
            -not (Test-Path -LiteralPath $uninstallKey) -and
            ($null -eq $shortcut -or -not (Test-Path -LiteralPath $shortcut))) {
            $installCleaned = $true
        } elseif ($cleanupErrors.Count -gt 0) {
            $script:Evidence.cleanupErrors = @($cleanupErrors.ToArray())
        } else {
            $script:Evidence.cleanupErrors = @('Owned installation cleanup remains incomplete; temporary root retained for recovery.')
        }
    }
    if ($installAttempted -and -not $installCleaned -and $null -ne $root -and
        -not (Test-Path -LiteralPath $installPath -PathType Container)) {
        $script:Evidence.cleanupErrors = @($script:Evidence.cleanupErrors) +
            @('No complete owned install directory was available for local uninstaller cleanup; temporary root retained.')
    }
    if ($null -ne $root -and (Test-Path -LiteralPath $root)) {
        if ($installAttempted -and -not $installCleaned) {
            $script:Evidence.retainedInstallRoot = $root
        } else {
            try {
                Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction Stop
            } catch {
                $script:Evidence.cleanupErrors = @($script:Evidence.cleanupErrors) + @("Temporary root cleanup failed: $($_.Exception.Message)")
                $script:Evidence.retainedInstallRoot = $root
            }
        }
    }
    try {
        Write-Evidence
    } catch {
        [Console]::Error.WriteLine("Could not write installer smoke evidence: $($_.Exception.Message)")
    }
}
