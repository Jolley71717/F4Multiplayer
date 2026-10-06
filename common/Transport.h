#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// How the server talks to players. Each transport (ENet over UDP, Steam networking) hands out
// its own peer IDs; the server tells them apart by transport.
using PeerId = std::uint32_t;

struct TransportEvent
{
	enum class Type
	{
		kConnect,
		kDisconnect,
		kPacket,
	};

	Type                      type;
	PeerId                    peer = 0;
	std::vector<std::uint8_t> data;  // kPacket only
};

class ServerTransport
{
public:
	virtual ~ServerTransport() = default;

	// Appends everything that happened since the last call. May wait up to a_waitMs for the first event.
	virtual void Poll(std::vector<TransportEvent>& a_out, int a_waitMs) = 0;

	virtual void Send(PeerId a_peer, std::span<const std::uint8_t> a_data, bool a_reliable) = 0;

	// Closes the connection after queued packets are delivered; a kDisconnect event follows.
	virtual void Disconnect(PeerId a_peer) = 0;

	// Drops every connection immediately (server shutdown).
	virtual void Close() = 0;

	// For log lines, e.g. "udp 1.2.3.4:5678" or "steam 7656...".
	[[nodiscard]] virtual std::string Describe(PeerId a_peer) const = 0;

	// The peer is on this machine (the host's own game).
	[[nodiscard]] virtual bool IsLocal(PeerId) const { return false; }
};
