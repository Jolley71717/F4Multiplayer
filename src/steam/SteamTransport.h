#pragma once

#include "Transport.h"
#include "net/ClientConnection.h"
#include "steam/SteamApi.h"

// Steam networking (ISteamNetworkingMessages): players address each other by Steam ID and traffic
// goes through Valve's relays, so nobody opens ports and nobody sees anyone's IP address.
//
// Each direction has a data channel and a control channel. Control messages are one byte:
// a keepalive (also the "hello" that opens the connection) or a goodbye.
namespace SteamTransport
{
	inline constexpr int CHANNEL_TO_SERVER = 0;
	inline constexpr int CHANNEL_TO_CLIENT = 1;
	inline constexpr int CHANNEL_CONTROL_TO_SERVER = 2;
	inline constexpr int CHANNEL_CONTROL_TO_CLIENT = 3;

	// "steam:76561198000000000" -> the Steam ID, or 0.
	[[nodiscard]] SteamApi::SteamId ParseAddress(std::string_view a_address);
	[[nodiscard]] std::string       FormatAddress(SteamApi::SteamId a_id);
}

// The host's side: the session server reaches players connected over Steam through this.
class SteamServerTransport final : public ServerTransport
{
public:
	explicit SteamServerTransport(std::size_t a_maxPeers);
	~SteamServerTransport() override;

	void Poll(std::vector<TransportEvent>& a_out, int a_waitMs) override;
	void Send(PeerId a_peer, std::span<const std::uint8_t> a_data, bool a_reliable) override;
	void Disconnect(PeerId a_peer) override;
	void Close() override;
	[[nodiscard]] std::string Describe(PeerId a_peer) const override;

private:
	using Clock = std::chrono::steady_clock;

	struct Peer
	{
		SteamApi::SteamId id;
		Clock::time_point lastHeard;
		Clock::time_point nextKeepAlive;
		bool              closing = false;
		Clock::time_point closeAt;
	};

	void Receive(int a_channel, Clock::time_point a_now, std::vector<TransportEvent>& a_out);
	void Forget(std::unordered_map<PeerId, Peer>::iterator a_it);

	std::size_t                                    maxPeers;
	PeerId                                         nextId = 1;
	std::unordered_map<PeerId, Peer>               peers;
	std::unordered_map<SteamApi::SteamId, PeerId>  ids;
	std::vector<PeerId>                            pendingDisconnects;
};

// A player's side: connects to a host's Steam ID ("steam:<id>").
class SteamClient final : public ClientConnection
{
public:
	~SteamClient() override { Disconnect(); }

private:
	void Run(std::string a_address, std::uint16_t a_defaultPort) override;
};
