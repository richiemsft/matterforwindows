# Native Windows camera controller preview

This is a native C++ application inside connectedhomeip. It uses the same
commissioning, storage, generated cluster commands, Interaction Model, and
authenticated CASE/TCP signaling as the other host controllers. WebView2
provides the Windows WebRTC, H.264 decoding, and video presentation backend. It
does not reference or build the separate `dotMorten/matter.winrt` SDK.

The Linux camera-controller/GStreamer application is unchanged. Windows live
view is a standalone command with an owned window and bounded cleanup, rather
than the Linux interactive singleton/process manager.

## Build

Use the [native Windows prerequisites](../../../docs/guides/windows.md), plus
the .NET 8 SDK or newer **only to restore the pinned native WebView2 package**.
The resulting executable is native and has no .NET runtime dependency. Install
the Microsoft Edge WebView2 Evergreen Runtime on the machine running the player.

From the repository root:

```powershell
.\scripts\tools\windows_camera_controller.ps1 -Architecture x64 -RunTests -RunMediaSelfTest
.\scripts\tools\windows_camera_controller.ps1 -Architecture arm64
```

Default outputs are `out\windows-camera-controller-x64` and
`out\windows-camera-controller-arm64`. ARM64 binaries must be executed on ARM64
hardware. The script restores Microsoft.Web.WebView2 **1.0.3719.77**, links its
architecture-matched static loader, copies the trusted receiver assets and
WebView2 license, and builds the player and test executables.
`-RunMediaSelfTest` runs the integrated Matter/video tests as well as the
standalone media-backend check; both open native WebView2 windows.

The feature is opt-in: `chip_windows_build_camera_controller=true`,
`chip_build_webrtc_signaling=true`, and `chip_windows_webview2_root` pointing to
that restored package. Ordinary chip-tool/default Windows builds do not need
WebView2, libdatachannel, GStreamer, or the separate WinRT SDK.

Keep `camera-receiver\` and `WebView2-LICENSE.txt` beside
`chip-camera-controller.exe` when moving the application.

## Commission and view

Use a reachable Matter camera supporting the SDK's Camera AV Stream Management
and WebRTC Transport Provider clusters, LiveView usage, H.264, and
controller-generated offers. Choose a private, persistent controller directory:

```powershell
New-Item -ItemType Directory -Force .\camera-controller-state | Out-Null
.\out\windows-camera-controller-x64\chip-camera-controller.exe pairing code 1 <setup-code> `
    --storage-directory .\camera-controller-state

.\out\windows-camera-controller-x64\chip-camera-controller.exe liveview start 1 `
    --storage-directory .\camera-controller-state --duration 300
```

The default request is H.264, exactly 640 x 480, exactly 30 fps, 500000-2000000
bits/second, and a 1000 ms key-frame interval. To select another advertised
resolution, set both bounds explicitly:

```powershell
.\out\windows-camera-controller-x64\chip-camera-controller.exe liveview start 1 `
    --storage-directory .\camera-controller-state --endpoint-id 1 `
    --min-res-width 1280 --max-res-width 1280 `
    --min-res-height 720 --max-res-height 720 `
    --min-framerate 30 --max-framerate 30
