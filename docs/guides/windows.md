# Building Matter Natively on Windows

The Matter SDK includes development-preview support for building native Windows
controller tools, example applications, and Python bindings with MSVC. These
builds run directly on Windows without WSL or a POSIX compatibility layer.

The supported development environment is Windows 11 on x64 or ARM64. The port
uses PowerShell, MSVC, GN, and Ninja.

## Supported targets

The native Windows build provides:

-   `chip-tool.exe`, including on-network commissioning, operational
    interaction, the local interactive shell, and the WebSocket command server.
-   `all-devices-app.exe`, with dynamically configured endpoints and Windows
    named-pipe test-event injection.
-   `msvc-windows-all-clusters.exe`, which runs the generated all-clusters data
    model.
-   A native Python controller wheel containing `_ChipDeviceCtrl.dll`.
-   Native platform implementations for storage, networking, DNS-SD,
    diagnostics, and Bluetooth LE.

Both x64 and ARM64 targets build with MSVC. The repository's Windows CI builds
both architectures and runs the application and platform smoke tests on x64.
Selected targets have also been run on native ARM64 hardware.

This support is a development preview. See
[Limitations](#limitations) before using it as the basis for a product.

## Prerequisites

Install:

-   Windows 11.
-   Visual Studio with the **Desktop development with C++** workload and the
    MSVC build tools for the target architecture.
-   Python 3.11 or newer, available as `python3.exe` or `python.exe` on
    `PATH`.
-   PowerShell 5.1 or newer.
-   Git for Windows.

Enable long paths for the checkout:

```powershell
git config core.longpaths true
```

Clone the repository with its submodules, or initialize the submodules in an
existing checkout:

```powershell
git submodule update --init --recursive
```

## Set up the build environment

Run all commands from the repository root. Dot-source the setup script so its
environment changes remain in the current PowerShell process:

```powershell
. .\scripts\setup\windows.ps1 -Architecture x64
```

Use `-Architecture arm64` in a native ARM64 shell or when cross-compiling for
ARM64.

The script:

-   discovers the installed Visual Studio toolchain;
-   downloads the repository-pinned GN, Ninja, and ZAP tools into
    `.environment\windows`; and
-   creates an isolated Python environment containing the constrained build
    dependencies.

Run the setup script again when switching target architectures. The active MSVC
environment must match the GN `target_cpu`.

## Build the Windows applications

The following configuration builds the application set used by Windows CI:

```powershell
$output = "out\win-apps-x64"
$arguments = @(
    'target_os="win"'
    'target_cpu="x64"'
    'chip_device_platform="windows"'
    'chip_windows_canonical_compile_probes=true'
    'chip_windows_device_layer_probe=true'
    'chip_windows_build_chip_tool=true'
    'chip_windows_build_all_devices_app=true'
    'chip_config_enable_groupcast=true'
    'chip_windows_enable_cxx20=true'
    'chip_with_nlfaultinjection=false'
    'chip_build_tests=false'
    'chip_build_tools=true'
    'chip_caller_handles_critical_failure=true'
    'config_use_interactive_mode=true'
    'config_enable_yaml_tests=true'
    'config_enable_https_requests=true'
) -join ' '

gn gen $output --args=$arguments
ninja -C $output all-devices-app chip-tool msvc-windows-all-clusters
```

For ARM64, initialize the ARM64 environment, change `target_cpu` to `arm64`,
and use a separate output directory such as `out\win-apps-arm64`.

## Run all-devices-app

`all-devices-app.exe` creates its endpoint composition at runtime. For example,
the following command creates an On/Off Light on endpoint 1 and an Occupancy
Sensor on endpoint 2:

```powershell
.\out\win-apps-x64\all-devices-app.exe `
    --device on-off-light:1 `
    --device occupancy-sensor:2 `
    --discriminator 1234 `
    --KVS out\win-apps-x64\all-devices-state `
    --interface-id -1
```

The application prints its setup QR code and manual pairing code before
starting the Matter event loop. Preserve the KVS directory to retain the
commissioned identity between runs. Use a new directory when a fresh test
identity is required.

For the available simulated device types, see
[supported device types](../../examples/all-devices-app/docs/supported_device_types.md).
For its command-line behavior, see the
[startup guide](../../examples/all-devices-app/docs/starting_up.md).

## Run all-clusters-app

The Windows all-clusters application loads the generated all-clusters data
model:

```powershell
.\out\win-apps-x64\msvc-windows-all-clusters.exe
```

The optional first argument limits the run time in seconds:

```powershell
.\out\win-apps-x64\msvc-windows-all-clusters.exe 180
```

Its persistent state is stored in `windows-all-clusters-kvs` under the current
working directory.

## Use chip-tool

Start `chip-tool` without arguments to list the generated command groups:

```powershell
.\out\win-apps-x64\chip-tool.exe
```

Commission a device that is advertising a commissioning window:

```powershell
.\out\win-apps-x64\chip-tool.exe pairing code 1 <setup-code>
```

After commissioning, interact with the device using the same commands as other
host builds. For example:

```powershell
.\out\win-apps-x64\chip-tool.exe onoff read on-off 1 1
.\out\win-apps-x64\chip-tool.exe onoff toggle 1 1
```

Controller state is stored under the Windows temporary directory by default.
Use `--storage-directory <path>` to select an explicit location that can be
reused across processes.

Start the local interactive shell:

```powershell
.\out\win-apps-x64\chip-tool.exe interactive start `
    --storage-directory .\chip-tool-state
```

Enter `quit` or `quit()` to exit.

Start the WebSocket command server used by the Python YAML adapter:

```powershell
.\out\win-apps-x64\chip-tool.exe interactive server --port 9102
```

HTTPS requests, including Distributed Compliance Ledger commands, use WinHTTP
and the Windows certificate and hostname validation APIs.

## Build the Python controller

Build the x64 wheel and install it into a virtual environment:

```powershell
.\scripts\tools\windows_python_controller.ps1 `
    -Architecture x64 `
    -InstallVirtualEnv out\venv
```

Use `-Architecture arm64` to build an ARM64 wheel. Only use
`-InstallVirtualEnv` when the host Python architecture matches the target
architecture.

The Windows Python controller uses the same native platform, storage, DNS-SD,
transport, and controller implementations as `chip-tool`. Bluetooth LE
commissioning uses the C++/WinRT central implementation.

## Run the Windows smoke tests

Build the platform and application smoke targets:

```powershell
ninja -C out\win-apps-x64 `
    msvc-canonical-controller-stack-smoke `
    msvc-canonical-platform-smoke `
    msvc-key-value-store-smoke `
    msvc-windows-ble-smoke `
    msvc-windows-configuration-manager-smoke
```

Run them on x64:

```powershell
.\out\win-apps-x64\msvc-canonical-controller-stack-smoke.exe
.\out\win-apps-x64\msvc-canonical-platform-smoke.exe
.\out\win-apps-x64\msvc-key-value-store-smoke.exe
.\out\win-apps-x64\msvc-windows-ble-smoke.exe
.\out\win-apps-x64\msvc-windows-configuration-manager-smoke.exe
```

The repository's
[`windows-native.yml`](../../.github/workflows/windows-native.yml) workflow is
the source of truth for the complete CI build and smoke-test command lines.

## Windows platform behavior

### Networking and discovery

The Windows Device Layer uses WinSock for IPv4 and IPv6 networking. DNS-SD uses
the native Windows DNS service-discovery APIs and the OS-managed mDNS responder;
applications do not bind UDP port 5353 themselves.

The Windows **DNS Client** service must be running. Network policy must permit
mDNS and Matter traffic for the active network profile. Production
applications should use application-specific firewall rules rather than
opening ports globally.

Matter devices using Thread are reached over operational IPv6 through an
external Thread Border Router. The Windows port does not include a local
Thread stack.

### Bluetooth LE

The C++/WinRT Bluetooth backend supports controller/central and
commissionee/peripheral roles. Live operation requires a Bluetooth adapter and
driver that support the required role. Packaged applications must declare the
`bluetooth` device capability.

The Bluetooth smoke test exercises the state machine and no-adapter behavior;
it does not replace over-the-air interoperability testing on the target
hardware.

### Persistent storage

The native key-value store uses versioned files, atomic replacement, process
locking, and user-specific access controls. Controller applications should use
separate storage directories when they must maintain independent Matter
fabrics.

Deleting an application's KVS directory resets its local Matter identity. Do
not delete it when testing persistence across restarts.

## Limitations

The native Windows port is not yet a production support commitment:

-   Windows 10, 32-bit x86, MinGW, clang-cl, and MSBuild-only builds are not
    supported.
-   Bluetooth LE and external Thread interoperability still require broader
    hardware coverage across x64 and ARM64.
-   A complete certification suite against physical devices has not been run
    on every supported architecture and transport.
-   Same-host tests can be affected by the Windows mDNS responder not resolving
    a service published by that same host. A single Bluetooth radio also may
    not discover its own advertisement.
-   The Python test harness does not support Perfetto tracing, packet capture,
    application output pipes, or stdin-pipe mode on Windows.
-   CI application ZIP files are unsigned validation artifacts, not release
    packages.
-   The preview does not provide an ABI or long-term servicing guarantee.

Before shipping a product, provide:

-   protected operational keys and unique IPKs;
-   an approved device-attestation trust and revocation policy;
-   signed binaries and constrained DLL loading;
-   explicit firewall and packaged-application capability declarations;
-   native validation on every supported architecture and hardware
    configuration; and
-   ownership for updates, dependency servicing, vulnerability response, and
    crash diagnostics.
