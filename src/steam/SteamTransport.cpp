#include "steam/SteamTransport.h"

namespace SteamTransport
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr std::uint8_t CONTROL_KEEPALIVE = 1;
		constexpr std::uint8_t CONTROL_BYE = 2;

		constexpr auto KEEPALIVE_INTERVAL = 1s;
		constexpr auto TIMEOUT = 20s;
		constexpr auto CONNECT_TIMEOUT = 30s;    // includes waiting for the host to accept
		constexpr auto CLOSE_DELAY = 2s;         // lets queued messages (a rejection) arrive before closing
		constexpr std::size_t MAX_MESSAGE_SIZE = 512 * 1024;  // Steam's limit for one message
		constexpr int BATCH = 64;

		bool SendTo(const SteamApi::Api* a_api, SteamApi::SteamId a_to, int a_channel, std::span<const std::uint8_t> a_data, bool a_reliable)
		{
			if (a_data.empty() || a_data.size() > MAX_MESSAGE_SIZE) {
				return false;
			}
			const auto identity = SteamApi::IdentityOf(a_to);
			const int  flags = SteamApi::SEND_AUTO_RESTART_BROKEN_SESSION |
			                  (a_reliable ? SteamApi::SEND_RELIABLE : SteamApi::SEND_UNRELIABLE | SteamApi::SEND_NO_NAGLE);
			return a_api->SendMessageToUser(a_api->messages, &identity, a_data.data(), static_cast<std::uint32_t>(a_data.size()), flags, a_channel) == 1;  // k_EResultOK
		}

		void SendControl(const SteamApi::Api* a_api, SteamApi::SteamId a_to, int a_channel, std::uint8_t a_control)
		{
			SendTo(a_api, a_to, a_channel, { &a_control, 1 }, true);
		}

		// Calls a_fn(message) for every waiting message on a channel, then releases them.
		template <class Fn>
		void Drain(const SteamApi::Api* a_api, int a_channel, Fn&& a_fn)
		{
			SteamApi::NetworkingMessage* messages[BATCH];
			int                          count = 0;
			do {
				count = a_api->ReceiveMessagesOnChannel(a_api->messages, a_channel, messages, BATCH);
				for (int i = 0; i < count; ++i) {
					a_fn(*messages[i]);
					a_api->ReleaseMessage(messages[i]);
				}
			} while (count == BATCH);
		}

		std::span<const std::uint8_t> Payload(const SteamApi::NetworkingMessage& a_message)
		{
			if (!a_message.data || a_message.size <= 0) {
				return {};
			}
			return { static_cast<const std::uint8_t*>(a_message.data), static_cast<std::size_t>(a_message.size) };
		}
	}

	SteamApi::SteamId ParseAddress(std::string_view a_address)
	{
		constexpr auto PREFIX = "steam:"sv;
		if (!a_address.starts_with(PREFIX)) {
			return 0;
		}
		a_address.remove_prefix(PREFIX.size());
		SteamApi::SteamId id = 0;
		const auto [ptr, ec] = std::from_chars(a_address.data(), a_address.data() + a_address.size(), id);
		return ec == std::errc{} && ptr == a_address.data() + a_address.size() ? id : 0;
	}

	std::string FormatAddress(SteamApi::SteamId a_id)
	{
		return std::format("steam:{}", a_id);
	}
}

using namespace SteamTransport;

SteamServerTransport::SteamServerTransport(std::size_t a_maxPeers) :
	maxPeers(a_maxPeers)
{}

SteamServerTransport::~SteamServerTransport()
{
	Close();
}

void SteamServerTransport::Receive(int a_channel, Clock::time_point a_now, std::vector<TransportEvent>& a_out)
{
	const auto api = SteamApi::Loaded();
	Drain(api, a_channel, [&](const SteamApi::NetworkingMessage& a_message) {
		const auto from = SteamApi::SteamIdOf(a_message.peer);
		const auto payload = Payload(a_message);
		if (!from || payload.empty()) {
			return;
		}

		auto known = ids.find(from);
		if (a_channel == CHANNEL_CONTROL_TO_SERVER) {
			if (payload[0] == CONTROL_KEEPALIVE && known == ids.end()) {
				if (peers.size() >= maxPeers) {
					return;
				}
				const auto id = nextId++;
				peers[id] = Peer{ from, a_now, a_now + KEEPALIVE_INTERVAL };
				ids[from] = id;
				a_out.push_back({ TransportEvent::Type::kConnect, id, {} });
				SendControl(api, from, CHANNEL_CONTROL_TO_CLIENT, CONTROL_KEEPALIVE);
				return;
			}
			if (known == ids.end()) {
				return;
			}
			auto& peer = peers.at(known->second);
			if (peer.closing) {
				return;
			}
			if (payload[0] == CONTROL_BYE) {
				a_out.push_back({ TransportEvent::Type::kDisconnect, known->second, {} });
				Forget(peers.find(known->second));
			} else {
				peer.lastHeard = a_now;
			}
			return;
		}

		// Data only counts from players whose connection we opened (their keepalive came first).
		if (known == ids.end()) {
			return;
		}
		auto& peer = peers.at(known->second);
		if (peer.closing) {
			return;
		}
		peer.lastHeard = a_now;
		a_out.push_back({ TransportEvent::Type::kPacket, known->second, { payload.begin(), payload.end() } });
	});
}

void SteamServerTransport::Forget(std::unordered_map<PeerId, Peer>::iterator a_it)
{
	if (const auto api = SteamApi::Loaded()) {
		const auto identity = SteamApi::IdentityOf(a_it->second.id);
		api->CloseSessionWithUser(api->messages, &identity);
	}
	ids.erase(a_it->second.id);
	peers.erase(a_it);
}

