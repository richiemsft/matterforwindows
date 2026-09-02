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
-   Toolchain and Base64 SDK-library smoke executables that build on both
    architectures and run on x64.

The default Windows GN graph is intentionally restricted to bootstrap targets.
The Matter core libraries remain disabled until Windows System and Inet
backends exist. This avoids presenting successful GN generation and a
platform-neutral library build as a usable Matter SDK port.

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
ninja -C out\win-msvc-smoke
.\out\win-msvc-smoke\msvc-toolchain-smoke.exe
.\out\win-msvc-smoke\msvc-sdk-smoke.exe
```

## Cross-build the ARM64 smoke target

Initialize a new PowerShell process for the ARM64 compiler environment:

```powershell
. .\scripts\setup\windows.ps1 -Architecture arm64
gn gen out\win-arm64-msvc-smoke --args='target_os="win" target_cpu="arm64" chip_device_platform="none"'
ninja -C out\win-arm64-msvc-smoke
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

## Compatibility inventory

The inventory below describes the first supported target closure rather than
every example in the repository. Failures are classified so build-system work
does not hide missing runtime behavior behind stubs.

| Target area | Reusable code | Windows gap | Classification |
|---|---|---|---|
| Core and support | TLV, data model types, encoders, containers, and most protocol logic | GNU-only flags and attributes, POSIX headers in transitive targets, and untested dependency closures | Compiler and build syntax |
| System | Generic timers, packet buffers, and layer contracts | `pthread_mutex_t`, POSIX clocks, pipe/eventfd wakeups, `select` assumptions, and Unix errors | Platform contract and POSIX API |
| Inet | Address types and endpoint contracts | Integer descriptors, BSD socket calls, `errno`, `fcntl`, `ifaddrs`, and interface-name conversion | Platform contract and POSIX API |
| Crypto | CryptoPAL API and credential logic | No selected MSVC x64/ARM64 backend closure | Dependency |
| Device Layer | Generic static-polymorphism mixins | No Windows platform composition, lifecycle, connectivity, storage, diagnostics, logging, or reset implementation | Platform contract |
| DNS-SD | Resolver and advertiser interfaces | No Windows DNS Service Discovery implementation or firewall guidance | Platform contract |
| BLE | Transport and commissioning state machines | No WinRT scanner, central connection, GATT server, advertising, or callback serialization | Platform contract |
| Controller | Portable command and controller logic | Build closure, storage paths, cancellation, terminal behavior, BLE, and DNS-SD | Platform and application |
| Server | Portable cluster and Interaction Model code | No Windows host lifecycle, network driver, event transport, named-pipe replacement, or example target | Platform and application |
| Tests | Portable C++ test bodies and Python suites | Pigweed host toolchain assumptions, executable naming, process control, paths, BLE hardware, and ARM64 runners | Build and test harness |

The port must not cast a WinSock `SOCKET` to `int`. `SOCKET` is pointer-sized
on 64-bit Windows, while the existing POSIX endpoint implementation frequently
stores descriptors as signed integers.

## Architecture decisions

These decisions define the initial ABI and platform boundaries. A decision may
be revised before Windows is declared stable, but changes must be deliberate
and applied consistently to all dependencies.

### Build and runtime library

-   GN and Ninja remain the canonical source graph and build runner.
-   MSVC is used in C++17 conforming mode with `/permissive-`,
    `/Zc:preprocessor`, exceptions and RTTI disabled, and UTF-8 source handling.
-   The dynamic CRT is the initial ABI contract: `/MDd` for debug and `/MD` for
    release. Static CRT consumers must rebuild the complete SDK and every
    dependency consistently rather than mixing CRT models.
-   x64 and ARM64 are distinct target environments. GN rejects a target CPU
    that differs from the environment initialized by `windows.ps1`.

### System and networking

-   Windows sockets will use a typed native handle whose invalid value is
    `INVALID_SOCKET`; no narrowing conversion to a POSIX descriptor is allowed.
-   The first event loop will preserve the existing System Layer callback
    contract using `WSAPoll` and a WinSock wake-socket pair. IOCP can be
    evaluated later without changing application APIs.
-   `SRWLOCK` is the preferred non-recursive mutex primitive.
-   Monotonic time will use `QueryPerformanceCounter`; UTC time will use a
    precise `FILETIME` API and convert from the Windows epoch with checked
    arithmetic.
-   Interfaces and addresses will be enumerated with `GetAdaptersAddresses`,
    preserving Windows interface indices and IPv6 scope IDs.

### Platform services

-   Windows DNS Service Discovery APIs are the primary DNS-SD backend. The port
    will not start a competing unconditional responder on UDP port 5353.
