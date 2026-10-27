# F Prime Bridge 2.1.0

- Use Engine-supplied Peer and EngineProtocol; remove plugin-side callback adapters and manual protocol construction.
- Adopt clarified channel, event, log and custom operation APIs; retain CCSDS dictionary encoding and measurement timestamps.
- Rebuild against Engine plugin ABI 6, TEMPEST compatibility 3 and Engine 2.1.0.
- Windows Debug/Release bridge tests and real Hadron SITL measurements, command catalog, no-op and execution event checks passed.
- Prepared locally; not published.

# F Prime Bridge 2.0.1

- Uses explicit platform ZIP filenames; generic plugin.zip releases are no longer supported.
- Windows x64 Release and Debug binaries, runtime DLLs, and PDB symbols ship together in plugin-win32-x64.zip. The Engine chooses its configuration.
- Bundled SDK locks, package manifests, and release checksums are verified. Requires DARTWIC Engine and Interface 2.0.0 or newer.
- README documents exact uploaded assets, manifest, archive structure, and build commands.
