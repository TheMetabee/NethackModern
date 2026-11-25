<#
.SYNOPSIS
    NetHack Modern: sign the Windows release zip with minisign.

.DESCRIPTION
    Writes <zip>.minisig beside vspackage\NetHackModern-v<version>-windows-x64.zip
    (made by make-release-zip.ps1). The game's updater installs a
    download only if its signature checks out against the public key built
    into the game, and only if the signature names that very file, so an
    older signed release can't be passed off as a newer one.

    minisign asks for the key's password. The signature is then checked
    against the public key in the repo (sys\share\nethackmodern.pub), so a
    zip signed with the wrong key is caught here, not by players.

    Needs minisign (winget install jedisct1.minisign) and the secret key,
    by default where minisign -G puts it: %USERPROFILE%\.minisign\minisign.key.

.PARAMETER Repo
    The NetHack Modern checkout. Defaults to the one this script is in.

.PARAMETER Version
    The version to sign, e.g. v0.3.3. Defaults to MODERN_VERSION_STRING in
    include\fx.h.

.PARAMETER Key
    The minisign secret key file.

.PARAMETER File
    Sign this file instead of the version's zip (its signature names it).
#>
[CmdletBinding()]
param(
    [string]$Repo = (Join-Path $PSScriptRoot '..\..\..\..'),
    [string]$Version,
    [string]$Key = (Join-Path $env:USERPROFILE '.minisign\minisign.key'),
    [string]$File
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path $Repo).Path
$pub = Join-Path $Repo 'sys\share\nethackmodern.pub'

$minisign = (Get-Command minisign -ErrorAction SilentlyContinue).Source
if (-not $minisign) { throw 'minisign is not installed (winget install jedisct1.minisign), or not on PATH yet: open a new terminal.' }
if (-not (Test-Path $Key)) { throw "No secret key at $Key; pass -Key." }
if (-not (Test-Path $pub)) { throw "No public key at $pub." }

if (-not $File) {
    if (-not $Version) {
        $fx = Join-Path $Repo 'include\fx.h'
        $m = Select-String -Path $fx -Pattern '#define\s+MODERN_VERSION_STRING\s+"([^"]+)"'
        if (-not $m) { throw "No MODERN_VERSION_STRING in $fx; pass -Version." }
        $Version = 'v' + $m.Matches[0].Groups[1].Value
    }
    $File = Join-Path $Repo ("vspackage\NetHackModern-{0}-windows-x64.zip" -f $Version)
}
if (-not (Test-Path $File)) { throw "Nothing to sign at $File; run make-release-zip.ps1 (or the package build) first." }
$File = (Resolve-Path $File).Path
$name = Split-Path $File -Leaf
$sig = "$File.minisig"

# the trusted comment is signed too: it names the file, which the game
# checks against the name of what it downloaded
& $minisign -S -s $Key -m $File -x $sig -t "file:$name"
if ($LASTEXITCODE -ne 0) { throw "minisign could not sign $name." }

& $minisign -V -p $pub -m $File -x $sig
if ($LASTEXITCODE -ne 0) {
    Remove-Item $sig -Confirm:$false
    throw "The signature doesn't match ${pub}: wrong key? It was removed."
}
Write-Host "Signed: $sig"
Write-Host "Upload both to the release: $name and $name.minisig"
