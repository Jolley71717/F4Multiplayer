// Joins by address: the server's UDP port opened (and closed) on more addresses of the machine while the session runs.

#include "ServerFixture.h"

#include "Net.h"

#include <chrono>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	// A game connecting over UDP to a_ip, as a friend joining by address does.
	class UdpGame
	{
	public:
		UdpGame(const char* a_ip, std::uint16_t a_port)
		{
			initialized = Net::Initialize();
			host = initialized ? enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0) : nullptr;
			if (host) {
				ENetAddress address{};
				enet_address_set_host_ip(&address, a_ip);
				address.port = a_port;
				peer = enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0);
			}
		}

		~UdpGame()
		{
			if (peer) {
				enet_peer_reset(peer);
			}
			if (host) {
				enet_host_destroy(host);
			}
			if (initialized) {
				Net::Shutdown();
			}
		}

		UdpGame(const UdpGame&) = delete;
		UdpGame& operator=(const UdpGame&) = delete;

		// Services the connection for up to a_wait; true once it is connected, or (a_wantConnected false) dropped.
		bool Wait(bool a_wantConnected, std::chrono::milliseconds a_wait = 10s)
		{
			const auto until = std::chrono::steady_clock::now() + a_wait;
			while (host && std::chrono::steady_clock::now() < until) {
				ENetEvent event;
				while (enet_host_service(host, &event, 10) > 0) {
					if (event.type == ENET_EVENT_TYPE_CONNECT) {
						connected = true;
					} else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
						connected = false;
						peer = nullptr;
					} else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
						enet_packet_destroy(event.packet);
					}
				}
				if (connected == a_wantConnected) {
					return true;
				}
			}
			return connected == a_wantConnected;
		}

	private:
		bool      initialized = false;
		ENetHost* host = nullptr;
		ENetPeer* peer = nullptr;
		bool      connected = false;
	};
}

TEST("direct join: the port opens beside a running session and closes again, and only those who came by address are dropped")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("A")));  // a friend over another transport (Steam, in the game)
	CHECK(!f.server.DirectOpen());

	// 127.0.0.2 stands for the machine's network address: the listener on 127.0.0.1 is still there.
	{
		UdpGame early{ "127.0.0.2", f.udpPort };
		CHECK(!early.Wait(true, 600ms));  // nobody listens there yet
	}
	CHECK(f.server.SetDirect(true, { "127.0.0.2" }).empty());
	CHECK(f.server.DirectOpen());
	CHECK(f.Logged("joins by address open"));

	UdpGame joiner{ "127.0.0.2", f.udpPort };
	CHECK(joiner.Wait(true));
	// Opened already: nothing changes.
	CHECK(f.server.SetDirect(true, { "127.0.0.2" }).empty());
	CHECK(joiner.Wait(true, 300ms));

	CHECK(f.server.SetDirect(false, {}).empty());
	CHECK(!f.server.DirectOpen());
	CHECK(joiner.Wait(false));          // the one who came by address is gone
	CHECK(!f.Logged("player 1 left"));  // the friend in the session never noticed

	// And it can be opened again.
	CHECK(f.server.SetDirect(true, { "127.0.0.2" }).empty());
	UdpGame again{ "127.0.0.2", f.udpPort };
	CHECK(again.Wait(true));
}

TEST("direct join: an address that is not this machine's opens nothing, and says so")
{
	Fixture f;
	CHECK(!f.server.SetDirect(true, { "not an address" }).empty());
	CHECK(!f.server.DirectOpen());
	CHECK(!f.server.SetDirect(true, {}).empty());  // no network address at all
	CHECK(!f.server.DirectOpen());
	// A second address that fails leaves none of the first open.
	CHECK(!f.server.SetDirect(true, { "127.0.0.2", "not an address" }).empty());
	UdpGame none{ "127.0.0.2", f.udpPort };
	CHECK(!none.Wait(true, 600ms));
}
