#pragma once

// A real Server with one FakeTransport; the test plays the players. Stopped at the end of the test.

#include "FakeTransport.h"
#include "Test.h"

#include "Protocol.h"
#include "Server.h"

#include <random>

namespace TestServer
{
	using MT = Protocol::MessageType;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	struct Fixture
	{
		Server         server{ [this](std::string_view a_line) { std::lock_guard lock{ logMutex }; logLines.emplace_back(a_line); } };
		FakeTransport* fake = nullptr;

		explicit Fixture(Server::Options a_options = {})
		{
			// The server also listens on UDP (this machine only). Test runs can overlap, so start
			// somewhere random and move on while a port is taken.
			static std::uint16_t port = static_cast<std::uint16_t>(40000 + std::random_device{}() % 20000);
			a_options.udpLoopbackOnly = true;
			for (int attempt = 0; attempt < 50; ++attempt) {
				auto transport = std::make_unique<FakeTransport>();
				const auto raw = transport.get();
				std::vector<std::unique_ptr<ServerTransport>> extra;
				extra.push_back(std::move(transport));
				a_options.port = port++;
				if (server.Start(a_options, std::move(extra))) {
					fake = raw;
					return;
				}
			}
			REQUIRE(fake);
		}

		~Fixture() { server.Stop(); }

		static Protocol::Hello MakeHello(std::string a_name, std::uint64_t a_identity = 0)
		{
			Protocol::Hello hello;
			hello.contentHash = 0x1234;
			hello.name = std::move(a_name);
			hello.identity = a_identity;
			return hello;
		}

		// Connects a_peer, says hello and returns the Welcome.
		std::optional<Protocol::Welcome> Join(PeerId a_peer, const Protocol::Hello& a_hello)
		{
			fake->Connect(a_peer);
			fake->Deliver(a_peer, Protocol::Encode(a_hello));
			const auto packet = fake->WaitFor(a_peer, Type(MT::kWelcome));
			return packet ? Protocol::DecodeWelcome(*packet) : std::nullopt;
		}

		bool Logged(std::string_view a_text)
		{
			std::lock_guard lock{ logMutex };
			return std::ranges::any_of(logLines, [&](const std::string& a_line) { return a_line.find(a_text) != std::string::npos; });
		}

	private:
		std::mutex               logMutex;
		std::vector<std::string> logLines;
	};
}
