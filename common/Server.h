#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

// Relay server for player state. Runs on its own thread; used by the standalone
// F4MPServer.exe and by the game plugin when a player hosts.
class Server
{
public:
	struct Options
	{
		std::uint16_t port = 7779;
		std::size_t   maxPlayers = 4;
		std::string   password;  // empty = no password
	};

	using LogFn = std::function<void(std::string_view)>;

	explicit Server(LogFn a_log);
	~Server();

	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;

	// Binds the port and starts the server thread. Returns false if the port can't be bound.
	bool Start(const Options& a_options);
	void Stop();

	[[nodiscard]] bool Running() const { return running; }

private:
	void Run(void* a_host);

	LogFn             log;
	Options           options;
	std::thread       thread;
	std::atomic<bool> running{ false };
	std::atomic<bool> stopRequested{ false };
};