```

The application discovers camera endpoints through Descriptor, refuses ambiguous
selection unless `--endpoint-id` is supplied, checks advertised commands, video
capabilities, LiveView usage, and present livestream/hardware privacy
attributes, and refuses incompatible bounds instead of silently clamping them.
After allocation it continues checking advertised software livestream and
hardware privacy attributes, with one second between read cycles. An enabled
privacy state or a failed privacy read stops the receiver and starts owned
remote cleanup. This is bounded polling, not an instantaneous privacy
subscription; each read retains its 20-second network timeout, and the camera
must enforce its own privacy state. It allocates an owned stream, validates
the allocated stream's metadata, and exchanges SDP/ICE using its routable
requestor endpoint, shared Binding cluster, and authenticated large-payload
CASE/TCP session.

Video-only offers omit AudioStreamID; present protocol null would request
automatic audio selection instead. Newly allocated streams with transport
ReferenceCount zero are valid. Transport use does not establish caller ownership:
only the exact ID returned by this application's successful allocation is
eligible for its cleanup. Unexpected non-video tracks terminate playback.

It reports playback only after a presented, decoded frame and matching H.264
receiver statistics. An accepted SDP answer or an ICE connection alone is not
playback. Audio, recording, alternative codecs, and external STUN/TURN
configuration are not implemented by this preview; use a directly reachable
camera on the local network. Camera codec/parameter support is also checked by
the allocation command and returned metadata.

Allow application-specific Matter UDP/TCP and WebRTC traffic through the local
firewall. `--listen-port` defaults to the configured controller port or 5540; it
must be available and reachable by the camera. No fixed RTP loopback ports or
external media-player processes are used.

Close the video window or press **Ctrl+C** to end playback. Startup has a
90-second budget; playback defaults to 300 seconds and is configurable up to
3600 seconds. Decoded-frame progress stalling for ten seconds is an error.
Cleanup allows up to 120 seconds to complete pending allocation/signaling,
attempt authenticated remote End, and deallocate only the application's owned
stream. A lost CASE connection gets one bounded reconnection attempt.
Native UI shutdown has a separate ten-second limit. Errors remain errors,
including remote cleanup failures. Commissioning and existing bindings are
preserved.

Forced process termination or an unavailable camera cannot guarantee remote
cleanup. If an allocation response is lost, its outcome is explicitly reported
as uncertain; the application will not guess a stream ID and risk deallocating
another client's stream.

The receiver only loads trusted files from its own asset directory on an exact
virtual HTTPS origin. Navigation, popups, permissions, host objects, and
external network fetches are restricted. The bridge exposes media messages, not
controller handles, keys, certificates, or storage. SDP and ICE contents are not
logged. Browser media state lives in
`%LOCALAPPDATA%\MatterNativeCamera\WebView2`, separate from Matter controller
state.

## Validation and remaining acceptance

`camera-liveview-tests.exe` covers selection, advertised privacy/features,
bounded endpoint discovery and ICE, decoded-frame/codec gating, and retention of
late allocation responses during cancellation. `msvc-webrtc-signaling-tests.exe`
exercises the shared signaling lifecycle, actual CASE handshakes, IPv4/IPv6 TCP,
large encrypted SDP, rejection, and disconnect cleanup.

`camera-liveview-flow-tests.exe` runs the actual native live-view command
against a local synthetic peer using operational test certificates and a real
CASE handshake over encrypted IPv6/TCP. Descriptor/capability reads, stream
allocation and returned metadata, SDP and bidirectional ICE, actual H.264
presentation, remote End, and owned deallocation run together. It asserts
resource ownership, cleanup ordering, and no remaining active exchanges.
Negative cases cover privacy rejection before allocation, incompatible returned
metadata, a rejected media answer, and a remote End failure that must remain an
error even after successful deallocation. The test-only provider uses a canvas
source. Additional cases cover a bridge at endpoint 1 with a camera at endpoint
2, ambiguous multiple-camera selection, and both software and hardware privacy
changes after actual playback. The provider is not a conformant camera or an
independent implementation, and the fixture injects the established CASE session
rather than commissioning a device.

For a repeated software lifecycle check:

```powershell
.\out\windows-camera-controller-x64\camera-liveview-flow-tests.exe `
    --gtest_filter=LiveViewFlowTests.CaseDiscoveryAllocationActualVideoAndOwnedCleanup `
    --gtest_repeat=100
```

The x64 development run completed 100 passing cycles, each including a real CASE
handshake, actual H.264 presentation, End/deallocation, no remaining active
exchanges or owned allocation, and bounded native receiver shutdown. This is
synthetic-peer endurance evidence, not 100 physical-device sessions or a
measured process-memory/performance baseline.

`chip-camera-controller.exe --media-self-test` opens the native receiver and
uses a local canvas sender and two real WebRTC peers to encode, decode, and
present H.264 video. It fails unless a decoded frame with the expected codec is
presented within 30 seconds. This checks the Windows media backend, **not** the
Matter allocation-to-video flow against a camera.

x64 unit/signaling, integrated synthetic-camera video, media-backend checks, and
ARM64 compilation are development evidence only. Physical-camera SDP/ICE and
codec interoperability, complete allocation/End/deallocation behavior against a
real provider, privacy changes during streaming, 100 real camera sessions,
latency/resource baselines, ARM64 execution, and current normative revision
compatibility still require device testing. This is not a certified or
production-ready Camera Controller.
