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
    `//src/crypto:crypto` GN targets compile with MSVC for x64 and ARM64 through
    an opt-in graph probe. The bootstrap graph now defines the Pigweed Python
    venv label needed while loading canonical BUILD files, without building the
    venv unless a target actually depends on it.
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
    `src/protocols/secure_channel/tests` GoogleTest suites run against focused
    Windows-local subsets of `//src/transport` and
    `//src/protocols/secure_channel` (see `//src/transport/windows` and
    `//src/protocols/secure_channel/windows`) linked to the canonical
    `//src/crypto`, `//src/inet`, `//src/system`, and `//src/lib` libraries.
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
The canonical library probe remains opt-in until the upstream System, Inet,
crypto, credentials, messaging, and Interaction Model test suites are enabled.
This avoids presenting successful library compilation as a usable Matter SDK
port.

Work advances only when the preceding phase exit criteria are complete. Phases
0 and 1 are complete; Phase 2 is the active phase.

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
link focused, host-neutral subsets of `//src/transport` and
`//src/protocols/secure_channel` (see `//src/transport/windows` and
`//src/protocols/secure_channel/windows`) against the canonical `//src/crypto`,
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

### Windows-local library subsets

The canonical `//src/transport:transport` and
`//src/protocols/secure_channel:secure_channel` targets pass the GCC/Clang
`-Wconversion` flag (which cl.exe rejects as an invalid numeric argument) and
build at the strict `/W4 /WX` default. Rather than modify those upstream BUILD
files, the Windows tests link small `source_set`s under
`//src/transport/windows` and `//src/protocols/secure_channel/windows` that list
exactly the host-neutral translation units the enabled suites need and add
`//build/config/win:upstream_sdk_warnings` (which both relaxes `/W4 /WX` for the
GCC/Clang-authored bodies and omits `-Wconversion`). The upstream `.cpp` sources
are compiled verbatim. `credentials/FabricTable.h` and
`messaging/ReliableMessageProtocolConfig.h` are reached only as headers through
`//src:includes`.

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

The canonical System, Inet, and BoringSSL CryptoPAL libraries can be added to
the bootstrap graph as a compile-only probe. Tests and tools are disabled here
because their broader Windows closure is tracked separately:

```powershell
gn gen out\win-canonical-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="none" chip_windows_canonical_compile_probes=true chip_build_tests=false chip_build_tools=false'
ninja -C out\win-canonical-x64
```

Use the same arguments with `target_cpu="arm64"` after initializing the ARM64
MSVC environment. The normal bootstrap build leaves
`chip_windows_canonical_compile_probes` false.

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
| Crypto | CryptoPAL API and credential logic | BoringSSL selected, compiled with MSVC (asm disabled). The real upstream `src/crypto/tests` GoogleTest suites (80 tests including the full `TestChipCryptoPAL` CryptoPAL suite) pass on x64 at `/std:c++17` against the canonical `//src/crypto:crypto` library and a focused CHIPCert subset (upstream sources adapted to C++17 by a build-time transform), and cross-build as `AA64`. The focused 23-test BoringSSL driver is retained. The full monolithic credentials/Device-Layer closure is deferred | Dependency |
| Device Layer | Generic static-polymorphism mixins | No Windows platform composition, lifecycle, connectivity, storage, diagnostics, logging, or reset implementation | Platform contract |
| DNS-SD | Resolver and advertiser interfaces | No Windows DNS Service Discovery implementation or firewall guidance | Platform contract |
| BLE | Transport and commissioning state machines | No WinRT scanner, central connection, GATT server, advertising, or callback serialization | Platform contract |
| Controller | Portable command and controller logic | Build closure, storage paths, cancellation, terminal behavior, BLE, and DNS-SD | Platform and application |
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
| Controller | `//examples/chip-tool` | Command model, controller, JsonCpp, INI parser, BoringSSL | Core closure, Windows Device Layer, DNS-SD, storage, cancellation, and BLE | Phases 3–5 |
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
gate items are the full monolithic credentials/Device-Layer closure (deferred
to the Device Layer phase), binary-size measurement, and an explicit
enterprise/FIPS deployment statement before Windows crypto support is claimed.
Selecting the build dependency in Phase 0 does not pre-approve runtime
correctness.

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
| WIN-006 | Use Windows DNS Service Discovery without an unconditional competing UDP 5353 responder | Accepted |
| WIN-007 | Use C++/WinRT BLE and marshal completions onto the Matter event loop | Accepted |
| WIN-008 | Use versioned `%LOCALAPPDATA%` state by default with an injectable service path and explicit ACL ownership | Accepted |
| WIN-009 | Support unpackaged and packaged desktop applications; surface capability differences explicitly | Accepted |

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
| Canonical System/Inet/CryptoPAL library compile probe | Supported | Compile only | Supported | Compile only |
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

-   The canonical System, Inet, and CryptoPAL libraries compile on Windows, and
    the upstream `src/crypto/tests` GoogleTest suites run on x64, but the
    complete upstream System/Inet tests are not yet enabled.
-   The aggregate Core SDK, Device Layer, controller, and server targets do not
    yet compile as complete Windows closures. The full upstream
    `src/crypto/tests` CryptoPAL suites (80 tests) build and pass on x64 against
    the canonical crypto library and a focused CHIPCert subset, but the
    monolithic `//src/credentials:credentials` library (`FabricTable`,
    `LastKnownGoodTime`, `GroupDataProvider`, `PersistentStorageOpCertStore`) is
    not yet ported because it pulls the unported Windows Device Layer.
-   ARM64 output has been inspected but not executed on native Windows ARM64
    hardware.
-   No Windows CI runner, DNS-SD backend, BLE backend, or persistence provider
    is present yet.
-   Native Wi-Fi provisioning, a local Thread stack, and a Windows Thread
    border router are outside the first-release scope.
