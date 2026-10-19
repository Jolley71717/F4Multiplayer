// Standalone dedicated server: F4MPServer.exe [--port N] [--max-players N] [--password X] [--friendly-fire] [--own-story] [--session-file PATH] [--ban-file PATH]
//                                              [--max-world-entries N] [--max-share N] [--loopback-only]
// --max-world-entries: how much each of the session's tables may hold (at most the default, 60000).
// --max-share: how much one player other than the host may add to each table this run (kills, pickups,
// doors, quest stages, loot and building alike). Both are for trying the limits (tools\soak.ps1 -Abuse).
// --ban-file: where the bans are kept, so they outlive the server; without it they last until it stops.
// While it runs, the console takes the host controls (Server::Console): help, players, kick <n>, ban <n>, bans,
// unban <n | ~alias | all>, password <text> (password - removes it), lock and unlock. The answers go to the output,
// which names no player.

#include "Protocol.h"
#include "Server.h"

#ifdef _WIN32
#	include <Windows.h>
#else
#	include <csignal>
#endif

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace
{
	std::atomic<bool> quit{ false };
	std::atomic<bool> stopped{ false };

#ifdef _WIN32
	BOOL WINAPI OnConsoleSignal(DWORD)
	{
		quit = true;
		// Closing the window ends the process as soon as this returns, so give main() time to
		// disconnect everyone cleanly (Windows allows about five seconds).
		for (int i = 0; i < 40 && !stopped; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return TRUE;
	}
#else
	// Ctrl+C or a kill (SIGINT, SIGTERM): the process goes on until main() returns, so only
	// mark the end here; main() disconnects everyone and returns.
	void OnSignal(int)
	{
		quit = true;
	}
#endif

	template <class T>
	bool ParseNumber(std::string_view a_str, T& a_out)
	{
		const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), a_out);
		return ec == std::errc{} && ptr == a_str.data() + a_str.size();
	}
}

int main(int argc, char* argv[])
{
	Server::Options options;
	options.sharedStory = true;  // as the game's own hosting (sStory = shared); --own-story for each player's own
	options.port = Protocol::DEFAULT_PORT;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		const bool             hasValue = i + 1 < argc;
		if (arg == "--port" && hasValue && ParseNumber(argv[i + 1], options.port)) {
			++i;
		} else if (arg == "--max-players" && hasValue && ParseNumber(argv[i + 1], options.maxPlayers)) {
			++i;
		} else if (arg == "--password" && hasValue) {
			options.password = argv[++i];
		} else if (arg == "--friendly-fire") {
			options.friendlyFire = true;
		} else if (arg == "--own-story") {
			options.sharedStory = false;
		} else if (arg == "--shared-story") {  // the default since 0.8.7, still accepted
			options.sharedStory = true;
		} else if (arg == "--ban-file" && hasValue) {
			options.banFile = argv[++i];
		} else if (arg == "--session-file" && hasValue) {
			options.sessionFile = argv[++i];
		} else if (arg == "--loopback-only") {
			// This machine only (tests and soaks): nothing from outside can reach it, and Windows asks nothing.
			options.udpLoopbackOnly = true;
		} else if (arg == "--max-world-entries" && hasValue && ParseNumber(argv[i + 1], options.maxWorldEntries)) {
			++i;
		} else if (std::size_t share = 0; arg == "--max-share" && hasValue && ParseNumber(argv[i + 1], share)) {
			options.maxWorldEntriesPerPlayer = options.maxContainerChangesPerPlayer = options.maxWorkshopItemsPerPlayer = share;
			++i;
		} else {
			std::cerr << "usage: F4MPServer [--port N] [--max-players N] [--password X] [--friendly-fire] [--own-story] [--session-file PATH] [--ban-file PATH] [--loopback-only] [--max-world-entries N] [--max-share N]\n";
			return 2;
		}
	}

	Server server{ [](std::string_view a_line) {
		std::cout << a_line << std::endl;
	} };

	if (!server.Start(options)) {
		return 1;
	}

#ifdef _WIN32
	SetConsoleCtrlHandler(OnConsoleSignal, TRUE);
#else
	std::signal(SIGINT, OnSignal);
	std::signal(SIGTERM, OnSignal);
#endif
	std::cout << "Press Ctrl+C to stop. Type help for the host controls (kick, ban, unban, password, lock)." << std::endl;

	// The console's lines go to the server thread. Detached: getline blocks until a line or the end of the input (none
	// when started without a console), and the process ends without waiting for it.
	std::thread{ [&server] {
		for (std::string line; !quit && std::getline(std::cin, line) && !quit;) {
			server.Console(line);
		}
	} }.detach();

	while (!quit) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	server.Stop();
	stopped = true;
	return 0;
}
