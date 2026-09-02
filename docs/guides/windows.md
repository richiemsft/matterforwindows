# Native Windows port

The Matter SDK does not yet provide production-ready native Windows host
libraries or applications. The Windows port is being developed without WSL or
a POSIX compatibility layer, targeting Windows 11 on x64 and ARM64 with MSVC,
GN, and Ninja.

Development takes place in
[`richiemsft/matterforwindows`](https://github.com/richiemsft/matterforwindows)
on the `windows-port` branch. The canonical
`project-chip/connectedhomeip` repository is retained as the `upstream` remote.

## Current status

The initial build foundation provides:

-   MSVC GN toolchains for x64 and ARM64.
-   Windows-specific compiler and linker defaults.
-   A native PowerShell setup script.
-   A toolchain smoke executable that builds on both architectures and runs on
    x64.

The default Windows GN graph is intentionally restricted to the smoke target.
The Matter core libraries remain disabled until Windows System and Inet
backends exist. This avoids presenting a successful GN generation as a usable
Matter SDK port.

## Prerequisites

-   Windows 11.
-   Visual Studio with the **Desktop development with C++** workload and the
    MSVC x64 and ARM64 build tools.
-   PowerShell 5.1 or newer.
-   Git with long paths enabled for this checkout.

## Initialize the build environment

Dot-source the setup script so the Visual Studio, GN, and Ninja environment
changes remain in the current PowerShell process:

```powershell
. .\scripts\setup\windows.ps1 -Architecture x64
```

The script discovers Visual Studio through `vswhere.exe` and downloads GN and
Ninja from Chromium's CIPD service into `.environment\windows` when they are
not already installed. The downloads use content-addressed CIPD instance IDs
recorded in the script so separate workstations acquire the same tool binaries.

## Build the x64 smoke target

```powershell
gn gen out\win-msvc-smoke --args='target_os="win" target_cpu="x64" chip_device_platform="none"'
ninja -C out\win-msvc-smoke build/config/win/tests:msvc-toolchain-smoke
.\out\win-msvc-smoke\msvc-toolchain-smoke.exe
```

## Cross-build the ARM64 smoke target

Initialize a new PowerShell process for the ARM64 compiler environment:

```powershell
. .\scripts\setup\windows.ps1 -Architecture arm64
gn gen out\win-arm64-msvc-smoke --args='target_os="win" target_cpu="arm64" chip_device_platform="none"'
ninja -C out\win-arm64-msvc-smoke build/config/win/tests:msvc-toolchain-smoke
dumpbin /headers out\win-arm64-msvc-smoke\msvc-toolchain-smoke.exe |
    Select-String "machine \(ARM64\)"
```

ARM64 runtime support must be tested on native Windows ARM64 hardware before
the architecture is listed as fully supported.

GN rejects a target architecture that does not match the active MSVC
environment. Re-run the setup script with the matching `-Architecture` value
before generating a differently targeted output directory.

## Intended capability baseline

macOS/Darwin is the functional baseline for the Windows desktop port:

| Capability | macOS today | Windows target |
|---|---|---|
| Core Matter SDK | Supported | Planned |
| Controller CLI | `chip-tool` and Darwin framework tool | Native non-interactive `chip-tool` equivalent |
| Device/server examples | Broad host-example coverage | `all-clusters-app` first |
| Operational IP | IPv4/IPv6, UDP/TCP | WinSock IPv4/IPv6, UDP/TCP |
| Service discovery | Apple DNS-SD | Windows DNS Service Discovery |
| BLE commissioning | CoreBluetooth central and peripheral | C++/WinRT central and peripheral |
| Thread | External border router; no Darwin Thread stack | External border router; no native Thread stack in the first release |
| Persistence | Darwin platform storage | Windows per-user/service storage |

Darwin-specific frameworks are not portable implementation code. Windows will
provide native implementations behind the same Matter System, Inet, BLE, DNS-SD,
and Device Layer contracts.

## Remaining port stages

1. Audit compiler, dependency, POSIX API, and platform-contract gaps.
2. Port System clocks, locking, errors, wake handling, and event dispatch.
3. Port Inet sockets, interface enumeration, multicast, and scoped IPv6.
4. Add the Windows Device Layer, persistence, diagnostics, and DNS-SD.
5. Add C++/WinRT BLE commissioner and commissionee support.
6. Bring up the controller CLI and Windows `all-clusters-app`.
7. Add x64 and ARM64 unit, integration, BLE, and external-Thread-border-router
   CI.

The complete working roadmap is maintained in the implementation plan used by
the Windows port project.
