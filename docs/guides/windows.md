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

## Application-facing API

[`Matter.Windows.Controller`](https://github.com/dotMorten/matter.winrt) is the
application-facing API for native Windows applications. It packages a C++/WinRT
component, WinRT metadata, and architecture-specific native DLLs for x64 and
ARM64. Applications use that versioned WinRT surface instead of linking to
Matter SDK C++ types or depending on their ABI.

The API is device-type agnostic. Matter device types are compositions of
clusters on endpoints, and the generic API addresses attributes, commands, and
events by endpoint, cluster, and field identifiers. It supports generic reads,
writes, invokes, attribute subscriptions, event reads, event subscriptions,
and the timed writes and invokes required by clusters such as Door Lock,
Energy EVSE, Administrator Commissioning, and Thread management. Consequently,
device-type coverage follows the data model in the pinned Matter SDK rather
than a list of WinRT device classes. Typed Basic Information, On/Off, and Level
Control clients are conveniences, not the supported device-type boundary.

Generic Matter TLV values project recursively through `IPropertySet`. Decimal
context-tag strings identify structure fields, `value` wraps a scalar root,
inspectable vectors represent lists, and byte vectors represent octet strings.

The WinRT package is independently versioned and pins a specific revision of
this fork. Its metadata is the intended application contract, but it remains a
development preview: applications must deploy the native DLL from the same
package version used at build time. Neither project currently promises
compatibility between preview package versions or exposes the Matter SDK's C++
ABI as a supported application interface. Stable WinRT releases will follow
semantic versioning: additive changes increment the minor version and breaking
changes require a new major version. A Matter SDK pin update requires a package
version change and x64/ARM64 component and sample validation.

## Camera signaling build components

Native camera signaling components can be compiled independently of the
upstream Python WebRTC example and its libdatachannel media receiver. Add
`chip_build_webrtc_signaling=true` to the GN arguments for a Windows controller
build, leaving `chip_support_webrtc_python_bindings=false`. Both x64 and ARM64
use the same switch. The default is `false`, preserving the controller-only
build.

The native Python-controller packaging script also accepts the opt-in switch:

```powershell
.\scripts\tools\windows_python_controller.ps1 -Architecture x64 -BuildCameraSignaling
```

This compiles the native components and component-test executable; it does not
add Python camera-session APIs or run the tests automatically.

The controller then depends on
`//src/controller/webrtc:chip_webrtc_signaling`, which supplies the WebRTC
Transport Requestor server, typed WebRTC Transport Provider commands through
the existing native controller interactions, an owned code-driven requestor
endpoint, and generated Descriptor and Camera AV Stream Management definitions.
It does not include `WebRTCClient`, `WebRTCTransportRequestorManager`, the
example access-control delegate, Python callbacks, or libdatachannel.

Native consumers can include `controller/CHIPControllerBuildConfig.h` and
inspect `CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING` (`0` or `1`). This indicates
compiled components only, not an initialized camera endpoint or available
video playback. The private SDK archive's consumers must use generated
headers from the same build as the archive.

These components use the shared generated camera cluster contracts, Interaction
Model command sender, CASE/TCP transport, Descriptor cluster, and code-driven
endpoint provider. They do not define a Windows-specific signaling protocol.
The owned requestor adds bounded queues, peer/fabric authorization, early-answer
correlation, and explicit lifecycle handling; its initial scope is client-offer
LiveView, not every flow supported by the upstream Python WebRTC harness.

`camera signaling-check` is an optional native diagnostic, not a replacement for
the upstream `src/python_testing/TC_WEBRTC_*.py` camera tests. Those tests also
exercise stream allocation, ICE exchange, actual WebRTC connection establishment,
and, where applicable, video reception using libdatachannel or a browser. Building
the Windows signaling components alone does not make that Python harness or media
backend available.

The signaling regression tests live in
`src/controller/tests/TestCameraSignaling.cpp` and
`src/controller/tests/TestCameraTransport.cpp` and are also included in the
ordinary controller test suite when `chip_build_webrtc_signaling=true`. The
Windows test executable compiles those same sources rather than a separate
Windows-only camera test implementation.

The transport tests use real IPv4/IPv6 TCP sockets. Most use encrypted sessions
with injected test keys; `CameraCaseTransportTests` performs the shared CASE
handshake with upstream operational test certificates and derived session keys.
The CASE tests cover large offer/answer commands, peer-reset cleanup, and
rejection of a mismatched identity-protection key. Windows platform state for
these tests uses a temporary, isolated storage directory, not controller storage.
Together the transport tests cover 16 KiB SDP, large ICE batches in both directions,
100 offer/answer/end cycles, oversized and stale commands, cancellation, and
prompt exchange cleanup after a peer reset. Shared `CommandSender` reports
session loss through its timeout callback even when the command timer has not
expired. These tests do not exercise commissioning, stream allocation,
real-camera interoperability, or decoded video.

With the switch enabled, build and run the Windows signaling-component tests:

```powershell
ninja -C out\win-apps-x64 msvc-webrtc-signaling-tests
.\out\win-apps-x64\msvc-webrtc-signaling-tests.exe
```

The shared Binding manager's owned-lifecycle regression tests also have a native
Windows target:

```powershell
ninja -C out\win-apps-x64 msvc-binding-lifecycle-tests
.\out\win-apps-x64\msvc-binding-lifecycle-tests.exe
```

Use the existing [Binding cluster](../../src/app/clusters/bindings/README.md#owned-manager-lifecycle)
for hosted endpoints that require it, rather than a Windows-specific Binding
implementation. An owned manager must detach before controller/factory teardown.

`CameraRequestorEndpoint` registers requestor and Descriptor clusters together
with endpoint metadata, including its WebRTC Transport Provider client cluster,
in a `CodeDrivenDataModelProvider`. Use
`FindAvailableEndpoint` rather than assuming endpoint 1 is free. Supply
device-type metadata appropriate to the selected Matter revision. The provider,
fabric table, system layer, and metadata spans must outlive the owned endpoint;
unregister it before shutting down its provider.

The overload accepting an initialized `Binding::Manager` and platform manager
also registers the SDK's existing Binding cluster transactionally. Its borrowed
manager must outlive the endpoint and detach before controller teardown forgets
the fabric. `CameraRequestorEndpoint::CheckAccess` authorizes active CASE peers
to read Descriptor/Binding metadata and invoke supported signaling commands;
it does not grant Binding writes or administration. The original two-cluster
constructor remains available for signaling-only diagnostics.

`CameraSignalingCluster` supports client-generated offers. Create a local session
before sending `ProvideOffer`; early Answer and ICE commands are correlated and
copied into owned payloads. `CameraProviderClient` sends typed offers, candidates,
and end commands, and validates provider response stream identities. Establish
CASE using `TransportPayloadCapability::kLargePayload` before supplying a
session to this client: UDP-only, PASE, and mismatched peers/fabrics are rejected.
Command inputs are encoded synchronously; command results and inbound signals
are polled independently using `TakeResult` and `TakeSignal`.
The `SendOffer` overload accepting ICE servers encodes caller-supplied STUN/TURN
configuration synchronously, preserving optional username, credential and CAID.
Limits are four servers, four URLs per server, 512 bytes per URI and 256 bytes
per username or credential. Empty optional credentials remain present; absent
fields stay absent. The original overload omits ICE servers. Callers configure
their receiver with matching servers and must not log credentials or URLs.
Signals preserve protocol-identity presence using `std::optional<uint16_t>`:
an unbound failure has no identifier, and a bound identifier of zero stays zero.

Stack-facing operations, cancellation, and destruction require the Matter stack
lock. The two polling methods are thread-safe and do not invoke application
callbacks. An initialized application access-control policy remains required.
For the owned requestor path only, that policy can delegate to `CheckAccess`,
which permits authorized CASE peers with active local sessions and rejects
unrelated paths and privileges. It does not replace existing ACL policy.

The runtime bounds concurrent sessions to 8, queued signaling messages to 32,
SDP to 16 KiB, and candidates to 16 per message. Negotiation and provider-command
timeouts are 30 seconds. Queue overflow, timeout, fabric removal, and shutdown
produce observable failures. Closing a session immediately reclaims queued
payload capacity. Retired protocol identities are not reused at the same
requestor endpoint: after 128 distinct identities, or cancellation before a
protocol identity is known, create a fresh requestor endpoint rather than
accepting ambiguous late callbacks. A client must be destroyed before its
requestor, and a command result must be consumed before starting another command.
`GetSessionInfoForEnd` exposes retained failed-session metadata only for remote
cleanup; it does not restore inbound authorization. `SendEnd` can use a known
authenticated early protocol identity even if the offer response did not arrive.
Unknown or contradictory identities must not be guessed. Cancellation queues a
command result, which must also be consumed before a cleanup command.

These are native integration primitives, not a supported live-camera
implementation. Applications must still wire the endpoint into their active
controller, explicitly select/allocate and deallocate camera streams, attempt
bounded remote cleanup on cancellation, and provide a receiver.
The example requestor manager is not a production authorization or endpoint
registration policy. WinRT camera APIs and receiver integration belong in
[`dotMorten/matter.winrt`](https://github.com/dotMorten/matter.winrt), not this
repository.

The native SDK and WinRT SDK are separate projects: `chip-tool` must not depend
on WinRT, and WinRT projections, packaging, and receiver UI stay in the WinRT
repository. Use independent sibling checkouts and an explicit `-MatterSdkRoot`
when building the WinRT component against a working native SDK.

NativeAOT consumption, live large-payload signaling, actual camera
interoperability, and playback are not validated by compiling these components
or running the native lifecycle and descriptor-chunking tests. No media backend
or recording is enabled by this switch.

With this flag, `chip-tool` also exposes the native-only `camera signaling-check`
diagnostic. It selects an owned native code-driven provider and allocates an
available requestor endpoint only for this standalone command, leaving other
commands' providers unchanged. It enables authenticated incoming CASE/TCP and
verifies the provider
response and Answer, then attempts bounded remote End; it is not a media player
or a complete Camera Controller product endpoint. See the
[chip-tool camera diagnostic](../../examples/chip-tool/README.md#native-camera-signaling-diagnostic)
for stream ownership, port selection, optional sensitive transcripts, and
real-device prerequisites. Default builds and interactive sessions retain their
existing behavior.

## Preview support matrix

The preview support contract is intentionally narrow. A configuration is
supported only when it is represented by the Windows CI workflow or explicitly
listed below.

| Area | Supported preview baseline |
| --- | --- |
| Operating system | Windows 11 for development and execution |
| Architectures | Native x64 and native ARM64 processes |
| Compiler | MSVC from Visual Studio 2022 17.14 or newer, with the **Desktop development with C++** workload |
| Windows SDK | 10.0.26100.0 or newer |
| Build system | Repository-pinned GN, Ninja, and ZAP tools initialized by `scripts\setup\windows.ps1` |
| PowerShell | Windows PowerShell 5.1 or PowerShell 7 |
| Python | Python 3.11 or newer matching the target process architecture |
| Bluetooth LE | Controller/central and commissionee/peripheral roles, subject to adapter and driver support |
| Thread | Operational IPv6 through an external Thread Border Router; no local Thread stack or border router |
| Distribution | Unsigned application ZIPs and native Python wheels for validation; `Matter.Windows.Controller` provides the separately versioned application-facing WinRT package |

The reproducible CI baseline uses the pinned `windows-2025` GitHub runner for
x64 and `windows-11-vs2026-arm` for ARM64. Runner image servicing can update
installed patch versions, but the runner labels and minimum tool versions above
remain fixed for this preview.

## Prerequisites

Install:

-   Windows 11.
-   Visual Studio 2022 17.14 or newer with the **Desktop development with C++**
    workload, Windows SDK 10.0.26100.0 or newer, and the MSVC build tools for
    the target architecture.
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

### Representative device validation

In addition to light-device testing, the Windows `chip-tool.exe` has been
independently exercised with a Shelly EM Mini Gen4 energy-monitoring device.
The reported validation:

-   commissioned the device onto a second Matter fabric while its existing Home
    Assistant fabric remained connected;
-   read the Basic Information and Descriptor data;
-   discovered Electrical Power Measurement, Electrical Energy Measurement,
    Power Topology, and Identify clusters on endpoint 1; and
-   read live cumulative imported-energy data.

This community-reported result demonstrates a non-lighting device type,
multi-admin commissioning, endpoint discovery, and measurement-cluster
interaction. It is representative interoperability evidence, not an exhaustive
hardware matrix or Matter certification result.

### Persistent storage

The native key-value store uses versioned files, atomic replacement, process
locking, and user-specific access controls. Controller applications should use
separate storage directories when they must maintain independent Matter
fabrics.

Deleting an application's KVS directory resets its local Matter identity. Do
not delete it when testing persistence across restarts.

## Limitations

The native Windows port is not yet a production support commitment:

-   [Windows 10, 32-bit x86, MinGW, clang-cl, and MSBuild-only
    builds](https://github.com/richiemsft/matterforwindows/issues/1) are not
    supported.
-   Bluetooth LE and external Thread interoperability still require broader
    [hardware coverage across x64 and
    ARM64](https://github.com/richiemsft/matterforwindows/issues/2).
-   A complete certification suite against physical devices has not been run
    on every supported architecture and transport.
-   Same-host tests can be affected by the Windows mDNS responder not resolving
    a service published by that same host. A single Bluetooth radio also may
    not discover its own advertisement.
-   The Python test harness does not support Perfetto tracing, packet capture,
    application output pipes, or stdin-pipe mode on Windows.
-   CI application ZIP files are
    [unsigned validation artifacts](https://github.com/richiemsft/matterforwindows/issues/3),
    not release packages.
-   `Matter.Windows.Controller` is the application-facing API, but preview
    versions do not yet provide a
    [stable ABI guarantee](https://github.com/richiemsft/matterforwindows/issues/4)
    or long-term servicing commitment.
-   [Production packaging and
    distribution](https://github.com/richiemsft/matterforwindows/issues/5)
    remain deferred.

Before shipping a product, provide:

-   protected operational keys and unique IPKs;
-   an approved device-attestation trust and revocation policy;
-   signed binaries and constrained DLL loading;
-   explicit firewall and packaged-application capability declarations;
-   native validation on every supported architecture and hardware
    configuration; and
-   ownership for updates, dependency servicing, vulnerability response, and
    crash diagnostics.
