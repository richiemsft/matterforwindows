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
-   Reproducible BoringSSL, Mbed TLS, and JsonCpp dependency prototypes that
    build on x64 and ARM64 and run on x64.
-   A static library built from shared core error and key-ID sources, with 23
    existing core tests running through GoogleTest on x64 and cross-building
    for ARM64.
-   Toolchain and Base64 SDK-library smoke executables that build on both
    architectures and run on x64.
-   Native QPC/FILETIME clock and SRW-lock primitives wired through the shared
    `System::Clock` and `System::Mutex` APIs. This subset builds for x64 and
    ARM64 and has x64 runtime coverage.
-   Native Win32, WinSock, and HRESULT error mapping with UTF-8 system
    descriptions, plus a nonblocking IPv6 loopback wake-socket primitive.
    Both have x64 runtime and ARM64 cross-build coverage.
-   A pointer-width shared System socket-watch contract and a native
    `LayerImplWindows` event loop using `WSAPoll`. Timer expiry, cross-thread
    wake, socket-read dispatch, shutdown, and reinitialization run on x64.
-   A typed WinSock handle with bounded IPv6 UDP and `WSAPoll` runtime
    coverage on x64 and ARM64 cross-build coverage.
-   Native adapter and unicast-address enumeration using
    `GetAdaptersAddresses`, protocol-independent interface LUIDs, interface
    name/index conversion, link state and type classification, hardware
    addresses, prefix lengths, and IPv6 link-local lookup. This has x64 runtime
    and ARM64 cross-build coverage.
-   A native `ConnectivityManager` composition for OS-managed Ethernet and
    Wi-Fi. It reports unsupported Thread, BLE, and SDK-managed Wi-Fi
    provisioning, exposes installed Ethernet/Wi-Fi adapters for diagnostics,
    and selects an operational external interface only when it is up,
    non-loopback, multicast-capable, and has a usable address. Native interface
    and unicast-address notifications are coalesced onto the Matter event loop
    and emit connectivity/address events. The focused smoke passes 21 checks on
    x64 and cross-builds as ARM64.
-   A public `ConfigurationManager` composed on the typed Windows storage
    backend. The canonical `PlatformMgr()` lifecycle initializes and shuts down
    configuration, UDP/TCP endpoint managers, and connectivity; reboot count,
    operational hours, boot reason, regulatory state, configuration version,
    unique ID, persisted counters, and primary MAC selection survive restart.
    A Windows diagnostics provider exposes persisted reboot/boot/operational
    state and fresh Matter-formatted interface, MAC, and IP data. The 45-check
    smoke also validates asynchronous factory reset and event-loop-marshaled
    network-change delivery on x64 and
    cross-builds as ARM64.
-   A native DNS-SD backend (`src/platform/Windows/DnssdImpl.cpp`) implementing
    the full `chip::Dnssd` platform contract over the Win32 `windns.h`
    service-discovery APIs (`DnsServiceRegister`/`Browse`/`Resolve` and their
    `DeRegister`/`Cancel` counterparts), using the native OS mDNS responder
    exclusively. Every native completion is marshaled onto
    `PlatformMgr().ScheduleWork()`; publish/browse/resolve operations are
    reference-counted so late, superseded, or post-shutdown callbacks are
    recognized as stale and cleaned up without ever invoking a Matter
    callback. The 65-check smoke covers deterministic conversion/validation
    seams, the init/shutdown/reinit lifecycle, a live GUID-service publish,
    and a live browse/`StopBrowse()` cancellation cycle (exactly one final
    callback) on x64, and cross-builds as ARM64.
-   The shared Inet UDP socket endpoint (`UDPEndPointImplSockets.cpp`) ported to
    native WinSock behind `#if defined(_WIN32)` branches: `WSASocketW`,
    `closesocket`, `WSAGetLastError` mapping, `WSASendMsg`/`WSARecvMsg` with
    `IP_PKTINFO`/`IPV6_PKTINFO` source/destination/interface control messages,
    ephemeral bind, IPv4 and IPv6 multicast join/leave and loopback, and scoped
    IPv6 via Windows interface indices. Ephemeral bind, packet-info
    send/receive over IPv4 and IPv6 loopback, multicast, and clean shutdown
    through `LayerImplWindows` have x64 runtime and ARM64 cross-build coverage.
-   The shared Inet TCP socket endpoint (`TCPEndPointImplSockets.cpp`) ported to
    native WinSock behind `#if defined(_WIN32)` branches: `WSASocketW`,
    `closesocket`, `ioctlsocket(FIONBIO)` non-blocking sockets,
    `WSAGetLastError` mapping, WinSock `char *`/`int` socket-option argument
    handling, IPv4 and IPv6 ephemeral bind, listen, non-blocking connect,
    accept, bidirectional send/receive, `TCP_NODELAY`/keepalive options, and
    scoped IPv6 via Windows interface indices. A `POLLERR`/`POLLHUP` branch in
    the connecting state lets a `WSAPoll`-reported connect failure complete.
    The Linux/BSD send-queue probe used by the optional TCP user-timeout
    override (`TIOCOUTQ`/`SO_NWRITE`) has no Windows equivalent, so that
    override is disabled for this port. Ephemeral bind/listen/connect/accept,
    peer-address reporting, bidirectional loopback exchange over IPv4 and IPv6,
    and clean shutdown through `LayerImplWindows` have x64 runtime and ARM64
    cross-build coverage.
-   The repository-pinned BoringSSL CryptoPAL compiled with native MSVC and
    BoringSSL assembly disabled (`OPENSSL_NO_ASM`). The focused
    `//src/crypto/windows:windows-crypto-boringssl` closure builds the shared
    `CHIPCryptoPALOpenSSL.cpp`, `P256KeyPairOpenSSL.cpp`, and backend-agnostic
    `CHIPCryptoPAL.cpp` against the pinned BoringSSL, together with the raw key
    store and the supporting ASN1/TLV/support translation units. A focused
    GoogleTest driver drives the real PAL for SHA-256 (one-shot and streaming),
    HMAC-SHA256, HKDF-SHA256, PBKDF2-SHA256, AES-CCM-128 AEAD, AES-CTR-128,
    ECDSA P-256 signatures and keypairs, deterministic ECDSA (RFC 6979), ECDH
    key agreement, the DRBG, constant-time comparison, P-256 keypair
    serialization/deserialization and raw-bit import, PKCS#10 CSR generation and
    verification, X.509 public-key extraction, the SPAKE2+ handshake (RFC
    known-answer and full mutual agreement), the DefaultSessionKeystore
    derive/export/destroy surface, and negative input boundaries. Vectors are
    checked against published NIST/RFC known-answer data (including the SPAKE2+
    draft-01 and NIST ECDSA2VS vectors reproduced from the `src/crypto/tests`
    headers) and algebraic correctness properties. Twenty-three tests
    pass on x64 and the closure cross-builds and inspects as `AA64` for ARM64.
-   The real upstream `src/crypto/tests` GoogleTest suites run against the
    canonical `//src/crypto:crypto` library under native MSVC at the repository
    `/std:c++17` default. `msvc-crypto-upstream-tests` runs `TestSessionKeystore`,
    `TestGroupOperationalCredentials`, and `TestPersistentStorageOpKeyStore`
    (7 tests); `msvc-crypto-pal-tests` runs the full `TestChipCryptoPAL`
    CryptoPAL suite (73 tests), linking a focused
    `//src/credentials/windows:windows-chip-cert` CHIPCert/attestation subset and
    the upstream certificate test vectors. All 80 tests pass on x64 and both
    executables cross-build and inspect as `AA64` for ARM64. The upstream test
    bodies and vector headers use GCC/Clang C++17 extensions (designated
    initializers, compound literals) that MSVC does not accept at `/std:c++17`,
    so a build-time transform emits `/std:c++17` positional-initialization copies
    into a gen directory (the upstream files are untouched); the known-answer
    assertions validate every rewrite at runtime. The Pigweed `pw_string`
    `StringBuilderAdapters` dependency is satisfied by a GoogleTest-only
    `lib/core/StringBuilderAdapters.h` facade (the upstream header otherwise
    pulls the pw_string / Fuchsia stdcompat closure, which uses GCC builtins MSVC
    lacks). Details are in the crypto decision gate.
-   The canonical `//src/system:system`, `//src/inet:inet`, and
    `//src/crypto:crypto` GN targets, together with the host-neutral canonical
    splits of `//src/transport`, `//src/protocols/secure_channel`, and
    `//src/messaging` (`//src/transport:crypto-context`,
    `//src/transport:group-peer-message-counter`,
    `//src/protocols/secure_channel:type_definitions`, `:check-in-counter`,
    `:session-resumption-storage`, and `//src/messaging:configurations`), compile
    with MSVC for x64 and ARM64 through an opt-in graph probe. The canonical
    `-Wconversion` flags these targets pass (which `cl.exe` rejects as an invalid
    numeric argument) are guarded under `!is_msvc`, and the upstream bodies that
    are not clean under the strict `/W4 /WX` Windows default reuse the
    `//build/config/win:upstream_sdk_warnings` isolation only under `is_msvc`; the
    transport / secure-channel translation units that reach the Device Layer via
    `//src/platform` stay out of the split and are deferred to Phase 3. The
    bootstrap graph now defines the Pigweed Python venv label needed while loading
    canonical BUILD files, without building the venv unless a target actually
    depends on it.
-   The real upstream `src/system/tests` and `src/inet/tests` GoogleTest suites
    run against the canonical `//src/system:system` and `//src/inet:inet`
    libraries under native MSVC. `msvc-system-upstream-tests` runs the packet
    buffer (`TestSystemPacketBuffer`), TLV packet-buffer backing store
    (`TestTLVPacketBufferBackingStore`), timer / event-loop
    (`TestSystemTimer`, driven directly through `LayerImplWindows`), mock system
    clock (`TestSystemClock`), `TestTimeSource`, and System error-string
    (`TestSystemErrorStr`) suites (53 tests). `msvc-inet-upstream-tests` runs the
    IP address / prefix / interface-id (`TestInetAddress`), Inet error-string
    (`TestInetErrorStr`), and `TestBasicPacketFilters` suites (35 tests).
    `msvc-inet-endpoint-tests` runs the Inet EndPoint suite (`TestInetEndPoint`:
    interface iteration/enumeration, hardware-address and link-local queries,
    UDP/TCP endpoint bind/listen/connect error branches, and endpoint- and
    timer-pool limits) against a small sockets-only Windows harness (5 tests).
    All 93 tests pass on x64 and the three executables cross-build and inspect as
    `AA64` for ARM64. The suites reuse the GoogleTest `pw_unit_test/framework.h`
    and `lib/core/StringBuilderAdapters.h` facades and the `upstream_sdk_warnings`
    isolation; upstream test bodies are compiled verbatim. See the System and
    Inet test section below for the enabled/skipped inventory and the port fixes
    the tests surfaced.
-   The host-neutral upstream `src/transport/tests` and
    `src/protocols/secure_channel/tests` GoogleTest suites run against
    host-neutral canonical splits of `//src/transport` and
    `//src/protocols/secure_channel` -- `//src/transport:crypto-context`,
    `//src/transport:group-peer-message-counter`,
    `//src/protocols/secure_channel:type_definitions`, `:check-in-counter`, and
    `:session-resumption-storage` -- linked to the canonical `//src/crypto`,
    `//src/inet`, `//src/system`, and `//src/lib` libraries. These `source_set`s
    live in the canonical `BUILD.gn` files (not a Windows-only directory) and are
    public dependencies of the monolithic `//src/transport:transport` /
    `//src/protocols/secure_channel:secure_channel` targets, so every platform
    builds the same translation units into `libTransportLayer` / `libSecureChannel`;
    the split only lets the Windows port compile the host-neutral portion without
    the Device Layer that the full targets pull in via `//src/platform`.
    `msvc-transport-crypto-context-tests` runs `TestCryptoContext` (1 test);
    `msvc-transport-tests` runs `TestSecureSession`, `TestPeerMessageCounter`,
    and `TestGroupMessageCounter` (22 tests); `msvc-secure-channel-tests` runs
    `TestCheckInCounter`, `TestDefaultSessionResumptionStorage`, and
    `TestSimpleSessionResumptionStorage` (13 tests); and
    `msvc-secure-channel-status-report-tests` runs `TestStatusReport` (4 tests).
    All 40 tests pass on x64 and the four executables cross-build and inspect as
    `AA64` for ARM64. The suites reuse the System/Inet GoogleTest facades and the
    `upstream_sdk_warnings` isolation; upstream test bodies are compiled
    verbatim. Suites whose `SecureSession` / `ExchangeManager` /
    `ReliableMessageMgr` closures reach the Device Layer
    (`platform/ConnectivityManager.h`) -- including every `src/messaging/tests`
    suite -- are deferred. Two shared-header portability fixes
    (`src/transport/raw/PeerAddress.h` union initializers,
    `src/lib/support/AutoRelease.h` `always_inline`) are documented in the
    transport / secure-channel test section below.

The default Windows GN graph is intentionally restricted to bootstrap targets.
The canonical library probe remains opt-in while Phase 3 is active. The
Windows Device Layer now composes the messaging, credentials, secure-session,
and Interaction Model closures, but no fabric-backed controller application
exists yet; keeping the probe opt-in avoids presenting successful library
compilation as a usable Matter SDK port.

