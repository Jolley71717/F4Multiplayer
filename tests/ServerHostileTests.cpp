// A joiner that sends what no game would: every message the protocol knows, cut short, padded,
// corrupted, or of a kind only the server sends. The others must not notice, and everything the
// server passes on, sends a later joiner or saves for the next run must read back cleanly.

#include "ProtocolSamples.h"
#include "ServerFixture.h"
#include "SessionFile.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <random>
#include <set>

using namespace TestServer;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t NPC = 0x0001F0A1;  // a plugin reference no sample uses

	// Returns once the server has handled everything a_peer sent so far: it handles one player's
	// packets in order and answers a heartbeat right away. The answer is known by its token, because
	// what a_peer already sent can hold heartbeats of its own, answered the same way. Those earlier
	// answers are dropped here.
	void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0x5C000000;  // no sample's token, nor one byte off it
		const auto           mine = ++token;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ mine }, MT::kHeartbeat));
		for (;;) {
			const auto ack = a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck));
			REQUIRE(ack);
			if (const auto beat = Protocol::DecodeHeartbeat(*ack); beat && beat->token == mine) {
				return;
			}
		}
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

	// The players a packet from the server says did something (killed, spoke, built, owns an NPC, ...),
	// which the receiving game shows as that player's doing. 0 (nobody) is left out.
	std::vector<std::uint32_t> Doers(std::span<const std::uint8_t> a_packet)
	{
		using namespace Protocol;
		std::vector<std::uint32_t> ids;
		const auto                 add = [&](const auto& a_msg) {
			if (a_msg) {
				ids.push_back(a_msg->playerId);
			}
		};
		switch (static_cast<MT>(a_packet[0])) {
		case MT::kPlayerJoined:
			add(DecodePlayerJoined(a_packet));
			break;
		case MT::kPlayerLeft:
			add(DecodePlayerLeft(a_packet));
			break;
		case MT::kPlayerStates:
			if (const auto states = DecodePlayerStates(a_packet)) {
				for (const auto& entry : states->players) {
					ids.push_back(entry.playerId);
				}
			}
			break;
		case MT::kActorDied:
			add(DecodeActorDeath(a_packet));
			break;
		case MT::kWorldState:
			if (const auto world = DecodeWorldState(a_packet)) {
				for (const auto& change : world->containerChanges) {
					ids.push_back(change.playerId);
				}
				for (const auto& state : world->refStates) {
					ids.push_back(state.playerId);
				}
			}
			break;
		case MT::kContainerChanged:
			add(DecodeIndexedContainerChange(a_packet));
			break;
		case MT::kRefStateChanged:
			add(DecodeRefState(a_packet));
			break;
		case MT::kPlayerEquipment:
			add(DecodeEquipment(a_packet));
			break;
		case MT::kActorOwners:
			if (const auto owners = DecodeOwners(a_packet)) {
				for (const auto& owner : *owners) {
					if (owner.playerId != 0) {
						ids.push_back(owner.playerId);
					}
				}
			}
			break;
		case MT::kPlayerDamaged:
			add(DecodePlayerHit(a_packet));
			break;
		case MT::kShotFired:
			add(DecodeShot(a_packet));
			break;
		case MT::kPlayerStatus:
			add(DecodePlayerStatus(a_packet));
			break;
		case MT::kWorldTime:
			add(DecodeWorldTime(a_packet));
			break;
		case MT::kPinged:
			add(DecodePing(a_packet));
			break;
		case MT::kPartyXp:
			add(DecodeXpGain(a_packet));
			break;
		case MT::kQuestDone:
			add(DecodeQuestDone(a_packet));
			break;
		case MT::kRevived:
			add(DecodeRevive(a_packet));
			break;
		case MT::kVoiceRelay:
			add(DecodeVoice(a_packet));
			break;
		case MT::kMarkersFound:
			add(DecodeMarkers(a_packet));
			break;
		case MT::kLineSpoken:
			add(DecodeLine(a_packet));
			break;
		case MT::kNpcEquipment:
			add(DecodeNpcEquipment(a_packet));
			break;
		case MT::kPlayerFace:
			add(DecodeFace(a_packet));
			break;
		case MT::kWorkshopItem:
			add(DecodeWorkshopItem(a_packet));
			break;
		case MT::kWorkshopItems:
			if (const auto items = DecodeWorkshopItems(a_packet)) {
				for (const auto& item : *items) {
					if (item.playerId != 0) {
						ids.push_back(item.playerId);
					}
				}
			}
			break;
		case MT::kExplosion:
			add(DecodeExplosion(a_packet));
			break;
		case MT::kActivation:
			add(DecodeActivation(a_packet));
			break;
		default:
			break;
		}
		std::erase(ids, 0u);
		return ids;
	}

	// Every packet in a_packets that names a player names a_doer: what a joiner sends is never shown
	// as another player's doing, whatever player id it put in the packet itself.
	void CheckAllNamed(const Packets& a_packets, std::uint32_t a_doer, std::string_view a_who)
	{
		for (const auto& packet : a_packets) {
			for (const auto id : Doers(packet)) {
				if (id != a_doer) {
					Test::Fail(__FILE__, __LINE__, std::format("{} was sent a type {} packet naming player {}, not {}", a_who, packet[0], id, a_doer));
				}
			}
		}
	}

	std::set<std::uint8_t> TypesNaming(const Packets& a_packets, std::uint32_t a_doer)
	{
		std::set<std::uint8_t> types;
		for (const auto& packet : a_packets) {
			if (std::ranges::contains(Doers(packet), a_doer)) {
				types.insert(packet[0]);
			}
		}
		return types;
	}

	Packets OfType(const Packets& a_packets, MT a_type)
	{
		Packets out;
		std::ranges::copy_if(a_packets, std::back_inserter(out), [&](const auto& a_packet) { return a_packet[0] == Type(a_type); });
		return out;
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
	const auto path = Test::TempPath("f4mp-hostile", ".session").string();
	Server::Options options;
	options.sessionFile = path;
	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
		const auto alice = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
		REQUIRE(alice);
		const auto mallory = f.Join(3, Fixture::MakeHello("Mallory", 0xBAD));
		REQUIRE(mallory);
		CheckEveryPacketReads(Received(f, 1), "Bob, on joining");
		CheckEveryPacketReads(Received(f, 2), "Alice, on joining");

		const auto hostile = Hostile();
		for (const auto& packet : hostile) {
			f.fake->Deliver(3, packet);
		}
		Sync(f, 3);
		// The server read all of it: none was dropped for being too much, so every decoder was reached.
		CHECK(!f.Logged("sends too many events"));

		// What Mallory sent reaches the others as hers, whoever her packets claimed did it (the samples
		// name Bob, Alice and players who aren't there).
		const auto bobDuring = Received(f, 1);
		const auto aliceDuring = Received(f, 2);
		CheckEveryPacketReads(bobDuring, "Bob, during Mallory's packets");
		CheckEveryPacketReads(aliceDuring, "Alice, during Mallory's packets");
		CheckAllNamed(bobDuring, mallory->playerId, "Bob, during Mallory's packets");
		CheckAllNamed(aliceDuring, mallory->playerId, "Alice, during Mallory's packets");
		// Not by passing nothing on: some of each kind that names a player got through.
		const auto relayed = TypesNaming(bobDuring, mallory->playerId);
		for (const auto type : { MT::kVoiceRelay, MT::kWorkshopItem, MT::kActivation, MT::kLineSpoken, MT::kPinged, MT::kPlayerFace,
				 MT::kPlayerEquipment, MT::kPlayerStatus, MT::kShotFired, MT::kQuestDone }) {
			if (!relayed.contains(Type(type))) {
				Test::Fail(__FILE__, __LINE__, std::format("Bob was sent no type {} packet from Mallory", Type(type)));
			}
		}

		// Bob and Alice still play together: her kill reaches him, as hers.
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

		// Someone joining now catches up on what was believed of it all, and can read every bit. Apart
		// from who is here and Alice's kill, all of it is Mallory's doing (Bob and Alice did nothing else).
		const auto dave = f.Join(4, Fixture::MakeHello("Dave", 0xDA5E));
		REQUIRE(dave);
		const auto catchUp = f.fake->WaitFor(4, Type(MT::kWorldState));
		REQUIRE(catchUp);
		auto daveGot = Received(f, 4);
		daveGot.push_back(*catchUp);
		CheckEveryPacketReads(daveGot, "Dave");
		Packets daveWorld;
		std::ranges::copy_if(daveGot, std::back_inserter(daveWorld), [](const auto& a_packet) { return a_packet[0] != Type(MT::kPlayerJoined); });
		CheckAllNamed(daveWorld, mallory->playerId, "Dave, catching up");
		CHECK(TypesNaming(daveWorld, mallory->playerId).contains(Type(MT::kPlayerFace)));
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

TEST("server: a joiner's build, face or explosion with a broken number (NaN, infinity) reaches nobody, not a later joiner, not the next day")
{
	const auto path = Test::TempPath("f4mp-hostile", ".session").string();
	Server::Options options;
	options.sessionFile = path;

	constexpr float nan = std::numeric_limits<float>::quiet_NaN();
	constexpr float inf = std::numeric_limits<float>::infinity();
	const auto      build = [](std::uint32_t a_ref) {
		auto item = ProtocolSamples::MakeWorkshopItem();
		item.refId = a_ref;
		return item;
	};
	auto badPosition = build(0xFF0000A1);
	badPosition.position[1] = nan;
	auto badRotation = build(0xFF0000A2);
	badRotation.rotation[0] = -inf;
	auto badScale = build(0xFF0000A3);
	badScale.scale = inf;
	const auto good = build(0xFF0000A4);
	auto       badFace = ProtocolSamples::MakeFace();
	badFace.sliders[1] = nan;
	const auto goodFace = ProtocolSamples::MakeFace();
	const Protocol::Explosion badBlast{ 0, 0x0001D2EE, nan, 100.0f, 0.0f, 0, 0x3C };
	const Protocol::Explosion goodBlast{ 0, 0x0001D2EE, 50.0f, 100.0f, 0.0f, 0, 0x3C };

	const auto ids = [](const std::vector<Protocol::WorkshopItem>& a_items) {
		std::set<std::uint32_t> out;
		for (const auto& item : a_items) {
			out.insert(item.refId);
		}
		return out;
	};
	const auto workshopIn = [&](const Packets& a_packets) {
		std::vector<Protocol::WorkshopItem> items;
		for (const auto& packet : a_packets) {
			if (packet[0] == Type(MT::kWorkshopItem)) {
				items.push_back(*Protocol::DecodeWorkshopItem(packet));
			} else if (packet[0] == Type(MT::kWorkshopItems)) {
				const auto batch = Protocol::DecodeWorkshopItems(packet);
				items.insert(items.end(), batch->begin(), batch->end());
			}
		}
		return items;
	};

	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
		const auto mallory = f.Join(2, Fixture::MakeHello("Mallory", 0xBAD));
		REQUIRE(mallory);
		f.fake->Take(1);

		// Each broken one first, then the same kind done right.
		for (const auto& item : { badPosition, badRotation, badScale, good }) {
			f.fake->Deliver(2, Protocol::Encode(item, MT::kReportWorkshopItem));
		}
		f.fake->Deliver(2, Protocol::Encode(badFace, MT::kReportFace));
		f.fake->Deliver(2, Protocol::Encode(goodFace, MT::kReportFace));
		// She says where she is first: an explosion is believed only near the player who threw it.
		Protocol::PlayerState here;
		here.sequence = 1;
		here.worldspace = 0x0000003C;
		here.x = 50.0f, here.y = 100.0f;
		f.fake->Deliver(2, Protocol::Encode(here));
		f.fake->Deliver(2, Protocol::Encode(Protocol::Shot{ 0, 0, Protocol::ShotAction::kThrow }, MT::kReportShot));
		f.fake->Deliver(2, Protocol::Encode(badBlast, MT::kReportExplosion));
		f.fake->Deliver(2, Protocol::Encode(goodBlast, MT::kReportExplosion));
		Sync(f, 2);

		const auto bobGot = Received(f, 1);
		CheckEveryPacketReads(bobGot, "Bob");
		CHECK((ids(workshopIn(bobGot)) == std::set<std::uint32_t>{ good.refId }));
		const auto faces = OfType(bobGot, MT::kPlayerFace);
		REQUIRE(faces.size() == 1);
		CHECK(Protocol::DecodeFace(faces[0])->sliders == goodFace.sliders);
		const auto blasts = OfType(bobGot, MT::kExplosion);
		REQUIRE(blasts.size() == 1);
		CHECK(Protocol::DecodeExplosion(blasts[0])->x == goodBlast.x);

		// Dave, joining later, catches up on the good build and the good face only.
		REQUIRE(f.Join(3, Fixture::MakeHello("Dave", 0xDA5E)));
		REQUIRE(f.fake->WaitFor(3, Type(MT::kWorldState)));
		const auto daveGot = Received(f, 3);
		CheckEveryPacketReads(daveGot, "Dave");
		CHECK((ids(workshopIn(daveGot)) == std::set<std::uint32_t>{ good.refId }));
		const auto daveFaces = OfType(daveGot, MT::kPlayerFace);
		REQUIRE(daveFaces.size() == 1);
		CHECK(Protocol::DecodeFace(daveFaces[0])->sliders == goodFace.sliders);
	}  // stopping saves

	// The file the host keeps holds only the good build...
	const auto saved = SessionFile::Load(path);
	REQUIRE(saved);
	CHECK((ids(saved->workshopItems) == std::set<std::uint32_t>{ good.refId }));
	for (const auto& item : saved->workshopItems) {
		CHECK(std::ranges::all_of(item.position, [](float a_v) { return std::isfinite(a_v); }));
		CHECK(std::isfinite(item.scale));
	}

	// ...and that is what someone joining the next day is given.
	{
		Fixture f{ options };
		REQUIRE(f.Join(5, Fixture::MakeHello("Erin", 0xE41)));
		CHECK(f.Logged("loaded the saved session"));
		REQUIRE(f.fake->WaitFor(5, Type(MT::kWorldState)));
		const auto erinGot = Received(f, 5);
		CheckEveryPacketReads(erinGot, "Erin, the next day");
		CHECK((ids(workshopIn(erinGot)) == std::set<std::uint32_t>{ good.refId }));
	}
	std::filesystem::remove(path);
}
