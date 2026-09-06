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
    Verify a native Windows Matter application package.

.DESCRIPTION
    Checks the package layout, deterministic entry metadata, architecture
    metadata, and every payload size and SHA-256 digest in manifest.json.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ArchivePath,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "arm64")]
    [string] $Architecture
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedArchive = (Resolve-Path -LiteralPath $ArchivePath).Path
$packageName = "matter-windows-$Architecture"
$prefix = "$packageName/"
$requiredPayload = @(
    "all-clusters-app.exe",
    "all-devices-app.exe",
    "chip-tool.exe",
    "licenses/BoringSSL-LICENSE.txt",
    "licenses/Matter-LICENSE.txt",
    "licenses/Matter-NOTICE.txt",
    "README.txt"
)
$expectedEntries = @($requiredPayload | ForEach-Object { "$prefix$_" }) + "${prefix}manifest.json"

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][IO.Stream] $Stream)

    $reader = New-Object IO.BinaryReader($Stream, [Text.Encoding]::ASCII, $true)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Payload is not a PE executable."
        }
        if ($reader.ReadBytes(0x3A).Count -ne 0x3A) {
            throw "Payload has a truncated DOS header."
        }
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -lt 0x40 -or $reader.ReadBytes($peOffset - 0x40).Count -ne ($peOffset - 0x40)) {
            throw "Payload has an invalid PE header offset."
        }
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Payload has an invalid PE signature."
        }
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($resolvedArchive)
try {
    $entryNames = @($archive.Entries | ForEach-Object FullName)
    if ($entryNames.Count -ne (@($entryNames | Sort-Object -Unique -CaseSensitive)).Count) {
        throw "Package contains duplicate entries."
    }
    if (($entryNames -join "`n") -ne ((@($entryNames | Sort-Object -CaseSensitive)) -join "`n")) {
        throw "Package entries are not in deterministic ordinal order."
    }
    if (($entryNames -join "`n") -ne ((@($expectedEntries | Sort-Object -CaseSensitive)) -join "`n")) {
        throw "Package contents do not match the required application bundle."
    }

    $fixedTimestamp = [datetime] "2000-01-01T00:00:00"
    foreach ($entry in $archive.Entries) {
        if ($entry.FullName.Contains("\") -or $entry.FullName.Contains("../")) {
            throw "Package contains an unsafe entry path: $($entry.FullName)"
        }
        if ($entry.LastWriteTime.DateTime -ne $fixedTimestamp) {
            throw "Package entry has a noncanonical timestamp: $($entry.FullName)"
        }
    }

    $manifestEntry = $archive.GetEntry("${prefix}manifest.json")
    $manifestReader = [IO.StreamReader]::new($manifestEntry.Open())
    try {
        $manifest = $manifestReader.ReadToEnd() | ConvertFrom-Json
    } finally {
        $manifestReader.Dispose()
    }

    if ($manifest.formatVersion -ne 1) {
        throw "Unsupported package manifest version: $($manifest.formatVersion)"
    }
    if ($manifest.architecture -ne $Architecture) {
        throw "Manifest architecture '$($manifest.architecture)' does not match $Architecture."
    }
    if (@($manifest.files).Count -ne $requiredPayload.Count) {
        throw "Manifest does not describe every required payload file."
    }
    if ((@($manifest.files.path | Sort-Object -CaseSensitive) -join "`n") -ne ($requiredPayload -join "`n")) {
        throw "Manifest file list does not match the package payload."
    }

    foreach ($file in $manifest.files) {
        $entry = $archive.GetEntry($prefix + $file.path)
        if ($entry.Length -ne $file.bytes) {
            throw "Size mismatch for '$($file.path)'."
        }

        $sha256 = [Security.Cryptography.SHA256]::Create()
        $entryStream = $entry.Open()
        try {
            $actualHash = [BitConverter]::ToString($sha256.ComputeHash($entryStream)).Replace("-", "").ToLowerInvariant()
        } finally {
            $entryStream.Dispose()
            $sha256.Dispose()
        }
        if ($actualHash -ne $file.sha256) {
            throw "SHA-256 mismatch for '$($file.path)'."
        }

        if ($file.path.EndsWith(".exe", [StringComparison]::OrdinalIgnoreCase)) {
            $expectedMachine = if ($Architecture -eq "x64") { 0x8664 } else { 0xAA64 }
            $entryStream = $entry.Open()
            try {
                $actualMachine = Get-PeMachine -Stream $entryStream
            } finally {
                $entryStream.Dispose()
            }
            if ($actualMachine -ne $expectedMachine) {
                throw "PE machine mismatch for '$($file.path)': expected 0x$($expectedMachine.ToString('X4')), got 0x$($actualMachine.ToString('X4'))."
            }
        }
    }
} finally {
    $archive.Dispose()
}

Write-Host "Verified Windows package: $resolvedArchive"
