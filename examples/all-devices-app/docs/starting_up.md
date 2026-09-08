# Application Initialization & Command Line Options

This guide describes configuring and running the `all-devices-app` executable.

The application builds its data model at runtime based on command-line
arguments.

---

## Prerequisite: Building from Source

Before executing any commands, activate your build environment and compile the
application from scratch:

```bash
source scripts/activate.sh
./scripts/build/build_examples.py --target linux-x64-all-devices-clang build
```

The compiled executable will be located at
`./out/linux-x64-all-devices-clang/all-devices-app`.

### Native Windows

Initialize the native build environment and generate the opt-in Windows target:

```powershell
. .\scripts\setup\windows.ps1 -Architecture x64
gn gen out\win-all-devices-x64 --args='target_os="win" target_cpu="x64" chip_device_platform="windows" chip_windows_canonical_compile_probes=true chip_windows_device_layer_probe=true chip_windows_build_all_devices_app=true chip_windows_enable_cxx20=true chip_with_nlfaultinjection=false chip_build_tests=false chip_build_tools=true chip_caller_handles_critical_failure=true'
ninja -C out\win-all-devices-x64 all-devices-app
```

Run a dynamic two-endpoint topology:

```powershell
.\out\win-all-devices-x64\all-devices-app.exe `
    --device on-off-light:1 `
    --device occupancy-sensor:2 `
    --storage-directory .\all-devices-state `
    --run-seconds 300
```

The Windows entrypoint supports repeated `--device`, `--storage-directory`
(`--KVS` is an alias), `--discriminator`, `--run-seconds`, and `--app-pipe`
arguments.
`--interface-id -1` is accepted for compatibility with the application test
harness; selecting one interface is not supported by the native DNS-SD
backend. A run duration of `0`, which is the default, runs until Ctrl+C,
Ctrl+Break, or console close. The application emits the standard setup QR code
and `APP STATUS: Starting event loop` readiness marker.

The entrypoint uses the native Windows persistence, WinSock, DNS-SD, and BLE
backends. `--app-pipe` maps the portable test identifier to a local Windows
named pipe and accepts the same newline-delimited JSON commands as the POSIX
simulator. Tracing and audio overrides are not exposed.

---

## Startup Architecture

When the application boots, it executes the following sequence:

1. **Option Parsing**: Reads CLI flags via `AppOptions::GetOptions()` to
   determine the required storage layer, network ports, and runtime endpoint
   composition.
2. **Platform App Main Loop**: Initializes the Matter POSIX/Platform server
   stack (`AppMain()`).
3. **Endpoint Instantiation**: For each `--device`, the runtime instantiates the
   device using `DeviceFactory` and registers it with the Interaction Model.

---

## Core Runtime Composition

Unlike static Matter targets, `all-devices-app` allows you to dynamically
assemble your exact endpoint structure at boot using the `--device` option.

### `--device <type>[:<endpoint>][,parent=<parentId>]`

Instantiates a specific simulated device feature on the Matter server. This flag
can be repeated multiple times to build composite or multi-endpoint topologies.

-   `<type>`: The registered runtime key (e.g., `contact-sensor`,
    `dimmable-light`, `occupancy-sensor`).
-   `:<endpoint>` _(Optional)_: Assigns an explicit endpoint index. If omitted,
    the runtime automatically assigns an incrementing ID starting from 1.
-   `,parent=<parentId>` _(Optional)_: Attaches this endpoint as a child of
    another endpoint, establishing a logical tree composition (such as attaching
    a Speaker to a parent Chime or building an Aggregator bridge).

> [!TIP] This application inherits the Matter SDK's complete baseline option
> parser. For the complete, live list of available network commissioning
> (`--wifi`, `--ble-controller`), operational binding (`--port`,
> `--interface-id`), descriptor (`--discriminator`, `--vendor-id`,
> `--product-id`), device attestation credentials (`--dac_provider`), and
> persistent storage (`--KVS`) arguments, execute the binary with `--help`:
>
> ```bash
> ./out/linux-x64-all-devices-clang/all-devices-app --help
> ```

---

## Practical Operating Recipes

### Recipe 1: Booting a Clean Composite Tree Device

When launching new multi-endpoint devices, always wipe your previous persistent
storage to ensure the server doesn't reload stale data model definitions:

```bash
# Safely wipe the default storage file
rm -rf /tmp/chip_all_devices_kvs

# Boot an explicit Chime on Endpoint 1 and attach a Speaker on Endpoint 2 as its child
./out/linux-x64-all-devices-clang/all-devices-app --device chime:1 --device speaker:2,parent=1
```

### Recipe 2: Running Multiple Simulators Concurrently

To launch multiple independent simulator processes side-by-side on the same host
interface without socket or commissioning collisions, provide distinct UDP
ports, storage paths, and Setup Discriminators:

```bash
# Boot Instance Alpha (Default settings)
./out/linux-x64-all-devices-clang/all-devices-app \
    --device occupancy-sensor:1 \
    --KVS /tmp/chip_kvs_alpha \
    --port 5540 \
    --discriminator 3840

# Boot Instance Beta (Isolated settings)
./out/linux-x64-all-devices-clang/all-devices-app \
    --device contact-sensor:1 \
    --KVS /tmp/chip_kvs_beta \
    --port 5541 \
    --discriminator 3841
```