void SteamServerTransport::Poll(std::vector<TransportEvent>& a_out, int)
{
	for (const auto id : std::exchange(pendingDisconnects, {})) {
		a_out.push_back({ TransportEvent::Type::kDisconnect, id, {} });
	}

	const auto api = SteamApi::Loaded();
	if (!api) {
		return;
	}

	// Data before control, so a goodbye is handled after the packets sent before it.
	const auto now = Clock::now();
	Receive(CHANNEL_TO_SERVER, now, a_out);
	Receive(CHANNEL_CONTROL_TO_SERVER, now, a_out);

	for (auto it = peers.begin(); it != peers.end();) {
		auto& peer = it->second;
		if (peer.closing) {
			if (now >= peer.closeAt) {
				const auto next = std::next(it);
				Forget(it);
				it = next;
				continue;
			}
		} else if (now - peer.lastHeard > TIMEOUT) {
			a_out.push_back({ TransportEvent::Type::kDisconnect, it->first, {} });
			const auto next = std::next(it);
			Forget(it);
			it = next;
			continue;
		} else if (now >= peer.nextKeepAlive) {
			SendControl(api, peer.id, CHANNEL_CONTROL_TO_CLIENT, CONTROL_KEEPALIVE);
			peer.nextKeepAlive = now + KEEPALIVE_INTERVAL;
		}
		++it;
	}
}

void SteamServerTransport::Send(PeerId a_peer, std::span<const std::uint8_t> a_data, bool a_reliable)
{
	const auto api = SteamApi::Loaded();
	const auto it = peers.find(a_peer);
	if (api && it != peers.end() && !it->second.closing) {
		SendTo(api, it->second.id, CHANNEL_TO_CLIENT, a_data, a_reliable);
	}
}

void SteamServerTransport::Disconnect(PeerId a_peer)
{
	const auto api = SteamApi::Loaded();
	const auto it = peers.find(a_peer);
	if (!api || it == peers.end() || it->second.closing) {
		return;
	}
	SendControl(api, it->second.id, CHANNEL_CONTROL_TO_CLIENT, CONTROL_BYE);
	it->second.closing = true;
	it->second.closeAt = Clock::now() + CLOSE_DELAY;
	pendingDisconnects.push_back(a_peer);
}

void SteamServerTransport::Close()
{
	const auto api = SteamApi::Loaded();
	if (api) {
		for (const auto& [id, peer] : peers) {
			if (!peer.closing) {
				SendControl(api, peer.id, CHANNEL_CONTROL_TO_CLIENT, CONTROL_BYE);
			}
		}
	}
	// The sessions are left to expire, so the goodbyes still go out.
	peers.clear();
	ids.clear();
	pendingDisconnects.clear();
}

std::string SteamServerTransport::Describe(PeerId a_peer) const
{
	const auto it = peers.find(a_peer);
	return it != peers.end() ? std::format("steam {}", it->second.id) : "steam ?";
}

void SteamClient::Run(std::string a_address, std::uint16_t)
{
	const auto api = SteamApi::Loaded();
	const auto host = ParseAddress(a_address);
	if (!api || !host) {
		REX::ERROR("SteamClient: can't connect to '{}' ({})", a_address, api ? "bad address" : "Steam isn't running");
		Finish();
		return;
	}

	const auto started = Clock::now();
	auto       lastHeard = started;
	auto       nextKeepAlive = started;
	bool       connected = false;
	bool       hostClosed = false;

	const auto receiveData = [&](Clock::time_point a_now) {
		Drain(api, CHANNEL_TO_CLIENT, [&](const SteamApi::NetworkingMessage& a_message) {
			const auto payload = Payload(a_message);
			if (SteamApi::SteamIdOf(a_message.peer) != host || payload.empty()) {
				return;
			}
			lastHeard = a_now;
			if (connected) {
				Push({ Event::Type::kPacket, { payload.begin(), payload.end() } });
			}
		});
	};

	while (!stopRequested) {
		const auto now = Clock::now();

		auto pending = TakeOutgoing();
		if (connected) {
			for (const auto& packet : pending) {
				SendTo(api, host, CHANNEL_TO_SERVER, packet.data, packet.reliable);
			}
		}
		if (now >= nextKeepAlive) {
			SendControl(api, host, CHANNEL_CONTROL_TO_SERVER, CONTROL_KEEPALIVE);
			nextKeepAlive = now + KEEPALIVE_INTERVAL;
		}

		// Data before control, so a rejection is shown before the goodbye that follows it.
		receiveData(now);
		Drain(api, CHANNEL_CONTROL_TO_CLIENT, [&](const SteamApi::NetworkingMessage& a_message) {
			const auto payload = Payload(a_message);
			if (SteamApi::SteamIdOf(a_message.peer) != host || payload.empty()) {
				return;
			}
			lastHeard = now;
			if (payload[0] == CONTROL_BYE) {
				hostClosed = true;
			} else if (payload[0] == CONTROL_KEEPALIVE && !connected) {
				connected = true;
				status = Status::kConnected;
				Push({ Event::Type::kConnected, {} });
				REX::INFO("SteamClient: connected to {}", host);
			}
		});

		if (hostClosed) {
			receiveData(now);
			REX::INFO("SteamClient: host closed the connection");
			break;
		}
		if (!connected && now - started > CONNECT_TIMEOUT) {
			REX::WARN("SteamClient: {} did not answer", host);
			break;
		}
		if (connected && now - lastHeard > TIMEOUT) {
			REX::WARN("SteamClient: lost connection to {}", host);
			break;
		}
		std::this_thread::sleep_for(2ms);
	}

	if (connected && !hostClosed) {
		SendControl(api, host, CHANNEL_CONTROL_TO_SERVER, CONTROL_BYE);
	}
	Finish();
}
