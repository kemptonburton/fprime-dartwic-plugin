# F´ CCSDS bridge for DARTWIC

This is the source repository for the DARTWIC `fprime_bridge` engine plugin. The
downloadable Windows x64 packages are on the [GitHub releases page](https://github.com/kemptonburton/fprime-dartwic-plugin/releases).
The standard package is `plugin.zip`; the debug package is `plugin-debug.zip`
and includes the plugin's PDB symbols. Install the package matching the engine
build you run. Keep the manifest ID `fprime_bridge` when upgrading an existing
connection.

`fprime_bridge.fprime_ccsds` is the **F Prime ComCcsds** TEMPEST peer for the DARTWIC engine. It connects a running F´ deployment to DARTWIC over TCP using F´ CCSDS telemetry (TM) and telecommand (TC) frames. The F´ deployment does not link against TEMPEST or DARTWIC code. The bridge runs in the DARTWIC engine and implements `TEMPEST::Transport`.

The bridge is driven by the **topology dictionary for the deployment currently connected to it**. Its decoder, command encoder, and command catalog use that dictionary at runtime; Hadron command names and IDs are not compiled into those paths. A deployment must provide the compatible CCSDS link described below. A dictionary alone cannot make a deployment with a different transport or frame format interoperable.

## Connection and settings

The bridge listens on `bind_host:port`; the deployment's TCP client connects to that address. The deployment sends TM frames to the bridge and receives TC frames from it. The `host` field in a saved TEMPEST peer connection is an engine connection setting; **`bind_host` is the address on which this bridge actually listens**. To accept a deployment on another machine, bind to an address reachable from that machine and arrange network access to the selected port.

Configure these fields in the TEMPEST peer connection:

| Field | Purpose | Bridge default |
| --- | --- | --- |
| `node_name` | DARTWIC name for the remote deployment | `HADRON_SITL` |
| `bind_host` | Local IPv4 listener address | `127.0.0.1` |
| `port` | Local TCP listener port | `50101` |
| `dictionary` | Path to this deployment's generated topology JSON dictionary | `SITLDeploymentTopologyDictionary.json` in the plugin's default configuration |
| `spacecraft_id` | CCSDS transfer frame spacecraft ID | `68` |
| `virtual_channel_id` | CCSDS transfer frame virtual channel ID | `1` |
| `tm_frame_size` | Fixed TM transfer frame size in bytes | `1024` |
| `downlink_idle_timeout_ms` | Reconnect after this long without a valid TM frame; `0` disables the timeout | `10000` |

The dictionary path is read by the **DARTWIC engine process**, on the machine running that engine. An absolute path is the safest setting. A relative path is resolved against the engine process's **current working directory**, because the bridge opens the configured path directly. It is not relative to the plugin directory, workspace directory, F´ checkout, or remote deployment. A Windows engine needs a Windows accessible path; a dictionary that exists only inside WSL or on another computer must be copied or exposed to the engine host. The bridge loads the dictionary when the peer starts. Regenerate it for a changed deployment topology and restart the peer to load the new version.

For example, a Windows engine configured for Hadron could use an absolute path like `C:/path/to/Hadron-FSW/build-local/fprime-ccsds-dictionary/SITLDeploymentTopologyDictionary.json` as the `dictionary` value. That path is an example, not a built-in requirement. For another F´ deployment, point `dictionary` at that deployment's generated topology dictionary and set the CCSDS link fields to match it. The dictionary should come from the same FPP/topology revision as the running flight binary; mismatched IDs or types can cause decode failures or incorrect command encoding.

## What goes over the network

The bridge reads fixed-size CCSDS TM transfer frames, validates their frame CRC and configured spacecraft/virtual channel IDs, then decodes telemetry packets (APID 1) and event packets (APID 2) using the dictionary. Numeric telemetry channels are published to DARTWIC through `DARTWIC::EngineProtocol`. Decoded F´ events are published as structured ARGUS events and to the `F Prime Events` ARGUS log stream. F´ `FATAL` events appear as errors, `WARNING_HI` and `WARNING_LO` as warnings, and other severities as info in that log stream. The original F´ severity is retained in the structured event details.

`tempest/telemetry/list` is a list of registered TEMPEST **topic descriptors**, not a list of F´ telemetry channels. This peer does not register topic descriptors, so that request succeeds with `telemetry: []` and an explanatory `reason`. F´ channel values still arrive through the CCSDS downlink and appear as remote DARTWIC channels.

Only F´ events actually sent over the CCSDS downlink can appear in `F Prime Events`. Arbitrary process stdout, stderr, and host terminal output are outside this protocol. The bridge does not read console files, inspect the F´ host, or require a shared filesystem. To see additional process logs remotely, the deployment must send them through a network protocol that the engine can receive; this bridge currently consumes CCSDS event packets for ARGUS logs.

The socket reader drains downlink into a bounded 256-frame queue. If decoding falls behind, it drops the oldest queued TM frame to keep the socket reader responsive. This queue does not drop outbound commands. The idle timeout detects a connection that stops sending valid TM frames. Peer traffic rates count TCP payload bytes received and sent by the bridge.

## Command catalog and sending commands

The bridge **does not ask F´ for a command list over TCP**. At peer startup it loads the local generated dictionary, including command names, opcodes, formal arguments, types, annotations, and enum choices. When the engine requests `tempest/operations/list`, the bridge builds the catalog from that loaded dictionary and returns each command under its fully qualified F´ name. The catalog is therefore available from the peer's loaded dictionary, not from a live discovery endpoint in the deployment. The connected deployment must match the dictionary.

Selecting a catalog command sends a TEMPEST operation to the bridge. The bridge validates the supplied arguments against the dictionary, serializes the opcode and arguments into a CCSDS TC frame, and writes that frame on the TCP connection to F´. Commands with no formal arguments use an empty arguments object `{}` automatically. The generic `fprime/command` operation also accepts a qualified command `name` and an `arguments` object. The legacy `demo/set-running` operation is available only when the dictionary contains a `.SET_RUNNING` command; ordinary F´ commands should be sent by their catalog names.

A successful response with `status: "sent"` means the TC frame was written to the TCP connection. It does **not** confirm that F´ accepted or executed the command; `execution_confirmed` remains `false`. This CCSDS link does not return a command execution acknowledgement to the bridge. Check subsequent F´ event/telemetry output for deployment-specific evidence of execution.

## Build and package from a clone

Windows x64 builds use Visual Studio 2022 with ClangCL, CMake, Node.js, and
vcpkg. Set `VCPKG_ROOT` to a vcpkg checkout, or initialize a local `vcpkg`
directory. The bundled `engine/include/sdk` headers and `vendor/` peer libraries
are versioned snapshots; `npm run verify` checks their hashes.

The native build is standalone. `TEMPEST::Messaging`, `TEMPEST::Peer`, and
`DARTWIC::EngineProtocol` sources are included under `vendor/`; no private
DARTWIC checkout is needed. The packaged plugin communicates with F´ over TCP
and requires no F´ source or shared filesystem at runtime.

If vcpkg dependencies are already installed in another DARTWIC build, set
`DARTWIC_VCPKG_INSTALLED_DIR` to that build's `vcpkg_installed` directory to
reuse them. Otherwise the package scripts install the manifest dependencies.

From the repository root in PowerShell:

```powershell
npm run verify
npm run package
npm run package-debug
```

The build outputs are `plugin/engine/fprime_bridge` for Release and
`plugin/engine-debug/fprime_bridge` for Debug. Each package script creates its
matching ZIP at the repository root. The package scripts configure CMake and
build the DLL, so manually building first is unnecessary. The debug ZIP includes
`fprime_bridge.pdb`.

For the native Windows bridge test, supply a generated topology dictionary:

```powershell
$env:FPRIME_BRIDGE_TEST_DICTIONARY = 'C:/path/to/DeploymentTopologyDictionary.json'
cmake --preset windows-clang-debug -DFPRIME_BRIDGE_TEST_DICTIONARY=$env:FPRIME_BRIDGE_TEST_DICTIONARY
cmake --build --preset build-windows-clang-debug --target fprime_bridge_windows_test
ctest --test-dir build/windows-clang-debug -C Debug --output-on-failure
```

The Hadron-specific helper `test/GenerateHadronDictionary.ps1` can generate the
dictionary from a neighboring `Hadron-FSW` checkout. The Windows test checks
frame parsing, network event publishing, command catalog generation, command
encoding, TCP reconnect, and TEMPEST peer registration.

### Engine compatibility

This version binds its TEMPEST engine peer to protocol `tempest.engine` version
1. The DARTWIC engine must accept that protocol binding. An older installed
engine that still validates `dartwic.engine` rejects this plugin at load with
`Peer definition has an invalid protocol binding.` Update or rebuild the engine
from a compatible revision before installing this release. The `minEngineVersion`
value in the manifest alone cannot detect that protocol change.

`test/windows_ccsds_fixture.py` is a wire-level test fixture. It is **not** the Hadron F´ SITL deployment. An end-to-end test of the actual deployment requires running the F´ SITL on a supported platform such as Linux or WSL and connecting its CCSDS TCP client to this peer. The bridge itself builds on Windows; the Hadron F´ checkout does not currently supply a native Windows platform/`Drv.TcpClient` implementation.
