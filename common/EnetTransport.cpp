#include "EnetTransport.h"

#include "Net.h"
#include "Protocol.h"

#include <format>

namespace
{
	// Bigger packets than any message we send are an attack or a bug.
	constexpr std::size_t MAX_PACKET_SIZE = 1024 * 1024;
}

std::unique_ptr<EnetServerTransport> EnetServerTransport::Create(std::uint16_t a_port, std::size_t a_maxPeers, bool a_loopbackOnly, std::string& a_error)
{
	if (a_maxPeers == 0 || a_maxPeers > ENET_PROTOCOL_MAXIMUM_PEER_ID) {
		a_error = std::format("can't host {} connections", a_maxPeers);
		return nullptr;
	}
	if (!Net::Initialize()) {
		a_error = "failed to initialize networking";
		return nullptr;
	}
	ENetAddress address{};
	if (a_loopbackOnly) {
		enet_address_set_host_ip(&address, "127.0.0.1");
	} else {
		address.host = ENET_HOST_ANY;
	}
	address.port = a_port;
	const auto host = enet_host_create(&address, a_maxPeers, Protocol::CHANNEL_COUNT, 0, 0);
	if (!host) {
		Net::Shutdown();
		a_error = std::format("could not bind UDP port {} (already in use?)", a_port);
		return nullptr;
	}
	host->maximumPacketSize = MAX_PACKET_SIZE;
	return std::unique_ptr<EnetServerTransport>(new EnetServerTransport(host));
}

EnetServerTransport::EnetServerTransport(ENetHost* a_host) :
	host(a_host)
{}

EnetServerTransport::~EnetServerTransport()
{
	Close();
	enet_host_destroy(host);
	Net::Shutdown();
}

void EnetServerTransport::Poll(std::vector<TransportEvent>& a_out, int a_waitMs)
{
	ENetEvent event;
	int       wait = a_waitMs;
	while (enet_host_service(host, &event, static_cast<enet_uint32>(wait)) > 0) {
		wait = 0;
		switch (event.type) {
		case ENET_EVENT_TYPE_CONNECT:
			{
				const auto id = nextId++;
				peers[id] = event.peer;
				ids[event.peer] = id;
				enet_peer_timeout(event.peer, 0, 10000, 20000);
				a_out.push_back({ TransportEvent::Type::kConnect, id, {} });
			}
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			if (const auto it = ids.find(event.peer); it != ids.end()) {
				a_out.push_back({ TransportEvent::Type::kPacket, it->second, { event.packet->data, event.packet->data + event.packet->dataLength } });
			}
			enet_packet_destroy(event.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			if (const auto it = ids.find(event.peer); it != ids.end()) {
				a_out.push_back({ TransportEvent::Type::kDisconnect, it->second, {} });
				peers.erase(it->second);
				ids.erase(it);
			}
			break;
		default:
			break;
		}
	}
}

void EnetServerTransport::Send(PeerId a_peer, std::span<const std::uint8_t> a_data, bool a_reliable)
{
	if (const auto it = peers.find(a_peer); it != peers.end()) {
		Net::Send(it->second, a_data, a_reliable);
	}
}

void EnetServerTransport::Disconnect(PeerId a_peer)
{
	if (const auto it = peers.find(a_peer); it != peers.end()) {
		enet_peer_disconnect_later(it->second, 0);
	}
}

void EnetServerTransport::Close()
{
	for (const auto& [id, peer] : peers) {
		enet_peer_disconnect_now(peer, 0);
	}
	peers.clear();
	ids.clear();
	enet_host_flush(host);
}

bool EnetServerTransport::IsLocal(PeerId a_peer) const
{
	const auto it = peers.find(a_peer);
	if (it == peers.end()) {
		return false;
	}
	char ip[64]{};
	enet_address_get_host_ip(&it->second->address, ip, sizeof(ip));
	return std::string_view{ ip }.starts_with("127.") || std::string_view{ ip } == "::1";
}

std::string EnetServerTransport::Describe(PeerId a_peer) const
{
	const auto it = peers.find(a_peer);
	if (it == peers.end()) {
		return "udp ?";
	}
	char ip[64]{};
	enet_address_get_host_ip(&it->second->address, ip, sizeof(ip));
	return std::format("udp {}:{}", ip, it->second->address.port);
}
