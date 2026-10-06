#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <enet/enet.h>

// Small helpers around ENet shared by the server, the game client and tools.
namespace Net
{
	// Reference-counted enet_initialize/enet_deinitialize, so the plugin can host a server
	// and run a client in the same process.
	bool Initialize();
	void Shutdown();

	// Sends a packet on the reliable channel or the unreliable state channel.
	void Send(ENetPeer* a_peer, std::span<const std::uint8_t> a_data, bool a_reliable);

	// Resolves "host" or "host:port". Returns false if the host can't be resolved.
	bool ResolveAddress(const std::string& a_hostAndPort, std::uint16_t a_defaultPort, ENetAddress& a_out);
}
