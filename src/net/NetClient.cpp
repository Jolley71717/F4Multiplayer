#include "net/NetClient.h"

#include "Net.h"
#include "Protocol.h"

// wingdi.h (pulled in by ENet) defines ERROR, which collides with REX::ERROR.
#undef ERROR

NetClient::~NetClient()
{
	Disconnect();
}

bool NetClient::Connect(const std::string& a_address, std::uint16_t a_defaultPort)
{
	Disconnect();

	if (!Net::Initialize()) {
		REX::ERROR("NetClient: ENet initialization failed");
		return false;
	}

	ENetAddress address{};
	if (!Net::ResolveAddress(a_address, a_defaultPort, address)) {
		REX::ERROR("NetClient: cannot resolve '{}'", a_address);
		Net::Shutdown();
		return false;
	}

	{
		std::scoped_lock l{ queueLock };
		outbox.clear();
		inbox.clear();
	}

	stopRequested = false;
	status = Status::kConnecting;
	thread = std::thread([this, address] { Run(address.host, address.port); });
	REX::INFO("NetClient: connecting to {}", a_address);
	return true;
}

void NetClient::Disconnect()
{
	if (!thread.joinable()) {
		return;
	}
	stopRequested = true;
	thread.join();
	status = Status::kDisconnected;
	Net::Shutdown();
}

void NetClient::Send(std::vector<std::uint8_t> a_data, bool a_reliable)
{
	if (status != Status::kConnected) {
		return;
	}
	std::scoped_lock l{ queueLock };
	outbox.push_back({ std::move(a_data), a_reliable });
}

std::vector<NetClient::Event> NetClient::Poll()
{
	std::scoped_lock l{ queueLock };
	return std::exchange(inbox, {});
}

void NetClient::Run(std::uint32_t a_host, std::uint16_t a_port)
{
	ENetAddress address{};
	address.host = a_host;
	address.port = a_port;

	const auto host = enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0);
	const auto peer = host ? enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0) : nullptr;
	if (!peer) {
		REX::ERROR("NetClient: could not create connection");
		if (host) {
			enet_host_destroy(host);
		}
		status = Status::kDisconnected;
		std::scoped_lock l{ queueLock };
		inbox.push_back({ Event::Type::kDisconnected, {} });
		return;
	}
	enet_peer_timeout(peer, 0, 10000, 20000);

	const auto push = [this](Event a_event) {
		std::scoped_lock l{ queueLock };
		inbox.push_back(std::move(a_event));
	};

	bool connected = false;
	while (!stopRequested) {
		std::vector<Outgoing> pending;
		{
			std::scoped_lock l{ queueLock };
			pending.swap(outbox);
		}
		if (connected) {
			for (const auto& packet : pending) {
				Net::Send(peer, packet.data, packet.reliable);
			}
		}

		ENetEvent event;
		while (enet_host_service(host, &event, 2) > 0) {
			switch (event.type) {
			case ENET_EVENT_TYPE_CONNECT:
				connected = true;
				status = Status::kConnected;
				push({ Event::Type::kConnected, {} });
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				push({ Event::Type::kPacket, { event.packet->data, event.packet->data + event.packet->dataLength } });
				enet_packet_destroy(event.packet);
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				connected = false;
				stopRequested = true;
				break;
			default:
				break;
			}
		}
	}

	if (connected) {
		enet_peer_disconnect(peer, 0);
		// Give the server a moment to hear about it, so others see us leave immediately.
		ENetEvent event;
		const auto deadline = std::chrono::steady_clock::now() + 300ms;
		while (std::chrono::steady_clock::now() < deadline && enet_host_service(host, &event, 50) >= 0) {
			if (event.type == ENET_EVENT_TYPE_RECEIVE) {
				enet_packet_destroy(event.packet);
			} else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
				break;
			}
		}
	}
	enet_host_destroy(host);

	status = Status::kDisconnected;
	push({ Event::Type::kDisconnected, {} });
}
