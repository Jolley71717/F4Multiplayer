#include "net/NetClient.h"

#include "Net.h"
#include "Protocol.h"

// wingdi.h (pulled in by ENet) defines ERROR, which collides with REX::ERROR.
#undef ERROR

namespace
{
	// How long a connection may take to come up before the friend is told nothing answered (and it goes on trying).
	constexpr auto CONNECT_DEADLINE = 8s;

	// One name lookup, on a thread of its own: getaddrinfo can block for a long time on a name that does not resolve, and the
	// game's thread joins this one when the player leaves or joins elsewhere. The thread holds only this.
	struct Lookup
	{
		std::atomic<bool> done{ false };
		bool              ok = false;
		ENetAddress       address{};
	};
}

void NetClient::Run(std::string a_address, std::uint16_t a_defaultPort)
{
	// The address (a friend's or a host's place on the internet) is never put in the log, nor is the text of a name.
	const auto fail = [this](std::string_view a_why, Failure a_failure) {
		REX::ERROR("NetClient: {}", a_why);
		Finish(a_failure);
	};

	if (!Net::Initialize()) {
		fail("ENet initialization failed", Failure::kNone);
		return;
	}

	std::string   hostName;
	std::uint16_t port{};
	if (!Net::SplitAddress(a_address, a_defaultPort, hostName, port)) {
		Net::Shutdown();
		fail("cannot resolve the address (not a host and port)", Failure::kResolve);
		return;
	}

	ENetAddress address{};
	if (enet_address_set_host_ip(&address, hostName.c_str()) == 0) {
		address.port = port;  // a dotted IPv4: no lookup
	} else {
		const auto lookup = std::make_shared<Lookup>();
		std::thread([lookup, hostName, port] {
			// Its own reference to the network library: Net::Shutdown may run while the lookup still does.
			WSADATA data;
			if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
				lookup->ok = enet_address_set_host(&lookup->address, hostName.c_str()) == 0;
				lookup->address.port = port;
				WSACleanup();
			}
			lookup->done = true;
		}).detach();
		while (!lookup->done && !stopRequested) {
			std::this_thread::sleep_for(2ms);
		}
		if (stopRequested && !lookup->done) {
			Net::Shutdown();
			Finish();  // the lookup ends on its own
			return;
		}
		if (!lookup->ok) {
			Net::Shutdown();
			fail(std::format("cannot resolve the address (a name, {} characters)", hostName.size()), Failure::kResolve);
			return;
		}
		address = lookup->address;
	}

	const auto host = enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0);
	const auto peer = host ? enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0) : nullptr;
	if (!peer) {
		if (host) {
			enet_host_destroy(host);
		}
		Net::Shutdown();
		fail("could not create connection", Failure::kNone);
		return;
	}
	host->maximumPacketSize = 1024 * 1024;
	enet_peer_timeout(peer, 0, 10000, 20000);

	bool       connected = false;
	Failure    failure = Failure::kNone;  // set only when the connection ends by itself, not by our Disconnect
	const auto started = std::chrono::steady_clock::now();
	while (!stopRequested) {
		auto pending = TakeOutgoing();
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
				Push({ Event::Type::kConnected, {} });
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				Push({ Event::Type::kPacket, { event.packet->data, event.packet->data + event.packet->dataLength } });
				enet_packet_destroy(event.packet);
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				failure = connected ? Failure::kLost : Failure::kNoAnswer;
				connected = false;
				stopRequested = true;
				break;
			default:
				break;
			}
		}
		if (!connected && failure == Failure::kNone && std::chrono::steady_clock::now() - started > CONNECT_DEADLINE) {
			failure = Failure::kNoAnswer;
			stopRequested = true;
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
	Net::Shutdown();

	Finish(failure);
}
