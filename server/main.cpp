// Standalone dedicated server: F4MPServer.exe [--port N] [--max-players N] [--password X]

#include "Protocol.h"
#include "Server.h"

#include <Windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string_view>
#include <thread>

namespace
{
	std::atomic<bool> quit{ false };

	BOOL WINAPI OnConsoleSignal(DWORD)
	{
		quit = true;
		return TRUE;
	}

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
		} else {
			std::cerr << "usage: F4MPServer [--port N] [--max-players N] [--password X]\n";
			return 2;
		}
	}

	Server server{ [](std::string_view a_line) {
		std::cout << a_line << std::endl;
	} };

	if (!server.Start(options)) {
		return 1;
	}

	SetConsoleCtrlHandler(OnConsoleSignal, TRUE);
	std::cout << "Press Ctrl+C to stop." << std::endl;

	while (!quit) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	server.Stop();
	return 0;
}
