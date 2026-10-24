[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$OutputDir = 'release-artifacts',
    [string]$ISCC = 'ISCC.exe',
    [switch]$RequirePublicReady
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Assert-NoReparsePoint([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing a symbolic-link/junction packaging path: $($item.FullName)"
        }
        $item = $item.Parent
    }
}
function Assert-FreshDirectory([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Assert-NoReparsePoint $Path
        if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw "Not a directory: $Path" }
        if (@(Get-ChildItem -LiteralPath $Path -Force).Count -ne 0) {
            throw "Output directory is populated and will not be overwritten: $Path"
        }
    } else {
        $parent = Split-Path -Parent $Path
        while (-not (Test-Path -LiteralPath $parent)) { $parent = Split-Path -Parent $parent }
        Assert-NoReparsePoint $parent
    }
}

if ($env:OS -ne 'Windows_NT') { throw 'Windows packaging must run on Windows with the configured x64 MinGW toolchain.' }
$build = (Resolve-Path -LiteralPath $BuildDir).Path
Assert-NoReparsePoint $build
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output = [IO.Path]::GetFullPath($OutputDir)
Assert-FreshDirectory $output
$metadataPath = Join-Path $build 'mikai-package-Release.json'
$stageScript = Join-Path $build 'mikai-package-Release.cmake'
if (-not (Test-Path -LiteralPath $metadataPath) -or -not (Test-Path -LiteralPath $stageScript)) {
    throw 'Configure a Release build of this checkout first (Qt 6.12, x64 MinGW, optional native/model inspection OFF).'
}
$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
if ($metadata.configuration -ne 'Release' -or
    [IO.Path]::GetFullPath($metadata.sourceDir).TrimEnd('\', '/') -ine $source.TrimEnd('\', '/')) {
    throw 'The build packaging metadata does not belong to this checkout/Release configuration.'
}
$version = [string]$metadata.version
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Invalid project release version: $version" }
$manifestPath = Join-Path $source 'packaging/third-party.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$profile = $manifest.profiles.'windows-x64'
$readinessValue = $profile.publicReleaseReady
if ($null -eq $readinessValue -or $readinessValue.GetType() -ne [bool]) {
    throw 'packaging/third-party.json profiles.windows-x64.publicReleaseReady must be a JSON Boolean.'
}
$publicReady = $readinessValue
if ($RequirePublicReady -and -not $publicReady) {
    throw "Public publication is blocked by packaging/third-party.json: $($profile.blockers | ConvertTo-Json -Compress -Depth 12)"
}
if (-not $publicReady) {
    Write-Warning 'Preparing PRIVATE REVIEW artifacts only. Redistribution/publication is blocked; see included third-party.json and release evidence.'
}
$isccCommand = Get-Command $ISCC -CommandType Application -ErrorAction SilentlyContinue
if ($null -eq $isccCommand) {
    # Inno Setup is not normally on PATH after a standard per-machine install.
    foreach ($root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:LOCALAPPDATA)) {
        if (-not $root) { continue }
        foreach ($relative in @('Inno Setup 6/ISCC.exe', 'Programs/Inno Setup 6/ISCC.exe')) {
            $candidate = Join-Path $root $relative
            if (Test-Path -LiteralPath $candidate) { $isccCommand = Get-Command $candidate; break }
        }
        if ($null -ne $isccCommand) { break }
    }
}
if ($null -eq $isccCommand) { throw 'Install Inno Setup 6.3+ separately, put ISCC.exe on PATH, or pass -ISCC <path>. No toolchain is downloaded by this script.' }
$cmake = (Get-Command cmake -CommandType Application).Source
# Building the actual app is intentional; no probes, test or native targets are packaged.
Invoke-Checked $cmake @('--build', $build, '--config', 'Release', '--target', 'mikai')

