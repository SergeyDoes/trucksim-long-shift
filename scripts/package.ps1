# Packs a release: long_shift.dll with the README, the changelog and the licenses into
# dist/long-shift-<version>.zip, plus its SHA-256 checksum and the release notes for this version
# taken from CHANGELOG.md. Run after building the Release configuration.
param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root "$BuildDir/version.txt") -Raw).Trim()
$name = "long-shift-$version"
$out = Join-Path $root $OutDir
$staging = Join-Path $out $name

if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging | Out-Null
Copy-Item (Join-Path $root "$BuildDir/src/plugin/$Configuration/long_shift.dll") $staging
foreach ($file in "README.md", "CHANGELOG.md", "LICENSE") {
    Copy-Item (Join-Path $root $file) $staging
}
Copy-Item (Join-Path $root "third_party/scs_sdk/sdk_license.txt") (Join-Path $staging "SCS_SDK_LICENSE.txt")

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
"$hash  $name.zip" | Set-Content -Encoding ascii "$zip.sha256"

# Release notes: the CHANGELOG.md section of this version.
$changelog = Get-Content (Join-Path $root "CHANGELOG.md")
$heading = $changelog | Select-String -Pattern "^## $([regex]::Escape($version))\b" | Select-Object -First 1
if (-not $heading) { throw "CHANGELOG.md has no section for $version" }
$notes = for ($i = $heading.LineNumber; $i -lt $changelog.Count -and -not $changelog[$i].StartsWith("## "); $i++) {
    $changelog[$i]
}
($notes -join "`n").Trim() | Set-Content -Encoding utf8 (Join-Path $out "release-notes.md")

Write-Output "$zip"
Write-Output "sha256 $hash"
