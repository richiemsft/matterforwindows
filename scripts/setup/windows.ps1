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

[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string] $Architecture = "x64",

    [string] $ToolsDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $ToolsDirectory) {
    $ToolsDirectory = Join-Path $repoRoot ".environment\windows"
}

$gnInstance = "6KccwOIUe7fkTjqmAJhCuY51aVrCKpbiSXk10DnD4T4C"
$ninjaInstance = "3TgJ1Ckw_8bqhqyrAzltNPV1oYhZClrVWwXWMVE8RVgC"

function Install-CipdTool {
    param(
        [Parameter(Mandatory)]
        [string] $PackageUri,

        [Parameter(Mandatory)]
        [string] $InstanceId,

        [Parameter(Mandatory)]
        [string] $Executable,

        [Parameter(Mandatory)]
        [string] $Destination
    )

    $executablePath = Join-Path $Destination $Executable
    $instancePath = Join-Path $Destination ".cipd-instance"
    if ((Test-Path $executablePath) -and (Test-Path $instancePath) -and
        ((Get-Content $instancePath -Raw).Trim() -eq $InstanceId)) {
        return $executablePath
    }

    $stagingDirectory = "$Destination.install-$PID"
    if (Test-Path $stagingDirectory) {
        Remove-Item -Path $stagingDirectory -Recurse -Force
    }

    try {
        New-Item -ItemType Directory -Force -Path $stagingDirectory | Out-Null
        $archivePath = Join-Path $stagingDirectory "$Executable.zip"

        Write-Host "Downloading $Executable from the Chromium CIPD service..."
        Invoke-WebRequest -Uri $PackageUri -OutFile $archivePath
        Expand-Archive -Path $archivePath -DestinationPath $stagingDirectory -Force
        Remove-Item -Path $archivePath

        $stagedExecutable = Join-Path $stagingDirectory $Executable
        if (-not (Test-Path $stagedExecutable)) {
            throw "$Executable was not present in the downloaded package."
        }

        Set-Content -Path (Join-Path $stagingDirectory ".cipd-instance") `
            -Value $InstanceId -NoNewline
        if (Test-Path $Destination) {
            Remove-Item -Path $Destination -Recurse -Force
        }
        Move-Item -Path $stagingDirectory -Destination $Destination
    } finally {
        if (Test-Path $stagingDirectory) {
            Remove-Item -Path $stagingDirectory -Recurse -Force
        }
    }

    return (Join-Path $Destination $Executable)
}

$programFilesX86 = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::ProgramFilesX86)
$vsInstallerDirectory = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer"
$vswhere = Join-Path $vsInstallerDirectory "vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Installer was not found. Install Visual Studio with the Desktop development with C++ workload."
}

$installationPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $installationPath) {
    throw "No Visual Studio installation with the MSVC x64 tools was found."
}

$vcvarsall = Join-Path $installationPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvarsall)) {
    throw "vcvarsall.bat was not found under $installationPath."
}

$vcvarsArchitecture = if ($Architecture -eq "arm64") {
    "x64_arm64"
} else {
    "x64"
}

$environmentCommand =
    "set `"PATH=$vsInstallerDirectory;%PATH%`" && call `"$vcvarsall`" $vcvarsArchitecture >nul && set"
$environmentLines = & $env:ComSpec /d /s /c $environmentCommand
if ($LASTEXITCODE -ne 0) {
    throw "Visual Studio environment initialization failed for $Architecture."
}

foreach ($line in $environmentLines) {
    if ($line -match "^([^=]+)=(.*)$") {
        Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
    }
}

$gnDirectory = Join-Path $ToolsDirectory "gn"
$ninjaDirectory = Join-Path $ToolsDirectory "ninja"
$gn = Install-CipdTool `
    -PackageUri "https://chrome-infra-packages.appspot.com/dl/gn/gn/windows-amd64/+/$gnInstance" `
    -InstanceId $gnInstance `
    -Executable "gn.exe" `
    -Destination $gnDirectory
$ninja = Install-CipdTool `
    -PackageUri "https://chrome-infra-packages.appspot.com/dl/infra/3pp/tools/ninja/windows-amd64/+/$ninjaInstance" `
    -InstanceId $ninjaInstance `
    -Executable "ninja.exe" `
    -Destination $ninjaDirectory

$env:PATH = "$gnDirectory;$ninjaDirectory;$env:PATH"

Write-Host "Matter Windows build environment initialized for $Architecture."
Write-Host "  MSVC:  $env:VCToolsVersion"
Write-Host "  GN:    $gn"
Write-Host "  Ninja: $ninja"
