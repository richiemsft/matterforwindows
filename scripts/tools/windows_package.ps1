# Copyright (c) 2026 Project CHIP Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

<#
.SYNOPSIS
    Package native Windows Matter applications for deployment.

.DESCRIPTION
    Creates a deterministic ZIP containing chip-tool, all-clusters-app,
    all-devices-app, license material, deployment notes, and a SHA-256 manifest.
    Release GN output (is_debug=false) is required unless -AllowDebug is
    explicitly supplied for local script validation.

    When -CertificateThumbprint is supplied, each executable is Authenticode
    signed before its package hash is computed. The certificate must be
    available in Cert:\CurrentUser\My and include a private key.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $OutDir,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "arm64")]
    [string] $Architecture,

    [string] $DestinationDirectory = "artifacts",

    [string] $CertificateThumbprint,

    [string] $TimestampServer = "http://timestamp.digicert.com",

    [switch] $AllowDebug
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$outRoot = (Resolve-Path -LiteralPath $OutDir).Path
$argsPath = Join-Path $outRoot "args.gn"
if (-not (Test-Path -LiteralPath $argsPath)) {
    throw "Build arguments were not found at '$argsPath'."
}

$buildArguments = (Get-Content -LiteralPath $argsPath -Raw).Trim()
$targetCpuPattern = "(?m)^\s*target_cpu\s*=\s*`"$([regex]::Escape($Architecture))`"\s*$"
if ($buildArguments -notmatch $targetCpuPattern) {
    throw "Build output '$OutDir' does not target $Architecture."
}
if (-not $AllowDebug -and $buildArguments -notmatch '(?m)^\s*is_debug\s*=\s*false\s*$') {
    throw "Refusing to package a debug CRT build. Generate with is_debug=false or use -AllowDebug for local validation."
}

$sourceFiles = [ordered]@{
    "chip-tool.exe" = "chip-tool.exe"
    "msvc-windows-all-clusters.exe" = "all-clusters-app.exe"
    "all-devices-app.exe" = "all-devices-app.exe"
}

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string] $Path)

    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "'$Path' is not a PE executable."
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "'$Path' has an invalid PE signature."
        }
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

$expectedMachine = if ($Architecture -eq "x64") { 0x8664 } else { 0xAA64 }
foreach ($sourceName in $sourceFiles.Keys) {
    $sourcePath = Join-Path $outRoot $sourceName
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "Required application was not found: $sourcePath"
    }
    $machine = Get-PeMachine -Path $sourcePath
    if ($machine -ne $expectedMachine) {
        throw "'$sourcePath' has PE machine type 0x$($machine.ToString('X4')); expected 0x$($expectedMachine.ToString('X4'))."
    }
}

New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
$destinationRoot = (Resolve-Path -LiteralPath $DestinationDirectory).Path
$packageName = "matter-windows-$Architecture"
$packageRoot = Join-Path $destinationRoot $packageName
$archivePath = Join-Path $destinationRoot "$packageName.zip"

if (Test-Path -LiteralPath $packageRoot) {
    $existing = Get-Item -LiteralPath $packageRoot -Force
    if (($existing.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to replace reparse-point package directory '$packageRoot'."
    }
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}

New-Item -ItemType Directory -Path $packageRoot | Out-Null
$licensesRoot = New-Item -ItemType Directory -Path (Join-Path $packageRoot "licenses")

foreach ($sourceName in $sourceFiles.Keys) {
    Copy-Item -LiteralPath (Join-Path $outRoot $sourceName) -Destination (Join-Path $packageRoot $sourceFiles[$sourceName])
}
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $licensesRoot "Matter-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $repoRoot "NOTICE") -Destination (Join-Path $licensesRoot "Matter-NOTICE.txt")
Copy-Item -LiteralPath (Join-Path $repoRoot "third_party\boringssl\repo\src\LICENSE") `
    -Destination (Join-Path $licensesRoot "BoringSSL-LICENSE.txt")

$readme = @"
Matter SDK native Windows applications ($Architecture)

Applications:
  chip-tool.exe         Matter controller CLI
  all-clusters-app.exe  Generated all-clusters test server
  all-devices-app.exe   Dynamic code-driven device simulator

Requirements:
  - Windows 11 on $Architecture
  - Microsoft Visual C++ Redistributable for Visual Studio 2015-2022 ($Architecture)
  - Local-network access for Matter UDP/TCP and mDNS
  - Bluetooth capability and an enabled adapter for BLE commissioning

The applications use per-user persistence by default. chip-tool also accepts
--storage-directory. See docs/guides/windows.md in the source repository for
firewall, commissioning, support, and security details.

This is a non-FIPS build. See licenses\ for the Matter and BoringSSL terms.
Verify every file against manifest.json before deployment.
"@
[IO.File]::WriteAllText((Join-Path $packageRoot "README.txt"), $readme, [Text.UTF8Encoding]::new($false))

if ($CertificateThumbprint) {
    $normalizedThumbprint = $CertificateThumbprint.Replace(" ", "")
    $certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$normalizedThumbprint" -ErrorAction Stop
    if (-not $certificate.HasPrivateKey) {
        throw "The signing certificate does not have an accessible private key."
    }

    foreach ($applicationName in $sourceFiles.Values) {
        $applicationPath = Join-Path $packageRoot $applicationName
        $signature = Set-AuthenticodeSignature -FilePath $applicationPath -Certificate $certificate `
            -HashAlgorithm SHA256 -TimestampServer $TimestampServer
        if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
            throw "Authenticode signing failed for '$applicationName': $($signature.StatusMessage)"
        }
    }
}

$revision = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Unable to determine the source revision."
}

$manifestFiles = Get-ChildItem -LiteralPath $packageRoot -Recurse -File |
    Sort-Object { $_.FullName.Substring($packageRoot.Length + 1) } -CaseSensitive |
    ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($packageRoot.Length + 1).Replace("\", "/")
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }

$manifest = [ordered]@{
    formatVersion = 1
    architecture = $Architecture
    sourceRevision = $revision
    signed = [bool] $CertificateThumbprint
    buildArguments = $buildArguments
    files = @($manifestFiles)
}
$manifestJson = $manifest | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText((Join-Path $packageRoot "manifest.json"), $manifestJson + "`n", [Text.UTF8Encoding]::new($false))

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($archivePath, [IO.Compression.ZipArchiveMode]::Create)
try {
    $fixedTimestamp = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
    $packageFiles = Get-ChildItem -LiteralPath $packageRoot -Recurse -File |
        Sort-Object { $_.FullName.Substring($packageRoot.Length + 1) } -CaseSensitive
    foreach ($file in $packageFiles) {
        $relativePath = $file.FullName.Substring($packageRoot.Length + 1).Replace("\", "/")
        $entry = $archive.CreateEntry("$packageName/$relativePath", [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $fixedTimestamp
        $input = [IO.File]::OpenRead($file.FullName)
        $output = $entry.Open()
        try {
            $input.CopyTo($output)
        } finally {
            $output.Dispose()
            $input.Dispose()
        }
    }
} finally {
    $archive.Dispose()
}

Write-Host "Created Windows package: $archivePath"
Write-Host "SHA256: $((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash)"
