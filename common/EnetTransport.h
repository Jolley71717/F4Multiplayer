#pragma once

#include "Transport.h"

#include <memory>
#include <unordered_map>

struct _ENetHost;
struct _ENetPeer;

// The server side of ENet: players connect to a UDP port.
class EnetServerTransport final : public ServerTransport
{
public:
	// Binds the port (on 127.0.0.1 only if a_loopbackOnly). Returns nullptr (with a reason in a_error) if it can't.
	static std::unique_ptr<EnetServerTransport> Create(std::uint16_t a_port, std::size_t a_maxPeers, bool a_loopbackOnly, std::string& a_error);

	~EnetServerTransport() override;

	void Poll(std::vector<TransportEvent>& a_out, int a_waitMs) override;
	void Send(PeerId a_peer, std::span<const std::uint8_t> a_data, bool a_reliable) override;
	void Disconnect(PeerId a_peer) override;
	void Close() override;
	[[nodiscard]] std::string Describe(PeerId a_peer) const override;
	[[nodiscard]] bool        IsLocal(PeerId a_peer) const override;

private:
	explicit EnetServerTransport(_ENetHost* a_host);

	_ENetHost*                                   host;
	PeerId                                       nextId = 1;
	std::unordered_map<PeerId, _ENetPeer*>       peers;
	std::unordered_map<_ENetPeer*, PeerId>       ids;
};
