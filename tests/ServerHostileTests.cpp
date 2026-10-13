// A joiner that sends what no game would: every message the protocol knows, cut short, padded,
// corrupted, or of a kind only the server sends. The others must not notice, and everything the
// server passes on, sends a later joiner or saves for the next run must read back cleanly.

#include "ProtocolSamples.h"
#include "ServerFixture.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <random>

using namespace TestServer;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t NPC = 0x0001F0A1;  // a plugin reference no sample uses

	// Returns once the server has handled everything delivered so far (it handles packets in order
	// and answers a heartbeat right away).
	void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ ++token }, MT::kHeartbeat));
		REQUIRE(a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck)));
	}

	Packets Received(Fixture& a_f, PeerId a_peer)
	{
		Sync(a_f, a_peer);
		return a_f.fake->Take(a_peer);
	}

	// What a client would make of each packet: the decoder its game uses for that type.
	void CheckEveryPacketReads(const Packets& a_packets, std::string_view a_who)
	{
		std::map<std::uint8_t, ProtocolSamples::Decoder> decoders;
		for (const auto& sample : ProtocolSamples::Samples()) {
			decoders.try_emplace(sample.packet[0], sample.decode);
		}
		for (const auto& packet : a_packets) {
			REQUIRE(!packet.empty());
			const auto decoder = decoders.find(packet[0]);
			if (packet[0] < Type(MT::kWelcome) || decoder == decoders.end()) {
				Test::Fail(__FILE__, __LINE__, std::format("{} was sent a packet of type {}, which the server never sends", a_who, packet[0]));
			} else if (!decoder->second(packet)) {
				Test::Fail(__FILE__, __LINE__, std::format("{} was sent a type {} packet that does not decode", a_who, packet[0]));
			}
		}
	}

	// Every message as a hostile or broken client could send it. A hello is left out: it starts the
	// sender's own arrival over, and the hello tests cover it.
	Packets Hostile()
	{
		std::mt19937 random{ 27 };
		Packets      out;
		for (const auto& sample : ProtocolSamples::Samples()) {
			const auto& packet = sample.packet;
			if (packet[0] == Type(MT::kHello)) {
				continue;
			}
			out.push_back(packet);  // as is: a server-only type, or a report that may well be believed
			if (packet[0] >= Type(MT::kWelcome)) {
				continue;
			}
			out.emplace_back(packet.begin(), packet.begin() + 1);
			out.emplace_back(packet.begin(), packet.begin() + packet.size() / 2);
			out.emplace_back(packet.begin(), packet.end() - 1);
			out.push_back(packet);
			out.back().push_back(0xFF);
			for (int i = 0; i < 3 && packet.size() > 1; ++i) {
				out.push_back(packet);
				auto& broken = out.back();
				broken[1 + random() % (broken.size() - 1)] ^= static_cast<std::uint8_t>(1 + random() % 255);
			}
		}
		return out;
	}
}

TEST("server: a joiner sending broken, cut-short or server-only packets of every kind can't spoil the session for the others")
{
	const auto path = (std::filesystem::temp_directory_path() / std::format("f4mp-hostile-{}.session", std::rand())).string();
	std::filesystem::remove(path);
	Server::Options options;
	options.sessionFile = path;
	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
		const auto alice = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
		REQUIRE(alice);
		REQUIRE(f.Join(3, Fixture::MakeHello("Mallory", 0xBAD)));

		const auto hostile = Hostile();
		for (const auto& packet : hostile) {
			f.fake->Deliver(3, packet);
		}
		Sync(f, 3);
		// The server read all of it: none was dropped for being too much, so every decoder was reached.
		CHECK(!f.Logged("sends too many events"));

		// Bob and Alice still play together: her kill reaches him.
		f.fake->Deliver(2, Protocol::Encode(Protocol::ActorDeath{ NPC, 0, true }, MT::kReportDeath));
		const auto bobGot = Received(f, 1);
		const auto died = std::ranges::find_if(bobGot, [](const auto& a_packet) {
			const auto death = a_packet[0] == Type(MT::kActorDied) ? Protocol::DecodeActorDeath(a_packet) : std::nullopt;
			return death && death->refId == NPC;
		});
		REQUIRE(died != bobGot.end());
		CHECK(Protocol::DecodeActorDeath(*died)->playerId == alice->playerId);
		CHECK(!f.fake->Disconnected(1));
		CHECK(!f.fake->Disconnected(2));

		CheckEveryPacketReads(bobGot, "Bob");
		CheckEveryPacketReads(Received(f, 2), "Alice");
		CheckEveryPacketReads(f.fake->Take(3), "Mallory");

		// Someone joining now catches up on what was believed of it all, and can read every bit.
		REQUIRE(f.Join(4, Fixture::MakeHello("Dave", 0xDA5E)));
		const auto catchUp = f.fake->WaitFor(4, Type(MT::kWorldState));
		REQUIRE(catchUp);
		auto daveGot = Received(f, 4);
		daveGot.push_back(*catchUp);
		CheckEveryPacketReads(daveGot, "Dave");
	}  // stopping saves

	// The host plays again the next day: the session file it wrote reads back, with Alice's kill in it.
	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
		CHECK(f.Logged("loaded the saved session"));
		const auto world = f.fake->WaitFor(1, Type(MT::kWorldState));
		REQUIRE(world);
		const auto state = Protocol::DecodeWorldState(*world);
		REQUIRE(state);
		CHECK(std::ranges::contains(state->deadActors, NPC));
		CheckEveryPacketReads(Received(f, 1), "Bob, the next day");
	}
	std::filesystem::remove(path);
}
