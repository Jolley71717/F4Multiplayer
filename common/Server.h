#pragma once

#include "Transport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The session server: relays player state and keeps the shared world state (kills, loot, doors,
// quests, NPC ownership). Runs on its own thread; used by the standalone F4MPServer.exe and by
// the game plugin when a player hosts.
class Server
{
public:
	struct Options
	{
		std::uint16_t port = 7779;
		std::size_t   maxPlayers = 4;
		std::string   password;  // empty = no password

		// Accept UDP connections only from this machine (the host's own game), e.g. when friends
		// connect through Steam instead.
		bool udpLoopbackOnly = false;
	};

	using LogFn = std::function<void(std::string_view)>;

	explicit Server(LogFn a_log);
	~Server();

	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;

	// Binds the UDP port and starts the server thread. Players can also arrive through any extra
	// transports (e.g. Steam). Returns false if the port can't be bound.
	bool Start(const Options& a_options, std::vector<std::unique_ptr<ServerTransport>> a_extraTransports = {});
	void Stop();

	[[nodiscard]] bool Running() const { return running; }

private:
	void Run();

	LogFn                                         log;
	Options                                       options;
	std::vector<std::unique_ptr<ServerTransport>> transports;
	std::thread                                   thread;
	std::atomic<bool>                             running{ false };
	std::atomic<bool>                             stopRequested{ false };
};
