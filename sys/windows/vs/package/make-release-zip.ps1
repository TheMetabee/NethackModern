<#
.SYNOPSIS
    NetHack Modern: zip the Windows build for release.

.DESCRIPTION
    Run by the package project (package.nmake) after it has gathered the
    game into <repo>\vsbinary; it can be run by hand too.

    Takes everything in vsbinary except test output (fxlog.txt and
    friends), adds the current README.md and Guidebook (.pdf and .txt)
    straight from the repo, puts it all under a top-level "NethackModernW"
    folder, and writes
    <repo>\vspackage\NetHackModern-v<version>-windows-x64.zip, where
    <version> is MODERN_VERSION_STRING in include\fx.h (for example
    v0.3.0). The name matches the Linux build's,
    NetHackModern-v<version>-x86_64.AppImage.

    Files taken from the repo replace any same-named copy in vsbinary, so
    the zip always has the latest docs even if vsbinary is stale.

    A signature left from an earlier zip of the same name is removed;
    sign-release.ps1 makes the new one.

.PARAMETER Repo
    The NetHack Modern checkout. Defaults to the one this script is in.

.PARAMETER Version
    Use this instead of the version in include\fx.h, e.g. -Version v0.3.0-rc1.

.PARAMETER Exclude
    File name patterns to leave out, matched against names anywhere in
    vsbinary. Defaults to fxlog*.txt.
#>
[CmdletBinding()]
param(
    [string]$Repo = (Join-Path $PSScriptRoot '..\..\..\..'),
    [string]$Version,
    [string[]]$Exclude = @('fxlog*.txt')
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path $Repo).Path
$folderName = 'NethackModernW'          # the folder inside the zip
$zipBase = 'NetHackModern'              # the zip: <base>-<version>-<platform>.zip
$platform = 'windows-x64'

# Files copied from the repo itself: name in the zip -> path in the repo.
$fromRepo = [ordered]@{
    'README.md'     = 'README.md'
    'Guidebook.pdf' = 'doc\Guidebook.pdf'
    'Guidebook.txt' = 'doc\Guidebook.txt'
}

$source = Join-Path $Repo 'vsbinary'
$destDir = Join-Path $Repo 'vspackage'
if (-not (Test-Path $source)) { throw "No vsbinary folder at $source - build the game first." }

# ---- version: the one the game shows (include\fx.h) ---------------------
if (-not $Version) {
    $fx = Join-Path $Repo 'include\fx.h'
    $m = Select-String -Path $fx -Pattern '#define\s+MODERN_VERSION_STRING\s+"([^"]+)"'
    if (-not $m) { throw "No MODERN_VERSION_STRING in $fx; pass -Version." }
    $Version = 'v' + $m.Matches[0].Groups[1].Value
}

# ---- collect files ----------------------------------------------------
$root = (Resolve-Path $source).Path.TrimEnd('\') + '\'

# Each entry: zip path (relative to the top folder) and the file on disk.
$entries = [ordered]@{}
foreach ($name in $fromRepo.Keys) {
    $path = Join-Path $Repo $fromRepo[$name]
    if (-not (Test-Path $path)) { throw "Missing $path in the repo." }
    $entries[$name] = (Get-Item $path).FullName
}
Get-ChildItem -Path $source -Recurse -File | ForEach-Object {
    $name = $_.Name
    if ($Exclude | Where-Object { $name -like $_ }) { return }
    $rel = $_.FullName.Substring($root.Length).Replace('\', '/')
    if (-not $entries.Contains($rel)) { $entries[$rel] = $_.FullName }
}
if ($entries.Count -eq $fromRepo.Count) { throw "Nothing from vsbinary to package in $source." }

$skipped = Get-ChildItem -Path $source -Recurse -File | Where-Object {
    $name = $_.Name
    $Exclude | Where-Object { $name -like $_ }
}

# ---- write zip --------------------------------------------------------
if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir | Out-Null }
$zipPath = Join-Path $destDir ("{0}-{1}-{2}.zip" -f $zipBase, $Version, $platform)
if (Test-Path $zipPath) { Remove-Item $zipPath -Confirm:$false }
# a signature of the old zip would fail for the new one: away with it
if (Test-Path "$zipPath.minisig") { Remove-Item "$zipPath.minisig" -Confirm:$false }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$stream = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
try {
    $zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($rel in $entries.Keys) {
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $entries[$rel], "$folderName/$rel",
                [System.IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $zip.Dispose() }
} finally { $stream.Dispose() }

$sizeMB = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host ("Packed {0} files into {1} ({2} MB)" -f $entries.Count, $zipPath, $sizeMB)
Write-Host ("From the repo: {0}" -f ($fromRepo.Keys -join ', '))
if ($skipped) { Write-Host ("Left out: {0}" -f (($skipped | ForEach-Object Name) -join ', ')) }
# the updater installs only signed downloads; signing asks for the key's
# password, so it's a step of its own
Write-Host ("Next: sign it with {0} (writes {1}.minisig)" -f
            (Join-Path $PSScriptRoot 'sign-release.ps1'), (Split-Path $zipPath -Leaf))
