#include "Net.h"

#include "Protocol.h"

#include <charconv>
#include <mutex>

namespace Net
{
	namespace
	{
		std::mutex initLock;
		int        initCount = 0;
	}

	bool Initialize()
	{
		std::scoped_lock l{ initLock };
		if (initCount == 0 && enet_initialize() != 0) {
			return false;
		}
		++initCount;
		return true;
	}

	void Shutdown()
	{
		std::scoped_lock l{ initLock };
		if (initCount > 0 && --initCount == 0) {
			enet_deinitialize();
		}
	}

	void Send(ENetPeer* a_peer, std::span<const std::uint8_t> a_data, bool a_reliable)
	{
		const auto packet = enet_packet_create(a_data.data(), a_data.size(), a_reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
		if (!packet) {
			return;
		}
		if (enet_peer_send(a_peer, a_reliable ? Protocol::CHANNEL_RELIABLE : Protocol::CHANNEL_STATE, packet) != 0) {
			enet_packet_destroy(packet);
		}
	}

	bool ResolveAddress(const std::string& a_hostAndPort, std::uint16_t a_defaultPort, ENetAddress& a_out)
	{
		std::string   host = a_hostAndPort;
		std::uint16_t port = a_defaultPort;

		// "host:port", but leave bare IPv6-looking strings alone.
		if (const auto colon = host.rfind(':'); colon != std::string::npos && host.find(':') == colon) {
			const auto portStr = host.substr(colon + 1);
			std::uint16_t parsed{};
			const auto [ptr, ec] = std::from_chars(portStr.data(), portStr.data() + portStr.size(), parsed);
			if (ec != std::errc{} || ptr != portStr.data() + portStr.size() || parsed == 0) {
				return false;
			}
			port = parsed;
			host.resize(colon);
		}

		if (host.empty() || enet_address_set_host(&a_out, host.c_str()) != 0) {
			return false;
		}
		a_out.port = port;
		return true;
	}
}