-   BLE central and peripheral roles use C++/WinRT
    `Windows.Devices.Bluetooth` APIs. Async completions must be marshalled onto
    the Matter event loop, and cancellation must not retain destroyed
    delegates.
-   Tool and test state defaults to a versioned directory under
    `%LOCALAPPDATA%`. Server/service deployments receive an injectable
    machine-scoped path with explicit ACL ownership.
-   Both unpackaged desktop applications and packaged applications are design
    targets. Packaging-dependent capabilities must be detected and reported;
    they must not silently change BLE or storage behavior.
-   Windows uses OS-managed Ethernet and Wi-Fi in the first release. Thread
    devices are reached through an external border router, matching the Darwin
    host model.

### Crypto decision gate

The crypto ABI is not selected yet. BoringSSL, OpenSSL, and Mbed TLS must each
be evaluated for:

-   MSVC x64 and ARM64 build support.
-   Complete CryptoPAL test behavior.
-   Binary size and runtime dependencies.
-   FIPS or enterprise deployment implications.
-   Repository-managed acquisition, patching, licensing, and security updates.

BoringSSL is the leading compatibility candidate because it is already used by
Darwin, but Windows support will not be claimed until the comparison and tests
are complete.

## Delivery phases and exit criteria

1. Audit compiler, dependency, POSIX API, and platform-contract gaps.
2. Port System clocks, locking, errors, wake handling, and event dispatch.
3. Port Inet sockets, interface enumeration, multicast, and scoped IPv6.
4. Add the Windows Device Layer, persistence, diagnostics, and DNS-SD.
5. Add C++/WinRT BLE commissioner and commissionee support.
6. Bring up the controller CLI and Windows `all-clusters-app`.
7. Add x64 and ARM64 unit, integration, BLE, and external-Thread-border-router
   CI.

| Phase | Required exit |
|---|---|
| Build bootstrap | A clean PowerShell command builds a real SDK library and runtime check for x64 and ARM64. |
| System and Inet | Core tests pass on x64; ARM64 builds and runs on native hardware. UDP/TCP, multicast, scoped IPv6, timer wakeup, and shutdown have regression coverage. |
| Device Layer and IP | A controller discovers and commissions on-network, persists a fabric across restart, subscribes, and removes the fabric. |
| BLE | Windows commissions Wi-Fi and Thread devices over BLE and can expose a commissionable Windows server over BLE. |
| Applications | Native controller and `all-clusters-app` binaries run on a clean Windows 11 host and survive repeated commission/uncommission cycles. |
| CI and support | Required x64 and ARM64 jobs, interoperability coverage, artifacts, troubleshooting, and an explicit support matrix are published. |

## Feature status

| Surface | x64 build | x64 runtime | ARM64 build | ARM64 runtime |
|---|---|---|---|---|
| MSVC toolchain smoke | Supported | Supported | Supported | Not yet run on native hardware |
| Base64 SDK library smoke | Supported | Supported | Supported | Not yet run on native hardware |
| Core Matter SDK | Not yet supported | Not yet supported | Not yet supported | Not yet supported |
| Controller CLI | Not yet supported | Not yet supported | Not yet supported | Not yet supported |
| Server application | Not yet supported | Not yet supported | Not yet supported | Not yet supported |
| DNS-SD | Not yet supported | Not yet supported | Not yet supported | Not yet supported |
| BLE central/peripheral | Not yet supported | Not yet supported | Not yet supported | Not yet supported |
| Thread through border router | Blocked by controller/IP work | Not yet supported | Blocked by controller/IP work | Not yet supported |

## Deployment and security requirements

-   Document inbound and outbound firewall rules by executable and network
    profile; do not require globally opening mDNS or Matter ports.
-   Store fabric and operational credentials with user/service-specific ACLs.
    Writes must be atomic, corruption must be surfaced, and factory reset must
    remove all versioned state owned by that application.
-   Load DLLs through constrained search paths and document the CRT and
    redistributable policy.
-   Treat Bluetooth permissions, adapter-disabled state, multiple adapters,
    cancellation, and packaged capability declarations as tested behavior.
-   Sign release binaries and define ownership for dependency servicing,
    vulnerability response, crash dumps, and updates.
-   Do not claim ARM64 support from cross-compilation alone.

## Known limitations

-   Core, System, Inet, crypto, Device Layer, controller, and server targets do
    not yet compile as complete Windows closures.
-   ARM64 output has been inspected but not executed on native Windows ARM64
    hardware.
-   No Windows CI runner, DNS-SD backend, BLE backend, or persistence provider
    is present yet.
-   Native Wi-Fi provisioning, a local Thread stack, and a Windows Thread
    border router are outside the first-release scope.
