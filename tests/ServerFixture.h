#pragma once

// A real Server with one FakeTransport; the test plays the players. Stopped at the end of the test.

#include "FakeTransport.h"
#include "Test.h"

#include "Protocol.h"
#include "Server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <iterator>
#include <random>
#include <string_view>
#include <thread>
#include <type_traits>

namespace TestServer
{
	using MT = Protocol::MessageType;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	struct Fixture
	{
		Server         server{ [this](std::string_view a_line) { std::lock_guard lock{ logMutex }; logLines.emplace_back(a_line); } };
		FakeTransport* fake = nullptr;
		std::uint16_t  udpPort = 0;  // the server's real UDP transport (ENet), on 127.0.0.1

		explicit Fixture(Server::Options a_options = {})
		{
			// The server also listens on UDP (this machine only). Test runs can overlap, so start
			// somewhere random and move on while a port is taken. A port that can't be had is often in
			// a block Windows reserved (Hyper-V takes them a hundred at a time), so the next try starts
			// somewhere else rather than at the next port, which would likely be reserved too.
			const auto randomPort = [] { return static_cast<std::uint16_t>(40000 + std::random_device{}() % 20000); };
			static std::uint16_t port = randomPort();
			a_options.udpLoopbackOnly = true;
			for (int attempt = 0; attempt < 50; ++attempt) {
				auto transport = std::make_unique<FakeTransport>();
				const auto raw = transport.get();
				std::vector<std::unique_ptr<ServerTransport>> extra;
				extra.push_back(std::move(transport));
				a_options.port = port++;
				if (server.Start(a_options, std::move(extra))) {
					fake = raw;
					udpPort = a_options.port;
					return;
				}
				port = randomPort();
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

		// Everything the server logged so far.
		std::vector<std::string> LogLines()
		{
			std::lock_guard lock{ logMutex };
			return logLines;
		}

		// The log lines that contain a_text.
		std::vector<std::string> LogLines(std::string_view a_text)
		{
			std::lock_guard          lock{ logMutex };
			std::vector<std::string> out;
			std::ranges::copy_if(logLines, std::back_inserter(out), [&](const std::string& a_line) { return a_line.find(a_text) != std::string::npos; });
			return out;
		}

	private:
		std::mutex               logMutex;
		std::vector<std::string> logLines;
	};

	inline std::uint32_t JoinAs(Fixture& a_f, PeerId a_peer, std::string a_name, std::uint64_t a_identity = 0)
	{
		const auto welcome = a_f.Join(a_peer, Fixture::MakeHello(std::move(a_name), a_identity));
		REQUIRE(welcome);
		return welcome->playerId;
	}

	// Returns once the server has handled everything delivered so far: it handles packets in the
	// order they arrive, and answers a heartbeat right away. (At most 10 a second per player.) The
	// answer is known by its token, because what a_peer already sent can hold heartbeats of its own,
	// answered the same way. Those earlier answers are dropped here.
	inline void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::atomic<std::uint32_t> token = 0x5C000000;  // no protocol sample's token, nor one byte off it
		const auto                        mine = ++token;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ mine }, MT::kHeartbeat));
		for (;;) {
			const auto ack = a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck));
			REQUIRE(ack);
			if (const auto beat = Protocol::DecodeHeartbeat(*ack); beat && beat->token == mine) {
				return;
			}
		}
	}

	// Everything the server sent a_peer up to now.
	inline std::vector<std::vector<std::uint8_t>> Received(Fixture& a_f, PeerId a_peer)
	{
		Sync(a_f, a_peer);
		return a_f.fake->Take(a_peer);
	}

	inline void Clear(Fixture& a_f, std::initializer_list<PeerId> a_peers)
	{
		for (const auto peer : a_peers) {
			Received(a_f, peer);
		}
	}

	inline std::size_t Count(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_packets, [&](const auto& a_packet) { return !a_packet.empty() && a_packet[0] == Type(a_type); }));
	}

	// The packets of a_type, decoded.
	template <class Decode>
	auto Decoded(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type, Decode a_decode)
	{
		using T = typename std::invoke_result_t<Decode, std::span<const std::uint8_t>>::value_type;
		std::vector<T> out;
		for (const auto& packet : a_packets) {
			if (!packet.empty() && packet[0] == Type(a_type)) {
				auto message = a_decode(std::span<const std::uint8_t>{ packet });
				REQUIRE(message);
				out.push_back(std::move(*message));
			}
		}
		return out;
	}

	inline std::vector<Protocol::WorldState> WorldStates(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		return Decoded(a_packets, MT::kWorldState, [](auto a_data) { return Protocol::DecodeWorldState(a_data); });
	}

	// Waits up to a_timeout for a_peer to be sent a packet of a_type, keeping everything that came in a_got.
	inline bool Await(Fixture& a_f, PeerId a_peer, MT a_type, std::vector<std::vector<std::uint8_t>>& a_got,
		std::chrono::milliseconds a_timeout = std::chrono::milliseconds(3000))
	{
		const auto deadline = std::chrono::steady_clock::now() + a_timeout;
		while (std::chrono::steady_clock::now() < deadline) {
			std::ranges::move(a_f.fake->Take(a_peer), std::back_inserter(a_got));
			if (Count(a_got, a_type) > 0) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return false;
	}

	// Everything a_peer is sent for a_wait.
	inline std::vector<std::vector<std::uint8_t>> During(Fixture& a_f, PeerId a_peer, std::chrono::milliseconds a_wait)
	{
		std::this_thread::sleep_for(a_wait);
		return a_f.fake->Take(a_peer);
	}
}
