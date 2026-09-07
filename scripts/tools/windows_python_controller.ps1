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

[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string] $Architecture = "x64",

    [string] $OutputRoot,

    [string] $InstallVirtualEnv,

    [switch] $BuildTestApp
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$env:PW_PROJECT_ROOT = $RepoRoot.Path.Replace("\", "/")

if (-not $OutputRoot) {
    $OutputRoot = Join-Path $RepoRoot "out\windows-python-$Architecture"
}

. (Join-Path $RepoRoot "scripts\setup\windows.ps1") -Architecture $Architecture

$gnArguments = @(
    'target_os="win"'
    "target_cpu=`"$Architecture`""
    'chip_device_platform="windows"'
    'chip_windows_build_python_controller=true'
    'chip_windows_enable_cxx20=true'
    'chip_stack_lock_tracking="fatal"'
    'chip_config_network_layer_ble=true'
    'chip_enable_ble=true'
    'chip_with_lwip=false'
    'chip_with_nlfaultinjection=false'
    'chip_build_tests=false'
    'chip_build_tools=false'
    'chip_support_webrtc_python_bindings=false'
    'chip_caller_handles_critical_failure=true'
    'matter_enable_tracing_support=true'
    'is_debug=false'
)
if ($BuildTestApp) {
    $gnArguments += 'chip_windows_build_all_devices_app=true'
}
$gnArguments = $gnArguments -join " "

& gn "--root=$RepoRoot" gen $OutputRoot "--args=$gnArguments"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$buildPython = Join-Path $RepoRoot ".environment\windows\python\Scripts\python3.exe"
if (-not (Test-Path $buildPython -PathType Leaf)) {
    throw "Windows build Python was not found at $buildPython"
}
& $buildPython -m pip install --quiet setuptools wheel
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$targets = @(
    "ChipDeviceCtrl",
    "matter-clusters",
    "matter-core"
)
if ($BuildTestApp) {
    $targets += "all-devices-app"
}
& ninja -C $OutputRoot @targets
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$controllerDll = Join-Path $OutputRoot "obj\src\controller\python\_ChipDeviceCtrl.dll"
if (-not (Test-Path $controllerDll -PathType Leaf)) {
    throw "Native controller DLL was not generated at $controllerDll"
}

$packageRoots = @(
    (Join-Path $OutputRoot "obj\src\controller\python\matter-clusters"),
    (Join-Path $OutputRoot "obj\src\controller\python\matter-core")
)
$wheelRoot = Join-Path $OutputRoot "python-wheels"
New-Item -Path $wheelRoot -ItemType Directory -Force | Out-Null
foreach ($packageRoot in $packageRoots) {
    if (-not (Test-Path $packageRoot -PathType Container)) {
        throw "Python package staging directory was not generated at $packageRoot"
    }
    & $buildPython -m pip wheel --quiet --no-deps --no-build-isolation --wheel-dir $wheelRoot $packageRoot
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

$wheels = @(Get-ChildItem -Path $wheelRoot -Filter "*.whl" -File)
if ($wheels.Count -ne 2) {
    throw "Expected controller and cluster wheels; found $($wheels.Count)"
}

if ($InstallVirtualEnv) {
    $hostArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString().ToLowerInvariant()
    if ($hostArchitecture -ne $Architecture) {
        throw "Cannot install $Architecture wheels with the $hostArchitecture host Python runtime"
    }

    & python -m venv $InstallVirtualEnv
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $venvPython = Join-Path $InstallVirtualEnv "Scripts\python.exe"
    & $venvPython -m pip install --upgrade --no-cache-dir alive-progress click colorama diskcache mobly python-path tabulate
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    & $venvPython -m pip install --upgrade --force-reinstall --no-cache-dir @($wheels.FullName)
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $sitePackages = & $venvPython -c "import site; print(site.getsitepackages()[0])"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    $testingPackage = Join-Path $OutputRoot "python-test-infrastructure"
    $testingMatterPackage = Join-Path $testingPackage "matter"
    if (Test-Path $testingMatterPackage) {
        Remove-Item -Path $testingMatterPackage -Recurse -Force
    }
    New-Item -Path $testingPackage -ItemType Directory -Force | Out-Null
    Copy-Item -Path (Join-Path $RepoRoot "src\python_testing\matter_testing_infrastructure\matter") `
        -Destination $testingPackage -Recurse

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($version in @("1.1", "1.2", "1.3", "1.4", "1.4.1", "1.4.2", "1.5", "1.5.1", "1.6", "1.6.1")) {
        $archiveDirectory = Join-Path $testingMatterPackage "testing\data_model\$version"
        New-Item -Path $archiveDirectory -ItemType Directory -Force | Out-Null
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            (Join-Path $RepoRoot "data_model\$version"),
            (Join-Path $archiveDirectory "allfiles.zip"))
    }
    foreach ($credentialSet in @("development", "production")) {
        $archiveDirectory = Join-Path $testingMatterPackage "testing\credentials\$credentialSet"
        New-Item -Path $archiveDirectory -ItemType Directory -Force | Out-Null
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            (Join-Path $RepoRoot "credentials\$credentialSet"),
            (Join-Path $archiveDirectory "allfiles.zip"))
    }

    $testingPackage = (Resolve-Path $testingPackage).Path
    Set-Content -Path (Join-Path $sitePackages "matter_testing_source.pth") -Value $testingPackage -Encoding utf8
}

Write-Host "Native controller: $controllerDll"
$wheels | ForEach-Object { Write-Host "Python wheel: $($_.FullName)" }
