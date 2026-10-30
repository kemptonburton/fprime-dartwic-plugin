#pragma once
#include <dartwic/EngineProtocolPeer.h>

namespace DARTWIC {
/** Compatibility name for the Engine Protocol Peer configuration. */
using PeerClientConfig = EngineProtocolPeerConfig;
/** Compatibility name for EngineProtocolPeer. Existing source and CMake usage remain supported. */
using PeerClient = EngineProtocolPeer;
} // namespace DARTWIC