# Stage in an unguessable sibling under our explicitly fresh output, never in
# dist-mikai or an installation. Failure leaves evidence intact, not a wipe.
Assert-FreshDirectory $output
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stage = Join-Path $output ('.stage-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage "mikai-$version-windows-x64"
New-Item -ItemType Directory -Path $stage | Out-Null
Invoke-Checked $cmake @("-DDIST_DIR=$payload", '-P', $stageScript)

$base = "mikai-$version-windows-x64"
$zipPath = Join-Path $output "$base-portable.zip"
$setupPath = Join-Path $output "$base-setup.exe"
$evidencePath = Join-Path $output "$base-release.json"
# CMake/libarchive has no Compress-Archive 2GB-per-file limitation and includes
# every staged file, including legal notices. One top-level portable directory.
Invoke-Checked $cmake @('-E', 'chdir', $stage, $cmake, '-E', 'tar', 'cf', $zipPath, '--format=zip', '--', (Split-Path -Leaf $payload))

# Explicit installer entries are generated from the validated payload, rather
# than a recursive wildcard that could consume newly acquired user state.
$entries = @()
$inventory = @()
foreach ($file in @(Get-ChildItem -LiteralPath $payload -Recurse -File -Force | Sort-Object FullName)) {
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Payload contains a reparse point: $($file.FullName)" }
    $relative = $file.FullName.Substring($payload.Length + 1)
    if ($relative -match '["\r\n;]' -or $relative.Contains('..')) { throw "Unsafe package filename: $relative" }
    $directory = Split-Path -Parent $relative
    $destination = '{app}'
    if ($directory) { $destination += '\' + $directory }
    $entries += 'Source: "{#StageDir}\' + $relative + '"; DestDir: "' + $destination + '"; Flags: ignoreversion'
    $owner = if ($relative -match '^mpv\\') { 'mpv' } elseif ($relative -match '^shaders\\') { 'anime4k' } elseif ($relative -match '^licenses\\') { 'legal' } elseif ($relative -match '^lib(gcc|stdc|winpthread)') { 'mingw' } elseif ($relative -match '^Qt6|^(platforms|styles|imageformats|iconengines|tls|networkinformation)\\|^translations\\qt') { 'qt' } else { 'mikai' }
    $inventory += [ordered]@{ path = $relative.Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); bytes = $file.Length; component = $owner; fileVersion = $file.VersionInfo.FileVersion }
}
$filesInclude = Join-Path $stage 'payload-files.iss'
[IO.File]::WriteAllLines($filesInclude, $entries, [Text.UTF8Encoding]::new($true))
Invoke-Checked $isccCommand.Source @('/Qp', "/DAppVersion=$version", "/DStageDir=$payload", "/DOutputDir=$output", "/DPayloadFiles=$filesInclude", (Join-Path $PSScriptRoot 'mikai.iss'))
if (-not (Test-Path -LiteralPath $zipPath) -or -not (Test-Path -LiteralPath $setupPath)) { throw 'Packaging completed without both required artifacts.' }

$checksums = @()
$artifacts = @()
foreach ($artifact in @($zipPath, $setupPath)) {
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $checksums += "$hash  $([IO.Path]::GetFileName($artifact))"
    $artifacts += [ordered]@{ name = [IO.Path]::GetFileName($artifact); sha256 = $hash; bytes = (Get-Item -LiteralPath $artifact).Length }
}
$evidence = [ordered]@{
    schema = 'mikai.windows.release/1'; version = $version; platform = 'windows-x64'; configuration = 'Release'
    preparedAtUtc = [DateTime]::UtcNow.ToString('o'); publicReleaseReady = $publicReady
    publicationStatus = $(if ($publicReady) { 'rights-ready-pending-verification' } else { 'blocked' })
    blockers = $profile.blockers; signature = 'unsigned'; tlsBackend = 'Windows Schannel (OS certificate store)'
    # Get-Content decorates strings with provider metadata that Windows
    # PowerShell 5.1's ConvertTo-Json can traverse; retain only plain text lines.
    versionEvidence = @([IO.File]::ReadAllLines((Join-Path $payload 'VERSION'), [Text.Encoding]::UTF8))
    legalManifestSha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    componentProvenance = $manifest; artifacts = $artifacts; payload = $inventory
    verification = 'Package preparation only; no runtime/installer/clean-machine verification is implied.'
}
[IO.File]::WriteAllText($evidencePath, ($evidence | ConvertTo-Json -Depth 32) + "`n", [Text.UTF8Encoding]::new($false))
$evidenceHash = (Get-FileHash -LiteralPath $evidencePath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksums += "$evidenceHash  $([IO.Path]::GetFileName($evidencePath))"
[IO.File]::WriteAllLines((Join-Path $output 'SHA256SUMS'), $checksums, [Text.UTF8Encoding]::new($false))
# Remove only the freshly generated scratch tree owned by this invocation.
# No installation, state directory, existing deployment or build tree is removed.
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Output "Prepared $zipPath"
Write-Output "Prepared $setupPath"
Write-Output "Evidence $evidencePath (publicationStatus=$($evidence.publicationStatus))"