Work advances only when the preceding implementation phase is complete. Phases
0 through 2 are complete; Phase 3 (Windows Device Layer and IP discovery) is
active. The first Phase 3 milestone -- a native Windows Device Layer
`PlatformManager` foundation -- has landed as an opt-in probe (see
[The Windows Device Layer foundation](#the-windows-device-layer-foundation)
below). The second Phase 3 milestone -- a native Windows
`KeyValueStoreManager` for controller fabric persistence -- has landed on the
same opt-in probe (see
[The Windows key-value store](#the-windows-key-value-store) below). The typed
configuration-storage backend and the OS-managed Windows
`ConnectivityManager` have also landed on that probe. The public Windows
`ConfigurationManager` now composes those pieces into the canonical
`PlatformMgr()` lifecycle. Native
ARM64 execution remains a release-support requirement and is
tracked for the hardware-backed CI phase; cross-compilation alone is not
presented as ARM64 runtime support.

## Prerequisites

-   Windows 11.
-   Visual Studio with the **Desktop development with C++** workload and the
    MSVC x64 and ARM64 build tools.
-   Python 3.11 or newer available as `python3.exe` or `python.exe` on `PATH`.
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
It also creates an isolated Python environment under `.environment\windows`
and installs the repository's constrained build requirements when they change.

Initialize the dependencies exercised by the Windows bootstrap graph:

```powershell
git submodule update --init third_party/boringssl/repo/src `
    third_party/mbedtls/repo third_party/jsoncpp/repo `
    third_party/googletest third_party/nlio/repo
```

## Build the x64 smoke target

```powershell
gn gen out\win-msvc-smoke --args='target_os="win" target_cpu="x64" chip_device_platform="none"'
ninja -C out\win-msvc-smoke
.\out\win-msvc-smoke\msvc-toolchain-smoke.exe
.\out\win-msvc-smoke\msvc-sdk-smoke.exe
.\out\win-msvc-smoke\msvc-dependency-smoke.exe
.\out\win-msvc-smoke\msvc-core-unit-tests.exe
.\out\win-msvc-smoke\msvc-inet-interface-smoke.exe
.\out\win-msvc-smoke\msvc-system-error-source-smoke.exe
.\out\win-msvc-smoke\msvc-system-layer-smoke.exe
.\out\win-msvc-smoke\msvc-system-primitives-smoke.exe
.\out\win-msvc-smoke\msvc-system-wake-event-smoke.exe
.\out\win-msvc-smoke\msvc-socket-smoke.exe
.\out\win-msvc-smoke\msvc-inet-udp-endpoint-smoke.exe
.\out\win-msvc-smoke\msvc-inet-tcp-endpoint-smoke.exe
.\out\win-msvc-smoke\msvc-crypto-boringssl-tests.exe
```

## Run the upstream crypto test suites

The `msvc-crypto-upstream-tests` and `msvc-crypto-pal-tests` executables run the
real `src/crypto/tests` GoogleTest suites against the canonical
`//src/crypto:crypto` library at the repository `/std:c++17` default (the
upstream sources are adapted to C++17 by a build-time transform; see the crypto
decision gate). Because they link the canonical crypto library (and a focused
CHIPCert subset), they are built with the canonical probe graph below rather
than the restricted plain bootstrap:

```powershell
gn gen out\win-canonical-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="none" chip_windows_canonical_compile_probes=true chip_build_tests=false chip_build_tools=false'
ninja -C out\win-canonical-x64
.\out\win-canonical-x64\msvc-crypto-upstream-tests.exe
.\out\win-canonical-x64\msvc-crypto-pal-tests.exe
```

`msvc-crypto-upstream-tests` links only `//src/crypto:crypto` and runs
`TestSessionKeystore`, `TestGroupOperationalCredentials`, and
`TestPersistentStorageOpKeyStore` (7 tests). `msvc-crypto-pal-tests` also links
the focused `//src/credentials/windows:windows-chip-cert` CHIPCert subset and
runs the full `TestChipCryptoPAL` CryptoPAL suite (73 tests) including the
certificate, DAC-attestation, and SPAKE2+ coverage. All 80 tests pass on x64.

These reuse the existing GoogleTest `pw_unit_test/framework.h` facade and add a
Pigweed-free `lib/core/StringBuilderAdapters.h` facade so the upstream sources
compile without the pw_string closure. See the crypto decision gate for the
portability fixes applied to the shared test vectors and the CHIPCert closure.

## Run the upstream System and Inet test suites

The `msvc-system-upstream-tests`, `msvc-inet-upstream-tests`, and
`msvc-inet-endpoint-tests` executables run the real `src/system/tests` and
`src/inet/tests` GoogleTest suites against the canonical `//src/system:system`
and `//src/inet:inet` libraries under native MSVC. They are built with the
canonical probe graph:

```powershell
gn gen out\win-canonical-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="none" chip_windows_canonical_compile_probes=true chip_build_tests=false chip_build_tools=false chip_caller_handles_critical_failure=true'
ninja -C out\win-canonical-x64
.\out\win-canonical-x64\msvc-system-upstream-tests.exe
.\out\win-canonical-x64\msvc-inet-upstream-tests.exe
.\out\win-canonical-x64\msvc-inet-endpoint-tests.exe
```

Use the same arguments with `target_cpu="arm64"` after initializing the ARM64
MSVC environment to cross-build; the executables inspect as `AA64`.

### Enabled suites

-   `msvc-system-upstream-tests` (53 tests, 6 suites): `TestSystemPacketBuffer`
    (packet buffers), `TestTLVPacketBufferBackingStore` (TLV packet-buffer
    backing store), `TestSystemTimer` (timers and the event-loop service path,
    driven directly through `LayerImplWindows`, which derives from
    `LayerSelectLoop`), `TestSystemClock` (the mock clock), `TestTimeSource`, and
    `TestSystemErrorStr`.
-   `msvc-inet-upstream-tests` (35 tests, 3 suites): `TestInetAddress` (IPv4/IPv6
    address and `IPPrefix` math, interface-scoped strings, socket-address
    conversion), `TestInetErrorStr`, and `TestBasicPacketFilters`.
-   `msvc-inet-endpoint-tests` (5 tests, 1 suite): `TestInetEndPoint` exercises
    interface iteration/enumeration, hardware-address and link-local queries, the
    UDP/TCP endpoint bind/listen/connect error branches, the POSIX error range,
    and the endpoint- and timer-pool limits.

All 93 tests pass on x64 and the three executables cross-build and inspect as
`AA64` for ARM64.

### Test reuse and facades

The suites reuse the existing GoogleTest `pw_unit_test/framework.h` facade, the
Pigweed-free `lib/core/StringBuilderAdapters.h` facade, and the
`//build/config/win:upstream_sdk_warnings` isolation. The upstream test bodies
are compiled verbatim; the additional Windows test-support facades under
`build/config/win/tests/system_inet_test_support/` supply only what the port
does not yet provide:

-   `platform/CHIPDeviceLayer.h` forwards to the CHIP allocator (`CHIPMem.h`) and
    supplies a do-nothing `DeviceLayer::PlatformMgr()` stub plus a `random()` /
    `srandom()` shim for `TestSystemPacketBuffer`, whose fixture makes an
    incidental `PlatformMgr().InitChipStack()` call and fills buffers with POSIX
    `random()`. The Device Layer itself is out of scope for Phase 2.
-   `sys/time.h`, `sys/socket.h`, and `netinet/in.h` forward to `<time.h>` and
    WinSock so `TestInetCommon.h` and `TestInetAddress` resolve their POSIX
    socket includes; WinSock provides the same `s_addr` / `s6_addr` member
    macros.
-   `msvc_posix_compat.h` is force-included (`/FI`) into `msvc-inet-upstream-tests`
    to map POSIX `strcasecmp`/`strncasecmp` onto the MSVC `_stricmp`/`_strnicmp`.
-   `msvc_inet_test_harness_windows.cpp` is a small sockets-only implementation
    of the `TestInetCommon.h` / `TestSetupSignalling.h` contract
    (`gSystemLayer`, `gUDP`, `gTCP`, `InitSystemLayer`/`InitNetwork`/
    `ServiceEvents`/`Shutdown*`) against the native Windows System layer, in
    place of the POSIX/LwIP `TestInetCommonPosix.cpp` helper. The upstream
    `TestInetEndPoint.cpp` itself is compiled verbatim.

The shared `msvc_system_inet_test_main.cpp` deliberately does not initialize the
CHIP allocator: the upstream fixtures that allocate own their per-suite
`MemoryInit`/`MemoryShutdown`, and `MemoryInit` is not reference-counted.

### Port fixes surfaced by the tests

-   `src/system/BUILD.gn` now emits
    `CHIP_SYSTEM_LAYER_IMPL_CONFIG_FILE=<system/windows/SystemLayerImplWindows.h>`
    for the Windows event loop. The previous
    `<system/SystemLayerImpl${event_loop}.h>` form resolved to a non-existent
    `<system/SystemLayerImplWindows.h>`; only translation units that include the
    generic `<system/SystemLayerImpl.h>` (the unit tests) hit it, because the
    library sources include the backend header by its real path.
-   `TestSystemPacketBuffer.CheckAlignPayload` cast a pointer to `unsigned long`
    (32-bit under Windows LLP64) before a modulo alignment check, truncating the
    high 32 bits. It now uses `uintptr_t`, matching the rest of the test and
    leaving LP64 behavior unchanged.
-   The native Windows `InterfaceIterator::GetHardwareAddress`
    (`src/inet/InetInterface.cpp`) returned success with a truncated address for
    adapters whose `PhysicalAddressLength` is not an EUI-48/EUI-64 length (0 for
    loopback/tunnel/virtual adapters). It now returns `CHIP_ERROR_NOT_IMPLEMENTED`
    for such interfaces, matching the `TestInetInterface` contract and other
    platforms.
-   `//src/inet:inet` now declares its WinSock / IP Helper import libraries
    (`ws2_32.lib`, `iphlpapi.lib`) under MSVC so every consumer links the symbols
    the native endpoint and interface backends reference.
-   A `chip_caller_handles_critical_failure` build argument (default
    `chip_build_tests`, so non-Windows behavior is unchanged) lets the probe
    compile `CriticalFailure` in its error-returning form without enabling the
    full `chip_build_tests` closure. The upstream System/Inet tests deliberately
    exercise error-return paths (e.g. `StartTimer` on an uninitialized layer,
    exhausting the endpoint/timer pools) that otherwise abort.

### Not yet enabled

The following upstream suites require the unported Windows Device Layer or a
POSIX-only harness and are tracked for a later phase:

-   `TestSystemScheduleWork`, `TestSystemScheduleLambda`, `TestEventLoopHandler`
    drive `DeviceLayer::PlatformMgr().RunEventLoop()`.
-   `TestSystemEventSource` drives `DeviceLayer::PlatformMgr()` and the Select
    event-source pool.
-   `TestSystemWakeEvent` uses POSIX `pthread` and the Select-loop `WakeEvent`;
    the native `WindowsWakeEvent` is covered by `msvc-system-wake-event-smoke`.
-   `TestInetAddress.TestCheckToLwIPAddr` and `TestInetEndPoint`'s LwIP paths are
    LwIP-only and compiled out on the sockets build.

## Run the upstream transport and secure-channel test suites

The `msvc-transport-*` and `msvc-secure-channel-*` executables run the
host-neutral upstream `src/transport/tests` and
`src/protocols/secure_channel/tests` GoogleTest suites under native MSVC. They
link host-neutral canonical splits of `//src/transport` and
`//src/protocols/secure_channel` (`//src/transport:crypto-context`,
`//src/transport:group-peer-message-counter`,
`//src/protocols/secure_channel:type_definitions`, `:check-in-counter`, and
`:session-resumption-storage`) against the canonical `//src/crypto`,
`//src/inet`, `//src/system`, and `//src/lib` libraries -- no Device Layer.
They are built with the canonical probe graph:

```powershell
gn gen out\win-canonical-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="none" chip_windows_canonical_compile_probes=true chip_build_tests=false chip_build_tools=false chip_caller_handles_critical_failure=true'
ninja -C out\win-canonical-x64
.\out\win-canonical-x64\msvc-transport-crypto-context-tests.exe
.\out\win-canonical-x64\msvc-transport-tests.exe
.\out\win-canonical-x64\msvc-secure-channel-tests.exe
.\out\win-canonical-x64\msvc-secure-channel-status-report-tests.exe
```

Use the same arguments with `target_cpu="arm64"` after initializing the ARM64
MSVC environment to cross-build; the executables inspect as `AA64`.

### Enabled suites

-   `msvc-transport-crypto-context-tests` (1 test, 1 suite): `TestCryptoContext`
    exercises the `CryptoContext` message privacy-nonce derivation.
-   `msvc-transport-tests` (22 tests, 3 suites): `TestSecureSession` (the
    `CryptoContext` session-key init and AEAD encrypt/decrypt round trips -- the
    suite exercises `CryptoContext`, not the `SecureSession` object),
    `TestPeerMessageCounter` (the header-only peer message-counter replay
    window), and `TestGroupMessageCounter` (the group peer message-counter
    table).
-   `msvc-secure-channel-tests` (13 tests, 3 suites): `TestCheckInCounter` (the
    monotonic ICD check-in counter), `TestDefaultSessionResumptionStorage`, and
    `TestSimpleSessionResumptionStorage` (session-resumption record storage over
    an in-memory `TestPersistentStorageDelegate`).
-   `msvc-secure-channel-status-report-tests` (4 tests, 1 suite):
    `TestStatusReport` (SecureChannel StatusReport message encode/parse).

All 40 tests pass on x64 and the four executables cross-build and inspect as
`AA64` for ARM64. They reuse the same GoogleTest `pw_unit_test/framework.h` and
`lib/core/StringBuilderAdapters.h` facades and the `upstream_sdk_warnings`
isolation as the System/Inet suites. Executables whose fixtures own their
per-suite `MemoryInit`/`MemoryShutdown` (`TestCryptoContext`, `TestStatusReport`)
use the no-init `msvc_system_inet_test_main.cpp`; the remaining suites use the
memory-initializing `msvc_crypto_test_main.cpp`.

### Canonical library splits

The canonical `//src/transport:transport`,
`//src/protocols/secure_channel:secure_channel`, and `//src/messaging:messaging`
targets pass the GCC/Clang `-Wconversion` flag (which cl.exe rejects as an
invalid numeric argument) and build at the strict `/W4 /WX` default. The
portability fix lives in the canonical `BUILD.gn` files rather than in
Windows-only test copies:

-   Every `-Wconversion` is guarded under `!is_msvc`, so non-Windows builds are
    unchanged and cl.exe never sees the flag.
-   The host-neutral, Phase-2 translation units are factored into narrowly named
    canonical `source_set`s -- `//src/transport/raw:message-header`,
    `//src/transport:crypto-context`,
    `//src/transport:group-peer-message-counter`, and
    `//src/protocols/secure_channel:session-resumption-storage` -- and the
    already-standalone `:type_definitions` (StatusReport) and `:check-in-counter`
    targets are reused. Each is a public dependency of its monolithic parent
    library, so every platform still builds the same objects into
    `libTransportLayer` / `libSecureChannel`; the split only lets the Windows
    port compile the host-neutral portion without the Device Layer.
-   Under `is_msvc` these splits add `//build/config/win:upstream_sdk_warnings`
    (which relaxes `/W4 /WX` for the GCC/Clang-authored bodies) only where the
    upstream sources are not clean under the strict default. The upstream `.cpp`
    sources are compiled verbatim. `credentials/FabricTable.h` and
    `messaging/ReliableMessageProtocolConfig.h` are reached only as headers
    through `//src:includes`.

The session-establishment / exchange / MRP translation units (`PASESession`,
`CASESession`, `ExchangeMgr`, `ReliableMessageMgr`, ...) that include
`platform/ConnectivityManager.h` stay in the monolithic libraries and are
deferred to Phase 3. `//src/messaging` has no host-neutral compilable unit
beyond the header-only `//src/messaging:configurations` (its
`ReliableMessageProtocolConfig.cpp` includes `platform/CHIPDeviceLayer.h`), so
only its `-Wconversion` is guarded and `:configurations` is added to the probe.

### Port fixes surfaced by the tests

-   `src/transport/raw/PeerAddress.h` initialized its `Id` union with C++20
    designated initializers (`mId{ .mRemoteId = ... }`) that GCC/Clang accept as
    a C++17 extension but MSVC rejects (C7555). The four first-member
    (`mRemoteId`) initializers now use the semantically identical positional
    form (`mId{ ... }`); the second-member (`mNFCShortId`) initializer moves to
    the constructor body. Both forms are standard C++17 and behave identically on
    every compiler and endianness, so non-Windows behavior is unchanged.
-   `src/lib/support/AutoRelease.h` used a raw
    `__attribute__((always_inline))`, which MSVC does not accept. A guarded
    `CHIP_AUTORELEASE_ALWAYS_INLINE` macro maps it to `__forceinline` under MSVC
    and keeps the GCC/Clang attribute elsewhere.

### Not yet enabled

The following upstream transport / messaging / secure-channel suites reach the
unported Windows Device Layer and are tracked for a later phase:

-   `TestPeerConnections` and `TestSecureSessionTable` construct
    `Transport::SecureSession`, whose MRP timeout virtuals
    (`GetAckTimeout` / `GetMessageReceiptTimeout`, present in the vtable) link
    `chip::GetRetransmissionTimeout` -> `ReliableMessageMgr::GetBackoff` from
    `src/messaging/ReliableMessageMgr.cpp`, which includes
    `platform/ConnectivityManager.h` (the Device Layer).
-   `TestSessionManager` / `TestSessionManagerDispatch` link `MessageCounterManager`,
    `PASESession`, and `GroupDataProviderImpl`, all of which drive exchanges /
    fabric storage through the Device Layer.
-   Every `src/messaging/tests` suite (`TestExchange`, `TestExchangeMgr`,
    `TestExchangeHolder`, `TestReliableMessageProtocol`,
    `TestAbortExchangesForFabric`, `TestMessagingLayer`) builds on
    `MessagingContext`, whose `ExchangeManager` / `ReliableMessageMgr` closure
    reaches `platform/ConnectivityManager.h`.
-   `TestPASESession`, `TestCASESession`, `TestPairingSession`,
    `TestMessageCounterManager`, and the fuzz suites drive the session-establishment
    exchange machinery (Device Layer).
-   `TestCheckinMsg` is host-neutral to compile, but its vectors are pulled in via
    an angle-bracket full-path include
    (`<protocols/secure_channel/tests/CheckIn_Message_test_vectors.h>`) that the
    same-directory C++17 source transform used for the crypto suites cannot
    shadow; it is deferred until that header can be adapted.

## Compile the canonical core libraries

The canonical System, Inet, and BoringSSL CryptoPAL libraries, plus the
host-neutral canonical splits of Transport, SecureChannel, and Messaging
(`//src/transport:crypto-context`,
`//src/transport:group-peer-message-counter`,
`//src/protocols/secure_channel:type_definitions`, `:check-in-counter`,
`:session-resumption-storage`, and `//src/messaging:configurations`), can be
added to the bootstrap graph as a compile-only probe. Tests and tools are
disabled here because their broader Windows closure is tracked separately:

```powershell
gn gen out\win-canonical-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="none" chip_windows_canonical_compile_probes=true chip_build_tests=false chip_build_tools=false'
ninja -C out\win-canonical-x64
```

Use the same arguments with `target_cpu="arm64"` after initializing the ARM64
MSVC environment. The normal bootstrap build leaves
`chip_windows_canonical_compile_probes` false.

## The Windows Device Layer foundation

The first Phase 3 milestone adds a native Windows Device Layer `PlatformManager`
foundation. It is the first `src/platform` code compiled for the Windows port
and composes the native `System::LayerImplWindows` event loop
(`WSAPoll` + WinSock wake socket) with the standard `DeviceLayer::PlatformMgr()`
contract:

-   `InitChipStack()` registers the CHIP error formatter and initializes the
    native System Layer.
-   `RunEventLoop()` / `StartEventLoopTask()` drive the loop directly or on a
    dedicated `std::thread`, and `StopEventLoopTask()` stops it (safely both
    from another thread and from within a work item on the loop thread).
-   `ScheduleWork()` / `PostEvent()` post work and events from any thread; a
    `DeviceSafeQueue` holds them and the native wake socket signals the loop.
-   `AddEventHandler()` / `RemoveEventHandler()` register application handlers
    that receive posted public events.
-   `Shutdown()` tears the System Layer down after the loop has stopped.

The foundation is deliberately scoped. It does **not** pull the full Device
Layer manager closure (`ConfigurationManager`, `ConnectivityManager`,
`KeyValueStoreManager`, DNS-SD, BLE, Thread): those are later phases and are not
stubbed. The disabled features are expressed with concrete
feature-off configuration macros in
`src/platform/Windows/CHIPDevicePlatformConfig.h`
(`CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE 0`, `CHIP_DEVICE_CONFIG_ENABLE_THREAD 0`,
`CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION 0`), not success-shaped fakes.

New files:

-   `src/platform/Windows/PlatformManagerImpl.{h,cpp}` -- the concrete
    singleton.
-   `src/include/platform/internal/GenericPlatformManagerImpl_Windows.{h,ipp}`
    -- the self-contained generic manager (std::thread / std::mutex /
    std::condition_variable, no pthreads); event dispatch handles no-op, lambda,
    and scheduled-work events plus application handlers. Device Layer component
    dispatch is added when those managers are ported.
-   `src/platform/Windows/SystemLayerGlobals.cpp` -- a focused analogue of
    `src/platform/Globals.cpp` that supplies `DeviceLayer::SystemLayer()` backed
    by `System::LayerImplWindows` without the full manager closure.
-   `src/platform/Windows/{System,Inet,CHIP,CHIPDevice,Ble}PlatformConfig.h` and
    `CHIPDevicePlatformEvent.h` -- the platform configuration header set wired by
    `chip_device_platform="windows"` (`_chip_device_layer = "Windows"`) in
    `src/platform/device.gni`.

The milestone is built as an opt-in probe
(`chip_windows_device_layer_probe`, default `false`) that is independent of the
canonical library probe, so the normal bootstrap graph is not broadened. A
runtime smoke (`msvc-platform-manager-smoke`) exercises the full lifecycle and
cross-thread work/event posting on x64:

```powershell
gn gen out\win-devlayer-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_device_layer_probe=true'
ninja -C out\win-devlayer-x64 msvc-platform-manager-smoke.exe
.\out\win-devlayer-x64\msvc-platform-manager-smoke.exe
```

Cross-build the same target for ARM64 in a new PowerShell process initialized
for the ARM64 compiler:

```powershell
. .\scripts\setup\windows.ps1 -Architecture arm64
gn gen out\win-devlayer-arm64 --args='target_os="win" target_cpu="arm64" chip_device_platform="windows" chip_windows_device_layer_probe=true'
ninja -C out\win-devlayer-arm64 msvc-platform-manager-smoke.exe
dumpbin /headers out\win-devlayer-arm64\msvc-platform-manager-smoke.exe |
    Select-String "machine \(ARM64\)"
```

The foundation builds for x64 and ARM64 under the strict `/W4 /WX` Windows
default and the smoke passes on x64; the ARM64 executable inspects as `AA64` and
is not yet run on native hardware.

### Foundation limitations

-   No `ConfigurationManager`, `ConnectivityManager`, DNS-SD, BLE, or Thread.
    `PlatformManager::HandleServerStarted()` reports the compile-time
    `CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION` because no configuration manager
    exists yet. (The `KeyValueStoreManager` has landed as a separate sibling
    milestone on the same probe; see
    [The Windows key-value store](#the-windows-key-value-store).)
-   `src/platform/PlatformEventSupport.cpp` (which routes System Layer
    `ScheduleLambda` / timer bridging through `PlatformMgr()`) is **not** part
    of the foundation target: it includes
    `platform/internal/CHIPDeviceLayerInternal.h`, which pulls the full manager
    header set. It is added once those managers exist. The foundation therefore
    provides `PlatformMgr().ScheduleWork()` / `PostEvent()` directly rather than
    `System::Layer::ScheduleLambda()`.
-   The device-layer error formatter (`RegisterDeviceLayerErrorFormatter()`,
    part of `GeneralUtils.cpp`) is registered with the rest of that translation
    unit in a later phase, since it also reaches the full manager closure.
-   The target links the focused `windows-system-layer` and
    `windows-core-portable` libraries rather than the canonical
    `//src/system:system`, keeping it free of the `PlatformEventing` symbols the
    canonical library references. A canonical-config twin,
    `windows-platform-manager-canonical`, now exists specifically to *not* have
    this limitation; see
    "Wiring the generic Device Layer dispatch to canonical libraries" below.

## Wiring the generic Device Layer dispatch to canonical libraries

The Phase 3 milestone above deliberately linked the ad hoc,
command-line-configured `windows-core-portable`/`windows-system-layer` rather
than canonical `//src/lib/core`/`//src/system`, specifically to avoid a
macro conflict: `windows-core-portable`'s `:public` config sets
`CHIP_ERROR_LOGGING`, `CHIP_SYSTEM_CONFIG_USE_SOCKETS`, and similar
CHIPBuildConfig-controlled macros directly as literal command-line `/D`
defines, while every canonical library instead gets them from the generated
`CHIPBuildConfig.h`/`SystemBuildConfig.h` (via `CHIP_HAVE_CONFIG_H=1`).
Combining both in one binary redefines the same macros with conflicting
values (a hard error under `/WX`) and links two independently configured
copies of `CHIPError`/TLV/the System Layer. This is why the generic
`//src/platform` umbrella Device Layer dispatch target could not yet be used:
canonical `//src/platform:platform`'s own `platform_base` sub-target already
depends on canonical `//src/lib/core`, `//src/system`, `//src/inet`, etc., so
its Windows `_platform_target` needed to agree with that, not with
`windows-core-portable`.

This milestone reconciles that conflict and wires `chip_device_platform=="windows"`
through to the generic `//src/platform:platform` target as a true canonical
platform selection, without introducing ODR duplication:

-   `src/platform/Windows/BUILD.gn` gains canonical-config *twins* of the
    targets canonical `//src/platform:platform`'s dependency graph needs:
    `windows-platform-manager-canonical` (linked from `src/platform/BUILD.gn`'s
    `_platform_target` for `chip_device_platform=="windows"`),
    `windows-system-primitives-canonical`, and `windows-system-errors-canonical`
    (in `src/system/windows/BUILD.gn`). Each compiles the *same* upstream
    sources as its existing ad hoc counterpart, but against canonical
    `//src/lib/core:core`/`//src/lib/core:error`/`//src/system:system` instead
    of `windows-core-portable`. The plain, ad hoc targets are unchanged and
    still used by their existing standalone smokes and by the other Windows
    Device Layer targets that pair with them (`windows-dnssd`,
    `windows-configuration-manager`, ...), so nothing already working
    regresses.
-   The canonical Windows platform target compiles the Windows
    `ConfigurationManager`, `ConnectivityManager`, `KeyValueStoreManager`, and
    diagnostic-provider implementations against canonical Core/System/Inet/
    Crypto dependencies. It defines
    `CHIP_WINDOWS_DEVICE_LAYER_COMPOSITION=1`, so `InitChipStack()` initializes
    configuration storage, UDP/TCP endpoint managers, and connectivity in
    order, with matching failure unwinding and shutdown. The canonical smoke
    performs a KVS write/read/delete after initialization to ensure these are
    real lifecycle-managed managers, not merely link-time singleton
    definitions.
-   `//src/system:system`'s own Windows-only addition
    (`windows-system-primitives` -> now `windows-system-primitives-canonical`)
    no longer transitively drags in a *second* compile of `SystemError.cpp`:
    canonical `//src/system:system` already compiles it directly, and the
    duplicate was a real, pre-existing (if latent) `LNK2005` risk once
    anything forced both archive members to be extracted into the same link
    (which is exactly what building the generic dispatch surfaced).
-   `MapErrorWindows()`'s overload set (1 vs. 3 arguments) depends on
    `CHIP_CONFIG_ERROR_SOURCE`, which itself depends on whether
    `CHIP_PLATFORM_CONFIG_INCLUDE` (`platform/Windows/CHIPPlatformConfig.h`,
    which sets `CHIP_CONFIG_ERROR_SOURCE=1`, matching Linux/Darwin) is applied
    -- which it only is once a *real* device platform (not `"none"`) is
    selected. The canonical-config twins consistently pick the 3-argument
    overload; the plain ad hoc targets consistently pick the 1-argument one.
    Mixing a canonical-config target with a plain one in the same link (as the
    standalone `msvc-system-wake-event-smoke`/`msvc-system-layer-smoke` did
    transiently while this was being reconciled) produces an
    unresolved-symbol error, not a silent bug; each smoke now pairs
    consistently-configured targets.
-   `src/lib/support/Pool.h`'s `HeapObjectPool` destructor used to *define* the
    reserved (leading-double-underscore) identifier `__SANITIZE_ADDRESS__`
    itself when unset, to detect ASan builds. Merely defining that identifier
    (to any value, including `0`) makes MSVC's STL believe ASan container
    annotations are active, producing an `LNK2038` `annotate_string`/
    `annotate_vector`/`annotate_optional` mismatch against every other
    translation unit that never defined it -- a real, pre-existing, if latent,
    bug independent of this port (undefined behavior per the reserved-identifier
    rule), that a mechanical, behavior-preserving fix now reads the flag via a
    project-named macro instead of writing to the reserved one.
-   `src/lib/core/CHIPError.h`'s `ChipError::AsString()` declared its
    `chip::ErrorStr()` forward reference as a block-scope `extern` inside a
    class member function. MSVC binds that block-scope declaration to the
    *global* namespace instead of the enclosing `chip::` namespace (unlike
    GCC/Clang), so any translation unit that actually calls `.Format()` /
    `.AsString()` (once `CHIP_CONFIG_ERROR_FORMAT_AS_STRING=1`, again from
    `CHIPPlatformConfig.h`) failed to link with an unresolved global
    `::ErrorStr`. A namespace-scope forward declaration fixes this
    portably for every compiler.
-   `src/platform/BUILD.gn`'s `static_library("platform")` had an unguarded
    GCC/Clang `-Wconversion` `cflags` entry (`cl.exe` rejects it outright) and
    needed the same `__attribute__` shim
    (`platform/Windows/MsvcCompilerCompatibility.h`) and `/wd4505` (etc.)
    warning relaxation the ad hoc Windows Device Layer targets already use for
    the generated `zzz_generated/app-common` cluster headers. Two upstream
    `.cpp` files (`DeviceControlServer.cpp`, `PlatformEventSupport.cpp`)
    aggregate-initialize a non-first anonymous-union member of
    `ChipDeviceEvent` via a C++20 designated initializer; unlike the
    crypto/dnssd upstream sources (whose C++20 designators are always the
    aggregate's leading members and can be mechanically stripped to positional
    form), a non-first union member cannot be selected positionally at all, so
    the scalar payloads are activated by C++17 assignment and the non-scalar
    `LambdaEvent` payload is explicitly placement-constructed, preserving the
    selected union member's lifetime without requiring C++20.
    `//src/credentials` and `//src/lib/dnssd`'s canonical `:dnssd`/`:naming`
    targets needed the same `-Wconversion` guard plus
    `//build/config/win:upstream_sdk_warnings` for a handful of upstream
    warnings (`C4267` narrowing, `C4702` unreachable code, `C4701`
    potentially-uninitialized) not yet clean under the strict default --
    the same treatment already applied to `//src/transport`/
    `//src/protocols/secure_channel`.
-   `src/platform/device.gni` adds `windows` to the `chip_mdns == "platform"`
    platform list, matching Darwin/Tizen/etc., so canonical
    `//src/lib/dnssd:dnssd` selects `Discovery_ImplPlatform.cpp` (the real
    `chip::Dnssd::Resolver`/`DiscoveryImplPlatform` contract) against the
    generic `//src/platform` dispatch instead of the no-op `Resolver_ImplNone`.
    `windows-platform-manager-canonical` now compiles `DnssdImpl.cpp` directly
    (matching how `src/platform/Darwin/BUILD.gn`'s own `"Darwin"` target
    includes `dnssd/DnssdImpl.cpp`), so the concrete Windows backend lives
    inside the same canonical Device Layer target Discovery_ImplPlatform.cpp
    reaches through `//src/platform`.

### What now builds canonically for Windows

With `chip_device_platform="windows"` (a real device platform, not `"none"`)
and the existing `chip_windows_canonical_compile_probes`/
`chip_windows_device_layer_probe` opt-ins both enabled together in the same
graph for the first time:

-   The generic **`//src/platform:platform`** target itself -- the full
    `libDeviceLayer` static library, including `ConfigurationManager`/
    `ConnectivityManager`/ICD/setup-payload wiring and every generated
    `zzz_generated/app-common` cluster's enum-conformance checks -- compiles
    and links. A runtime smoke, `msvc-canonical-platform-smoke` (the same
    source as `msvc-platform-manager-smoke`, linked against
    `//src/platform` + `//src/platform/logging:default` instead of the focused
    `windows-platform-manager`), passes: `InitChipStack()`, event-loop
    start/stop (both caller- and library-managed), cross-thread
    `ScheduleWork()`, and public event dispatch all work through the
    canonical dispatch graph. The smoke also verifies that canonical
    `InitChipStack()` initialized persistent storage by completing a KVS
    write/read/delete cycle.
-   **`//src/credentials:credentials`** -- `FabricTable`, `CHIPCert`,
    `CertificationDeclaration`, `GroupDataProviderImpl`,
    `PersistentStorageOpCertStore`, `DeviceAttestationVerifier`, and the
    example DAC/PAI credentials -- compiles (this is the "next controller
    dependency closure" milestone; it transitively pulls in and confirms
    `//src/platform:platform` too).
-   **`//src/lib/dnssd:dnssd`** (the canonical, `chip_mdns_platform` variant)
    compiles, including the real upstream `Discovery_ImplPlatform.cpp`.

All of the above compile for x64 and cross-compile for ARM64 (`dumpbin
/headers` confirms `machine (ARM64)`); every existing focused Windows smoke
(`msvc-key-value-store-smoke`, `msvc-windows-configuration-manager-smoke`,
`msvc-windows-connectivity-smoke`, `msvc-windows-dnssd-smoke`,
`msvc-system-wake-event-smoke`, `msvc-system-layer-smoke`,
`msvc-inet-tcp-endpoint-smoke`, `msvc-inet-udp-endpoint-smoke`, ...) and every
existing canonical-probe test (crypto/system/inet/transport/secure-channel)
continues to pass unchanged on x64; the pre-existing, environment-specific
`msvc-inet-interface-smoke` exit-1 (reproduced identically on the unmodified
tree) is unrelated. The default `chip_device_platform="none"` bootstrap build
is unaffected (no new dependency reaches it).

```powershell
gn gen out\win-canonical-devlayer-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_canonical_compile_probes=true chip_windows_device_layer_probe=true chip_with_nlfaultinjection=false chip_build_tests=false chip_build_tools=false chip_caller_handles_critical_failure=true'
ninja -C out\win-canonical-devlayer-x64
.\out\win-canonical-devlayer-x64\msvc-canonical-platform-smoke.exe
```

### Remaining blockers to a full `//examples/chip-tool` build

-   The canonical transport, messaging, secure-session, PASE/CASE, and
    Interaction Model libraries now compile and link against the Windows
    Device Layer. A real controller must still initialize them with
    fabric-backed persistent storage and operational credentials.
-   `examples/chip-tool` itself has not been attempted: its own Windows
    `BUILD.gn`, persistent-path, console cancellation, and CLI/dependency
    wiring do not exist yet.
-   BLE, Wi-Fi/Thread network commissioning, and native ARM64 execution remain
    explicitly out of scope / unverified, as in every earlier phase.

Canonical `//src/lib/dnssd:dnssd` now links end to end with canonical
`//src/platform` in `msvc-windows-controller-discovery.exe`; its real
`DiscoveryImplPlatform` browse/start/stop path runs successfully. The
temporary `//src/lib/dnssd:dnssd_windows` re-derived dependency graph has
therefore been removed.

## The Windows key-value store

The second Phase 3 milestone adds a native Windows `KeyValueStoreManager`
(`src/platform/Windows/KeyValueStoreManagerImpl.{h,cpp}`), the persistence
foundation a Windows Matter controller needs to keep its fabric state across
restarts. It implements the standard `PersistedStorage::KeyValueStoreMgr()`
contract (`Get` with offset/partial reads, `Put`, `Delete`) plus Windows-side
lifecycle (`Init`, `Shutdown`) and a scoped factory reset (`ClearAll`). It is
self-contained: it does **not** depend on `ConfigurationManager` or any other
Device Layer manager, so it builds and is exercised on its own while the rest of
the closure is still being ported.

### Storage model

-   **File per key.** Each value is stored in its own file under a storage root.
    This is what makes every write an isolated, atomic, durable replacement and
    keeps a corrupt or partially written value from affecting any other key.
-   **Per-user versioned root.** The default root is
    `%LOCALAPPDATA%\Matter\KVS\v1`, resolved with the known-folder API
    (`SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, …)`) and Unicode
    paths -- not by concatenating the `LOCALAPPDATA` ANSI environment variable --
    so it is correct under folder redirection, impersonation, and non-ASCII
    profile paths. The `v1` segment versions the on-disk layout for future
    migrations.
-   **Injectable override.** `Init(const char * storageRoot)` takes an optional
    absolute UTF-8 path; relative and drive-relative paths are rejected rather
    than being resolved against mutable process current-directory state. The
    path is canonicalized (`GetFullPathNameW`) and converted to extended-length
    `\\?\` form (including correct `\\?\UNC\` handling); `nullptr`/empty selects
    the default root. Tests use this for isolation, and a future per-service or
    per-machine deployment points it at its own directory (for example a
    `ProgramData` location).
-   **Exclusive root ownership.** The store creates a versioned
    `.matter-kvs.owner` signature in a new or empty root. An existing root
    without that valid marker is rejected, preventing `ClearAll` from treating
    a shared override directory as KVS-owned.

### Safe key-to-filename encoding

Keys are arbitrary null-terminated strings; file names are not. Each key maps to
a single path component `kv_` + a reversible per-byte encoding: the bytes
`a-z`, `0-9`, `-`, and `_` are kept literally and every other byte (uppercase
letters, `.`, path separators, `%`, control bytes) becomes `%XX` with uppercase
hex. Because no literal byte has a case variant and every escape uses fixed
uppercase hex, two distinct keys can never collide on the case-insensitive
Windows filesystem. The fixed `kv_` prefix guarantees a generated name can never
equal a reserved DOS device name (`CON`, `NUL`, `COM1`, …) and can never be `.`
or `..`, so path traversal and reserved-name collisions are structurally
impossible. Keys are bounded to 64 bytes (well above
`PersistentStorageDelegate::kKeyLengthMax` of 32, and small enough that the
worst-case 3x-expanded name stays under the NTFS component limit); an empty or
over-long key is rejected with `CHIP_ERROR_INVALID_ARGUMENT`.

### Value format and integrity

Every file carries a 20-byte header -- magic `MKV1`, a little-endian format
version, a reserved flags field, the little-endian value length, and an IEEE
CRC-32 over the value bytes -- followed by the raw value. Values are fully
binary (embedded NULs and high bytes are preserved) and reads honor the KVS
`offset_bytes` / buffer-size contract, returning `CHIP_ERROR_BUFFER_TOO_SMALL`
with the copied count when the caller's buffer is short. A truncated, mis-sized,
or bit-rotted file is reported as an explicit `CHIP_ERROR_INTEGRITY_CHECK_FAILED`
and a missing key as `CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND`; the store
never silently substitutes an empty or default value. All native failures flow
through the existing `CHIP_ERROR_WINDOWS()` / `CHIP_ERROR_HRESULT()` mapping.

### Atomic, durable writes

`Put` writes the header and value to a GUID-named temp file in the **same
directory** (hence same volume) as the destination, calls `FlushFileBuffers` to
force the data to disk, then `MoveFileExW(…, MOVEFILE_REPLACE_EXISTING |
MOVEFILE_WRITE_THROUGH)` to atomically rename it over the destination. GUID
names avoid collisions with leftovers from a crashed process or reused process
ID. A reader -- in this process, or another process, or after a crash --
therefore only ever observes the complete previous value or the complete new
value, never a torn write. (`ReplaceFileW` is the ACL-preserving alternative and
is noted in the code; it is unnecessary here because the destination inherits
the ACL of the locked-down root.)

### Concurrency, process locking, and ACLs

-   **In-process.** A `std::recursive_mutex` serializes every operation, so
    concurrent calls from any thread are safe.
-   **Cross-process.** `Init` opens a hidden `.matter-kvs.lock` file in the root
    with no write/delete sharing and holds the handle for the store's lifetime.
    A second process that opens the same root fails fast with
    `CHIP_ERROR_ACCESS_DENIED` rather than racing. The documented stance is
    single-process ownership of a given root (the normal controller model);
    because each value write is atomic, a concurrent reader in another process
    still never sees a torn value, but concurrent writers across processes are
    not supported.
-   **ACLs / storage expectations.** The default root lives under the user's
    private `%LOCALAPPDATA%`, which is ACLed to that user by Windows; the store
    relies on this inherited protection rather than setting its own DACL.
    Deployments that place the root elsewhere (for example a machine-wide
    `ProgramData` directory for a service) are responsible for applying an
    appropriately restrictive ACL to that directory.

### Deletion and factory reset

`Delete` removes a single key's file (`CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND`
when absent). `ClearAll` performs a factory reset scoped strictly to owned
storage: after root ownership has been established, it enumerates only the
`kv_*` namespace and deletes entries only when their names parse as the
reversible key encoding or the exact GUID-temp format. It ignores directories
and malformed lookalikes, leaves the ownership marker, lock, and unrelated
content untouched, and reports enumeration failures rather than returning
false success.

### Build and smoke

The store builds as `//src/platform/Windows:windows-key-value-store` and is
wired into the same opt-in `chip_windows_device_layer_probe` group as the
`PlatformManager` foundation. A focused runtime smoke
(`msvc-key-value-store-smoke`) drives the store against a real, isolated
on-disk directory and proves absolute-root enforcement, extended-length path
operation, exclusive root claiming, binary put/get, offset/size reads,
overwrite, delete/not-found, restart persistence across `Shutdown()`+`Init()`,
invalid-key and traversal/reserved-name safety, ownership-marker retention, and
owned-only `ClearAll` scoping. Its cleanup refuses to traverse reparse-point
directories before removing its GUID-named temporary tree:

```powershell
gn gen out\win-devlayer-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_device_layer_probe=true'
ninja -C out\win-devlayer-x64 msvc-key-value-store-smoke.exe
.\out\win-devlayer-x64\msvc-key-value-store-smoke.exe
```

Cross-build the same target for ARM64 in a new PowerShell process initialized
for the ARM64 compiler:

```powershell
. .\scripts\setup\windows.ps1 -Architecture arm64
gn gen out\win-devlayer-arm64 --args='target_os="win" target_cpu="arm64" chip_device_platform="windows" chip_windows_device_layer_probe=true'
ninja -C out\win-devlayer-arm64 msvc-key-value-store-smoke.exe
dumpbin /headers out\win-devlayer-arm64\msvc-key-value-store-smoke.exe |
    Select-String "machine \(ARM64\)"
```

The store builds for x64 and ARM64 under the strict `/W4 /WX` Windows default;
the smoke passes on x64 and the ARM64 executable inspects as `AA64` and is not
yet run on native hardware.

## The Windows configuration storage backend

The next Phase 3 persistence layer is
`src/platform/Windows/WindowsConfig.{h,cpp}`. It maps the standard Matter
factory, runtime-configuration, and counter keys into the native Windows KVS
without introducing a second storage format:

-   Keys use stable `factory/`, `config/`, and `counter/` prefixes.
-   Values carry a one-byte type discriminator followed by a fixed-width
    little-endian integer, UTF-8 string bytes, or an opaque binary payload.
    Reads reject type and size mismatches as integrity failures rather than
    reinterpreting corrupt or incompatible data.
-   String and binary reads support size queries with a null output buffer.
    Strings are always returned NUL-terminated when the caller supplies enough
    space, while the reported length excludes the terminator.
-   `FactoryResetConfig()` removes only runtime configuration. Factory
    provisioning—including vendor/product identity, setup passcode,
    discriminator, SPAKE2+ data, certificates, and keys—survives. Counter reset
    is a separate operation.
-   Storage persists across `Shutdown()` and `Init()` through the same
    checksummed, process-locked KVS root.

The focused `msvc-windows-configuration-smoke` executable validates typed
round trips, wrong-type rejection, string/blob sizing, existence and idempotent
deletion, restart persistence, and factory/config/counter reset boundaries.
Build it with the existing Device Layer probe:

```powershell
gn gen out\win-devlayer-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_device_layer_probe=true'
ninja -C out\win-devlayer-x64 msvc-windows-configuration-smoke.exe
.\out\win-devlayer-x64\msvc-windows-configuration-smoke.exe
```

This storage target is also consumed by the public Windows
`ConfigurationManager`, described below.

## The Windows connectivity manager

The next Phase 3 milestone adds
`src/platform/Windows/ConnectivityManagerImpl.{h,cpp}`. Windows remains the
owner of adapter configuration and Wi-Fi credentials: the Matter SDK observes
the already-connected network rather than exposing station/AP provisioning.
The manager composes the standard generic no-Wi-Fi-management, no-Thread, and
no-BLE contracts with the shared UDP and TCP endpoint managers.

Adapter lookup and operational selection intentionally have different
semantics:

-   Ethernet and Wi-Fi name lookup can return an installed adapter even when it
    is currently down, so diagnostics can describe known hardware.
-   `GetExternalInterface()` returns only an up, non-loopback,
    multicast-capable interface with a usable IPv4 or IPv6 address. It prefers
    Ethernet, then Wi-Fi, then another qualifying adapter such as a VPN or
    cellular interface.
-   Interface names are converted through the existing Windows
    `InterfaceId`/LUID implementation; status is looked up from a fresh
    `GetAdaptersAddresses` snapshot rather than cached.
-   Windows Ethernet classification covers CSMA/CD, Fast Ethernet,
    Fast Ethernet FX, Gigabit Ethernet, and token-ring interface constants.
-   `NotifyIpInterfaceChange()` and `NotifyUnicastIpAddressChange()` callbacks
    coalesce refresh work onto the Matter event loop. The refresh notifies the
    connectivity delegate and posts `kInternetConnectivityChange` plus
    `kInterfaceIpAddressChanged` when IPv4 or IPv6 availability changes.
-   Shutdown cancels both native registrations before UDP/TCP endpoint
    teardown. Generation-tagged work prevents a queued callback from a prior
    lifecycle from changing state after restart.

The focused smoke validates unsupported feature contracts, interface
name/status round trips, installed Ethernet/Wi-Fi lookup, and the external
interface invariants:

```powershell
gn gen out\win-devlayer-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_device_layer_probe=true'
ninja -C out\win-devlayer-x64 msvc-windows-connectivity-smoke.exe
.\out\win-devlayer-x64\msvc-windows-connectivity-smoke.exe
```

It passes 21 checks on the development x64 host. The same executable
cross-builds and inspects as `AA64`; it has not yet run on native ARM64
hardware. The public configuration-manager smoke additionally verifies that a
native-style notification is delivered to the connectivity delegate on the
Matter event-loop thread.

## The Windows configuration manager

`src/platform/Windows/ConfigurationManagerImpl.{h,cpp}` composes
`GenericConfigurationManagerImpl<WindowsConfig>` into the public
`ConfigurationMgr()` singleton. The full
`//src/platform/Windows:windows-device-layer` target makes the canonical
`PlatformMgr().InitChipStack()` path initialize configuration storage, UDP and
TCP endpoint managers, and connectivity in order, with failure unwinding; its
shutdown path releases those resources and the KVS ownership lock. The earlier
`windows-platform-manager` target remains a deliberately isolated event-loop
foundation probe.

Initialization generates and persists the unique ID when absent, stores
compile-time vendor/product IDs on first use, increments reboot count, and
initializes operational hours, boot reason, regulatory location/capability, and
configuration version. Persisted-storage counters share the `counter/`
namespace. Primary MAC selection prefers the active external Ethernet or Wi-Fi
adapter, then another Wi-Fi or Ethernet adapter; the Wi-Fi-specific API returns
only Wi-Fi hardware addresses.

Factory reset is scheduled onto the Matter event loop. It removes every
KVS value except the `factory/` provisioning namespace, including runtime
`config/` and `counter/` values plus standard fabric/global records such as
`f/...` and `g/...`. The host process is not forcibly terminated; applications
must restart after the logged reset completion.

Tests may select an isolated absolute UTF-8 storage root with
`ConfigurationMgrImpl().ConfigureStorageRoot()` before `InitChipStack()`.
Production uses the default `%LOCALAPPDATA%\Matter\KVS\v1` root:

```powershell
gn gen out\win-devlayer-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_device_layer_probe=true'
ninja -C out\win-devlayer-x64 msvc-windows-configuration-manager-smoke.exe
.\out\win-devlayer-x64\msvc-windows-configuration-manager-smoke.exe
```

The smoke passes 45 checks on x64 and cross-builds as `AA64`. It covers the
canonical lifecycle, restart counter and unique-ID persistence, public
persisted counters, primary MAC contract, event-loop-marshaled network-change
delivery, diagnostics, and asynchronous factory reset.

## The Windows diagnostic data provider

`src/platform/Windows/DiagnosticDataProviderImpl.{h,cpp}` supplies the public
`GetDiagnosticDataProvider()` singleton used by the General Diagnostics data
model:

-   Reboot count, boot reason, and total operational hours reuse the typed
    Windows configuration backend. Uptime is measured from the current
    `PlatformMgr()` lifecycle's monotonic start time.
-   Active hardware, radio, and network fault lists are empty when Windows has
    not reported a corresponding Matter fault. Unsupported heap, thread, and
    driver-specific counters continue to return the base provider's explicit
    unsupported error rather than fabricated values.
-   Network interfaces are returned as an owned linked snapshot with native
    names, operational state, Matter interface type, available EUI-48/EUI-64
    hardware addresses, and bounded IPv4/IPv6 address lists.
-   Interface and address enumeration use one snapshot each. A transient
    adapter that disappears during enumeration is skipped without failing the
    complete General Diagnostics attribute read.

## The Windows DNS-SD backend

`src/platform/Windows/DnssdImpl.{h,cpp}` implements every
`src/lib/dnssd/platform/Dnssd.h` entry point active for non-Darwin builds
(`ChipDnssdInit`/`Shutdown`, `RemoveServices`/`PublishService`/
`FinalizeServiceUpdate`, `Browse`/`StopBrowse`, `Resolve`/
`ResolveNoLongerNeeded`, `ReconfirmRecord`) on top of the Win32 `windns.h`
service-discovery APIs (`DnsServiceRegister`/`DnsServiceBrowse`/
`DnsServiceResolve` and their `DeRegister`/`Cancel` counterparts). This is the
**native OS mDNS responder** (implemented by the `Dnscache` service): the port
never binds UDP 5353 and never implements any part of the mDNS wire protocol
itself.

-   Every `windns.h` entry point used here is asynchronous: a successful call
    returns `DNS_REQUEST_PENDING` and the outcome is delivered later, on an
    arbitrary OS thread, to the completion callback in the request. Every such
    callback deep-copies what it needs (freeing any Windows-owned
    `PDNS_RECORD`/`PDNS_SERVICE_INSTANCE` memory) and then marshals onto
    `PlatformMgr().ScheduleWork()` before invoking any `chip::Dnssd` callback,
    so no Matter callback ever runs on a Windows worker thread.
-   Publish/Browse/Resolve operations are individually reference-counted
    (`std::shared_ptr`) and kept alive, for exactly as long as the OS may still
    call back into them, by a private copy of that `shared_ptr` heap-allocated
    alongside the native request. A backend generation counter and registry
    membership checks let late or superseded callbacks (a repeated
    `PublishService()` for the same name/type/protocol/interface/port,
    `Shutdown()`, or a lost race with `StopBrowse()`) be recognized as stale
    and cleaned up (including a best-effort `DnsServiceDeRegister()` of an
    orphaned registration) without ever invoking a Matter callback for them.
-   Publication replacement is serialized by DNS-SD identity. When Matter
    removes and immediately republishes an instance during an advertisement
    refresh, the replacement registration waits for the previous
    cancel/deregister completion. This prevents a late asynchronous
    deregistration from removing the newly published record.
-   `StopBrowse()` calls `DnsServiceBrowseCancel()` and relies on Windows
    invoking the browse callback once more with a cancellation status
    (observed consistently on this host); if the cancel call itself fails
    (operation already finished), the required final `finalBrowse=true`
    callback is synthesized directly. Either path is guarded by a
    compare-and-swap flag so exactly one final callback is ever delivered.
-   `ResolveNoLongerNeeded()` removes the matching in-flight resolve(s) from
    the registry before calling `DnsServiceResolveCancel()`, so a resolve
    completion that arrives after abandonment is recognized as stale and
    suppressed rather than delivered.
-   Publish validates protocol, required name/type, and TXT entries up front:
    an embedded NUL byte in a binary TXT value is rejected
    (`CHIP_ERROR_INVALID_ARGUMENT`) rather than silently truncated, because
    `DNS_SERVICE_INSTANCE` TXT values are NUL-terminated wide strings and
    cannot represent one. UTF-8 to UTF-16 conversion failures (and the reverse,
    when reading resolved host names/TXT values back) are surfaced the same
    way. `ReconfirmRecord()` validates its hostname argument and then returns
    `CHIP_ERROR_NOT_IMPLEMENTED`: `windns.h` has no equivalent to the
    mDNSResponder "reconfirm record" hook this API models.
-   Interface selection follows the Matter `Inet::InterfaceId` contract: index
    0 (the `windns.h` convention for "any interface") when no interface is
    given, otherwise the interface's native Windows interface index
    (`InterfaceId::GetInterfaceIndex()`, an `IfLuid`-backed conversion also
    used by `//src/inet/windows`). Resolved results convert the numeric
    interface index back with `ConvertInterfaceIndexToLuid()`.
-   Subtype browse queries retain the full subtype in the wire query (for
    example `_L840._sub._matterc._udp.local`) while matching the returned PTR
    target and reporting the result using its base type
    (`_matterc._udp.local`). This keeps filtered commissionable-node discovery
    compatible with the Matter browse-to-resolve contract.
-   `IP6_ADDRESS` (the `windns.h` IPv6 address type) is a union whose typed
    `In6` alternative is compiled in only behind `#ifdef IN6_ADDR` -- a guard
    that is never satisfied in the Windows SDK headers, because `IN6_ADDR` is
    only ever a `typedef`, never a `#define`. The backend copies the 16
    address bytes directly (`IP6_ADDRESS::IP6Byte`) instead of relying on that
    member.

New files:

-   `src/platform/Windows/DnssdImpl.{h,cpp}` -- the backend. The header also
    exposes a small set of pure, deterministic helpers (`MakeFullServiceType`,
    `GetBaseServiceType`, `Utf8ToWide`/`WideToUtf8`, `MapServiceStatus`,
    `InterfaceIdFromIndex`) used by the implementation and exercised directly
    by the smoke test.
-   `build/config/win/tests/msvc_windows_dnssd_smoke.cpp` -- the focused
    runtime smoke, built as `msvc-windows-dnssd-smoke` under the same
    `chip_windows_device_layer_probe` opt-in used by the rest of the Device
    Layer probe.

```powershell
gn gen out\win-devlayer-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_device_layer_probe=true'
ninja -C out\win-devlayer-x64 msvc-windows-dnssd-smoke.exe
.\out\win-devlayer-x64\msvc-windows-dnssd-smoke.exe
```

The smoke covers, deterministically and without any flaky network assertion:

-   the UTF-8/UTF-16, status-mapping, service-type, and interface-index
    conversion seams, in isolation;
-   synchronous argument validation for every entry point, both before and
    after `Init()`, including the embedded-NUL TXT rejection and the
    unimplemented `ReconfirmRecord()` contract;
-   the init/shutdown/reinit lifecycle, including that every operation is
    rejected with `CHIP_ERROR_INCORRECT_STATE` after `Shutdown()` and that a
    fresh `Init()` recovers it;
-   a **live** publish of a uniquely named (GUID) service under
    `_matterc._udp`, asserting the success callback reports the base
    `_matterc._udp` type and the instance name -- this exercises the real
    `DnsServiceRegister()` path against the host's `Dnscache` responder;
-   an immediate remove-and-republish of the same service identity, verifying
    that asynchronous deregistration is completed before replacement
    registration;
-   a **live** browse for `_matterc._udp` followed by `StopBrowse()`, asserting
    exactly one `finalBrowse=true` callback is ever delivered -- this
    exercises the real `DnsServiceBrowse()`/`DnsServiceBrowseCancel()`
    cancellation contract;
-   a resolve started and then immediately abandoned via
    `ResolveNoLongerNeeded()`, asserting the resolve callback is never
    invoked.

It deliberately does **not** assert that browse/resolve discover any published
service over multicast: that depends on host firewall profile, adapter state,
and (per manual investigation on this development host) mDNS implementations
commonly suppress a host from resolving its own just-published record over
loopback, so a same-host resolve did not complete within a generous 10-second
wait even though the identical name published and browsed successfully. This
is host/environment-dependent and is called out as a known limitation below
rather than encoded as a flaky test assertion.

### On-network discovery acceptance tool

`msvc-windows-dnssd-discovery.exe` is a focused native Windows acceptance tool
for the Phase 3 real-device gate. It browses `_matterc._udp.local`, resolves
each instance through the Windows backend, and prints the host, port, interface,
IPv4/IPv6 addresses, and TXT entries. It exits with code `0` after resolving at
least one commissionable device, `2` when the timeout expires without a
resolved device, and `1` for an invalid invocation or platform error.

Build and run it from an initialized x64 environment:

```powershell
ninja -C out\win-devlayer-x64 msvc-windows-dnssd-discovery.exe
.\out\win-devlayer-x64\msvc-windows-dnssd-discovery.exe 30
```

Before running it against a Matter bulb, commission the bulb onto the same LAN
with an existing ecosystem controller and then open its Matter commissioning
window. A factory-reset Wi-Fi bulb that has not yet received Wi-Fi credentials
cannot advertise over IP and therefore requires BLE (or another supported
commissioning transport) before this tool can discover it. Resolving a device
with this executable proves native Windows browse/resolve and LAN reachability;
it does not itself establish a Matter fabric or replace the later controller
commissioning acceptance test.

### Firewall and responder notes

-   The backend depends on the Windows `Dnscache` service (displayed as
    "DNS Client"); on the development host used for this milestone it was
    already running and required no configuration. The separate Function
    Discovery services (`FDResPub`/`fdPHost`) were stopped throughout and were
    not required for `DnsServiceRegister`/`Browse`/`Resolve` to function.
-   No inbound/outbound Windows Firewall rule changes were required to
    exercise the live publish and browse/cancel smoke checks on this host.
    Production deployments should still confirm the "mDNS" / "Network
    Discovery" firewall rule group (or an application-specific rule) is
    enabled for the profile in use; this port does not open the mDNS port
    itself; only the OS responder does.
-   Same-host self-resolve reliability is not guaranteed (see above); this is
    an mDNS-implementation characteristic, not specific to this backend.

## The Windows controller-side discovery foundation

Canonical `//src/lib/dnssd:dnssd` compiles the real upstream
`chip::Dnssd::Resolver` and `DiscoveryImplPlatform` implementation against the
canonical native Windows Device Layer and `windns.h` backend. This is the same
controller-facing discovery path used by other `chip_mdns_platform` targets;
it is not a second hand-written discovery implementation.

The earlier `dnssd_windows` intermediate target was removed after generic
`//src/platform` dispatch and canonical configuration were wired for Windows.
The upstream DNS-SD sources remain unchanged except for the private C++17
build transform needed for a designated initializer that MSVC rejects under
`/std:c++17`.

`msvc-windows-controller-discovery.exe` initializes that real resolver, starts
commissionable-node discovery, reports resolved `CommissionNodeData`, stops
discovery, and waits for the asynchronous final browse callback to release
the retained `DiscoveryContext` before shutdown. Calls into the resolver and
context are serialized with the CHIP stack lock because their internal state
is intentionally non-atomic:

```powershell
gn gen out\win-devlayer-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_canonical_compile_probes=true chip_windows_device_layer_probe=true chip_with_nlfaultinjection=false'
ninja -C out\win-devlayer-x64 msvc-windows-controller-discovery.exe
.\out\win-devlayer-x64\msvc-windows-controller-discovery.exe 30
```

It exits `0` when at least one commissionable node resolves, `2` when discovery
starts and stops correctly but the timeout expires without a node, and `1` for
a platform or lifecycle failure. With no Matter accessory currently on the
development LAN, the executable reaches the expected exit code `2`.

### What this does not prove

-   No real device has been commissioned yet. This proves the controller's
    browse-to-resolve path, not PASE/CASE establishment, fabric persistence,
    attribute operations, subscription, or fabric removal.
-   The full `examples/chip-tool` closure still requires messaging,
    session-establishment, Interaction Model client, and Windows application
    wiring. Canonical platform dispatch, credentials, storage, and DNS-SD are
    now available.

## The canonical controller stack closure

The next Phase 3 compile gate now builds the complete canonical
`//src/transport`, `//src/messaging`, `//src/protocols/secure_channel`,
`//src/app:interaction-model`, and `//src/app:app` libraries with MSVC in the
same Windows Device Layer graph. This includes `SessionManager`,
`ExchangeManager`, reliable messaging, PASE, CASE, read/write clients, and the
Interaction Model engine.

The port keeps its C++17 contract. MSVC portability changes replace
GCC-supported C++20 designated initializers with C++17 initialization, use
fixed-width bitfield storage where MSVC otherwise allocates mixed underlying
types separately, and replace the POSIX-only `ssize_t` use in `ReadClient`.
Generated enum-check headers are handled by the existing forced-include
compatibility shim rather than editing generated files.

`msvc-canonical-controller-stack-smoke.exe` links the complete closure,
initializes and shuts down the canonical Windows `PlatformManager`, constructs
the session, exchange, PASE, and CASE objects, and resolves the Interaction
Model singleton. It passes on x64 and cross-builds for ARM64. This is a
link/lifecycle acceptance test, not a commissioning test: it does not initialize
a fabric-backed `SessionManager`, establish PASE or CASE, or send an
Interaction Model request.

Canonical `//src/controller:controller` also builds under MSVC for x64 and
ARM64. This brought the controller factory, persistent `FabricTable`,
commissioner, auto-commissioning, BDX, User Directed Commissioning, and
attestation-verifier closures into the Windows graph. Windows leaves
`chip_config_network_layer_ble` disabled by default until the Phase 4 WinRT
transport exists; on-network commissioning does not require that transport.

One canonical layering defect was fixed as part of this gate:
`//src/protocols:type_definitions` now carries `Protocols.cpp`, as its existing
comment required. `SessionManager` and `ExchangeManager` call
`GetProtocolName()` and `GetMessageTypeName()`, so a target containing only
`Protocols.h` compiled but could not link a real executable.

## Focused native Windows commissioner

`msvc-windows-controller.exe` is the first non-interactive controller
executable. It composes the canonical controller library with the Windows
Device Layer and:

-   adapts the Windows KVS through `KvsPersistentStorageDelegate`;
-   persists the operational key and certificate stores, fabric table, group
    state, IPK, and example credential issuer under
    `%LOCALAPPDATA%\Matter\KVS\v1`;
-   generates the controller operational key through
    `PersistentStorageOperationalKeystore`, rather than injecting an
    in-memory key into `FabricTable`;
-   creates one controller fabric on first launch and restores the same fabric
    on later launches;
-   starts setup-code pairing with
    `DiscoveryType::kDiscoveryNetworkOnly`;
-   reconnects to a commissioned node after process restart and can read its
    OnOff attribute or invoke its On and Off commands; and
-   bounds pairing, CASE connection, and Interaction Model waits so a missing
    node or commissioning window does not leave the process running
    indefinitely.

Build it and confirm controller-fabric persistence:

```powershell
ninja -C out\win-devlayer-x64 msvc-windows-controller.exe
.\out\win-devlayer-x64\msvc-windows-controller.exe status
.\out\win-devlayer-x64\msvc-windows-controller.exe status
```

The first run reports `Created controller fabric`; subsequent runs report
`Restored controller fabric` with the same fabric index and controller node ID.
To commission an accessory that is already on the same LAN, open its
commissioning window and run:

```powershell
.\out\win-devlayer-x64\msvc-windows-controller.exe pair 1 <setup-code> 180
```

The node ID accepts decimal or `0x`-prefixed input. The optional timeout is
1-600 seconds and defaults to 120.

After commissioning, restart the executable and use the device's OnOff
endpoint (endpoint 1 for the hardware-tested bulb):

```powershell
.\out\win-devlayer-x64\msvc-windows-controller.exe read-onoff 1 1 180
.\out\win-devlayer-x64\msvc-windows-controller.exe off 1 1 180
.\out\win-devlayer-x64\msvc-windows-controller.exe on 1 1 180
.\out\win-devlayer-x64\msvc-windows-controller.exe subscribe-onoff 1 1 30
```

Every invocation prints a prominent, authoritative `=== ... ===` result as its
final line after the verbose Matter protocol and shutdown logs. Successful
pairing and OnOff commands print `SUCCEEDED`; a successful attribute read
prints `=== ON/OFF ATTRIBUTE: ON ===` or `OFF`; and failures include the
process exit code. The subscription command allows up to 120 seconds for CASE
and subscription establishment, records the initial value, automatically sends
the opposite OnOff command, and waits for the requested number of seconds for
the resulting pushed value report. Success therefore proves subscription
delivery without requiring a second process or a manual toggle. The final line
includes the report and interruption counts; the bulb remains in the newly
commanded state.

After all operational tests are complete, remove this controller's fabric from
the accessory:

```powershell
.\out\win-devlayer-x64\msvc-windows-controller.exe remove-fabric 1 180
```

This removes the Windows controller from the remote node; the local controller
identity remains persisted so it can commission another node or recommission
the bulb after a new commissioning window is opened.

This is a development acceptance tool, not a production commissioner. Its
example certificate authority stores private keys in cleartext, its default
IPK is a test value, and it deliberately permits commissioning to continue
after device-attestation verification fails. Production software must provide
a protected operational keystore, unique IPKs, and an approved PAA trust and
revocation policy. The current executable has proven fabric creation,
persistence across process restart, on-network pairing startup, bounded
cancellation, and clean teardown on x64. A real Smart Multicolor Bulb has also
completed PASE over IPv4, certificate signing, trusted-root installation, and
NOC installation. The development attestation delegate allowed commissioning
to continue after the configured empty PAA store could not verify the vendor
PAA; this does not constitute production attestation validation.

The first hardware run did not reach `CommissioningComplete`. Operational
DNS-SD returned both the working IPv4 address and a link-local IPv6 address.
The generic address scorer preferred link-local IPv6, but the bulb did not
respond to CASE Sigma1 on that address. Windows had retained only the single
highest-scored result, so each automatic retry repeated the same unusable
address instead of trying IPv4. `SystemPlatformConfig.h` now retains five
resolved addresses, matching Linux and Darwin. This activates the existing
`OperationalSessionSetup` fallback path: after a CASE timeout it can consume
the next resolved address without starting another lookup. A second hardware
run confirmed that behavior: link-local CASE timed out, CASE immediately
retried IPv4, and the bulb returned Sigma2.

That run exposed a separate persistence bug in the focused application. Its
original fabric-creation path injected a temporary operational keypair into
`FabricTable`. The table copied the key for that process but intentionally
persisted only fabric metadata and certificates; after restart, the restored
fabric therefore lacked the private key needed to sign CASE Sigma3. The
controller now asks `PersistentStorageOperationalKeystore` to generate the
controller key and commits it with the new fabric. At startup it detects and
deletes a legacy fabric whose operational key is missing before creating a
valid replacement. An x64 two-process test confirmed the repair on the first
run and restoration of the replacement fabric and key on the second.

A third hardware run validated the repaired path end to end. CASE timed out on
the link-local IPv6 address, retried the retained IPv4 address, received
Sigma2, signed and sent Sigma3 with the restored controller key, and activated
the secure session. The bulb then returned `errorCode=0` to
`CommissioningComplete`, and commissioning completed successfully for node 1.
Restart-based CASE reconnection and OnOff read, Off, and On operations were
then validated from separate processes on the Windows machine connected to the
bulb's LAN, including visible changes from both commands. Because operational
discovery ranks the bulb's advertised link-local IPv6 address above IPv4, each
new process first exhausts CASE retransmissions on the non-responsive IPv6
address before the retained IPv4 fallback succeeds. This delay is currently a
known interoperability issue rather than a command failure. Subscription
delivery and fabric removal remain to be hardware-validated.

### Cross-build

The resolver, canonical controller library, and focused commissioner executable
cross-build and link as ARM64. They have not run on native Windows ARM64
hardware.

## Cross-build the ARM64 smoke target

Initialize a new PowerShell process for the ARM64 compiler environment:

```powershell
. .\scripts\setup\windows.ps1 -Architecture arm64
gn gen out\win-arm64-msvc-smoke --args='target_os="win" target_cpu="arm64" chip_device_platform="none"'
ninja -C out\win-arm64-msvc-smoke
dumpbin /headers out\win-arm64-msvc-smoke\msvc-toolchain-smoke.exe |
    Select-String "machine \(ARM64\)"
```

The DNS-SD backend and its smoke target cross-build the same way
(`chip_device_platform="windows" chip_windows_device_layer_probe=true`,
target `msvc-windows-dnssd-smoke.exe`) and were confirmed to compile and link
as `ARM64` for this milestone; like every other Windows Device Layer
component so far, **ARM64 runtime support has not been exercised on native
ARM64 hardware** and must not be claimed as supported until it is.

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
| Crypto | CryptoPAL API and credential logic | BoringSSL selected, compiled with MSVC (asm disabled). The real upstream `src/crypto/tests` GoogleTest suites (80 tests including the full `TestChipCryptoPAL` CryptoPAL suite) pass on x64 at `/std:c++17` against the canonical `//src/crypto:crypto` library and a focused CHIPCert subset (upstream sources adapted to C++17 by a build-time transform), and cross-build as `AA64`. The focused 23-test BoringSSL driver is retained. The full monolithic credentials/Device-Layer closure is deferred | Dependency |
| Device Layer | Generic static-polymorphism mixins | Phase 3 lands the native `PlatformManager` (lifecycle, event loop, cross-thread work posting), `KeyValueStoreManager` (per-user versioned root, safe key encoding, atomic durable writes, integrity checks), typed/public configuration management with scoped reset, OS-managed Ethernet/Wi-Fi `ConnectivityManager` with native interface/address change events, the General Diagnostics provider, and a native DNS-SD backend over `windns.h`; BLE and process restart after reset remain | Platform contract |
| DNS-SD | Resolver and advertiser interfaces | Implemented (`src/platform/Windows/DnssdImpl.cpp`) over the Win32 `windns.h` service-discovery APIs and the native OS mDNS responder; no firewall rule automation is provided (documented, not automated) | Platform contract |
| BLE | Transport and commissioning state machines | No WinRT scanner, central connection, GATT server, advertising, or callback serialization | Platform contract |
| Controller | Portable command and controller logic | Controller library/application closure, persistent fabric wiring, cancellation, terminal behavior, and BLE. Canonical transport, messaging, PASE/CASE, and Interaction Model libraries now compile and link on Windows. | Platform and application |
| Server | Portable cluster and Interaction Model code | No Windows host lifecycle, network driver, event transport, named-pipe replacement, or example target | Platform and application |
| Tests | Portable C++ test bodies and Python suites | Pigweed host toolchain assumptions, executable naming, process control, paths, BLE hardware, and ARM64 runners | Build and test harness |

The port must not cast a WinSock `SOCKET` to `int`. `SOCKET` is pointer-sized
on 64-bit Windows, while the existing POSIX endpoint implementation frequently
stores descriptors as signed integers.

### Target-closure inventory

The Phase 0 inventory uses the eventual product roots rather than treating the
bootstrap graph as the finished SDK:

| Closure | Root target | Reusable dependencies | First Windows blockers | Owning phase |
|---|---|---|---|---|
| Core SDK | `//src/lib`, `//src/system:system`, `//src/inet:inet`, `//src/crypto:crypto` | Core/support protocols, BoringSSL, System and Inet contracts | Complete System event loop, Windows errors, typed handles in shared Inet, platform entropy, and remaining MSVC attributes | Phase 1 build gate, then Phase 2 runtime |
| Controller | `//examples/chip-tool` | Command model, controller, JsonCpp, INI parser, BoringSSL | Core closure, Windows Device Layer, DNS-SD, storage, cancellation, and BLE. The generic `//src/platform` umbrella Device Layer dispatch now composes canonical Windows configuration, connectivity, KVS, diagnostics, endpoint lifecycle, and DNS-SD without mixing the focused command-line configuration. `//src/credentials` compiles, and canonical `//src/lib/dnssd:dnssd` links and runs the real `DiscoveryImplPlatform` path in `msvc-windows-controller-discovery.exe`; the temporary `dnssd_windows` target is removed. The remaining controller closure is messaging, PASE/CASE/session establishment, the Interaction Model client, application CLI/storage wiring, and BLE -- see "Wiring the generic Device Layer dispatch to canonical libraries" above | Phases 3–5 |
| Server | `//examples/all-clusters-app` plus a new Windows host target | Interaction Model, clusters, app server, generated data model | Windows app lifecycle, Device Layer, storage, DNS-SD, network drivers, and test-event transport | Phases 3 and 5 |
| Unit tests | `//src/lib/core/tests:tests`, then System/Inet/Crypto/transport/secure-channel suites | Existing test bodies and GoogleTest | The focused Windows target runs existing core tests; the upstream `src/crypto/tests` (80 tests), `src/system/tests` + `src/inet/tests` (93 tests), and host-neutral `src/transport/tests` + `src/protocols/secure_channel/tests` (40 tests) suites run on x64 via GoogleTest facades and cross-build as `AA64`; suites reaching the Device Layer (messaging / session establishment / SessionManager) are deferred | Phase 2 |

Failures are tracked in six categories:

| Category | Confirmed examples |
|---|---|
| Build syntax | GNU warning flags passed to MSVC; duplicate object outputs when one source belongs to multiple targets |
| Compiler/language | GNU attributes, MSVC warnings-as-errors, and missing explicit C11 mode |
| POSIX API | `pthread`, POSIX clocks, pipes, `ifaddrs`, signals, `errno`, and filesystem assumptions |
| Dependency | Linux-only OpenSSL `pkg-config` acquisition and an ungenerated Pigweed environment override |
| Platform contract | WinSock handle width, event wakeups, DNS-SD, BLE, storage, diagnostics, and interface monitoring |
| Test harness | Pigweed backend/toolchain configuration, process control, executable naming, and ARM64 runtime hardware |

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
-   Win32 and WinSock codes use the 24-bit `ChipError::Range::kOS` value.
    Values that cannot be represented are rejected instead of truncated.
    Failed HRESULTs use `kPlatformExtended`: the fixed failure-severity bit is
    restored when converting back to HRESULT, preserving the remaining 31
    facility, customer, and code bits.
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

**BoringSSL is selected for the initial Windows CryptoPAL closure.** The
repository-pinned source builds with MSVC for x64 and ARM64, matches the Darwin
backend choice, and does not require a machine-global package manager.
Assembly is disabled initially; enabling architecture-specific assembly is a
later measured optimization.

The repository-pinned BoringSSL CryptoPAL now compiles with native MSVC for
x64 and ARM64, and a focused GoogleTest driver
(`msvc-crypto-boringssl-tests`) exercises the real PAL primitives against
published NIST/RFC known-answer vectors and correctness properties: SHA-256
(one-shot and streaming), HMAC-SHA256, HKDF-SHA256, PBKDF2-SHA256,
AES-CCM-128 AEAD (round-trip, determinism, and authentication), AES-CTR-128
(RFC 3686), ECDSA P-256 sign/verify and keypairs, deterministic ECDSA
(RFC 6979), ECDH key agreement, the DRBG, constant-time comparison, P-256
keypair serialization/deserialization and raw-bit import (NIST ECDSA2VS),
PKCS#10 CSR generation/verification, X.509 public-key extraction from a
self-contained certificate, the SPAKE2+ handshake (draft-01 RFC known-answer
plus a full mutual-agreement run), the `DefaultSessionKeystore`
derive/export/destroy surface, and negative input boundaries. The upstream
`src/crypto/tests` vector headers express their data with C++20 designated
initializers (and some use zero-length arrays), which MSVC rejects under the
repository-required C++17 standard, so the needed known-answer vectors
(including `P256_test_vectors.h` and `SPAKE2P_RFC_test_vectors.h`) are
reproduced locally with C++17 positional aggregate initialization, copied
verbatim from those sources. Twenty-three tests pass on x64; the
closure cross-builds and inspects as `AA64` for ARM64. Additional surgical MSVC
portability fixes were required in shared upstream sources, each guarded so
non-Windows behavior is unchanged: a weak-symbol fallback macro for the default
`P256Keypair` methods in `CHIPCryptoPAL.cpp`, replacing a zero-length group-key
salt array with a one-byte placeholder plus an explicit length of `0`, an MSVC
`__declspec` form of `NO_INLINE` in `TLVWriter.cpp`, and `#if !defined(_WIN32)`
guards on the unused POSIX `<getopt.h>`/`<unistd.h>` includes in
`ASN1Time.cpp`. Canonical graph compilation additionally uses a portable
restrict macro, MSVC packing for BLE service data, conforming log-category
macro expansion in the presence of Windows' `ERROR` macro, and platform guards
around GCC/Clang-only compiler flags.

Mbed TLS remains a tested fallback and builds in the same dependency smoke.
OpenSSL is rejected for the initial closure because its current Matter GN
integration imports Linux `pkg-config` and provides no repository-pinned
Windows acquisition path. A machine-global OpenSSL installation would violate
the clean-machine and reproducibility requirements.

The Phase 2 crypto gate now runs the complete upstream `src/crypto/tests`
GoogleTest suites (80 tests: `TestSessionKeystore`,
`TestGroupOperationalCredentials`, `TestPersistentStorageOpKeyStore`, and the
full `TestChipCryptoPAL` CryptoPAL suite) against the canonical
`//src/crypto:crypto` library on x64 at the repository `/std:c++17` default,
with both executables cross-building and inspecting as `AA64` for ARM64.
Enabling them required surgical, portable Windows fixes, each preserving
non-Windows behavior and the C++17 contract:

-   A GoogleTest-only `lib/core/StringBuilderAdapters.h` facade (in
    `build/config/win/tests/crypto_test_support`) plus a matching `PrintTo`
    implementation, so the upstream test sources compile without the Pigweed
    `pw_string` closure (which reaches Fuchsia `stdcompat` `<bit>` and GCC
    builtins such as `__builtin_clzll` that MSVC does not provide). The
    GoogleTest `pw_unit_test/framework.h` facade is reused unchanged, and an
    empty `<unistd.h>` shim satisfies the upstream test's unused POSIX include.
-   The upstream test bodies and their vector headers use GCC/Clang extensions
    that MSVC accepts only under `/std:c++20` or not at all: C++20
    designated-initializer aggregates (used in both the vector data and the test
    bodies' local test-case tables), C99/GCC compound literals, and unsized empty
    arrays. Rather than override the standard or edit the upstream files,
    `build/config/win/tests/gen_msvc_cxx17_source.py` produces `/std:c++17`
    build-artifact copies of the test bodies and vector headers into a private
    gen directory: designated initializers become equivalent positional
    aggregate initialization, compound literals become named arrays, and unsized
    empty arrays get an unused placeholder element with any `sizeof` of that
    array rewritten to `0`. The transformed test bodies are compiled from the gen
    directory so their quoted `"..._test_vectors.h"` includes resolve to the
    transformed vectors beside them (MSVC does not let `/I` override a quoted
    include found in the source file's own directory). Every rewrite is
    mechanical and semantics-preserving, and is validated at runtime by the
    known-answer assertions in the test bodies. The upstream files are untouched.
-   A focused `//src/credentials/windows:windows-chip-cert` subset compiles just
    the CHIPCert / attestation translation units `TestChipCryptoPAL` needs at
    `/std:c++17`, avoiding the monolithic `//src/credentials:credentials` library
    whose `FabricTable` / `LastKnownGoodTime` / `GroupDataProvider` /
    `PersistentStorageOpCertStore` sources pull the unported Windows Device
    Layer. `//src/credentials/windows:windows-cert-test-vectors` compiles the
    already-C++17-compatible `CHIPAttCert_test_vectors.cpp` as-is and the
    compound-literal `CHIPCert_test_vectors.cpp` / `TestPAAStore.cpp` through the
    same C++17 transform. Both reuse a shared
    `//build/config/win:upstream_sdk_warnings` relaxation (`/W3 /WX-` with
    specific `/wdNNNN` mappings) rather than the strict `/W4 /WX` default -- a
    narrowly justified warning mapping for verbatim GCC/Clang sources. The one
    genuine SDK change is `CHIPCert_Internal.h`, whose GCC compound-literal
    constant is rewritten to a portable named array (valid on every compiler).

The focused `msvc-crypto-boringssl-tests` driver and its locally reproduced
vectors are retained as an independent correctness check. The remaining crypto
gate item is the full monolithic credentials/Device-Layer closure (deferred
to the Device Layer phase); binary-size measurement and the explicit
enterprise/FIPS deployment statement are now recorded in the crypto
release-engineering section below.
Selecting the build dependency in Phase 0 does not pre-approve runtime
correctness.

### Crypto release engineering (footprint, FIPS, servicing)

This is the Phase 2 release-engineering record for the BoringSSL CryptoPAL
closure. It does not by itself make Windows crypto a released support claim
(see the crypto decision gate and the feature-status table).

**Assembly policy (`OPENSSL_NO_ASM`).**
`third_party/boringssl/repo/BUILD.gn` compiles BoringSSL with
`defines = [ "OPENSSL_NO_ASM=1" ]` for every Windows target, so only the
portable C implementations build and no `.asm`/`.S` source is assembled. This
is the intentional initial policy: correctness first, a single `cl`-only
toolchain, and no assembler on the clean machine. Enabling
architecture-specific assembly is a later, measured optimization and would
require, per architecture:

-   x64: remove `OPENSSL_NO_ASM`, wire the `crypto_sources_nasm` entries
    (`src/gen/bcm/*-x86_64-win.asm`) into the GN target, and add **NASM** to the
    toolchain -- these files use NASM syntax that MSVC's `ml64` does not accept.
-   ARM64: remove `OPENSSL_NO_ASM`, wire the `src/gen/bcm/*-armv8-win.S`
    GAS-syntax sources, and assemble them with `clang`/`clang-cl`.
    The `cpu_aarch64_win.c` runtime feature probe is already in `crypto_sources`.

Either path adds an assembler to the clean-machine requirement and must be
re-measured for size and re-validated for correctness before adoption.

**Build type and CRT.** The Windows port default is `is_debug = true`
(`build/config/BUILDCONFIG.gn`), so `build/config/win/BUILD.gn` selects the
dynamic **debug** CRT: `/MDd /Od /Z7` for debug versus `/MD /O2` for release,
with executables linking `/DEBUG` in debug and `/OPT:REF /OPT:ICF` in release.
The artifacts measured below are debug/`/MDd`; `dumpbin /DEPENDENTS` on a debug
`msvc-crypto-pal-tests.exe` shows it importing `MSVCP140D.dll`,
`VCRUNTIME140D.dll`, `VCRUNTIME140_1D.dll`, `ucrtbased.dll`, and `KERNEL32.dll`
-- the developer-only debug CRT, which is **not redistributable**. CRT models
must not be mixed: a static-CRT (`/MT`) or release (`/MD`) consumer must rebuild
BoringSSL and the entire SDK with the same model. Release footprint is smaller
and is not represented here.

**Static-link footprint (measured).** Deterministic on-disk PE/COFF sizes taken
with native tooling (`Get-Item` length; machine type via `dumpbin /HEADERS`)
from the existing debug build trees `out\win-crypto-tests-x64` and
`out\win-crypto-tests-arm64`. Measurement configuration:
`target_os="win"`, `target_cpu="x64"|"arm64"`, `chip_device_platform="none"`,
`chip_windows_canonical_compile_probes=true`, `chip_build_tests=false`,
`chip_build_tools=false`, `is_debug` default (`/MDd /Od /Z7`),
`OPENSSL_NO_ASM=1`. Reproduce with
`scripts/tools/windows_artifact_sizes.ps1` (native, no external dependencies;
`-IncludeMachineType` adds the `dumpbin` COFF machine column).

| Artifact | Kind | x64 (bytes) | ARM64 (bytes) |
|---|---|---|---|
| `obj/third_party/boringssl/repo/boringssl.lib` | static lib (COFF archive) | 12,860,366 | 12,988,326 |
| `obj/src/crypto/windows/windows-crypto-boringssl.lib` (CryptoPAL) | static lib | 2,986,184 | 2,952,544 |
| `obj/src/crypto/libChipCrypto.lib` (`//src/crypto:crypto`) | static lib | 759,332 | 752,136 |
| `msvc-crypto-pal-tests.exe` | linked exe | 1,859,584 | 1,846,784 |
| `msvc-crypto-upstream-tests.exe` | linked exe | 1,357,824 | 1,352,704 |
| `msvc-crypto-boringssl-tests.exe` | linked exe | 1,390,592 | 1,377,280 |

COFF machine type via `dumpbin /HEADERS`: x64 = `8664`, ARM64 = `AA64`. The
`.lib` archives are **not** deliverable footprint -- they are collections of
unlinked COFF object members carrying `/Z7` debug info, and are far larger than
the code that survives linking. The linked `.exe` sizes reflect actual code
plus embedded debug; a shipping release build (`/MD /O2 /OPT:REF /OPT:ICF`, no
test framework) would be materially smaller and must be measured separately
before any size is quoted as a product number.

**FIPS / non-FIPS status.** This repository builds standard, **non-FIPS**
BoringSSL. There is no `FIPS=1` build define anywhere in the GN integration; the
`src/crypto/fipsmodule/*` sources listed in `BUILD.generated.gni` are
BoringSSL's ordinary code layout, not a validated cryptographic-module boundary
with power-on self-tests and integrity checks. No FIPS 140-2/140-3 claim can be
made from this build. Enterprise or FIPS deployments require a **separately
validated** provider/backend (a validated module build, or an OS/CNG-backed
provider) whose validation cannot be inherited from this closure.

**Licensing and notice obligations (repository facts only).** BoringSSL ships a
single `third_party/boringssl/repo/src/LICENSE` (11,558 bytes): a fork of
OpenSSL under the dual OpenSSL/SSLeay BSD-style licenses, with wholly new
Google-authored files under ISC and `third_party/fiat` (which, unlike other
`third_party` directories, is compiled into non-test libraries) under MIT. It
carries no separate `NOTICE`. The Matter SDK root is Apache-2.0 (`LICENSE`) with
a `NOTICE`, and vulnerability reporting routes through CSA-IOT (`SECURITY.md`).
A Windows binary that statically links BoringSSL must therefore reproduce the
BoringSSL `LICENSE` (OpenSSL + SSLeay + ISC + the fiat MIT text) in its
third-party notices; the fiat MIT text in particular must be carried because
fiat is linked into shipping libraries. The fallback Mbed TLS is Apache-2.0 and
JsonCpp is Public-Domain/MIT (see the dependency decision record).

**Dynamic-linkage, DLL search, and signing (future).** The current closure links
BoringSSL statically (`static_library` targets; the Windows default config
defines `CHIP_STATIC_LIBRARY=1`), so there is no BoringSSL DLL and no crypto
DLL search-path exposure today. If dynamic linkage is adopted later, then
before shipping: load dependent DLLs only from constrained, non-writable search
paths (the packaged install directory or `AddDllDirectory` /
`LOAD_LIBRARY_SEARCH_*`, never the current working directory); Authenticode-sign
every shipped DLL and the loader-facing executables; and document the VC++
redistributable/CRT servicing policy, since the debug CRT DLLs listed above are
developer-only and must be replaced by the redistributable release CRT.

**Servicing and pinned commit.** BoringSSL is pinned at gitlink
`9cac8a6b38c1cbd45c77aee108411d588da006fe` under
`third_party/boringssl/repo/src` (`.gitmodules` tracks upstream `master`). The
Windows port maintainers own the GN integration and its servicing: advancing
the pin, tracking upstream BoringSSL security fixes, and re-running the crypto
gate on x64 and ARM64 after any bump. SDK vulnerability response follows the
root `SECURITY.md` (CSA-IOT). There is no automatic update mechanism; updates
are deliberate submodule bumps.

### Dependency decision record

| Dependency | Decision and owner | License | Reproducible acquisition | x64 | ARM64 |
|---|---|---|---|---|---|
| BoringSSL | Selected crypto backend; Windows port maintainers own GN integration and servicing | ISC/OpenSSL/SSLeay plus per-directory third-party licenses | Gitlink `9cac8a6b38c1cbd45c77aee108411d588da006fe` at `third_party/boringssl/repo/src` | Build and runtime SHA-256 check | Build and `AA64` inspection |
| Mbed TLS | Retained fallback; Windows port maintainers own compatibility | Apache-2.0 option from the dual license | Gitlink `6a58fa8122c9bb8ee1644733d21b5677c93f1169` at `third_party/mbedtls/repo` | Build and runtime SHA-256 comparison | Build and `AA64` inspection |
| OpenSSL | Rejected for the initial Windows closure | Apache-2.0 for current upstream releases | No repository-pinned Windows path in the current GN integration | Not eligible | Not eligible |
| JsonCpp | Selected existing controller JSON dependency | Public Domain/MIT | Gitlink `8519b8381f3c741ad1421f88237b1deda0b11412` at `third_party/jsoncpp/repo` | Build and runtime parse check | Build and `AA64` inspection |
| Argument parsing | Keep the repository-owned non-interactive `chip-tool` command parser; defer editline interactive mode | Matter SDK Apache-2.0 | Current source tree | Closure inventory complete | Closure inventory complete |
| Unit-test harness | Reuse existing test bodies through a Windows `pw_unit_test` compatibility include backed by GoogleTest; port the full Pigweed facade with the complete test closure | Pigweed Apache-2.0; GoogleTest BSD-3-Clause | Existing pinned gitlinks; native setup generates the required Python environment override | 23 core tests pass | Same tests build and inspect as `AA64` |

### Architecture decision record

| ID | Decision | Status |
|---|---|---|
| WIN-001 | GN/Ninja with MSVC x64 and ARM64 toolchains remains canonical | Accepted and built |
| WIN-002 | Use the dynamic CRT: `/MDd` for debug and `/MD` for release | Accepted |
| WIN-003 | Use repository-pinned BoringSSL for the initial CryptoPAL closure | Accepted; CryptoPAL compiles on x64/ARM64. The real upstream `src/crypto/tests` GoogleTest suites (80 tests, incl. full `TestChipCryptoPAL`) pass on x64 against the canonical `//src/crypto:crypto` library and a focused CHIPCert subset, and cross-build as `AA64`; the monolithic credentials/Device-Layer closure is deferred |
| WIN-004 | Preserve WinSock `SOCKET` in a typed, pointer-width native handle | Accepted and prototyped |
| WIN-005 | Start with `WSAPoll` and WinSock wake sockets behind the System callback contract | Accepted |
| WIN-006 | Use Windows DNS Service Discovery without an unconditional competing UDP 5353 responder | Accepted; realized by the native DNS-SD backend (`src/platform/Windows/DnssdImpl.cpp`) over `windns.h` `DnsServiceRegister`/`Browse`/`Resolve`; 65-check smoke covers the lifecycle plus live publish, serialized remove/republish, and browse/cancel cycles on x64 and cross-builds as ARM64 |
| WIN-007 | Use C++/WinRT BLE and marshal completions onto the Matter event loop | Accepted |
| WIN-008 | Use versioned `%LOCALAPPDATA%` state by default with an injectable service path and explicit ACL ownership | Accepted; realized by the native `KeyValueStoreManager` (default `%LOCALAPPDATA%\Matter\KVS\v1`, injectable root, atomic durable writes, integrity checks, single-owner lock, scoped factory reset) |
| WIN-009 | Support unpackaged and packaged desktop applications; surface capability differences explicitly | Accepted |
| WIN-010 | Ship BoringSSL statically with `OPENSSL_NO_ASM=1` and the dynamic CRT; treat assembly, FIPS, signing, and dynamic linkage as later gated work | Accepted; non-FIPS footprint measured on x64/ARM64 (see crypto release engineering); enterprise/FIPS requires a separately validated backend |

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
| BoringSSL/Mbed TLS/JsonCpp dependency smoke | Supported | Supported | Supported | Not yet run on native hardware |
| Shared core error/key-ID library and tests | Supported subset | 23 tests pass | Supported subset | Not yet run on native hardware |
| QPC/FILETIME/SRW System primitives | Supported | Supported | Supported | Not yet run on native hardware |
| Shared System clock/mutex APIs | Supported subset | Supported subset | Supported subset | Not yet run on native hardware |
| Typed WinSock/IPv6 UDP/`WSAPoll` primitives | Supported | Supported | Supported | Not yet run on native hardware |
| Inet interface/address enumeration (`GetAdaptersAddresses`) | Supported | Supported | Supported | Not yet run on native hardware |
| Shared Inet UDP socket endpoint (WinSock) | Supported | Supported | Supported | Not yet run on native hardware |
| Shared Inet TCP socket endpoint (WinSock) | Supported | Supported | Supported | Not yet run on native hardware |
| BoringSSL CryptoPAL closure and expanded correctness suite | Supported | 23 tests pass | Supported | Not yet run on native hardware |
| Upstream `src/crypto/tests` GoogleTest suites | Supported | 80 tests pass | Supported | Not yet run on native hardware |
| Upstream System and Inet suites | Supported | 93 tests pass | Supported | Not yet run on native hardware |
| Upstream transport and Secure Channel suites | Supported | 40 tests pass | Supported | Not yet run on native hardware |
| Canonical System/Inet/CryptoPAL and host-neutral transport compile probe | Supported | Compile only | Supported | Compile only |
| Windows Device Layer `PlatformManager` foundation | Supported | Smoke passes | Supported | Not yet run on native hardware |
| Windows Device Layer `KeyValueStoreManager` | Supported | Smoke passes (72 checks) | Supported | Not yet run on native hardware |
| Windows typed configuration storage | Supported | Smoke passes (49 checks) | Supported | Not yet run on native hardware |
| Windows Device Layer `ConnectivityManager` | Supported for OS-managed adapters with native change events | Smoke passes (21 checks plus event-loop delivery coverage) | Supported | Not yet run on native hardware |
| Windows Device Layer configuration and diagnostics | Supported | Smoke passes (45 checks) | Supported | Not yet run on native hardware |
| Windows Device Layer DNS-SD backend (`windns.h`) | Supported (native OS mDNS responder) | Smoke passes (65 checks), incl. live publish, serialized remove/republish, and browse/cancel | Supported | Not yet run on native hardware |
| Controller-facing canonical `chip::Dnssd::Resolver`/`DiscoveryImplPlatform` | Supported | Acceptance tool passes: init/shutdown, discovery start/stop | Supported | Not yet run on native hardware |
| Canonical `//src/platform:platform` Device Layer dispatch | Supported | Lifecycle, event-loop, and initialized-KVS smoke passes (`msvc-canonical-platform-smoke`) | Supported | Not yet run on native hardware |
| Canonical `//src/credentials:credentials` | Supported | Compile only | Supported | Compile only |
| Canonical `//src/lib/dnssd:dnssd` (real `Discovery_ImplPlatform.cpp`) | Supported | Links and runs in `msvc-windows-controller-discovery.exe` | Supported | Cross-build only |
| Canonical transport, messaging, PASE/CASE, and Interaction Model closure | Supported | Link/lifecycle smoke passes (`msvc-canonical-controller-stack-smoke`) | Supported | Not yet run on native hardware |
| Canonical `//src/controller` library | Supported | Persistent controller factory and `FabricTable` initialization pass | Supported | Not yet run on native hardware |
| Core Matter SDK | Not yet supported | Not yet supported | Not yet supported | Not yet supported |
| Focused non-interactive controller | Supported subset | Fabric/key create/restore; real commissioning through IPv4 PASE, trusted-root/NOC installation, CASE IPv6-to-IPv4 fallback, Sigma3, and `CommissioningComplete`; restart-safe OnOff read/invoke all pass against a real bulb. Subscription and remote-fabric-removal commands build and await hardware validation | Supported subset | Cross-build only |
| Server application | Not yet supported | Not yet supported | Not yet supported | Not yet supported |
| DNS-SD | Supported (native `windns.h` backend) | Smoke passes (65 checks) | Supported | Not yet run on native hardware |
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

-   Canonical System, Inet, CryptoPAL, credentials, DNS-SD, and host-neutral
    transport/Secure Channel components compile on Windows. The Phase 3
    canonical Device Layer composes PlatformManager, configuration, KVS,
    diagnostics, OS-managed connectivity, endpoint lifecycle, and DNS-SD for
    x64/ARM64, with its lifecycle/storage smoke passing on x64. The canonical
    controller library and a focused persistent on-network commissioner now
    compile on x64/ARM64; BLE, full command compatibility, and the server target
    remain incomplete. The full upstream
    `src/crypto/tests` CryptoPAL suites (80 tests) build and pass on x64 against
    the canonical crypto library and a focused CHIPCert subset; 93 selected
    upstream System/Inet tests and 40 host-neutral transport/Secure Channel
    tests also pass. The monolithic `//src/credentials:credentials` library
    (`FabricTable`, `LastKnownGoodTime`, `GroupDataProvider`,
    `PersistentStorageOpCertStore`) now runs through persistent controller
    fabric creation and process-restart restoration. A real accessory has
    completed IPv4 PASE and accepted its trusted root and NOC, but the first
    post-NOC CASE attempt selected an unresponsive link-local IPv6 address.
    Windows now retains alternate DNS-SD results, and a hardware rerun proved
    that CASE falls back to IPv4 and receives Sigma2. Sigma3 then exposed an
    operational-key persistence defect in the focused application: its
    injected controller key had not survived restart. Controller keys are now
    generated and committed through `PersistentStorageOperationalKeystore`,
    and legacy keyless local fabrics are replaced automatically. A subsequent
    hardware run completed Sigma3, activated CASE, and received
    `CommissioningComplete` with device error code 0. Restart-safe OnOff
    read/invoke commands also pass against the real bulb, including visible Off
    and On behavior. The bulb's non-responsive advertised link-local IPv6
    address adds a CASE retry delay before IPv4 succeeds. Subscription and
    remote fabric removal commands now build but still await LAN hardware
    validation.
-   The native DNS-SD backend does not publish Matter subtype PTR records
    (e.g. `_S15._sub._matterc._udp`): the Win32 `DNS_SERVICE_INSTANCE`/
    `DnsServiceConstructInstance()` surface it registers through has no
    subtype field, and there is no separate `windns.h` call to add one. A
    published service is discoverable by its base type only.
-   A TXT value containing an embedded NUL byte cannot be published: the
    underlying `DNS_SERVICE_INSTANCE` TXT values are NUL-terminated wide
    strings. `ChipDnssdPublishService()` rejects this case explicitly
    (`CHIP_ERROR_INVALID_ARGUMENT`) instead of silently truncating it.
-   `ChipDnssdReconfirmRecord()` always returns `CHIP_ERROR_NOT_IMPLEMENTED`
    (after validating its hostname argument): `windns.h` has no equivalent to
    the mDNSResponder "reconfirm record" hook this API models.
-   Same-host resolve reliability was not established on the development host
    used for this milestone: publishing and browsing a uniquely named (GUID)
    service both completed live and quickly, but resolving that same service
    from the same host did not complete within a generous 10-second wait, even
    after cancellation. This is consistent with the common mDNS behavior of
    suppressing a host's own multicast announcements on receive (loopback
    suppression) and is not necessarily representative of resolving a
    *different* host's service. The DNS-SD smoke test therefore does not
    assert on resolve completion; see "Firewall and responder notes" above.
-   No Windows Firewall rule automation is provided for the DNS-SD responder;
    see "Firewall and responder notes" above for what was and was not required
    on the development host used for this milestone.
-   ARM64 output has been inspected but not executed on native Windows ARM64
    hardware. This applies to the DNS-SD backend as much as every other
    Windows Device Layer component so far: it cross-builds and links as
    `ARM64`, but has not been run on native ARM64 hardware.
-   No Windows CI runner, BLE backend, or persistence-provider ACL hardening
    is present yet.
-   Native Wi-Fi provisioning, a local Thread stack, and a Windows Thread
    border router are outside the first-release scope.
