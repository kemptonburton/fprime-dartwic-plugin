# F´ CCSDS bridge for DARTWIC 2.0.0-beta.1

First public source and binary release of the F´ CCSDS TEMPEST peer.

- `plugin.zip`: Windows x64 Release engine plugin.
- `plugin-debug.zip`: Windows x64 Debug engine plugin with PDB symbols.

The plugin reads a deployment topology dictionary from the DARTWIC engine host and communicates with F´ over a CCSDS TCP link. The dictionary path and link parameters are configured in the TEMPEST peer settings. It does not discover commands from the F´ process over TCP; the command catalog comes from the loaded dictionary.

**Engine compatibility:** this plugin binds to `tempest.engine` protocol version 1. An older DARTWIC engine that still requires `dartwic.engine` rejects it with `Peer definition has an invalid protocol binding.` Use a compatible engine build. No Hadron FSW source changes are required.

Building from this source currently requires the DARTWIC `TEMPEST::Peer` and `DARTWIC::EngineProtocol` sources, since the public example plugin SDK does not yet distribute them as standalone binaries. See README.md for build and test commands.
