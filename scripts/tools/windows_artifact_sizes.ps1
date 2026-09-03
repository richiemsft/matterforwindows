# Copyright (c) 2026 Project CHIP Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

<#
.SYNOPSIS
    Report deterministic Windows PE/COFF artifact sizes for a GN/Ninja build tree.

.DESCRIPTION
    Opt-in release-engineering helper for the native Windows port. It measures
    the on-disk size of static libraries (.lib COFF archives) and linked
    executables (.exe / .dll PE images) produced under an `out\<dir>` build
    directory, using only built-in PowerShell file APIs. It does NOT use the
    ELF-only binary-size-comparison workflow and has no external dependencies.

    Sizes are toolset-, configuration-, and CRT-dependent. The Windows port
    default is a debug build (is_debug=true) with the dynamic debug CRT (/MDd,
    /Od, /Z7); .lib archives therefore embed /Z7 object debug info and are far
    larger than the code that survives linking. Always record the build
    configuration alongside the numbers (this script prints the target's
    args.gn when present).

    Optionally, when -IncludeMachineType is supplied and `dumpbin` is
    resolvable (via an active MSVC environment or -DumpbinPath), each artifact's
    COFF machine type is reported (8664 = x64, AA64 = ARM64). dumpbin is never
    required for the size report.

.PARAMETER OutDir
    One or more build output directories to measure (e.g. out\win-crypto-tests-x64).

.PARAMETER Filter
    Optional wildcard filter applied to the artifact path relative to OutDir
    (e.g. '*crypto*'). Default measures every .lib, .exe and .dll.

.PARAMETER IncludeMachineType
    Also report the COFF machine type via dumpbin (optional, best-effort).

.PARAMETER DumpbinPath
    Explicit path to dumpbin.exe. If omitted, the script tries the PATH.

.PARAMETER Json
    Emit the result set as JSON instead of a formatted table.

.EXAMPLE
    pwsh scripts\tools\windows_artifact_sizes.ps1 -OutDir out\win-crypto-tests-x64,out\win-crypto-tests-arm64 -Filter *crypto*

.EXAMPLE
    pwsh scripts\tools\windows_artifact_sizes.ps1 -OutDir out\win-crypto-tests-x64 -IncludeMachineType
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]] $OutDir,

    [string] $Filter = "*",

    [switch] $IncludeMachineType,

    [string] $DumpbinPath,

    [switch] $Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Dumpbin {
    param([string] $Explicit)

    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
        Write-Warning "dumpbin not found at -DumpbinPath '$Explicit'; machine type will be omitted."
        return $null
    }

    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    Write-Warning "dumpbin.exe not on PATH; run from a Developer PowerShell or pass -DumpbinPath. Machine type will be omitted."
    return $null
}

function Get-MachineType {
    param([string] $Dumpbin, [string] $Path)

    if (-not $Dumpbin) { return "" }
    $line = & $Dumpbin /HEADERS $Path 2>$null | Select-String -Pattern 'machine \((.+)\)' | Select-Object -First 1
    if ($line -and $line.Matches.Count -gt 0) { return $line.Matches[0].Groups[1].Value }
    return ""
}

$dumpbin = $null
if ($IncludeMachineType) { $dumpbin = Resolve-Dumpbin -Explicit $DumpbinPath }

$rows = New-Object System.Collections.Generic.List[object]

foreach ($dir in $OutDir) {
    if (-not (Test-Path -LiteralPath $dir)) {
        Write-Warning "Skipping missing directory: $dir"
        continue
    }

    $root = (Resolve-Path -LiteralPath $dir).Path
    $argsFile = Join-Path $root "args.gn"
    $config = if (Test-Path -LiteralPath $argsFile) {
        ((Get-Content -LiteralPath $argsFile) -replace '\s+', ' ') -join ' '
    } else {
        "(no args.gn)"
    }

    # Deterministic order: sort by relative path (ordinal).
    $files = Get-ChildItem -LiteralPath $root -Recurse -File -Include *.lib, *.exe, *.dll -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName.Substring($root.Length + 1) -like $Filter } |
        Sort-Object { $_.FullName.Substring($root.Length + 1) } -CaseSensitive

    foreach ($f in $files) {
        $rel = $f.FullName.Substring($root.Length + 1)
        $machine = if ($IncludeMachineType) { Get-MachineType -Dumpbin $dumpbin -Path $f.FullName } else { "" }
        $rows.Add([pscustomobject]@{
            OutDir  = $dir
            Config  = $config
            Artifact = $rel
            Bytes   = [int64] $f.Length
            KiB     = [math]::Round($f.Length / 1KB, 1)
            Machine = $machine
        })
    }
}

if ($Json) {
    $rows | ConvertTo-Json -Depth 4
    return
}

$byDir = $rows | Group-Object OutDir
foreach ($g in $byDir) {
    Write-Host ""
    Write-Host "=== $($g.Name) ===" -ForegroundColor Cyan
    Write-Host "config: $($g.Group[0].Config)"
    $cols = @('Artifact', 'Bytes', 'KiB')
    if ($IncludeMachineType) { $cols += 'Machine' }
    $g.Group | Format-Table -Property $cols -AutoSize
    $total = ($g.Group | Measure-Object -Property Bytes -Sum).Sum
    Write-Host ("total: {0:N0} bytes ({1:N1} KiB) across {2} artifacts" -f $total, ($total / 1KB), $g.Group.Count)
}
